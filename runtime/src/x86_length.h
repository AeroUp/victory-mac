// x86-64 instruction lengths, so a function's code can be walked one instruction at a time (src/adx_rewrite.h).
//
// length() returns how many bytes the 64-bit-mode instruction at `code` takes, prefixes included, from the opcode
// maps of the Intel SDM vol. 2 (appendix A) and the AMD APM vol. 3 (SSE4a, XOP, 3DNow!). It needs no operand
// meaning, only the shape: legacy prefixes and REX, the opcode (one byte, 0F, 0F 38, 0F 3A, VEX, XOP or EVEX), the
// ModRM, SIB and displacement bytes, and the immediate. 0 means the bytes are no valid 64-bit-mode instruction (an
// opcode 64-bit mode removed, a VEX/XOP/EVEX prefix after 66, F2, F3, F0 or REX, more than 15 bytes) or run past
// `avail`. Where Intel and AMD differ the Intel length is used: a near branch with a 66 prefix keeps its rel32, 0F FF
// (UD0) has a ModRM byte. Neither case is code a compiler writes.
//
// Checked against llvm-mc's disassembler: test/adx_rewrite_test.cpp holds the hand-written cases, and
// test/check_x86_length.py compares every length on random bytes when llvm-mc is installed.
#pragma once

#include <cstddef>
#include <cstdint>

