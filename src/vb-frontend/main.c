/*
 * Spectre VB -- VIRTUAL INFOCOM, INTERPRETER BUILD
 * (2026-11-16): the front end of src-infocom (gate,
 * C64 boot ritual, dual-cursor keyboard -- hardware
 * proven, ROM_DEBUG.md section 69) with the Z-machine
 * interpreter core standing behind it
 * (workspace/zmachine-vb/src/zmachine.c; design:
 * workspace/zmachine-vb/DESIGN_SEED.md).
 *
 * Boot flow: gate -> C64 boot screen (specs probed
 * live) -> the machine types LOAD -> SEARCHING FOR
 * STORY -> if the combiner's descriptor is present
 * at ROM offset 0xFFFC0 ("VBZSTORY"), the story at
 * 0x100000 is verified (header checksum AND CRC32)
 * during LOADING -> READY. -> the machine types RUN
 * -> the interpreter takes the screen: story output
 * scrolls in the transcript, the status line lives
 * on row 0, input comes from the dual-cursor
 * keyboard. With no story (or a corrupt one) the
 * ritual says so in C64 style and the proven
 * front-end screen answers NO STORY LOADED, exactly
 * as the front-end build did.
 *
 * Saves: Quetzal (IFhd + CMem + Stks) to cartridge
 * SRAM at 0x06000000, capped at 8 KB (the smallest
 * SRAM the flash carts carry). One level of
 * save_undo is kept in WRAM when the story's
 * dynamic memory leaves room for the snapshot.
 *
 * Memory: the interpreter's arena is 52 KB of the
 * 64 KB WRAM: dynamic memory up to 28 KB at the
 * base, the undo snapshot directly above it.
 * Stories whose dynamic memory exceeds 28 KB are
 * refused with a clear message, not a crash.
 *
 * FRONT-END MERGE (2026-11-16, ROM_DEBUG sec. 81):
 * this line branched from the sec. 69 front end and
 * has now received the two approved front-end
 * pieces from the src-infocom2 / src-infocom2b
 * line, ported verbatim: (1) the APPROVED Virtual
 * Infocom calibration gate FIRST in the boot order
 * (gatedata.h, flat assets, secs. 70-71), replacing
 * the placeholder gate; (2) the keyboard chrome
 * with the sec. 73 sharp-corner box-drawing grid
 * ("sharp corners, nothing hanging off the end")
 * plus the separator line at row 17. Front-end
 * only: the interpreter core (zmachine.c) is
 * untouched by the merge.
 */

#include "libgccvb/libgccvb.h"
#include "font/fontChar.h"
#include "gatedata.h"

/* Halfword copy into VIP memory (font is stored in ROM as bytes;
 * each destination halfword is assembled from a byte pair).
 * Identical to the helper in the B1/B2/DEBUG mains. */
static void copyToVip16(u16 *dest, const BYTE *src, u32 halfwords)
{
	u32 i;
	for (i = 0; i < halfwords; i++)
		dest[i] = (u16)src[2 * i] | ((u16)src[2 * i + 1] << 8);
}

/* Halfword clear/fill of VIP memory. Identical to the DEBUG mains. */
static void clearVip16(u16 *dest, u16 value, u32 halfwords)
{
	u32 i;
	for (i = 0; i < halfwords; i++)
		dest[i] = value;
}

/* Word clear of a memory window with stores of zero
 * (PROBE5CLEAN addition, carried over unchanged). Used
 * once, at the start of main, to restore as application
 * code the bulk VIP clear our old lineage's crt0
 * performed before main: its "ClrLoop" stored words of
 * zero from 0x00000000 for 0x40000 bytes — the whole VIP
 * RAM window 0x00000000-0x0003FFFF. The pointer is
 * volatile so every store is emitted: the window is
 * device memory, not ordinary RAM the compiler may
 * reason about. */
static void clearVip32(volatile u32 *dest, u32 words)
{
	u32 i;
	for (i = 0; i < words; i++)
		dest[i] = 0;
}

/* Write one eye's gate map (48x28 entries from
 * gatedata.h) into a BG map, row-major at the map's
 * 64-cell stride. Setup-only helper for the gate. */
static void bgGateMap(u16 map, const u16 *entries)
{
	u16 *bg = (u16 *)BGMap(map);
	u16 r, c;
	for (r = 0; r < 28; r++)
		for (c = 0; c < 48; c++)
			bg[r * 64 + c] = entries[r * 48 + c];
}

/* The default column table and its setup — vbSetColTable(),
 * which our libgccvb carries only as a comment in
 * libgccvb/video.h, provided here in main.c (PROBE4LINEAGE
 * adaptation 2: in the VUEngine lineage the table is
 * application code's job — Virtualcraft writes it from its
 * display_on()). The 128 bytes are verbatim from the
 * PROBE1CHAR crt0's `coltable`, verified byte-identical to
 * the commented libgccvb table and to Virtualcraft's
 * col_table. The write pattern is the shared one: forward
 * at halfwords 0-127 and 256-383, reversed at 128-255 and
 * 384-511, repeat byte 0. */
static const u8 colTable[128] = {
	0xFE, 0xFE, 0xFE, 0xFE, 0xFE, 0xFE, 0xFE, 0xFE,
	0xFE, 0xFE, 0xFE, 0xFE, 0xFE, 0xFE, 0xFE, 0xFE,
	0xFE, 0xFE, 0xFE, 0xFE, 0xFE, 0xFE, 0xFE, 0xFE,
	0xFE, 0xFE, 0xFE, 0xFE, 0xFE, 0xFE, 0xFE, 0xFE,
	0xFE, 0xFE, 0xFE, 0xFE, 0xFE, 0xFE, 0xFE, 0xFE,
	0xFE, 0xFE, 0xFE, 0xFE, 0xFE, 0xFE, 0xFE, 0xFE,
	0xFE, 0xFE, 0xFE, 0xFE, 0xFE, 0xFE, 0xFE, 0xFE,
	0xFE, 0xFE, 0xFE, 0xFE, 0xFE, 0xFE, 0xE0, 0xBC,
	0xA6, 0x96, 0x8A, 0x82, 0x7A, 0x74, 0x6E, 0x6A,
	0x66, 0x62, 0x60, 0x5C, 0x5A, 0x58, 0x56, 0x54,
	0x52, 0x50, 0x50, 0x4E, 0x4C, 0x4C, 0x4A, 0x4A,
	0x48, 0x48, 0x46, 0x46, 0x46, 0x44, 0x44, 0x44,
	0x42, 0x42, 0x42, 0x40, 0x40, 0x40, 0x40, 0x40,
	0x3E, 0x3E, 0x3E, 0x3E, 0x3E, 0x3E, 0x3E, 0x3E,
	0x3C, 0x3C, 0x3C, 0x3C, 0x3C, 0x3C, 0x3C, 0x3C,
	0x3C, 0x3C, 0x3C, 0x3C, 0x3C, 0x3C, 0x3C, 0x3C
};

