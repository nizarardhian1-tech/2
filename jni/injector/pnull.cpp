#include "trace/pnull.h"
#include <unistd.h>

// ==========================================
// PTRACE ATTACH DENGAN RETRY
// ==========================================

bool NullTrace::ptraceAttachWithRetry(pid_t pid, int maxRetries) {
    for (int i = 0; i < maxRetries; i++) {
        if (ptrace(PTRACE_ATTACH, pid, nullptr, nullptr) == 0) {
            waitpid(pid, nullptr, WUNTRACED);
            return true;
        }
        if (i < maxRetries - 1) {
            usleep(50000); // tunggu 50ms sebelum retry
        }
    }
    return false;
}

// ==========================================
// PTRACE READ / WRITE
// ==========================================

bool NullTrace::ptraceWrite(pid_t pid, uintptr_t addr, uint8_t* data, size_t len) {
    // Coba /proc/pid/mem dulu (lebih cepat, tidak butuh POKEDATA loop)
    std::string memPath = "/proc/" + std::to_string(pid) + "/mem";
    int memFd = open(memPath.c_str(), O_WRONLY);
    if (memFd >= 0) {
        ssize_t written = pwrite(memFd, data, len, static_cast<off_t>(addr));
        close(memFd);
        if (written == static_cast<ssize_t>(len)) {
            return true;
        }
        // Gagal, fallthrough ke PTRACE_POKEDATA
    }

    // Fallback: PTRACE_POKEDATA word-by-word
    size_t remain = len % NullTrace::WORDSIZE;
    size_t amount = len / NullTrace::WORDSIZE;

    for (size_t i = 0; i < amount; i++) {
        long buffer;
        std::memcpy(&buffer, data + i * WORDSIZE, NullTrace::WORDSIZE);
        if (ptrace(PTRACE_POKEDATA, pid, i * NullTrace::WORDSIZE + addr, buffer) == -1) {
            return false;
        }
    }

    if (remain > 0) {
        size_t  leftToWord = NullTrace::WORDSIZE - remain;
        uint8_t oldAfterRemain[NullTrace::WORDSIZE];

        if (!NullTrace::ptraceRead(pid, amount * NullTrace::WORDSIZE + remain + addr,
                                   oldAfterRemain, leftToWord))
            return false;

        long buffer = 0;
        std::memcpy(&buffer, data + amount * NullTrace::WORDSIZE, remain);
        std::memcpy(reinterpret_cast<uint8_t*>(&buffer) + remain, oldAfterRemain, leftToWord);

        if (ptrace(PTRACE_POKEDATA, pid, amount * NullTrace::WORDSIZE + addr, buffer) == -1) {
            return false;
        }
    }

    return true;
}

bool NullTrace::ptraceRead(pid_t pid, uintptr_t addr, uint8_t* data, size_t len) {
    // Coba /proc/pid/mem dulu
    std::string memPath = "/proc/" + std::to_string(pid) + "/mem";
    int memFd = open(memPath.c_str(), O_RDONLY);
    if (memFd >= 0) {
        ssize_t bytesRead = pread(memFd, data, len, static_cast<off_t>(addr));
        close(memFd);
        if (bytesRead == static_cast<ssize_t>(len)) {
            return true;
        }
    }

    // Fallback: PTRACE_PEEKDATA
    size_t remain = len % NullTrace::WORDSIZE;
    size_t amount = len / NullTrace::WORDSIZE;

    for (size_t i = 0; i < amount; i++) {
        long buffer = 0;
        errno = 0;
        buffer = ptrace(PTRACE_PEEKDATA, pid, i * NullTrace::WORDSIZE + addr, NULL);
        if (errno) {
            return false;
        }
        std::memcpy(data + i * NullTrace::WORDSIZE, &buffer, NullTrace::WORDSIZE);
    }

    if (remain > 0) {
        errno = 0;
        long buffer = ptrace(PTRACE_PEEKDATA, pid, amount * NullTrace::WORDSIZE + addr, NULL);
        if (errno) {
            return false;
        }
        std::memcpy(data + amount * NullTrace::WORDSIZE, &buffer, remain);
    }

    return true;
}

// ==========================================
// REMOTE CALL via PTRACE
// ==========================================

