#!/usr/bin/env python3
"""
combiner.py -- the Virtual Infocom combiner (the "maker" tool).

Takes the story-less Virtual Infocom interpreter ROM and a
player-supplied Z-code story file, and assembles a flashable
Virtual Boy ROM:

    [ interpreter ROM ][ 0xFF pad ][ descriptor ][ 0xFF pad ]
    [ story at offset 0x100000   ][ padding: the user's choice ]

Padding is the user's choice (carts differ, so the tool does
not decide): by default the image is padded with 0xFF to the
smallest power of two that fits; --pad MBIT pads to exactly
16, 32, 64 or 128 megabits; --no-pad writes the natural image,
ending at the story's last byte.

The descriptor is 64 bytes at 0x100000-64 = 0xFFFC0, magic
"VBZSTORY", all integer fields little-endian (the V810 reads it;
the story itself is stored byte-for-byte, never swapped):

    0   8   "VBZSTORY"
    8   4   story offset (0x100000)
    12  4   story length (header-declared length)
    16  4   CRC32 of the story bytes
    20  2   Z release number
    22  6   serial (ASCII)
    28  2   header checksum
    30  1   Z-machine version
    31  1   flags (0)
    32  32  title, NUL-padded (from the filename, or --title)

Validation is combiner-side and strict, per the Nov 15 design
consult: a bad story never becomes a ROM. The interpreter binary
is never patched -- it is byte-identical for every user.

Usage:
    python3 combiner.py INTERP.vb STORY.z5 -o OUT.vb [--title NAME]
        [--pad MBIT | --no-pad]
"""

import argparse
import struct
import sys
import zlib

STORY_OFFSET = 0x100000
DESC_OFFSET = STORY_OFFSET - 64
SUPPORTED_VERSIONS = (3, 4, 5, 8)


def story_declared_length(data):
    version = data[0]
    raw = struct.unpack(">H", data[0x1A:0x1C])[0]
    mult = 2 if version == 3 else 4 if version <= 5 else 8
    return raw * mult if raw else len(data)


def story_checksum(data, length):
    return sum(data[0x40:length]) & 0xFFFF


def validate_story(data):
    """Returns (declared_length, info dict) or raises ValueError."""
    if len(data) < 64:
        raise ValueError("story file is smaller than a Z-machine header")
    version = data[0]
    if version not in SUPPORTED_VERSIONS:
        raise ValueError(
            f"unsupported Z-machine version {version} "
            f"(this interpreter runs 3, 4, 5 and 8)")
    declared = story_declared_length(data)
    if declared > len(data):
        raise ValueError(
            f"story header declares {declared} bytes but the file "
            f"has only {len(data)} (truncated file?)")
    if declared < len(data):
        print(f"note: file has {len(data) - declared} trailing bytes "
              f"beyond the header-declared length; using the header "
              f"length, the extra bytes are not copied",
              file=sys.stderr)
    stored = struct.unpack(">H", data[0x1C:0x1E])[0]
    actual = story_checksum(data, declared)
    if stored != actual:
        raise ValueError(
            f"story header checksum mismatch: header says "
            f"0x{stored:04X}, computed 0x{actual:04X} (corrupt file?)")
    info = {
        "version": version,
        "release": struct.unpack(">H", data[2:4])[0],
        "serial": data[0x12:0x18].decode("ascii", "replace"),
        "checksum": stored,
        "dynamic": struct.unpack(">H", data[0x0E:0x10])[0],
    }
    return declared, info


def build_descriptor(story, declared, title):
    desc = bytearray(64)
    desc[0:8] = b"VBZSTORY"
    struct.pack_into("<I", desc, 8, STORY_OFFSET)
    struct.pack_into("<I", desc, 12, declared)
    struct.pack_into("<I", desc, 16, zlib.crc32(story[:declared]) & 0xFFFFFFFF)
    struct.pack_into("<H", desc, 20, struct.unpack(">H", story[2:4])[0])
    desc[22:28] = story[0x12:0x18]
    struct.pack_into("<H", desc, 28, struct.unpack(">H", story[0x1C:0x1E])[0])
    desc[30] = story[0]
    desc[31] = 0
    t = title.encode("ascii", "replace")[:31]
    desc[32:32 + len(t)] = t
    return bytes(desc)


def next_pow2(n):
    p = 1
    while p < n:
        p *= 2
    return p


