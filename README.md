# Virtual Infocom

**A Z-machine interpreter for the Virtual Boy.**

Infocom shipped its text adventures for just about every computer of
the 1980s — but never for the Virtual Boy. Virtual Infocom is the
interpreter that fixes that: a full Z-machine that runs classic
text adventures on real Virtual Boy hardware, with an on-screen
dual-cursor keyboard for typing your commands, and a boot sequence
styled after the Commodore 64 — the machine so many of these games
were first played on.

To the author's knowledge, it is also only the second emulator
ever written *for* the Virtual Boy — the first was an attempt some
time ago to run Game Boy games on the Virtual Boy, a Game Boy
emulator cart that appeared on Kevin Mellott's old website.
Nothing else like it has turned up since; this one just emulates
a machine from a decade earlier instead.

## What it is — and what it is not

Virtual Infocom is an **interpreter only**. On its own it does
nothing: it is an engine with no game in it. To play something, you
combine the interpreter with a story file **you supply** (see the
walkthrough below) and the two become one Virtual Boy ROM — your
game.

- **No commercial story files are included in this release, and
  none are distributed with it. Ever.** The Hitchhiker's Guide to
  the Galaxy you may see mentioned in project notes is the author's
  local test story from his own collection; it is not in this
  package and is never distributed. Bring a story file you own.