static void vbSetColTable(void)
{
	u8 i;
	for (i = 0; i <= 127; i++) {
		CLMN_TBL[i      ] = colTable[i];
		CLMN_TBL[i + 256] = colTable[i];
		CLMN_TBL[i + 128] = colTable[127 - i];
		CLMN_TBL[i + 384] = colTable[127 - i];
	}
}

/* Frame pacing — TESTTYPER v3's cycle-verified wait,
 * carried over unchanged (see src-typer3/main.c for
 * the full derivation). A wait is VERIFIED only if
 * it consumed at least FRAME_POLLS_MIN iterations. */
#define FRAME_POLLS_MIN 20000

static u32 waitFrameCounted(void)
{
	u32 polls = 0;
	unsigned int volatile *xpstts = (unsigned int*)&VIP_REGS[XPSTTS];

	while (!(*xpstts & XPBSYR)) polls++;
	while (*xpstts & XPBSYR) polls++;
	while (!(VIP_REGS[INTPND] & XPEND)) polls++;
	VIP_REGS[INTCLR] = XPEND;
	return polls;
}

static u32 g_frames = 0;

static void waitVerifiedFrame(void)
{
	while (waitFrameCounted() < FRAME_POLLS_MIN)
		;
	g_frames++;
}

static void waitN(u16 n)
{
	while (n--)
		waitVerifiedFrame();
}

/* ------------------------------------------------ text on BG map 0 */
static u16 *BG0;

static void cell(u16 row, u16 col, u16 entry)
{
	BG0[row * 64 + col] = entry;
}

static void putCh(u16 row, u16 col, char ch)
{
	cell(row, col, 0x600 + (u8)ch);
}

static void printAt(u16 row, u16 col, const char *s)
{
	while (*s)
		putCh(row, col++, *s++);
}

static u16 strLen(const char *s)
{
	u16 n = 0;
	while (s[n])
		n++;
	return n;
}

static void printCentered(u16 row, const char *s)
{
	printAt(row, (48 - strLen(s)) / 2, s);
}

/* Append a small unsigned number to a string buffer. */
static void appendDec(char *buf, u16 *pos, u32 v)
{
	char tmp[8];
	u16 n = 0;
	if (v == 0)
		tmp[n++] = '0';
	while (v) {
		tmp[n++] = (char)('0' + (v % 10));
		v /= 10;
	}
	while (n)
		buf[(*pos)++] = tmp[--n];
	buf[*pos] = 0;
}

static void appendStr(char *buf, u16 *pos, const char *s)
{
	while (*s)
		buf[(*pos)++] = *s++;
	buf[*pos] = 0;
}

/* Probe WRAM honestly: write/read a pattern at five
 * points across the 64 KB span at 0x05000000 and
 * restore what was there. Returns the verified span
 * in KB (16/32/48/64 by highest passing probe). */
static u32 probeRamKB(void)
{
	static const u32 offs[4] = { 0x0000, 0x4000, 0x8000, 0xC000 };
	volatile u16 *wram = (volatile u16 *)0x05000000;
	u32 ok = 0;
	u16 i;
	for (i = 0; i < 4; i++) {
		u32 idx = offs[i] / 2;
		u16 save = wram[idx];
		u16 pat = (u16)(0xA55A ^ (u16)offs[i]);
		wram[idx] = pat;
		if (wram[idx] == pat)
			ok = (offs[i] / 1024) + 16;
		wram[idx] = save;
	}
	{
		u16 save = wram[0x7FFF];
		wram[0x7FFF] = 0x5AA5;
		if (wram[0x7FFF] == 0x5AA5)
			ok = 64;
		wram[0x7FFF] = save;
	}
	return ok;
}

/* Type a string at (row, col), one character per
 * `pace` verified frames, the block cursor riding
 * the frontier (the Matrix terminal's cadence).
 * Ends with the cursor cell cleared — the RETURN. */
static void typeText(u16 row, u16 col, const char *s, u16 pace)
{
	u16 i = 0;
	cell(row, col, 0x600 + 128);
	while (s[i]) {
		waitN(pace);
		putCh(row, col + i, s[i]);
		i++;
		cell(row, col + i, 0x600 + 128);
		waitVerifiedFrame();
	}
	waitN(20);
	cell(row, col + i, 0);
}

/* ------------------------------------------------ transcript */
#define TROWS 14
#define TROW0 1
static char tlines[TROWS][49];

static void redrawTranscript(void)
{
	u16 r, c;
	for (r = 0; r < TROWS; r++)
		for (c = 0; c < 48; c++) {
			char ch = tlines[r][c];
			BG0[(TROW0 + r) * 64 + c] = ch ? (0x600 + (u8)ch) : 0;
		}
}

/* Push a line into the transcript RAM only (the
 * interpreter's output path; the redraw happens at
 * the next frame-paced flush). */
static void tlinePush(const char *s)
{
	u16 r, c;
	for (r = 0; r < TROWS - 1; r++)
		for (c = 0; c < 48; c++)
			tlines[r][c] = tlines[r + 1][c];
	for (c = 0; c < 48; c++)
		tlines[TROWS - 1][c] = 0;
	for (c = 0; c < 48 && s[c]; c++)
		tlines[TROWS - 1][c] = s[c];
}

static void addLine(const char *s)
{
	tlinePush(s);
	redrawTranscript();
}

/* ------------------------------------------------ the keyboard */
static const char *kbRowsL[3] = { "qwertyuiop", "asdfghjkl", "zxcvbnm.," };
static const char *kbRowsD[3] = { "1234567890", "asdfghjkl", "zxcvbnm.," };
static const u16 kbLen[3]  = { 10, 9, 9 };
static const u16 kbCol0[3] = { 4, 6, 6 };
static const u16 kbRowY[3] = { 20, 22, 24 };

static int kbDigits = 0;
static u16 lcRow = 1, lcIdx = 0;	/* left cursor: starts on A */
static u16 rcRow = 1, rcIdx = 8;	/* right cursor: starts on L */

static char kbChar(u16 r, u16 i)
{
	return (kbDigits ? kbRowsD : kbRowsL)[r][i];
}

