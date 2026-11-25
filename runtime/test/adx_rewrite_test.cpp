// ADCX/ADOX rewritten to stubs before they run (src/adx_rewrite.h) and the instruction lengths the sweep walks with
// (src/x86_length.h). Runs on every OS: lengths of hand-written encodings, the sweep's sync rule, the stub bytes and
// the fault mapping. On an x86-64 host (Linux, macOS, Rosetta 2, Windows, Wine) it also rewrites three routines in an
// executable buffer and runs them: every result and every flag must match the model, on a CPU without ADX too. Under
// Rosetta that includes the `xor reg, reg; adcx` chain whose trap-path result is wrong (docs/macos.md). On Windows it
// also faults a stub's memory read and checks that the title's handler sees it at the original instruction, with its
// RSP and flags. The routines are our own code. No game files.
//
// Exit 0 when every check passes, 1 on a failure.
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <initializer_list>
#include <vector>

#include "adx_rewrite.h"
#include "x86_length.h"

#if defined(__x86_64__) || defined(_M_X64)
#define LIVE 1
#if defined(_WIN32)
#include <windows.h>
#else
#include <sys/mman.h>
#if defined(__APPLE__)
#include <sys/sysctl.h>
#endif
#endif
#if !defined(_WIN32)
#include <cpuid.h>  // Windows asks CPUID through cpu_emulation.h's detect_host (intrin.h's __cpuid)
#endif
#endif

namespace ar = rt::adx_rewrite;
namespace ce = rt::cpu_emulation;

