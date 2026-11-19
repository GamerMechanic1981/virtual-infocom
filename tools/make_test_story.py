#!/usr/bin/env python3
"""
make_test_story.py -- hand-built minimal Z5 story file for Virtual
Infocom. ALL text and code are original to this project (written for
this shakedown test); no Infocom game data is used or reproduced.

The story is "TEST CHAMBER": one room, a brass lamp, a smooth rock,
and a small hand-rolled parser (tokenise + dictionary-address
dispatch, the way real games bottom out). It exercises: print,
print_obj, print_num, print_char, read/tokenise (incl. unknown-word
reporting from the parse buffer), insert_obj/remove_obj,
get_parent/get_child/get_sibling walks, get_prop (descriptions),
attributes (score-once flag), globals, the upper window (status
line), and the save/restore opcodes.

Layout (byte addresses in the story file):
  0x0000 header (64 bytes)
  0x0040 abbreviation table (96 words, all zero)
  0x0100 object table (dynamic) -- 63 default words + 4 objects
  0x01F0 globals (240 words, dynamic)
  0x03D0 input text buffer (64 bytes, dynamic)
  0x0410 parse buffer (34 bytes, dynamic)
  0x0440 static base == dictionary (static)
  then strings and routines in high memory (4-byte aligned,
  packed address = byte address / 4, offsets zero).

Usage: python3 make_test_story.py [out.z5]
"""

import struct
import sys

# ------------------------------------------------------------------
# Z-character encoding (Z-machine standard alphabets)
# ------------------------------------------------------------------
A0 = "abcdefghijklmnopqrstuvwxyz"
A1 = "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
# z-chars 6..31 of alphabet A2. Slot 6 is a (duplicate) space and
# slot 7 is the newline in string encoding; digits start at z-char 8.
A2 = " \n0123456789.,!?_#'\"/\\-:()"


def zchars_of(s):
    """Encode a string to a list of z-chars (no packing)."""
    out = []
    for ch in s:
        if ch == " ":
            out.append(0)
        elif ch in A0:
            out.append(6 + A0.index(ch))
        elif ch in A1:
            out.append(4)  # shift to A1
            out.append(6 + A1.index(ch))
        elif ch in A2:
            out.append(5)  # shift to A2
            out.append(6 + A2.index(ch))
        else:
            code = ord(ch)
            if code > 1023:
                code = ord("?")
            out.append(5)
            out.append(6)
            out.append((code >> 5) & 31)
            out.append(code & 31)
    return out


def pack_zchars(zcs, pad=5):
    """Pack z-chars 3 per word, big-endian, end bit on last word."""
    zcs = list(zcs)
    while len(zcs) % 3:
        zcs.append(pad)
    words = []
    for i in range(0, len(zcs), 3):
        w = (zcs[i] << 10) | (zcs[i + 1] << 5) | zcs[i + 2]
        words.append(w)
    if words:
        words[-1] |= 0x8000
    else:
        words = [0x8000 | (5 << 10) | (5 << 5) | 5]
    return b"".join(struct.pack(">H", w) for w in words)


def encode_zstring(s):
    return pack_zchars(zchars_of(s))


def encode_dict_word(word):
    """V5 dictionary form: 9 z-chars in 6 bytes."""
    zcs = []
    for ch in word[:9]:
        if ch in A0:
            zcs.append(6 + A0.index(ch))
        elif ch in A2:
            zcs.append(5)
            zcs.append(6 + A2.index(ch))
        else:
            zcs.append(31)
    while len(zcs) < 9:
        zcs.append(5)
    zcs = zcs[:9]
    out = bytearray()
    for i in range(0, 9, 3):
        w = (zcs[i] << 10) | (zcs[i + 1] << 5) | zcs[i + 2]
        if i == 6:
            w |= 0x8000   # Z-string end bit on the final word, as in
                          # encode_zstring above and in real files (§77)
        out += struct.pack(">H", w)
    return bytes(out)


# ------------------------------------------------------------------
# Tiny Z-code assembler
# ------------------------------------------------------------------
def C(v):
    return ("c", v)


def V(n):
    return ("v", n)


class Fixup:
    def __init__(self, pos, kind, label):
        self.pos = pos
        self.kind = kind  # 'branch' | 'jump' | 'packed'
        self.label = label