/* Inverted glyphs, generated at runtime into
 * CharSeg0 tiles 300.. (see buildInvertedTiles):
 * the left cursor is a TRUE inverse-video key.
 * (The BG palettes cannot be trusted with it -
 * probes 8-10, 2026-11-15: in the emulator a BG
 * palette remaps the stroke shades but shade-0
 * pixels stayed black under every value tried,
 * so inversion is done with tiles, and palettes
 * are used only to shade strokes, which the
 * probes verified: GPLT byte 0xAA puts strokes
 * at shade 2, 0x55 at shade 1.) */
static const char invSet[] = "abcdefghijklmnopqrstuvwxyz0123456789.,";

static u16 invTile(char ch)
{
	u16 k = 0;
	while (invSet[k] && invSet[k] != ch)
		k++;
	return 300 + k;
}

static void buildInvertedTiles(void)
{
	u16 k, b;
	for (k = 0; invSet[k]; k++) {
		BYTE tmp[16];
		const BYTE *src = FONT + (u8)invSet[k] * 16;
		for (b = 0; b < 16; b += 2) {
			u16 w = (u16)src[b] | ((u16)src[b + 1] << 8);
			u16 inv = 0;
			u16 px;
			for (px = 0; px < 8; px++) {
				u16 v = (w >> (2 * px)) & 3;
				inv |= (u16)(3 - v) << (2 * px);
			}
			tmp[b] = (BYTE)(inv & 0xFF);
			tmp[b + 1] = (BYTE)(inv >> 8);
		}
		copyToVip16((u16 *)(CharSeg0 + (300 + k) * 16), tmp, 8);
	}
}

static void drawKey(u16 r, u16 i)
{
	char ch = kbChar(r, i);
	int hereL = (r == lcRow && i == lcIdx);
	int hereR = (r == rcRow && i == rcIdx);
	if (hereL && hereR)
		cell(kbRowY[r], kbCol0[r] + 4 * i,
		     invTile(ch) | (3 << 14));
	else if (hereL)
		cell(kbRowY[r], kbCol0[r] + 4 * i, invTile(ch));
	else if (hereR)
		cell(kbRowY[r], kbCol0[r] + 4 * i,
		     (0x600 + (u8)ch) | (2 << 14));
	else
		cell(kbRowY[r], kbCol0[r] + 4 * i, 0x600 + (u8)ch);
}

static void drawKeyboard(void)
{
	u16 r, i;
	for (r = 0; r < 3; r++)
		for (i = 0; i < kbLen[r]; i++)
			drawKey(r, i);
	printAt(18, 40, kbDigits ? "DIGITS " : "LETTERS");
}

/* ------------------------------------------------ keyboard chrome
 * (Seth, 2026-11-16, after v1's hardware pass: a
 * SIMPLE GRID around the keys, and ONE LINE
 * separating the keyboard from the terminal —
 * nothing more graphical.) The grid tiles are
 * drawn at runtime into CharSeg0 tiles 400-415 at
 * shade 2, so the letters (shade-3 strokes) stay
 * on top of the visual hierarchy.
 *
 * GRID DISCIPLINE (Seth, ROM_DEBUG sec. 72, the
 * INFOCOM2 hardware verdict): "sharp corners,
 * nothing hanging off the end." v2 drew three
 * crude tiles (full-height vertical, full-width
 * horizontal, both), so every stroke overshot its
 * intersections — worst on the line rows shared by
 * two key bands, where one band's verticals poked
 * through the line toward the neighbouring band's
 * letters. This build generates the full
 * box-drawing set instead: one tile per arm mask
 * (N/S/W/E), each stroke running from the tile
 * centre out to an edge ONLY where the line
 * actually continues, so every segment terminates
 * exactly at its intersections. (The pure-H, pure-V
 * and full-cross masks reproduce v2's three tiles
 * pixel-for-pixel; only junction cells change.) */
#define TILE_GRID_BASE 400
#define ARM_N 1
#define ARM_S 2
#define ARM_W 4
#define ARM_E 8

static void buildGridTiles(void)
{
	u16 m, r, c;
	for (m = 1; m < 16; m++) {
		BYTE t[16];
		for (r = 0; r < 8; r++) {
			u16 w = 0;
			/* VIP packing in this lineage is LSB-first:
			 * pixel column c sits at bits 2c (the same
			 * convention buildInvertedTiles assumes) —
			 * MSB-first packing renders every asymmetric
			 * tile mirrored, which the sec. 73 emulator
			 * proof caught on the first pass. */
			for (c = 0; c < 8; c++) {
				int on = (r >= 3 && r <= 4 && c >= 3 && c <= 4)
					|| ((m & ARM_N) && c >= 3 && c <= 4 && r < 3)
					|| ((m & ARM_S) && c >= 3 && c <= 4 && r > 4)
					|| ((m & ARM_W) && r >= 3 && r <= 4 && c < 3)
					|| ((m & ARM_E) && r >= 3 && r <= 4 && c > 4);
				if (on)
					w |= (u16)(2 << (2 * c));
			}
			t[2 * r] = (BYTE)(w & 0xFF);
			t[2 * r + 1] = (BYTE)(w >> 8);
		}
		copyToVip16((u16 *)(CharSeg0 + (TILE_GRID_BASE + m) * 16),
			    t, 8);
	}
}

/* Draw the grid once, under the keys. For each
 * keyboard row-band: horizontal lines on the rows
 * above and below the keys, vertical separators in
 * the gaps between keys and at both ends. Plus the
 * separator line at row 17, the full width of the
 * screen, between the terminal (transcript and
 * input line above) and the keyboard below. Each
 * cell's tile is chosen by which arms actually
 * continue into its neighbours (hf/vf membership),
 * so strokes terminate at their intersections —
 * on the rows shared by two bands, a band's
 * verticals stop at the line instead of poking
 * through toward the other band's keys. The grid
 * is static: nothing in the interactive loop ever
 * writes rows 17 or 19-25 except drawKey, which
 * writes key cells only. */
