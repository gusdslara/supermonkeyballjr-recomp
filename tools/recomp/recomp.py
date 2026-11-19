#!/usr/bin/env python3
"""Super Recomp Ball Jr. static recompiler: GBA ARM7TDMI (Thumb + ARM) -> C.

Reads the player's own ROM, finds the code (recursive descent from the entry point, code
pointers in literal pools and pointer tables, and the addresses listed in entries.txt that were
learned at runtime), and writes one C function per contiguous code chunk. Every guest
instruction becomes straight-line C with all of its fields constant, so the native compiler folds
the decode away; branches inside a chunk become gotos. Instruction semantics come from mGBA's
reference implementation (isa-thumb.c / isa-arm.c, MPL-2.0), turned into always-inline functions,
so the recompiled code is cycle-exact with the interpreter.

The output is derived from the game and must never be committed or distributed.

usage: recomp.py --rom game.gba --mgba externals/mgba --entries tools/recomp/entries.txt --out DIR
"""
import argparse
import hashlib
import os
import re
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from gbarom import read_ramcode  # noqa: E402
from opnames import ARM, THUMB  # noqa: E402
import armfast  # noqa: E402

ROM_BASE = 0x08000000
SHARDS = 32          # fixed number of generated .c files (CMake needs the list at configure time)
MAX_CHUNK = 800      # instructions per C function (bigger functions compile much slower)
MAX_GAP = 64         # bytes of non-code (literal pools) allowed inside one chunk

THUMB_BCOND = {"BEQ", "BNE", "BCS", "BCC", "BMI", "BPL", "BVS", "BVC",
               "BHI", "BLS", "BGE", "BLT", "BGT", "BLE"}
ARM_ALU = ("AND", "EOR", "SUB", "RSB", "ADD", "ADC", "SBC", "RSC", "ORR", "MOV", "BIC", "MVN")


def sext(v, bits):
    v &= (1 << bits) - 1
    return v - (1 << bits) if v & (1 << (bits - 1)) else v


def fnv64(data):
    h = 0xcbf29ce484222325
    # Hash 32-bit words to keep this fast in Python; the runtime does the same.
    for (w,) in struct.iter_unpack("<I", data[: len(data) & ~3]):
        h ^= w
        h = (h * 0x100000001b3) & 0xFFFFFFFFFFFFFFFF
    return h


class Space:
    """A block of guest memory whose contents are known at build time."""

    def __init__(self, name, base, data, ram):
        self.name, self.base, self.data, self.ram = name, base, data, ram

    def has(self, addr, size):
        return self.base <= addr and addr + size <= self.base + len(self.data)

    def u16(self, addr):
        o = addr - self.base
        return self.data[o] | self.data[o + 1] << 8

    def u32(self, addr):
        return struct.unpack_from("<I", self.data, addr - self.base)[0]


def thumb_flow(sp, addr):
    """-> (falls_through, [local/jump targets], [call targets], [arm targets])"""
    op = sp.u16(addr)
    name = THUMB[op >> 6]
    if name in THUMB_BCOND:
        return True, [addr + 4 + sext(op & 0xFF, 8) * 2], [], []
    if name == "B":
        return False, [addr + 4 + sext(op & 0x7FF, 11) * 2], [], []
    if name == "BL2":
        if sp.has(addr - 2, 2) and THUMB[sp.u16(addr - 2) >> 6] == "BL1":
            hi = sext(sp.u16(addr - 2) & 0x7FF, 11) << 12
            return True, [], [(addr - 2 + 4 + hi + (op & 0x7FF) * 2) & 0xFFFFFFFF], []
        return True, [], [], []
    if name == "BX":
        rm = (op >> 3) & 0xF
        return False, [], [], ([(addr + 4) & ~3] if rm == 15 else [])
    if name in ("POPR", "ILL", "BKPT"):
        return False, [], [], []
    if name[:4] in ("ADD4", "MOV3") and len(name) == 6 and name[4] == "1" and (op & 7) == 7:
        return False, [], [], []  # high-register op with Rd = PC: indirect jump
    return True, [], [], []


def thumb_writes_pc(name, op):
    if name in THUMB_BCOND or name in ("B", "BL2", "BX", "POPR", "SWI", "ILL", "BKPT"):
        return True
    return name[:4] in ("ADD4", "MOV3") and len(name) == 6 and name[4] == "1" and (op & 7) == 7