- It runs **Z3, Z4, and Z5** story files, and **Z8** on the Z5 code
  path (the combiner accepts versions 3, 4, 5, and 8 — the version
  byte in the story's header is what counts, not the file name).
  That covers the whole classic Infocom shelf — Zork, Enchanter,
  Planetfall, The Hitchhiker's Guide to the Galaxy, Wishbringer,
  and the rest — plus modern Z5 interactive fiction.
- The Z6 graphical titles (Zork Zero, Arthur, Journey, Shogun) are
  **out of scope**: they use a different, graphics-based screen
  model. This is a text machine, proudly.

## What's in this package

| File | What it is |
|---|---|
| `virtual-infocom.vb` | The interpreter ROM, story-less, **unpadded** at its natural size (64 KiB). |
| `combiner.py` | The combiner tool (Python 3, standard library only). See `COMBINER.md`. |
| `test-chamber.z5` | **TEST CHAMBER**, a small original story written for this project, included so you can try the interpreter immediately. |
| `README.txt` | This file, in plain text so anything can open it. |
| `COMBINER.md` | Combiner usage and format notes. |

## What you need

1. **This package**, downloaded and unzipped.
2. **Python 3** on any computer — the combiner is a single script
   and uses only the standard library. Nothing to install.
3. **A story file you own**, in Z-machine version 3, 4, 5, or 8
   (files usually named with a `.z3`, `.z4`, `.z5`, or `.z8`
   ending). The bundled `test-chamber.z5` covers this for your
   first build, so you can do the whole walkthrough before
   reaching for your own collection.
4. **Somewhere to play the result**: a Virtual Boy emulator, or a
   real Virtual Boy with a flash cart and its programmer.

## Build a playable game — step by step

A Virtual Boy cartridge can't load files at runtime, so the story
has to live inside the ROM image. That's the combiner's job: it
takes the story-less interpreter and a story file, validates the
story, and assembles one flashable ROM. Every command below is
complete and runnable as written — where an example uses a
particular story, substitute your own file's name for the
example's.

One instruction before you start, borrowed from a considerably
more famous guide: **DON'T PANIC.** The whole build is a single
command, and the combiner checks its work as it goes.

### Step 1 — Open a terminal in the package folder

Change into the folder you unzipped the release into, so that
`virtual-infocom.vb`, `combiner.py`, and `test-chamber.z5` are all
in front of you. Every command below is run from that folder.

### Step 2 — First build: the bundled TEST CHAMBER

Build the included demo story into a ROM:

```
python3 combiner.py virtual-infocom.vb test-chamber.z5 -o test-chamber.vb --title "TEST CHAMBER"
```

The combiner checks the story before building anything and prints
what it found. A good run looks like this:

```
interpreter : 65536 bytes
story       : TEST CHAMBER -- Z5 release 1, serial 261116, 3416 bytes, dynamic memory 1088 bytes, checksum 0x5D05, CRC32 0x175534BB
layout      : descriptor at 0xFFFC0, story at 0x100000
output      : test-chamber.vb -- 2097152 bytes (2 MiB), power of two, verified
```

You now have `test-chamber.vb`, a complete, playable Virtual Boy
ROM. If instead you get an error — unsupported version, declared
length longer than the file, checksum mismatch — the story was
refused and **no ROM was made**: a bad story never becomes a ROM.
Fix the story file (a truncated or corrupt download is the usual
cause) and run the command again.

The `--title` part is optional. It sets the name the boot sequence
shows for the story. Leave it off and the combiner uses the
story's file name without its ending:

```
python3 combiner.py virtual-infocom.vb test-chamber.z5 -o test-chamber.vb
```

### Step 3 — Build a game from your own story

Same command, your file. This example builds Zork I from a story
file named `zork1.z3` sitting in the same folder:

```
python3 combiner.py virtual-infocom.vb zork1.z3 -o zork1.vb --title "ZORK I"
```

That produces `zork1.vb` — the interpreter and your story,
assembled and verified, ready to play. The interpreter binary
inside is never patched; it is byte-identical in every ROM the
combiner makes. Only the story, its descriptor, and the padding
differ.

### Step 4 — Know your output

- **Size.** By default, the combiner pads its output with 0xFF
  to the next power of two. Any story that fits under the 1 MiB
  mark — which is all of them in practice — comes out at
  **2 MiB** (2,097,152 bytes), like both examples above. The
  size is your choice, not the tool's: `--pad` picks an exact
  size in megabits and `--no-pad` picks none at all — see "A
  note on the ROM file" at the end. The story itself is copied
  byte-for-byte to offset 0x100000 inside the image; a 64-byte
  descriptor just below it is how the interpreter finds and
  re-verifies the story at boot.
- **The bare interpreter is a different story.** The packaged
  `virtual-infocom.vb` ships **unpadded**, at its natural 64 KiB
  size — that's the release standard: releases ship the raw image
  at its natural size, and padding or tiling for a particular
  cart is the user's step, because every cart wants it
  differently. ROMs that come *out of the combiner* are already
  sized by your own padding choice (the default is the smallest
  power of two), so for games you build yourself there is
  normally nothing more to pad.

### Step 5 — Run it in an emulator

Open the combined ROM (for example `test-chamber.vb`) in a Virtual
Boy emulator the way you'd open any homebrew ROM. Mednafen's
Virtual Boy core is the one this project is developed against.
In an emulator, SAVE and RESTORE work through the emulator's
battery/SRAM file, exactly as they would on a cart.

### Step 6 — Or flash it to a real cart

Flash the combined ROM to your flash cart with its programmer,
the way you flash any Virtual Boy ROM of that size, and play it
in the visor. For SAVE and RESTORE to persist on hardware, the
cart needs battery-backed SRAM in the standard window — see
"Saving and undo" below for exactly what that means and what has
been proven where.

## What happens when you boot it

1. **The calibration gate.** Like every Virtual Boy game, it opens
   on the IPD/focus adjustment screen — adjust until the corner
   squares and the dot blocks line up, then press **A** or **Start**.
2. **The boot ritual.** A Commodore 64-style power-on screen, in
   Virtual Boy red and the C64's own typeface, reporting the
   machine's real specs. The computer then types the launch itself —
   `LOAD`, `SEARCHING`, `LOADING`, `READY.`, `RUN` — verifying your
   story's checksum as it goes. If the story checks out, the game
   begins. (If a ROM has no story, or a corrupt one, you'll get the
   `?FILE NOT FOUND ERROR` / `NO STORY LOADED` screen instead.)
3. **The game.** The story's opening text appears above the
   keyboard, and you type.

## Controls — the dual-cursor keyboard

There is no keyboard port on a Virtual Boy, so the keyboard is on
the screen: a QWERTY grid, worked with **both** of the controller's
D-pads at once — one cursor per hand.

| Control | Action |
|---|---|
| **Left D-pad** | Move the **left cursor** over the grid |
| **Right D-pad** | Move the **right cursor** over the grid |
| **L button** | Type the key under the **left** cursor |
| **R button** | Type the key under the **right** cursor |
| **A** | Space |
| **B** | Backspace |
| **Start** | Enter — submit the command line |
| **Select** | Switch the grid to the digits layer and back |

The main grid holds the 26 letters plus **period** and **comma** as
ordinary keys — you'll want them: period chains commands in one
line (`take lamp. north`), and comma is how you give orders to
characters (`marvin, go to the pantry`). The digits live on the
Select layer, for codes, dials, and counts.

Input is **lower case only**, and that's fine: the Infocom parser
folds everything to lower case anyway. The games' own prose prints
in its proper mixed case; only your typing is folded. Note that a
few very old games have small vocabularies and won't know long
forms of commands — in Hitchhiker's, for instance, inventory is
**I**, not INVENTORY. That's the game talking, not the keyboard.

### Read this before your first game: the two cursors feel different

The cursors do **not** give the same feedback, and this trips up
every new player:

- The **left cursor fully inverts** the key it sits on — the glyph
  flips to its negative. Very visible.
- The **right cursor only makes its key slightly darker** — a
  subtle palette change, much easier to miss.

**Both cursors work exactly the same.** If you press R and the
letter appears on the command line, your right hand is fine — the
darkening *is* the feedback, it's just quieter than the left hand's
full inversion. Don't conclude the right side is broken and type
one-handed; two hands over the grid is much faster once it clicks,
and it clicks quickly.

## Playing — the commands every game understands

These games predate menus, tutorials, and tool-tips: the parser
*is* the interface, and it expects to be talked to. That was common
knowledge in 1983; it isn't now, so here is the core vocabulary
nearly every Infocom game shares. Type a command, press **Start**
(Enter), and the game answers.

| Command | What it does |
|---|---|
| **LOOK** (or **L**) | Describe where you are, again |
| **INVENTORY** (**I**) | List what you're carrying |
| **EXAMINE thing** (**X thing**) | Look at something closely — this is the most useful command in any of these games |
| **TAKE thing** / **DROP thing** | Pick something up / put it down |
| **OPEN thing** / **CLOSE thing** | Doors, boxes, lids |
| **NORTH** (**N**), **SOUTH** (**S**), **EAST** (**E**), **WEST** (**W**), **NE**, **NW**, **SE**, **SW**, **UP** (**U**), **DOWN** (**D**) | Move |
| **ENTER** / **EXIT** | Go in / come back out |
| **WAIT** (**Z**) | Let a turn pass |
| **PUT thing IN box** / **PUT thing ON table** | Containers and surfaces |
| **ASK person ABOUT thing** | Question a character |
| **person, command** | Give a character an order — `marvin, go to the pantry` |
| **SAVE** / **RESTORE** / **UNDO** | Save your game / bring it back / take back your last move |
| **SCORE** | Show your score, in games that keep one |
| **VERBOSE** / **BRIEF** | Full room descriptions every visit / only on the first |
| **RESTART** / **QUIT** (**Q**) | Start over / stop playing |

Two habits worth forming early: **EXAMINE everything** — the
interesting detail is almost always behind an X — and **SAVE
before you try anything you're not sure you can undo**, because
these games were written when losing was part of the fun. Beyond
this list, just type what you'd say: the parser knows hundreds of
words, and when it doesn't understand it will tell you so.

## Saving and undo

- **SAVE** and **RESTORE** work the way the games expect: the state
  is stored in standard **Quetzal** format in the cartridge's SRAM.
  There is **one save slot** per cart.
- **UNDO** is supported (single level) in games that offer it.
- **How saves sit on the cart.** Saves live in the first 8 KB of
  the standard SRAM window at 0x06000000 — the same window every
  commercial Virtual Boy game was written against. The save code
  writes byte-wise using the byte-lane addressing real carts use
  (one data byte per 16-bit cell, at even addresses), waits out
  the SRAM's power-on wake-up before its first access, and
  **reads every save back and verifies it** before reporting
  success — a save that doesn't verify doesn't claim to have
  worked.
- **Proven on hardware.** The full round trip — SAVE, power the
  Virtual Boy completely off, power on, RESTORE, game state
  intact — is verified on a **16 Mbit Mellott flash cart**, the
  development hardware. Other flash carts and multicarts (MrCART,
  HyperFlash32, FlashBoy Plus, and the like) present the same
  standard SRAM window to the game, but each manages persistence
  its own way — its own battery, an SD-card backup, a shared save
  pool — so whether a save survives swapping ROMs is that
  device's business, not the interpreter's. In an emulator, saves
  persist through the emulator's SRAM/battery file.

## Limits and known issues

- **28 KB dynamic-memory cap.** A story's dynamic memory (its
  mutable world state) must fit in 28 KB. Most Infocom titles fit
  comfortably — The Hitchhiker's Guide, for example, uses about
  15 KB. The very largest late titles may exceed the cap; the
  interpreter refuses those with a clear message rather than
  crashing.
