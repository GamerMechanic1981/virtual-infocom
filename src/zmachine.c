/*
 * zmachine.c -- Virtual Infocom Z-machine interpreter core.
 * See zmachine.h for the contract and DESIGN_SEED.md for the spec.
 * Versions 3, 4, 5 supported (8 shares the v5 path, scale 8).
 * No libc: all byte work is explicit loops (the V810 build is
 * freestanding).
 */

#include "zmachine.h"

/* ---------------------------------------------------------------- */
/* small utilities                                                  */
/* ---------------------------------------------------------------- */

static void mem_copy(uint8_t *dst, const uint8_t *src, uint32_t n)
{
    uint32_t i;
    for (i = 0; i < n; i++)
        dst[i] = src[i];
}

static void fatal(Zm *zm, const char *msg)
{
    if (zm->halted == 0)
        zm->halted = -1;
    if (zm->io && zm->io->fatal)
        zm->io->fatal(zm, msg);
}

/* ---------------------------------------------------------------- */
/* story memory access (big-endian; dynamic region is writable)     */
/* ---------------------------------------------------------------- */

static uint8_t pr8(const Zm *zm, uint32_t a)
{
    return (a < zm->story_len) ? zm->story[a] : 0;
}

static uint16_t pr16(const Zm *zm, uint32_t a)
{
    return (uint16_t)((pr8(zm, a) << 8) | pr8(zm, a + 1));
}

static uint8_t rd8(Zm *zm, uint32_t a)
{
    if (a < zm->dyn_size)
        return zm->dyn[a];
    if (a < zm->story_len)
        return zm->story[a];
    fatal(zm, "read outside story memory");
    return 0;
}

static uint16_t rd16(Zm *zm, uint32_t a)
{
    return (uint16_t)((rd8(zm, a) << 8) | rd8(zm, a + 1));
}

static void wr8(Zm *zm, uint32_t a, uint8_t v)
{
    if (a < zm->dyn_size) {
        zm->dyn[a] = v;
        return;
    }
    fatal(zm, "write to static or high memory");
}

static void wr16(Zm *zm, uint32_t a, uint16_t v)
{
    wr8(zm, a, (uint8_t)(v >> 8));
    wr8(zm, a + 1, (uint8_t)(v & 0xFF));
}

/* ---------------------------------------------------------------- */
/* variables and the evaluation stack                               */
/* ---------------------------------------------------------------- */

static void push(Zm *zm, uint16_t v)
{
    if (zm->sp >= ZM_STACK_WORDS) {
        fatal(zm, "evaluation stack overflow");
        return;
    }
    zm->stack[zm->sp++] = v;
}

static uint16_t pop(Zm *zm)
{
    if (zm->sp <= 0) {
        fatal(zm, "evaluation stack underflow");
        return 0;
    }
    return zm->stack[--zm->sp];
}

/* Read a variable as an operand: variable 0 pops the stack. */
static uint16_t read_var(Zm *zm, int n)
{
    if (n == 0)
        return pop(zm);
    if (n < 16) {
        ZmFrame *f;
        if (zm->frame_count == 0) {
            fatal(zm, "local variable with no frame");
            return 0;
        }
        f = &zm->frames[zm->frame_count - 1];
        if (n > f->nlocals) {
            fatal(zm, "local variable out of range");
            return 0;
        }
        return zm->stack[f->fp + (n - 1)];
    }
    return rd16(zm, zm->globals_addr + 2u * (uint32_t)(n - 16));
}

/* Write a variable as a result: variable 0 pushes the stack. */
static void write_var(Zm *zm, int n, uint16_t v)
{
    if (n == 0) {
        push(zm, v);
        return;
    }
    if (n < 16) {
        ZmFrame *f;
        if (zm->frame_count == 0) {
            fatal(zm, "local variable with no frame");
            return;
        }
        f = &zm->frames[zm->frame_count - 1];
        if (n > f->nlocals) {
            fatal(zm, "local variable out of range");
            return;
        }
        zm->stack[f->fp + (n - 1)] = v;
        return;
    }
    wr16(zm, zm->globals_addr + 2u * (uint32_t)(n - 16), v);
}

/* Indirect access (load/store/inc/dec/pull): variable 0 means the
 * top of the stack, peeked/patched in place, never popped/pushed. */
static uint16_t read_ref(Zm *zm, int n)
{
    if (n == 0) {
        if (zm->sp <= 0) {
            fatal(zm, "stack underflow (indirect)");
            return 0;
        }
        return zm->stack[zm->sp - 1];
    }
    return read_var(zm, n);
}

static void write_ref(Zm *zm, int n, uint16_t v)
{
    if (n == 0) {
        if (zm->sp <= 0) {
            fatal(zm, "stack underflow (indirect)");
            return;
        }
        zm->stack[zm->sp - 1] = v;
        return;
    }
    write_var(zm, n, v);
}

/* ---------------------------------------------------------------- */
/* randomness (core-owned LCG; the HAL supplies entropy)            */
/* ---------------------------------------------------------------- */

static uint32_t rng_next(Zm *zm)
{
    zm->rng_state = zm->rng_state * 1664525u + 1013904223u;
    return zm->rng_state >> 8;
}

/* ---------------------------------------------------------------- */
/* output routing: streams, windows, ZSCII folding                  */
/* ---------------------------------------------------------------- */

/* ZSCII 155..223 folded to plain ASCII (accented letters lose
 * their accents; the C64 font on the VB has nothing better). */
static const char zscii_fold[69] = {
    'a','o','u','A','O','U','s','"','"','e','i','y','E','I',
    'a','e','i','o','u','y','A','E','I','O','U','Y',
    'a','e','i','o','u','A','E','I','O','U',
    'a','e','i','o','u','A','E','I','O','U',
    'a','A','o','O','a','n','o','A','N','O','a','A','c','C',
    't','d','T','D','L','o','O','!','?'
};

static uint16_t fold_zscii(uint16_t c)
{
    if (c >= 155 && c <= 223)
        return (uint16_t)(uint8_t)zscii_fold[c - 155];
    if (c > 127)
        return '?';
    return c;
}

static void upper_put(Zm *zm, uint16_t c)
{
    if (zm->upper_lines <= 0)
        return;
    if (c == 13) {
        zm->upper_x = 0;
        if (zm->upper_y + 1 < zm->upper_lines)
            zm->upper_y++;
        zm->upper_dirty = 1;
        return;
    }
    if (zm->upper_y >= zm->upper_lines)
        return;
    if (zm->upper_x >= ZM_UPPER_COLS) {
        zm->upper_x = 0;
        zm->upper_y++;
        if (zm->upper_y >= zm->upper_lines)
            return;
    }
    zm->upper[zm->upper_y][zm->upper_x++] = fold_zscii(c);
    zm->upper_dirty = 1;
}

static void upper_clear(Zm *zm)
{
    int x, y;
    for (y = 0; y < ZM_UPPER_LINES; y++)
        for (x = 0; x < ZM_UPPER_COLS; x++)
            zm->upper[y][x] = ' ';
    zm->upper_x = 0;
    zm->upper_y = 0;
    zm->upper_dirty = 1;
}

static void flush_out(Zm *zm)
{
    if (zm->upper_dirty && zm->io->draw_upper) {
        zm->io->draw_upper(zm, &zm->upper[0][0], ZM_UPPER_COLS,
                           zm->upper_lines > 0 ? zm->upper_lines : 1);
        zm->upper_dirty = 0;
    }
    if (zm->io->flush)
        zm->io->flush(zm);
}

/* Emit one ZSCII character through the stream/window routing. */
static uint32_t out_count;
static void out_char(Zm *zm, uint16_t c)
{
    if (zm->stream3_depth > 0) {
        uint16_t table = zm->stream3_table[zm->stream3_depth - 1];
        uint16_t n = rd16(zm, table);
        wr8(zm, (uint32_t)table + 2 + n, (uint8_t)c);
        wr16(zm, table, (uint16_t)(n + 1));
        return;
    }
    if (!zm->stream1_on)
        return;
    if (zm->cur_window == 1) {
        upper_put(zm, c);
        return;
    }
    if (zm->io->put_char)
        zm->io->put_char(zm, fold_zscii(c));
    if (!zm->buffer_mode) {
        flush_out(zm);
    } else if ((++out_count & 0x3FF) == 0) {
        flush_out(zm);   /* pacing point for long print bursts */
    }
}

static void out_str(Zm *zm, const char *s)
{
    while (*s)
        out_char(zm, (uint16_t)(uint8_t)*s++);
}

static void out_num(Zm *zm, int16_t v)
{
    char buf[8];
    int i = 0;
    uint16_t u;
    if (v < 0) {
        out_char(zm, '-');
        u = (uint16_t)(-(int32_t)v);
    } else {
        u = (uint16_t)v;
    }
    if (u == 0) {
        out_char(zm, '0');
        return;
    }
    while (u) {
        buf[i++] = (char)('0' + (u % 10));
        u /= 10;
    }
    while (i)
        out_char(zm, (uint16_t)(uint8_t)buf[--i]);
}


/* ---------------------------------------------------------------- */
/* packed addresses                                                 */
/* ---------------------------------------------------------------- */

static uint32_t unpack_addr(Zm *zm, uint16_t packed)
{
    switch (zm->version) {
    case 3:  return (uint32_t)packed * 2;
    case 8:  return (uint32_t)packed * 8;
    default: return (uint32_t)packed * 4;   /* versions 4 and 5 */
    }
}

/* ---------------------------------------------------------------- */
/* Z-string decoding                                                */
/* ---------------------------------------------------------------- */

