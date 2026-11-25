// ADCX and ADOX rewritten before the image runs, for a host without ADX (Rosetta 2 on an Apple Silicon Mac, Haswell).
//
// src/cpu_emulation.h emulates the two instructions when they raise #UD, from the thread's context. That is exact on a
// CPU, but under Rosetta 2 the RFLAGS a SIGILL handler is given are often not the ones the code computed (16 of 23
// measured cases, docs/macos.md), so a carry chain started by `xor reg, reg` can add a stale carry. The flags Rosetta
// keeps for PUSHFQ are right. So here each ADCX/ADOX of the image becomes a jump to a stub that does the same work with
// ADC, which every x86-64 CPU and Rosetta have, and keeps every other flag through PUSHFQ/POPFQ:
//
//   ADCX dst, src                          ADOX dst, src
//     pushfq                                 pushfq
//     adc  dst, src                          bt   qword [rsp], 11     ; CF = the OF saved above
//     jc   1f                                adc  dst, src
//     and  qword [rsp], ~CF                  jc   1f
//     jmp  2f                                and  qword [rsp], ~OF
//   1: or   qword [rsp], CF                  jmp  2f
//   2: popfq                               1: or   qword [rsp], OF
//     jmp  <after the instruction>         2: popfq
//                                            jmp  <after the instruction>
//
// ADC adds the same three values and gives the same carry out, and its 32-bit form zero-extends dst as ADCX does; the
// new CF (ADCX) or OF (ADOX) is put in the saved flags, the rest are POPFQ'd back as they were. The source operand keeps
// its registers, scale and segment override; a displacement off RSP grows by the 8 bytes PUSHFQ took, and a
// RIP-relative one is recomputed from the stub. Windows x64 has no red zone, so the 8 bytes below RSP are free.
//
// Where the sites are. The 1.0.2.1 executable has 2,678 ADCX/ADOX, none inside a .pdata function range: they are in
// leaf routines (measured with this file's sweep and a byte-pattern count, which agree on every site; docs/macos.md).
// So each executable section is walked one instruction at a time (src/x86_length.h) from its start. A .pdata
// BeginAddress is a known instruction start and restarts the walk there; between functions the walk goes on from the
// end of the previous one. A site outside a function is rewritten only once the walk has decoded kInSync valid
// instructions in a row before it, so a walk that just went through data (a jump table, padding the decoder misreads)
// cannot patch the middle of another instruction.
//
// A fault in a stub's memory read (or in PUSHFQ, on the stack) is moved back to the original instruction by a vectored
// handler (fault_to_site), with RSP and RFLAGS as they were there, so the title's own handlers and the unwinder see what
// the CPU would have raised. Not covered: a stack walk taken while a thread is inside a stub (the stub has no unwind
// data; a sampling profiler or a debugger would stop there).
#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

#include "cpu_emulation.h"
#include "x86_length.h"