static void drawKeyboardGrid(void)
{
	static u8 hf[28][48];
	static u8 vf[28][48];
	u16 r, i, row, col;
	for (row = 0; row < 28; row++)
		for (col = 0; col < 48; col++)
			hf[row][col] = vf[row][col] = 0;
	for (r = 0; r < 3; r++) {
		u16 top = kbRowY[r] - 1, bot = kbRowY[r] + 1;
		u16 left = kbCol0[r] - 2;
		u16 right = kbCol0[r] + 4 * (kbLen[r] - 1) + 2;
		for (col = left; col <= right; col++) {
			hf[top][col] = 1;
			hf[bot][col] = 1;
		}
		for (i = 0; i <= kbLen[r]; i++) {
			u16 sep = (i == 0) ? left :
				(i == kbLen[r]) ? right :
				kbCol0[r] + 4 * i - 2;
			for (row = top; row <= bot; row++)
				vf[row][sep] = 1;
		}
	}
	for (row = 19; row <= 25; row++)
		for (col = 0; col < 48; col++) {
			u16 m = 0;
			if (!hf[row][col] && !vf[row][col])
				continue;
			if (vf[row][col]) {
				if (vf[row - 1][col])
					m |= ARM_N;
				if (vf[row + 1][col])
					m |= ARM_S;
			}
			if (hf[row][col]) {
				if (hf[row][col - 1])
					m |= ARM_W;
				if (hf[row][col + 1])
					m |= ARM_E;
			}
			if (m)
				cell(row, col, TILE_GRID_BASE + m);
		}
	for (col = 0; col < 48; col++)
		cell(17, col, TILE_GRID_BASE + ARM_W + ARM_E);
}

static int g_story_mode = 0;	/* 0 = no story, 1 = story ended */

/* ------------------------------------------------ the input line */
#define INROW 16
static char inbuf[44];
static u16 inlen = 0;
static int inCursorOn = -1;

/* The line that was on screen when the last timed read
 * ticked, and its length. A resumed read's seed is honoured
 * only when it is byte-identical to this (see vbGetLine):
 * any other offered seed is a previous command's leftovers
 * in the story's text buffer, not a resumption. */
static char tick_seed[44];
static int tick_seed_len = 0;
static int tick_seed_valid = 0;

static void drawInputCursor(int on)
{
	cell(INROW, 3 + inlen, on ? (0x600 + 128) : 0);
	inCursorOn = on;
}

static void typeChar(char ch)
{
	if (inlen >= 43)
		return;
	putCh(INROW, 3 + inlen, ch);
	inbuf[inlen++] = ch;
	inbuf[inlen] = 0;
	drawInputCursor(1);
}

static void backspace(void)
{
	if (inlen == 0)
		return;
	inlen--;
	inbuf[inlen] = 0;
	cell(INROW, 3 + inlen + 1, 0);
	drawInputCursor(1);
}

static void submit(void)
{
	char line[49];
	u16 p = 0;
	line[p++] = '>';
	line[p++] = ' ';
	appendStr(line, &p, inbuf);
	if (inlen == 0) {
		addLine(">");
		return;
	}
	addLine(line);
	if (inlen == 4 &&
	    inbuf[0] == 'h' && inbuf[1] == 'e' &&
	    inbuf[2] == 'l' && inbuf[3] == 'p') {
		addLine("A SPACE. B DELETE. START ENTERS.");
		addLine("SELECT SWAPS LETTERS AND DIGITS.");
		addLine("L AND R SHOULDERS TYPE THE CURSORS.");
	} else if (g_story_mode) {
		addLine("STORY ENDED. RESET TO REPLAY.");
	} else {
		addLine("NO STORY LOADED.");
	}
	inlen = 0;
	inbuf[0] = 0;
	{
		u16 c;
		for (c = 3; c < 48; c++)
			cell(INROW, c, 0);
	}
	drawInputCursor(1);
}

/* Move one cursor; redraws the vacated and entered
 * cells. dir: 0 left, 1 right, 2 up, 3 down. */
static void moveCursor(int isRight, u16 dir)
{
	u16 *row = isRight ? &rcRow : &lcRow;
	u16 *idx = isRight ? &rcIdx : &lcIdx;
	u16 or_ = *row, oi = *idx;
	if (dir == 0 && *idx > 0)
		(*idx)--;
	else if (dir == 1 && *idx + 1 < kbLen[*row])
		(*idx)++;
	else if (dir == 2 && *row > 0) {
		(*row)--;
		if (*idx >= kbLen[*row])
			*idx = kbLen[*row] - 1;
	} else if (dir == 3 && *row < 2) {
		(*row)++;
		if (*idx >= kbLen[*row])
			*idx = kbLen[*row] - 1;
	}
	drawKey(or_, oi);
	drawKey(*row, *idx);
}


/* ================================================================
 * THE INTERPRETER (workspace/zmachine-vb/src/zmachine.c)
 * ================================================================ */
#include "zmachine.h"

#define ROM_BASE_ADDR 0x07000000u
static const u8 *DESC_PTR = (const u8 *)(ROM_BASE_ADDR + 0xFFFC0u);
static const u8 *STORY_PTR = (const u8 *)(ROM_BASE_ADDR + 0x100000u);

/* story_state: 0 = no story, 1 = descriptor present (unverified),
 * 2 = verified and good, 3 = present but corrupt */
static int story_state = 0;
static u32 story_len = 0;
static char story_title[33];

static u32 rd32le(const u8 *p)
{
	return (u32)p[0] | ((u32)p[1] << 8) | ((u32)p[2] << 16) |
	       ((u32)p[3] << 24);
}

static void detectStoryQuick(void)
{
	u16 i;
	story_state = 0;
	story_len = 0;
	story_title[0] = 0;
	if (DESC_PTR[0] != 'V' || DESC_PTR[1] != 'B' ||
	    DESC_PTR[2] != 'Z' || DESC_PTR[3] != 'S' ||
	    DESC_PTR[4] != 'T' || DESC_PTR[5] != 'O' ||
	    DESC_PTR[6] != 'R' || DESC_PTR[7] != 'Y')
		return;
	if (rd32le(DESC_PTR + 8) != 0x100000u)
		return;
	story_len = rd32le(DESC_PTR + 12);
	if (story_len < 64 || story_len > 0x100000u)
		return;
	if (DESC_PTR[30] != 3 && DESC_PTR[30] != 4 &&
	    DESC_PTR[30] != 5 && DESC_PTR[30] != 8)
		return;
	for (i = 0; i < 32; i++) {
		char ch = (char)DESC_PTR[32 + i];
		if (ch == 0)
			break;
		story_title[i] = ch;
		story_title[i + 1] = 0;
	}
	if (story_title[0] == 0) {
		story_title[0] = '?';
		story_title[1] = 0;
	}
	story_state = 1;
}

static u32 crc32Of(const u8 *p, u32 n)
{
	u32 crc = 0xFFFFFFFFu;
	u32 i;
	int k;
	for (i = 0; i < n; i++) {
		crc ^= p[i];
		for (k = 0; k < 8; k++)
			crc = (crc & 1) ? (crc >> 1) ^ 0xEDB88320u
					: (crc >> 1);
	}
	return crc ^ 0xFFFFFFFFu;
}