class Routine:
    def __init__(self, name, nlocals):
        self.name = name
        self.code = bytearray()
        self.labels = {}
        self.fixups = []
        self.nlocals = nlocals
        self.code.append(nlocals)  # V5 routine header: local count

    def here(self):
        return len(self.code)

    def label(self, name):
        self.labels[name] = self.here()

    def emit(self, *bs):
        for b in bs:
            self.code.append(b & 0xFF)

    @staticmethod
    def _otype(op):
        if op[0] == "v":
            return 2
        return 1 if op[1] <= 255 else 0  # 1=small const, 0=large const

    def _emit_ops(self, ops):
        for op in ops:
            if op[0] == "v":
                self.emit(op[1])
            elif op[1] <= 255:
                self.emit(op[1])
            else:
                self.emit((op[1] >> 8) & 0xFF, op[1] & 0xFF)

    def _types_byte(self, ops):
        t = 0
        for i in range(4):
            code = self._otype(ops[i]) if i < len(ops) else 3
            t |= code << (6 - 2 * i)
        return t

    def op0(self, opcode):
        self.emit(0xB0 | opcode)

    def op1(self, opcode, op, store=None, branch=None):
        t = self._otype(op)
        self.emit((0x80 if t == 0 else 0x90 if t == 1 else 0xA0) | opcode)
        self._emit_ops([op])
        if store is not None:
            self.emit(store)
        if branch is not None:
            self._branch(branch)

    def op2(self, opcode, a, b, store=None, branch=None):
        if self._otype(a) in (1, 2) and self._otype(b) in (1, 2):
            byte = opcode
            if self._otype(a) == 2:
                byte |= 0x40
            if self._otype(b) == 2:
                byte |= 0x20
            self.emit(byte)
            self._emit_ops([a, b])
        else:
            self.emit(0xC0 | opcode, self._types_byte([a, b]))
            self._emit_ops([a, b])
        if store is not None:
            self.emit(store)
        if branch is not None:
            self._branch(branch)

    def opvar(self, opcode, ops, store=None, branch=None):
        self.emit(0xE0 | opcode, self._types_byte(ops))
        self._emit_ops(ops)
        if store is not None:
            self.emit(store)
        if branch is not None:
            self._branch(branch)

    def op2var(self, opcode, ops, branch=None):
        self.emit(0xC0 | opcode, self._types_byte(ops))
        self._emit_ops(ops)
        if branch is not None:
            self._branch(branch)

    def _branch(self, spec):
        on_true, label = spec
        pos = self.here()
        self.emit(0, 0)
        self.fixups.append(Fixup(pos, "branch", (label, on_true)))

    # ---- concrete instructions --------------------------------
    def print_str(self, s):
        self.op0(2)
        self.code += encode_zstring(s)

    def new_line(self):
        self.op0(11)

    def quit(self):
        self.op0(10)

    def ret(self, op):
        self.op1(11, op)

    def jump(self, label):
        self.emit(0x8C)  # jump, large-constant operand
        pos = self.here()
        self.emit(0, 0)
        self.fixups.append(Fixup(pos, "jump", label))

    def jz(self, op, label, on_true=True):
        self.op1(0, op, branch=(on_true, label))

    def je(self, ops, label, on_true=True):
        if len(ops) == 2:
            self.op2(1, ops[0], ops[1], branch=(on_true, label))
        else:
            self.op2var(1, ops, branch=(on_true, label))

    def test_attr(self, obj, attr, label, on_true=True):
        self.op2(10, obj, C(attr), branch=(on_true, label))

    def set_attr(self, obj, attr):
        self.op2(11, obj, C(attr))

    def store(self, var, op):
        self.op2(13, C(var), op)

    def loadb(self, base, off, store):
        self.op2(16, base, off, store=store)

    def loadw(self, base, off, store):
        self.op2(15, base, off, store=store)

    def add(self, a, b, store):
        self.op2(20, a, b, store=store)

    def inc(self, var):
        self.emit(0xA5, var)

    def dec(self, var):
        self.emit(0xA6, var)

    def insert_obj(self, obj, dest):
        self.op2(14, obj, dest)

    def get_parent(self, obj, store):
        self.op1(3, obj, store=store)

    def get_child(self, obj, store, label, on_true=True):
        self.op1(2, obj, store=store, branch=(on_true, label))

    def get_sibling(self, obj, store, label, on_true=True):
        self.op1(1, obj, store=store, branch=(on_true, label))

    def get_prop(self, obj, prop, store):
        self.op2(17, obj, C(prop), store=store)

    def print_obj(self, obj):
        self.op1(10, obj)

    def print_paddr_op(self, op):
        self.op1(13, op)

    def print_num(self, op):
        self.opvar(6, [op])

    def print_char(self, op):
        self.opvar(5, [op])

    def call(self, routine_label, args, store):
        # store is None -> call_vn (VAR 25, no store byte follows);
        # otherwise call_vs (VAR 0) with a store byte.
        ops = [("packed", routine_label)] + list(args)
        self.emit(0xE0 | (25 if store is None else 0), 0)
        tpos = self.here() - 1
        t = 0
        for i in range(4):
            if i < len(ops):
                code = 0 if ops[i][0] == "packed" else self._otype(ops[i])
            else:
                code = 3
            t |= code << (6 - 2 * i)
        self.code[tpos] = t
        for op in ops:
            if op[0] == "packed":
                pos = self.here()
                self.emit(0, 0)
                self.fixups.append(Fixup(pos, "packed", op[1]))
            else:
                self._emit_ops([op])
        if store is not None:
            self.emit(store)

    def read(self, textbuf, parsebuf, store):
        self.opvar(4, [C(textbuf), C(parsebuf)], store=store)

    def save(self, store):
        self.emit(0xB5, store)  # 0OP save; V4+: store byte follows

    def restore(self, store):
        self.emit(0xB6, store)

    def split_window(self, lines):
        self.opvar(10, [C(lines)])

    def set_window(self, win):
        self.opvar(11, [C(win)])

    def set_cursor(self, line, col):
        self.opvar(15, [line, col])

    def buffer_mode(self, flag):
        self.opvar(18, [C(flag)])

    def resolve(self, base_addr, label_addr):
        for fx in self.fixups:
            if fx.kind == "branch":
                label, on_true = fx.label
                target = base_addr + self.labels[label]
                after = base_addr + fx.pos + 2
                off = target - after + 2
                assert -8192 <= off < 8192, (self.name, label, off)
                self.code[fx.pos] = (0x80 if on_true else 0) | ((off >> 8) & 0x3F)
                self.code[fx.pos + 1] = off & 0xFF
            elif fx.kind == "jump":
                target = base_addr + self.labels[fx.label]
                after = base_addr + fx.pos + 2
                off = target - after + 2
                assert -32768 <= off < 32768
                self.code[fx.pos] = (off >> 8) & 0xFF
                self.code[fx.pos + 1] = off & 0xFF
            elif fx.kind == "packed":
                addr = label_addr(fx.label)
                assert addr % 4 == 0, fx.label
                packed = addr // 4
                self.code[fx.pos] = (packed >> 8) & 0xFF
                self.code[fx.pos + 1] = packed & 0xFF


