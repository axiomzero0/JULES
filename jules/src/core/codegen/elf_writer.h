// ELF64 writer: assembles .text + .rodata into a position-independent
// executable (ET_DYN, PT_INTERP, BIND_NOW) with a GOT for externals.
// No external tools: bytes are emitted directly.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace jules {

struct ElfImage {
    std::vector<uint8_t> text;            // machine code
    uint64_t entry_offset = 0;            // offset within text of the entry point
    std::vector<uint8_t> rodata;          // read-only data
    std::vector<std::string> externs;     // GOT slot i corresponds to externs[i]
    enum class RelKind : uint8_t { RipToRodata, RipToGot };
    struct Reloc {
        RelKind kind;
        uint64_t at;      // offset within `text` of a disp32 field
        uint32_t target;  // rodata byte offset (RipToRodata) or GOT slot index (RipToGot)
    };
    std::vector<Reloc> relocs;
};

// Writes the executable. On failure returns false and fills `err`.
bool write_elf_executable(const char* path, const ElfImage& img, std::string& err);

} // namespace jules
