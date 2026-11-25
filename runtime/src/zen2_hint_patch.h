// In-place patching of Zen 2 hint instructions (MONITORX, MWAITX, CLWB) on hosts that lack them.
//
// In src/cpu_emulation.h, MONITORX and CLWB are no-ops and MWAITX executes YieldProcessor() (PAUSE). Taking a #UD
// exception through the OS/Rosetta/Wine VEH for each one is thousands of times slower than the instruction itself
// (over 1.2M #UD traps in a 2-minute boot on Apple Silicon / Rosetta 2).
//
// Using the same .pdata + x86_length.h instruction walk as src/adx_rewrite.h (before protect_sections), each site is
// replaced in place with an exact-length instruction sequence:
//   MONITORX (3 bytes: 0F 01 FA)       -> 0F 1F 00 (3-byte NOP)
//   MWAITX   (3 bytes: 0F 01 FB)       -> F3 90 90 (PAUSE; NOP)
//   CLWB m8  (4..15 bytes)             -> canonical multi-byte NOPs of the same length
//
// Set LEONIDA_HINT_PATCH=0 to keep the #UD path.
#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <vector>

#include "adx_rewrite.h"
#include "cpu_emulation.h"
#include "x86_length.h"

namespace rt::zen2_hint_patch {

namespace ce = rt::cpu_emulation;
using Range = rt::adx_rewrite::Range;

struct Result {
    std::size_t monitorx = 0;
    std::size_t mwaitx = 0;
    std::size_t clwb = 0;
    std::size_t unsynced = 0;
};

inline bool enabled() {
#ifdef _WIN32
    char v[8] = {};
    std::size_t n = 0;
    if (getenv_s(&n, v, "LEONIDA_HINT_PATCH") == 0 && n > 1 && v[0] == '0') return false;
#else
    if (const char* v = std::getenv("LEONIDA_HINT_PATCH"); v && v[0] == '0') return false;
#endif
    return true;
}

inline void write_nops(uint8_t* dst, std::size_t len) {
    static constexpr uint8_t kNop1[1] = {0x90};
    static constexpr uint8_t kNop2[2] = {0x66, 0x90};
    static constexpr uint8_t kNop3[3] = {0x0F, 0x1F, 0x00};
    static constexpr uint8_t kNop4[4] = {0x0F, 0x1F, 0x40, 0x00};
    static constexpr uint8_t kNop5[5] = {0x0F, 0x1F, 0x44, 0x00, 0x00};
    static constexpr uint8_t kNop6[6] = {0x66, 0x0F, 0x1F, 0x44, 0x00, 0x00};
    static constexpr uint8_t kNop7[7] = {0x0F, 0x1F, 0x80, 0x00, 0x00, 0x00, 0x00};
    static constexpr uint8_t kNop8[8] = {0x0F, 0x1F, 0x84, 0x00, 0x00, 0x00, 0x00, 0x00};
    static constexpr uint8_t kNop9[9] = {0x66, 0x0F, 0x1F, 0x84, 0x00, 0x00, 0x00, 0x00, 0x00};
    static constexpr const uint8_t* kTable[9] = {kNop1, kNop2, kNop3, kNop4, kNop5, kNop6, kNop7, kNop8, kNop9};
    while (len > 0) {
        const std::size_t chunk = len > 9 ? 9 : len;
        std::memcpy(dst, kTable[chunk - 1], chunk);
        dst += chunk;
        len -= chunk;
    }
}

inline Result patch(uint8_t* image, std::size_t size, std::vector<Range> sections, std::vector<Range> functions,
                    bool patch_monitorx, bool patch_clwb) {
    Result out;
    if (!patch_monitorx && !patch_clwb) return out;
    functions.erase(std::remove_if(functions.begin(), functions.end(),
                                   [&](const Range& r) { return r.begin >= r.end || r.end > size; }),
                    functions.end());
    std::sort(functions.begin(), functions.end(), [](const Range& a, const Range& b) { return a.begin < b.begin; });

    for (Range sec : sections) {
        if (sec.end > size) sec.end = static_cast<uint32_t>(size);
        auto f = std::lower_bound(functions.begin(), functions.end(), sec.begin,
                                  [](const Range& r, uint32_t at) { return r.end <= at; });
        std::size_t run = 0;
        for (uint32_t pos = sec.begin; pos < sec.end;) {
            while (f != functions.end() && f->end <= pos) ++f;
            const bool inside = f != functions.end() && f->begin <= pos;
            if (f != functions.end() && f->begin == pos) run = rt::adx_rewrite::kInSync;
            const uint32_t limit = inside ? f->end : (f != functions.end() ? std::min(f->begin, sec.end) : sec.end);
            const std::size_t avail = std::min<std::size_t>(15, limit - pos);
            const std::size_t n = rt::x86::length(image + pos, avail);
            if (!n) {
                run = 0;
                ++pos;
                continue;
            }
            const ce::Decoded d = ce::decode(image + pos, avail, true);
            const bool want_mon = patch_monitorx && (d.op == ce::Op::monitorx || d.op == ce::Op::mwaitx) && d.length == n && n == 3;
            const bool want_clwb = patch_clwb && d.op == ce::Op::clwb && d.length == n && n >= 4;
            if (want_mon || want_clwb) {
                if (inside || run >= rt::adx_rewrite::kInSync) {
                    if (d.op == ce::Op::monitorx) {
                        write_nops(image + pos, 3);
                        ++out.monitorx;
                    } else if (d.op == ce::Op::mwaitx) {
                        image[pos] = 0xF3;
                        image[pos + 1] = 0x90;  // PAUSE
                        image[pos + 2] = 0x90;  // NOP
                        ++out.mwaitx;
                    } else {
                        write_nops(image + pos, n);
                        ++out.clwb;
                    }
                } else {
                    ++out.unsynced;
                }
            }
            ++run;
            pos += static_cast<uint32_t>(n);
        }
    }
#ifdef _WIN32
    if (out.monitorx || out.mwaitx || out.clwb) FlushInstructionCache(GetCurrentProcess(), image, size);
#endif
    return out;
}

}  // namespace rt::zen2_hint_patch
