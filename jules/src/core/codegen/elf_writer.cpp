// ELF64 x86-64 PIE writer — implementation (no exceptions / no RTTI).
//
// Layout math. All quantities below are *file offsets*; kBase = 0x400000 and
// every PT_LOAD starts at a 0x1000-aligned file offset, so with
//     p_vaddr = kBase + p_offset
// the loader requirement  p_vaddr == p_offset (mod 0x1000)  holds for every
// PT_LOAD automatically (kBase is page-aligned). Data is laid out strictly in
// offset order; segment starts are page-aligned with zero padding:
//
//   off 0x0000    ELF header (64 B)                                    |
//   off 0x0040    7 program headers (7 * 56 = 392 B)                   | PT_LOAD #1 (R)
//   off 0x01c8    .interp  ("/lib64/ld-linux-x86-64.so.2", 28 B)       | p_offset 0, p_vaddr kBase,
//   (align 8)     .dynsym  (24 * (1 + n) B, n = externs.size())        | p_filesz = p_memsz =
//   (align 8)     .dynstr  ("\0libc.so.6\0" + extern names)            |   end of .rela.plt
//   (align 8)     .rela.plt (24 * n B)                                 |
//   align(0x1000) .text     -> PT_LOAD #2 (R+X), e_entry lives here
//   align(0x1000) .rodata   -> PT_LOAD #3 (R)
//   align(0x1000) .got      -> PT_LOAD #4 (RW) starts here
//                .got      (16 + 8*n B: reserved slots 0,1 + one per extern)
//   got + got_sz  .dynamic  (15 entries * 16 B, still inside PT_LOAD #4)
//   (align 8)     .shstrtab (not mapped; readelf only)
//   (align 8)     10 section headers
//
// .dynsym / .dynstr / .rela.plt must be inside a mapped segment because
// ld.so reads them at startup, so they share PT_LOAD #1 with the ELF header
// and the program headers (the classic read-only "header" segment).
//
// GOT / dynamic linking contract (BIND_NOW, so no PLT trampolines are needed):
//   got[0] = &_DYNAMIC (reserved by DT_PLTGOT), got[1] = 0 (reserved),
//   got[2 + i] = extern i, resolved eagerly by ld.so from .rela.plt
//   (R_X86_64_JUMP_SLOT at r_offset = got + 8*(i+2)). Code reaches extern i
//   with `call *disp32(%rip)` where disp32 targets got + 8*(i+2).
//
// Output is byte-for-byte deterministic: no timestamps, no host-endianness
// dependence (multi-byte fields are emitted one byte at a time).

#include "elf_writer.h"