static const char alpha0[] = "abcdefghijklmnopqrstuvwxyz";
static const char alpha1[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZ";
/* A2 slots 2..25 (slot 0 = 10-bit escape, slot 1 = newline). */
static const char alpha2[] = "0123456789.,!?_#'\"/\\-:()";

typedef struct {
    Zm *zm;
    uint32_t addr;    /* address of the next word to load */
    uint16_t word;    /* current word */
    int idx;          /* next z-char index in word: 0,1,2 */
    int last;         /* current word is the final word */
    int finished;     /* all z-chars consumed */
} ZStr;

/* Fetch the next raw z-char (0..31), or -1 at the string end. */
static int zstr_next(ZStr *zs)
{
    int shift, v;
    if (zs->finished)
        return -1;
    if (zs->idx == 0) {
        zs->word = rd16(zs->zm, zs->addr);
        zs->addr += 2;
        zs->last = (zs->word & 0x8000) != 0;
    }
    shift = 10 - 5 * zs->idx;
    v = (zs->word >> shift) & 31;
    zs->idx++;
    if (zs->idx == 3) {
        zs->idx = 0;
        if (zs->last)
            zs->finished = 1;
    }
    return v;
}

/* Decode and emit the Z-string at byte address `addr`, up to
 * recursion depth for abbreviations. Returns the address just
 * past the string. */
static uint32_t decode_print(Zm *zm, uint32_t addr, int depth)
{
    ZStr zs;
    int alphabet = 0;   /* 0,1,2 = A0,A1,A2 for the NEXT char */
    zs.zm = zm;
    zs.addr = addr;
    zs.word = 0;
    zs.idx = 0;
    zs.last = 0;
    zs.finished = 0;

    for (;;) {
        int zc = zstr_next(&zs);
        uint16_t out;
        if (zc < 0)
            break;
        if (zc == 0) {
            out_char(zm, ' ');
            alphabet = 0;
            continue;
        }
        if (zc >= 1 && zc <= 3) {
            /* abbreviation: next z-char indexes the table */
            int z2 = zstr_next(&zs);
            uint16_t entry;
            uint32_t saddr;
            if (z2 < 0 || depth > 3 || zm->abbrev_addr == 0)
                break;
            entry = rd16(zm, zm->abbrev_addr + 2u * ((zc - 1) * 32 + z2));
            saddr = (uint32_t)entry * 2;
            decode_print(zm, saddr, depth + 1);
            alphabet = 0;
            continue;
        }
        if (zc == 4) {
            alphabet = 1;
            continue;
        }
        if (zc == 5) {
            alphabet = 2;
            continue;
        }
        /* zc in 6..31 */
        if (alphabet == 2 && zc == 6) {
            /* 10-bit ZSCII escape */
            int hi = zstr_next(&zs);
            int lo = zstr_next(&zs);
            if (hi < 0 || lo < 0)
                break;
            out = (uint16_t)((hi << 5) | lo);
            out_char(zm, out);
            alphabet = 0;
            continue;
        }
        if (zm->alphabet_addr) {
            uint32_t base = zm->alphabet_addr + 26u * (uint32_t)alphabet;
            out = rd8(zm, base + (zc - 6));
        } else if (alphabet == 0) {
            out = (uint16_t)(uint8_t)alpha0[zc - 6];
        } else if (alphabet == 1) {
            out = (uint16_t)(uint8_t)alpha1[zc - 6];
        } else {
            if (zc == 7)
                out = 13;
            else
                out = (uint16_t)(uint8_t)alpha2[zc - 8];
        }
        out_char(zm, out);
        alphabet = 0;
    }
    return zs.addr;
}


/* ---------------------------------------------------------------- */
/* object system                                                    */
/* ---------------------------------------------------------------- */

static int obj_entry_size(const Zm *zm) { return zm->version <= 3 ? 9 : 14; }
static int obj_defaults(const Zm *zm)   { return zm->version <= 3 ? 31 : 63; }

static uint32_t obj_entry(Zm *zm, int obj)
{
    if (obj <= 0) {
        fatal(zm, "object number out of range");
        return 0;
    }
    return (uint32_t)zm->objtab_addr
         + (uint32_t)obj_defaults(zm) * 2
         + (uint32_t)(obj - 1) * (uint32_t)obj_entry_size(zm);
}

static int obj_tree_field(Zm *zm, int obj, int field)
{
    /* field: 0 parent, 1 sibling, 2 child */
    uint32_t e = obj_entry(zm, obj);
    if (zm->version <= 3)
        return rd8(zm, e + 4 + (uint32_t)field);
    return rd16(zm, e + 6 + 2u * (uint32_t)field);
}

static void obj_set_tree_field(Zm *zm, int obj, int field, int v)
{
    uint32_t e = obj_entry(zm, obj);
    if (zm->version <= 3)
        wr8(zm, e + 4 + (uint32_t)field, (uint8_t)v);
    else
        wr16(zm, e + 6 + 2u * (uint32_t)field, (uint16_t)v);
}

static int obj_parent(Zm *zm, int o)  { return obj_tree_field(zm, o, 0); }
static int obj_sibling(Zm *zm, int o) { return obj_tree_field(zm, o, 1); }
static int obj_child(Zm *zm, int o)   { return obj_tree_field(zm, o, 2); }

static uint32_t obj_props_addr(Zm *zm, int obj)
{
    uint32_t e = obj_entry(zm, obj);
    return rd16(zm, e + (zm->version <= 3 ? 7u : 12u));
}

static int test_attr(Zm *zm, int obj, int attr)
{
    uint32_t e = obj_entry(zm, obj);
    return (rd8(zm, e + (uint32_t)(attr / 8)) >> (7 - (attr % 8))) & 1;
}

static void set_attr(Zm *zm, int obj, int attr, int on)
{
    uint32_t e = obj_entry(zm, obj) + (uint32_t)(attr / 8);
    uint8_t b = rd8(zm, e);
    uint8_t mask = (uint8_t)(1u << (7 - (attr % 8)));
    wr8(zm, e, on ? (uint8_t)(b | mask) : (uint8_t)(b & (uint8_t)~mask));
}

/* Detach obj from its parent (parent/sibling surgery). */
static void obj_detach(Zm *zm, int obj)
{
    int p = obj_parent(zm, obj);
    int s;
    if (p == 0)
        return;
    if (obj_child(zm, p) == obj) {
        obj_set_tree_field(zm, p, 2, obj_sibling(zm, obj));
    } else {
        s = obj_child(zm, p);
        while (s && obj_sibling(zm, s) != obj)
            s = obj_sibling(zm, s);
        if (s)
            obj_set_tree_field(zm, s, 1, obj_sibling(zm, obj));
    }
    obj_set_tree_field(zm, obj, 0, 0);
    obj_set_tree_field(zm, obj, 1, 0);
}

static void obj_insert(Zm *zm, int obj, int dest)
{
    obj_detach(zm, obj);
    obj_set_tree_field(zm, obj, 0, dest);
    obj_set_tree_field(zm, obj, 1, obj_child(zm, dest));
    obj_set_tree_field(zm, dest, 2, obj);
}

/* One property-list entry, decoded. */
typedef struct {
    int num;          /* property number, 0 = end of list */
    uint32_t data;    /* address of the property data */
    int size;         /* data length in bytes */
    uint32_t next;    /* address of the next entry header */
} PropEnt;

static PropEnt prop_decode(Zm *zm, uint32_t at)
{
    PropEnt pe;
    uint8_t b = rd8(zm, at);
    pe.num = 0;
    pe.data = at;
    pe.size = 0;
    pe.next = at + 1;
    if (b == 0)
        return pe;
    if (zm->version <= 3) {
        pe.num = b & 0x1F;
        pe.size = (b >> 5) + 1;
        pe.data = at + 1;
        pe.next = at + 1 + (uint32_t)pe.size;
    } else {
        pe.num = b & 0x3F;
        if (b & 0x80) {
            pe.size = rd8(zm, at + 1) & 0x3F;
            if (pe.size == 0)
                pe.size = 64;
            pe.data = at + 2;
        } else {
            pe.size = (b & 0x40) ? 2 : 1;
            pe.data = at + 1;
        }
        pe.next = pe.data + (uint32_t)pe.size;
    }
    return pe;
}

/* Address of an object's first property entry (after the name). */
static uint32_t prop_first(Zm *zm, int obj)
{
    uint32_t pa = obj_props_addr(zm, obj);
    int name_words = rd8(zm, pa);
    return pa + 1 + 2u * (uint32_t)name_words;
}

static int find_prop(Zm *zm, int obj, int prop, PropEnt *out)
{
    uint32_t at = prop_first(zm, obj);
    for (;;) {
        PropEnt pe = prop_decode(zm, at);
        if (pe.num == 0)
            return 0;
        if (pe.num == prop) {
            if (out)
                *out = pe;
            return 1;
        }
        if (pe.num < prop)
            return 0;   /* properties are stored in descending order */
        at = pe.next;
    }
}

static uint16_t get_prop(Zm *zm, int obj, int prop)
{
    PropEnt pe;
    if (!find_prop(zm, obj, prop, &pe)) {
        /* fall back to the defaults table */
        return rd16(zm, (uint32_t)zm->objtab_addr + 2u * (uint32_t)(prop - 1));
    }
    if (pe.size == 1)
        return rd8(zm, pe.data);
    return rd16(zm, pe.data);
}

static uint32_t get_prop_addr(Zm *zm, int obj, int prop)
{
    PropEnt pe;
    if (!find_prop(zm, obj, prop, &pe))
        return 0;
    return pe.data;
}

static int get_next_prop(Zm *zm, int obj, int prop)
{
    PropEnt pe;
    if (prop == 0) {
        pe = prop_decode(zm, prop_first(zm, obj));
        return pe.num;
    }
    if (!find_prop(zm, obj, prop, &pe))
        return 0;
    pe = prop_decode(zm, pe.next);
    return pe.num;
}

static int get_prop_len(Zm *zm, uint32_t data_addr)
{
    uint8_t b;
    if (data_addr == 0)
        return 0;
    if (zm->version <= 3) {
        b = rd8(zm, data_addr - 1);
        return (b >> 5) + 1;
    }
    b = rd8(zm, data_addr - 1);
    if ((b & 0x80) == 0 && data_addr >= 2 && (rd8(zm, data_addr - 2) & 0x80)) {
        int s = b & 0x3F;
        return s ? s : 64;
    }
    return (b & 0x40) ? 2 : 1;
}

static void put_prop(Zm *zm, int obj, int prop, uint16_t value)
{
    PropEnt pe;
    if (!find_prop(zm, obj, prop, &pe)) {
        fatal(zm, "put_prop on absent property");
        return;
    }
    if (pe.size == 1)
        wr8(zm, pe.data, (uint8_t)(value & 0xFF));
    else if (pe.size == 2)
        wr16(zm, pe.data, value);
    else
        fatal(zm, "put_prop on long property");
}

static void print_obj_name(Zm *zm, int obj)
{
    uint32_t pa = obj_props_addr(zm, obj);
    if (rd8(zm, pa) == 0)
        return;
    decode_print(zm, pa + 1, 0);
}


/* ---------------------------------------------------------------- */
/* dictionary and tokenising                                        */
/* ---------------------------------------------------------------- */

static int dict_word_bytes(const Zm *zm) { return zm->version <= 3 ? 4 : 6; }
static int dict_max_zchars(const Zm *zm) { return zm->version <= 3 ? 6 : 9; }

/* Encode one word (already lower-cased ASCII) into dictionary
 * form. Returns 1 on success, 0 if a character cannot be encoded
 * (such words simply never match a dictionary entry). */
static int encode_dict_form(Zm *zm, const char *word, int len,
                            uint8_t *out)
{
    int zc[9];
    int n = 0;
    int i, maxzc = dict_max_zchars(zm);
    for (i = 0; i < len && n < maxzc; i++) {
        char c = word[i];
        const char *p;
        int idx = -1, alpha = 0;
        for (p = alpha0; *p; p++)
            if (*p == c) { idx = (int)(p - alpha0); alpha = 0; break; }
        if (idx < 0) {
            for (p = alpha2; *p; p++)
                if (*p == c) { idx = (int)(p - alpha2); alpha = 2; break; }
        }
        if (idx < 0)
            return 0;
        if (alpha == 2) {
            if (n + 2 > maxzc)
                break;
            zc[n++] = 5;
            zc[n++] = 8 + idx;   /* alpha2[] here starts at z-char 8 */
        } else {
            zc[n++] = 6 + idx;
        }
    }
    while (n < maxzc)
        zc[n++] = 5;
    for (i = 0; i < maxzc; i += 3) {
        uint16_t w = (uint16_t)((zc[i] << 10) | (zc[i + 1] << 5) | zc[i + 2]);
        if (i + 3 >= maxzc)
            w = (uint16_t)(w | 0x8000);   /* Z-string end bit: a dictionary
                                           * word IS a Z-string, so its final
                                           * word carries the terminator bit
                                           * (top bit set). Without it the
                                           * encoded lookup form never matches
                                           * a real dictionary entry (§77). */
        out[i / 3 * 2] = (uint8_t)(w >> 8);
        out[i / 3 * 2 + 1] = (uint8_t)(w & 0xFF);
    }
    return 1;
}

static uint32_t dict_lookup(Zm *zm, uint32_t dict, const uint8_t *encoded)
{
    int nsep = rd8(zm, dict);
    int entry_len = rd8(zm, dict + 1 + (uint32_t)nsep);
    int count = rd16(zm, dict + 2 + (uint32_t)nsep);
    uint32_t at = dict + 4 + (uint32_t)nsep;
    int wb = dict_word_bytes(zm);
    int i, j;
    if (entry_len < wb)
        return 0;
    for (i = 0; i < count; i++, at += (uint32_t)entry_len) {
        int same = 1;
        for (j = 0; j < wb; j++) {
            if (rd8(zm, at + (uint32_t)j) != encoded[j]) {
                same = 0;
                break;
            }
        }
        if (same)
            return at;
    }
    return 0;
}

static int is_separator(Zm *zm, uint32_t dict, uint8_t c)
{
    int nsep = rd8(zm, dict);
    int i;
    for (i = 0; i < nsep; i++)
        if (rd8(zm, dict + 1 + (uint32_t)i) == c)
            return 1;
    return 0;
}

/* The tokenise core, shared by @read and the tokenise opcode.
 * text/parse are story addresses; dict 0 = the default dictionary;
 * flag != 0 preserves parse entries that already hold an address. */
static void tokenise_line(Zm *zm, uint16_t text, uint16_t parse,
                          uint16_t dict, int flag)
{
    int max_words, tlen, i, words = 0;
    if (dict == 0)
        dict = zm->dict_addr;
    max_words = rd8(zm, parse);
    tlen = rd8(zm, text + 1);
    i = 0;
    while (i < tlen && words < max_words) {
        uint8_t c = rd8(zm, (uint32_t)text + 2 + (uint32_t)i);
        int start, wlen, k;
        uint32_t entry_addr;
        char wbuf[64];
        uint8_t enc[6];
        int enc_ok;
        if (c == ' ') {
            i++;
            continue;
        }
        start = i;
        if (is_separator(zm, dict, c)) {
            wlen = 1;
            i++;
        } else {
            while (i < tlen) {
                c = rd8(zm, (uint32_t)text + 2 + (uint32_t)i);
                if (c == ' ' || is_separator(zm, dict, c))
                    break;
                i++;
            }
            wlen = i - start;
        }
        entry_addr = (uint32_t)parse + 2 + 4u * (uint32_t)words;
        if (!(flag && rd16(zm, entry_addr) != 0)) {
            for (k = 0; k < wlen && k < 63; k++) {
                char ch = (char)rd8(zm, (uint32_t)text + 2 + (uint32_t)(start + k));
                if (ch >= 'A' && ch <= 'Z')
                    ch = (char)(ch + ('a' - 'A'));
                wbuf[k] = ch;
            }
            enc_ok = encode_dict_form(zm, wbuf, wlen < 63 ? wlen : 63, enc);
            wr16(zm, entry_addr,
                 enc_ok ? (uint16_t)dict_lookup(zm, dict, enc) : 0);
        }
        wr8(zm, entry_addr + 2, (uint8_t)wlen);
        wr8(zm, entry_addr + 3, (uint8_t)(start + 2));
        words++;
    }
    wr8(zm, parse + 1, (uint8_t)words);
}

/* The encode_text opcode: ZSCII text -> dictionary form. */
static void encode_text_op(Zm *zm, uint16_t zscii_addr, int len,
                           int from, uint16_t coded_addr)
{
    char buf[64];
    uint8_t enc[6];
    int i, n = len < 63 ? len : 63;
    for (i = 0; i < n; i++) {
        char ch = (char)rd8(zm, (uint32_t)zscii_addr + (uint32_t)(from + i));
        if (ch >= 'A' && ch <= 'Z')
            ch = (char)(ch + ('a' - 'A'));
        buf[i] = ch;
    }
    if (!encode_dict_form(zm, buf, n, enc)) {
        /* unencodable characters become padding-only words */
        for (i = 0; i < 6; i++)
            enc[i] = 0x14;   /* z-chars 5,5,5 per word */
        /* rebuild as all-pad words */
        {
            int wb = dict_word_bytes(zm);
            for (i = 0; i < wb; i += 2) {
                enc[i] = 0x14;
                enc[i + 1] = 0xA5;
            }
        }
    }
    {
        int wb = dict_word_bytes(zm);
        for (i = 0; i < wb; i++)
            wr8(zm, (uint32_t)coded_addr + (uint32_t)i, enc[i]);
    }
}


/* ---------------------------------------------------------------- */
/* Quetzal saves (IFZS): IFhd + CMem + Stks                         */
/* ---------------------------------------------------------------- */

static uint32_t save_pc;   /* PC to resume at after a restore */

static int q_putc(Zm *zm, int b)
{
    return zm->io->save_byte ? zm->io->save_byte(zm, b & 0xFF) : -1;
}

static int q_puts(Zm *zm, const char *s, int n)
{
    int i;
    for (i = 0; i < n; i++)
        if (q_putc(zm, (uint8_t)s[i]))
            return -1;
    return 0;
}

static int q_put16(Zm *zm, uint16_t v)
{
    if (q_putc(zm, v >> 8)) return -1;
    if (q_putc(zm, v & 0xFF)) return -1;
    return 0;
}

static int q_put32(Zm *zm, uint32_t v)
{
    if (q_put16(zm, (uint16_t)(v >> 16))) return -1;
    if (q_put16(zm, (uint16_t)(v & 0xFFFF))) return -1;
    return 0;
}

/* CMem: dynamic memory XOR the pristine story, RLE on zero runs.
 * With emit == 0 this only counts the encoded size. */
static int cmem_emit(Zm *zm, int emit, uint32_t *size_out)
{
    uint32_t i, count = 0;
    int run = 0;
    for (i = 0; i < zm->dyn_size; i++) {
        uint8_t d = (uint8_t)(zm->dyn[i] ^ pr8(zm, i));
        if (d == 0) {
            run++;
            if (run == 256) {
                if (emit) {
                    if (q_putc(zm, 0) || q_putc(zm, 255)) return -1;
                }
                count += 2;
                run = 0;
            }
        } else {
            if (run) {
                if (emit) {
                    if (q_putc(zm, 0) || q_putc(zm, run - 1)) return -1;
                }
                count += 2;
                run = 0;
            }
            if (emit && q_putc(zm, d)) return -1;
            count++;
        }
    }
    if (run) {
        if (emit) {
            if (q_putc(zm, 0) || q_putc(zm, run - 1)) return -1;
        }
        count += 2;
    }
    *size_out = count;
    return 0;
}

/* Stks size for the current frame/stack state. */
static uint32_t stks_size(Zm *zm)
{
    uint32_t total = 0;
    int i;
    /* dummy frame: pc(3) + flags(1) + eval count(2) + eval words */
    total += 6 + 2u * (zm->frame_count ? zm->frames[0].fp : (uint16_t)zm->sp);
    for (i = 0; i < zm->frame_count; i++) {
        ZmFrame *f = &zm->frames[i];
        int top = (i + 1 < zm->frame_count) ? zm->frames[i + 1].fp : zm->sp;
        int eval = top - (f->fp + f->nlocals);
        total += 3 + 1 + (f->ret_store >= 0 ? 1u : 0u)
               + 2u * (uint32_t)f->nlocals + 2 + 2u * (uint32_t)eval;
    }
    return total;
}

static int stks_write(Zm *zm)
{
    int i;
    /* dummy frame */
    if (q_putc(zm, 0) || q_putc(zm, 0) || q_putc(zm, 0)) return -1;
    if (q_putc(zm, 0x10)) return -1;   /* flags: no store var, 0 locals */
    {
        int eval = zm->frame_count ? zm->frames[0].fp : zm->sp;
        int j;
        if (q_put16(zm, (uint16_t)eval)) return -1;
        for (j = 0; j < eval; j++)
            if (q_put16(zm, zm->stack[j])) return -1;
    }
    for (i = 0; i < zm->frame_count; i++) {
        ZmFrame *f = &zm->frames[i];
        int top = (i + 1 < zm->frame_count) ? zm->frames[i + 1].fp : zm->sp;
        int eval = top - (f->fp + f->nlocals);
        int j;
        uint8_t flags = (uint8_t)(f->nlocals | ((f->nargs & 7) << 5));
        if (q_putc(zm, (int)(f->ret_pc >> 16))) return -1;
        if (q_putc(zm, (int)(f->ret_pc >> 8))) return -1;
        if (q_putc(zm, (int)f->ret_pc)) return -1;
        if (f->ret_store < 0)
            flags |= 0x10;
        if (q_putc(zm, flags)) return -1;
        if (f->ret_store >= 0 && q_putc(zm, f->ret_store)) return -1;
        for (j = 0; j < f->nlocals; j++)
            if (q_put16(zm, zm->stack[f->fp + j])) return -1;
        if (q_put16(zm, (uint16_t)eval)) return -1;
        for (j = 0; j < eval; j++)
            if (q_put16(zm, zm->stack[f->fp + f->nlocals + j])) return -1;
    }
    return 0;
}

static int quetzal_save(Zm *zm)
{
    uint32_t cmem_size = 0, stks_sz, form_size;
    int i;
    if (!zm->io->save_begin || zm->io->save_begin(zm))
        return 0;
    if (cmem_emit(zm, 0, &cmem_size))
        goto fail;
    stks_sz = stks_size(zm);
    form_size = 4
        + 8 + 13 + (13 & 1)
        + 8 + cmem_size + (cmem_size & 1)
        + 8 + stks_sz + (stks_sz & 1);
    if (q_puts(zm, "FORM", 4) || q_put32(zm, form_size) ||
        q_puts(zm, "IFZS", 4))
        goto fail;
    if (q_puts(zm, "IFhd", 4) || q_put32(zm, 13))
        goto fail;
    if (q_put16(zm, pr16(zm, 2))) goto fail;                 /* release */
    for (i = 0; i < 6; i++)
        if (q_putc(zm, pr8(zm, 0x12 + (uint32_t)i))) goto fail; /* serial */
    if (q_put16(zm, pr16(zm, 0x1C))) goto fail;              /* checksum */
    if (q_putc(zm, (int)(save_pc >> 16)) ||
        q_putc(zm, (int)(save_pc >> 8)) ||
        q_putc(zm, (int)save_pc))
        goto fail;
    if (q_putc(zm, 0)) goto fail;                            /* pad */
    if (q_puts(zm, "CMem", 4) || q_put32(zm, cmem_size))
        goto fail;
    {
        uint32_t dummy;
        if (cmem_emit(zm, 1, &dummy))
            goto fail;
    }
    if ((cmem_size & 1) && q_putc(zm, 0)) goto fail;
    if (q_puts(zm, "Stks", 4) || q_put32(zm, stks_sz))
        goto fail;
    if (stks_write(zm))
        goto fail;
    if ((stks_sz & 1) && q_putc(zm, 0)) goto fail;
    if (zm->io->save_end && zm->io->save_end(zm))
        return 0;
    return 1;
fail:
    if (zm->io->save_end)
        zm->io->save_end(zm);
    return 0;
}

static int q_getc(Zm *zm)
{
    return zm->io->restore_byte ? zm->io->restore_byte(zm) : -1;
}

static int q_get16(Zm *zm, uint16_t *out)
{
    int a = q_getc(zm), b = q_getc(zm);
    if (a < 0 || b < 0)
        return -1;
    *out = (uint16_t)((a << 8) | b);
    return 0;
}

static int q_get32(Zm *zm, uint32_t *out)
{
    uint16_t a, b;
    if (q_get16(zm, &a) || q_get16(zm, &b))
        return -1;
    *out = ((uint32_t)a << 16) | b;
    return 0;
}

static int q_skip(Zm *zm, uint32_t n)
{
    while (n--)
        if (q_getc(zm) < 0)
            return -1;
    return 0;
}

/* Parse Stks into the live stack/frames. Returns 0 on success. */
static int stks_read(Zm *zm, uint32_t size)
{
    uint32_t consumed = 0;
    int sp = 0;
    int fc = 0;
    int first = 1;
    while (consumed < size) {
        int b0 = q_getc(zm), b1 = q_getc(zm), b2 = q_getc(zm);
        int flags, j;
        uint32_t ret_pc;
        uint16_t eval_count;
        if (b0 < 0 || b1 < 0 || b2 < 0)
            return -1;
        ret_pc = ((uint32_t)b0 << 16) | ((uint32_t)b1 << 8) | (uint32_t)b2;
        flags = q_getc(zm);
        if (flags < 0)
            return -1;
        consumed += 4;
        if (first) {
            /* dummy frame: no store var, no locals; its eval words
             * form the bottom of the stack */
            if (q_get16(zm, &eval_count))
                return -1;
            consumed += 2;
            for (j = 0; j < eval_count; j++) {
                uint16_t w;
                if (q_get16(zm, &w))
                    return -1;
                if (sp >= ZM_STACK_WORDS)
                    return -1;
                zm->stack[sp++] = w;
                consumed += 2;
            }
            first = 0;
            continue;
        }
        {
            int nlocals = flags & 0x0F;
            int discard = (flags & 0x10) != 0;
            int nargs = (flags >> 5) & 7;
            int store = -1;
            ZmFrame *f;
            if (fc >= ZM_MAX_FRAMES)
                return -1;
            if (!discard) {
                store = q_getc(zm);
                if (store < 0)
                    return -1;
                consumed++;
            }
            f = &zm->frames[fc];
            f->ret_pc = ret_pc;
            f->ret_store = (int16_t)store;
            f->fp = (uint16_t)sp;
            f->nlocals = (uint8_t)nlocals;
            f->nargs = (uint8_t)nargs;
            for (j = 0; j < nlocals; j++) {
                uint16_t w;
                if (q_get16(zm, &w))
                    return -1;
                if (sp >= ZM_STACK_WORDS)
                    return -1;
                zm->stack[sp++] = w;
                consumed += 2;
            }
            if (q_get16(zm, &eval_count))
                return -1;
            consumed += 2;
            for (j = 0; j < eval_count; j++) {
                uint16_t w;
                if (q_get16(zm, &w))
                    return -1;
                if (sp >= ZM_STACK_WORDS)
                    return -1;
                zm->stack[sp++] = w;
                consumed += 2;
            }
            fc++;
        }
    }
    zm->sp = sp;
    zm->frame_count = fc;
    return 0;
}

static int quetzal_restore(Zm *zm)
{
    char id[4];
    uint32_t form_size, remaining;
    int i, have_ifhd = 0, have_mem = 0, have_stks = 0;
    uint32_t new_pc = 0;
    if (!zm->io->restore_begin || zm->io->restore_begin(zm))
        return 0;
    for (i = 0; i < 4; i++) {
        int c = q_getc(zm);
        if (c < 0) goto out;
        id[i] = (char)c;
    }
    if (id[0] != 'F' || id[1] != 'O' || id[2] != 'R' || id[3] != 'M')
        goto out;
    if (q_get32(zm, &form_size))
        goto out;
    for (i = 0; i < 4; i++) {
        int c = q_getc(zm);
        if (c < 0) goto out;
        id[i] = (char)c;
    }
    if (id[0] != 'I' || id[1] != 'F' || id[2] != 'Z' || id[3] != 'S')
        goto out;
    remaining = form_size - 4;
    while (remaining >= 8) {
        char cid[4];
        uint32_t csize;
        for (i = 0; i < 4; i++) {
            int c = q_getc(zm);
            if (c < 0) goto out;
            cid[i] = (char)c;
        }
        if (q_get32(zm, &csize))
            goto out;
        remaining -= 8;
        if (cid[0] == 'I' && cid[1] == 'F' && cid[2] == 'h' && cid[3] == 'd') {
            uint16_t rel, chk;
            uint8_t ser[6];
            int b0, b1, b2;
            if (csize < 13 || q_get16(zm, &rel))
                goto out;
            for (i = 0; i < 6; i++) {
                int c = q_getc(zm);
                if (c < 0) goto out;
                ser[i] = (uint8_t)c;
            }
            if (q_get16(zm, &chk))
                goto out;
            b0 = q_getc(zm); b1 = q_getc(zm); b2 = q_getc(zm);
            if (b0 < 0 || b1 < 0 || b2 < 0)
                goto out;
            new_pc = ((uint32_t)b0 << 16) | ((uint32_t)b1 << 8) | (uint32_t)b2;
            if (q_skip(zm, csize - 13))
                goto out;
            if (rel != pr16(zm, 2) || chk != pr16(zm, 0x1C))
                goto out;
            for (i = 0; i < 6; i++)
                if (ser[i] != pr8(zm, 0x12 + (uint32_t)i))
                    goto out;
            have_ifhd = 1;
        } else if (cid[0] == 'C' && cid[1] == 'M' && cid[2] == 'e' && cid[3] == 'm') {
            uint32_t pos = 0;
            int owed = 0;   /* zero-diff bytes still owed by a run */
            while (pos < zm->dyn_size) {
                int b;
                if (owed > 0) {
                    zm->dyn[pos] = pr8(zm, pos);
                    pos++;
                    owed--;
                    continue;
                }
                b = q_getc(zm);
                if (b < 0) {
                    /* truncated stream: the rest is pristine */
                    while (pos < zm->dyn_size) {
                        zm->dyn[pos] = pr8(zm, pos);
                        pos++;
                    }
                    break;
                }
                if (b == 0) {
                    int n = q_getc(zm);
                    if (n < 0) goto out;
                    /* run of n+1 zero-diff bytes in total */
                    zm->dyn[pos] = pr8(zm, pos);
                    pos++;
                    owed = n;
                } else {
                    zm->dyn[pos] = (uint8_t)(pr8(zm, pos) ^ b);
                    pos++;
                }
            }
            have_mem = 1;
        } else if (cid[0] == 'U' && cid[1] == 'M' && cid[2] == 'e' && cid[3] == 'm') {
            uint32_t n = csize < zm->dyn_size ? csize : zm->dyn_size;
            uint32_t k;
            for (k = 0; k < n; k++) {
                int c = q_getc(zm);
                if (c < 0) goto out;
                zm->dyn[k] = (uint8_t)c;
            }
            if (q_skip(zm, csize - n))
                goto out;
            have_mem = 1;
        } else if (cid[0] == 'S' && cid[1] == 't' && cid[2] == 'k' && cid[3] == 's') {
            if (stks_read(zm, csize))
                goto out;
            have_stks = 1;
        } else {
            if (q_skip(zm, csize))
                goto out;
        }
        remaining -= csize;
        if (csize & 1) {
            if (q_getc(zm) < 0)
                goto out;
            remaining -= 1;
        }
    }
    if (have_ifhd && have_mem && have_stks) {
        zm->pc = new_pc;
        /* interpreter header bytes must be re-applied over the
         * restored dynamic memory (the save holds the game's copy;
         * ours differ only in capability fields, re-assert them) */
        zm->dyn[0x1E] = 6;
        zm->dyn[0x1F] = 'V';
        if (zm->io->restore_end)
            zm->io->restore_end(zm);
        return 1;
    }
out:
    if (zm->io->restore_end)
        zm->io->restore_end(zm);
    return 0;
}

/* Complete the resumed @save after a successful Quetzal
 * restore. The standard convention: the saved PC points just
 * past the @save instruction, so the byte before it is that
 * instruction's store byte, and the resumed @save now stores
 * 2 ("restored") in the restored context -- the value the
 * game's library actually checks. (Writing 2 into the
 * @restore instruction's own store variable instead, the old
 * behaviour, both missed that check and scribbled on an
 * unrelated variable of the restored state.)
 *
 * The same completion serves restore_undo: undo_restore
 * resumes at the PC recorded just past the @save_undo
 * instruction, so the byte before the resumed PC is that
 * instruction's store byte, and the resumed @save_undo must
 * store its 2 there for exactly the same reason. */
static void complete_restored_save(Zm *zm)
{
    write_var(zm, pr8(zm, zm->pc - 1), 2);
}

/* ---------------------------------------------------------------- */
/* undo (single level, caller-supplied snapshot RAM)                */
/* ---------------------------------------------------------------- */

static int undo_snapshot(Zm *zm, uint32_t pc_after)
{
    int i;
    if (!zm->undo_dyn || !zm->undo_stack || !zm->undo_frames)
        return 0;
    mem_copy(zm->undo_dyn, zm->dyn, zm->dyn_size);
    for (i = 0; i < zm->sp; i++)
        zm->undo_stack[i] = zm->stack[i];
    for (i = 0; i < zm->frame_count; i++)
        zm->undo_frames[i] = zm->frames[i];
    zm->undo_sp = zm->sp;
    zm->undo_frame_count = zm->frame_count;
    zm->undo_pc = pc_after;
    zm->undo_dyn_size = zm->dyn_size;
    zm->undo_valid = 1;
    return 1;
}

static int undo_restore(Zm *zm)
{
    int i;
    if (!zm->undo_valid)
        return 0;
    mem_copy(zm->dyn, zm->undo_dyn, zm->undo_dyn_size);
    for (i = 0; i < zm->undo_sp; i++)
        zm->stack[i] = zm->undo_stack[i];
    for (i = 0; i < zm->undo_frame_count; i++)
        zm->frames[i] = zm->undo_frames[i];
    zm->sp = zm->undo_sp;
    zm->frame_count = zm->undo_frame_count;
    zm->pc = zm->undo_pc;
    return 1;
}


/* ---------------------------------------------------------------- */
/* call / return machinery                                          */
/* ---------------------------------------------------------------- */

static void do_return(Zm *zm, uint16_t value)
{
    ZmFrame *f;
    if (zm->frame_count == 0) {
        zm->halted = 1;   /* returned from the root: game over */
        return;
    }
    f = &zm->frames[--zm->frame_count];
    zm->sp = f->fp;
    if (f->ret_store == -2) {
        zm->halted = 1;   /* the initial routine returned */
        return;
    }
    zm->pc = f->ret_pc;
    if (f->ret_store >= 0)
        write_var(zm, f->ret_store, value);
}

static void do_call(Zm *zm, uint16_t packed, const uint16_t *args,
                    int nargs, int store_var, uint32_t ret_pc)
{
    uint32_t addr;
    int nlocals, i;
    ZmFrame *f;
    if (packed == 0) {
        zm->pc = ret_pc;
        if (store_var >= 0)
            write_var(zm, store_var, 0);
        return;
    }
    addr = unpack_addr(zm, packed);
    nlocals = rd8(zm, addr);
    if (zm->frame_count >= ZM_MAX_FRAMES) {
        fatal(zm, "call nesting too deep");
        return;
    }
    if (zm->sp + nlocals > ZM_STACK_WORDS) {
        fatal(zm, "stack overflow in call");
        return;
    }
    f = &zm->frames[zm->frame_count];
    f->ret_pc = ret_pc;
    f->ret_store = (int16_t)store_var;
    f->fp = (uint16_t)zm->sp;
    f->nlocals = (uint8_t)nlocals;
    f->nargs = (uint8_t)(nargs > 15 ? 15 : nargs);
    zm->frame_count++;
    for (i = 0; i < nlocals; i++) {
        uint16_t v = 0;
        if (zm->version <= 3)
            v = rd16(zm, addr + 1 + 2u * (uint32_t)i);
        if (i < nargs)
            v = args[i];
        zm->stack[zm->sp++] = v;
    }
    zm->pc = addr + 1 + (zm->version <= 3 ? 2u * (uint32_t)nlocals : 0u);
}

static void do_throw(Zm *zm, uint16_t value, uint16_t depth)
{
    if (depth == 0 || depth > zm->frame_count) {
        fatal(zm, "throw to a bad frame");
        return;
    }
    zm->frame_count = (int)depth;
    do_return(zm, value);
}

static void step(Zm *zm);   /* below */

/* Call an interrupt routine (timed input) and run it to
 * completion. Returns its return value. */
static int call_and_wait(Zm *zm, uint16_t packed)
{
    int depth = zm->frame_count;
    int sp0 = zm->sp;
    do_call(zm, packed, 0, 0, 0, zm->pc);
    while (!zm->halted && zm->frame_count > depth)
        step(zm);
    if (zm->sp > sp0) {
        uint16_t v = zm->stack[sp0];
        zm->sp = sp0;
        return v;
    }
    return 0;
}

/* ---------------------------------------------------------------- */
/* input: @read and @read_char                                      */
/* ---------------------------------------------------------------- */

static void do_read(Zm *zm, const uint16_t *ops, int nops, int store_var)
{
    uint16_t text = ops[0];
    uint16_t parse = nops > 1 ? ops[1] : 0;
    int time = nops > 2 ? ops[2] : 0;
    uint16_t routine = nops > 3 ? ops[3] : 0;
    char buf[256];
    char init[256];
    int max, len = 0, term = 13, init_len = 0, i;

    flush_out(zm);
    max = rd8(zm, text);
    if (max == 0 || max > 255)
        max = 255;
    if (zm->version >= 4) {
        init_len = rd8(zm, text + 1);
        if (init_len > max)
            init_len = max;
        for (i = 0; i < init_len; i++)
            init[i] = (char)rd8(zm, (uint32_t)text + 2 + (uint32_t)i);
    }
    for (;;) {
        int rc = zm->io->get_line
            ? zm->io->get_line(zm, buf, max, &len, init, init_len,
                               time, &term)
            : ZM_LINE_EOF;
        if (rc == ZM_LINE_EOF) {
            zm->halted = -1;
            return;
        }
        if (rc == ZM_LINE_TICK) {
            int r = 0;
            if (routine)
                r = call_and_wait(zm, routine);
            if (zm->halted)
                return;
            if (!r) {
                init_len = len;
                for (i = 0; i < len && i < 255; i++)
                    init[i] = buf[i];
                continue;
            }
            term = 0;   /* interrupt routine ended the read */
        }
        break;
    }
    if (term != 13 && zm->version >= 5 && zm->termkeys_addr) {
        /* a function key may only terminate the read if the story's
         * terminating-keys table lists it (or 255 = all) */
        uint32_t a = zm->termkeys_addr;
        int c, ok = 0;
        while ((c = rd8(zm, a++)) != 0) {
            if (c == 255 || c == term) {
                ok = 1;
                break;
            }
        }
        if (!ok)
            term = 13;
    }
    if (len > max)
        len = max;
    wr8(zm, text + 1, (uint8_t)len);
    for (i = 0; i < len; i++) {
        char ch = buf[i];
        if (ch >= 'A' && ch <= 'Z')
            ch = (char)(ch + ('a' - 'A'));
        wr8(zm, (uint32_t)text + 2 + (uint32_t)i, (uint8_t)ch);
    }
    if (parse)
        tokenise_line(zm, text, parse, 0, 0);
    if (store_var >= 0)
        write_var(zm, store_var, (uint16_t)term);
}

static uint16_t do_read_char(Zm *zm, const uint16_t *ops, int nops)
{
    int time = nops > 1 ? ops[1] : 0;
    uint16_t routine = nops > 2 ? ops[2] : 0;
    flush_out(zm);
    for (;;) {
        int c;
        if (time && zm->io->get_char_timed) {
            c = zm->io->get_char_timed(zm, time);
            if (c == -ZM_LINE_TICK) {
                int r = routine ? call_and_wait(zm, routine) : 0;
                if (zm->halted)
                    return 0;
                if (r)
                    return 0;
                continue;
            }
        } else {
            c = zm->io->get_char ? zm->io->get_char(zm) : ZM_LINE_EOF;
        }
        if (c == ZM_LINE_EOF) {
            zm->halted = -1;
            return 0;
        }
        return (uint16_t)c;
    }
}

/* ---------------------------------------------------------------- */
/* init / restart                                                   */
/* ---------------------------------------------------------------- */

static void apply_header(Zm *zm)
{
    if (zm->version >= 5)
        zm->dyn[0x01] |= 0x08 | 0x40;   /* fixed pitch + timed input */
    else if (zm->version == 4)
        zm->dyn[0x01] |= 0x08;
    zm->dyn[0x1E] = 6;                  /* interpreter: "IBM PC" class */
    zm->dyn[0x1F] = 'V';                /* ...the Virtual one */
    if (zm->version >= 4) {
        /* Header screen geometry, per the standard header table:
         * 0x20 = screen height in lines, 0x21 = screen width in
         * characters, 0x22 = screen width in units (word), 0x24 =
         * screen height in units (word), 0x26 = font height in
         * units, 0x27 = font width in units. Games read these
         * back; leaving 0x20/0x21 at zero makes HHG conclude
         * "[Screen too narrow.]" and quit (§77). */
        zm->dyn[0x20] = 25;             /* screen height, lines */
        zm->dyn[0x21] = 48;             /* screen width, chars */
        wr16(zm, 0x22, 48);             /* screen width, units */
        wr16(zm, 0x24, 25);             /* screen height, units */
        zm->dyn[0x26] = 1;              /* font height in units */
        zm->dyn[0x27] = 1;              /* font width in units */
        zm->dyn[0x2C] = 2;              /* default background: black */
        zm->dyn[0x2D] = 3;              /* default foreground: red */
    }
    zm->dyn[0x11] |= 0x08;              /* undo is available */
}

static void reset_machine(Zm *zm)
{
    zm->sp = 0;
    zm->frame_count = 0;
    zm->halted = 0;
    zm->cur_window = 0;
    zm->upper_lines = 0;
    upper_clear(zm);
    zm->upper_dirty = 0;
    zm->buffer_mode = 1;
    zm->stream1_on = 1;
    zm->stream2_on = 0;
    zm->stream3_depth = 0;
    zm->text_style = 0;
    if (zm->version <= 3) {
        zm->pc = pr16(zm, 6);           /* v3: a plain byte address */
    } else if (zm->version >= 5) {
        /* V5+: the header's initial-PC field is the BYTE ADDRESS
         * of the first instruction — packed routine addressing
         * applies to call operands, not to this field. The main
         * routine's header byte (its locals count) sits at PC-1,
         * and V5 locals start at 0, so seed frame 0 with that
         * many zeroed locals. Infocom's own V5 files use this
         * layout: HHG r31 enters at 0x7101 with the header byte
         * at 0x7100 (§77, probe-verified against frotz). */
        uint32_t entry = pr16(zm, 6);
        uint32_t addr = entry - 1;
        int nlocals = rd8(zm, addr), i;
        ZmFrame *f = &zm->frames[0];
        f->ret_pc = 0; f->ret_store = -2; f->fp = 0;
        f->nlocals = (uint8_t)nlocals; f->nargs = 0;
        zm->frame_count = 1;
        for (i = 0; i < nlocals; i++) zm->stack[zm->sp++] = 0;
        zm->pc = entry;
    } else {
        do_call(zm, pr16(zm, 6), 0, 0, -2, 0);
    }
}

static void set_err(char *errbuf, int len, const char *msg)
{
    int i = 0;
    if (!errbuf || len <= 0)
        return;
    while (msg[i] && i < len - 1) {
        errbuf[i] = msg[i];
        i++;
    }
    errbuf[i] = 0;
}

int zm_init(Zm *zm, const uint8_t *story, uint32_t story_len,
            uint8_t *dyn, uint32_t dyn_cap,
            uint8_t *undo_dyn, uint16_t *undo_stack, ZmFrame *undo_frames,
            const ZmIO *io, char *errbuf, int errbuf_len)
{
    uint32_t raw_len, mult;
    int i;
    for (i = 0; i < (int)sizeof(Zm); i++)
        ((uint8_t *)zm)[i] = 0;
    zm->io = io;
    zm->story = story;
    zm->story_len = story_len;
    zm->dyn = dyn;
    zm->dyn_cap = dyn_cap;
    zm->undo_dyn = undo_dyn;
    zm->undo_stack = undo_stack;
    zm->undo_frames = undo_frames;
    if (story_len < 64) {
        set_err(errbuf, errbuf_len, "story file too small");
        return -1;
    }
    zm->version = story[0];
    if (zm->version != 3 && zm->version != 4 && zm->version != 5 &&
        zm->version != 8) {
        set_err(errbuf, errbuf_len, "unsupported Z-machine version");
        return -1;
    }
    raw_len = pr16(zm, 0x1A);
    mult = zm->version == 3 ? 2u : zm->version <= 5 ? 4u : 8u;
    zm->file_len = raw_len ? raw_len * mult : story_len;
    if (zm->file_len > story_len)
        zm->file_len = story_len;
    zm->dyn_size = pr16(zm, 0x0E);
    if (zm->dyn_size < 64 || zm->dyn_size > story_len) {
        set_err(errbuf, errbuf_len, "bad dynamic memory size in header");
        return -1;
    }
    if (zm->dyn_size > dyn_cap) {
        set_err(errbuf, errbuf_len,
                "story dynamic memory exceeds the RAM budget");
        return -1;
    }
    mem_copy(dyn, story, zm->dyn_size);
    zm->dict_addr = pr16(zm, 8);
    zm->objtab_addr = pr16(zm, 0x0A);
    zm->globals_addr = pr16(zm, 0x0C);
    zm->abbrev_addr = pr16(zm, 0x18);
    zm->termkeys_addr = pr16(zm, 0x2E);
    zm->alphabet_addr = pr16(zm, 0x34);
    zm->exttab_addr = pr16(zm, 0x36);
    zm->rng_state = (io && io->rand32) ? io->rand32(zm) : 0x2B992DDFu;
    if (zm->rng_state == 0)
        zm->rng_state = 1;
    apply_header(zm);
    reset_machine(zm);
    return 0;
}

void zm_restart(Zm *zm)
{
    mem_copy(zm->dyn, zm->story, zm->dyn_size);
    apply_header(zm);
    if (zm->io && zm->io->clear_screen)
        zm->io->clear_screen(zm);
    zm->undo_valid = 0;
    reset_machine(zm);
}


/* ---------------------------------------------------------------- */
/* instruction decode and dispatch                                  */
/* ---------------------------------------------------------------- */

typedef struct {
    uint16_t v[8];
    uint8_t raw[8];
    int n;
} Ops;

/* Fetch operands per the type bytes. When first_ref is set, the
 * first operand is a variable REFERENCE (store/inc/dec/load/pull):
 * its "value" is the variable number itself. */
static void fetch_ops(Zm *zm, Ops *o, const uint8_t *types, int nbytes,
                      int first_ref)
{
    int i, total = 0;
    for (i = 0; i < nbytes * 4 && i < 8; i++) {
        int t = (types[i / 4] >> (6 - 2 * (i % 4))) & 3;
        if (t == 3)
            break;
        total++;
    }
    o->n = total;
    for (i = 0; i < total; i++) {
        int t = (types[i / 4] >> (6 - 2 * (i % 4))) & 3;
        if (t == 0) {
            o->v[i] = rd16(zm, zm->pc);
            zm->pc += 2;
            o->raw[i] = 0;
        } else if (t == 1) {
            o->v[i] = rd8(zm, zm->pc++);
            o->raw[i] = 0;
        } else {
            int vn = rd8(zm, zm->pc++);
            o->raw[i] = (uint8_t)vn;
            o->v[i] = (i == 0 && first_ref) ? (uint16_t)vn
                                            : read_var(zm, vn);
        }
    }
}

static int read_store(Zm *zm)
{
    return rd8(zm, zm->pc++);
}

/* Read branch data and apply it. Returns 1 if a return happened. */
static int do_branch(Zm *zm, int cond)
{
    uint8_t b0 = rd8(zm, zm->pc++);
    int on_true = (b0 & 0x80) != 0;
    int32_t off;
    if (b0 & 0x40) {
        off = b0 & 0x3F;
    } else {
        uint8_t b1 = rd8(zm, zm->pc++);
        off = (int32_t)(((b0 & 0x3F) << 8) | b1);
        if (off & 0x2000)
            off -= 0x4000;
    }
    if ((cond != 0) == on_true) {
        if (off == 0) {
            do_return(zm, 0);
            return 1;
        }
        if (off == 1) {
            do_return(zm, 1);
            return 1;
        }
        zm->pc = (uint32_t)((int32_t)zm->pc + off - 2);
    }
    return 0;
}

static int verify_checksum(Zm *zm)
{
    uint32_t i, sum = 0;
    for (i = 0x40; i < zm->file_len; i++)
        sum += pr8(zm, i);
    return (uint16_t)(sum & 0xFFFF) == pr16(zm, 0x1C);
}

static void exec_0op(Zm *zm, int op)
{
    switch (op) {
    case 0: do_return(zm, 1); break;                    /* rtrue */
    case 1: do_return(zm, 0); break;                    /* rfalse */
    case 2:                                             /* print */
        zm->pc = decode_print(zm, zm->pc, 0);
        break;
    case 3:                                             /* print_ret */
        zm->pc = decode_print(zm, zm->pc, 0);
        out_char(zm, 13);
        do_return(zm, 1);
        break;
    case 4: break;                                      /* nop */
    case 5:                                             /* save */
        if (zm->version <= 3) {
            uint8_t b0 = rd8(zm, zm->pc);
            /* resume point is just past this instruction's
             * branch data (1 or 2 bytes) */
            save_pc = zm->pc + ((b0 & 0x40) ? 1u : 2u);
            do_branch(zm, quetzal_save(zm));
        } else {
            int store = read_store(zm);
            save_pc = zm->pc;
            write_var(zm, store, quetzal_save(zm) ? 1 : 0);
        }
        break;
    case 6:                                             /* restore */
        if (zm->version <= 3) {
            if (quetzal_restore(zm)) {
                /* V3 branches on success at the saved PC: the
                 * branch data belongs to this instruction, which
                 * the restore has already left behind; treat as
                 * the V4 store form would: nothing more to do. */
            } else {
                do_branch(zm, 0);
            }
        } else {
            int store = read_store(zm);
            if (quetzal_restore(zm))
                complete_restored_save(zm);
            else
                write_var(zm, store, 0);
        }
        break;
    case 7: zm_restart(zm); break;                      /* restart */
    case 8: do_return(zm, pop(zm)); break;              /* ret_popped */
    case 9:                                             /* catch / pop */
        if (zm->version >= 5) {
            int store = read_store(zm);
            write_var(zm, store, (uint16_t)zm->frame_count);
        } else {
            (void)pop(zm);
        }
        break;
    case 10:                                            /* quit */
        flush_out(zm);
        zm->halted = 1;
        break;
    case 11: out_char(zm, 13); break;                   /* new_line */
    case 12:                                            /* show_status (v3) */
        if (zm->version == 3) {
            int player = rd16(zm, zm->globals_addr);
            int saved_win = zm->cur_window;
            int saved_lines = zm->upper_lines;
            if (zm->upper_lines < 1)
                zm->upper_lines = 1;
            zm->cur_window = 1;
            zm->upper_x = 0;
            zm->upper_y = 0;
            {
                int x;
                for (x = 0; x < ZM_UPPER_COLS; x++)
                    zm->upper[0][x] = ' ';
            }
            if (player && obj_parent(zm, player))
                print_obj_name(zm, obj_parent(zm, player));
            zm->upper_x = 30;
            if (zm->dyn[1] & 0x04) {
                /* timed game: globals 1/2 are hours/minutes */
                out_str(zm, "Time: ");
                out_num(zm, (int16_t)rd16(zm, zm->globals_addr + 2));
                out_char(zm, ':');
                {
                    int16_t mins = (int16_t)rd16(zm, zm->globals_addr + 4);
                    if (mins < 10)
                        out_char(zm, '0');
                    out_num(zm, mins);
                }
            } else {
                out_str(zm, "Score: ");
                out_num(zm, (int16_t)rd16(zm, zm->globals_addr + 2));
                out_str(zm, "  Moves: ");
                out_num(zm, (int16_t)rd16(zm, zm->globals_addr + 4));
            }
            zm->cur_window = saved_win;
            zm->upper_lines = saved_lines;
            zm->upper_dirty = 1;
        }
        break;
    case 13: do_branch(zm, verify_checksum(zm)); break; /* verify */
    case 15: do_branch(zm, 1); break;                   /* piracy */
    default: fatal(zm, "unimplemented 0OP opcode"); break;
    }
}

static void exec_1op(Zm *zm, int op, Ops *o)
{
    int store;
    switch (op) {
    case 0: do_branch(zm, o->v[0] == 0); break;         /* jz */
    case 1:                                             /* get_sibling */
        store = read_store(zm);
        write_var(zm, store, (uint16_t)obj_sibling(zm, o->v[0]));
        do_branch(zm, obj_sibling(zm, o->v[0]) != 0);
        break;
    case 2:                                             /* get_child */
        store = read_store(zm);
        write_var(zm, store, (uint16_t)obj_child(zm, o->v[0]));
        do_branch(zm, obj_child(zm, o->v[0]) != 0);
        break;
    case 3:                                             /* get_parent */
        store = read_store(zm);
        write_var(zm, store, (uint16_t)obj_parent(zm, o->v[0]));
        break;
    case 4:                                             /* get_prop_len */
        store = read_store(zm);
        write_var(zm, store, (uint16_t)get_prop_len(zm, o->v[0]));
        break;
    case 5:                                             /* inc */
        write_ref(zm, o->v[0], (uint16_t)(read_ref(zm, o->v[0]) + 1));
        break;
    case 6:                                             /* dec */
        write_ref(zm, o->v[0], (uint16_t)(read_ref(zm, o->v[0]) - 1));
        break;
    case 7: decode_print(zm, o->v[0], 0); break;        /* print_addr */
    case 8:                                             /* call_1s */
        store = read_store(zm);
        do_call(zm, o->v[0], 0, 0, store, zm->pc);
        break;
    case 9: obj_detach(zm, o->v[0]); break;             /* remove_obj */
    case 10: print_obj_name(zm, o->v[0]); break;        /* print_obj */
    case 11: do_return(zm, o->v[0]); break;             /* ret */
    case 12:                                            /* jump */
        zm->pc = (uint32_t)((int32_t)zm->pc + (int16_t)o->v[0] - 2);
        break;
    case 13:                                            /* print_paddr */
        decode_print(zm, unpack_addr(zm, o->v[0]), 0);
        break;
    case 14:                                            /* load */
        store = read_store(zm);
        write_var(zm, store, read_ref(zm, o->v[0]));
        break;
    case 15:                                            /* not / call_1n */
        if (zm->version >= 5) {
            do_call(zm, o->v[0], 0, 0, -1, zm->pc);
        } else {
            store = read_store(zm);
            write_var(zm, store, (uint16_t)~o->v[0]);
        }
        break;
    default: fatal(zm, "unimplemented 1OP opcode"); break;
    }
}


static void exec_2op(Zm *zm, int op, Ops *o)
{
    int store, i;
    switch (op) {
    case 1:                                             /* je */
        {
            int eq = 0;
            for (i = 1; i < o->n; i++)
                if (o->v[0] == o->v[i])
                    eq = 1;
            do_branch(zm, eq);
        }
        break;
    case 2: do_branch(zm, (int16_t)o->v[0] < (int16_t)o->v[1]); break;
    case 3: do_branch(zm, (int16_t)o->v[0] > (int16_t)o->v[1]); break;
    case 4:                                             /* dec_chk */
        {
            uint16_t v = (uint16_t)(read_ref(zm, o->v[0]) - 1);
            write_ref(zm, o->v[0], v);
            do_branch(zm, (int16_t)v < (int16_t)o->v[1]);
        }
        break;
    case 5:                                             /* inc_chk */
        {
            uint16_t v = (uint16_t)(read_ref(zm, o->v[0]) + 1);
            write_ref(zm, o->v[0], v);
            do_branch(zm, (int16_t)v > (int16_t)o->v[1]);
        }
        break;
    case 6: do_branch(zm, obj_parent(zm, o->v[0]) == o->v[1]); break; /* jin */
    case 7: do_branch(zm, (o->v[0] & o->v[1]) == o->v[1]); break;     /* test */
    case 8:
        store = read_store(zm);
        write_var(zm, store, (uint16_t)(o->v[0] | o->v[1]));
        break;
    case 9:
        store = read_store(zm);
        write_var(zm, store, (uint16_t)(o->v[0] & o->v[1]));
        break;
    case 10: do_branch(zm, test_attr(zm, o->v[0], o->v[1])); break;
    case 11: set_attr(zm, o->v[0], o->v[1], 1); break;
    case 12: set_attr(zm, o->v[0], o->v[1], 0); break;
    case 13: write_ref(zm, o->v[0], o->v[1]); break;    /* store */
    case 14: obj_insert(zm, o->v[0], o->v[1]); break;   /* insert_obj */
    case 15:
        store = read_store(zm);
        write_var(zm, store, rd16(zm, o->v[0] + 2u * o->v[1]));
        break;
    case 16:
        store = read_store(zm);
        write_var(zm, store, rd8(zm, o->v[0] + o->v[1]));
        break;
    case 17:
        store = read_store(zm);
        write_var(zm, store, get_prop(zm, o->v[0], o->v[1]));
        break;
    case 18:
        store = read_store(zm);
        write_var(zm, store, (uint16_t)get_prop_addr(zm, o->v[0], o->v[1]));
        break;
    case 19:
        store = read_store(zm);
        write_var(zm, store, (uint16_t)get_next_prop(zm, o->v[0], o->v[1]));
        break;
    case 20:
        store = read_store(zm);
        write_var(zm, store, (uint16_t)(o->v[0] + o->v[1]));
        break;
    case 21:
        store = read_store(zm);
        write_var(zm, store, (uint16_t)(o->v[0] - o->v[1]));
        break;
    case 22:
        store = read_store(zm);
        write_var(zm, store,
                  (uint16_t)((int16_t)o->v[0] * (int16_t)o->v[1]));
        break;
    case 23:
        store = read_store(zm);
        if (o->v[1] == 0) {
            fatal(zm, "division by zero");
            write_var(zm, store, 0);
        } else {
            write_var(zm, store,
                      (uint16_t)((int16_t)o->v[0] / (int16_t)o->v[1]));
        }
        break;
    case 24:
        store = read_store(zm);
        if (o->v[1] == 0) {
            fatal(zm, "modulo by zero");
            write_var(zm, store, 0);
        } else {
            write_var(zm, store,
                      (uint16_t)((int16_t)o->v[0] % (int16_t)o->v[1]));
        }
        break;
    case 25:                                            /* call_2s */
        store = read_store(zm);
        do_call(zm, o->v[0], &o->v[1], 1, store, zm->pc);
        break;
    case 26:                                            /* call_2n */
        do_call(zm, o->v[0], &o->v[1], 1, -1, zm->pc);
        break;
    case 27: break;                                     /* set_colour: mono */
    case 28: do_throw(zm, o->v[0], o->v[1]); break;     /* throw */
    default: fatal(zm, "unimplemented 2OP opcode"); break;
    }
}


static void exec_var(Zm *zm, int op, Ops *o)
{
    int store, i;
    switch (op) {
    case 0:                                             /* call_vs */
        store = read_store(zm);
        do_call(zm, o->v[0], &o->v[1], o->n - 1, store, zm->pc);
        break;
    case 1: wr16(zm, o->v[0] + 2u * o->v[1], o->v[2]); break;  /* storew */
    case 2: wr8(zm, o->v[0] + o->v[1], (uint8_t)o->v[2]); break; /* storeb */
    case 3: put_prop(zm, o->v[0], o->v[1], o->v[2]); break;
    case 4:                                             /* read */
        if (zm->version >= 5) {
            store = read_store(zm);
            do_read(zm, o->v, o->n, store);
        } else {
            do_read(zm, o->v, o->n, -1);
        }
        break;
    case 5: out_char(zm, o->v[0]); break;               /* print_char */
    case 6: out_num(zm, (int16_t)o->v[0]); break;       /* print_num */
    case 7:                                             /* random */
        store = read_store(zm);
        if (o->v[0] == 0) {
            zm->rng_state = (zm->io && zm->io->rand32)
                ? zm->io->rand32(zm) : zm->rng_state ^ 0x9E3779B9u;
            if (zm->rng_state == 0)
                zm->rng_state = 1;
            write_var(zm, store, 0);
        } else if ((int16_t)o->v[0] < 0) {
            zm->rng_state = (uint32_t)(-(int16_t)o->v[0]);
            if (zm->rng_state == 0)
                zm->rng_state = 1;
            write_var(zm, store, 0);
        } else {
            write_var(zm, store,
                      (uint16_t)(1 + rng_next(zm) % o->v[0]));
        }
        break;
    case 8: push(zm, o->v[0]); break;                   /* push */
    case 9: write_ref(zm, o->v[0], pop(zm)); break;     /* pull */
    case 10:                                            /* split_window */
        zm->upper_lines = o->v[0] > ZM_UPPER_LINES
            ? ZM_UPPER_LINES : o->v[0];
        zm->upper_dirty = 1;
        break;
    case 11:                                            /* set_window */
        zm->cur_window = o->v[0] ? 1 : 0;
        if (zm->cur_window == 0)
            flush_out(zm);
        break;
    case 12:                                            /* call_vs2 */
        store = read_store(zm);
        do_call(zm, o->v[0], &o->v[1], o->n - 1, store, zm->pc);
        break;
    case 13:                                            /* erase_window */
        {
            int16_t w = (int16_t)o->v[0];
            if (w == -1) {
                upper_clear(zm);
                zm->upper_lines = 0;
                zm->cur_window = 0;
                if (zm->io->clear_screen)
                    zm->io->clear_screen(zm);
            } else if (w == -2) {
                upper_clear(zm);
                if (zm->io->clear_screen)
                    zm->io->clear_screen(zm);
            } else if (w == 0) {
                if (zm->io->clear_screen)
                    zm->io->clear_screen(zm);
            } else if (w == 1) {
                upper_clear(zm);
            }
        }
        break;
    case 14:                                            /* erase_line */
        if (o->v[0] == 1 && zm->cur_window == 1) {
            int x;
            for (x = zm->upper_x; x < ZM_UPPER_COLS; x++)
                zm->upper[zm->upper_y][x] = ' ';
            zm->upper_dirty = 1;
        }
        break;
    case 15:                                            /* set_cursor */
        if ((int16_t)o->v[0] > 0 && zm->upper_lines > 0) {
            int y = o->v[0] - 1;
            int x = (int16_t)o->v[1] > 0 ? o->v[1] - 1 : 0;
            if (y >= zm->upper_lines)
                y = zm->upper_lines - 1;
            if (x >= ZM_UPPER_COLS)
                x = ZM_UPPER_COLS - 1;
            zm->upper_y = y;
            zm->upper_x = x;
        }
        break;
    case 16:                                            /* get_cursor */
        wr16(zm, o->v[0], (uint16_t)(zm->upper_y + 1));
        wr16(zm, o->v[0] + 2, (uint16_t)(zm->upper_x + 1));
        break;
    case 17: zm->text_style = o->v[0]; break;           /* set_text_style */
    case 18:                                            /* buffer_mode */
        zm->buffer_mode = o->v[0] ? 1 : 0;
        if (!zm->buffer_mode)
            flush_out(zm);
        break;
    case 19:                                            /* output_stream */
        {
            int16_t s = (int16_t)o->v[0];
            if (s == 1) zm->stream1_on = 1;
            else if (s == -1) zm->stream1_on = 0;
            else if (s == 2) zm->stream2_on = 1;
            else if (s == -2) zm->stream2_on = 0;
            else if (s == 3) {
                if (o->n > 1 && zm->stream3_depth < 4) {
                    zm->stream3_table[zm->stream3_depth++] = o->v[1];
                    wr16(zm, o->v[1], 0);
                }
            } else if (s == -3) {
                if (zm->stream3_depth > 0)
                    zm->stream3_depth--;
            }
        }
        break;
    case 20: break;                                     /* input_stream */
    case 21:                                            /* sound_effect */
        if ((o->v[0] == 1 || o->v[0] == 2) && zm->io->beep)
            zm->io->beep(zm);
        break;
    case 22:                                            /* read_char */
        store = read_store(zm);
        write_var(zm, store, do_read_char(zm, o->v, o->n));
        break;
    case 23:                                            /* scan_table */
        {
            uint16_t form = o->n > 3 ? o->v[3] : 0x82;
            int entry_len = form & 0x7F;
            int word_keys = (form & 0x80) != 0;
            uint32_t found = 0;
            if (entry_len > 0) {
                for (i = 0; i < o->v[2]; i++) {
                    uint32_t at = o->v[1] + (uint32_t)i * (uint32_t)entry_len;
                    uint16_t val = word_keys ? rd16(zm, at) : rd8(zm, at);
                    if (val == o->v[0]) {
                        found = at;
                        break;
                    }
                }
            }
            store = read_store(zm);
            write_var(zm, store, (uint16_t)found);
            do_branch(zm, found != 0);
        }
        break;
    case 24:                                            /* not (v5) */
        store = read_store(zm);
        write_var(zm, store, (uint16_t)~o->v[0]);
        break;
    case 25:                                            /* call_vn */
        do_call(zm, o->v[0], &o->v[1], o->n - 1, -1, zm->pc);
        break;
    case 26:                                            /* call_vn2 */
        do_call(zm, o->v[0], &o->v[1], o->n - 1, -1, zm->pc);
        break;
    case 27:                                            /* tokenise */
        tokenise_line(zm, o->v[0], o->v[1],
                      o->n > 2 ? o->v[2] : 0,
                      o->n > 3 ? o->v[3] : 0);
        break;
    case 28:                                            /* encode_text */
        encode_text_op(zm, o->v[0], o->v[1], o->v[2], o->v[3]);
        break;
    case 29:                                            /* copy_table */
        {
            uint32_t first = o->v[0], second = o->v[1];
            int32_t size = (int16_t)o->v[2];
            if (second == 0) {
                int32_t n = size < 0 ? -size : size;
                int32_t k;
                for (k = 0; k < n; k++)
                    wr8(zm, first + (uint32_t)k, 0);
            } else if (size > 0) {
                int32_t k;
                if (second > first) {
                    for (k = size - 1; k >= 0; k--)
                        wr8(zm, second + (uint32_t)k,
                            rd8(zm, first + (uint32_t)k));
                } else {
                    for (k = 0; k < size; k++)
                        wr8(zm, second + (uint32_t)k,
                            rd8(zm, first + (uint32_t)k));
                }
            } else if (size < 0) {
                int32_t k;
                for (k = 0; k < -size; k++)
                    wr8(zm, second + (uint32_t)k,
                        rd8(zm, first + (uint32_t)k));
            }
        }
        break;
    case 30:                                            /* print_table */
        {
            int width = o->v[1];
            int height = o->n > 2 ? o->v[2] : 1;
            int skip = o->n > 3 ? o->v[3] : 0;
            int r, c;
            for (r = 0; r < height; r++) {
                for (c = 0; c < width; c++)
                    out_char(zm, rd8(zm, o->v[0]
                        + (uint32_t)(r * (width + skip) + c)));
                if (r + 1 < height)
                    out_char(zm, 13);
            }
        }
        break;
    case 31:                                            /* check_arg_count */
        {
            int nargs = zm->frame_count
                ? zm->frames[zm->frame_count - 1].nargs : 0;
            do_branch(zm, o->v[0] >= 1 && o->v[0] <= nargs);
        }
        break;
    default: fatal(zm, "unimplemented VAR opcode"); break;
    }
}

/* Standard ZSCII 155..223 Unicode equivalents (for EXT print /
 * check unicode). */
static const uint16_t zscii_unicode[69] = {
    0xE4,0xF6,0xFC,0xC4,0xD6,0xDC,0xDF,0xBB,0xAB,0xEB,0xEF,0xFF,
    0xCB,0xCF,0xE1,0xE9,0xED,0xF3,0xFA,0xFD,0xC1,0xC9,0xCD,0xD3,
    0xDA,0xDD,0xE0,0xE8,0xEC,0xF2,0xF9,0xC0,0xC8,0xCC,0xD2,0xD9,
    0xE2,0xEA,0xEE,0xF4,0xFB,0xC2,0xCA,0xCE,0xD4,0xDB,0xE5,0xC5,
    0xF8,0xD8,0xE3,0xF1,0xF5,0xC3,0xD1,0xD5,0xE6,0xC6,0xE7,0xC7,
    0xFE,0xF0,0xDE,0xD0,0xA3,0x153,0x152,0xA1,0xBF
};

static void exec_ext(Zm *zm, int op, Ops *o)
{
    int store, i;
    switch (op) {
    case 0:                                             /* save */
        store = read_store(zm);
        if (o->n > 0) {
            write_var(zm, store, 0);   /* auxiliary saves: staged */
        } else {
            save_pc = zm->pc;
            write_var(zm, store, quetzal_save(zm) ? 1 : 0);
        }
        break;
    case 1:                                             /* restore */
        store = read_store(zm);
        if (o->n > 0) {
            write_var(zm, store, 0);
        } else if (quetzal_restore(zm)) {
            complete_restored_save(zm);
        } else {
            write_var(zm, store, 0);
        }
        break;
    case 2:                                             /* log_shift */
        store = read_store(zm);
        {
            int16_t shift = (int16_t)o->v[1];
            uint16_t r;
            if (shift > 0)
                r = (uint16_t)(o->v[0] << shift);
            else if (shift < 0)
                r = (uint16_t)(o->v[0] >> -shift);
            else
                r = o->v[0];
            write_var(zm, store, r);
        }
        break;
    case 3:                                             /* art_shift */
        store = read_store(zm);
        {
            int16_t shift = (int16_t)o->v[1];
            uint16_t r;
            if (shift > 0)
                r = (uint16_t)(o->v[0] << shift);
            else if (shift < 0)
                r = (uint16_t)((int16_t)o->v[0] >> -shift);
            else
                r = o->v[0];
            write_var(zm, store, r);
        }
        break;
    case 4:                                             /* set_font */
        store = read_store(zm);
        write_var(zm, store, o->v[0] == 1 ? 1 : 0);
        break;
    case 9:                                             /* save_undo */
        store = read_store(zm);
        write_var(zm, store, undo_snapshot(zm, zm->pc) ? 1 : 0);
        break;
    case 10:                                            /* restore_undo */
        store = read_store(zm);
        if (undo_restore(zm))
            complete_restored_save(zm);
        else
            write_var(zm, store, 0);
        break;
    case 11:                                            /* print_unicode */
        {
            uint16_t code = o->v[0];
            if (code < 128) {
                out_char(zm, code);
            } else {
                int found = 0;
                for (i = 0; i < 69; i++) {
                    if (zscii_unicode[i] == code) {
                        out_char(zm, (uint16_t)(155 + i));
                        found = 1;
                        break;
                    }
                }
                if (!found)
                    out_char(zm, '?');
            }
        }
        break;
    case 12:                                            /* check_unicode */
        store = read_store(zm);
        {
            uint16_t code = o->v[0];
            int res = 0;
            if (code >= 32 && code < 127)
                res = 3;
            else {
                for (i = 0; i < 69; i++)
                    if (zscii_unicode[i] == code) {
                        res = 1;
                        break;
                    }
            }
            write_var(zm, store, (uint16_t)res);
        }
        break;
    default:
        /* picture opcodes and other V6-only EXT opcodes: decoded
         * (operands consumed) and ignored. */
        break;
    }
}



static void step(Zm *zm)
{
    uint8_t b = rd8(zm, zm->pc++);
    Ops o;
    uint8_t types[2];
    int opnum, i;
#ifdef ZM_TRACE
    {
        extern void zm_trace_hook(uint32_t pc, int byte);
        zm_trace_hook(zm->pc - 1, b);
    }
#endif
    for (i = 0; i < 8; i++) {
        o.v[i] = 0;
        o.raw[i] = 0;
    }
    o.n = 0;
    if (zm->halted)
        return;

    if (b == 0xBE && zm->version >= 5) {
        /* extended opcode */
        opnum = rd8(zm, zm->pc++);
        types[0] = rd8(zm, zm->pc++);
        fetch_ops(zm, &o, types, 1, 0);
        exec_ext(zm, opnum, &o);
        return;
    }
    if (b < 0x80) {
        /* long form: 2OP, operand types from bits 6 and 5 */
        int t1 = (b & 0x40) ? 2 : 1;
        int t2 = (b & 0x20) ? 2 : 1;
        opnum = b & 0x1F;
        types[0] = (uint8_t)((t1 << 6) | (t2 << 4) | 0x0F);
        fetch_ops(zm, &o, types, 1,
                  opnum == 4 || opnum == 5 || opnum == 13);
        exec_2op(zm, opnum, &o);
        return;
    }    if (b < 0xB0) {
        /* short form: 1OP (or 0OP when the type field is 3) */
        int t = (b >> 4) & 3;
        opnum = b & 0x0F;
        if (t == 3) {
            exec_0op(zm, opnum);
            return;
        }
        types[0] = (uint8_t)((t << 6) | 0x3F);
        fetch_ops(zm, &o, types, 1,
                  opnum == 5 || opnum == 6 || opnum == 14);
        exec_1op(zm, opnum, &o);
        return;
    }
    if (b < 0xC0) {
        /* 0OP */
        exec_0op(zm, b & 0x0F);
        return;
    }
    if (b < 0xE0) {
        /* variable form, 2OP class */
        opnum = b & 0x1F;
        types[0] = rd8(zm, zm->pc++);
        fetch_ops(zm, &o, types, 1,
                  opnum == 4 || opnum == 5 || opnum == 13);
        exec_2op(zm, opnum, &o);
        return;
    }
    /* variable form, VAR class */
    opnum = b & 0x1F;
    types[0] = rd8(zm, zm->pc++);
    if (opnum == 12 || opnum == 26) {
        /* call_vs2 / call_vn2 carry a second type byte */
        types[1] = rd8(zm, zm->pc++);
        fetch_ops(zm, &o, types, 2, 0);
    } else {
        fetch_ops(zm, &o, types, 1, opnum == 9);
    }
    exec_var(zm, opnum, &o);
}

int zm_run(Zm *zm)
{
    while (!zm->halted)
        step(zm);
    flush_out(zm);
    return zm->halted;
}
