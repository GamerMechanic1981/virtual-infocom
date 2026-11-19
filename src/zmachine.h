/*
 * zmachine.h -- Virtual Infocom Z-machine interpreter core.
 *
 * A portable Z-machine (versions 3, 4, 5; 8 shares the v5 code path
 * with a different packing scale) written for the Virtual Boy port
 * in ~/workspace/zmachine-vb (see DESIGN_SEED.md). The core has no
 * platform dependencies: all I/O goes through the ZmIO callbacks,
 * all memory is supplied by the caller. No libc calls are made, so
 * the same file links into the freestanding V810 build.
 *
 * Memory model (per the Nov 15 Claude consult): only DYNAMIC memory
 * is copied to caller RAM; static and high memory are read directly
 * from the story image. All 16-bit story accesses are big-endian
 * through accessors; the story image itself is never modified.
 *
 * Saves are Quetzal (IFZS): IFhd + CMem (XOR-diff against the
 * pristine story dynamic memory, RLE) + Stks. One level of
 * save_undo/restore_undo is kept in caller-supplied snapshot RAM.
 */

#ifndef ZMACHINE_H
#define ZMACHINE_H

#include <stdint.h>

#ifndef ZM_STACK_WORDS
#define ZM_STACK_WORDS 1024
#endif
#ifndef ZM_MAX_FRAMES
#define ZM_MAX_FRAMES 64
#endif
#ifndef ZM_UPPER_LINES
#define ZM_UPPER_LINES 32
#endif
#ifndef ZM_UPPER_COLS
#define ZM_UPPER_COLS 64
#endif

/* get_line return codes */
#define ZM_LINE_OK   0    /* Enter pressed; buf/len filled          */
#define ZM_LINE_TICK 1    /* timeout tick; buf/len hold partial text */
#define ZM_LINE_EOF (-1)  /* no more input (host harness end)        */

typedef struct Zm Zm;

typedef struct {
    void *user;

    /* --- output --- */
    /* One ZSCII character for the lower window / transcript. */
    void (*put_char)(Zm *zm, uint16_t zscii);
    /* Flush pending output to the display (frame boundary on VB). */
    void (*flush)(Zm *zm);
    /* The upper window changed: cells are ZSCII, row-major,
     * width x lines; draw as much as the display supports. */
    void (*draw_upper)(Zm *zm, const uint16_t *cells, int width, int lines);
    /* Erase the lower window (erase_window 0 / restart). */
    void (*clear_screen)(Zm *zm);
    /* Optional: a beep for sound_effect 1/2. May be NULL. */
    void (*beep)(Zm *zm);

    /* --- input --- */
    /* Read one line into buf (capacity max chars, not counting the
     * NUL the callback may add). initial/initial_len: text already
     * in the story's text buffer (V5 pre-fill). timeout_tenths > 0:
     * return ZM_LINE_TICK after that many tenths with the partial
     * text in buf and *len_out set. *term_out receives the ZSCII
     * code of the terminating key (13 for Enter). */
    int (*get_line)(Zm *zm, char *buf, int max, int *len_out,
                    const char *initial, int initial_len,
                    int timeout_tenths, int *term_out);
    /* Read a single ZSCII character. timeout_tenths as above;
     * returns the character, or -ZM_LINE_TICK on a tick, or
     * ZM_LINE_EOF at end of input. */
    int (*get_char)(Zm *zm);
    int (*get_char_timed)(Zm *zm, int timeout_tenths);

    /* --- randomness --- */
    /* Raw entropy / generator. seed_rand(zm, 0) means "reseed
     * unpredictably"; any other value is an exact seed. */
    uint32_t (*rand32)(Zm *zm);
    void (*seed_rand)(Zm *zm, uint32_t seed);

    /* --- Quetzal save streams --- */
    /* save: begin returns 0 on success. save_byte returns 0 on
     * success (e.g. SRAM full -> nonzero). save_end returns 0 on
     * success. restore_byte returns -1 at end of stream. */
    int (*save_begin)(Zm *zm);
    int (*save_byte)(Zm *zm, int byte);
    int (*save_end)(Zm *zm);
    int (*restore_begin)(Zm *zm);
    int (*restore_byte)(Zm *zm);
    void (*restore_end)(Zm *zm);

    /* --- fatal errors --- */
    /* Called with a short message; should not return (if it does,
     * the core halts the run loop). */
    void (*fatal)(Zm *zm, const char *msg);
} ZmIO;

typedef struct {
    uint32_t ret_pc;
    int16_t ret_store;   /* variable number, or -1 for call_vn */
    uint16_t fp;         /* stack index of first local */
    uint8_t nlocals;
    uint8_t nargs;
} ZmFrame;

struct Zm {
    const ZmIO *io;

    const uint8_t *story;     /* pristine story image (ROM/RAM) */
    uint32_t story_len;       /* bytes available at story         */
    uint32_t file_len;        /* length declared in the header    */
    uint8_t *dyn;             /* dynamic memory copy              */
    uint32_t dyn_size;        /* = header static-memory base      */
    uint32_t dyn_cap;         /* size of the dyn buffer           */

    uint8_t version;
    uint32_t pc;
    int halted;               /* 0 run, 1 quit, -1 fatal/EOF      */

    uint16_t stack[ZM_STACK_WORDS];
    int sp;                   /* next free stack slot             */
    ZmFrame frames[ZM_MAX_FRAMES];
    int frame_count;

    /* header-derived addresses */
    uint16_t dict_addr, objtab_addr, globals_addr, abbrev_addr;
    uint16_t alphabet_addr, exttab_addr, termkeys_addr;

    /* screen state */
    int cur_window;           /* 0 lower, 1 upper                 */
    int upper_lines;          /* split_window height              */
    uint16_t upper[ZM_UPPER_LINES][ZM_UPPER_COLS];
    int upper_x, upper_y;     /* cursor in upper window (0-based) */
    int upper_dirty;
    int buffer_mode;          /* 1 = word-wrap buffering on       */
    int stream1_on, stream2_on;
    int stream3_depth;
    uint16_t stream3_table[4];
    int text_style;

    /* input state */
    uint16_t last_read_result;

    /* undo snapshot (single level) */
    uint8_t *undo_dyn;
    uint16_t *undo_stack;
    ZmFrame *undo_frames;
    int undo_valid;
    int undo_sp, undo_frame_count;
    uint32_t undo_pc;
    uint32_t undo_dyn_size;

    uint32_t rng_state;
    int tick_routine_pending; /* internal */
};

/* Initialise the interpreter over a story image. dyn/undo buffers
 * are caller-owned. Returns 0 on success, -1 with a message in
 * errbuf on failure (bad version, dynamic memory too large, ...). */
int zm_init(Zm *zm, const uint8_t *story, uint32_t story_len,
            uint8_t *dyn, uint32_t dyn_cap,
            uint8_t *undo_dyn, uint16_t *undo_stack, ZmFrame *undo_frames,
            const ZmIO *io, char *errbuf, int errbuf_len);

/* Run until @quit, a fatal error, or input EOF.
 * Returns 1 on quit, -1 on fatal/EOF, 0 if it returns for another
 * reason (should not happen). */
int zm_run(Zm *zm);

/* Restart the game (the @restart opcode path, exposed for hosts). */
void zm_restart(Zm *zm);

#endif /* ZMACHINE_H */