- **Sound effects are not wired up.** A handful of games beep or
  buzz at dramatic moments; here they play silently. Nothing else
  is affected.
- **Text only**, in the Virtual Boy's reds. Colour and text-style
  requests from a story are accepted and politely ignored.
- Auxiliary table-form save/restore is not supported (the standard
  save/restore is), and the Z3 status line is approximate.

## TEST CHAMBER

`test-chamber.z5` is an original miniature adventure — one room, a
brass lamp, a smooth rock — written for this project to shake the
interpreter down, and included so anyone can try Virtual Infocom
the minute they download it, no story collection required. It is
generated by `tools/make_test_story.py` in the source tree, where
it doubles as the interpreter's regression test. Build it with the
Step 2 command above and you have a complete game.

## A note on the ROM file

`virtual-infocom.vb` ships **unpadded**, at its natural 64 KiB
size. Modern flash carts take it as-is; if your cart or flasher
requires a padded image, pad it to whatever size your hardware
wants — the padding is yours to choose, so the release doesn't
choose it for you.

The same etiquette applies to the games you build. By default
the combiner pads its output to the smallest power of two —
2 MiB for any story this interpreter runs. If your cart wants
one exact size, say so: `--pad 32` pads to exactly 32 megabits
(16, 64 and 128 work the same way). If your setup wants no
padding at all, `--no-pad` writes the natural image, ending at
the story's last byte. We don't know what cartridge you have,
and some want one thing and some another, so the choice is left
where it belongs. One caution travels with `--no-pad`: a
Virtual Boy looks for its boot block at the top of the image,
so read the padding section of `COMBINER.md` before flashing
an unpadded build anywhere unusual.

## License

Virtual Infocom is released under the **MIT License** (decided
2026-11-17). You may use, copy, modify, and redistribute it with
attribution. Story files you combine with it remain yours and are
not covered by this license — the combiner never modifies them,
and this release neither includes nor grants rights to any
commercial game.

## Credits

Virtual Infocom was designed and built by **Seth Amos Jones**
(**GamerMechanic1981**), one story file at a time, on real hardware.
