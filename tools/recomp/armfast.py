"""Register-cached ARM code generator (phase 2b) for code that runs from IWRAM/EWRAM.

Inside a chunk, r0-r14, the NZCV flags and the cycle counter live in C locals (the host compiler
keeps them in registers). They are written back to the ARMCore only when mGBA must see them: before
an instruction this generator does not translate (it then runs mGBA's own handler), around memory
accesses outside IWRAM/EWRAM, on taken branches (mGBA's region/idle-loop logic reads the registers)
and whenever control leaves the chunk. Every translated instruction mirrors mGBA's isa-arm.c exactly,
flags and cycle counts included, so the per-frame state hash stays identical to the interpreter.
"""

COND = {
    0x0: "FZ", 0x1: "!FZ", 0x2: "FC", 0x3: "!FC", 0x4: "FN", 0x5: "!FN", 0x6: "FV", 0x7: "!FV",
    0x8: "(FC && !FZ)", 0x9: "(!FC || FZ)", 0xA: "(FN == FV)", 0xB: "(FN != FV)",
    0xC: "(!FZ && FN == FV)", 0xD: "(FZ || FN != FV)",
}


def R(n):
    return "R%d" % n


def shifter(op):
    """ALU operand 2 (addressing mode 1) -> (C statements, operand, carry, extra cycle)."""
    if op & 0x02000000:
        rotate = (op & 0xF00) >> 7
        imm = op & 0xFF
        if not rotate:
            return [], "0x%Xu" % imm, "FC", False
        val = ((imm >> rotate) | (imm << (32 - rotate))) & 0xFFFFFFFF
        return [], "0x%Xu" % val, str(val >> 31), False
    rm = op & 15
    kind = (op >> 5) & 3
    v = R(rm)
    if op & 0x10:  # shift by register: one extra internal cycle
        rs = (op >> 8) & 15
        s = ["uint32_t sv = %s, sh = %s & 0xFF, so, sc;" % (v, R(rs))]
        if kind == 0:
            s.append("if (!sh) { so = sv; sc = FC; } else if (sh < 32) { so = sv << sh; sc = (sv >> (32 - sh)) & 1; }"
                     " else if (sh == 32) { so = 0; sc = sv & 1; } else { so = 0; sc = 0; }")
        elif kind == 1:
            s.append("if (!sh) { so = sv; sc = FC; } else if (sh < 32) { so = sv >> sh; sc = (sv >> (sh - 1)) & 1; }"
                     " else if (sh == 32) { so = 0; sc = sv >> 31; } else { so = 0; sc = 0; }")
        elif kind == 2:
            s.append("if (!sh) { so = sv; sc = FC; } else if (sh < 32) { so = (uint32_t) ((int32_t) sv >> sh);"
                     " sc = ((int32_t) sv >> (sh - 1)) & 1; } else if (sv >> 31) { so = 0xFFFFFFFFu; sc = 1; }"
                     " else { so = 0; sc = 0; }")
        else:
            s.append("uint32_t rot = sh & 0x1F; if (!sh) { so = sv; sc = FC; } else if (rot) {"
                     " so = (sv >> rot) | (sv << (32 - rot)); sc = (sv >> (rot - 1)) & 1; }"
                     " else { so = sv; sc = sv >> 31; }")
        return s, "so", "sc", True
    imm = (op >> 7) & 31
    if kind == 0:
        if not imm:
            return [], v, "FC", False
        return [], "(%s << %d)" % (v, imm), "((%s >> %d) & 1)" % (v, 32 - imm), False
    if kind == 1:
        if not imm:
            return [], "0u", "(%s >> 31)" % v, False
        return [], "(%s >> %d)" % (v, imm), "((%s >> %d) & 1)" % (v, imm - 1), False
    if kind == 2:
        if not imm:
            return [], "(uint32_t) ((int32_t) %s >> 31)" % v, "(%s >> 31)" % v, False
        return ([], "(uint32_t) ((int32_t) %s >> %d)" % (v, imm),
                "(((int32_t) %s >> %d) & 1)" % (v, imm - 1), False)
    if not imm:  # RRX
        return [], "((FC << 31) | (%s >> 1))" % v, "(%s & 1)" % v, False
    return [], "((%s >> %d) | (%s << %d))" % (v, imm, v, 32 - imm), "((%s >> %d) & 1)" % (v, imm - 1), False