def main():
    ap = argparse.ArgumentParser(description="Virtual Infocom combiner")
    ap.add_argument("interpreter", help="story-less interpreter ROM (.vb)")
    ap.add_argument("story", help="player-supplied story file (.z3/.z5/...)")
    ap.add_argument("-o", "--output", required=True, help="output ROM (.vb)")
    ap.add_argument("--title", help="title for the descriptor "
                                    "(default: story filename)")
    pad_group = ap.add_mutually_exclusive_group()
    pad_group.add_argument("--pad", type=int, metavar="MBIT",
                           help="pad the output with 0xFF to exactly "
                                "MBIT megabits -- a power of two, at "
                                "most 128 (16, 32, 64 or 128 in "
                                "practice); default is the smallest "
                                "power of two that fits")
    pad_group.add_argument("--no-pad", action="store_true",
                           help="write the natural image -- "
                                "interpreter, descriptor and story, "
                                "ending at the story's last byte, "
                                "with no padding at all")
    args = ap.parse_args()

    interp = open(args.interpreter, "rb").read()
    story = open(args.story, "rb").read()

    if len(interp) > DESC_OFFSET:
        print(f"error: interpreter ROM is {len(interp)} bytes; it must "
              f"fit below the descriptor at 0x{DESC_OFFSET:X}",
              file=sys.stderr)
        return 1
    try:
        declared, info = validate_story(story)
    except ValueError as e:
        print(f"error: {e}", file=sys.stderr)
        return 1

    title = args.title
    if not title:
        import os
        title = os.path.splitext(os.path.basename(args.story))[0]

    desc = build_descriptor(story, declared, title)

    out = bytearray()
    out += interp
    out += b"\xFF" * (DESC_OFFSET - len(out))
    out += desc
    out += b"\xFF" * (STORY_OFFSET - len(out))
    out += story[:declared]
    natural = len(out)

    # The Virtual Boy header (.rominfo) and the interrupt/reset
    # vectors sit at the TOP of the ROM image (the linker places
    # them at the interpreter's own power-of-two boundary:
    # vectors end at the boundary, header 0x20 bytes before
    # them, block size 0x21F). The raw builds tile the image so
    # a copy lands at every boundary; a combined image is
    # sparse, so in the padded modes that tail block is
    # replicated at the top of the final image, exactly where a
    # native build of this size would put it. A padded size must
    # therefore leave TAIL clear bytes above the story -- a size
    # whose tail block would land on top of the story is refused
    # rather than allowed to corrupt it. In --no-pad mode no
    # tail block is replicated: the image ends at the story's
    # last byte and carries the header/vectors only where the
    # interpreter itself carries them, at the top of its own
    # 64 KiB (see COMBINER.md before flashing an unpadded image).
    TAIL = 0x21F
    if args.no_pad:
        total = natural
    else:
        if len(interp) & (len(interp) - 1):
            print("error: interpreter ROM is not a power of two; "
                  "cannot locate its header/vector block",
                  file=sys.stderr)
            return 1
        if args.pad is not None:
            if args.pad <= 0 or (args.pad & (args.pad - 1)):
                print(f"error: --pad {args.pad} is not a usable "
                      f"size: the size in megabits must be a power "
                      f"of two (16, 32, 64 or 128)", file=sys.stderr)
                return 1
            if args.pad > 128:
                print(f"error: --pad {args.pad} is too big: the "
                      f"Virtual Boy's ROM window is 128 Mbit "
                      f"(16 MiB), so 128 is the largest padded size",
                      file=sys.stderr)
                return 1
            total = args.pad * 131072
        else:
            total = next_pow2(natural)
        if total - TAIL < natural:
            minimum = next_pow2(natural + TAIL)
            print(f"error: a {total}-byte image is too small for "
                  f"this story: the story sits at "
                  f"0x{STORY_OFFSET:X} and ends at {natural} "
                  f"bytes, and the boot block needs {TAIL} clear "
                  f"bytes at the top of the image -- the smallest "
                  f"size that works is {minimum // 131072} Mbit "
                  f"({minimum} bytes)", file=sys.stderr)
            return 1
        out += b"\xFF" * (total - len(out))
        out[total - TAIL:total] = interp[len(interp) - TAIL:]

    # ---- re-read and verify what was assembled ----
    assert len(out) == total
    assert bytes(out[DESC_OFFSET:DESC_OFFSET + 8]) == b"VBZSTORY"
    assert struct.unpack_from("<I", out, DESC_OFFSET + 8)[0] == STORY_OFFSET
    assert bytes(out[STORY_OFFSET:STORY_OFFSET + declared]) == story[:declared]
    crc = zlib.crc32(bytes(out[STORY_OFFSET:STORY_OFFSET + declared])) & 0xFFFFFFFF
    assert crc == struct.unpack_from("<I", out, DESC_OFFSET + 16)[0]
    assert story_checksum(bytes(out[STORY_OFFSET:STORY_OFFSET + declared]),
                          declared) == info["checksum"]
    if not args.no_pad:
        assert (total & (total - 1)) == 0
        assert bytes(out[total - TAIL:total]) == \
            bytes(interp[len(interp) - TAIL:])

    with open(args.output, "wb") as f:
        f.write(out)

    print(f"interpreter : {len(interp)} bytes")
    print(f"story       : {title} -- Z{info['version']} release "
          f"{info['release']}, serial {info['serial']}, "
          f"{declared} bytes, dynamic memory {info['dynamic']} bytes, "
          f"checksum 0x{info['checksum']:04X}, CRC32 0x{crc:08X}")
    print(f"layout      : descriptor at 0x{DESC_OFFSET:X}, "
          f"story at 0x{STORY_OFFSET:X}")
    if args.no_pad:
        print(f"output      : {args.output} -- {total} bytes, "
              f"unpadded (natural size), verified")
    elif args.pad is not None:
        print(f"output      : {args.output} -- {total} bytes "
              f"({total // 1048576} MiB), power of two, "
              f"padded to {args.pad} Mbit, verified")
    else:
        print(f"output      : {args.output} -- {total} bytes "
              f"({total // 1048576} MiB), power of two, verified")
    return 0


if __name__ == "__main__":
    sys.exit(main())
