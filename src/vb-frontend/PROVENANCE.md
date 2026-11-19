# src-infocom-zm — provenance

Descends from src-infocom (the hardware-proven Virtual
Infocom front end, ROM_DEBUG.md section 69): crt0.s,
vb.ld, libgccvb/ and font/ are byte-identical to that
branch. main.c is src-infocom's main.c with the
interpreter integrated: story detection against the
combiner's descriptor, checksum+CRC32 verification
during the boot ritual's LOADING pause, the Z-machine
HAL (transcript terminal with word wrap, status line
on row 0, dual-cursor keyboard input incl. timed-read
ticks, Quetzal streams to cartridge SRAM), and the
original front-end screen kept verbatim as the
no-story / story-ended fallback. The interpreter core
itself is NOT in this tree: it is
workspace/zmachine-vb/src/zmachine.c, compiled in by
Makefile.infocomzm with Virtual Boy memory caps
(ZM_STACK_WORDS=512, ZM_MAX_FRAMES=32,
ZM_UPPER_LINES=8, ZM_UPPER_COLS=48).
