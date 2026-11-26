#!/usr/bin/env python3
"""Static x86-32 ELF -> Portable C99 lifter for Psychonauts (Linux Steam build)."""

import argparse
import collections
import os
import re
import struct
import sys
from pathlib import Path
from elftools.elf.elffile import ELFFile
from capstone import Cs, CS_ARCH_X86, CS_MODE_32, CS_OP_REG, CS_OP_IMM, CS_OP_MEM
from capstone.x86 import (
    X86_REG_AL, X86_REG_AH, X86_REG_AX, X86_REG_EAX,
    X86_REG_BL, X86_REG_BH, X86_REG_BX, X86_REG_EBX,
    X86_REG_CL, X86_REG_CH, X86_REG_CX, X86_REG_ECX,
    X86_REG_DL, X86_REG_DH, X86_REG_DX, X86_REG_EDX,
    X86_REG_SIL, X86_REG_SI, X86_REG_ESI,
    X86_REG_DIL, X86_REG_DI, X86_REG_EDI,
    X86_REG_BPL, X86_REG_BP, X86_REG_EBP,
    X86_REG_SPL, X86_REG_SP, X86_REG_ESP,
    X86_REG_XMM0, X86_REG_XMM1, X86_REG_XMM2, X86_REG_XMM3,
    X86_REG_XMM4, X86_REG_XMM5, X86_REG_XMM6, X86_REG_XMM7,
    X86_REG_ST0, X86_REG_ST1, X86_REG_ST2, X86_REG_ST3,
    X86_REG_ST4, X86_REG_ST5, X86_REG_ST6, X86_REG_ST7,
)

PROJECT_ROOT = Path(__file__).resolve().parents[2]
NUM_SHARDS = 64

ENGINE_START = 0x081109D0
ENGINE_END = 0x0852A970
CORO_START = 0x084BDCA2
CORO_END = 0x084BDDE9
SETJMP_PLT = 0x0810FF9C

HLE_EXTRA = {
    0x0852B560: "atexit",
    0x0852B5A0: "__stat",
    0x0852B5E0: "__fstat",
    0x0852B6E0: "BinkSetSoundSystem",
    0x0852BC20: "BinkSetSoundTrack",
    0x0852BF30: "BinkOpen",
    0x0852D6B0: "BinkWait",
    0x0852E3E0: "BinkSetVolume",
    0x0852E580: "BinkGetTrackID",
    0x0852E9C0: "BinkCopyToBuffer",
    0x0852EA00: "BinkDoFrame",
    0x0852F040: "BinkNextFrame",
    0x0852F3D0: "BinkClose",
    0x081e47b2: "DFXInputSetState",
    0x081e47b7: "XInputControllerExists",
}

REG32_MAP = {
    X86_REG_EAX: "eax", X86_REG_EBX: "ebx", X86_REG_ECX: "ecx", X86_REG_EDX: "edx",
    X86_REG_ESI: "esi", X86_REG_EDI: "edi", X86_REG_EBP: "ebp", X86_REG_ESP: "esp",
}
REG16_MAP = {
    X86_REG_AX: "eax", X86_REG_BX: "ebx", X86_REG_CX: "ecx", X86_REG_DX: "edx",
    X86_REG_SI: "esi", X86_REG_DI: "edi", X86_REG_BP: "ebp", X86_REG_SP: "esp",
}
REG8L_MAP = {
    X86_REG_AL: "eax", X86_REG_BL: "ebx", X86_REG_CL: "ecx", X86_REG_DL: "edx",
    X86_REG_SIL: "esi", X86_REG_DIL: "edi", X86_REG_BPL: "ebp", X86_REG_SPL: "esp",
}
REG8H_MAP = {
    X86_REG_AH: "eax", X86_REG_BH: "ebx", X86_REG_CH: "ecx", X86_REG_DH: "edx",
}
XMM_MAP = {
    X86_REG_XMM0: 0, X86_REG_XMM1: 1, X86_REG_XMM2: 2, X86_REG_XMM3: 3,
    X86_REG_XMM4: 4, X86_REG_XMM5: 5, X86_REG_XMM6: 6, X86_REG_XMM7: 7,
}
ST_MAP = {
    X86_REG_ST0: 0, X86_REG_ST1: 1, X86_REG_ST2: 2, X86_REG_ST3: 3,
    X86_REG_ST4: 4, X86_REG_ST5: 5, X86_REG_ST6: 6, X86_REG_ST7: 7,
}

CF, ZF, SF, OF, PF = 1, 2, 4, 8, 16
ALL_FLAGS = CF | ZF | SF | OF | PF

JCC_READS = {
    "je": ZF, "jne": ZF,
    "jb": CF, "jae": CF,
    "jbe": CF | ZF, "ja": CF | ZF,
    "js": SF, "jns": SF,
    "jp": PF, "jnp": PF,
    "jl": SF | OF, "jge": SF | OF,
    "jle": ZF | SF | OF, "jg": ZF | SF | OF,
}

def cond_expr(cc):
    return {
        "e": "zf", "ne": "!zf",
        "b": "cf", "ae": "!cf",
        "be": "(cf || zf)", "a": "(!cf && !zf)",
        "s": "sf", "ns": "!sf",
        "p": "pf", "np": "!pf",
        "l": "(sf != of)", "ge": "(sf == of)",
        "le": "(zf || (sf != of))", "g": "(!zf && (sf == of))",
    }[cc]


def sanitize_c_ident(name):
    s = re.sub(r'[^A-Za-z0-9_]', '_', name)
    if not s or s[0].isdigit():
        s = "_" + s
    return s


def op_mem_addr(op):
    parts = []
    if op.mem.base != 0:
        parts.append(f"ctx->{REG32_MAP[op.mem.base]}")
    if op.mem.index != 0:
        idx = f"ctx->{REG32_MAP[op.mem.index]}"
        if op.mem.scale != 1:
            parts.append(f"({idx} * {op.mem.scale}u)")
        else:
            parts.append(idx)
    disp = op.mem.disp & 0xFFFFFFFF
    if disp != 0 or not parts:
        parts.append(f"0x{disp:x}u")
    return "(uint32_t)(" + " + ".join(parts) + ")"


def read_op(op, signed=False):
    sz = op.size
    if op.type == CS_OP_IMM:
        u = op.imm & ((1 << (sz * 8)) - 1) if sz in (1, 2, 4) else (op.imm & 0xFFFFFFFF)
        if signed:
            bits = sz * 8
            s = u - (1 << bits) if (u & (1 << (bits - 1))) else u
            return f"((int{bits}_t)({s}))"
        return f"0x{u:x}u"
    if op.type == CS_OP_REG:
        r = op.reg
        if r in REG32_MAP:
            expr = f"ctx->{REG32_MAP[r]}"
            return f"(int32_t)({expr})" if signed else expr
        if r in REG16_MAP:
            expr = f"reg_r16(ctx->{REG16_MAP[r]})"
            return f"(int16_t)({expr})" if signed else expr
        if r in REG8L_MAP:
            expr = f"reg_r8l(ctx->{REG8L_MAP[r]})"
            return f"(int8_t)({expr})" if signed else expr
        if r in REG8H_MAP:
            expr = f"reg_r8h(ctx->{REG8H_MAP[r]})"
            return f"(int8_t)({expr})" if signed else expr
        raise ValueError(f"Unsupported reg read: {r}")
    if op.type == CS_OP_MEM:
        addr = op_mem_addr(op)
        if sz == 1:
            e = f"mem_r8({addr})"
            return f"(int8_t){e}" if signed else e
        if sz == 2:
            e = f"mem_r16({addr})"
            return f"(int16_t){e}" if signed else e
        if sz == 4:
            e = f"mem_r32({addr})"
            return f"(int32_t){e}" if signed else e
        if sz == 8:
            e = f"mem_r64({addr})"
            return f"(int64_t){e}" if signed else e
        raise ValueError(f"Unsupported mem read size {sz}")
    raise ValueError(f"Unsupported op type {op.type}")


