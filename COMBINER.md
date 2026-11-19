# The combiner — usage notes

`combiner.py` is the Virtual Infocom "maker" tool. It takes the
story-less interpreter ROM and a story file **you** supply, and
assembles the flashable Virtual Boy ROM:

```
python3 combiner.py INTERP.vb STORY.z5 -o OUT.vb [--title NAME] [--pad MBIT | --no-pad]
```

- `INTERP.vb` — the interpreter ROM (`virtual-infocom.vb` from this
  package).
- `STORY.z5` — your story file: Z-machine version **3, 4, 5, or 8**
  (`.z3`, `.z4`, `.z5`, `.z8` — the extension doesn't matter, the
  header does).
- `-o OUT.vb` — where the combined ROM goes (required).
- `--title NAME` — optional. The name shown for the story in the
  boot sequence. Defaults to the story's filename without its
  extension.
- `--pad MBIT` — optional. Pad the output to exactly MBIT
  megabits instead of the default size (see "Padding" below).
- `--no-pad` — optional. Write the output unpadded, at its
  natural size (see "Padding" below). `--pad` and `--no-pad`
  cannot be used together.

Requires Python 3; standard library only; nothing to install.

## Validation — a bad story never becomes a ROM

The combiner checks the story *before* assembling anything, and
refuses with a plain error if any check fails:

- the file is at least a full Z-machine header (64 bytes);
- the version byte is one this interpreter runs (3, 4, 5, 8);
- the header-declared length doesn't exceed the actual file (a
  truncated file is rejected);
- the **header checksum verifies** — the sum of the story bytes
  after the header must match the checksum stored in it, so a
  corrupt download is caught here, not on the Virtual Boy.

If the file carries extra trailing bytes beyond the declared
length, that's noted and the extra bytes are left out. The
interpreter binary itself is never patched — it is byte-identical
in every ROM the combiner makes.

On success it prints the story's vitals (version, release, serial,
size, dynamic memory, checksum, CRC32) and the output size.

## How the combined ROM is laid out

```
[ interpreter ROM ][ 0xFF pad ][ descriptor ][ 0xFF pad ]
[ story at offset 0x100000   ][ 0xFF pad to a power of two ]
```

- **Story placement:** the story bytes are copied verbatim —
  never modified, never byte-swapped — to offset **0x100000**
  (1 MiB) in the output image.
- **The descriptor** is 64 bytes at **0xFFFC0** (immediately below
  the story), magic `VBZSTORY`. It records the story's offset,
  header-declared length, CRC32, release number, serial, header
  checksum, Z-machine version, and the title. The interpreter finds
  the descriptor at boot, and the boot sequence re-verifies the
  story (header checksum + CRC32) while it "loads" — a story that
  doesn't verify never runs.
- The Virtual Boy header and interrupt/reset vectors are replicated
  at the top of the final image, exactly where a native build of
  that size would put them, so the combined ROM boots like any
  other.
- By default the output is padded with 0xFF to the smallest power
  of two that holds the layout — 2 MiB for any story that fits
  under the 1 MiB mark, which is all of them in practice. The
  padding is yours to choose instead; see the next section.

## Padding — your choice

Carts differ: some take an image of any size, some want one exact
size, some want none of the padding at all. So the combiner does
not decide for you. There are three ways to build:

**Default — smallest power of two.** With neither option, the
output is padded with 0xFF to the smallest power of two that
holds the layout (2 MiB for the stories this interpreter runs):

```
python3 combiner.py virtual-infocom.vb test-chamber.z5 -o test-chamber.vb --title "TEST CHAMBER"
```

**`--pad MBIT` — an exact size.** Pads with 0xFF to exactly MBIT
megabits. MBIT must be a power of two, at most 128 (the Virtual
Boy's whole ROM window): 16, 32, 64 or 128 in practice. Use it
when your cart or flasher wants a specific size:

```
python3 combiner.py virtual-infocom.vb test-chamber.z5 -o test-chamber.vb --title "TEST CHAMBER" --pad 32
```

Because the story sits at offset 0x100000 and the boot block
needs 543 clear bytes at the top of the image, **16 Mbit (2 MiB)
is the smallest padded size that can hold any story**. Asking
for less is refused, with the smallest size that works named in
the error — nothing is written.

**`--no-pad` — the natural image.** No padding at all: the output
is the interpreter, the descriptor and the story, ending at the
story's last byte:

```
python3 combiner.py virtual-infocom.vb test-chamber.z5 -o test-chamber-raw.vb --title "TEST CHAMBER" --no-pad
```

One honest warning about unpadded images. A Virtual Boy finds
its boot header and reset/interrupt vectors at the **top** of
the ROM image, so every padded build carries a copy of that
block at its top end. An unpadded image ends at the story's
last byte and carries the block only where the interpreter
itself carries it, at the top of the interpreter's own 64 KiB.
That is the right file for a cart, flasher or emulator that
handles natural-size images — but padding one afterwards with
plain fill puts fill, not vectors, at the top, and the result
will not boot. If your hardware wants a size, choose it at
build time with `--pad` (or take the default), not afterwards
with a fill tool.