# ------------------------------------------------------------------
# Story layout constants
# ------------------------------------------------------------------
HEADER = 0x0000
ABBREV = 0x0040
OBJTAB = 0x0100
GLOBALS = 0x01F0
TEXTBUF = 0x03D0
PARSEBUF = 0x0410
STATIC_BASE = 0x0440
DICT = STATIC_BASE

OBJ_ROOM, OBJ_PLAYER, OBJ_LAMP, OBJ_ROCK = 1, 2, 3, 4
ATTR_SCORED = 10
PROP_DESC = 1

G_SCORE = 16 + 0   # variable number of global 0
G_MOVES = 16 + 1   # variable number of global 1

WORDS = [
    (".", "sep"), (",", "sep"),
    ("all", "x"), ("brass", "adj"), ("down", "dir"), ("drop", "verb-drop"),
    ("east", "dir"), ("examine", "verb-examine"), ("get", "verb-take"),
    ("hello", "verb-hello"), ("i", "verb-inv"), ("inventory", "verb-inv"),
    ("jump", "verb-jump"), ("l", "verb-look"), ("lamp", "noun-lamp"),
    ("look", "verb-look"), ("north", "dir"), ("put", "verb-drop"),
    ("q", "verb-quit"), ("quit", "verb-quit"), ("restore", "verb-restore"),
    ("rock", "noun-rock"), ("save", "verb-save"), ("score", "verb-score"),
    ("smooth", "adj"), ("south", "dir"), ("take", "verb-take"),
    ("up", "dir"), ("west", "dir"), ("x", "verb-examine"),
]