def write_op(op, val_expr, addr_var=None):
    sz = op.size
    if op.type == CS_OP_REG:
        r = op.reg
        if r in REG32_MAP:
            return f"ctx->{REG32_MAP[r]} = (uint32_t)({val_expr});"
        if r in REG16_MAP:
            return f"reg_w16(&ctx->{REG16_MAP[r]}, (uint16_t)({val_expr}));"
        if r in REG8L_MAP:
            return f"reg_w8l(&ctx->{REG8L_MAP[r]}, (uint8_t)({val_expr}));"
        if r in REG8H_MAP:
            return f"reg_w8h(&ctx->{REG8H_MAP[r]}, (uint8_t)({val_expr}));"
        raise ValueError(f"Unsupported reg write: {r}")
    if op.type == CS_OP_MEM:
        addr = addr_var if addr_var else op_mem_addr(op)
        if sz == 1:
            return f"mem_w8({addr}, (uint8_t)({val_expr}));"
        if sz == 2:
            return f"mem_w16({addr}, (uint16_t)({val_expr}));"
        if sz == 4:
            return f"mem_w32({addr}, (uint32_t)({val_expr}));"
        if sz == 8:
            return f"mem_w64({addr}, (uint64_t)({val_expr}));"
    raise ValueError(f"Unsupported write op type {op.type} sz {sz}")


def flag_effect(ins):
    m = ins.mnemonic
    if m in ("cmp", "sub"):
        return 0, ALL_FLAGS
    if m == "sbb":
        return CF, ALL_FLAGS
    if m == "add":
        return 0, ALL_FLAGS
    if m == "adc":
        return CF, ALL_FLAGS
    if m == "neg":
        return 0, ALL_FLAGS
    if m in ("inc", "dec"):
        return 0, ZF | SF | OF | PF
    if m in ("test", "and", "or", "xor"):
        return 0, ALL_FLAGS
    if m in ("shl", "shr", "sar", "shld", "shrd", "ror"):
        return 0, ALL_FLAGS
    if m in ("bt", "bts"):
        return 0, CF
    if m in ("fucomi", "fucomip"):
        return 0, ZF | PF | CF
    if m == "sahf":
        return 0, SF | ZF | PF | CF
    if m == "call":
        return 0, ALL_FLAGS
    if m.startswith("j") and m != "jmp":
        return JCC_READS[m], 0
    if m.startswith("set"):
        cc = m[3:]
        return JCC_READS["j" + cc], 0
    if m.startswith("cmov"):
        cc = m[4:]
        return JCC_READS["j" + cc], 0
    return 0, 0


def emit_flags_add(lines, sz, a, b, res, need):
    bits = sz * 8
    mask = (1 << bits) - 1
    sign = 1 << (bits - 1)
    if need & CF:
        lines.append(f"cf = (uint8_t)(((uint64_t)({a}) + (uint64_t)({b})) >> {bits});")
    if need & ZF:
        lines.append(f"zf = (({res}) == 0);")
    if need & SF:
        lines.append(f"sf = ((({res}) & 0x{sign:x}u) != 0);")
    if need & OF:
        lines.append(f"of = ((((({a}) ^ ({res})) & (({b}) ^ ({res}))) & 0x{sign:x}u) != 0);")
    if need & PF:
        lines.append(f"pf = recomp_parity((uint8_t)({res}));")


def emit_flags_adc(lines, sz, a, b, cf_in, res, need):
    bits = sz * 8
    sign = 1 << (bits - 1)
    if need & CF:
        lines.append(f"cf = (uint8_t)(((uint64_t)({a}) + (uint64_t)({b}) + (uint64_t)({cf_in})) >> {bits});")
    if need & ZF:
        lines.append(f"zf = (({res}) == 0);")
    if need & SF:
        lines.append(f"sf = ((({res}) & 0x{sign:x}u) != 0);")
    if need & OF:
        lines.append(f"of = ((((({a}) ^ ({res})) & (({b}) ^ ({res}))) & 0x{sign:x}u) != 0);")
    if need & PF:
        lines.append(f"pf = recomp_parity((uint8_t)({res}));")


def emit_flags_sub(lines, sz, a, b, res, need):
    bits = sz * 8
    sign = 1 << (bits - 1)
    if need & CF:
        lines.append(f"cf = (({a}) < ({b}));")
    if need & ZF:
        lines.append(f"zf = (({res}) == 0);")
    if need & SF:
        lines.append(f"sf = ((({res}) & 0x{sign:x}u) != 0);")
    if need & OF:
        lines.append(f"of = ((((({a}) ^ ({b})) & (({a}) ^ ({res}))) & 0x{sign:x}u) != 0);")
    if need & PF:
        lines.append(f"pf = recomp_parity((uint8_t)({res}));")


def emit_flags_sbb(lines, sz, a, b, cf_in, res, need):
    bits = sz * 8
    sign = 1 << (bits - 1)
    if need & CF:
        lines.append(f"cf = ((uint64_t)({a}) < ((uint64_t)({b}) + (uint64_t)({cf_in})));")
    if need & ZF:
        lines.append(f"zf = (({res}) == 0);")
    if need & SF:
        lines.append(f"sf = ((({res}) & 0x{sign:x}u) != 0);")
    if need & OF:
        lines.append(f"of = ((((({a}) ^ ({b})) & (({a}) ^ ({res}))) & 0x{sign:x}u) != 0);")
    if need & PF:
        lines.append(f"pf = recomp_parity((uint8_t)({res}));")


def emit_flags_logic(lines, sz, res, need):
    bits = sz * 8
    sign = 1 << (bits - 1)
    if need & CF:
        lines.append("cf = 0;")
    if need & OF:
        lines.append("of = 0;")
    if need & ZF:
        lines.append(f"zf = (({res}) == 0);")
    if need & SF:
        lines.append(f"sf = ((({res}) & 0x{sign:x}u) != 0);")
    if need & PF:
        lines.append(f"pf = recomp_parity((uint8_t)({res}));")


def uint_t(sz):
    return {1: "uint8_t", 2: "uint16_t", 4: "uint32_t", 8: "uint64_t"}[sz]


def sint_t(sz):
    return {1: "int8_t", 2: "int16_t", 4: "int32_t", 8: "int64_t"}[sz]


def resolve_switch_tables(insns, fn_start, fn_end, valid_insn_addrs, rdata, rbase):
    rend = rbase + len(rdata)
    tables = {}
    for idx, ins in enumerate(insns):
        if ins.mnemonic == "jmp" and ins.operands[0].type == CS_OP_REG:
            disp = None
            for back in range(1, min(10, idx + 1)):
                prev = insns[idx - back]
                if prev.mnemonic == "mov" and len(prev.operands) == 2 and prev.operands[1].type == CS_OP_MEM:
                    d = prev.operands[1].mem.disp & 0xFFFFFFFF
                    if rbase <= d < rend:
                        disp = d
                        break
            if disp is None:
                raise RuntimeError(f"Could not find switch table disp for jmp {ins.op_str} at 0x{ins.address:08x}")
            off = disp - rbase
            targets = []
            while off + 4 <= len(rdata):
                w = struct.unpack_from("<I", rdata, off)[0]
                if w in valid_insn_addrs:
                    targets.append(w)
                    off += 4
                else:
                    break
            if len(targets) < 2:
                raise RuntimeError(f"Switch table at 0x{disp:08x} for 0x{ins.address:08x} yielded only {len(targets)} targets")
            tables[ins.address] = sorted(set(targets))
    return tables


