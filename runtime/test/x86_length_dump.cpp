// Prints src/x86_length.h's length for each line of hex bytes on stdin ("48 89 d8", one instruction to a line), one
// number to a line (0 for not an instruction). test/check_x86_length.py compares these with llvm-mc's disassembler.
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include "x86_length.h"

int main() {
    char line[512];
    while (std::fgets(line, sizeof line, stdin)) {
        uint8_t bytes[32];
        std::size_t n = 0;
        for (char* p = line; *p && n < sizeof bytes;) {
            while (*p == ' ' || *p == '\t' || *p == ',') ++p;
            if (!std::isxdigit(static_cast<unsigned char>(p[0])) || !std::isxdigit(static_cast<unsigned char>(p[1])))
                break;
            const char pair[3] = {p[0], p[1], 0};
            bytes[n++] = static_cast<uint8_t>(std::strtoul(pair, nullptr, 16));
            p += 2;
        }
        std::printf("%zu\n", rt::x86::length(bytes, n));
    }
    return 0;
}