/* Full verification, run during the ritual's LOADING pause:
 * the story header checksum AND the descriptor CRC32 must both
 * match before the interpreter is allowed near the story. */
static void verifyStory(void)
{
	u32 i, sum = 0;
	u16 want;
	if (story_state != 1)
		return;
	want = (u16)((STORY_PTR[0x1C] << 8) | STORY_PTR[0x1D]);
	for (i = 0x40; i < story_len; i++)
		sum += STORY_PTR[i];
	if ((u16)(sum & 0xFFFF) != want) {
		story_state = 3;
		return;
	}
	if (crc32Of(STORY_PTR, story_len) != rd32le(DESC_PTR + 16)) {
		story_state = 3;
		return;
	}
	story_state = 2;
}

/* ---- interpreter memory: one 52 KB arena in WRAM ---- */
#define ZM_DYN_CAP 0x7000	/* 28 KB dynamic-memory budget */
static uint8_t zm_arena[52 * 1024];
static uint16_t zm_undo_stack[ZM_STACK_WORDS];
static ZmFrame zm_undo_frames[ZM_MAX_FRAMES];
static Zm zm;
static ZmIO zmio;

/* ---- the interpreter's terminal: word-wrapped lines into the
 * transcript RAM; redraws only at frame-paced flushes ---- */
static char curline[49];
static int curllen = 0;
static int term_dirty = 0;
static uint16_t upper_stash[48];
static int upper_stash_valid = 0;

static void termCommit(void)
{
	curline[curllen] = 0;
	tlinePush(curline);
	curllen = 0;
	curline[0] = 0;
	term_dirty = 1;
}

static void vbFlush(Zm *z);

static void vbPutChar(Zm *z, uint16_t c)
{
	(void)z;
	if (c == 13) {
		termCommit();
		return;
	}
	if (c < 32 || c > 126)
		c = '?';
	if (curllen >= 48) {
		/* wrap at the last space, else hard-break the line */
		int i, sp = -1;
		for (i = curllen - 1; i > 0; i--) {
			if (curline[i] == ' ') {
				sp = i;
				break;
			}
		}
		if (sp > 0) {
			char tail[49];
			int tailn = curllen - (sp + 1);
			int j;
			for (j = 0; j < tailn; j++)
				tail[j] = curline[sp + 1 + j];
			curline[sp] = 0;
			curllen = sp;
			termCommit();
			for (j = 0; j < tailn; j++)
				curline[j] = tail[j];
			curllen = tailn;
			curline[curllen] = 0;
		} else {
			termCommit();
		}
	}
	curline[curllen++] = (char)c;
	curline[curllen] = 0;
}

static void vbDrawUpper(Zm *z, const uint16_t *cells, int width,
			int lines)
{
	u16 i;
	(void)z;
	(void)lines;
	for (i = 0; i < 48; i++)
		upper_stash[i] = (i < width) ? cells[i] : ' ';
	upper_stash_valid = 1;
}

static void vbFlush(Zm *z)
{
	u16 c;
	(void)z;
	waitVerifiedFrame();
	if (term_dirty) {
		redrawTranscript();
		term_dirty = 0;
	}
	/* the uncommitted partial line lives on row 15 */
	for (c = 0; c < 48; c++)
		cell(15, c, (c < curllen && curline[c])
			   ? (0x600 + (u8)curline[c]) : 0);
	if (upper_stash_valid) {
		for (c = 0; c < 48; c++) {
			uint16_t ch = upper_stash[c];
			cell(0, c, (ch >= 32 && ch <= 126)
				   ? (0x600 + ch) : 0);
		}
	}
}

static void vbClearScreen(Zm *z)
{
	u16 r, c;
	(void)z;
	for (r = 0; r < TROWS; r++)
		for (c = 0; c < 48; c++)
			tlines[r][c] = 0;
	curllen = 0;
	curline[0] = 0;
	term_dirty = 1;
}

/* ---- input: the dual-cursor keyboard, driven per frame ---- */
static void drawInputLine(void)
{
	u16 c;
	for (c = 3; c < 48; c++)
		cell(INROW, c, 0);
	for (c = 0; c < inlen; c++)
		putCh(INROW, 3 + c, inbuf[c]);
	drawInputCursor(1);
}

/* Shared pad handling for the interpreter's input loops.
 * Returns 1 when Start was pressed (line finished). */
static u16 padServicePrev = 0;

static int padService(int *lDir, u16 *lHold, int *rDir, u16 *rHold)
{
	u16 pad = vbReadPad() & K_ANY;
	u16 edges = (u16)(pad & ~padServicePrev);
	padServicePrev = pad;
	{
		int d = -1;
		if (pad & K_LL) d = 0;
		else if (pad & K_LR) d = 1;
		else if (pad & K_LU) d = 2;
		else if (pad & K_LD) d = 3;
		if (d < 0) { *lDir = -1; *lHold = 0; }
		else if (d != *lDir) { moveCursor(0, (u16)d); *lDir = d; *lHold = 0; }
		else if (++(*lHold) == 16 || (*lHold > 16 && (*lHold - 16) % 6 == 0))
			moveCursor(0, (u16)d);
	}
	{
		int d = -1;
		if (pad & K_RL) d = 0;
		else if (pad & K_RR) d = 1;
		else if (pad & K_RU) d = 2;
		else if (pad & K_RD) d = 3;
		if (d < 0) { *rDir = -1; *rHold = 0; }
		else if (d != *rDir) { moveCursor(1, (u16)d); *rDir = d; *rHold = 0; }
		else if (++(*rHold) == 16 || (*rHold > 16 && (*rHold - 16) % 6 == 0))
			moveCursor(1, (u16)d);
	}
	if (edges & K_LT)
		typeChar(kbChar(lcRow, lcIdx));
	if (edges & K_RT)
		typeChar(kbChar(rcRow, rcIdx));
	if (edges & K_A)
		typeChar(' ');
	if (edges & K_B)
		backspace();
	if (edges & K_SEL) {
		kbDigits = !kbDigits;
		drawKeyboard();
	}
	return (edges & K_STA) != 0;
}