namespace rt::adx_rewrite {

namespace ce = rt::cpu_emulation;

struct Range {
    uint32_t begin = 0, end = 0;  // offsets from the image base, end exclusive
};

struct Site {
    uint32_t offset = 0;          // of the instruction, from the image base
    ce::Decoded d;
    bool in_function = false;     // inside a .pdata range
};

struct Sweep {
    std::vector<Site> sites;
    std::size_t instructions = 0;
    std::size_t invalid = 0;      // bytes the walk stepped over one at a time
    std::size_t unsynced = 0;     // ADCX/ADOX outside a function, left alone: too soon after data
};

constexpr std::size_t kInSync = 4;

// Finds the ADCX/ADOX of `sections` (executable, within `size` bytes at `image`), restarting at each `functions`
// begin. `functions` need not be sorted; ranges outside `size` are ignored.
inline Sweep find_sites(const uint8_t* image, std::size_t size, std::vector<Range> sections,
                        std::vector<Range> functions) {
    Sweep out;
    functions.erase(std::remove_if(functions.begin(), functions.end(),
                                   [&](const Range& r) { return r.begin >= r.end || r.end > size; }),
                    functions.end());
    std::sort(functions.begin(), functions.end(), [](const Range& a, const Range& b) { return a.begin < b.begin; });
    for (Range sec : sections) {
        if (sec.end > size) sec.end = static_cast<uint32_t>(size);
        auto f = std::lower_bound(functions.begin(), functions.end(), sec.begin,
                                  [](const Range& r, uint32_t at) { return r.end <= at; });
        std::size_t run = 0;  // valid instructions in a row
        for (uint32_t pos = sec.begin; pos < sec.end;) {
            while (f != functions.end() && f->end <= pos) ++f;
            const bool inside = f != functions.end() && f->begin <= pos;
            if (f != functions.end() && f->begin == pos) run = kInSync;  // a function start is an instruction start
            // An instruction does not run into the next function, nor past the end of its own.
            const uint32_t limit = inside ? f->end : (f != functions.end() ? std::min(f->begin, sec.end) : sec.end);
            const std::size_t avail = std::min<std::size_t>(15, limit - pos);
            const std::size_t n = rt::x86::length(image + pos, avail);
            if (!n) {
                ++out.invalid;
                run = 0;
                ++pos;
                continue;
            }
            const ce::Decoded d = ce::decode(image + pos, avail, true);
            if ((d.op == ce::Op::adcx || d.op == ce::Op::adox) && d.length == n) {
                if (inside || run >= kInSync) out.sites.push_back({pos, d, inside});
                else ++out.unsynced;
            }
            ++out.instructions;
            ++run;
            pos += static_cast<uint32_t>(n);
        }
    }
    return out;
}

constexpr std::size_t kStubSize = 64;  // bytes per stub; the longest is 43

inline bool fits_rel32(int64_t v) { return v >= INT32_MIN && v <= INT32_MAX; }

// Where the stub's ADC is, and so where a fault in it lands.
inline std::size_t adc_offset(ce::Op op) { return op == ce::Op::adox ? 7 : 1; }

// `adc dst, src` with the source of `d`, its displacement replaced by `disp`. Returns the bytes written.
inline std::size_t encode_adc(const ce::Decoded& d, int64_t disp, uint8_t* out) {
    std::size_t n = 0;
    if (d.memory && d.segment) out[n++] = d.segment;
    if (d.memory && d.addr32) out[n++] = 0x67;
    uint8_t rex = uint8_t(0x40 | (d.wide ? 8 : 0) | ((d.dst & 8) ? 4 : 0));
    if (!d.memory) rex |= (d.src & 8) ? 1 : 0;
    else rex |= uint8_t((d.mem.index >= 8 ? 2 : 0) | (d.mem.base >= 8 ? 1 : 0));
    if (rex != 0x40) out[n++] = rex;
    out[n++] = 0x13;  // ADC r, r/m
    const uint8_t reg = uint8_t((d.dst & 7) << 3);
    if (!d.memory) {
        out[n++] = uint8_t(0xC0 | reg | (d.src & 7));
        return n;
    }
    auto put32 = [&](int64_t v) {
        const int32_t v32 = static_cast<int32_t>(v);
        std::memcpy(out + n, &v32, 4);
        n += 4;
    };
    const uint8_t scale = uint8_t(d.mem.scale == 8 ? 3 : d.mem.scale == 4 ? 2 : d.mem.scale == 2 ? 1 : 0);
    const uint8_t index = uint8_t(d.mem.index >= 0 ? (d.mem.index & 7) : 4);
    if (d.mem.rip) {
        out[n++] = uint8_t(0x05 | reg);
        put32(disp);
        return n;
    }
    if (d.mem.base < 0) {  // [index * scale + disp32] or [disp32], through a SIB with no base
        out[n++] = uint8_t(0x04 | reg);
        out[n++] = uint8_t((scale << 6) | (index << 3) | 5);
        put32(disp);
        return n;
    }
    const int base = d.mem.base & 7;
    const bool sib = d.mem.index >= 0 || base == 4;
    const int mod = (disp == 0 && base != 5) ? 0 : (disp >= -128 && disp <= 127) ? 1 : 2;
    out[n++] = uint8_t((mod << 6) | reg | (sib ? 4 : base));
    if (sib) out[n++] = uint8_t((scale << 6) | (index << 3) | base);
    if (mod == 1) out[n++] = uint8_t(int8_t(disp));
    else if (mod == 2) put32(disp);
    return n;
}

// The stub for the instruction `d` at address `site`, to run at address `stub`: its size, or 0 with `*why` set when
// this form is left to the #UD path (RSP as dst or register source, EIP-relative, out of rel32 reach).
inline std::size_t encode_stub(const ce::Decoded& d, uint64_t site, uint64_t stub, uint8_t (&out)[kStubSize],
                               const char** why = nullptr) {
    auto refuse = [&](const char* reason) -> std::size_t {
        if (why) *why = reason;
        return 0;
    };
    if (d.op != ce::Op::adcx && d.op != ce::Op::adox) return refuse("not ADCX or ADOX");
    if (d.dst == 4 || (!d.memory && d.src == 4)) return refuse("RSP is an operand");
    if (d.memory && d.mem.rip && d.addr32) return refuse("EIP-relative source");
    std::size_t n = 0;
    out[n++] = 0x9C;  // pushfq
    if (d.op == ce::Op::adox) {
        static constexpr uint8_t bt[] = {0x48, 0x0F, 0xBA, 0x24, 0x24, 0x0B};  // bt qword [rsp], 11
        std::memcpy(out + n, bt, sizeof bt);
        n += sizeof bt;
    }
    int64_t disp = d.memory ? d.mem.disp : 0;
    if (d.memory && d.mem.base == 4) disp += 8;  // PUSHFQ moved RSP
    if (d.memory && d.mem.rip) {
        // The ADC's length with a disp32 does not depend on its value: encode once to measure, then for real.
        uint8_t probe[16];
        const std::size_t len = encode_adc(d, 0, probe);
        const uint64_t target = site + d.length + uint64_t(d.mem.disp);
        disp = int64_t(target - (stub + n + len));
    }
    if (!fits_rel32(disp)) return refuse("displacement out of reach");
    n += encode_adc(d, disp, out + n);
    if (d.op == ce::Op::adcx) {
        static constexpr uint8_t tail[] = {
            0x72, 0x07,                    // jc 1f
            0x48, 0x83, 0x24, 0x24, 0xFE,  // and qword [rsp], ~1
            0xEB, 0x05,                    // jmp 2f
            0x48, 0x83, 0x0C, 0x24, 0x01,  // 1: or qword [rsp], 1
            0x9D,                          // 2: popfq
        };
        std::memcpy(out + n, tail, sizeof tail);
        n += sizeof tail;
    } else {
        static constexpr uint8_t tail[] = {
            0x72, 0x0A,                                      // jc 1f
            0x48, 0x81, 0x24, 0x24, 0xFF, 0xF7, 0xFF, 0xFF,  // and qword [rsp], ~0x800
            0xEB, 0x08,                                      // jmp 2f
            0x48, 0x81, 0x0C, 0x24, 0x00, 0x08, 0x00, 0x00,  // 1: or qword [rsp], 0x800
            0x9D,                                            // 2: popfq
        };
        std::memcpy(out + n, tail, sizeof tail);
        n += sizeof tail;
    }
    const int64_t back = int64_t((site + d.length) - (stub + n + 5));
    if (!fits_rel32(back)) return refuse("the jump back is out of reach");
    out[n++] = 0xE9;
    const int32_t back32 = static_cast<int32_t>(back);
    std::memcpy(out + n, &back32, 4);
    n += 4;
    return n;
}

// The bytes that replace the instruction (`length` of them) at `site`: a jump to `stub`, then INT3s. False when the
// stub is out of rel32 reach or the instruction is shorter than the jump (an ADCX/ADOX never is).
inline bool encode_jump(uint64_t site, uint64_t stub, std::size_t length, uint8_t* out) {
    const int64_t rel = int64_t(stub - (site + 5));
    if (length < 5 || !fits_rel32(rel)) return false;
    out[0] = 0xE9;
    const int32_t rel32 = static_cast<int32_t>(rel);
    std::memcpy(out + 1, &rel32, 4);
    for (std::size_t i = 5; i < length; ++i) out[i] = 0xCC;
    return true;
}

// A fault at `rip` inside the stub at `stub` for an instruction `op` at `site`: the registers the original instruction
// would have faulted with. PUSHFQ (offset 0) faults before it moves RSP; past it, the stub's slot at [rsp] holds the
// flags of the original instruction. `read_stack(rsp)` reads that qword. False when `rip` is not a stub instruction
// that can fault (it then keeps its values).
template <class ReadStack>
inline bool fault_to_site(uint64_t rip, uint64_t stub, uint64_t site, ce::Op op, uint64_t& rsp, uint64_t& rflags,
                          uint64_t& out_rip, ReadStack&& read_stack) {
    const uint64_t at = rip - stub;
    if (at == 0) {
        out_rip = site;
        return true;
    }
    if (at >= 1 && at <= adc_offset(op)) {  // BT (ADOX) or the ADC
        rflags = read_stack(rsp);
        rsp += 8;
        out_rip = site;
        return true;
    }
    return false;
}

struct Installed {
    std::size_t rewritten = 0;
    std::size_t in_functions = 0;  // of those, inside .pdata ranges
    std::size_t refused = 0;       // forms left to the #UD path (encode_stub)
};

// Writes a stub for each of `sites` into `stubs` (kStubSize bytes each, in order; executable memory within rel32 reach
// of `image`, writable until this returns) and the jump over each site. `site_addresses` and `ops` (one entry per
// site) receive what fault_to_site needs. A refused site keeps its instruction and its stub slot stays INT3.
inline Installed install(uint8_t* image, const std::vector<Site>& sites, uint8_t* stubs, uint64_t* site_addresses,
                         uint8_t* ops) {
    Installed r;
    std::memset(stubs, 0xCC, sites.size() * kStubSize);
    for (std::size_t k = 0; k < sites.size(); ++k) {
        const Site& s = sites[k];
        const uint64_t at = reinterpret_cast<uint64_t>(image) + s.offset;
        const uint64_t stub = reinterpret_cast<uint64_t>(stubs) + k * kStubSize;
        site_addresses[k] = at;
        ops[k] = uint8_t(s.d.op);
        uint8_t code[kStubSize];
        uint8_t jump[15];
        const std::size_t len = encode_stub(s.d, at, stub, code);
        if (!len || !encode_jump(at, stub, s.d.length, jump)) {
            ++r.refused;
            continue;
        }
        std::memcpy(stubs + k * kStubSize, code, len);
        std::memcpy(image + s.offset, jump, s.d.length);
        ++r.rewritten;
        if (s.in_function) ++r.in_functions;
    }
    return r;
}

}  // namespace rt::adx_rewrite