# Strings (original text, written for this test)
S_INTRO_TITLE = "TEST CHAMBER"
S_INTRO = ("A shakedown cruise for the Virtual Infocom interpreter.\n"
           "Everything you see is a hand-built test story doing its "
           "exercises: parser, objects, score, save and restore.\n")
S_ROOM_DESC = ("A small white chamber, softly lit from nowhere in "
               "particular. The walls hum very faintly, as if pleased "
               "with themselves.")
S_LAMP_DESC = "A sturdy brass lamp, unlit and patient."
S_ROCK_DESC = "A smooth grey rock. It fits your palm exactly."
S_SEE = "You can see: "
S_TAKEN = "Taken."
S_DROPPED = "Dropped."
S_ALREADY = "You already have that."
S_NOTHERE = "It isn't here."
S_NOTCARRIED = "You aren't carrying it."
S_CANT = "You can't do that with it."
S_WHAT_TAKE = "What do you want to take?"
S_WHAT_DROP = "What do you want to drop?"
S_WHAT_EXAMINE = "What do you want to examine?"
S_UNKNOWN_WORD_PRE = "I don't know the word \""
S_UNKNOWN_WORD_POST = "\"."
S_DONT_UNDERSTAND = "I don't understand that sentence."
S_PARDON = "Pardon?"
S_CARRYING = "You are carrying:"
S_EMPTY = "You are empty-handed."
S_SCORE_A = "Your score is "
S_SCORE_B = " of 5, after "
S_SCORE_C = " moves."
S_SCORE_UP = "(Your score has gone up.)"
S_COMPLETE = ("That completes the shakedown: the interpreter, the "
              "parser and the save system all work. Thank you.")
S_HELLO = "The chamber hums back, politely."
S_JUMP = "You jump. The chamber approves."
S_NOWHERE = "There's nowhere to go. It's a chamber."
S_SAVED = "Saved."
S_SAVE_FAIL = "Save failed."
S_RESTORED = "Restored."
S_RESTORE_FAIL = "Restore failed."
S_BYE = "Thanks for testing."

STRINGS = {k: v for k, v in list(globals().items()) if k.startswith("S_")}