static int vbGetLine(Zm *z, char *buf, int max, int *len_out,
		     const char *initial, int initial_len,
		     int timeout_tenths, int *term_out)
{
	int lDir = -1, rDir = -1;
	u16 lHold = 0, rHold = 0;
	u32 frames = 0, tick_at = 0;
	int i;
	(void)z;
	(void)max;
	/* A new read starts with an empty input line. The core
	 * offers the story buffer's current contents as `initial`,
	 * and after any completed read that buffer still holds
	 * the previous command (the core leaves it there for the
	 * game to parse) — seeding from it verbatim resurrects
	 * the last command at every prompt, to be erased by hand
	 * (hardware, Nov 18: TEST CHAMBER). The one legitimate
	 * seed is a timed read resuming after a tick: the core
	 * re-calls us with the very line that was on screen when
	 * the tick fired. Honour the seed only when it is
	 * byte-identical to that line; anything else is a
	 * previous command's leftovers and is dropped. */
	inlen = 0;
	inbuf[0] = 0;
	if (tick_seed_valid && initial_len == tick_seed_len) {
		for (i = 0; i < initial_len; i++)
			if (initial[i] != tick_seed[i])
				break;
		if (i == initial_len) {
			for (i = 0; i < initial_len && i < 43; i++)
				inbuf[inlen++] = initial[i];
			inbuf[inlen] = 0;
		}
	}
	tick_seed_valid = 0;
	/* The game prints its prompt (">") into curline, which
	 * vbFlush displays on row 15 — directly above the input
	 * row, which has its own '>' at column 2. A bare prompt
	 * in curline is a duplicate: drop it so row 15 stays
	 * blank. Anything longer is real text; leave it. The
	 * submit echo rebuilds "> " + input on its own. */
	if ((curllen == 1 && curline[0] == '>') ||
	    (curllen == 2 && curline[0] == '>' && curline[1] == ' ')) {
		curllen = 0;
		curline[0] = 0;
		term_dirty = 1;
	}
	drawInputLine();
	if (timeout_tenths > 0)
		tick_at = (u32)timeout_tenths * 5;	/* 50 Hz frames */
	padServicePrev = vbReadPad() & K_ANY;
	for (;;) {
		int done;
		waitVerifiedFrame();
		frames++;
		if (term_dirty || upper_stash_valid)
			vbFlush(z);
		done = padService(&lDir, &lHold, &rDir, &rHold);
		{
			int on = ((frames % 50) < 25) ? 1 : 0;
			if (on != inCursorOn)
				drawInputCursor(on);
		}
		if (done)
			break;
		if (tick_at && frames >= tick_at) {
			for (i = 0; i < inlen; i++) {
				buf[i] = inbuf[i];
				tick_seed[i] = inbuf[i];
			}
			tick_seed[inlen] = 0;
			tick_seed_len = inlen;
			tick_seed_valid = 1;
			buf[inlen] = 0;
			*len_out = inlen;
			*term_out = 0;
			return ZM_LINE_TICK;
		}
	}
	/* echo the submitted line into the transcript */
	{
		char line[49];
		u16 p = 0;
		line[p++] = '>';
		line[p++] = ' ';
		appendStr(line, &p, inbuf);
		tlinePush(line);
		term_dirty = 1;
	}
	for (i = 0; i < inlen; i++)
		buf[i] = inbuf[i];
	buf[inlen] = 0;
	*len_out = inlen;
	*term_out = 13;
	inlen = 0;
	inbuf[0] = 0;
	drawInputLine();
	return ZM_LINE_OK;
}

static int vbGetCharCommon(Zm *z, int timeout_tenths)
{
	u32 frames = 0, tick_at = 0;
	u16 prev = vbReadPad() & K_ANY;
	(void)z;
	if (timeout_tenths > 0)
		tick_at = (u32)timeout_tenths * 5;
	for (;;) {
		u16 pad, edges;
		waitVerifiedFrame();
		frames++;
		if (term_dirty || upper_stash_valid)
			vbFlush(z);
		pad = vbReadPad() & K_ANY;
		edges = (u16)(pad & ~prev);
		prev = pad;
		if (edges & K_LT)
			return kbChar(lcRow, lcIdx);
		if (edges & K_RT)
			return kbChar(rcRow, rcIdx);
		if (edges & (K_A | K_STA))
			return 13;
		if (edges & K_B)
			return 8;
		if (edges & K_SEL) {
			kbDigits = !kbDigits;
			drawKeyboard();
		}
		if (tick_at && frames >= tick_at)
			return -ZM_LINE_TICK;
	}
}

static int vbGetChar(Zm *z)
{
	return vbGetCharCommon(z, 0);
}

static int vbGetCharTimed(Zm *z, int timeout_tenths)
{
	return vbGetCharCommon(z, timeout_tenths);
}

static uint32_t vbRand32(Zm *z)
{
	static uint32_t s = 0;
	(void)z;
	if (s == 0)
		s = 0x9E3779B9u ^ (g_frames * 2654435761u) ^ vbReadPad();
	s = s * 1664525u + 1013904223u;
	return s >> 8;
}

/* ---- Quetzal streams to cartridge SRAM (8 KB budget) ---- */
/* Commercial byte-lane addressing (VB-GRAMMAR #4): the cart
 * carries one data byte per 16-bit cell, so stream byte n
 * lives at base + 2n and is reached by a byte access at an
 * even address. A consecutive-byte stream loses its odd
 * bytes on such hardware — the original SAVE/RESTORE fault:
 * the FORM header came back with its odd bytes destroyed,
 * so RESTORE's validity gate failed. An 8 KB stream thus
 * spans 16 KB of address space, exactly the span the 8 KB
 * chip is decoded across. Accesses stay byte-wide
 * throughout: word writes to SRAM can silently no-op on
 * flash carts. First access happens only when the player
 * issues SAVE or RESTORE — long after power-on, and there
 * is no boot-time SRAM probe — so the wake-up wait of
 * VB-GRAMMAR #4 is satisfied by construction. */
#define SRAM_BYTES ((volatile u8 *)0x06000000)
#define SRAM_CAP 8192
#define SRAM_LANE(n) (2u * (n))
static u32 sram_w = 0, sram_r = 0, sram_limit = 0;
static int sram_err = 0;

static int vbSaveBegin(Zm *z)
{
	(void)z;
	sram_w = 0;
	sram_err = 0;
	return 0;
}

static int vbSaveByte(Zm *z, int b)
{
	(void)z;
	if (sram_w >= SRAM_CAP) {
		sram_err = 1;
		return -1;
	}
	SRAM_BYTES[SRAM_LANE(sram_w)] = (u8)b;
	sram_w++;
	return 0;
}