def arm_name(op):
    return ARM[((op >> 16) & 0xFF0) | ((op >> 4) & 0xF)]


THUMB_SIMPLE = {"LSL1", "LSR1", "ASR1", "ADD3", "SUB3", "ADD1", "SUB1", "MOV1", "CMP1", "ADD2", "SUB2",
                "AND", "EOR", "LSL2", "LSR2", "ASR2", "ADC", "SBC", "ROR", "TST", "NEG", "CMP2", "CMN",
                "ORR", "MUL", "BIC", "MVN", "ADD6", "ADD7", "SUB4"}
ARM_MUL = {"MUL", "MULS", "MLA", "MLAS", "UMULL", "UMULLS", "UMLAL", "UMLALS",
           "SMULL", "SMULLS", "SMLAL", "SMLALS"}


def thumb_simple(name, op):
    """Register/flag-only instruction: never reads PC or prefetch, never touches memory or PC."""
    if os.environ.get("RECOMP_NO_SIMPLE"):
        return False
    if name in THUMB_SIMPLE:
        return True
    if name[:4] in ("ADD4", "CMP3", "MOV3") and len(name) == 6:
        rd = (op & 7) | (8 if name[4] == "1" else 0)
        rm = ((op >> 3) & 7) | (8 if name[5] == "1" else 0)
        return rd != 15 and rm != 15
    return False


def arm_simple(name, op):
    if os.environ.get("RECOMP_NO_SIMPLE"):
        return False
    if name in ARM_MUL:
        return all((op >> s) & 15 != 15 for s in (0, 8, 12, 16))
    if not name.startswith(ARM_ALU + ("TST", "TEQ", "CMP", "CMN")):
        return False
    regs = [(op >> 16) & 15, (op >> 12) & 15]
    if not (op & 0x02000000):  # register operand (and register shift amount)
        regs.append(op & 15)
        if op & 0x10:
            regs.append((op >> 8) & 15)
    return 15 not in regs


def arm_flow(sp, addr):
    op = sp.u32(addr)
    cond = op >> 28
    name = arm_name(op)
    cond_fall = cond != 0xE
    if cond == 0xF:
        return True, [], [], []
    if name == "B":
        return cond_fall, [(addr + 8 + sext(op & 0xFFFFFF, 24) * 4) & 0xFFFFFFFF], [], []
    if name == "BL":
        return True, [], [(addr + 8 + sext(op & 0xFFFFFF, 24) * 4) & 0xFFFFFFFF], []
    if name == "BX":
        return cond_fall, [], [], []
    if name in ("ILL", "BKPT", "CDP", "LDC", "STC", "MCR", "MRC"):
        return False, [], [], []
    rd = (op >> 12) & 0xF
    if name.startswith(ARM_ALU) and not name.startswith(("TST", "TEQ", "CMP", "CMN")) and rd == 15:
        return cond_fall, [], [], []
    if name.startswith("LDR") and rd == 15:
        return cond_fall, [], [], []
    if name.startswith("LDM") and op & 0x8000:
        return cond_fall, [], [], []
    return True, [], [], []


class Code:
    def __init__(self):
        self.insns = {}  # (space, mode) -> set(addr)

    def add(self, sp, mode, addr):
        self.insns.setdefault((sp, mode), set()).add(addr)


def discover(spaces, entries):
    """Recursive descent. entries: list of (mode 'T'/'A', addr). Returns Code.
    A RAM address can hold several overlays: an entry there is decoded in every space that has it,
    and control flow stays inside the space it started in."""
    code = Code()
    seen = set()
    work = []
    for mode, addr in entries:
        for sp in spaces:
            if sp.has(addr, 2):
                work.append((sp, mode, addr))

    def target_space(cur, addr, size):
        if cur.has(addr, size):
            return cur
        for sp in spaces:
            if sp.has(addr, size):
                return sp
        return None

    while work:
        sp, mode, addr = work.pop()
        width = 2 if mode == "T" else 4
        addr &= ~(width - 1)
        while sp is not None and sp.has(addr, width):
            key = (sp.name, mode, addr)
            if key in seen:
                break
            seen.add(key)
            code.add(sp, mode, addr)
            if mode == "T":
                fall, jumps, calls, arms = thumb_flow(sp, addr)
                op = sp.u16(addr)
                if THUMB[op >> 6] == "LDR3":  # ldr rd, [pc, #imm]: look for code pointers
                    lit = ((addr + 4) & ~3) + (op & 0xFF) * 4
                    if sp.has(lit, 4):
                        v = sp.u32(lit)
                        if v & 1:
                            t = target_space(sp, v & ~1, 2)
                            if t:
                                work.append((t, "T", v & ~1))
            else:
                fall, jumps, calls, arms = arm_flow(sp, addr)
                arms = []
            for t in jumps + calls:
                ts = target_space(sp, t, width)
                if ts:
                    work.append((ts, mode, t))
            for t in arms:
                ts = target_space(sp, t, 4)
                if ts:
                    work.append((ts, "A", t))
            if not fall:
                break
            addr += width
    return code