def alu(op):
    """Data processing without PC operands -> C body or None."""
    if (op & 0x0C000000) != 0 or (op & 0x0E000090) == 0x00000090:
        return None  # not data processing (multiply / halfword transfers share the encoding space)
    opc = (op >> 21) & 15
    s_bit = bool(op & 0x00100000)
    if opc in (8, 9, 10, 11) and not s_bit:
        return None  # MRS/MSR live here
    rn, rd = (op >> 16) & 15, (op >> 12) & 15
    regs = [rn, rd]
    if not op & 0x02000000:
        regs.append(op & 15)
        if op & 0x10:
            regs.append((op >> 8) & 15)
    if 15 in regs:
        return None
    pre, o2, sc, extra = shifter(op)
    n = R(rn)
    out = list(pre)
    out.append("uint32_t o2 = %s;" % o2)
    if extra:
        out.append("++CYC;")  # mGBA: ++cpu->cycles in the register-shift path
    logical = {0: "%s & o2", 1: "%s ^ o2", 12: "%s | o2", 13: "o2", 14: "%s & ~o2", 15: "~o2",
               8: "%s & o2", 9: "%s ^ o2"}
    if opc in logical:
        expr = logical[opc] % n if "%s" in logical[opc] else logical[opc]
        out.append("uint32_t d = %s;" % expr)
        if s_bit:
            out.append("FN = d >> 31; FZ = !d; FC = (%s) & 1;" % sc)
        if opc not in (8, 9):
            out.append("%s = d;" % R(rd))
        return out
    if opc in (4, 11):  # ADD, CMN
        out.append("uint32_t m = %s, d = m + o2;" % n)
        if s_bit:
            out.append("RCF_ADD_FLAGS(m, o2, d);")
    elif opc == 5:  # ADC (mGBA computes C/V from n, operand and result, ignoring the carry-in)
        out.append("uint32_t m = %s, d = m + o2 + FC;" % n)
        if s_bit:
            out.append("RCF_ADD_FLAGS(m, o2, d);")
    elif opc in (2, 10):  # SUB, CMP
        out.append("uint32_t m = %s, d = m - o2;" % n)
        if s_bit:
            out.append("RCF_SUB_FLAGS(m, o2, d);")
    elif opc == 3:  # RSB
        out.append("uint32_t m = %s, d = o2 - m;" % n)
        if s_bit:
            out.append("RCF_SUB_FLAGS(o2, m, d);")
    elif opc == 6:  # SBC
        out.append("uint32_t m = %s, b = !FC, d = m - o2 - b;" % n)
        if s_bit:
            out.append("RCF_SBC_FLAGS(m, o2, d, b);")
    elif opc == 7:  # RSC
        out.append("uint32_t m = %s, b = !FC, d = o2 - m - b;" % n)
        if s_bit:
            out.append("RCF_SBC_FLAGS(o2, m, d, b);")
    if opc not in (10, 11):
        out.append("%s = d;" % R(rd))
    return out


def multiply(op):
    """MUL/MLA/UMULL/UMLAL/SMULL/SMLAL without the S bit -> C body or None."""
    if (op & 0x0FC000F0) == 0x00000090:  # MUL / MLA
        if op & 0x00100000:
            return None  # MULS/MLAS set C from mGBA's stale shifter carry: leave them to mGBA
        rd, rn, rs, rm = (op >> 16) & 15, (op >> 12) & 15, (op >> 8) & 15, op & 15
        if 15 in (rd, rn, rs, rm):
            return None
        acc = bool(op & 0x00200000)
        wait = 1 if acc else 0
        out = ["CC += rc_stall(cpu, MEM, RCF_SMUL_WAIT(%s, %d));" % (R(rs), wait)]
        if acc:
            out.append("%s = %s * %s + %s;" % (R(rd), R(rm), R(rs), R(rn)))
        else:
            out.append("%s = %s * %s;" % (R(rd), R(rm), R(rs)))
        out.append("CC += NS;")
        return out
    if (op & 0x0F8000F0) == 0x00800090:  # long multiplies
        if op & 0x00100000:
            return None
        hi, lo, rs, rm = (op >> 16) & 15, (op >> 12) & 15, (op >> 8) & 15, op & 15
        if 15 in (hi, lo, rs, rm):
            return None
        signed, acc = bool(op & 0x00400000), bool(op & 0x00200000)
        wait = 2 if acc else 1
        w = "RCF_SMUL_WAIT" if signed else "RCF_UMUL_WAIT"
        out = ["CC += rc_stall(cpu, MEM, %s(%s, %d));" % (w, R(rs), wait)]
        if signed and acc:
            out.append("int64_t d = (int64_t) (int32_t) %s * (int64_t) (int32_t) %s + (uint32_t) %s;"
                       % (R(rm), R(rs), R(lo)))
            out.append("uint32_t dHi = %s + (uint32_t) (d >> 32); %s = (uint32_t) d; %s = dHi;" % (R(hi), R(lo), R(hi)))
        elif signed:
            out.append("int64_t d = (int64_t) (int32_t) %s * (int64_t) (int32_t) %s;" % (R(rm), R(rs)))
            out.append("%s = (uint32_t) d; %s = (uint32_t) (d >> 32);" % (R(lo), R(hi)))
        elif acc:
            out.append("uint64_t d = (uint64_t) %s * (uint64_t) %s + (uint32_t) %s;" % (R(rm), R(rs), R(lo)))
            out.append("uint32_t dHi = %s + (uint32_t) (d >> 32); %s = (uint32_t) d; %s = dHi;" % (R(hi), R(lo), R(hi)))
        else:
            out.append("uint64_t d = (uint64_t) %s * (uint64_t) %s;" % (R(rm), R(rs)))
            out.append("%s = (uint32_t) d; %s = (uint32_t) (d >> 32);" % (R(lo), R(hi)))
        out.append("CC += NS;")
        return out
    return None