static int vbSaveEnd(Zm *z)
{
	u32 form_len;
	(void)z;
	if (sram_err)
		return -1;
	/* Verify the write actually landed before reporting
	 * success: a store to an unpopulated or unmapped SRAM
	 * window is indistinguishable from a good store at write
	 * time, so read back the FORM header and its length
	 * field. A failed verify makes @save store 0, so the
	 * game reports the failure at SAVE time. */
	if (sram_w < 12)
		return -1;
	if (SRAM_BYTES[SRAM_LANE(0)] != 'F' ||
	    SRAM_BYTES[SRAM_LANE(1)] != 'O' ||
	    SRAM_BYTES[SRAM_LANE(2)] != 'R' ||
	    SRAM_BYTES[SRAM_LANE(3)] != 'M' ||
	    SRAM_BYTES[SRAM_LANE(8)] != 'I' ||
	    SRAM_BYTES[SRAM_LANE(9)] != 'F' ||
	    SRAM_BYTES[SRAM_LANE(10)] != 'Z' ||
	    SRAM_BYTES[SRAM_LANE(11)] != 'S')
		return -1;
	form_len = ((u32)SRAM_BYTES[SRAM_LANE(4)] << 24) |
		   ((u32)SRAM_BYTES[SRAM_LANE(5)] << 16) |
		   ((u32)SRAM_BYTES[SRAM_LANE(6)] << 8) |
		   (u32)SRAM_BYTES[SRAM_LANE(7)];
	if (form_len != sram_w - 8)
		return -1;
	return 0;
}

static int vbRestoreBegin(Zm *z)
{
	(void)z;
	if (SRAM_BYTES[SRAM_LANE(0)] != 'F' ||
	    SRAM_BYTES[SRAM_LANE(1)] != 'O' ||
	    SRAM_BYTES[SRAM_LANE(2)] != 'R' ||
	    SRAM_BYTES[SRAM_LANE(3)] != 'M')
		return -1;
	sram_limit = 8u + (((u32)SRAM_BYTES[SRAM_LANE(4)] << 24) |
			   ((u32)SRAM_BYTES[SRAM_LANE(5)] << 16) |
			   ((u32)SRAM_BYTES[SRAM_LANE(6)] << 8) |
			   (u32)SRAM_BYTES[SRAM_LANE(7)]);
	if (sram_limit > SRAM_CAP)
		sram_limit = SRAM_CAP;
	sram_r = 0;
	return 0;
}

static int vbRestoreByte(Zm *z)
{
	int b;
	(void)z;
	if (sram_r >= sram_limit)
		return -1;
	b = SRAM_BYTES[SRAM_LANE(sram_r)];
	sram_r++;
	return b;
}

static void vbRestoreEnd(Zm *z)
{
	(void)z;
}

static void vbFatal(Zm *z, const char *msg)
{
	(void)z;
	termCommit();
	tlinePush("INTERPRETER ERROR:");
	tlinePush(msg);
	term_dirty = 1;
	vbFlush(z);
	for (;;)
		waitVerifiedFrame();
}

/* ---- run the story ---- */
static void runStory(void)
{
	static char err[80];
	uint32_t dyn_size = ((u32)STORY_PTR[0x0E] << 8) | STORY_PTR[0x0F];
	uint8_t *undo_dyn = 0;
	u16 r, c;

	/* the interpreter's screen: transcript, prompt, keyboard */
	clearVip16(BG0, 0, 8192 / 2);
	for (r = 0; r < TROWS; r++)
		for (c = 0; c < 48; c++)
			tlines[r][c] = 0;
	curllen = 0;
	curline[0] = 0;
	term_dirty = 0;
	upper_stash_valid = 0;
	putCh(INROW, 2, '>');
	inlen = 0;
	inbuf[0] = 0;
	drawInputCursor(1);
	printAt(18, 2, "KEYBOARD");
	drawKeyboardGrid();
	drawKeyboard();
	printAt(26, 2, "L/R TYPE - A SPACE - B DEL - START GO");

	zmio.user = 0;
	zmio.put_char = vbPutChar;
	zmio.flush = vbFlush;
	zmio.draw_upper = vbDrawUpper;
	zmio.clear_screen = vbClearScreen;
	zmio.beep = 0;
	zmio.get_line = vbGetLine;
	zmio.get_char = vbGetChar;
	zmio.get_char_timed = vbGetCharTimed;
	zmio.rand32 = vbRand32;
	zmio.seed_rand = 0;
	zmio.save_begin = vbSaveBegin;
	zmio.save_byte = vbSaveByte;
	zmio.save_end = vbSaveEnd;
	zmio.restore_begin = vbRestoreBegin;
	zmio.restore_byte = vbRestoreByte;
	zmio.restore_end = vbRestoreEnd;
	zmio.fatal = vbFatal;

	if (dyn_size <= 0x6800)
		undo_dyn = zm_arena + dyn_size;
	if (zm_init(&zm, STORY_PTR, story_len, zm_arena, ZM_DYN_CAP,
		    undo_dyn, zm_undo_stack, zm_undo_frames,
		    &zmio, err, sizeof err)) {
		addLine("THIS STORY DOES NOT FIT:");
		addLine(err);
		addLine("PRESS START.");
		while (!(vbReadPad() & K_STA))
			waitVerifiedFrame();
		return;
	}
	zm_run(&zm);
	g_story_mode = 1;
	termCommit();
	tlinePush("");
	tlinePush("THE STORY HAS ENDED.");
	term_dirty = 1;
	vbFlush(&zm);
	waitN(50);
}