def pointer_scan(rom):
    """Thumb function pointers stored anywhere in the ROM (state tables, callbacks)."""
    out = []
    end = ROM_BASE + len(rom.data)
    for off in range(0, len(rom.data) - 3, 4):
        v = struct.unpack_from("<I", rom.data, off)[0]
        if v & 1 and ROM_BASE <= v < end:
            t = v & ~1
            op = rom.u16(t)
            if (op & 0xFE00) == 0xB400:  # push {..} / push {.., lr}: a function prologue
                out.append(("T", t))
    return out


def chunks_of(addrs, width):
    addrs = sorted(addrs)
    chunk = []
    for a in addrs:
        if chunk and (a - chunk[-1] > MAX_GAP or len(chunk) >= MAX_CHUNK):
            yield chunk
            chunk = []
        chunk.append(a)
    if chunk:
        yield chunk


OPS_HEADER = """/* Generated by tools/recomp/recomp.py from mGBA's {src} (MPL-2.0, (c) Jeffrey Pfau).
 * Each instruction handler becomes an always-inline function so that a constant opcode folds away. */
"""


def make_ops(mgba, which):
    src = os.path.join(mgba, "src", "arm", "isa-%s.c" % which)
    text = open(src, encoding="utf-8").read()
    if which == "thumb":
        text = re.sub(r"static void _ThumbInstruction ## NAME \(struct ARMCore\* cpu, unsigned opcode\)",
                      "static RC_INLINE void T_ ## NAME (struct ARMCore* cpu, const unsigned opcode)", text)
        text = text[: text.index("const ThumbInstruction _thumbTable")]
    else:
        text = re.sub(r"static void _ARMInstruction ## NAME \(struct ARMCore\* cpu, uint32_t opcode\)",
                      "static RC_INLINE void A_ ## NAME (struct ARMCore* cpu, const uint32_t opcode)", text)
        text = text[: text.index("const ARMInstruction _armTable")]
    # IWRAM/EWRAM accesses take the inline fast path from rc_prelude.h.
    if not os.environ.get("RECOMP_NO_FASTMEM"):
        for kind in ("load32", "load16", "load8", "store32", "store16", "store8"):
            text = text.replace("cpu->memory.%s(" % kind, "rc_%s(" % kind)
    if " T_ ## NAME" not in text and " A_ ## NAME" not in text:
        raise SystemExit("recomp: could not transform %s (mGBA version changed?)" % src)
    return OPS_HEADER.format(src="src/arm/isa-%s.c" % which) + text