def build_story(D):
    """D = dictionary word -> address. Returns routines dict."""
    M = Routine("MAIN", 7)
    W1, W2, COUNT, OBJ, TMP, RES, ITER = 1, 2, 3, 4, 5, 6, 7

    M.print_str(S_INTRO_TITLE)
    M.new_line()
    M.print_str(S_INTRO)
    M.new_line()
    M.call("LOOK", [], None)

    M.label("turn")
    M.inc(G_MOVES)
    M.call("STATUS", [], None)
    M.print_str(">")
    M.read(TEXTBUF, PARSEBUF, RES)
    M.loadb(C(PARSEBUF), C(1), COUNT)
    M.jz(V(COUNT), "pardon", True)
    M.loadw(C(PARSEBUF), C(1), W1)
    M.jz(V(W1), "unknown_word", True)
    M.loadw(C(PARSEBUF), C(3), W2)

    M.je([V(W1), C(D["look"]), C(D["l"])], "v_look", True)
    M.je([V(W1), C(D["take"]), C(D["get"])], "v_take", True)
    M.je([V(W1), C(D["drop"]), C(D["put"])], "v_drop", True)
    M.je([V(W1), C(D["examine"]), C(D["x"])], "v_examine", True)
    M.je([V(W1), C(D["inventory"]), C(D["i"])], "v_inv", True)
    M.je([V(W1), C(D["score"])], "v_score", True)
    M.je([V(W1), C(D["save"])], "v_save", True)
    M.je([V(W1), C(D["restore"])], "v_restore", True)
    M.je([V(W1), C(D["quit"]), C(D["q"])], "v_quit", True)
    M.je([V(W1), C(D["hello"])], "v_hello", True)
    M.je([V(W1), C(D["jump"])], "v_jump", True)
    M.je([V(W1), C(D["north"]), C(D["south"]), C(D["east"])], "v_dir", True)
    M.je([V(W1), C(D["west"]), C(D["up"]), C(D["down"])], "v_dir", True)
    M.print_str(S_DONT_UNDERSTAND)
    M.new_line()
    M.jump("turn")

    M.label("pardon")
    M.print_str(S_PARDON)
    M.new_line()
    M.jump("turn")

    M.label("unknown_word")
    M.call("UNKNOWN", [], None)
    M.jump("turn")

    M.label("v_look")
    M.call("LOOK", [], None)
    M.jump("turn")

    M.label("v_hello")
    M.print_str(S_HELLO)
    M.new_line()
    M.jump("turn")

    M.label("v_jump")
    M.print_str(S_JUMP)
    M.new_line()
    M.jump("turn")

    M.label("v_dir")
    M.print_str(S_NOWHERE)
    M.new_line()
    M.jump("turn")

    # ---- TAKE ----
    M.label("v_take")
    M.je([V(COUNT), C(2)], "take_have_noun", True)
    M.print_str(S_WHAT_TAKE)
    M.new_line()
    M.jump("turn")
    M.label("take_have_noun")
    M.jz(V(W2), "unknown_word", True)
    M.je([V(W2), C(D["lamp"])], "take_lamp", True)
    M.je([V(W2), C(D["rock"])], "take_rock", True)
    M.print_str(S_CANT)
    M.new_line()
    M.jump("turn")
    M.label("take_lamp")
    M.store(OBJ, C(OBJ_LAMP))
    M.jump("take_common")
    M.label("take_rock")
    M.store(OBJ, C(OBJ_ROCK))
    M.label("take_common")
    M.get_parent(V(OBJ), TMP)
    M.je([V(TMP), C(OBJ_PLAYER)], "take_already", True)
    M.je([V(TMP), C(OBJ_ROOM)], "take_do", True)
    M.print_str(S_NOTHERE)
    M.new_line()
    M.jump("turn")
    M.label("take_already")
    M.print_str(S_ALREADY)
    M.new_line()
    M.jump("turn")
    M.label("take_do")
    M.insert_obj(V(OBJ), C(OBJ_PLAYER))
    M.print_str(S_TAKEN)
    M.new_line()
    M.je([V(OBJ), C(OBJ_LAMP)], "take_score_check", True)
    M.jump("turn")
    M.label("take_score_check")
    M.test_attr(C(OBJ_LAMP), ATTR_SCORED, "turn", True)
    M.set_attr(C(OBJ_LAMP), ATTR_SCORED)
    M.add(V(G_SCORE), C(5), G_SCORE)
    M.print_str(S_SCORE_UP)
    M.new_line()
    M.print_str(S_COMPLETE)
    M.new_line()
    M.jump("turn")

    # ---- DROP ----
    M.label("v_drop")
    M.je([V(COUNT), C(2)], "drop_have_noun", True)
    M.print_str(S_WHAT_DROP)
    M.new_line()
    M.jump("turn")
    M.label("drop_have_noun")
    M.jz(V(W2), "unknown_word", True)
    M.je([V(W2), C(D["lamp"])], "drop_lamp", True)
    M.je([V(W2), C(D["rock"])], "drop_rock", True)
    M.print_str(S_CANT)
    M.new_line()
    M.jump("turn")
    M.label("drop_lamp")
    M.store(OBJ, C(OBJ_LAMP))
    M.jump("drop_common")
    M.label("drop_rock")
    M.store(OBJ, C(OBJ_ROCK))
    M.label("drop_common")
    M.get_parent(V(OBJ), TMP)
    M.je([V(TMP), C(OBJ_PLAYER)], "drop_do", True)
    M.print_str(S_NOTCARRIED)
    M.new_line()
    M.jump("turn")
    M.label("drop_do")
    M.get_parent(C(OBJ_PLAYER), TMP)
    M.insert_obj(V(OBJ), V(TMP))
    M.print_str(S_DROPPED)
    M.new_line()
    M.jump("turn")


    # ---- EXAMINE ----
    M.label("v_examine")
    M.je([V(COUNT), C(2)], "ex_have_noun", True)
    M.print_str(S_WHAT_EXAMINE)
    M.new_line()
    M.jump("turn")
    M.label("ex_have_noun")
    M.jz(V(W2), "unknown_word", True)
    M.je([V(W2), C(D["lamp"])], "ex_lamp", True)
    M.je([V(W2), C(D["rock"])], "ex_rock", True)
    M.print_str(S_CANT)
    M.new_line()
    M.jump("turn")
    M.label("ex_lamp")
    M.store(OBJ, C(OBJ_LAMP))
    M.jump("ex_common")
    M.label("ex_rock")
    M.store(OBJ, C(OBJ_ROCK))
    M.label("ex_common")
    M.get_prop(V(OBJ), PROP_DESC, TMP)
    M.print_paddr_op(V(TMP))
    M.new_line()
    M.jump("turn")

    # ---- INVENTORY ----
    M.label("v_inv")
    M.get_child(C(OBJ_PLAYER), ITER, "inv_some", True)
    M.print_str(S_EMPTY)
    M.new_line()
    M.jump("turn")
    M.label("inv_some")
    M.print_str(S_CARRYING)
    M.new_line()
    M.label("inv_loop")
    M.print_str("  ")
    M.print_obj(V(ITER))
    M.new_line()
    M.get_sibling(V(ITER), ITER, "inv_loop", True)
    M.jump("turn")

    # ---- SCORE ----
    M.label("v_score")
    M.print_str(S_SCORE_A)
    M.print_num(V(G_SCORE))
    M.print_str(S_SCORE_B)
    M.print_num(V(G_MOVES))
    M.print_str(S_SCORE_C)
    M.new_line()
    M.jump("turn")

    # ---- SAVE / RESTORE ----
    M.label("v_save")
    M.save(RES)
    # A successful RESTORE resumes execution HERE, just after the
    # save, with RES == 2 -- the standard Z-machine convention.
    M.je([V(RES), C(2)], "restore_ok", True)
    M.je([V(RES), C(1)], "save_ok", True)
    M.print_str(S_SAVE_FAIL)
    M.new_line()
    M.jump("turn")
    M.label("save_ok")
    M.print_str(S_SAVED)
    M.new_line()
    M.jump("turn")

    M.label("v_restore")
    M.restore(RES)
    # On success execution resumes here with RES == 2.
    M.je([V(RES), C(2)], "restore_ok", True)
    M.print_str(S_RESTORE_FAIL)
    M.new_line()
    M.jump("turn")
    M.label("restore_ok")
    M.print_str(S_RESTORED)
    M.new_line()
    M.jump("turn")

    # ---- QUIT ----
    M.label("v_quit")
    M.print_str(S_BYE)
    M.new_line()
    M.quit()

    # ---- LOOK subroutine: locals 1=room, 2=iter, 3=any ----
    L = Routine("LOOK", 3)
    LR, LI, LANY = 1, 2, 3
    L.get_parent(C(OBJ_PLAYER), LR)
    L.print_obj(V(LR))
    L.new_line()
    L.get_prop(V(LR), PROP_DESC, LANY)
    L.print_paddr_op(V(LANY))
    L.new_line()
    L.store(LANY, C(0))
    L.get_child(V(LR), LI, "look_scan", True)
    L.jump("look_done")
    L.label("look_scan")
    L.je([V(LI), C(OBJ_PLAYER)], "look_next", True)
    L.je([V(LANY), C(1)], "look_more", True)
    L.print_str(S_SEE)
    L.store(LANY, C(1))
    L.jump("look_item")
    L.label("look_more")
    L.print_str(", ")
    L.label("look_item")
    L.print_obj(V(LI))
    L.label("look_next")
    L.get_sibling(V(LI), LI, "look_scan", True)
    L.je([V(LANY), C(1)], "look_period", True)
    L.jump("look_done")
    L.label("look_period")
    L.print_str(".")
    L.new_line()
    L.label("look_done")
    L.ret(C(0))

    # ---- STATUS subroutine: local 1 = counter/temp ----
    T = Routine("STATUS", 1)
    T.split_window(1)
    T.set_window(1)
    T.buffer_mode(0)
    T.set_cursor(C(1), C(1))
    T.store(1, C(48))
    T.label("st_clear")
    T.print_char(C(32))
    T.dec(1)
    T.jz(V(1), "st_room", True)
    T.jump("st_clear")
    T.label("st_room")
    T.set_cursor(C(1), C(1))
    T.get_parent(C(OBJ_PLAYER), 1)
    T.print_obj(V(1))
    T.set_cursor(C(1), C(30))
    T.print_str("Score: ")
    T.print_num(V(G_SCORE))
    T.print_str("  Moves: ")
    T.print_num(V(G_MOVES))
    T.set_window(0)
    T.buffer_mode(1)
    T.ret(C(0))

    # ---- UNKNOWN subroutine: print word 1 of the input ----
    # locals: 1 = pointer into text buffer, 2 = remaining length;
    # each char is pushed with loadb (store 0) and popped by
    # print_char's variable-0 operand.
    U = Routine("UNKNOWN", 2)
    U.loadb(C(PARSEBUF), C(4), 2)   # length of word 1
    U.loadb(C(PARSEBUF), C(5), 1)   # position of word 1 in text buffer
    U.print_str(S_UNKNOWN_WORD_PRE)
    U.label("u_loop")
    U.jz(V(2), "u_done", True)
    U.loadb(C(TEXTBUF), V(1), 0)    # push char
    U.print_char(V(0))              # pop and print it
    U.inc(1)
    U.dec(2)
    U.jump("u_loop")
    U.label("u_done")
    U.print_str(S_UNKNOWN_WORD_POST)
    U.new_line()
    U.ret(C(0))

    return {"MAIN": M, "LOOK": L, "STATUS": T, "UNKNOWN": U}