namespace rt::x86 {

namespace length_detail {

// The ModRM byte at code[i] and the SIB and displacement bytes after it. Returns the index after them, 0 when they
// run past `avail`. 64-bit mode has no 16-bit addressing, so a 67 prefix keeps this shape.
inline std::size_t modrm_end(const uint8_t* code, std::size_t avail, std::size_t i) {
    if (i >= avail) return 0;
    const uint8_t modrm = code[i++];
    const int mod = modrm >> 6, rm = modrm & 7;
    if (mod == 3) return i;
    if (rm == 4) {
        if (i >= avail) return 0;
        if (mod == 0 && (code[i] & 7) == 5) i += 4;  // no base: disp32
        ++i;
    } else if (mod == 0 && rm == 5) {
        i += 4;  // RIP-relative
    }
    if (mod == 1) i += 1;
    else if (mod == 2) i += 4;
    return i <= avail ? i : 0;
}

// Operand shapes of an opcode.
enum : uint8_t {
    kNone = 0,
    kModrm = 1,
    kImm8 = 2,     // also rel8
    kImm16 = 4,
    kImmZ = 8,     // 2 bytes with a 66 prefix, else 4
    kRel32 = 16,   // always 4 in 64-bit mode (Intel)
    kInvalid = 32,
};

// The one-byte map. Prefixes, REX, 0F and the VEX/XOP/EVEX escapes are handled before it is read.
constexpr uint8_t one_byte(unsigned op) {
    if (op < 0x40) {
        if (op == 0x0F || op == 0x26 || op == 0x2E || op == 0x36 || op == 0x3E) return kNone;  // handled elsewhere
        switch (op & 7) {
        case 4: return kImm8;
        case 5: return kImmZ;
        case 6: case 7: return kInvalid;  // PUSH/POP segment, DAA, DAS, AAA, AAS: gone in 64-bit mode
        default: return kModrm;
        }
    }
    if (op < 0x60) return kNone;  // REX (handled as a prefix), PUSH/POP r64
    switch (op) {
    case 0x60: case 0x61: case 0x82: case 0x9A: case 0xCE: case 0xD4: case 0xD5: case 0xD6: case 0xEA:
        return kInvalid;
    case 0x63: return kModrm;  // MOVSXD
    case 0x68: return kImmZ;
    case 0x69: return kModrm | kImmZ;
    case 0x6A: return kImm8;
    case 0x6B: return kModrm | kImm8;
    case 0x80: case 0x83: case 0xC0: case 0xC1: case 0xC6: return kModrm | kImm8;
    case 0x81: case 0xC7: return kModrm | kImmZ;
    case 0xA8: case 0xCD: case 0xEB: return kImm8;
    case 0xA9: return kImmZ;
    case 0xC2: case 0xCA: return kImm16;
    case 0xC8: return kImm16 | kImm8;  // ENTER
    case 0xE8: case 0xE9: return kRel32;
    default: break;
    }
    if (op >= 0x6C && op <= 0x6F) return kNone;  // INS, OUTS
    if (op >= 0x70 && op <= 0x7F) return kImm8;  // Jcc rel8
    if (op >= 0x84 && op <= 0x8F) return kModrm;
    if (op >= 0x90 && op <= 0x9F) return kNone;
    if (op >= 0xA0 && op <= 0xA3) return kNone;  // MOV moffs: the offset is added by length()
    if (op >= 0xA4 && op <= 0xAF) return kNone;
    if (op >= 0xB0 && op <= 0xB7) return kImm8;
    if (op >= 0xB8 && op <= 0xBF) return kImmZ;  // imm64 with REX.W: length()
    if (op >= 0xD0 && op <= 0xD3) return kModrm;
    if (op >= 0xD8 && op <= 0xDF) return kModrm;  // x87
    if (op >= 0xE0 && op <= 0xE7) return kImm8;   // LOOPcc, JRCXZ, IN/OUT imm8
    if (op == 0xFE || op == 0xFF) return kModrm;
    if (op == 0xF6 || op == 0xF7) return kModrm;  // TEST's immediate: length()
    return kNone;  // C3, C9, CB, CC, CF, D7, EC-EF, F1, F4, F5, F8-FD
}

// The 0F map (the 0F 38 and 0F 3A escapes are handled before it is read).
constexpr uint8_t two_byte(unsigned op) {
    switch (op) {
    case 0x04: case 0x0A: case 0x0C: case 0x24: case 0x25: case 0x26: case 0x27: case 0x36: case 0x39:
    case 0x3B: case 0x3C: case 0x3D: case 0x3E: case 0x3F: case 0x7A: case 0x7B: case 0xA6: case 0xA7:
        return kInvalid;
    case 0x05: case 0x06: case 0x07: case 0x08: case 0x09: case 0x0B: case 0x0E: case 0x77:
    case 0xA0: case 0xA1: case 0xA2: case 0xA8: case 0xA9: case 0xAA:
        return kNone;  // SYSCALL, CLTS, SYSRET, INVD, WBINVD, UD2, FEMMS, EMMS, PUSH/POP FS/GS, CPUID, RSM
    case 0x0F: return kModrm | kImm8;  // 3DNow!: the opcode is a trailing byte
    case 0x70: case 0x71: case 0x72: case 0x73: case 0xA4: case 0xAC: case 0xBA:
    case 0xC2: case 0xC4: case 0xC5: case 0xC6:
        return kModrm | kImm8;
    default: break;
    }
    if (op >= 0x30 && op <= 0x37) return kNone;  // WRMSR, RDTSC, RDMSR, RDPMC, SYSENTER, SYSEXIT, GETSEC
    if (op >= 0x80 && op <= 0x8F) return kRel32;  // Jcc rel32
    if (op >= 0xC8 && op <= 0xCF) return kNone;   // BSWAP
    return kModrm;
}

// VEX and EVEX map 1 opcodes with an imm8 (shifts by immediate, compares, shuffles, inserts and extracts).
constexpr bool map1_imm8(unsigned op) {
    return (op >= 0x70 && op <= 0x73) || op == 0xC2 || op == 0xC4 || op == 0xC5 || op == 0xC6;
}

// ModRM onwards for an instruction whose opcode byte is code[i - 1]: `imm` immediate bytes after the addressing.
inline std::size_t finish(const uint8_t* code, std::size_t avail, std::size_t i, bool modrm, std::size_t imm) {
    if (modrm) {
        i = modrm_end(code, avail, i);
        if (!i) return 0;
    }
    i += imm;
    return i <= avail && i <= 15 ? i : 0;
}

}  // namespace length_detail

inline std::size_t length(const uint8_t* code, std::size_t avail) {
    using namespace length_detail;
    if (avail > 15) avail = 15;
    std::size_t i = 0;
    bool p66 = false, p67 = false, simd_prefix = false;  // simd_prefix: 66, F2, F3 or F0 (VEX after it is #UD)
    bool pf2 = false, pf3 = false;
    uint8_t rex = 0;
    for (; i < avail; ++i) {
        const uint8_t b = code[i];
        if ((b & 0xF0) == 0x40) { rex = b; continue; }
        if (b == 0x66) p66 = simd_prefix = true;
        else if (b == 0x67) p67 = true;
        else if (b == 0xF2) pf2 = simd_prefix = true;
        else if (b == 0xF3) pf3 = simd_prefix = true;
        else if (b == 0xF0) simd_prefix = true;
        else if (b == 0x2E || b == 0x3E || b == 0x26 || b == 0x36 || b == 0x64 || b == 0x65) {}
        else break;
        rex = 0;  // a REX counts only right before the opcode
    }
    if (i >= avail) return 0;
    const uint8_t op = code[i++];

    if (op == 0xC4 || op == 0xC5 || op == 0x62 || (op == 0x8F && i < avail && (code[i] & 0x1F) >= 8)) {
        if (rex || simd_prefix) return 0;
        unsigned map = 1;
        if (op == 0xC5) {
            i += 1;
        } else if (op == 0x62) {  // EVEX: P0 holds the map in bits 2:0
            if (i + 3 > avail) return 0;
            map = code[i] & 7;
            i += 3;
        } else {  // VEX3 and XOP: the map in bits 4:0 of the first payload byte
            if (i + 2 > avail) return 0;
            map = code[i] & 0x1F;
            i += 2;
        }
        if (i >= avail) return 0;
        const uint8_t opcode = code[i++];
        if (op == 0x8F) {  // XOP maps 8 (imm8), 9 (none), 10 (imm32)
            if (map == 8) return finish(code, avail, i, true, 1);
            if (map == 9) return finish(code, avail, i, true, 0);
            if (map == 10) return finish(code, avail, i, true, 4);
            return 0;
        }
        if (op != 0x62 && map == 1 && opcode == 0x77) return i <= 15 ? i : 0;  // VZEROUPPER, VZEROALL
        switch (map) {
        case 1: return finish(code, avail, i, true, map1_imm8(opcode) ? 1 : 0);
        case 2: return finish(code, avail, i, true, 0);
        case 3: return finish(code, avail, i, true, 1);
        case 5: case 6: return op == 0x62 ? finish(code, avail, i, true, 0) : 0;  // EVEX FP16 maps
        default: return 0;
        }
    }

    if (op == 0x0F) {
        if (i >= avail) return 0;
        const uint8_t op2 = code[i++];
        if (op2 == 0x38) {
            if (i >= avail) return 0;
            ++i;
            return finish(code, avail, i, true, 0);
        }
        if (op2 == 0x3A) {
            if (i >= avail) return 0;
            ++i;
            return finish(code, avail, i, true, 1);
        }
        const uint8_t shape = two_byte(op2);
        if (shape & kInvalid) return 0;
        if (shape & kRel32) return finish(code, avail, i, false, 4);
        std::size_t imm = (shape & kImm8) ? 1 : 0;
        // SSE4a INSERTQ (F2 0F 78 /r ib ib) and EXTRQ (66 0F 78 /0 ib ib) take two imm8, in the register form only,
        // as src/cpu_emulation.h decodes them. Other forms have VMREAD's shape, as llvm-mc decodes them (#UD with
        // these prefixes).
        if (op2 == 0x78 && !pf3 && i < avail && (code[i] >> 6) == 3 && (pf2 || (p66 && !(code[i] & 0x38))))
            imm = 2;
        return finish(code, avail, i, (shape & kModrm) != 0, imm);
    }

    const uint8_t shape = one_byte(op);
    if (shape & kInvalid) return 0;
    const bool wide = (rex & 8) != 0;
    const std::size_t z = (p66 && !wide) ? 2 : 4;
    if (op >= 0xA0 && op <= 0xA3) return finish(code, avail, i, false, p67 ? 4 : 8);  // MOV with moffs
    if (op >= 0xB8 && op <= 0xBF) return finish(code, avail, i, false, wide ? 8 : (p66 ? 2 : 4));
    if (op == 0xF6 || op == 0xF7) {  // TEST r/m, imm (/0 and /1) has an immediate; NOT, NEG, MUL, DIV do not
        if (i >= avail) return 0;
        const bool test = ((code[i] >> 3) & 7) < 2;
        return finish(code, avail, i, true, test ? (op == 0xF6 ? 1 : z) : 0);
    }
    std::size_t imm = 0;
    if (shape & kImm8) imm += 1;
    if (shape & kImm16) imm += 2;
    if (shape & kImmZ) imm += z;
    if (shape & kRel32) imm += 4;
    return finish(code, avail, i, (shape & kModrm) != 0, imm);
}

}  // namespace rt::x86