uintptr_t NullTrace::ptraceRemoteCall(pid_t pid, uintptr_t addr, uintptr_t* argv,
                                      size_t argc, uintptr_t retAddr) {
    struct regs_s regs{}, oldRegs{};

    if (!NullTrace::ptraceGetRegs(pid, regs)) {
        return 0;
    }
    std::memcpy(&oldRegs, &regs, sizeof(regs));

#if defined(__arm__)
    for (int i = 0; (i < (int)argc) && (i < 4); i++) {
        regs.uregs[i] = argv[i];
    }
    if (argc > 4) {
        regs.ARM_sp -= sizeof(uintptr_t) * (argc - 4);
        uintptr_t stack = regs.ARM_sp;
        for (int i = 4; i < (int)argc; i++) {
            uintptr_t arg = argv[i];
            if (!ptraceWrite(pid, stack, (uint8_t*)&arg, sizeof(uintptr_t)))
                return 0;
            stack += sizeof(uintptr_t);
        }
    }
    regs.ARM_pc = addr;
    if (regs.ARM_pc & 1) {
        regs.ARM_pc  &= (~1u);
        regs.ARM_cpsr |= CPSRTMASK;
    } else {
        regs.ARM_cpsr &= ~CPSRTMASK;
    }
    regs.ARM_lr = retAddr;

#elif defined(__aarch64__)
    for (int i = 0; (i < (int)argc) && (i < 8); i++) {
        regs.regs[i] = argv[i];
    }
    if (argc > 8) {
        regs.sp -= sizeof(uintptr_t) * (argc - 8);
        uintptr_t stack = regs.sp;
        for (int i = 8; i < (int)argc; i++) {
            uintptr_t arg = argv[i];
            if (!ptraceWrite(pid, stack, (uint8_t*)&arg, sizeof(uintptr_t)))
                return 0;
            stack += sizeof(uintptr_t);
        }
    }
    regs.pc        = addr;
    regs.regs[30]  = retAddr;

#elif defined(__i386__)
    regs.esp -= sizeof(uintptr_t) * argc;
    uintptr_t stack = regs.esp;
    for (int i = 0; i < (int)argc; i++) {
        uintptr_t arg = argv[i];
        if (!ptraceWrite(pid, stack, (uint8_t*)&arg, sizeof(uintptr_t)))
            return 0;
        stack += sizeof(uintptr_t);
    }
    uintptr_t lr = retAddr;
    regs.esp -= sizeof(uintptr_t);
    if (!ptraceWrite(pid, regs.esp, (uint8_t*)&lr, sizeof(uintptr_t)))
        return 0;
    regs.eip = addr;

#elif defined(__x86_64__)
    uintptr_t space = sizeof(uintptr_t);
    if (argc > 6) space += sizeof(uintptr_t) * (argc - 6);
    while (((regs.rsp - space - 8) & 0xF) != 0) regs.rsp--;

    for (int i = 0; (i < (int)argc) && (i < 6); i++) {
        uintptr_t arg = argv[i];
        switch (i) {
            case 0: regs.rdi = arg; break;
            case 1: regs.rsi = arg; break;
            case 2: regs.rdx = arg; break;
            case 3: regs.rcx = arg; break;
            case 4: regs.r8  = arg; break;
            case 5: regs.r9  = arg; break;
        }
    }
    if (argc > 6) {
        regs.rsp -= sizeof(uintptr_t) * (argc - 6);
        uintptr_t stack = regs.rsp;
        for (int i = 6; i < (int)argc; i++) {
            uintptr_t arg = argv[i];
            if (!ptraceWrite(pid, stack, (uint8_t*)&arg, sizeof(uintptr_t)))
                return 0;
            stack += sizeof(uintptr_t);
        }
    }
    uintptr_t lr = retAddr;
    regs.rsp     -= sizeof(uintptr_t);
    if (!ptraceWrite(pid, regs.rsp, (uint8_t*)&lr, sizeof(uintptr_t)))
        return 0;
    regs.rip      = addr;
    regs.rax      = 0;
    regs.orig_rax = 0;

#else
#error Unsupported architecture
#endif

    if (!NullTrace::ptraceSetRegs(pid, regs))
        return 0;

    if (ptrace(PTRACE_CONT, pid, NULL, NULL) == -1)
        return 0;

    // Tunggu SIGSEGV (return address 0x0 sengaja trigger SIGSEGV)
    // Batas iterasi untuk mencegah infinite loop
    int status;
    const int MAX_SIGNALS = 64;
    int signalCount = 0;

    waitpid(pid, &status, WUNTRACED);

    while (signalCount < MAX_SIGNALS) {
        if (!WIFSTOPPED(status)) break; // proses tidak stopped, keluar
        if (WSTOPSIG(status) == SIGSEGV) break; // kita dapat return signal

        signalCount++;
        // Forward signal ke proses (jangan drop signal asli)
        int sig = WSTOPSIG(status);
        int forwardSig = (sig == SIGSTOP || sig == SIGTRAP) ? 0 : sig;
        if (ptrace(PTRACE_CONT, pid, NULL, (void*)(uintptr_t)forwardSig) == -1)
            break;
        waitpid(pid, &status, WUNTRACED);
    }

    if (!NullTrace::ptraceGetRegs(pid, regs)) {
        NullTrace::ptraceSetRegs(pid, oldRegs);
        return 0;
    }

    NullTrace::ptraceSetRegs(pid, oldRegs);

#if defined(__arm__)
    return regs.ARM_r0;
#elif defined(__aarch64__)
    return regs.regs[0];
#elif defined(__i386__)
    return regs.eax;
#elif defined(__x86_64__)
    return regs.rax;
#endif
}

// ==========================================
// GET / SET REGISTERS
// ==========================================

bool NullTrace::ptraceGetRegs(pid_t pid, regs_s& regs) {
#if defined(__LP64__)
    struct iovec iov{};
    iov.iov_base = &regs;
    iov.iov_len  = sizeof(regs);
    return ptrace(PTRACE_GETREGSET, pid, NT_PRSTATUS, &iov) != -1;
#else
    return ptrace(PTRACE_GETREGS, pid, nullptr, &regs) != -1;
#endif
}

bool NullTrace::ptraceSetRegs(pid_t pid, regs_s& regs) {
#if defined(__LP64__)
    struct iovec iov{};
    iov.iov_base = &regs;
    iov.iov_len  = sizeof(regs);
    return ptrace(PTRACE_SETREGSET, pid, NT_PRSTATUS, &iov) != -1;
#else
    return ptrace(PTRACE_SETREGS, pid, nullptr, &regs) != -1;
#endif
}
