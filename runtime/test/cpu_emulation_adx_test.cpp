// ADCX/ADOX emulation (src/cpu_emulation.h, docs/macos.md) without windows.h, so it runs on every OS: the decoder on
// hand-written encodings, the arithmetic and addressing, apply_adx on a register file, and on an x86-64 host the
// real instructions. A host with ADX runs them itself and its results must match the model; one without it (Rosetta
// 2 on an Apple Silicon Mac, a Haswell) raises SIGILL, and a SIGILL handler here does the work through apply_adx,
// as the runtime's vectored handler does under Wine. The test prints which path it took. No game files.
//
// Exit 0 when every check passes, 1 on a failure.
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <initializer_list>
#include <vector>

#include "cpu_emulation.h"

#if (defined(__x86_64__) && (defined(__GNUC__) || defined(__clang__))) && (defined(__APPLE__) || defined(__linux__))
#define LIVE_POSIX 1
#include <cpuid.h>
#include <signal.h>
#if defined(__APPLE__)
#include <sys/sysctl.h>
#include <sys/ucontext.h>  // <ucontext.h> wants _XOPEN_SOURCE for the deprecated routines; only the type is needed
#else
#include <ucontext.h>
#endif
#endif

namespace ce = rt::cpu_emulation;

namespace {
int failures = 0;

void check(bool ok, const char* what) {
    if (!ok) {
        std::fprintf(stderr, "FAIL %s\n", what);
        ++failures;
    }
}

ce::Decoded dec(std::initializer_list<uint8_t> bytes) {
    std::vector<uint8_t> v(bytes);
    return ce::decode(v.data(), v.size());
}

void test_decode() {
    ce::Decoded d = dec({0x66, 0x0F, 0x38, 0xF6, 0xC1});  // adcx eax, ecx
    check(d.op == ce::Op::adcx && d.length == 5 && d.dst == 0 && d.src == 1 && !d.wide && !d.memory, "adcx r32");
    d = dec({0x66, 0x48, 0x0F, 0x38, 0xF6, 0xC1});  // adcx rax, rcx
    check(d.op == ce::Op::adcx && d.length == 6 && d.wide, "adcx r64");
    d = dec({0xF3, 0x4D, 0x0F, 0x38, 0xF6, 0xC8});  // adox r9, r8
    check(d.op == ce::Op::adox && d.length == 6 && d.wide && d.dst == 9 && d.src == 8, "adox r9, r8 (REX.WRB)");
    d = dec({0xF3, 0x0F, 0x38, 0xF6, 0xFF});  // adox edi, edi
    check(d.op == ce::Op::adox && d.dst == 7 && d.src == 7 && !d.wide, "adox r32");

    d = dec({0x66, 0x48, 0x0F, 0x38, 0xF6, 0x04, 0x24});  // adcx rax, [rsp]
    check(d.op == ce::Op::adcx && d.memory && d.length == 7 && d.mem.base == 4 && d.mem.index == -1 &&
              d.mem.disp == 0 && !d.mem.rip,
          "adcx rax, [rsp]");
    d = dec({0x66, 0x4A, 0x0F, 0x38, 0xF6, 0x44, 0xE1, 0xF8});  // adcx rax, [rcx + r12*8 - 8]
    check(d.memory && d.length == 8 && d.mem.base == 1 && d.mem.index == 12 && d.mem.scale == 8 && d.mem.disp == -8,
          "adcx rax, [rcx+r12*8-8] (REX.X, disp8)");
    d = dec({0xF3, 0x48, 0x0F, 0x38, 0xF6, 0x05, 0x10, 0, 0, 0});  // adox rax, [rip+0x10]
    check(d.op == ce::Op::adox && d.memory && d.mem.rip && d.mem.base == -1 && d.mem.disp == 0x10 && d.length == 10,
          "adox rax, [rip+disp32]");
    d = dec({0x66, 0x41, 0x0F, 0x38, 0xF6, 0x45, 0x00});  // adcx eax, [r13+0]
    check(d.memory && d.mem.base == 13 && !d.mem.rip && d.length == 7, "adcx eax, [r13+0]: mod 1, not RIP-relative");
    d = dec({0x66, 0x0F, 0x38, 0xF6, 0x04, 0x25, 0x00, 0x10, 0, 0});  // adcx eax, [0x1000]
    check(d.memory && d.mem.base == -1 && d.mem.index == -1 && d.mem.disp == 0x1000 && d.length == 10,
          "adcx eax, [disp32] through SIB");
    d = dec({0x66, 0x43, 0x0F, 0x38, 0xF6, 0x04, 0x25, 0x00, 0x10, 0, 0});  // adcx eax, [r12 + 0x1000], base 5 is none
    check(d.memory && d.mem.base == -1 && d.mem.index == 12 && d.length == 11, "SIB base 5 with REX.B is still none");
    d = dec({0x66, 0x0F, 0x38, 0xF6, 0x84, 0x88, 0x00, 0x01, 0, 0});  // adcx eax, [rax + rcx*4 + 0x100]
    check(d.memory && d.mem.base == 0 && d.mem.index == 1 && d.mem.scale == 4 && d.mem.disp == 0x100 &&
              d.length == 10,
          "adcx eax, [rax+rcx*4+disp32]");
    d = dec({0x67, 0x66, 0x0F, 0x38, 0xF6, 0x01});  // adcx eax, [ecx]
    check(d.memory && d.addr32 && d.mem.base == 1 && d.length == 6, "adcx with 67");
    d = dec({0x3E, 0x66, 0x0F, 0x38, 0xF6, 0x01});  // ds: adcx eax, [rcx] (ignored in 64-bit mode)
    check(d.op == ce::Op::adcx && d.length == 6, "a DS override is ignored");
    d = dec({0x65, 0x66, 0x0F, 0x38, 0xF6, 0xC1});  // gs: on a register operand changes nothing
    check(d.op == ce::Op::adcx && d.length == 6, "a GS override on a register operand");

    check(dec({0x0F, 0x38, 0xF6, 0xC1}).op == ce::Op::none, "0F 38 F6 without a prefix is WRSS");
    check(dec({0xF2, 0x0F, 0x38, 0xF6, 0xC1}).op == ce::Op::none, "F2 0F 38 F6 is not ADX");
    check(dec({0x66, 0xF3, 0x0F, 0x38, 0xF6, 0xC1}).op == ce::Op::none, "66 with F3 is left alone");
    check(dec({0xF0, 0x66, 0x0F, 0x38, 0xF6, 0xC1}).op == ce::Op::none, "LOCK adcx stays #UD");
    check(dec({0x65, 0x66, 0x0F, 0x38, 0xF6, 0x01}).op == ce::Op::none, "a GS memory operand stays #UD");
    check(dec({0x64, 0x66, 0x0F, 0x38, 0xF6, 0x01}).op == ce::Op::none, "an FS memory operand stays #UD");
    check(dec({0x66, 0x0F, 0x38, 0xF5, 0xC1}).op == ce::Op::none, "66 0F 38 F5 is not ADX");
    check(dec({0x66, 0x0F, 0x38, 0xF6}).op == ce::Op::none, "a cut-off adcx (no ModRM)");
    check(dec({0x66, 0x0F, 0x38, 0xF6, 0x44, 0x24}).op == ce::Op::none, "a cut-off adcx (no disp8)");
    check(dec({0x66, 0x0F, 0x38, 0xF6, 0x05, 0, 0}).op == ce::Op::none, "a cut-off adcx (short disp32)");

    // The CLWB lengths go through the same addressing parser now.
    check(dec({0x66, 0x0F, 0xAE, 0x31}).length == 4, "clwb [rcx]");
    check(dec({0x66, 0x0F, 0xAE, 0x74, 0x01, 0x08}).length == 6, "clwb [rcx+rax+8]");
    check(dec({0x66, 0x0F, 0xAE, 0xB1, 0, 1, 0, 0}).length == 8, "clwb [rcx+disp32]");
    check(dec({0x66, 0x0F, 0xAE, 0x35, 1, 2, 3, 4}).length == 8, "clwb [rip+disp32]");
    check(dec({0x66, 0x0F, 0xAE, 0x34, 0x25, 1, 2, 3, 4}).length == 9, "clwb [disp32] through SIB");
    check(dec({0x66, 0x0F, 0xAE, 0x74, 0x01}).op == ce::Op::none, "a cut-off clwb is not decoded");
}

void test_arithmetic() {
    bool c = false;
    check(ce::add_with_carry(~0ull, 1, false, true, &c) == 0 && c, "64: carry out of bit 63");
    check(ce::add_with_carry(~0ull, 0, true, true, &c) == 0 && c, "64: the carry in carries out");
    check(ce::add_with_carry(~0ull, ~0ull, true, true, &c) == ~0ull && c, "64: max + max + 1");
    check(ce::add_with_carry(5, 7, true, true, &c) == 13 && !c, "64: no carry");
    check(ce::add_with_carry(0x123400000000FFFFull, 1, false, false, &c) == 0x10000 && !c,
          "32: the upper half of dst is zeroed");
    check(ce::add_with_carry(0xFFFFFFFFu, 0, true, false, &c) == 0 && c, "32: carry out of bit 31");
    check(ce::add_with_carry(0xFFFFFFFFu, 0xFFFFFFFF00000001ull, false, false, &c) == 0 && c,
          "32: only the low half of src counts");

    uint64_t regs[16] = {};
    for (int r = 0; r < 16; ++r) regs[r] = 0x1000ull * (r + 1);
    ce::Mem m;
    m.base = 1;
    m.index = 12;
    m.scale = 8;
    m.disp = -8;
    check(ce::effective_address(m, regs, 0, false) == 0x2000 + 0xD000 * 8 - 8, "base + index*8 + disp");
    m = ce::Mem{};
    m.rip = true;
    m.disp = 0x10;
    check(ce::effective_address(m, regs, 0x7000, false) == 0x7010, "RIP-relative from the next instruction");
    m = ce::Mem{};
    m.base = 0;
    m.disp = -1;
    regs[0] = 0x100000000ull;
    check(ce::effective_address(m, regs, 0, true) == 0xFFFFFFFFull, "67: the address wraps at 32 bits");
}

void test_apply() {
    uint64_t regs[16] = {};
    uint64_t rip = 0x1000;
    // ZF, SF, PF and bit 1 set; CF and OF clear. ADCX may change CF alone, ADOX OF alone.
    const uint64_t others = 0x2 | 0x4 | 0x40 | 0x80;
    uint64_t rflags = others | ce::kFlagCf;
    regs[0] = ~0ull;
    regs[1] = 0;
    auto no_read = [](uint64_t, std::size_t, uint64_t*) { return false; };
    check(ce::apply_adx(dec({0x66, 0x48, 0x0F, 0x38, 0xF6, 0xC1}), regs, rip, rflags, no_read),
          "apply adcx rax, rcx");  // ~0 + 0 + CF
    check(regs[0] == 0 && (rflags & ce::kFlagCf) && rip == 0x1006, "adcx: result, CF out and rip");
    check((rflags & ~ce::kFlagCf) == others, "adcx leaves every other flag");

    rflags = others | ce::kFlagCf;  // CF set but OF clear: ADOX must take OF
    regs[9] = 41;
    regs[8] = 1;
    check(ce::apply_adx(dec({0xF3, 0x4D, 0x0F, 0x38, 0xF6, 0xC8}), regs, rip, rflags, no_read) && regs[9] == 42,
          "adox r9, r8 takes OF, not CF");
    check(rflags == (others | ce::kFlagCf), "adox clears OF and keeps CF");
    rflags = ce::kFlagOf;
    regs[9] = ~0ull;
    regs[8] = 0;
    check(ce::apply_adx(dec({0xF3, 0x4D, 0x0F, 0x38, 0xF6, 0xC8}), regs, rip, rflags, no_read) && regs[9] == 0 &&
              rflags == ce::kFlagOf,
          "adox: OF in carries out to OF");

    // A memory source through the read callback: adcx rax, [rcx + r12*8 - 8].
    regs[0] = 10;
    regs[1] = 0x5000;
    regs[12] = 2;
    rflags = 0;
    uint64_t asked = 0;
    std::size_t asked_size = 0;
    auto read = [&](uint64_t address, std::size_t size, uint64_t* value) {
        asked = address;
        asked_size = size;
        *value = 0xFFFFFFFFFFFFFFF6ull;  // -10
        return true;
    };
    rip = 0x2000;
    check(ce::apply_adx(dec({0x66, 0x4A, 0x0F, 0x38, 0xF6, 0x44, 0xE1, 0xF8}), regs, rip, rflags, read),
          "apply adcx from memory");
    check(asked == 0x5000 + 16 - 8 && asked_size == 8 && regs[0] == 0 && (rflags & ce::kFlagCf) && rip == 0x2008,
          "adcx from memory: address, size, result, CF");

    // RIP-relative reads count from the end of the instruction; a 32-bit form reads 4 bytes.
    rip = 0x3000;
    check(ce::apply_adx(dec({0xF3, 0x0F, 0x38, 0xF6, 0x05, 0x10, 0, 0, 0}), regs, rip, rflags, read) &&
              asked == 0x3000 + 9 + 0x10 && asked_size == 4,
          "adox eax, [rip+0x10] reads 4 bytes after the instruction");

    // A source that cannot be read: nothing changes and the address is reported.
    regs[0] = 7;
    rip = 0x4000;
    rflags = others;
    uint64_t fault = 0;
    check(!ce::apply_adx(dec({0x66, 0x48, 0x0F, 0x38, 0xF6, 0x04, 0x24}), regs, rip, rflags, no_read, &fault),
          "an unreadable source is refused");
    check(regs[0] == 7 && rip == 0x4000 && rflags == others && fault == regs[4], "and leaves the registers alone");
}

#ifdef LIVE_POSIX
// The live path: the general registers of a SIGILL'd thread in encoding order, read and written back.
#if defined(__APPLE__)
#define R(name) uc->uc_mcontext->__ss.__##name
#else
#define R(name) uc->uc_mcontext.gregs[REG_##name]
#endif
#if defined(__APPLE__)
#define GREG(lower, upper) R(lower)
#else
#define GREG(lower, upper) R(upper)
#endif

volatile sig_atomic_t emulated = 0;

void on_sigill(int, siginfo_t*, void* ctx) {
    auto* uc = static_cast<ucontext_t*>(ctx);
    uint64_t* slots[16] = {
        reinterpret_cast<uint64_t*>(&GREG(rax, RAX)), reinterpret_cast<uint64_t*>(&GREG(rcx, RCX)),
        reinterpret_cast<uint64_t*>(&GREG(rdx, RDX)), reinterpret_cast<uint64_t*>(&GREG(rbx, RBX)),
        reinterpret_cast<uint64_t*>(&GREG(rsp, RSP)), reinterpret_cast<uint64_t*>(&GREG(rbp, RBP)),
        reinterpret_cast<uint64_t*>(&GREG(rsi, RSI)), reinterpret_cast<uint64_t*>(&GREG(rdi, RDI)),
        reinterpret_cast<uint64_t*>(&GREG(r8, R8)),   reinterpret_cast<uint64_t*>(&GREG(r9, R9)),
        reinterpret_cast<uint64_t*>(&GREG(r10, R10)), reinterpret_cast<uint64_t*>(&GREG(r11, R11)),
        reinterpret_cast<uint64_t*>(&GREG(r12, R12)), reinterpret_cast<uint64_t*>(&GREG(r13, R13)),
        reinterpret_cast<uint64_t*>(&GREG(r14, R14)), reinterpret_cast<uint64_t*>(&GREG(r15, R15)),
    };
    auto* rip_slot = reinterpret_cast<uint64_t*>(&GREG(rip, RIP));
#if defined(__APPLE__)
    auto* flags_slot = reinterpret_cast<uint64_t*>(&R(rflags));
#else
    auto* flags_slot = reinterpret_cast<uint64_t*>(&R(EFL));
#endif
    uint8_t code[15];
    std::memcpy(code, reinterpret_cast<const void*>(*rip_slot), sizeof code);  // test code: never at a page end
    const ce::Decoded d = ce::decode(code, sizeof code);
    uint64_t regs[16];
    for (int r = 0; r < 16; ++r) regs[r] = *slots[r];
    uint64_t rip = *rip_slot, rflags = *flags_slot;
    auto read = [](uint64_t address, std::size_t size, uint64_t* value) {
        *value = 0;
        std::memcpy(value, reinterpret_cast<const void*>(address), size);
        return true;
    };
    if (!ce::apply_adx(d, regs, rip, rflags, read)) {
        signal(SIGILL, SIG_DFL);  // not ours: fault again with the default action
        return;
    }
    *slots[d.dst] = regs[d.dst];
    *rip_slot = rip;
    *flags_slot = rflags;
    emulated = emulated + 1;
}

bool host_has_adx() {
    unsigned a = 0, b = 0, c = 0, dx = 0;
    if (__get_cpuid_max(0, nullptr) < 7) return false;
    __cpuid_count(7, 0, a, b, c, dx);
    return (b >> 19) & 1;
}

// A 256-bit add as a compiler writes it with ADX: two carry chains, ADCX on CF and ADOX on OF, here each over four
// limbs: sum_c = x + y (ADCX chain), sum_o = x + z (ADOX chain), interleaved, with register and memory sources.
// The flags start from POPFQ rather than the usual `xor reg, reg`: Rosetta 2 hands a SIGILL handler wrong CF and OF
// after most instructions but not after POPFQ (rosetta_sigill_flags below), and this checks the emulation, not that.
// The stack is moved past the System V red zone first, which a leaf function may keep locals in.
struct Limbs { uint64_t x[4], y[4], z[4], sum_c[4], sum_o[4]; uint8_t cf, of; };
// The memory sources are addressed off one pointer register: a debug build keeps RBP as its frame pointer, which
// leaves too few registers for one memory operand each.
static_assert(offsetof(Limbs, y) == 32 && offsetof(Limbs, z) == 64 && offsetof(Limbs, cf) == 160 &&
              offsetof(Limbs, of) == 161, "the offsets in chains()");

__attribute__((noinline)) void chains(Limbs* l) {
    uint64_t c0 = l->x[0], c1 = l->x[1], c2 = l->x[2], c3 = l->x[3];
    uint64_t o0 = l->x[0], o1 = l->x[1], o2 = l->x[2], o3 = l->x[3];
    uint64_t y1 = l->y[1], z2 = l->z[2];
    __asm__ volatile(
        "lea -128(%%rsp), %%rsp\n\t"
        "pushq $2\n\t"  // CF = OF = 0
        "popfq\n\t"
        "lea 128(%%rsp), %%rsp\n\t"
        "adcx 32(%[p]), %[c0]\n\t"  // y[0]
        "adox 64(%[p]), %[o0]\n\t"  // z[0]
        "adcx %[y1], %[c1]\n\t"
        "adox 72(%[p]), %[o1]\n\t"  // z[1]
        "adcx 48(%[p]), %[c2]\n\t"  // y[2]
        "adox %[z2], %[o2]\n\t"
        "adcx 56(%[p]), %[c3]\n\t"  // y[3]
        "adox 88(%[p]), %[o3]\n\t"  // z[3]
        "setc 160(%[p])\n\t"
        "seto 161(%[p])"
        : [c0] "+r"(c0), [c1] "+r"(c1), [c2] "+r"(c2), [c3] "+r"(c3), [o0] "+r"(o0), [o1] "+r"(o1), [o2] "+r"(o2),
          [o3] "+r"(o3)
        : [p] "r"(l), [y1] "r"(y1), [z2] "r"(z2)
        : "cc", "memory");
    l->sum_c[0] = c0, l->sum_c[1] = c1, l->sum_c[2] = c2, l->sum_c[3] = c3;
    l->sum_o[0] = o0, l->sum_o[1] = o1, l->sum_o[2] = o2, l->sum_o[3] = o3;
}

__attribute__((noinline)) uint32_t adcx32(uint32_t a, uint32_t b, bool carry_in, uint8_t* carry_out) {
    uint64_t wide = 0xDEAD000000000000ull | a;  // the upper half must be zeroed by the 32-bit form
    uint8_t c = 0;
    __asm__ volatile("lea -128(%%rsp), %%rsp\n\tpushq %[fl]\n\tpopfq\n\tlea 128(%%rsp), %%rsp\n\t"
                     "adcx %k[b], %k[w]\n\tsetc %[c]"
                     : [w] "+r"(wide), [c] "=&r"(c)
                     : [b] "r"(b), [fl] "r"(uint64_t(2 | (carry_in ? ce::kFlagCf : 0)))
                     : "cc");
    *carry_out = c;
    return wide >> 32 ? 0xBAD : uint32_t(wide);
}

// CF as the ADCX after `stc; xor ecx, ecx` leaves it: the XOR clears CF, so 0 + 0 + CF is 0 with no carry. A host
// whose SIGILL context carries the real flags gives 0; Rosetta 2 hands the handler the stale CF=1 (docs/macos.md).
__attribute__((noinline)) uint64_t adcx_after_xor() {
    uint64_t a = 0;
    __asm__ volatile("stc\n\txor %%ecx, %%ecx\n\tadcx %%rcx, %[a]" : [a] "+r"(a) : : "rcx", "cc");
    return a;
}

bool translated_by_rosetta() {
#if defined(__APPLE__)
    int translated = 0;
    std::size_t size = sizeof translated;
    return sysctlbyname("sysctl.proc_translated", &translated, &size, nullptr, 0) == 0 && translated == 1;
#else
    return false;
#endif
}

void model_chain(const uint64_t* a, const uint64_t* b, uint64_t* out, uint64_t* carry) {
    bool c = false;
    for (int i = 0; i < 4; ++i) out[i] = ce::add_with_carry(a[i], b[i], c, true, &c);
    *carry = c;
}

void test_live() {
    const bool native = host_has_adx();
    struct sigaction sa {};
    sa.sa_sigaction = on_sigill;
    sa.sa_flags = SA_SIGINFO;
    sigemptyset(&sa.sa_mask);
    check(sigaction(SIGILL, &sa, nullptr) == 0, "install the SIGILL handler");

    const uint64_t patterns[][4] = {
        {~0ull, ~0ull, ~0ull, ~0ull}, {1, 0, 0, 0}, {0x8000000000000000ull, ~0ull, 0, 0x7FFFFFFFFFFFFFFFull},
        {0x0123456789ABCDEFull, 0xFEDCBA9876543210ull, 0xFFFFFFFF00000000ull, 0x00000000FFFFFFFFull},
    };
    int cases = 0;
    for (const auto& x : patterns)
        for (const auto& y : patterns)
            for (const auto& z : patterns) {
                Limbs l{};
                std::memcpy(l.x, x, sizeof l.x);
                std::memcpy(l.y, y, sizeof l.y);
                std::memcpy(l.z, z, sizeof l.z);
                chains(&l);
                uint64_t want_c[4], want_o[4], cf = 0, of = 0;
                model_chain(l.x, l.y, want_c, &cf);
                model_chain(l.x, l.z, want_o, &of);
                check(std::memcmp(l.sum_c, want_c, sizeof want_c) == 0 && l.cf == cf, "ADCX chain matches the model");
                check(std::memcmp(l.sum_o, want_o, sizeof want_o) == 0 && l.of == of, "ADOX chain matches the model");
                ++cases;
            }
    for (uint32_t a : {0u, 1u, 0x7FFFFFFFu, 0xFFFFFFFFu})
        for (uint32_t b : {0u, 1u, 0x80000000u, 0xFFFFFFFFu})
            for (bool cin : {false, true}) {
                uint8_t cout = 0;
                bool want_c = false;
                const uint64_t want = ce::add_with_carry(a, b, cin, false, &want_c);
                check(adcx32(a, b, cin, &cout) == want && cout == want_c, "32-bit adcx matches and zero-extends");
                ++cases;
            }
    std::printf("adcx/adox: %s (%d cases, %d instructions emulated)\n", native ? "native" : "emulated", cases,
                int(emulated));
    if (native) check(emulated == 0, "a host with ADX runs the instructions itself");
    else check(emulated > 0, "a host without ADX went through the handler");

    const uint64_t after_xor = adcx_after_xor();
    if (native || !translated_by_rosetta()) {
        check(after_xor == 0, "the SIGILL context carries the flags the XOR set");
    } else {
        // Measured, not asserted: the handler can only be as exact as the flags it is given.
        std::printf("rosetta_sigill_flags: adcx after `stc; xor` gives %llu (exact flags give 0)%s\n",
                    static_cast<unsigned long long>(after_xor),
                    after_xor ? "; Rosetta 2 passed the stale CF to the handler, see docs/macos.md" : "");
    }
}
#endif
}  // namespace

int main() {
    test_decode();
    test_arithmetic();
    test_apply();
#ifdef LIVE_POSIX
    test_live();
#else
    std::printf("adcx/adox: live run skipped (not an x86-64 POSIX host; the Windows handler is cpu_emulation_test)\n");
#endif
    if (failures) {
        std::fprintf(stderr, "%d failure(s)\n", failures);
        return 1;
    }
    std::printf("cpu_emulation_adx: all checks passed\n");
    return 0;
}