def emit_chunk(sp, mode, addrs, fname):
    """C source for one chunk function."""
    width = 2 if mode == "T" else 4
    aset = set(addrs)
    out = ["int %s(struct ARMCore* cpu, uint32_t pc) {" % fname, "\tswitch (pc) {"]
    for a in addrs:
        out.append("\tcase 0x%08Xu: goto L_%08X;" % (a, a))
    out.append("\tdefault: return 0;\n\t}")
    for a in addrs:
        pc = a + 2 * width
        def pf(x):
            return ("0x%X" % (sp.u16(x) if mode == "T" else sp.u32(x))) if sp.has(x, width) else "0"
        pre = "0x%08Xu, %s, %s" % (pc, pf(a + width), pf(a + 2 * width))
        if mode == "T":
            op = sp.u16(a)
            name = THUMB[op >> 6]
            simple = thumb_simple(name, op)
        else:
            op = sp.u32(a)
            name = arm_name(op)
            simple = arm_simple(name, op)
        # Simple ALU instructions never look at PC/prefetch: that state is only written when control
        # can leave the chunk (RC_EVENT_AT) or before an instruction that needs it.
        out.append(("L_%08X:" % a) if simple else ("L_%08X: RC_PRE(%s);" % (a, pre)))
        if mode == "T":
            out.append("\tT_%s(cpu, 0x%04Xu);" % (name, op))
            if thumb_writes_pc(name, op):
                _, jumps, _, _ = thumb_flow(sp, a)
                local = [t for t in jumps if t in aset]
                if local:
                    t = local[0]
                    out.append("\tRC_BRANCH(0x%08Xu, 0x%08Xu, L_%08X);" % (pc, t + 2, t))
                else:
                    out.append("\tRC_EXIT_IF_PC(0x%08Xu);" % pc)
        else:
            cond = op >> 28
            if cond == 0xE:
                out.append("\tA_%s(cpu, 0x%08Xu);" % (name, op))
            elif cond == 0xF:
                out.append("\tcpu->cycles += ARM_PREFETCH_CYCLES;")
            else:
                out.append("\tif (ARMTestCondition(cpu, %d)) A_%s(cpu, 0x%08Xu); else cpu->cycles += ARM_PREFETCH_CYCLES;"
                           % (cond, name, op))
            if not simple:
                _, jumps, _, _ = arm_flow(sp, a)
                local = [t for t in jumps if t in aset]
                if local:
                    t = local[0]
                    out.append("\tRC_BRANCH_ARM(0x%08Xu, 0x%08Xu, L_%08X);" % (pc, t + 4, t))
                else:
                    out.append("\tRC_EXIT_IF_PC_ARM(0x%08Xu);" % pc)
        out.append(("\tRC_EVENT_AT(%s);" % pre) if simple else "\tRC_EVENT();")
    # Falling off the end of the chunk: leave the interpreter-visible state of the last instruction.
    out.append("\tRC_PRE(%s);\n\treturn 1;\n}\n" % pre)
    return "\n".join(out)


def load_entries(path):
    """entries.txt: 'T 0800ABCD' / 'A 03004000' (mode, address)."""
    entries = []
    if path and os.path.exists(path):
        for line in open(path, encoding="utf-8"):
            f = line.split("#")[0].split()
            if f:
                entries.append((f[0], int(f[1], 16)))
    return entries