def mode2_offset(op):
    """Addressing mode 2 register offset (ADDR_MODE_2_LSL/LSR/ASR/ROR)."""
    rm = op & 15
    imm = (op >> 7) & 31
    kind = (op >> 5) & 3
    v = R(rm)
    if kind == 0:
        return "(%s << %d)" % (v, imm)
    if kind == 1:
        return "(%s >> %d)" % (v, imm) if imm else "0u"
    if kind == 2:
        return "(uint32_t) ((int32_t) %s >> %d)" % (v, imm if imm else 31)
    if imm:
        return "((%s >> %d) | (%s << %d))" % (v, imm, v, 32 - imm)
    return "((FC << 31) | (%s >> 1))" % v


def load_store(op, pre_state):
    """Single data transfers (LDR/STR/LDRB/STRB/LDRH/STRH/LDRSB/LDRSH) -> C body or None."""
    p, u, w, l = bool(op & (1 << 24)), bool(op & (1 << 23)), bool(op & (1 << 21)), bool(op & (1 << 20))
    rn, rd = (op >> 16) & 15, (op >> 12) & 15
    if (op & 0x0C000000) == 0x04000000:  # mode 2
        if not p and w:
            return None  # LDRT/STRT
        byte = bool(op & (1 << 22))
        if op & 0x02000000:
            if (op & 0x10) or (op & 15) == 15:
                return None
            off = mode2_offset(op)
        else:
            off = "0x%Xu" % (op & 0xFFF)
        size = "8" if byte else "32"
        kind = "load" + size if l else "store" + size
        conv = ""
    elif (op & 0x0E000090) == 0x00000090 and (op & 0x60):  # mode 3 halfword / signed
        sh = (op >> 5) & 3
        if op & (1 << 22):
            off = "0x%Xu" % (((op & 0xF00) >> 4) | (op & 0xF))
        else:
            if (op & 15) == 15 or (op & 0xF00):
                return None
            off = R(op & 15)
        if not p and w:
            return None
        if l:
            kind, conv = {1: ("load16", ""), 2: ("load8", "s8"), 3: ("load16", "s16")}[sh]
        else:
            if sh != 1:
                return None
            kind, conv = "store16", ""
    else:
        return None
    if 15 in (rn, rd):
        return None
    sign = "+" if u else "-"
    out = ["uint32_t base = %s;" % R(rn)]
    if p:
        out.append("uint32_t a = base %s %s;" % (sign, off))
        wb = "a" if w else None
    else:
        out.append("uint32_t a = base;")
        wb = "base %s %s" % (sign, off)
    if l:
        if wb:
            out.append("%s = %s;" % (R(rn), wb))  # writeback happens before the load (mGBA order)
        if conv == "s16":
            # ARM_SXT_8 for odd addresses, ARM_SXT_16 otherwise
            out.append("uint32_t v; RCF_MEM(v = rc_load16(cpu, a, &CC), v = cpu->memory.load16(cpu, a, &CC), %s);" % pre_state)
            out.append("%s = (a & 1) ? (uint32_t) (int32_t) (int8_t) v : (uint32_t) (int32_t) (int16_t) v;" % R(rd))
        elif conv == "s8":
            out.append("uint32_t v; RCF_MEM(v = rc_load8(cpu, a, &CC), v = cpu->memory.load8(cpu, a, &CC), %s);" % pre_state)
            out.append("%s = (uint32_t) (int32_t) (int8_t) v;" % R(rd))
        else:
            out.append("uint32_t v; RCF_MEM(v = rc_%s(cpu, a, &CC), v = cpu->memory.%s(cpu, a, &CC), %s);"
                       % (kind, kind, pre_state))
            out.append("%s = v;" % R(rd))
    else:
        cast = {"store32": "int32_t", "store16": "int16_t", "store8": "int8_t"}[kind]
        out.append("%s sd = (%s) %s;" % (cast, cast, R(rd)))
        out.append("RCF_MEM(rc_%s(cpu, a, sd, &CC), cpu->memory.%s(cpu, a, sd, &CC), %s);" % (kind, kind, pre_state))
        if wb:
            out.append("%s = %s;" % (R(rn), wb))
    out.append("CC += NS;")
    return out