def compute_flag_liveness(insns, switch_tables, fn_start, fn_end):
    n = len(insns)
    addr_to_idx = {ins.address: i for i, ins in enumerate(insns)}
    reads = [0] * n
    writes = [0] * n
    succs = [[] for _ in range(n)]

    for i, ins in enumerate(insns):
        r, w = flag_effect(ins)
        reads[i] = r
        writes[i] = w
        m = ins.mnemonic
        if m == "ret" or m == "int3":
            pass
        elif m == "jmp":
            op0 = ins.operands[0]
            if op0.type == CS_OP_IMM:
                tgt = op0.imm & 0xFFFFFFFF
                if tgt in addr_to_idx:
                    succs[i].append(addr_to_idx[tgt])
            elif ins.address in switch_tables:
                for tgt in switch_tables[ins.address]:
                    succs[i].append(addr_to_idx[tgt])
        elif m.startswith("j"):
            tgt = ins.operands[0].imm & 0xFFFFFFFF
            if tgt in addr_to_idx:
                succs[i].append(addr_to_idx[tgt])
            if i + 1 < n:
                succs[i].append(i + 1)
        else:
            if i + 1 < n:
                succs[i].append(i + 1)

    live_in = [0] * n
    changed = True
    while changed:
        changed = False
        for i in range(n - 1, -1, -1):
            out_f = 0
            for s in succs[i]:
                out_f |= live_in[s]
            in_f = (out_f & ~writes[i]) | reads[i]
            if in_f != live_in[i]:
                live_in[i] = in_f
                changed = True

    live_after = [0] * n
    for i in range(n):
        out_f = 0
        for s in succs[i]:
            out_f |= live_in[s]
        live_after[i] = out_f
    return live_after


