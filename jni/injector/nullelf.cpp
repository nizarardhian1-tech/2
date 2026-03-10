#include "null/nullelf.h"

// ==========================================
// GET SYMBOL ADDRESS FROM ELF
// ==========================================

uintptr_t NullElf::getAddrSym(const char* path, const char* symbol, SymbolResMode mode) {
    int fd = open(path, O_RDONLY);
    if (fd < 0) return 0;

    struct stat sb;
    if (fstat(fd, &sb) < 0) {
        close(fd);
        return 0;
    }

    char* data = reinterpret_cast<char*>(
        mmap(NULL, sb.st_size, PROT_READ, MAP_PRIVATE, fd, 0));
    close(fd); // aman di-close setelah mmap, data tetap valid

    if (data == MAP_FAILED) return 0;

    // Baca ELF class langsung dari pointer mmap (bukan read() lagi)
    if (sb.st_size < EI_NIDENT) {
        munmap(data, sb.st_size);
        return 0;
    }

    unsigned char e_class = static_cast<unsigned char>(data[EI_CLASS]);
    uintptr_t address = 0;

    if (e_class == ELFCLASS32) {
        address = NullElfUtils::searchSymbolTable<Elf32_Ehdr, Elf32_Shdr, Elf32_Sym>(
            data, symbol, mode);
    } else if (e_class == ELFCLASS64) {
        address = NullElfUtils::searchSymbolTable<Elf64_Ehdr, Elf64_Shdr, Elf64_Sym>(
            data, symbol, mode);
    }

    munmap(data, sb.st_size);
    return address;
}

// ==========================================
// GET LIBRARY ARCHITECTURE FROM ELF
// ==========================================

int NullElf::getLibraryArch(const char* path) {
    int fd = open(path, O_RDONLY);
    if (fd < 0) return -1;

    struct stat sb;
    if (fstat(fd, &sb) < 0) {
        close(fd);
        return -1;
    }

    char* data = reinterpret_cast<char*>(
        mmap(NULL, sb.st_size, PROT_READ, MAP_PRIVATE, fd, 0));
    close(fd);

    if (data == MAP_FAILED) return -1;

    if (sb.st_size < EI_NIDENT) {
        munmap(data, sb.st_size);
        return -1;
    }

    unsigned char e_class = static_cast<unsigned char>(data[EI_CLASS]);
    int arch = -1;

    if (e_class == ELFCLASS32) {
        arch = NullElfUtils::getArch<Elf32_Ehdr>(data);
    } else if (e_class == ELFCLASS64) {
        arch = NullElfUtils::getArch<Elf64_Ehdr>(data);
    }

    munmap(data, sb.st_size);
    return arch;
}
