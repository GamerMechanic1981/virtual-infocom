#!/usr/bin/env python3
"""
make_undo_story.py -- hand-built minimal Z5 story that exercises
the EXT save_undo / restore_undo opcodes end to end, built for the
2026-11-17 restore_undo mirror-defect fix (see
zmachine-vb/release/EXT-RESTORE-UNDO-FIX-2026-11-17.md).

No parser, no text: MAIN runs a fixed script and reports purely
with print_num, one value per line:

  save_undo -> G16
  if G16 == 2 -> restored        (resumed by a successful undo)
  if G16 == 1 -> first           (fresh snapshot just taken)
  else print 999, quit           (BAD: resumed with neither)
first:
  print G16                      -> "1"
  G17 = 77                       (mutate state after the snapshot)
  restore_undo -> G18
  print 888, quit                (only reached if restore FAILED)
restored:
  print G16                      -> "2"  (the resumed save_undo's
                                          own result variable)
  print G17                      -> "0"  (mutation rolled back)
  print G18                      -> "0"  (restore_undo's own store
                                          variable NOT scribbled on)
  quit

Correct interpreter output:  1 / 2 / 0 / 0
Defective (pre-fix) output:  1 / 999 -- the resumed save_undo never
sees its 2; the 2 lands in G18, the abandoned restore_undo's own
store variable, and execution falls through with G16 == 0.

Layout: header 0x0000, globals 0x0040 (240 words), static base
0x0220, a stub dictionary (never parsed), then MAIN in high memory.
Reuses the tiny assembler from make_test_story.py.

Usage: python3 make_undo_story.py [out.z5]
"""

import struct
import sys
import os

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from make_test_story import Routine, C, V

GLOBALS = 0x0040
STATIC_BASE = 0x0220
DICT = STATIC_BASE

G_RES_SAVE = 16 + 0    # @save_undo result variable
G_MARKER = 16 + 1      # state marker, mutated after the snapshot
G_RES_REST = 16 + 2    # @restore_undo result variable


def save_undo(r, store):
    r.emit(0xBE, 9, 0xFF, store)       # EXT 9, no operands


def restore_undo(r, store):
    r.emit(0xBE, 10, 0xFF, store)      # EXT 10, no operands


def build():
    M = Routine("MAIN", 0)
    save_undo(M, G_RES_SAVE)
    M.je([V(G_RES_SAVE), C(2)], "restored", True)
    M.je([V(G_RES_SAVE), C(1)], "first", True)
    M.print_num(C(999))
    M.new_line()
    M.quit()

    M.label("first")
    M.print_num(V(G_RES_SAVE))
    M.new_line()
    M.store(G_MARKER, C(77))
    restore_undo(M, G_RES_REST)
    M.print_num(C(888))
    M.new_line()
    M.quit()

    M.label("restored")
    M.print_num(V(G_RES_SAVE))
    M.new_line()
    M.print_num(V(G_MARKER))
    M.new_line()
    M.print_num(V(G_RES_REST))
    M.new_line()
    M.quit()
    return M


def main():
    out_path = sys.argv[1] if len(sys.argv) > 1 else "test_undo.z5"

    # stub dictionary: 0 separators, entry length 7, 0 entries
    dict_bytes = bytes([0, 7, 0, 0])

    M = build()
    code_at = DICT + len(dict_bytes)
    while code_at % 4:
        code_at += 1
    M.resolve(code_at, lambda name: code_at)

    img = bytearray()
    img += bytes(64)                       # header
    img += bytes(GLOBALS - len(img))
    img += bytes(240 * 2)                  # globals, all zero
    assert len(img) == STATIC_BASE
    img += dict_bytes
    img += bytes(code_at - len(img))
    img += M.code

    while len(img) % 8:
        img.append(0)
    total = len(img)

    def put16(off, val):
        img[off] = (val >> 8) & 0xFF
        img[off + 1] = val & 0xFF

    img[0] = 5
    put16(2, 1)                            # release 1
    put16(4, STATIC_BASE)                  # high memory base
    put16(6, code_at + 1)                  # initial PC (instruction
                                           # after MAIN's header byte)
    put16(8, DICT)                         # dictionary
    put16(0x0A, STATIC_BASE)               # object table (unused)
    put16(0x0C, GLOBALS)                   # globals
    put16(0x0E, STATIC_BASE)               # static base
    img[0x12:0x18] = b"261117"             # serial
    put16(0x18, 0x0040)                    # abbreviations (unused)
    put16(0x1A, total // 4)                # file length / 4
    put16(0x28, 0)                         # routines offset
    put16(0x2A, 0)                         # strings offset
    checksum = sum(img[0x40:total]) & 0xFFFF
    put16(0x1C, checksum)

    with open(out_path, "wb") as f:
        f.write(img)
    print(f"wrote {out_path}: {total} bytes, checksum 0x{checksum:04X}, "
          f"MAIN at 0x{code_at:04X}, dynamic size 0x{STATIC_BASE:04X}")


if __name__ == "__main__":
    main()