namespace {
int failures = 0;

void check(bool ok, const char* what) {
    if (!ok) {
        std::fprintf(stderr, "FAIL %s\n", what);
        ++failures;
    }
}

std::size_t len(std::initializer_list<uint8_t> bytes) {
    std::vector<uint8_t> v(bytes);
    return rt::x86::length(v.data(), v.size());
}

// Lengths from llvm-mc's encodings (llvm-objdump -d over `.byte` lines); test/check_x86_length.py compares random
// cases with it when llvm-mc is installed.
void test_lengths() {
    check(len({0x90}) == 1, "nop");
    check(len({0xC3}) == 1, "ret");
    check(len({0x48, 0x89, 0xD8}) == 3, "mov rax, rbx");
    check(len({0x48, 0x8B, 0x05, 1, 2, 3, 4}) == 7, "mov rax, [rip+disp32]");
    check(len({0x48, 0x8B, 0x44, 0x24, 0x08}) == 5, "mov rax, [rsp+8]");
    check(len({0x48, 0x8B, 0x04, 0x25, 1, 2, 3, 4}) == 8, "mov rax, [disp32] through SIB");
    check(len({0x48, 0x8B, 0x84, 0xC8, 1, 2, 3, 4}) == 8, "mov rax, [rax+rcx*8+disp32]");
    check(len({0x48, 0xB8, 1, 2, 3, 4, 5, 6, 7, 8}) == 10, "movabs rax, imm64");
    check(len({0xB8, 1, 2, 3, 4}) == 5, "mov eax, imm32");
    check(len({0x66, 0xB8, 1, 2}) == 4, "mov ax, imm16");
    check(len({0x66, 0x48, 0xB8, 1, 2, 3, 4, 5, 6, 7, 8}) == 11, "REX.W wins over 66 for imm64");
    check(len({0x48, 0x81, 0xEC, 0x88, 0, 0, 0}) == 7, "sub rsp, imm32");
    check(len({0x66, 0x81, 0xC1, 1, 2}) == 5, "add cx, imm16");
    check(len({0x83, 0xC0, 0x40}) == 3, "add eax, imm8");
    check(len({0xF6, 0xC1, 0x01}) == 3, "test cl, imm8");
    check(len({0xF7, 0xC1, 1, 2, 3, 4}) == 6, "test ecx, imm32");
    check(len({0xF7, 0xD9}) == 2, "neg ecx: no immediate");
    check(len({0xE8, 1, 2, 3, 4}) == 5, "call rel32");
    check(len({0x0F, 0x84, 1, 2, 3, 4}) == 6, "je rel32");
    check(len({0x74, 0x05}) == 2, "je rel8");
    check(len({0xC8, 0x10, 0x00, 0x01}) == 4, "enter imm16, imm8");
    check(len({0xA1, 1, 2, 3, 4, 5, 6, 7, 8}) == 9, "mov eax, moffs64");
    check(len({0x67, 0xA1, 1, 2, 3, 4}) == 6, "mov eax, moffs32");
    check(len({0xF0, 0x48, 0x0F, 0xB1, 0x0A}) == 5, "lock cmpxchg [rdx], rcx");
    check(len({0x65, 0x48, 0x8B, 0x04, 0x25, 0x58, 0, 0, 0}) == 9, "mov rax, gs:[58h]");
    check(len({0x0F, 0x01, 0xFC}) == 3, "clzero");
    check(len({0x0F, 0xA2}) == 2, "cpuid");
    check(len({0x0F, 0x1F, 0x44, 0x00, 0x00}) == 5, "nop dword [rax+rax]");
    check(len({0x66, 0x0F, 0x1F, 0x84, 0x00, 0, 0, 0, 0}) == 9, "nop word [rax+rax+0]");
    check(len({0xF3, 0x0F, 0x1E, 0xFA}) == 4, "endbr64");
    check(len({0x0F, 0xBA, 0xE0, 0x05}) == 4, "bt eax, 5");
    check(len({0x66, 0x0F, 0x3A, 0x0F, 0xC1, 0x08}) == 6, "palignr xmm0, xmm1, 8");
    check(len({0x66, 0x0F, 0x38, 0x00, 0xC1}) == 5, "pshufb xmm0, xmm1");
    check(len({0x66, 0x0F, 0x70, 0xC1, 0x1B}) == 5, "pshufd xmm0, xmm1, imm8");
    check(len({0xF2, 0x0F, 0x78, 0xC1, 0x08, 0x10}) == 6, "insertq xmm0, xmm1, imm8, imm8");
    check(len({0x66, 0x0F, 0x78, 0xC0, 0x08, 0x10}) == 6, "extrq xmm0, imm8, imm8 (/0)");
    check(len({0xF2, 0x0F, 0x79, 0xC1}) == 4, "insertq xmm0, xmm1");
    check(len({0xC5, 0xFC, 0x77}) == 3, "vzeroall");
    check(len({0xC5, 0xF8, 0x77}) == 3, "vzeroupper");
    check(len({0xC5, 0xFD, 0xFE, 0xC1}) == 4, "vpaddd ymm0, ymm0, ymm1");
    check(len({0xC4, 0xE2, 0x7D, 0x58, 0x05, 1, 2, 3, 4}) == 9, "vpbroadcastd ymm0, [rip+disp32]");
    check(len({0xC4, 0xE3, 0x7D, 0x38, 0xC1, 0x01}) == 6, "vinserti128 ymm0, ymm0, xmm1, 1");
    check(len({0xC4, 0xE2, 0xF5, 0xB8, 0xC2}) == 5, "vfmadd231pd ymm0, ymm1, ymm2");
    check(len({0xC5, 0xF9, 0x72, 0xD0, 0x04}) == 5, "vpsrld xmm0, xmm0, 4");
    check(len({0xC4, 0xE2, 0xFB, 0xF6, 0xC1}) == 5, "mulx rax, rax, rcx");
    check(len({0x62, 0xF1, 0x7C, 0x48, 0x58, 0xC1}) == 6, "vaddps zmm0, zmm0, zmm1 (EVEX)");
    check(len({0x66, 0x48, 0x0F, 0x38, 0xF6, 0xC1}) == 6, "adcx rax, rcx");
    check(len({0xF3, 0x49, 0x0F, 0x38, 0xF6, 0x53, 0x20}) == 7, "adox rdx, [r11+0x20]");

    check(len({0x06}) == 0, "push es: gone in 64-bit mode");
    check(len({0x9A, 1, 2, 3, 4, 5, 6}) == 0, "far call: gone in 64-bit mode");
    check(len({0xD5, 0x0A}) == 0, "aad: gone in 64-bit mode");
    check(len({0x48, 0xC5, 0xFC, 0x77}) == 0, "REX before VEX is #UD");
    check(len({0x66, 0xC5, 0xFC, 0x77}) == 0, "66 before VEX is #UD");
    check(len({0x48, 0x8B, 0x05, 1, 2}) == 0, "a cut-off disp32");
    check(len({0x66}) == 0, "a prefix alone");
    check(len({0x66, 0x66, 0x66, 0x66, 0x66, 0x66, 0x66, 0x66, 0x66, 0x66, 0x66, 0x48, 0xB8, 1, 2, 3, 4, 5, 6, 7, 8}) ==
              0,
          "more than 15 bytes");
}

ce::Decoded dec(std::initializer_list<uint8_t> bytes) {
    std::vector<uint8_t> v(bytes);
    return ce::decode(v.data(), v.size(), true);
}

// The sweep: a function start is trusted, a gap needs kInSync instructions before a site.
void test_sweep() {
    // Gap code: three NOPs, then ADCX: too soon after the start of the section to trust.
    std::vector<uint8_t> code = {0x90, 0x90, 0x90, 0x66, 0x48, 0x0F, 0x38, 0xF6, 0xC1};
    ar::Sweep s = ar::find_sites(code.data(), code.size(), {{0, uint32_t(code.size())}}, {});
    check(s.sites.empty() && s.unsynced == 1, "a gap site after 3 instructions is left alone");
    code.insert(code.begin(), 0x90);  // four instructions before it now
    s = ar::find_sites(code.data(), code.size(), {{0, uint32_t(code.size())}}, {});
    check(s.sites.size() == 1 && s.sites[0].offset == 4 && !s.sites[0].in_function, "a gap site after 4 instructions");
    // The same three NOPs as a function: its start is a known instruction start.
    code.erase(code.begin());
    s = ar::find_sites(code.data(), code.size(), {{0, uint32_t(code.size())}}, {{0, uint32_t(code.size())}});
    check(s.sites.size() == 1 && s.sites[0].in_function, "a site inside a function is trusted");

    // A function start resyncs a walk that went through data: 0x48 0xB8 swallows 8 bytes as an imm64, which here would
    // hide the ADOX that the function at offset 4 starts with.
    std::vector<uint8_t> data = {0x90, 0x90, 0x48, 0xB8,                    // movabs rax, imm64 (cut by the function)
                                 0xF3, 0x48, 0x0F, 0x38, 0xF6, 0xC1,        // adox rax, rcx: the function's start
                                 0xC3, 0xCC, 0xCC, 0xCC, 0xCC};
    s = ar::find_sites(data.data(), data.size(), {{0, uint32_t(data.size())}}, {{4, 11}});
    check(s.sites.size() == 1 && s.sites[0].offset == 4 && s.sites[0].d.op == ce::Op::adox,
          "a function start restarts the walk");

    // A pattern inside another instruction is not a site: the walk steps over the imm32.
    std::vector<uint8_t> inside = {0x90, 0x90, 0x90, 0x90, 0xB8, 0x66, 0x0F, 0x38, 0xF6, 0x90, 0x90, 0x90};
    s = ar::find_sites(inside.data(), inside.size(), {{0, uint32_t(inside.size())}}, {});
    check(s.sites.empty() && s.unsynced == 0, "ADCX bytes inside an immediate are no site");
    // Invalid bytes reset the count of instructions in sync.
    std::vector<uint8_t> after_bad = {0x90, 0x90, 0x90, 0x90, 0x06, 0x90, 0x66, 0x48, 0x0F, 0x38, 0xF6, 0xC1};
    s = ar::find_sites(after_bad.data(), after_bad.size(), {{0, uint32_t(after_bad.size())}}, {});
    check(s.sites.empty() && s.unsynced == 1 && s.invalid == 1, "a site right after an invalid byte is left alone");
}

std::vector<uint8_t> stub_of(std::initializer_list<uint8_t> insn, uint64_t site, uint64_t stub) {
    uint8_t out[ar::kStubSize];
    const std::size_t n = ar::encode_stub(dec(insn), site, stub, out);
    return std::vector<uint8_t>(out, out + n);
}

bool starts(const std::vector<uint8_t>& v, std::size_t at, std::initializer_list<uint8_t> bytes) {
    return v.size() >= at + bytes.size() && std::equal(bytes.begin(), bytes.end(), v.begin() + std::ptrdiff_t(at));
}

void test_stubs() {
    const uint64_t site = 0x140001000, stub = 0x150000000;
    // adcx rax, rcx -> pushfq; adc rax, rcx; ...; popfq; jmp site+6
    std::vector<uint8_t> s = stub_of({0x66, 0x48, 0x0F, 0x38, 0xF6, 0xC1}, site, stub);
    check(s.size() == 1 + 3 + 15 + 5 && starts(s, 0, {0x9C, 0x48, 0x13, 0xC1}), "adcx rax, rcx: pushfq; adc rax, rcx");
    check(starts(s, 4, {0x72, 0x07, 0x48, 0x83, 0x24, 0x24, 0xFE, 0xEB, 0x05, 0x48, 0x83, 0x0C, 0x24, 0x01, 0x9D}),
          "adcx: CF into the saved flags, popfq");
    int32_t back = 0;
    std::memcpy(&back, s.data() + s.size() - 4, 4);
    check(s[s.size() - 5] == 0xE9 && stub + s.size() + back == site + 6, "the jump back lands after the instruction");
    // adox r9, r8 -> pushfq; bt [rsp], 11; adc r9, r8 (REX.WRB)
    s = stub_of({0xF3, 0x4D, 0x0F, 0x38, 0xF6, 0xC8}, site, stub);
    check(starts(s, 0, {0x9C, 0x48, 0x0F, 0xBA, 0x24, 0x24, 0x0B, 0x4D, 0x13, 0xC8}), "adox r9, r8: bt then adc");
    // 32-bit: adcx eax, edx -> adc eax, edx (no REX)
    s = stub_of({0x66, 0x0F, 0x38, 0xF6, 0xC2}, site, stub);
    check(starts(s, 0, {0x9C, 0x13, 0xC2, 0x72}), "adcx eax, edx: adc eax, edx");
    // adcx rax, [rsp+8] -> adc rax, [rsp+0x10]; [rsp+0x78] -> [rsp+0x80] needs a disp32
    s = stub_of({0x66, 0x48, 0x0F, 0x38, 0xF6, 0x44, 0x24, 0x08}, site, stub);
    check(starts(s, 0, {0x9C, 0x48, 0x13, 0x44, 0x24, 0x10}), "an RSP displacement grows by 8");
    s = stub_of({0x66, 0x48, 0x0F, 0x38, 0xF6, 0x44, 0x24, 0x78}, site, stub);
    check(starts(s, 0, {0x9C, 0x48, 0x13, 0x84, 0x24, 0x80, 0x00, 0x00, 0x00}), "and takes a disp32 past 127");
    // adcx rcx, [r11 + r9*8 + 0x20] keeps its SIB (REX.WXB)
    s = stub_of({0x66, 0x4B, 0x0F, 0x38, 0xF6, 0x4C, 0xCB, 0x20}, site, stub);
    check(starts(s, 0, {0x9C, 0x4B, 0x13, 0x4C, 0xCB, 0x20}), "a SIB with REX.X and REX.B");
    // adcx eax, [r13+0]: base 13 needs mod 1 with disp8 0
    s = stub_of({0x66, 0x41, 0x0F, 0x38, 0xF6, 0x45, 0x00}, site, stub);
    check(starts(s, 0, {0x9C, 0x41, 0x13, 0x45, 0x00}), "[r13] keeps a disp8");
    // adcx eax, [0x1000] (SIB, no base) and gs: adcx eax, [rcx] with 67
    s = stub_of({0x66, 0x0F, 0x38, 0xF6, 0x04, 0x25, 0x00, 0x10, 0, 0}, site, stub);
    check(starts(s, 0, {0x9C, 0x13, 0x04, 0x25, 0x00, 0x10, 0x00, 0x00}), "[disp32] through a SIB with no base");
    s = stub_of({0x65, 0x67, 0x66, 0x0F, 0x38, 0xF6, 0x01}, site, stub);
    check(starts(s, 0, {0x9C, 0x65, 0x67, 0x13, 0x01}), "a GS override and a 67 prefix are kept");
    // adox rax, [rip+0x19] at `site` (10 bytes): the stub reads the same address
    s = stub_of({0xF3, 0x48, 0x0F, 0x38, 0xF6, 0x05, 0x19, 0, 0, 0}, site, stub);
    int32_t disp = 0;
    std::memcpy(&disp, s.data() + 7 + 3, 4);  // pushfq, bt (6), REX, 13, ModRM
    check(starts(s, 7, {0x48, 0x13, 0x05}) && stub + 7 + 7 + disp == site + 10 + 0x19,
          "a RIP-relative source is read from the same address");

    const char* why = nullptr;
    uint8_t out[ar::kStubSize];
    check(!ar::encode_stub(dec({0x66, 0x48, 0x0F, 0x38, 0xF6, 0xE0}), site, stub, out, &why) && why,
          "adcx rsp, rax is left to the #UD path");
    check(!ar::encode_stub(dec({0x66, 0x48, 0x0F, 0x38, 0xF6, 0xC4}), site, stub, out, &why), "adcx rax, rsp too");
    check(!ar::encode_stub(dec({0x66, 0x48, 0x0F, 0x38, 0xF6, 0xC1}), site, site + 0x100000000ull, out, &why),
          "a stub out of rel32 reach is refused");
    // Every stub's instructions decode back to its own length, so a stack walk could step through it.
    for (auto insn : {std::vector<uint8_t>{0x66, 0x48, 0x0F, 0x38, 0xF6, 0xC1},
                      std::vector<uint8_t>{0xF3, 0x48, 0x0F, 0x38, 0xF6, 0x05, 0x19, 0, 0, 0},
                      std::vector<uint8_t>{0x65, 0x67, 0xF3, 0x4B, 0x0F, 0x38, 0xF6, 0x84, 0xE1, 1, 2, 3, 4}}) {
        const std::size_t n = ar::encode_stub(ce::decode(insn.data(), insn.size(), true), site, stub, out);
        std::size_t at = 0, count = 0;
        while (at < n) {
            const std::size_t l = rt::x86::length(out + at, n - at);
            if (!l) break;
            at += l;
            ++count;
        }
        check(n && at == n && count >= 8, "a stub decodes instruction by instruction");
    }

    uint8_t jump[8] = {};
    check(ar::encode_jump(site, stub, 8, jump) && jump[0] == 0xE9 && jump[5] == 0xCC && jump[7] == 0xCC,
          "the site becomes jmp rel32 and INT3 padding");
    check(!ar::encode_jump(site, stub, 4, jump), "no jump into a 4-byte instruction");
}

void test_fault_mapping() {
    const uint64_t stub = 0x150000040, site = 0x140001000;
    uint64_t rsp = 0x7000, rflags = 0x246, to = 0;
    auto stack = [](uint64_t at) { return at == 0x7000 ? uint64_t(0x8D7) : 0; };
    check(ar::fault_to_site(stub, stub, site, ce::Op::adcx, rsp, rflags, to, stack) && to == site && rsp == 0x7000 &&
              rflags == 0x246,
          "a fault in PUSHFQ: RSP and flags as they were");
    check(ar::fault_to_site(stub + 1, stub, site, ce::Op::adcx, rsp, rflags, to, stack) && rsp == 0x7008 &&
              rflags == 0x8D7,
          "a fault in the ADC: RSP back up, the saved flags");
    rsp = 0x7000;
    check(ar::fault_to_site(stub + 7, stub, site, ce::Op::adox, rsp, rflags, to, stack) && rsp == 0x7008,
          "ADOX's ADC is at offset 7");
    check(!ar::fault_to_site(stub + 9, stub, site, ce::Op::adcx, rsp, rflags, to, stack), "past the ADC: not mapped");
}

#ifdef LIVE
// The routines take a State* in r11 (each starts with `mov r11, <first argument>`). Our own code, assembled with
// llvm-mc from the listing beside each.
struct State {
    uint64_t a[4];       // 0: rax, rcx, rdx, r8 in
    uint64_t m[2];       // 32: memory sources
    uint64_t flags_in;   // 48: POPFQ'd before the chain
    uint64_t out[4];     // 56: rax, rcx, rdx, r8 out
    uint64_t flags_out;  // 88: PUSHFQ'd after it
    uint64_t rsp_at_site;  // 96: the fault test's RSP at its ADCX
    uint64_t bad;          // 104: an address that cannot be read
};
static_assert(offsetof(State, flags_out) == 88 && offsetof(State, bad) == 104, "the offsets the routines use");

#if defined(_WIN32)
constexpr uint8_t kEntry[] = {0x49, 0x89, 0xCB};  // mov r11, rcx
#else
constexpr uint8_t kEntry[] = {0x49, 0x89, 0xFB};  // mov r11, rdi
#endif

// mov rax,[r11]; mov rcx,[r11+8]; mov rdx,[r11+16]; mov r8,[r11+24]; mov r9d,1; push [r11+48]; popfq
// adcx rax,rcx; adox rdx,[r11+32]; adcx rcx,[r11+r9*8+32]; adox r8,rax; adcx eax,edx
// pushfq; pop [r11+88]; mov [r11+56..80], rax/rcx/rdx/r8; ret
constexpr uint8_t kChain[] = {
    0x49, 0x8B, 0x03, 0x49, 0x8B, 0x4B, 0x08, 0x49, 0x8B, 0x53, 0x10, 0x4D, 0x8B, 0x43, 0x18, 0x41,
    0xB9, 0x01, 0x00, 0x00, 0x00, 0x41, 0xFF, 0x73, 0x30, 0x9D, 0x66, 0x48, 0x0F, 0x38, 0xF6, 0xC1,
    0xF3, 0x49, 0x0F, 0x38, 0xF6, 0x53, 0x20, 0x66, 0x4B, 0x0F, 0x38, 0xF6, 0x4C, 0xCB, 0x20, 0xF3,
    0x4C, 0x0F, 0x38, 0xF6, 0xC0, 0x66, 0x0F, 0x38, 0xF6, 0xC2, 0x9C, 0x41, 0x8F, 0x43, 0x58, 0x49,
    0x89, 0x43, 0x38, 0x49, 0x89, 0x4B, 0x40, 0x49, 0x89, 0x53, 0x48, 0x4D, 0x89, 0x43, 0x50, 0xC3};
// mov rax,[r11]; mov rcx,[r11+32]; mov rdx,[r11+40]; sub rsp,0x88; mov [rsp+8],rcx; mov [rsp+0x78],rdx
// push [r11+48]; popfq; adcx rax,[rsp+8]; adox rax,[rip+data]; adcx rax,[rsp+0x78]
// pushfq; pop [r11+88]; add rsp,0x88; mov [r11+56],rax; ret; data: dq 0x8000000000000001
constexpr uint8_t kStack[] = {
    0x49, 0x8B, 0x03, 0x49, 0x8B, 0x4B, 0x20, 0x49, 0x8B, 0x53, 0x28, 0x48, 0x81, 0xEC, 0x88, 0x00,
    0x00, 0x00, 0x48, 0x89, 0x4C, 0x24, 0x08, 0x48, 0x89, 0x54, 0x24, 0x78, 0x41, 0xFF, 0x73, 0x30,
    0x9D, 0x66, 0x48, 0x0F, 0x38, 0xF6, 0x44, 0x24, 0x08, 0xF3, 0x48, 0x0F, 0x38, 0xF6, 0x05, 0x19,
    0x00, 0x00, 0x00, 0x66, 0x48, 0x0F, 0x38, 0xF6, 0x44, 0x24, 0x78, 0x9C, 0x41, 0x8F, 0x43, 0x58,
    0x48, 0x81, 0xC4, 0x88, 0x00, 0x00, 0x00, 0x49, 0x89, 0x43, 0x38, 0xC3, 0x01, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x80};
// mov rax,[r11]; mov rcx,[r11+8]; stc; xor edx,edx; adcx rax,rcx; adox rax,rdx; pushfq; pop [r11+88];
// mov [r11+56],rax; ret
constexpr uint8_t kAfterXor[] = {
    0x49, 0x8B, 0x03, 0x49, 0x8B, 0x4B, 0x08, 0xF9, 0x31, 0xD2, 0x66, 0x48, 0x0F, 0x38, 0xF6, 0xC1,
    0xF3, 0x48, 0x0F, 0x38, 0xF6, 0xC2, 0x9C, 0x41, 0x8F, 0x43, 0x58, 0x49, 0x89, 0x43, 0x38, 0xC3};
#if defined(_WIN32)
// mov [r11+96],rsp; mov r10,[r11+104]; push [r11+48]; popfq; adcx rax,[r10]; ret
constexpr uint8_t kFault[] = {0x49, 0x89, 0x63, 0x60, 0x4D, 0x8B, 0x53, 0x68, 0x41, 0xFF, 0x73, 0x30,
                              0x9D, 0x66, 0x49, 0x0F, 0x38, 0xF6, 0x02, 0xC3};
#endif

constexpr std::size_t kRoutine = 0x100;  // each routine's slot in the code buffer
constexpr std::size_t kCode = 0x1000;    // stubs follow the code

bool host_has_adx() {
#if defined(_WIN32)
    return ce::detect_host().adx;
#else
    unsigned a = 0, b = 0, c = 0, d = 0;
    if (__get_cpuid_max(0, nullptr) < 7) return false;
    __cpuid_count(7, 0, a, b, c, d);
    return (b >> 19) & 1;
#endif
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

uint8_t* executable(std::size_t size) {
#if defined(_WIN32)
    return static_cast<uint8_t*>(VirtualAlloc(nullptr, size, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE));
#else
    void* p = mmap(nullptr, size, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANON, -1, 0);
    return p == MAP_FAILED ? nullptr : static_cast<uint8_t*>(p);
#endif
}

// Three routines in one buffer, INT3 between them. Returns the buffer.
uint8_t* lay_out(uint8_t* buf) {
    std::memset(buf, 0xCC, kCode);
    const uint8_t* bodies[] = {kChain, kStack, kAfterXor};
    const std::size_t sizes[] = {sizeof kChain, sizeof kStack, sizeof kAfterXor};
    for (int i = 0; i < 3; ++i) {
        std::memcpy(buf + i * kRoutine, kEntry, sizeof kEntry);
        std::memcpy(buf + i * kRoutine + sizeof kEntry, bodies[i], sizes[i]);
    }
#if defined(_WIN32)
    std::memcpy(buf + 3 * kRoutine, kEntry, sizeof kEntry);
    std::memcpy(buf + 3 * kRoutine + sizeof kEntry, kFault, sizeof kFault);
#endif
    return buf;
}

using Routine = void (*)(State*);
Routine routine(uint8_t* buf, int i) { return reinterpret_cast<Routine>(buf + i * kRoutine); }

constexpr uint64_t kArith = 0x8D5;  // OF, SF, ZF, AF, PF, CF

// Expected results; `f` is the flags the routine POPFQs (CF and OF are the two carries).
void model(int which, const State& in, uint64_t out[4], uint64_t* flags) {
    bool cf = in.flags_in & 1, of = (in.flags_in >> 11) & 1;
    uint64_t rax = in.a[0], rcx = in.a[1], rdx = in.a[2], r8 = in.a[3];
    if (which == 0) {
        rax = ce::add_with_carry(rax, rcx, cf, true, &cf);
        rdx = ce::add_with_carry(rdx, in.m[0], of, true, &of);
        rcx = ce::add_with_carry(rcx, in.m[1], cf, true, &cf);
        r8 = ce::add_with_carry(r8, rax, of, true, &of);
        rax = ce::add_with_carry(rax, rdx, cf, false, &cf);
        *flags = (in.flags_in & ~uint64_t(0x801)) | (cf ? 1 : 0) | (of ? 0x800 : 0);
    } else if (which == 1) {
        rax = ce::add_with_carry(rax, in.m[0], cf, true, &cf);
        rax = ce::add_with_carry(rax, 0x8000000000000001ull, of, true, &of);
        rax = ce::add_with_carry(rax, in.m[1], cf, true, &cf);
        *flags = (in.flags_in & ~uint64_t(0x801)) | (cf ? 1 : 0) | (of ? 0x800 : 0);
    } else {
        // stc; xor edx, edx: CF = OF = 0, ZF = PF = 1, SF = 0 (AF undefined, not compared)
        rax = ce::add_with_carry(rax, rcx, false, true, &cf);
        rax = ce::add_with_carry(rax, 0, false, true, &of);
        *flags = 0x44 | (cf ? 1 : 0) | (of ? 0x800 : 0);
    }
    out[0] = rax, out[1] = rcx, out[2] = rdx, out[3] = r8;
}

int run_cases(uint8_t* buf, const char* path) {
    const uint64_t patterns[] = {0, 1, ~0ull, 0x8000000000000000ull, 0x7FFFFFFFFFFFFFFFull, 0xFFFFFFFF00000000ull,
                                 0x00000000FFFFFFFFull, 0x0123456789ABCDEFull};
    int cases = 0;
    for (int which = 0; which < 3; ++which)
        for (uint64_t flags = 0; flags < 64; ++flags) {
            // CF, PF, AF, ZF, SF, OF from the six bits of `flags`
            const uint64_t f = 0x2 | (flags & 1) | ((flags & 2) << 1) | ((flags & 4) << 2) | ((flags & 8) << 3) |
                               ((flags & 16) << 3) | ((flags & 32) << 6);
            for (std::size_t p = 0; p < 8; ++p) {
                State st{};
                for (int k = 0; k < 4; ++k) st.a[k] = patterns[(p + 3 * k) % 8];
                st.m[0] = patterns[(p + 1) % 8];
                st.m[1] = patterns[(p + 5) % 8];
                st.flags_in = f;
                uint64_t want[4], want_flags = 0;
                model(which, st, want, &want_flags);
                routine(buf, which)(&st);
                const uint64_t mask = which == 2 ? (kArith & ~uint64_t(0x10)) : kArith;
                bool ok = st.out[0] == want[0] && (st.flags_out & mask) == (want_flags & mask);
                if (which == 0) ok = ok && st.out[1] == want[1] && st.out[2] == want[2] && st.out[3] == want[3];
                if (!ok) {
                    std::fprintf(stderr, "FAIL %s routine %d flags %#llx: rax %#llx want %#llx, flags %#llx want %#llx\n",
                                 path, which, static_cast<unsigned long long>(f),
                                 static_cast<unsigned long long>(st.out[0]), static_cast<unsigned long long>(want[0]),
                                 static_cast<unsigned long long>(st.flags_out & mask),
                                 static_cast<unsigned long long>(want_flags & mask));
                    ++failures;
                    if (failures > 20) return cases;
                }
                ++cases;
                if (which == 2) break;  // the xor sets the flags itself; the patterns are enough once
            }
        }
    return cases;
}

#if defined(_WIN32)
// What the title's handler sees of the fault: filled by the filter below.
struct Seen {
    DWORD code;
    uint64_t address, info, rip, rsp, flags;
};
Seen g_seen;

int record(EXCEPTION_POINTERS* e) {
    g_seen.code = e->ExceptionRecord->ExceptionCode;
    g_seen.address = reinterpret_cast<uint64_t>(e->ExceptionRecord->ExceptionAddress);
    g_seen.info = e->ExceptionRecord->NumberParameters >= 2 ? e->ExceptionRecord->ExceptionInformation[1] : 0;
    g_seen.rip = e->ContextRecord->Rip;
    g_seen.rsp = e->ContextRecord->Rsp;
    g_seen.flags = e->ContextRecord->EFlags;
    return EXCEPTION_EXECUTE_HANDLER;
}

void call_guarded(Routine r, State* st) {
    __try {
        r(st);
    } __except (record(GetExceptionInformation())) {
    }
}

// A stub's ADC from memory that cannot be read: the title's handler must see the fault at the ADCX itself, and the
// unwinder must find the routine's return address where the ADCX left it (RSP back up by the 8 bytes of PUSHFQ).
void test_fault(uint8_t* buf) {
    uint8_t* bad = static_cast<uint8_t*>(VirtualAlloc(nullptr, 4096, MEM_RESERVE | MEM_COMMIT, PAGE_NOACCESS));
    State st{};
    st.flags_in = 0x2 | 0x1 | 0x40 | 0x800;
    st.bad = reinterpret_cast<uint64_t>(bad);
    g_seen = Seen{};
    const uint64_t site = reinterpret_cast<uint64_t>(buf) + 3 * kRoutine + sizeof kEntry + 13;
    check(buf[site - reinterpret_cast<uint64_t>(buf)] == 0xE9, "the fault routine's ADCX was rewritten");
    call_guarded(routine(buf, 3), &st);
    check(g_seen.code == DWORD(STATUS_ACCESS_VIOLATION), "the read faults");
    check(g_seen.rip == site && g_seen.address == site, "at the ADCX, not in the stub");
    check(g_seen.rsp == st.rsp_at_site, "with the RSP of the ADCX");
    check((g_seen.flags & kArith) == (st.flags_in & kArith), "and its flags");
    check(g_seen.info == st.bad, "naming the address it read");
    VirtualFree(bad, 0, MEM_RELEASE);
}
#endif

void test_live() {
    const bool adx = host_has_adx(), rosetta = translated_by_rosetta();
    const std::size_t total = kCode + 64 * ar::kStubSize;
    if (adx) {
        // The routines as written, on a CPU that has ADX: the model must be the CPU's.
        uint8_t* plain = executable(total);
        check(plain != nullptr, "an executable buffer");
        if (plain) std::printf("adx_rewrite: %d cases native\n", run_cases(lay_out(plain), "native"));
    }
    uint8_t* buf = executable(total);
    check(buf != nullptr, "an executable buffer for the rewrite");
    if (!buf) return;
    lay_out(buf);
    // Routine 0 as a .pdata function, the others as leaf code between functions.
    const std::vector<ar::Range> sections = {{0, uint32_t(kCode)}};
    const std::vector<ar::Range> functions = {{0, uint32_t(sizeof kEntry + sizeof kChain)}};
#if defined(_WIN32)
    const ar::Result r = ar::rewrite(buf, kCode, sections, functions);
    check(!r.error, "rewrite() found memory for its stubs");
    check(r.rewritten == 11 && r.in_functions == 5 && r.refused == 0 && r.unsynced == 0,
          "all eleven sites rewritten (five in the function), the fault routine's too");
#else
    const ar::Sweep sweep = ar::find_sites(buf, kCode, sections, functions);
    std::vector<uint64_t> addresses(sweep.sites.size());
    std::vector<uint8_t> ops(sweep.sites.size());
    const ar::Installed r = ar::install(buf, sweep.sites, buf + kCode, addresses.data(), ops.data());
    check(r.rewritten == 10 && r.in_functions == 5 && r.refused == 0 && sweep.unsynced == 0,
          "all ten sites rewritten, five in the function");
#endif
    const int cases = run_cases(buf, "rewritten");
    std::printf("adx_rewrite: %d cases through %zu stubs (host %s ADX%s)\n", cases, r.rewritten,
                adx ? "has" : "lacks", rosetta ? ", Rosetta 2" : "");

#if defined(_WIN32)
    test_fault(buf);
#endif

    if (!adx && rosetta) {
        // The case the trap path gets wrong under Rosetta (cpu_emulation_adx_test measures it) came out exact above.
        std::printf("adx_rewrite: `stc; xor; adcx` exact through the stubs under Rosetta 2\n");
    }
}
#endif
}  // namespace

int main() {
    test_lengths();
    test_sweep();
    test_stubs();
    test_fault_mapping();
#ifdef LIVE
    test_live();
#else
    std::printf("adx_rewrite: live run skipped (not an x86-64 host)\n");
#endif
    if (failures) {
        std::fprintf(stderr, "%d failure(s)\n", failures);
        return 1;
    }
    std::printf("adx_rewrite: all checks passed\n");
    return 0;
}