#ifdef _WIN32
#include <windows.h>

#include <atomic>
#include <cstdio>

#include "code_hooks.h"

namespace rt::adx_rewrite {

// LEONIDA_ADX_REWRITE: unset or "auto" rewrites when CPUID has no ADX; "1" always (to run the stubs on a CPU that has
// ADX, for a comparison); "0" never (the #UD path of src/cpu_emulation.h stays).
inline bool wanted() {
    char v[8] = {};
    std::size_t n = 0;
    if (getenv_s(&n, v, "LEONIDA_ADX_REWRITE") == 0 && n > 1) {
        if (v[0] == '0') return false;
        if (v[0] == '1') return true;
    }
    return !ce::detect_host().adx;
}

struct Pool {
    uint8_t* base = nullptr;
    std::size_t count = 0;            // stubs
    const uint64_t* sites = nullptr;  // per stub: the original instruction's address
    const uint8_t* ops = nullptr;     // per stub: ce::Op
};

constexpr std::size_t kMaxPools = 64;

inline Pool* pools() {
    static Pool table[kMaxPools];
    return table;
}

inline std::atomic<std::size_t>& pool_count() {
    static std::atomic<std::size_t> n{0};
    return n;
}

// Moves a fault in a stub back to its instruction (top of this file). Runs first in the vectored chain and hands the
// exception on, so the next handlers see it at the title's own address.
inline LONG CALLBACK fault_handler(EXCEPTION_POINTERS* info) {
    CONTEXT* c = info->ContextRecord;
    const uint64_t rip = c->Rip;
    const std::size_t n = pool_count().load(std::memory_order_acquire);
    for (std::size_t p = 0; p < n; ++p) {
        const Pool& pool = pools()[p];
        const uint64_t base = reinterpret_cast<uint64_t>(pool.base);
        if (rip < base || rip >= base + pool.count * kStubSize) continue;
        const std::size_t k = std::size_t((rip - base) / kStubSize);
        uint64_t rsp = c->Rsp, rflags = c->EFlags, to = rip;
        const auto read_stack = [](uint64_t at) { return *reinterpret_cast<const uint64_t*>(at); };
        if (!fault_to_site(rip, base + k * kStubSize, pool.sites[k], ce::Op(pool.ops[k]), rsp, rflags, to, read_stack))
            return EXCEPTION_CONTINUE_SEARCH;
        c->Rsp = rsp;
        c->EFlags = DWORD(rflags);
        c->Rip = to;
        info->ExceptionRecord->ExceptionAddress = reinterpret_cast<void*>(to);
        return EXCEPTION_CONTINUE_SEARCH;
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

struct Result : Installed {
    std::size_t unsynced = 0;      // Sweep::unsynced
    uint8_t* stubs = nullptr;
    const char* error = nullptr;
};

// Rewrites every site of `image` (`size` bytes, its executable sections still writable: before the loader's
// protect_sections). Stubs go in executable memory within rel32 reach of the image. The loader calls it with its lock
// held, so two images are never rewritten at once.
inline Result rewrite(uint8_t* image, std::size_t size, const std::vector<Range>& sections,
                      const std::vector<Range>& functions) {
    Result r;
    const Sweep sweep = find_sites(image, size, sections, functions);
    r.unsynced = sweep.unsynced;
    if (sweep.sites.empty()) return r;
    static const bool handler = AddVectoredExceptionHandler(1, &fault_handler) != nullptr;
    if (!handler) {
        r.error = "AddVectoredExceptionHandler failed";
        return r;
    }
    const std::size_t slot = pool_count().load(std::memory_order_relaxed);
    if (slot >= kMaxPools) {
        r.error = "too many rewritten images";
        return r;
    }
    const std::size_t count = sweep.sites.size();
    uint8_t* stubs = leonida::hooks::alloc_near(image, count * kStubSize);
    if (!stubs) {
        r.error = "no executable memory within 2 GiB of the image";
        return r;
    }
    auto* sites = new uint64_t[count];  // kept for the process: the handler reads them
    auto* ops = new uint8_t[count];
    static_cast<Installed&>(r) = install(image, sweep.sites, stubs, sites, ops);
    DWORD old = 0;
    VirtualProtect(stubs, count * kStubSize, PAGE_EXECUTE_READ, &old);
    FlushInstructionCache(GetCurrentProcess(), stubs, count * kStubSize);
    FlushInstructionCache(GetCurrentProcess(), image, size);
    Pool& pool = pools()[slot];
    pool.base = stubs;
    pool.count = count;
    pool.sites = sites;
    pool.ops = ops;
    pool_count().store(slot + 1, std::memory_order_release);
    r.stubs = stubs;
    return r;
}

}  // namespace rt::adx_rewrite
#endif