static void frontEndFallback(void)
{
	/* --- THE FRONT-END SCREEN --- */
	clearVip16(BG0, 0, 8192 / 2);
	{
		u16 r, c;
		for (r = 0; r < TROWS; r++)
			for (c = 0; c < 48; c++)
				tlines[r][c] = 0;
	}
	if (g_story_mode) {
		addLine("VIRTUAL INFOCOM");
		addLine("THE STORY HAS ENDED.");
		addLine("RESET THE CONSOLE TO PLAY AGAIN.");
	} else {
		addLine("VIRTUAL INFOCOM");
		addLine("NO STORY FILE IS LOADED.");
		addLine("THE KEYBOARD IS LIVE.");
	}
	addLine("LEFT PAD: INVERTED CURSOR. RIGHT PAD:");
	addLine("SHADED CURSOR. SHOULDERS TYPE.");
	addLine("TYPE HELP, THEN PRESS START.");
	putCh(INROW, 2, '>');
	drawInputCursor(1);
	printAt(18, 2, "KEYBOARD");
	drawKeyboardGrid();
	drawKeyboard();
	printAt(26, 2, "L/R TYPE - A SPACE - B DEL - START GO");

	/* --- THE INTERACTIVE LOOP: one verified frame
	 * per pass; pad edges act once, held directions
	 * repeat, the input cursor blinks hard 25/25. --- */
	{
		u16 prevPad = 0;
		u32 frames = 0;
		int lDir = -1, rDir = -1;
		u16 lHold = 0, rHold = 0;

		while (1) {
			u16 pad, edges;
			int on;
			waitVerifiedFrame();
			frames++;
			pad = vbReadPad() & K_ANY;
			edges = (u16)(pad & ~prevPad);
			prevPad = pad;

			/* directions, with auto-repeat */
			{
				int d = -1;
				if (pad & K_LL) d = 0;
				else if (pad & K_LR) d = 1;
				else if (pad & K_LU) d = 2;
				else if (pad & K_LD) d = 3;
				if (d < 0) { lDir = -1; lHold = 0; }
				else if (d != lDir) { moveCursor(0, (u16)d); lDir = d; lHold = 0; }
				else if (++lHold == 16 || (lHold > 16 && (lHold - 16) % 6 == 0))
					moveCursor(0, (u16)d);
			}
			{
				int d = -1;
				if (pad & K_RL) d = 0;
				else if (pad & K_RR) d = 1;
				else if (pad & K_RU) d = 2;
				else if (pad & K_RD) d = 3;
				if (d < 0) { rDir = -1; rHold = 0; }
				else if (d != rDir) { moveCursor(1, (u16)d); rDir = d; rHold = 0; }
				else if (++rHold == 16 || (rHold > 16 && (rHold - 16) % 6 == 0))
					moveCursor(1, (u16)d);
			}

			/* actions, on press edges */
			if (edges & K_LT)
				typeChar(kbChar(lcRow, lcIdx));
			if (edges & K_RT)
				typeChar(kbChar(rcRow, rcIdx));
			if (edges & K_A)
				typeChar(' ');
			if (edges & K_B)
				backspace();
			if (edges & K_SEL) {
				kbDigits = !kbDigits;
				drawKeyboard();
			}
			if (edges & K_STA)
				submit();

			/* the input cursor's hard blink */
			on = ((frames % 50) < 25) ? 1 : 0;
			if (on != inCursorOn)
				drawInputCursor(on);
		}
	}
}

int main(void)
{
	/* --- Setup while the display/drawing are OFF (B2 discipline) --- */
	clearVip32((volatile u32 *)0x00000000, 0x00040000 / 4);
	copyToVip16((u16*)CharSeg3, FONT, (512 * 16) / 2);
	clearVip16((u16*)(CharSeg3 + 128 * 16), 0xFFFF, 16 / 2);

	/* --- THE GATE SCREEN (the approved VIRTUAL
	 * INFOCOM calibration gate, gatedata.h — the
	 * boxed lockup, halftone dot blocks above and
	 * below the box as focus targets, corner squares
	 * split diagonally by eye): map 0 = LEFT eye,
	 * map 1 = RIGHT eye. The gate tiles live in
	 * CharSeg0; the keyboard's runtime tiles
	 * (inverted glyphs at 300+, grid at 400+) are
	 * written only after the gate and do not
	 * collide with the gate pool (143 tiles). --- */
	copyToVip16((u16*)CharSeg0, gateChars,
		    (GATE_TILE_COUNT * 16) / 2);
	clearVip16((u16*)BGMap(0), 0, 8192 / 2);
	clearVip16((u16*)BGMap(1), 0, 8192 / 2);
	bgGateMap(0, gateMapL);
	bgGateMap(1, gateMapR);

	WORLD_HEAD(31, WRLD_LON | WRLD_BGMAP | 0);
	WORLD_GSET(31, 0, 0, 0);
	WORLD_SIZE(31, 384, 224);
	WORLD_HEAD(30, WRLD_RON | WRLD_BGMAP | 1);
	WORLD_GSET(30, 0, 0, 0);
	WORLD_SIZE(30, 384, 224);
	WORLD_HEAD(29, WRLD_END);

	vbSetColTable();
	vbDisplayOn();
	vbDisplayShow();

	/* --- THE HUMAN GATE --- */
	while (!(vbReadPad() & (K_A | K_STA)))
		;

	/* --- Through the gate: one both-eyes world on
	 * map 0, cursor palettes armed, warm-up. --- */
	WORLD_HEAD(31, WRLD_ON | WRLD_BGMAP | 0);
	WORLD_GSET(31, 0, 0, 0);
	WORLD_SIZE(31, 384, 224);
	WORLD_HEAD(30, WRLD_END);
	BG0 = (u16 *)BGMap(0);
	clearVip16(BG0, 0, 8192 / 2);
	VIP_REGS[GPLT0 + 2] = 0xAA;	/* right cursor: strokes to shade 2 */
	VIP_REGS[GPLT0 + 3] = 0x55;	/* both cursors: strokes to shade 1 */
	buildInvertedTiles();
	buildGridTiles();
	{
		u16 warm;
		for (warm = 0; warm < 30; warm++)
			waitVerifiedFrame();
	}

	detectStoryQuick();

	/* --- THE C64 BOOT SCREEN --- */
	{
		u32 ramK = probeRamKB();
		char spec[49];
		u16 p = 0;
		printCentered(2, "**** VIRTUAL INFOCOM ****");
		printCentered(4, "V810 AT 20 MHZ - 384 X 224 DISPLAY");
		spec[0] = 0;
		appendDec(spec, &p, ramK);
		appendStr(spec, &p, "K RAM SYSTEM - ");
		appendDec(spec, &p, 1024);
		appendStr(spec, &p, "K STORY SPACE FREE");
		printCentered(5, spec);
		if (story_state != 0) {
			char tline[49];
			u16 tp = 0;
			tline[0] = 0;
			appendStr(tline, &tp, "STORY: ");
			appendStr(tline, &tp, story_title);
			printCentered(6, tline);
		}
		printAt(7, 2, "READY.");
		/* the idle block cursor, ~1 s */
		cell(8, 2, 0x600 + 128);
		waitN(25);
		cell(8, 2, 0);
		waitN(25);
		/* the machine launches itself */
		typeText(8, 2, "LOAD", 8);
		waitN(25);
		printAt(10, 2, "SEARCHING FOR STORY");
		waitN(30);
		if (story_state == 0) {
			printAt(11, 2, "?FILE NOT FOUND ERROR");
			waitN(60);
		} else {
			printAt(11, 2, "LOADING");
			verifyStory();
			waitN(80);
			if (story_state == 3) {
				printAt(12, 2, "?STORY CHECKSUM ERROR");
				waitN(60);
			}
		}
		printAt(13, 2, "READY.");
		waitN(30);
		if (story_state == 2) {
			typeText(14, 2, "RUN", 8);
			waitN(70);
		}
	}

	/* --- RUN: the interpreter, or the front end --- */
	if (story_state == 2)
		runStory();

	frontEndFallback();
	return 0;
}