def emit_insn(ins, need, fn_start, fn_end, switch_tables, label_targets, plt_map, engine_funcs, out):
    m = ins.mnemonic
    ops = ins.operands
    addr = ins.address

    if addr in label_targets:
        out.append(f"L_{addr:08x}:;")

    # Special hook inside EScriptVM::RunScript for coroutine stack switch
    if addr == CORO_START and fn_start == 0x084BD82E:
        out.append(
            "  recomp_escript_run_coro(ctx, mem_r32(ctx->ebp - 0x14u), mem_r32(ctx->ebp - 0x1cu), fn_084bdca2_coro);"
        )
        out.append("  return;")
        return

    if m == "nop" or ins.mnemonic == "cld":
        return

    if m == "int3":
        out.append(f"  recomp_int3(ctx, 0x{addr:08x}u);")
        return

    if m == "mov":
        dst, src = ops[0], ops[1]
        if dst.type == CS_OP_REG and src.type == CS_OP_REG and dst.reg == src.reg:
            return
        out.append(f"  {write_op(dst, read_op(src))}")
        return

    if m == "movzx":
        dst, src = ops[0], ops[1]
        val = f"(uint32_t)({read_op(src, signed=False)})"
        out.append(f"  {write_op(dst, val)}")
        return

    if m == "movsx":
        dst, src = ops[0], ops[1]
        val = f"({sint_t(dst.size)})({read_op(src, signed=True)})"
        out.append(f"  {write_op(dst, f'({uint_t(dst.size)})({val})')}")
        return

    if m == "lea":
        dst, src = ops[0], ops[1]
        out.append(f"  {write_op(dst, op_mem_addr(src))}")
        return

    if m == "cwde":
        out.append("  ctx->eax = (uint32_t)(int32_t)(int16_t)reg_r16(ctx->eax);")
        return

    if m == "cdq":
        out.append("  ctx->edx = ((int32_t)ctx->eax < 0) ? 0xFFFFFFFFu : 0u;")
        return

    if m == "push":
        out.append(f"  push32(ctx, (uint32_t)({read_op(ops[0])}));")
        return

    if m == "pop":
        if ops[0].type == CS_OP_REG:
            out.append(f"  {write_op(ops[0], 'pop32(ctx)')}")
        else:
            out.append("  { uint32_t _pv = pop32(ctx);")
            out.append(f"    {write_op(ops[0], '_pv')} }}")
        return

    if m == "leave":
        out.append("  ctx->esp = ctx->ebp;")
        out.append("  ctx->ebp = pop32(ctx);")
        return

    if m == "ret":
        out.append("  ctx->eip = pop32(ctx);")
        if len(ops) == 1:
            imm = ops[0].imm & 0xFFFF
            out.append(f"  ctx->esp += 0x{imm:x}u;")
        out.append("  return;")
        return

    if m == "sahf":
        out.append("  { uint8_t _ah = reg_r8h(ctx->eax);")
        if need & SF: out.append("    sf = (_ah >> 7) & 1;")
        if need & ZF: out.append("    zf = (_ah >> 6) & 1;")
        if need & PF: out.append("    pf = (_ah >> 2) & 1;")
        if need & CF: out.append("    cf = _ah & 1;")
        out.append("  }")
        return

    # LOCK ADD support
    if m == "add" and ins.prefix[0] == 0xF0:
        dst, src = ops[0], ops[1]
        out.append(f"  {{ uint32_t _a = {op_mem_addr(dst)}; uint32_t _s = {read_op(src)};")
        out.append("    uint32_t _old = __atomic_fetch_add((uint32_t*)(g_mem_base + _a), _s, __ATOMIC_SEQ_CST);")
        if need:
            out.append("    uint32_t _r = _old + _s;")
            emit_flags_add(out, 4, "_old", "_s", "_r", need)
        out.append("  }")
        return

    if m in ("add", "adc", "sub", "sbb", "and", "or", "xor"):
        dst, src = ops[0], ops[1]
        sz = dst.size
        ut = uint_t(sz)
        lines = []
        if dst.type == CS_OP_MEM:
            lines.append(f"  {{ uint32_t _da = {op_mem_addr(dst)};")
            a_expr = {1: "mem_r8(_da)", 2: "mem_r16(_da)", 4: "mem_r32(_da)"}[sz]
            lines.append(f"    {ut} _a = {a_expr}; {ut} _b = ({ut})({read_op(src)});")
            addr_override = "_da"
        else:
            lines.append(f"  {{ {ut} _a = ({ut})({read_op(dst)}); {ut} _b = ({ut})({read_op(src)});")
            addr_override = None

        if m == "add":
            lines.append(f"    {ut} _r = ({ut})(_a + _b);")
            if need: emit_flags_add(lines, sz, "_a", "_b", "_r", need)
        elif m == "adc":
            lines.append(f"    uint8_t _cin = cf; {ut} _r = ({ut})(_a + _b + _cin);")
            if need: emit_flags_adc(lines, sz, "_a", "_b", "_cin", "_r", need)
        elif m == "sub":
            lines.append(f"    {ut} _r = ({ut})(_a - _b);")
            if need: emit_flags_sub(lines, sz, "_a", "_b", "_r", need)
        elif m == "sbb":
            lines.append(f"    uint8_t _cin = cf; {ut} _r = ({ut})(_a - _b - _cin);")
            if need: emit_flags_sbb(lines, sz, "_a", "_b", "_cin", "_r", need)
        elif m == "and":
            lines.append(f"    {ut} _r = ({ut})(_a & _b);")
            if need: emit_flags_logic(lines, sz, "_r", need)
        elif m == "or":
            lines.append(f"    {ut} _r = ({ut})(_a | _b);")
            if need: emit_flags_logic(lines, sz, "_r", need)
        elif m == "xor":
            lines.append(f"    {ut} _r = ({ut})(_a ^ _b);")
            if need: emit_flags_logic(lines, sz, "_r", need)

        lines.append(f"    {write_op(dst, '_r', addr_override)}")
        lines.append("  }")
        out.extend(lines)
        return

    if m in ("cmp", "test"):
        if not need:
            return
        op0, op1 = ops[0], ops[1]
        sz = op0.size
        ut = uint_t(sz)
        lines = [f"  {{ {ut} _a = ({ut})({read_op(op0)}); {ut} _b = ({ut})({read_op(op1)});"]
        if m == "cmp":
            lines.append(f"    {ut} _r = ({ut})(_a - _b);")
            emit_flags_sub(lines, sz, "_a", "_b", "_r", need)
        else:
            lines.append(f"    {ut} _r = ({ut})(_a & _b);")
            emit_flags_logic(lines, sz, "_r", need)
        lines.append("  }")
        out.extend(lines)
        return

    if m in ("inc", "dec", "neg", "not"):
        dst = ops[0]
        sz = dst.size
        ut = uint_t(sz)
        sign = 1 << (sz * 8 - 1)
        lines = []
        if dst.type == CS_OP_MEM:
            lines.append(f"  {{ uint32_t _da = {op_mem_addr(dst)};")
            a_expr = {1: "mem_r8(_da)", 2: "mem_r16(_da)", 4: "mem_r32(_da)"}[sz]
            lines.append(f"    {ut} _a = {a_expr};")
            addr_override = "_da"
        else:
            lines.append(f"  {{ {ut} _a = ({ut})({read_op(dst)});")
            addr_override = None

        if m == "inc":
            lines.append(f"    {ut} _r = ({ut})(_a + 1u);")
            if need & ZF: lines.append("    zf = (_r == 0);")
            if need & SF: lines.append(f"    sf = ((_r & 0x{sign:x}u) != 0);")
            if need & OF: lines.append(f"    of = (_r == 0x{sign:x}u);")
            if need & PF: lines.append("    pf = recomp_parity((uint8_t)_r);")
        elif m == "dec":
            lines.append(f"    {ut} _r = ({ut})(_a - 1u);")
            if need & ZF: lines.append("    zf = (_r == 0);")
            if need & SF: lines.append(f"    sf = ((_r & 0x{sign:x}u) != 0);")
            if need & OF: lines.append(f"    of = (_a == 0x{sign:x}u);")
            if need & PF: lines.append("    pf = recomp_parity((uint8_t)_r);")
        elif m == "neg":
            lines.append(f"    {ut} _r = ({ut})(0u - _a);")
            if need & CF: lines.append("    cf = (_a != 0);")
            if need & ZF: lines.append("    zf = (_r == 0);")
            if need & SF: lines.append(f"    sf = ((_r & 0x{sign:x}u) != 0);")
            if need & OF: lines.append(f"    of = (_a == 0x{sign:x}u);")
            if need & PF: lines.append("    pf = recomp_parity((uint8_t)_r);")
        else:
            lines.append(f"    {ut} _r = ({ut})(~_a);")

        lines.append(f"    {write_op(dst, '_r', addr_override)}")
        lines.append("  }")
        out.extend(lines)
        return

    if m in ("shl", "shr", "sar", "ror"):
        dst, cnt_op = ops[0], ops[1]
        sz = dst.size
        bits = sz * 8
        ut = uint_t(sz)
        st = sint_t(sz)
        sign = 1 << (bits - 1)
        lines = []
        if dst.type == CS_OP_MEM:
            lines.append(f"  {{ uint32_t _da = {op_mem_addr(dst)};")
            a_expr = {1: "mem_r8(_da)", 2: "mem_r16(_da)", 4: "mem_r32(_da)"}[sz]
            lines.append(f"    {ut} _a = {a_expr}; uint32_t _c = ({read_op(cnt_op)}) & 31u;")
            addr_override = "_da"
        else:
            lines.append(f"  {{ {ut} _a = ({ut})({read_op(dst)}); uint32_t _c = ({read_op(cnt_op)}) & 31u;")
            addr_override = None

        if m == "shl":
            lines.append(f"    {ut} _r = (_c < {bits}u) ? ({ut})((uint32_t)_a << _c) : 0;")
            if need & CF: lines.append(f"    if (_c > 0 && _c <= {bits}u) cf = (uint8_t)((_a >> ({bits}u - _c)) & 1u);")
            if need & OF: lines.append(f"    if (_c == 1u) of = (uint8_t)(((_r >> ({bits}-1)) ^ cf) & 1u);")
        elif m == "shr":
            lines.append(f"    {ut} _r = (_c < {bits}u) ? ({ut})((uint32_t)_a >> _c) : 0;")
            if need & CF: lines.append(f"    if (_c > 0 && _c <= {bits}u) cf = (uint8_t)((_a >> (_c - 1u)) & 1u);")
            if need & OF: lines.append(f"    if (_c == 1u) of = (uint8_t)((_a >> ({bits}-1)) & 1u);")
        elif m == "sar":
            lines.append(f"    {st} _sa = ({st})_a; {ut} _r = (_c < {bits}u) ? ({ut})(_sa >> _c) : ({ut})(_sa < 0 ? -1 : 0);")
            if need & CF: lines.append(f"    if (_c > 0) cf = (_c <= {bits}u) ? (uint8_t)((_sa >> (_c - 1u)) & 1) : (uint8_t)(_sa < 0);")
            if need & OF: lines.append("    if (_c == 1u) of = 0;")
        else:  # ror
            lines.append(f"    {ut} _rc = _c % {bits}u; {ut} _r = _rc ? ({ut})((_a >> _rc) | (_a << ({bits}u - _rc))) : _a;")
            if need & CF: lines.append(f"    if (_c > 0) cf = (uint8_t)((_r >> ({bits}-1)) & 1u);")
            if need & OF: lines.append(f"    if (_c == 1u) of = (uint8_t)(((_r >> ({bits}-1)) ^ (_r >> ({bits}-2))) & 1u);")

        if m != "ror":
            if need & ZF: lines.append("    if (_c) zf = (_r == 0);")
            if need & SF: lines.append(f"    if (_c) sf = ((_r & 0x{sign:x}u) != 0);")
            if need & PF: lines.append("    if (_c) pf = recomp_parity((uint8_t)_r);")

        lines.append(f"    {write_op(dst, '_r', addr_override)}")
        lines.append("  }")
        out.extend(lines)
        return

    if m in ("shld", "shrd"):
        dst, src, cnt_op = ops[0], ops[1], ops[2]
        lines = []
        if dst.type == CS_OP_MEM:
            lines.append(f"  {{ uint32_t _da = {op_mem_addr(dst)}; uint32_t _a = mem_r32(_da);")
            addr_override = "_da"
        else:
            lines.append(f"  {{ uint32_t _a = {read_op(dst)};")
            addr_override = None
        lines.append(f"    uint32_t _b = {read_op(src)}; uint32_t _c = ({read_op(cnt_op)}) & 31u;")
        if m == "shld":
            lines.append("    uint32_t _r = _c ? ((_a << _c) | (_b >> (32u - _c))) : _a;")
            if need & CF: lines.append("    if (_c) cf = (uint8_t)((_a >> (32u - _c)) & 1u);")
        else:
            lines.append("    uint32_t _r = _c ? ((_a >> _c) | (_b << (32u - _c))) : _a;")
            if need & CF: lines.append("    if (_c) cf = (uint8_t)((_a >> (_c - 1u)) & 1u);")
        if need & ZF: lines.append("    if (_c) zf = (_r == 0);")
        if need & SF: lines.append("    if (_c) sf = ((_r & 0x80000000u) != 0);")
        if need & PF: lines.append("    if (_c) pf = recomp_parity((uint8_t)_r);")
        lines.append(f"    {write_op(dst, '_r', addr_override)}")
        lines.append("  }")
        out.extend(lines)
        return

    if m in ("bt", "bts"):
        dst, bit_op = ops[0], ops[1]
        lines = [f"  {{ int32_t _bit = (int32_t)({read_op(bit_op)});"]
        if dst.type == CS_OP_MEM:
            lines.append(f"    uint32_t _wa = (uint32_t)({op_mem_addr(dst)} + (uint32_t)((_bit >> 5) * 4));")
            lines.append("    uint32_t _mask = 1u << ((uint32_t)_bit & 31u);")
            lines.append("    uint32_t _w = mem_r32(_wa);")
            if need & CF: lines.append("    cf = ((_w & _mask) != 0);")
            if m == "bts": lines.append("    mem_w32(_wa, _w | _mask);")
        else:
            lines.append(f"    uint32_t _w = {read_op(dst)}; uint32_t _mask = 1u << ((uint32_t)_bit & 31u);")
            if need & CF: lines.append("    cf = ((_w & _mask) != 0);")
            if m == "bts": lines.append(f"    {write_op(dst, '_w | _mask')}")
        lines.append("  }")
        out.extend(lines)
        return

    if m == "imul":
        if len(ops) == 1:
            out.append(f"  {{ int64_t _p = (int64_t)(int32_t)ctx->eax * (int64_t)({read_op(ops[0], signed=True)});")
            out.append("    ctx->eax = (uint32_t)(uint64_t)_p; ctx->edx = (uint32_t)((uint64_t)_p >> 32); }")
        elif len(ops) == 2:
            sz = ops[0].size
            st = sint_t(sz)
            ut = uint_t(sz)
            out.append(f"  {{ {st} _a = ({st})({read_op(ops[0], signed=True)}); {st} _b = ({st})({read_op(ops[1], signed=True)});")
            out.append(f"    {write_op(ops[0], f'({ut})((int32_t)_a * (int32_t)_b)')} }}")
        else:
            out.append(f"  {{ int32_t _a = (int32_t)({read_op(ops[1], signed=True)}); int32_t _b = (int32_t)({read_op(ops[2], signed=True)});")
            out.append(f"    {write_op(ops[0], '(uint32_t)(_a * _b)')} }}")
        return

    if m == "mul":
        sz = ops[0].size
        if sz == 1:
            out.append(f"  {{ uint16_t _p = (uint16_t)reg_r8l(ctx->eax) * (uint16_t)({read_op(ops[0])});")
            out.append("    reg_w16(&ctx->eax, _p); }")
        else:
            out.append(f"  {{ uint64_t _p = (uint64_t)ctx->eax * (uint64_t)({read_op(ops[0])});")
            out.append("    ctx->eax = (uint32_t)_p; ctx->edx = (uint32_t)(_p >> 32); }")
        return

    if m == "div":
        out.append(f"  {{ uint64_t _num = ((uint64_t)ctx->edx << 32) | ctx->eax; uint32_t _den = {read_op(ops[0])};")
        out.append("    ctx->eax = (uint32_t)(_num / _den); ctx->edx = (uint32_t)(_num % _den); }")
        return

    if m == "idiv":
        out.append(f"  {{ int64_t _num = (int64_t)(((uint64_t)ctx->edx << 32) | ctx->eax); int32_t _den = {read_op(ops[0], signed=True)};")
        out.append("    ctx->eax = (uint32_t)(int32_t)(_num / _den); ctx->edx = (uint32_t)(int32_t)(_num % _den); }")
        return

    if m.startswith("set"):
        cc = m[3:]
        out.append(f"  {write_op(ops[0], f'(uint8_t)({cond_expr(cc)} ? 1u : 0u)')}")
        return

    if m.startswith("cmov"):
        cc = m[4:]
        out.append(f"  if ({cond_expr(cc)}) {{ {write_op(ops[0], read_op(ops[1]))} }}")
        return

    if m == "jmp":
        op0 = ops[0]
        if op0.type == CS_OP_IMM:
            tgt = op0.imm & 0xFFFFFFFF
            if fn_start <= tgt < fn_end and tgt in label_targets:
                out.append(f"  goto L_{tgt:08x};")
            elif tgt in engine_funcs:
                out.append(f"  fn_{tgt:08x}(ctx, guest_memory); return;")
            elif tgt in plt_map:
                out.append(f"  imp_{sanitize_c_ident(plt_map[tgt])}(ctx); ctx->eip = pop32(ctx); return;")
            elif tgt in HLE_EXTRA:
                out.append(f"  imp_{sanitize_c_ident(HLE_EXTRA[tgt])}(ctx); ctx->eip = pop32(ctx); return;")
            else:
                out.append(f"  recomp_tailcall(ctx, 0x{tgt:08x}u); return;")
        else:
            targets = switch_tables[addr]
            out.append(f"  switch ({read_op(op0)}) {{")
            for t in targets:
                out.append(f"    case 0x{t:08x}u: goto L_{t:08x};")
            out.append(f"    default: recomp_tailcall(ctx, {read_op(op0)}); return;")
            out.append("  }")
        return

    if m.startswith("j"):
        cc = m[1:]
        tgt = ops[0].imm & 0xFFFFFFFF
        if fn_start <= tgt < fn_end and tgt in label_targets:
            out.append(f"  if ({cond_expr(cc)}) goto L_{tgt:08x};")
        else:
            out.append(f"  if ({cond_expr(cc)}) {{ recomp_tailcall(ctx, 0x{tgt:08x}u); return; }}")
        return

    if m == "call":
        op0 = ops[0]
        if op0.type == CS_OP_IMM:
            tgt = op0.imm & 0xFFFFFFFF
            if tgt == SETJMP_PLT:
                out.append("  { RecompJmpSlot* volatile _js = recomp_setjmp_prep(ctx, mem_r32(ctx->esp));")
                out.append("    if (setjmp(_js->env) == 0) { ctx->eax = 0; } else { recomp_setjmp_restore(ctx, _js); } }")
                return
            out.append(f"  push32(ctx, 0x{addr + ins.size:08x}u);")
            if tgt in engine_funcs:
                out.append(f"  fn_{tgt:08x}(ctx, guest_memory);")
            elif tgt in plt_map:
                out.append(f"  imp_{sanitize_c_ident(plt_map[tgt])}(ctx); ctx->eip = pop32(ctx);")
            elif tgt in HLE_EXTRA:
                out.append(f"  imp_{sanitize_c_ident(HLE_EXTRA[tgt])}(ctx); ctx->eip = pop32(ctx);")
            else:
                out.append(f"  recomp_icall(ctx, 0x{tgt:08x}u);")
        else:
            out.append(f"  {{ uint32_t _tgt = {read_op(op0)};")
            out.append(f"    push32(ctx, 0x{addr + ins.size:08x}u);")
            out.append("    recomp_icall(ctx, _tgt); }")
        return

    if m == "rep movsd":
        out.append("  while (ctx->ecx) { mem_w32(ctx->edi, mem_r32(ctx->esi)); ctx->esi += 4u; ctx->edi += 4u; ctx->ecx--; }")
        return

    if m == "rep stosd":
        out.append("  while (ctx->ecx) { mem_w32(ctx->edi, ctx->eax); ctx->edi += 4u; ctx->ecx--; }")
        return

    # x87 FPU instructions
    if m == "fld":
        op0 = ops[0]
        if op0.type == CS_OP_REG:
            out.append(f"  fpu_push(ctx, fpu_st(ctx, {ST_MAP[op0.reg]}));")
        elif op0.size == 4:
            out.append(f"  fpu_push(ctx, (double)mem_rf32({op_mem_addr(op0)}));")
        else:
            out.append(f"  fpu_push(ctx, mem_rf64({op_mem_addr(op0)}));")
        return

    if m == "fldz":
        out.append("  fpu_push(ctx, 0.0);")
        return

    if m == "fld1":
        out.append("  fpu_push(ctx, 1.0);")
        return

    if m == "fild":
        op0 = ops[0]
        out.append(f"  fpu_push(ctx, (double){read_op(op0, signed=True)});")
        return

    if m == "fstp":
        op0 = ops[0]
        if op0.type == CS_OP_REG:
            idx = ST_MAP[op0.reg]
            out.append(f"  {{ double _fv = fpu_st(ctx, 0); fpu_set(ctx, {idx}, _fv); (void)fpu_pop(ctx); }}")
        elif op0.size == 4:
            out.append(f"  mem_wf32({op_mem_addr(op0)}, (float)fpu_pop(ctx));")
        else:
            out.append(f"  mem_wf64({op_mem_addr(op0)}, fpu_pop(ctx));")
        return

    if m == "fist":
        op0 = ops[0]
        out.append(f"  {write_op(op0, 'fpu_round_i32(fpu_st(ctx, 0), ctx->fpu_cw)')}")
        return

    if m == "fistp":
        op0 = ops[0]
        if op0.size == 2:
            out.append(f"  {write_op(op0, 'fpu_round_i16(fpu_pop(ctx), ctx->fpu_cw)')}")
        elif op0.size == 4:
            out.append(f"  {write_op(op0, 'fpu_round_i32(fpu_pop(ctx), ctx->fpu_cw)')}")
        else:
            out.append(f"  {write_op(op0, '(uint64_t)fpu_round_i64(fpu_pop(ctx), ctx->fpu_cw)')}")
        return

    if m == "fxch":
        idx = ST_MAP[ops[0].reg] if ops else 1
        out.append(f"  fpu_fxch(ctx, {idx});")
        return

    if m == "fchs":
        out.append("  fpu_set(ctx, 0, -fpu_st(ctx, 0));")
        return

    if m == "fabs":
        out.append("  fpu_set(ctx, 0, fabs(fpu_st(ctx, 0)));")
        return

    if m == "fsqrt":
        out.append("  fpu_set(ctx, 0, sqrt(fpu_st(ctx, 0)));")
        return

    if m == "fprem":
        out.append("  fpu_fprem(ctx);")
        return

    if m in ("fadd", "fsub", "fsubr", "fmul", "fdiv", "fdivr",
             "faddp", "fsubp", "fsubrp", "fmulp", "fdivp", "fdivrp"):
        pop = m.endswith("p")
        base_m = m[:-1] if pop else m
        op0 = ops[0]
        if op0.type == CS_OP_MEM:
            rhs = f"(double)mem_rf32({op_mem_addr(op0)})" if op0.size == 4 else f"mem_rf64({op_mem_addr(op0)})"
            a_expr, b_expr = "fpu_st(ctx, 0)", rhs
            dst_idx = 0
        else:
            idx = ST_MAP[op0.reg]
            if pop:
                # Intel: fsubp st(i), st(0) -> st(i) = st(i) - st(0); pop()
                #        fsubrp st(i), st(0) -> st(i) = st(0) - st(i); pop()
                a_expr, b_expr = f"fpu_st(ctx, {idx})", "fpu_st(ctx, 0)"
                dst_idx = idx
            else:
                a_expr, b_expr = "fpu_st(ctx, 0)", f"fpu_st(ctx, {idx})"
                dst_idx = 0

        if base_m == "fadd": expr = f"({a_expr}) + ({b_expr})"
        elif base_m == "fsub": expr = f"({a_expr}) - ({b_expr})"
        elif base_m == "fsubr": expr = f"({b_expr}) - ({a_expr})"
        elif base_m == "fmul": expr = f"({a_expr}) * ({b_expr})"
        elif base_m == "fdiv": expr = f"({a_expr}) / ({b_expr})"
        elif base_m == "fdivr": expr = f"({b_expr}) / ({a_expr})"

        out.append(f"  fpu_set(ctx, {dst_idx}, {expr});")
        if pop:
            out.append("  (void)fpu_pop(ctx);")
        return

    if m in ("fucomi", "fucomip"):
        idx = ST_MAP[ops[0].reg]
        out.append(f"  {{ uint8_t _z, _p, _c; fpu_fucom(ctx, fpu_st(ctx, 0), fpu_st(ctx, {idx}), &_z, &_p, &_c);")
        if need & ZF: out.append("    zf = _z;")
        if need & PF: out.append("    pf = _p;")
        if need & CF: out.append("    cf = _c;")
        out.append("  }")
        if m == "fucomip":
            out.append("  (void)fpu_pop(ctx);")
        return

    if m == "fldcw":
        out.append(f"  ctx->fpu_cw = mem_r16({op_mem_addr(ops[0])});")
        return

    if m == "fnstcw":
        out.append(f"  mem_w16({op_mem_addr(ops[0])}, ctx->fpu_cw);")
        return

    if m == "fnstsw":
        out.append("  reg_w16(&ctx->eax, ctx->fpu_sw);")
        return

    # SSE instructions
    if m == "movss":
        dst, src = ops[0], ops[1]
        if dst.type == CS_OP_REG and src.type == CS_OP_MEM:
            xi = XMM_MAP[dst.reg]
            out.append(f"  ctx->xmm[{xi}].u[0] = mem_r32({op_mem_addr(src)}); ctx->xmm[{xi}].u[1] = 0; ctx->xmm[{xi}].u[2] = 0; ctx->xmm[{xi}].u[3] = 0;")
        elif dst.type == CS_OP_MEM and src.type == CS_OP_REG:
            xi = XMM_MAP[src.reg]
            out.append(f"  mem_w32({op_mem_addr(dst)}, ctx->xmm[{xi}].u[0]);")
        else:
            di, si = XMM_MAP[dst.reg], XMM_MAP[src.reg]
            out.append(f"  ctx->xmm[{di}].u[0] = ctx->xmm[{si}].u[0];")
        return

    if m == "cvttss2si":
        dst, src = ops[0], ops[1]
        if src.type == CS_OP_REG:
            fval = f"ctx->xmm[{XMM_MAP[src.reg]}].f[0]"
        else:
            fval = f"mem_rf32({op_mem_addr(src)})"
        out.append(f"  {write_op(dst, f'(uint32_t)(int32_t)({fval})')}")
        return

    if m == "movlps":
        dst, src = ops[0], ops[1]
        if dst.type == CS_OP_MEM:
            out.append(f"  mem_w64({op_mem_addr(dst)}, ctx->xmm[{XMM_MAP[src.reg]}].u64[0]);")
        else:
            out.append(f"  ctx->xmm[{XMM_MAP[dst.reg]}].u64[0] = mem_r64({op_mem_addr(src)});")
        return

    if m == "movaps":
        dst, src = ops[0], ops[1]
        if dst.type == CS_OP_REG and src.type == CS_OP_MEM:
            out.append(f"  __builtin_memcpy(&ctx->xmm[{XMM_MAP[dst.reg]}], g_mem_base + {op_mem_addr(src)}, 16);")
        elif dst.type == CS_OP_MEM and src.type == CS_OP_REG:
            out.append(f"  __builtin_memcpy(g_mem_base + {op_mem_addr(dst)}, &ctx->xmm[{XMM_MAP[src.reg]}], 16);")
        else:
            out.append(f"  ctx->xmm[{XMM_MAP[dst.reg]}] = ctx->xmm[{XMM_MAP[src.reg]}];")
        return

    if m == "movlhps":
        di, si = XMM_MAP[ops[0].reg], XMM_MAP[ops[1].reg]
        out.append(f"  ctx->xmm[{di}].u[2] = ctx->xmm[{si}].u[0]; ctx->xmm[{di}].u[3] = ctx->xmm[{si}].u[1];")
        return

    if m == "movmskps":
        dst, src = ops[0], ops[1]
        si = XMM_MAP[src.reg]
        expr = f"(((ctx->xmm[{si}].u[0]>>31)&1u) | (((ctx->xmm[{si}].u[1]>>31)&1u)<<1) | (((ctx->xmm[{si}].u[2]>>31)&1u)<<2) | (((ctx->xmm[{si}].u[3]>>31)&1u)<<3))"
        out.append(f"  {write_op(dst, expr)}")
        return

    if m in ("addps", "cmpltps", "unpcklps", "shufps"):
        di = XMM_MAP[ops[0].reg]
        src = ops[1]
        if src.type == CS_OP_REG:
            rhs_setup = f"XmmReg _xr = ctx->xmm[{XMM_MAP[src.reg]}];"
        else:
            rhs_setup = f"XmmReg _xr; __builtin_memcpy(&_xr, g_mem_base + {op_mem_addr(src)}, 16);"
        out.append(f"  {{ {rhs_setup}")
        if m == "addps":
            out.append(f"    for (int _k=0; _k<4; _k++) ctx->xmm[{di}].f[_k] += _xr.f[_k]; }}")
        elif m == "cmpltps":
            out.append(f"    for (int _k=0; _k<4; _k++) ctx->xmm[{di}].u[_k] = (ctx->xmm[{di}].f[_k] < _xr.f[_k]) ? 0xFFFFFFFFu : 0u; }}")
        elif m == "unpcklps":
            out.append(f"    XmmReg _xt; _xt.u[0]=ctx->xmm[{di}].u[0]; _xt.u[1]=_xr.u[0]; _xt.u[2]=ctx->xmm[{di}].u[1]; _xt.u[3]=_xr.u[1]; ctx->xmm[{di}]=_xt; }}")
        else:  # shufps
            imm = ops[2].imm & 0xFF
            out.append(
                f"    XmmReg _xt; _xt.u[0]=ctx->xmm[{di}].u[{imm&3}]; _xt.u[1]=ctx->xmm[{di}].u[{(imm>>2)&3}]; "
                f"_xt.u[2]=_xr.u[{(imm>>4)&3}]; _xt.u[3]=_xr.u[{(imm>>6)&3}]; ctx->xmm[{di}]=_xt; }}"
            )
        return

    raise RuntimeError(f"Unhandled instruction at 0x{addr:08x}: {m} {ins.op_str}")