def main():
    out_path = sys.argv[1] if len(sys.argv) > 1 else "test_story.z5"

    # ---- dictionary ----
    entries = []
    for word, role in WORDS:
        entries.append((encode_dict_word(word), word, role))
    entries.sort(key=lambda e: e[0])  # sorted by encoded form
    dict_bytes = bytearray()
    dict_bytes.append(2)               # two separators
    dict_bytes += b".,"
    dict_bytes.append(7)               # entry length
    dict_bytes += struct.pack(">H", len(entries))
    D = {}
    for enc, word, role in entries:
        D[word] = DICT + len(dict_bytes)
        dict_bytes += enc
        dict_bytes.append(0)           # one data byte, unused

    routines = build_story(D)

    # ---- string pool placement ----
    high_at = DICT + len(dict_bytes)
    while high_at % 4:
        high_at += 1
    pool = bytearray()
    pool_addr = {}
    cur = high_at
    for name, text in STRINGS.items():
        while cur % 4:
            pool.append(0)
            cur += 1
        pool_addr[name] = cur
        enc = encode_zstring(text)
        pool += enc
        cur += len(enc)

    code_at = cur
    while code_at % 4:
        pool.append(0)
        code_at += 1
    routine_addr = {}
    code_blob = bytearray()
    for name, r in routines.items():
        while (code_at + len(code_blob)) % 4:
            code_blob.append(0)
        routine_addr[name] = code_at + len(code_blob)
        r._blob_off = len(code_blob)
        code_blob += r.code

    def label_addr(name):
        if name in routine_addr:
            return routine_addr[name]
        return pool_addr[name]

    for name, r in routines.items():
        r.resolve(routine_addr[name], label_addr)
        code_blob[r._blob_off:r._blob_off + len(r.code)] = r.code

    # ---- object table ----
    entries_at = OBJTAB + 63 * 2
    prop_tables_at = entries_at + 4 * 14

    def prop_table(name_str, desc_packed=None):
        t = bytearray()
        enc = encode_zstring(name_str)
        t.append(len(enc) // 2)      # name length in words
        t += enc
        if desc_packed is not None:
            t.append(0x40 | PROP_DESC)  # 2-byte property
            t += struct.pack(">H", desc_packed)
        t.append(0)
        return t

    pt = {}
    pt[OBJ_ROOM] = prop_table("Testing Chamber", pool_addr["S_ROOM_DESC"] // 4)
    pt[OBJ_PLAYER] = prop_table("yourself")
    pt[OBJ_LAMP] = prop_table("brass lamp", pool_addr["S_LAMP_DESC"] // 4)
    pt[OBJ_ROCK] = prop_table("smooth rock", pool_addr["S_ROCK_DESC"] // 4)
    pt_addr = {}
    pt_blob = bytearray()
    for obj in (OBJ_ROOM, OBJ_PLAYER, OBJ_LAMP, OBJ_ROCK):
        pt_addr[obj] = prop_tables_at + len(pt_blob)
        pt_blob += pt[obj]

    def entry(attrs, parent, sibling, child, props):
        e = bytearray()
        e += attrs.to_bytes(6, "big")
        e += struct.pack(">HHH", parent, sibling, child)
        e += struct.pack(">H", props)
        assert len(e) == 14
        return e

    objtab = bytearray(63 * 2)         # default properties, all zero
    objtab += entry(0, 0, 0, OBJ_PLAYER, pt_addr[OBJ_ROOM])
    objtab += entry(0, OBJ_ROOM, OBJ_LAMP, 0, pt_addr[OBJ_PLAYER])
    objtab += entry(0, OBJ_ROOM, OBJ_ROCK, 0, pt_addr[OBJ_LAMP])
    objtab += entry(0, OBJ_ROOM, 0, 0, pt_addr[OBJ_ROCK])
    objtab += pt_blob
    assert OBJTAB + len(objtab) <= GLOBALS, hex(OBJTAB + len(objtab))

    # ---- assemble the file ----
    img = bytearray()
    img += bytes(64)
    img += bytes(ABBREV + 192 - len(img))
    assert len(img) == OBJTAB
    img += objtab
    img += bytes(GLOBALS - len(img))
    img += bytes(240 * 2)              # globals, all zero
    assert len(img) == TEXTBUF
    img += bytes(64)                   # text buffer space
    img += bytes(34)                   # parse buffer space
    img += bytes(STATIC_BASE - len(img))
    assert len(img) == STATIC_BASE
    img += dict_bytes
    img += bytes(high_at - len(img))
    img += pool
    img += code_blob

    while len(img) % 8:
        img.append(0)
    total = len(img)

    # buffer capacities live in the buffers' first bytes (the game
    # supplies them as initial dynamic-memory data)
    img[TEXTBUF] = 60    # max input characters
    img[PARSEBUF] = 8    # max words per parse

    def put16(off, val):
        img[off] = (val >> 8) & 0xFF
        img[off + 1] = val & 0xFF

    img[0] = 5
    put16(2, 1)                            # release 1
    put16(4, STATIC_BASE)                  # high memory base
    put16(6, routine_addr["MAIN"] + 1)     # initial PC: V5 header field is the
                                           # byte address of the first
                                           # instruction (routine header byte
                                           # sits at MAIN), as in real files
                                           # (§77) — NOT a packed address
    put16(8, DICT)                         # dictionary
    put16(0x0A, OBJTAB)                    # object table
    put16(0x0C, GLOBALS)                   # globals
    put16(0x0E, STATIC_BASE)               # static base
    img[0x12:0x18] = b"261116"             # serial
    put16(0x18, ABBREV)                    # abbreviations
    put16(0x1A, total // 4)                # file length / 4 (V4/V5 unit)
    put16(0x28, 0)                         # routines offset
    put16(0x2A, 0)                         # strings offset
    checksum = sum(img[0x40:total]) & 0xFFFF
    put16(0x1C, checksum)

    with open(out_path, "wb") as f:
        f.write(img)

    assert img[0] == 5
    assert (img[0x1A] << 8 | img[0x1B]) * 4 == total
    assert (sum(img[0x40:]) & 0xFFFF) == checksum
    print(f"wrote {out_path}: {total} bytes, checksum 0x{checksum:04X}, "
          f"release 1, serial 261116")
    print(f"dictionary at 0x{DICT:04X} ({len(dict_bytes)} bytes), "
          f"MAIN at 0x{routine_addr['MAIN']:04X} "
          f"(packed 0x{routine_addr['MAIN'] // 4:04X}), "
          f"dynamic size 0x{STATIC_BASE:04X}")


if __name__ == "__main__":
    main()