def emit(sp, addrs, fname, arm_flow):
    aset = set(addrs)
    out = ["int %s(struct ARMCore* cpu, uint32_t pc) {" % fname, "\tRCF_DECL", "\tRCF_IN", "\tswitch (pc) {"]
    for a in addrs:
        out.append("\tcase 0x%08Xu: goto L_%08X;" % (a, a))
    out.append("\tdefault: return 0;\n\t}")
    native = 0
    for a in addrs:
        op = sp.u32(a)
        pc = a + 8

        def pf(x):
            return "0x%X" % sp.u32(x) if sp.has(x, 4) else "0"
        pre = "0x%08Xu, %s, %s" % (pc, pf(a + 4), pf(a + 8))
        cond = op >> 28
        out.append("L_%08X:" % a)
        if cond == 0xF:
            out.append("\tCYC += PF;")
            out.append("\tRCF_EVENT(%s);" % pre)
            native += 1
            continue
        body = None
        branch = None
        if (op & 0x0E000000) == 0x0A000000:  # B / BL
            off = ((op & 0xFFFFFF) ^ 0x800000) - 0x800000
            branch = (a + 8 + off * 4) & 0xFFFFFFFF
        else:
            body = alu(op) or multiply(op) or load_store(op, pre)
        guard = "" if cond == 0xE else "if (%s) " % COND[cond]
        if branch is not None:
            native += 1
            link = "R14 = 0x%08Xu; " % (a + 4) if op & 0x01000000 else ""
            local = branch in aset
            out.append("\t%s{" % guard)
            out.append("\t\t%sCC = PF; RCF_OUT_REGS cpu->gprs[ARM_PC] = (int32_t) 0x%08Xu; CC += ARMWritePC(cpu);"
                       % (link, branch))
            out.append("\t\tRCF_REFRESH CYC += CC;")
            if local:
                out.append("\t\tif (CYC < NEV) goto L_%08X;" % branch)
            out.append("\t\tcpu->cycles = CYC; return 1;")
            out.append("\t}" + (" else CYC += PF;" if guard else ""))
            out.append("\tRCF_EVENT(%s);" % pre)
            continue
        if body is not None:
            native += 1
            out.append("\t%s{" % guard)
            out.append("\t\tCC = PF;")
            for line in body:
                out.append("\t\t" + line)
            out.append("\t\tCYC += CC;")
            out.append("\t}" + (" else CYC += PF;" if guard else ""))
            out.append("\tRCF_EVENT(%s);" % pre)
            continue
        # Not translated: run mGBA's handler on the synced ARMCore.
        name = arm_name_of(op)
        out.append("\tRCF_OUT RC_PRE(%s);" % pre)
        if cond == 0xE:
            out.append("\tA_%s(cpu, 0x%08Xu);" % (name, op))
        else:
            out.append("\tif (ARMTestCondition(cpu, %d)) A_%s(cpu, 0x%08Xu); else cpu->cycles += ARM_PREFETCH_CYCLES;"
                       % (cond, name, op))
        out.append("\tRCF_IN")
        _, jumps, _, _ = arm_flow(sp, a)
        local = [t for t in jumps if t in aset]
        if local:
            out.append("\tRC_BRANCH_ARM(0x%08Xu, 0x%08Xu, L_%08X);" % (pc, local[0] + 4, local[0]))
        else:
            out.append("\tRC_EXIT_IF_PC_ARM(0x%08Xu);" % pc)
        out.append("\tif (RC_UNLIKELY(CYC >= NEV)) return 1;")
    out.append("\tRCF_OUT RC_PRE(%s);\n\treturn 1;\n}\n" % pre)
    return "\n".join(out), native


arm_name_of = None  # set by recomp.py (avoids a circular import)