#include <cerrno>
#include <cstring>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace jules {
namespace {

// ---------------------------------------------------------------------------
// Little-endian emitters.
// ---------------------------------------------------------------------------
void put8(std::vector<uint8_t>& b, uint8_t v) { b.push_back(v); }

void put16(std::vector<uint8_t>& b, uint16_t v) {
    put8(b, static_cast<uint8_t>(v));
    put8(b, static_cast<uint8_t>(v >> 8));
}

void put32(std::vector<uint8_t>& b, uint32_t v) {
    put16(b, static_cast<uint16_t>(v));
    put16(b, static_cast<uint16_t>(v >> 16));
}

void put64(std::vector<uint8_t>& b, uint64_t v) {
    put32(b, static_cast<uint32_t>(v));
    put32(b, static_cast<uint32_t>(v >> 32));
}

constexpr uint64_t align_up(uint64_t x, uint64_t a) { return (x + a - 1) & ~(a - 1); }

// Appends `s` (including its NUL) to a string-table blob; returns its offset.
uint32_t put_cstr(std::vector<uint8_t>& b, const char* s) {
    const uint32_t off = static_cast<uint32_t>(b.size());
    while (*s != '\0') put8(b, static_cast<uint8_t>(*s++));
    put8(b, 0);
    return off;
}

// Zero-pads the image so the next component starts at file offset `off`.
void pad_to(std::vector<uint8_t>& b, uint64_t off) {
    if (b.size() < off) b.insert(b.end(), static_cast<size_t>(off - b.size()), 0);
}

// ---------------------------------------------------------------------------
// ELF constants (only the ones this writer uses).
// ---------------------------------------------------------------------------
constexpr uint16_t ET_DYN = 3;
constexpr uint16_t EM_X86_64 = 62;

constexpr uint32_t PT_LOAD = 1, PT_DYNAMIC = 2, PT_INTERP = 3;
constexpr uint32_t PT_GNU_STACK = 0x6474e551;
constexpr uint32_t PF_X = 1, PF_W = 2, PF_R = 4;

constexpr uint32_t SHT_PROGBITS = 1, SHT_STRTAB = 3, SHT_RELA = 4, SHT_DYNAMIC = 6,
                   SHT_DYNSYM = 11;
constexpr uint64_t SHF_WRITE = 1, SHF_ALLOC = 2, SHF_EXECINSTR = 4;

constexpr uint64_t DT_NULL = 0, DT_NEEDED = 1, DT_PLTRELSZ = 2, DT_PLTGOT = 3,
                   DT_STRTAB = 5, DT_SYMTAB = 6, DT_RELA = 7, DT_RELASZ = 8,
                   DT_RELAENT = 9, DT_STRSZ = 10, DT_SYMENT = 11, DT_PLTREL = 20,
                   DT_JMPREL = 23, DT_BIND_NOW = 24, DT_FLAGS = 30;
constexpr uint64_t DF_BIND_NOW = 8;

constexpr uint32_t R_X86_64_JUMP_SLOT = 7;
constexpr uint8_t STB_GLOBAL_STT_FUNC = 0x12;  // (STB_GLOBAL << 4) | STT_FUNC
constexpr uint16_t SHN_UNDEF = 0;

constexpr uint64_t kBase = 0x400000;  // image base vaddr (PIE: just an image bias)
constexpr uint64_t kPage = 0x1000;
// PT_INTERP path. sizeof() counts the NUL — the kernel requires the path to be
// NUL-terminated *within* p_filesz, so this length must be exact.
constexpr const char* kInterp = "/lib64/ld-linux-x86-64.so.2";
constexpr size_t kInterpLen = sizeof("/lib64/ld-linux-x86-64.so.2");
constexpr size_t kPhnum = 7;   // LOAD, LOAD, LOAD, LOAD, DYNAMIC, INTERP, GNU_STACK
constexpr size_t kShnum = 10;  // NULL .interp .text .rodata .dynstr .dynsym .rela.plt .got .dynamic .shstrtab

void put_phdr(std::vector<uint8_t>& b, uint32_t type, uint32_t flags, uint64_t off,
              uint64_t vaddr, uint64_t filesz, uint64_t memsz, uint64_t align) {
    put32(b, type);
    put32(b, flags);
    put64(b, off);    // p_offset
    put64(b, vaddr);  // p_vaddr
    put64(b, vaddr);  // p_paddr == p_vaddr
    put64(b, filesz);
    put64(b, memsz);
    put64(b, align);
}

void put_shdr(std::vector<uint8_t>& b, uint32_t name, uint32_t type, uint64_t flags,
              uint64_t addr, uint64_t off, uint64_t size, uint32_t link, uint32_t info,
              uint64_t addralign, uint64_t entsize) {
    put32(b, name);
    put32(b, type);
    put64(b, flags);
    put64(b, addr);
    put64(b, off);
    put64(b, size);
    put32(b, link);
    put32(b, info);
    put64(b, addralign);
    put64(b, entsize);
}

}  // namespace

