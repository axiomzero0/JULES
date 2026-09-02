// elf_selftest.cpp — exercises jules::write_elf_executable with a
// hand-assembled program that does:
//
//     printf("hello from raw elf\n");
//     exit(0);                      // raw SYS_exit syscall, no libc teardown
//
// and leaves /tmp/elf_selftest_bin behind for manual verification
// (readelf -l / -d / -S, running it, checking the exit status).
#include "core/codegen/elf_writer.h"

#include <cstdio>
#include <cstring>

int main() {
    using jules::ElfImage;

    ElfImage img;

    // Hand-assembled x86-64, System V (offsets on the left; both rip-relative
    // displacements are the last 4 bytes of their instruction):
    //    0: 48 8d 3d XX XX XX XX   lea    disp32(%rip),%rdi  ; rdi = &msg
    //    7: 31 c0                  xor    %eax,%eax          ; varargs: al = 0
    //    9: ff 15 XX XX XX XX      call   *disp32(%rip)      ; *got[2] -> printf
    //   15: b8 3c 00 00 00         mov    $60,%eax           ; SYS_exit
    //   20: 31 ff                  xor    %edi,%edi          ; status 0
    //   22: 0f 05                  syscall
    static const unsigned char code[] = {
        0x48, 0x8d, 0x3d, 0x00, 0x00, 0x00, 0x00,  // lea  disp32(%rip),%rdi
        0x31, 0xc0,                                 // xor  %eax,%eax
        0xff, 0x15, 0x00, 0x00, 0x00, 0x00,         // call *disp32(%rip)
        0xb8, 0x3c, 0x00, 0x00, 0x00,               // mov  $60,%eax
        0x31, 0xff,                                 // xor  %edi,%edi
        0x0f, 0x05,                                 // syscall
    };
    img.text.assign(code, code + sizeof(code));
    img.entry_offset = 0;

    static const char msg[] = "hello from raw elf\n";
    img.rodata.resize(sizeof(msg));  // includes the trailing NUL
    std::memcpy(img.rodata.data(), msg, sizeof(msg));

    img.externs.emplace_back("printf");

    img.relocs.push_back({ElfImage::RelKind::RipToRodata, 3, 0});  // lea's disp32
    img.relocs.push_back({ElfImage::RelKind::RipToGot, 11, 0});    // call's disp32

    std::string err;
    if (!jules::write_elf_executable("/tmp/elf_selftest_bin", img, err)) {
        std::fprintf(stderr, "elf_selftest: FAILED: %s\n", err.c_str());
        return 1;
    }
    std::printf("elf_selftest: wrote /tmp/elf_selftest_bin\n");
    return 0;
}