def ram_spaces(ramcode):
    """RAM code (copied or unpacked from the ROM at runtime), checked by a guard before each run.
    Later sources at the same address win (one RAM area can host several overlays)."""
    out = []
    for i, (base, data, spec) in enumerate(ramcode):
        out.append(Space("ram%d_%08X" % (i, base), base, data, True))
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--rom", required=True)
    ap.add_argument("--mgba", required=True)
    ap.add_argument("--entries")
    ap.add_argument("--ramcode")
    ap.add_argument("--out", required=True)
    ap.add_argument("--stats", action="store_true")
    args = ap.parse_args()

    data = open(args.rom, "rb").read()
    if data[0xAC:0xB0] != b"ALUE":
        raise SystemExit("recomp: %s is not Super Monkey Ball Jr. (game code %r)" % (args.rom, data[0xAC:0xB0]))
    rom = Space("rom", ROM_BASE, data, False)
    entries = load_entries(args.entries)
    ramcode = read_ramcode(args.ramcode, data)
    # mGBA's high-level BIOS replacement (MPL-2.0): its IRQ dispatcher runs on every interrupt.
    hle = open(os.path.join(args.mgba, "src", "gba", "hle-bios.c"), encoding="utf-8").read()
    hle = hle[hle.index("{") + 1: hle.index("}")]
    bios = Space("bios", 0, bytes(int(x, 16) for x in re.findall(r"0x[0-9a-fA-F]{2}", hle)), True)  # guarded: a real BIOS may be loaded
    spaces = [rom, bios] + ram_spaces(ramcode)

    seeds = [("A", ROM_BASE), ("A", 0x18), ("A", 0x08)] + entries + pointer_scan(rom)
    code = discover(spaces, seeds)

    os.makedirs(args.out, exist_ok=True)
    with open(os.path.join(args.out, "ops_thumb.h"), "w", encoding="utf-8") as f:
        f.write(make_ops(args.mgba, "thumb"))
    with open(os.path.join(args.out, "ops_arm.h"), "w", encoding="utf-8") as f:
        f.write(make_ops(args.mgba, "arm"))

    chunks = []  # (space, mode, addrs, fname)
    for (sp, mode), addrs in sorted(code.insns.items(), key=lambda kv: (kv[0][0].base, kv[0][1])):
        for c in chunks_of(addrs, 2 if mode == "T" else 4):
            chunks.append((sp, mode, c, "rc_%s_%s%08X" % (sp.name, mode, c[0])))

    chunk_index = {c[3]: i for i, c in enumerate(chunks)}
    armfast.arm_name_of = arm_name
    fast_native = fast_total = 0

    # Balance chunks over shards by instruction count; Thumb and ARM never share a file.
    shards = [[] for _ in range(SHARDS)]
    load = [0] * SHARDS
    arm_shards = range(SHARDS - 4, SHARDS)
    thumb_shards = range(0, SHARDS - 4)
    for ch in sorted(chunks, key=lambda c: -len(c[2])):
        pool = thumb_shards if ch[1] == "T" else arm_shards
        i = min(pool, key=lambda k: load[k])
        shards[i].append(ch)
        load[i] += len(ch[2])

    for i, shard in enumerate(shards):
        ops = "ops_arm.h" if i in arm_shards else "ops_thumb.h"
        body = ['#include "rc_prelude.h"', '#include "%s"' % ops, ""]
        for sp, mode, addrs, fname in shard:
            # RC_CID: this chunk's index in recomp_chunks (per-chunk counters in RC_STATS builds).
            body.append("#define RC_CID %d" % chunk_index[fname])
            if mode == "A" and sp.ram and (sp.base >> 24) in (2, 3) and not os.environ.get("RECOMP_NO_ARMFAST"):
                src, n = armfast.emit(sp, addrs, fname, arm_flow)
                fast_native += n
                fast_total += len(addrs)
                body.append(src)
            else:
                body.append(emit_chunk(sp, mode, addrs, fname))
            body.append("#undef RC_CID")
        with open(os.path.join(args.out, "rc_%02d.c" % i), "w", encoding="utf-8") as f:
            f.write("\n".join(body))

    total = sum(len(c[2]) for c in chunks)
    tbl = ['#include "rc_prelude.h"', ""]
    for sp, mode, addrs, fname in chunks:
        tbl.append("int %s(struct ARMCore*, uint32_t);" % fname)
    guards = {}
    for sp in spaces:
        if sp.ram:
            tbl.append("static const uint8_t g_%s[%d] = {%s};" % (sp.name, len(sp.data), ",".join(str(b) for b in sp.data)))
            guards[sp.name] = sp
    tbl.append("const struct RecompChunk recomp_chunks[] = {")
    for sp, mode, addrs, fname in chunks:
        width = 2 if mode == "T" else 4
        if sp.ram:
            g = "g_%s + %d, %d" % (sp.name, addrs[0] - sp.base, addrs[-1] + width - addrs[0])
        else:
            g = "0, 0"
        tbl.append("\t{0x%08Xu, 0x%08Xu, %d, %s, %s}," % (addrs[0], addrs[-1] + width, 1 if mode == "T" else 0, fname, g))
    tbl.append("};")
    tbl.append("const int recomp_chunk_count = %d;" % len(chunks))
    tbl.append("const int recomp_insn_count = %d;" % total)
    tbl.append("const uint64_t recomp_rom_hash = 0x%016XULL;" % fnv64(data))
    with open(os.path.join(args.out, "rc_table.c"), "w", encoding="utf-8") as f:
        f.write("\n".join(tbl) + "\n")

    by = {}
    for sp, mode, addrs, _ in chunks:
        k = "%s/%s" % ("ram" if sp.ram else "rom", mode)
        by[k] = by.get(k, 0) + len(addrs)
    print("recomp: register-cached ARM: %d of %d instructions translated natively" % (fast_native, fast_total))
    print("recomp: %d instructions in %d chunks %s (rom sha1 %s)"
          % (total, len(chunks), by, hashlib.sha1(data).hexdigest()[:12]))


if __name__ == "__main__":
    main()