def emit_func_body(fn_name, fn_start, fn_end, insns, plt_map, engine_funcs, rdata, rbase):
    valid_addrs = {ins.address for ins in insns}
    switch_tables = resolve_switch_tables(insns, fn_start, fn_end, valid_addrs, rdata, rbase)
    live_after = compute_flag_liveness(insns, switch_tables, fn_start, fn_end)

    label_targets = set()
    for idx, ins in enumerate(insns):
        m = ins.mnemonic
        if m == "jmp" or m.startswith("j"):
            op0 = ins.operands[0]
            if op0.type == CS_OP_IMM:
                t = op0.imm & 0xFFFFFFFF
                if fn_start <= t < fn_end:
                    label_targets.add(t)
        if ins.address in switch_tables:
            label_targets.update(switch_tables[ins.address])

    out = [f"void {fn_name}(CPUContext* ctx, uint8_t* guest_memory) {{"]
    out.append("  uint8_t cf = 0, zf = 0, sf = 0, of = 0, pf = 0;")
    out.append("  (void)cf; (void)zf; (void)sf; (void)of; (void)pf;")
    for i, ins in enumerate(insns):
        emit_insn(ins, live_after[i], fn_start, fn_end, switch_tables, label_targets, plt_map, engine_funcs, out)
    out.append("}")
    return "\n".join(out), len(switch_tables)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--elf", type=Path, default=PROJECT_ROOT / "game/linux/Psychonauts")
    parser.add_argument("--output", type=Path, default=PROJECT_ROOT / "src/gen")
    args = parser.parse_args()
    elf_path = args.elf
    out_dir = args.output
    out_dir.mkdir(parents=True, exist_ok=True)
    with open(elf_path, "rb") as f:
        elf = ELFFile(f)
        text_sec = elf.get_section_by_name(".text")
        rodata_sec = elf.get_section_by_name(".rodata")
        plt_sec = elf.get_section_by_name(".plt")
        rel_plt = elf.get_section_by_name(".rel.plt")
        dynsym = elf.get_section_by_name(".dynsym")
        symtab = elf.get_section_by_name(".symtab")

        tdata, tbase = text_sec.data(), text_sec["sh_addr"]
        rdata, rbase = rodata_sec.data(), rodata_sec["sh_addr"]
        plt_base = plt_sec["sh_addr"]

        # Map PLT stub address (0x0810f93c + idx*16) -> symbol name
        plt_map = {}
        got_plt_entries = []
        for idx, rel in enumerate(rel_plt.iter_relocations()):
            sym = dynsym.get_symbol(rel["r_info_sym"])
            plt_addr = plt_base + 16 * (idx + 1)
            plt_map[plt_addr] = sym.name
            got_plt_entries.append((rel["r_offset"], sym.name, plt_addr))

        funcs_by_addr = {}
        for sym in symtab.iter_symbols():
            val, size, stype = sym["st_value"], sym["st_size"], sym["st_info"]["type"]
            if ENGINE_START <= val < ENGINE_END and stype == "STT_FUNC":
                if val not in funcs_by_addr or size > funcs_by_addr[val][1]:
                    funcs_by_addr[val] = (val, size, sym.name)

    funcs = sorted(funcs_by_addr.values(), key=lambda x: x[0])
    engine_func_addrs = {v for v, _, _ in funcs}

    md = Cs(CS_ARCH_X86, CS_MODE_32)
    md.detail = True

    total_insns = 0
    total_switches = 0
    shard_buffers = [[] for _ in range(NUM_SHARDS)]
    shard_counts = [0] * NUM_SHARDS

    for idx, (start, size, sym_name) in enumerate(funcs):
        code = tdata[start - tbase : start + size - tbase]
        insns = list(md.disasm(code, start))
        if sum(i.size for i in insns) != len(code):
            raise RuntimeError(f"Decode mismatch in {sym_name} at 0x{start:08x}")
        total_insns += len(insns)

        shard_id = idx % NUM_SHARDS
        fn_name = f"fn_{start:08x}"

        if start == 0x084BD82E:
            # Split out coroutine body [CORO_START, CORO_END) as static helper in same shard
            main_insns = [i for i in insns if not (CORO_START < i.address < CORO_END)]
            coro_insns = [i for i in insns if CORO_START <= i.address < CORO_END]
            coro_code, sw_c = emit_func_body("fn_084bdca2_coro", CORO_START, CORO_END, coro_insns, plt_map, engine_func_addrs, rdata, rbase)
            # In main_insns, replace CORO_START insn via emit_insn hook
            main_code, sw_m = emit_func_body(fn_name, start, start + size, main_insns, plt_map, engine_func_addrs, rdata, rbase)
            shard_buffers[shard_id].append(f"/* {sym_name} coro */\nstatic {coro_code}")
            shard_buffers[shard_id].append(f"/* {sym_name} */\n{main_code}")
            total_switches += sw_c + sw_m
        else:
            code_c, sw = emit_func_body(fn_name, start, start + size, insns, plt_map, engine_func_addrs, rdata, rbase)
            shard_buffers[shard_id].append(f"/* {sym_name} */\n{code_c}")
            total_switches += sw
        shard_counts[shard_id] += 1

    # Emit header recomp_funcs.h
    all_imports = sorted(set(plt_map.values()) | set(HLE_EXTRA.values()))
    with open(os.path.join(out_dir, "recomp_funcs.h"), "w") as hf:
        hf.write("#ifndef RECOMP_FUNCS_H\n#define RECOMP_FUNCS_H\n\n")
        hf.write('#include "runtime/cpu.h"\n\n')
        hf.write("#ifdef __cplusplus\nextern \"C\" {\n#endif\n\n")
        for v, _, sym_name in funcs:
            hf.write(f"void fn_{v:08x}(CPUContext* ctx, uint8_t* guest_memory);\n")
        hf.write("\n/* Imported / HLE thunks */\n")
        for imp in all_imports:
            hf.write(f"void imp_{sanitize_c_ident(imp)}(CPUContext* ctx);\n")
        hf.write("\n#ifdef __cplusplus\n}\n#endif\n#endif\n")

    # Emit 64 shard files
    for s in range(NUM_SHARDS):
        with open(os.path.join(out_dir, f"recomp_{s:02d}.c"), "w") as sf:
            sf.write('#include "recomp_funcs.h"\n\n')
            sf.write("\n\n".join(shard_buffers[s]))
            sf.write("\n")

    # Emit recomp_dispatch.c
    dispatch_entries = []
    for v, _, _ in funcs:
        dispatch_entries.append((v, f"fn_{v:08x}"))
    for plt_addr, imp in plt_map.items():
        dispatch_entries.append((plt_addr, f"_wimp_{sanitize_c_ident(imp)}"))
    for hle_addr, imp in HLE_EXTRA.items():
        dispatch_entries.append((hle_addr, f"_wimp_{sanitize_c_ident(imp)}"))
    dispatch_entries.sort(key=lambda x: x[0])

    with open(os.path.join(out_dir, "recomp_dispatch.c"), "w") as df:
        df.write('#include "recomp_funcs.h"\n')
        df.write("#include <stdio.h>\n#include <stdlib.h>\n\n")
        for imp in all_imports:
            ci = sanitize_c_ident(imp)
            df.write(f"static void _wimp_{ci}(CPUContext* ctx, uint8_t* guest_memory) {{ imp_{ci}(ctx); ctx->eip = pop32(ctx); }}\n")
        df.write("\n")
        df.write("typedef struct RecompDispatchEntry {\n    uint32_t addr;\n    RecompFn fn;\n} RecompDispatchEntry;\n\n")
        df.write(f"static const RecompDispatchEntry g_dispatch[{len(dispatch_entries)}] = {{\n")
        for addr, fname in dispatch_entries:
            df.write(f"    {{ 0x{addr:08x}u, {fname} }},\n")
        df.write("};\n\n")

        # Synthetic GOT.PLT table info for ELF loader
        df.write("typedef struct RecompGotSlot {\n    uint32_t got_addr;\n    uint32_t synth_addr;\n    RecompFn fn;\n    const char* name;\n} RecompGotSlot;\n\n")
        df.write(f"static const RecompGotSlot g_got_slots[{len(got_plt_entries)}] = {{\n")
        for idx, (got_off, sym_name, _) in enumerate(got_plt_entries):
            synth = 0xF0000000 + idx * 4
            df.write(f'    {{ 0x{got_off:08x}u, 0x{synth:08x}u, _wimp_{sanitize_c_ident(sym_name)}, "{sym_name}" }},\n')
        df.write("};\n\n")

        df.write("""#define DISPATCH_HASH_SIZE 65536u
#define DISPATCH_HASH_MASK (DISPATCH_HASH_SIZE - 1u)
static RecompDispatchEntry g_dispatch_hash[DISPATCH_HASH_SIZE];
static int g_dispatch_hash_ready = 0;

static void recomp_build_dispatch_hash(void) {
    for (size_t i = 0; i < sizeof(g_dispatch) / sizeof(g_dispatch[0]); i++) {
        uint32_t addr = g_dispatch[i].addr;
        uint32_t h = (addr * 2654435761u) >> 16;
        while (g_dispatch_hash[h].addr != 0) {
            h = (h + 1u) & DISPATCH_HASH_MASK;
        }
        g_dispatch_hash[h] = g_dispatch[i];
    }
    g_dispatch_hash_ready = 1;
}

void recomp_init_got_plt(void) {
    for (size_t i = 0; i < sizeof(g_got_slots) / sizeof(g_got_slots[0]); i++) {
        mem_w32(g_got_slots[i].got_addr, g_got_slots[i].synth_addr);
    }
    if (!g_dispatch_hash_ready) {
        recomp_build_dispatch_hash();
    }
}

extern RecompFn recomp_gl_lookup(uint32_t synth_addr);

RecompFn recomp_lookup(uint32_t addr) {
    if (__builtin_expect(addr >= 0xF0000000u, 0)) {
        if (addr < 0xF0010000u) {
            uint32_t idx = (addr - 0xF0000000u) >> 2;
            if (idx < sizeof(g_got_slots) / sizeof(g_got_slots[0])) {
                return g_got_slots[idx].fn;
            }
            return NULL;
        }
        if (addr < 0xF0020000u) {
            return recomp_gl_lookup(addr);
        }
    }
    if (__builtin_expect(!g_dispatch_hash_ready, 0)) {
        recomp_build_dispatch_hash();
    }
    uint32_t h = (addr * 2654435761u) >> 16;
    for (;;) {
        uint32_t ma = g_dispatch_hash[h].addr;
        if (__builtin_expect(ma == addr, 1)) return g_dispatch_hash[h].fn;
        if (ma == 0) return NULL;
        h = (h + 1u) & DISPATCH_HASH_MASK;
    }
}
""")

        df.write(r"""extern uint64_t g_recomp_icall_count;

void recomp_icall(CPUContext* ctx, uint32_t addr) {
    g_recomp_icall_count++;
    RecompFn fn = recomp_lookup(addr);
    if (__builtin_expect(!fn, 0)) {
        uint32_t ret_eip = mem_r32(ctx->esp);
        fprintf(stderr, "[recomp] FATAL: unresolved indirect call to 0x%08x from ret=0x%08x (eax=0x%08x esp=0x%08x)\n",
                addr, ret_eip, ctx->eax, ctx->esp);
        abort();
    }
    fn(ctx, g_mem_base);
}

void recomp_tailcall(CPUContext* ctx, uint32_t addr) {
    RecompFn fn = recomp_lookup(addr);
    if (__builtin_expect(!fn, 0)) {
        fprintf(stderr, "[recomp] FATAL: unresolved tailcall/jump to 0x%08x (esp=0x%08x)\n",
                addr, ctx->esp);
        abort();
    }
    fn(ctx, g_mem_base);
}
""")

    print(f"Lifted {len(funcs)} functions ({total_insns} insns, {total_switches} switch tables) into {NUM_SHARDS} shards.")
    print(f"Unique imports: {len(all_imports)}, dispatch entries: {len(dispatch_entries)}, got slots: {len(got_plt_entries)}")


if __name__ == "__main__":
    main()