bool write_elf_executable(const char* path, const ElfImage& img, std::string& err) {
    using RelKind = ElfImage::RelKind;

    // ---- 0. Validate the image (fail closed, never emit a broken file) ----
    if (path == nullptr || path[0] == '\0') {
        err = "elf_writer: empty output path";
        return false;
    }
    if (img.entry_offset > img.text.size()) {
        err = "elf_writer: entry_offset past end of .text";
        return false;
    }
    for (const ElfImage::Reloc& r : img.relocs) {
        if (r.at + 4 < r.at || r.at + 4 > img.text.size()) {
            err = "elf_writer: relocation displacement runs past end of .text";
            return false;
        }
        if (r.kind == RelKind::RipToRodata) {
            if (r.target > img.rodata.size()) {  // one-past-end is a valid address
                err = "elf_writer: rodata relocation target out of range";
                return false;
            }
        } else if (r.kind == RelKind::RipToGot) {
            if (r.target >= img.externs.size()) {
                err = "elf_writer: GOT relocation target out of range";
                return false;
            }
        } else {
            err = "elf_writer: unknown relocation kind";
            return false;
        }
    }

    const size_t n = img.externs.size();  // externs = GOT slots = PLT relocs = dynsyms

    // ---- 1. String tables --------------------------------------------------
    std::vector<uint8_t> dynstr;
    dynstr.push_back(0);  // offset 0: the empty name used by Sym[0]
    const uint32_t libc_name = put_cstr(dynstr, "libc.so.6");
    std::vector<uint32_t> sym_name(n);
    for (size_t i = 0; i < n; ++i) sym_name[i] = put_cstr(dynstr, img.externs[i].c_str());

    std::vector<uint8_t> shstr;
    shstr.push_back(0);  // offset 0: the empty name used by the NULL section
    const uint32_t sn_interp = put_cstr(shstr, ".interp");
    const uint32_t sn_text = put_cstr(shstr, ".text");
    const uint32_t sn_rodata = put_cstr(shstr, ".rodata");
    const uint32_t sn_dynstr = put_cstr(shstr, ".dynstr");
    const uint32_t sn_dynsym = put_cstr(shstr, ".dynsym");
    const uint32_t sn_relaplt = put_cstr(shstr, ".rela.plt");
    const uint32_t sn_got = put_cstr(shstr, ".got");
    const uint32_t sn_dynamic = put_cstr(shstr, ".dynamic");
    const uint32_t sn_shstrtab = put_cstr(shstr, ".shstrtab");

    // ---- 2. Layout (see the diagram at the top of the file) ----------------
    // Header segment PT_LOAD #1 (R): ehdr + phdrs + .interp + .dynsym + .dynstr
    // + .rela.plt. Everything ld.so must read at startup.
    const uint64_t interp_off = 64 + 56 * kPhnum;  // 0x1c8, right after the phdrs
    const uint64_t interp_sz = kInterpLen;         // path + NUL, exact
    const uint64_t dynsym_off = align_up(interp_off + interp_sz, 8);
    const uint64_t dynsym_sz = 24 * (1 + n);       // Sym[0] (all zeroes) + one per extern
    const uint64_t dynstr_off = dynsym_off + dynsym_sz;  // 24 % 8 == 0: no padding
    const uint64_t dynstr_sz = dynstr.size();
    const uint64_t relaplt_off = align_up(dynstr_off + dynstr_sz, 8);
    const uint64_t relaplt_sz = 24 * n;
    const uint64_t load1_end = relaplt_off + relaplt_sz;  // PT_LOAD #1 p_filesz

    const uint64_t text_off = align_up(load1_end, kPage);       // PT_LOAD #2 (R+X)
    const uint64_t text_sz = img.text.size();
    const uint64_t rodata_off = align_up(text_off + text_sz, kPage);  // PT_LOAD #3 (R)
    const uint64_t rodata_sz = img.rodata.size();
    const uint64_t got_off = align_up(rodata_off + rodata_sz, kPage); // PT_LOAD #4 (RW)
    const uint64_t got_sz = 16 + 8 * n;                         // slots 0,1 reserved + n
    const uint64_t dyn_off = got_off + got_sz;                  // .dynamic right after .got
    const uint64_t dyn_sz = 16 * 15;                            // 15 dynamic entries
    const uint64_t load4_end = dyn_off + dyn_sz;                // PT_LOAD #4 p_filesz

    const uint64_t shstr_off = align_up(load4_end, 8);          // not mapped: readelf only
    const uint64_t shstr_sz = shstr.size();
    const uint64_t sh_off = align_up(shstr_off + shstr_sz, 8);
    const uint64_t total_sz = sh_off + 64 * kShnum;

    // vaddrs: kBase is page-aligned and every LOAD start offset is page-aligned,
    // so vaddr == offset (mod 0x1000) for every PT_LOAD.
    const uint64_t interp_va = kBase + interp_off;
    const uint64_t dynsym_va = kBase + dynsym_off;
    const uint64_t dynstr_va = kBase + dynstr_off;
    const uint64_t relaplt_va = kBase + relaplt_off;
    const uint64_t text_va = kBase + text_off;
    const uint64_t rodata_va = kBase + rodata_off;
    const uint64_t got_va = kBase + got_off;
    const uint64_t dyn_va = kBase + dyn_off;

    // ---- 3. Emit bytes -----------------------------------------------------
    std::vector<uint8_t> f;
    f.reserve(static_cast<size_t>(total_sz));

    // ELF header.
    put8(f, 0x7f); put8(f, 'E'); put8(f, 'L'); put8(f, 'F');
    put8(f, 2);  // ELFCLASS64
    put8(f, 1);  // ELFDATA2LSB
    put8(f, 1);  // EV_CURRENT
    put8(f, 0);  // ELFOSABI_SYSV
    put8(f, 0);  // ABI version
    for (int i = 0; i < 7; ++i) put8(f, 0);  // EI_PAD
    put16(f, ET_DYN);
    put16(f, EM_X86_64);
    put32(f, 1);  // e_version
    put64(f, text_va + img.entry_offset);  // e_entry
    put64(f, 64);            // e_phoff
    put64(f, sh_off);        // e_shoff
    put32(f, 0);             // e_flags
    put16(f, 64);            // e_ehsize
    put16(f, 56);            // e_phentsize
    put16(f, static_cast<uint16_t>(kPhnum));
    put16(f, 64);            // e_shentsize
    put16(f, static_cast<uint16_t>(kShnum));
    put16(f, static_cast<uint16_t>(kShnum - 1));  // e_shstrndx: .shstrtab is last

    // Program headers (order: LOAD, LOAD, LOAD, LOAD, DYNAMIC, INTERP, GNU_STACK).
    put_phdr(f, PT_LOAD, PF_R, 0, kBase, load1_end, load1_end, kPage);
    put_phdr(f, PT_LOAD, PF_R | PF_X, text_off, text_va, text_sz, text_sz, kPage);
    put_phdr(f, PT_LOAD, PF_R, rodata_off, rodata_va, rodata_sz, rodata_sz, kPage);
    put_phdr(f, PT_LOAD, PF_R | PF_W, got_off, got_va, load4_end - got_off,
             load4_end - got_off, kPage);
    put_phdr(f, PT_DYNAMIC, PF_R | PF_W, dyn_off, dyn_va, dyn_sz, dyn_sz, 8);
    put_phdr(f, PT_INTERP, PF_R, interp_off, interp_va, interp_sz, interp_sz, 1);
    put_phdr(f, PT_GNU_STACK, PF_R | PF_W, 0, 0, 0, 0, 16);  // RW stack, no exec

    // .interp
    pad_to(f, interp_off);
    put_cstr(f, kInterp);

    // .dynsym: Sym[0] all zeroes; each extern is a GLOBAL FUNC SHN_UNDEF import.
    pad_to(f, dynsym_off);
    for (int i = 0; i < 24; ++i) put8(f, 0);
    for (size_t i = 0; i < n; ++i) {
        put32(f, sym_name[i]);
        put8(f, STB_GLOBAL_STT_FUNC);
        put8(f, 0);            // st_other
        put16(f, SHN_UNDEF);   // undefined: resolved from DT_NEEDED libraries
        put64(f, 0);           // st_value
        put64(f, 0);           // st_size
    }

    // .dynstr
    pad_to(f, dynstr_off);
    f.insert(f.end(), dynstr.begin(), dynstr.end());

    // .rela.plt: one R_X86_64_JUMP_SLOT per extern, patching got slot i + 2.
    pad_to(f, relaplt_off);
    for (size_t i = 0; i < n; ++i) {
        put64(f, got_va + 8 * (i + 2));                          // r_offset
        put64(f, (uint64_t(i + 1) << 32) | R_X86_64_JUMP_SLOT);  // r_info: sym i+1
        put64(f, 0);                                             // r_addend
    }

    // .text, with rip-relative disp32 fields patched in place. For both encodings
    // (lea 48 8d 3d <d32> / call ff 15 <d32>) the displacement is the last 4 bytes
    // of the instruction, so rip = vaddr(at) + 4.
    pad_to(f, text_off);
    {
        std::vector<uint8_t> text = img.text;  // writable copy
        for (const ElfImage::Reloc& r : img.relocs) {
            const uint64_t rip = text_va + r.at + 4;
            const uint64_t dst = (r.kind == RelKind::RipToRodata)
                                     ? rodata_va + r.target
                                     : got_va + 8 * (uint64_t(r.target) + 2);
            const uint32_t disp = static_cast<uint32_t>(int64_t(dst) - int64_t(rip));
            text[r.at + 0] = static_cast<uint8_t>(disp);
            text[r.at + 1] = static_cast<uint8_t>(disp >> 8);
            text[r.at + 2] = static_cast<uint8_t>(disp >> 16);
            text[r.at + 3] = static_cast<uint8_t>(disp >> 24);
        }
        f.insert(f.end(), text.begin(), text.end());
    }

    // .rodata
    pad_to(f, rodata_off);
    f.insert(f.end(), img.rodata.begin(), img.rodata.end());

    // .got: slot 0 = &_DYNAMIC (DT_PLTGOT reserved), slot 1 = 0 (reserved),
    // slots 2.. start at 0 and are filled by ld.so via .rela.plt. We always emit
    // DT_BIND_NOW / DF_BIND_NOW, so every slot holds its final address before
    // control reaches e_entry.
    pad_to(f, got_off);
    put64(f, dyn_va);
    put64(f, 0);
    for (size_t i = 0; i < n; ++i) put64(f, 0);

    // .dynamic: 15 entries, DT_NULL-terminated, in the mandated order.
    pad_to(f, dyn_off);
    put64(f, DT_NEEDED);   put64(f, libc_name);  // "libc.so.6"
    put64(f, DT_STRTAB);   put64(f, dynstr_va);
    put64(f, DT_SYMTAB);   put64(f, dynsym_va);
    put64(f, DT_STRSZ);    put64(f, dynstr_sz);
    put64(f, DT_SYMENT);   put64(f, 24);
    put64(f, DT_RELA);     put64(f, relaplt_va);
    put64(f, DT_RELASZ);   put64(f, 0);          // no non-PLT relocations
    put64(f, DT_RELAENT);  put64(f, 24);
    put64(f, DT_JMPREL);   put64(f, relaplt_va);
    put64(f, DT_PLTRELSZ); put64(f, relaplt_sz);
    put64(f, DT_PLTREL);   put64(f, DT_RELA);    // PLT relocs are Elf64_Rela
    put64(f, DT_PLTGOT);   put64(f, got_va);
    put64(f, DT_BIND_NOW); put64(f, 0);
    put64(f, DT_FLAGS);    put64(f, DF_BIND_NOW);
    put64(f, DT_NULL);     put64(f, 0);

    // .shstrtab (not part of any LOAD; only readelf/gdb consume it).
    pad_to(f, shstr_off);
    f.insert(f.end(), shstr.begin(), shstr.end());

    // Section headers — cosmetic for readelf. Section indices:
    //   0 NULL, 1 .interp, 2 .text, 3 .rodata, 4 .dynstr, 5 .dynsym,
    //   6 .rela.plt, 7 .got, 8 .dynamic, 9 .shstrtab.
    pad_to(f, sh_off);
    put_shdr(f, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0);                                            // [0] NULL
    put_shdr(f, sn_interp, SHT_PROGBITS, SHF_ALLOC, interp_va, interp_off, interp_sz,
             0, 0, 1, 0);                                                                 // [1] .interp
    put_shdr(f, sn_text, SHT_PROGBITS, SHF_ALLOC | SHF_EXECINSTR, text_va, text_off,
             text_sz, 0, 0, 16, 0);                                                       // [2] .text
    put_shdr(f, sn_rodata, SHT_PROGBITS, SHF_ALLOC, rodata_va, rodata_off, rodata_sz,
             0, 0, 1, 0);                                                                 // [3] .rodata
    put_shdr(f, sn_dynstr, SHT_STRTAB, SHF_ALLOC, dynstr_va, dynstr_off, dynstr_sz,
             0, 0, 1, 0);                                                                 // [4] .dynstr
    put_shdr(f, sn_dynsym, SHT_DYNSYM, SHF_ALLOC, dynsym_va, dynsym_off, dynsym_sz,
             4, 1, 8, 24);  // sh_link=.dynstr, sh_info=first global                        // [5] .dynsym
    put_shdr(f, sn_relaplt, SHT_RELA, SHF_ALLOC, relaplt_va, relaplt_off, relaplt_sz,
             5, 7, 8, 24);  // sh_link=.dynsym, sh_info=.got (relocation target)           // [6] .rela.plt
    put_shdr(f, sn_got, SHT_PROGBITS, SHF_ALLOC | SHF_WRITE, got_va, got_off, got_sz,
             0, 0, 8, 8);                                                                 // [7] .got
    put_shdr(f, sn_dynamic, SHT_DYNAMIC, SHF_ALLOC | SHF_WRITE, dyn_va, dyn_off, dyn_sz,
             5, 0, 8, 16);  // sh_link=.dynsym                                             // [8] .dynamic
    put_shdr(f, sn_shstrtab, SHT_STRTAB, 0, 0, shstr_off, shstr_sz, 0, 0, 1, 0);          // [9] .shstrtab

    if (f.size() != static_cast<size_t>(total_sz)) {  // self-check of the layout math
        err = "elf_writer: internal layout mismatch";
        return false;
    }

    // ---- 4. Write the file (plain POSIX: open/write/fchmod/close) ----------
    const int fd = ::open(path, O_WRONLY | O_CREAT | O_TRUNC, 0755);
    if (fd < 0) {
        err = "elf_writer: open " + std::string(path) + " failed: " + std::strerror(errno);
        return false;
    }
    size_t done = 0;
    while (done < f.size()) {
        const ssize_t w = ::write(fd, f.data() + done, f.size() - done);
        if (w < 0) {
            if (errno == EINTR) continue;
            err = "elf_writer: write " + std::string(path) + " failed: " + std::strerror(errno);
            ::close(fd);
            ::unlink(path);
            return false;
        }
        if (w == 0) {
            err = "elf_writer: write returned 0 bytes";
            ::close(fd);
            ::unlink(path);
            return false;
        }
        done += static_cast<size_t>(w);
    }
    if (::fchmod(fd, 0755) != 0) {  // 0755 exactly, independent of umask
        err = "elf_writer: chmod " + std::string(path) + " failed: " + std::strerror(errno);
        ::close(fd);
        ::unlink(path);
        return false;
    }
    if (::close(fd) != 0) {
        err = "elf_writer: close " + std::string(path) + " failed: " + std::strerror(errno);
        ::unlink(path);
        return false;
    }
    return true;
}

}  // namespace jules
