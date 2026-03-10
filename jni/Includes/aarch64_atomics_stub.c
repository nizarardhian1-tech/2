/*
 * Software fallback implementations for __aarch64_* outline atomic functions.
 * Uses __atomic builtins (compiler intrinsics) instead of LSE instructions,
 * so this works on ALL arm64 CPUs including ARMv8.0 (Snapdragon 630/660/710 etc).
 *
 * These symbols are normally in libclang_rt.builtins but older NDK versions
 * bundled with AIDE do not include them.
 */

#if defined(__aarch64__)
#include <stdint.h>

/* ---- 4-byte (32-bit) operations ---- */

uint32_t __aarch64_ldadd4_acq_rel(uint32_t val, uint32_t *ptr) {
    return __atomic_fetch_add(ptr, val, __ATOMIC_ACQ_REL);
}

uint32_t __aarch64_ldclr4_acq_rel(uint32_t val, uint32_t *ptr) {
    return __atomic_fetch_and(ptr, ~val, __ATOMIC_ACQ_REL);
}

uint32_t __aarch64_ldset4_acq_rel(uint32_t val, uint32_t *ptr) {
    return __atomic_fetch_or(ptr, val, __ATOMIC_ACQ_REL);
}

uint32_t __aarch64_swp4_acq_rel(uint32_t val, uint32_t *ptr) {
    return __atomic_exchange_n(ptr, val, __ATOMIC_ACQ_REL);
}

uint32_t __aarch64_swp4_acq(uint32_t val, uint32_t *ptr) {
    return __atomic_exchange_n(ptr, val, __ATOMIC_ACQUIRE);
}

uint32_t __aarch64_swp4_rel(uint32_t val, uint32_t *ptr) {
    return __atomic_exchange_n(ptr, val, __ATOMIC_RELEASE);
}

uint32_t __aarch64_cas4_acq_rel(uint32_t expected, uint32_t desired, uint32_t *ptr) {
    __atomic_compare_exchange_n(ptr, &expected, desired, 0,
                                __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE);
    return expected;
}

uint32_t __aarch64_cas4_acq(uint32_t expected, uint32_t desired, uint32_t *ptr) {
    __atomic_compare_exchange_n(ptr, &expected, desired, 0,
                                __ATOMIC_ACQUIRE, __ATOMIC_ACQUIRE);
    return expected;
}

uint32_t __aarch64_cas4_rel(uint32_t expected, uint32_t desired, uint32_t *ptr) {
    __atomic_compare_exchange_n(ptr, &expected, desired, 0,
                                __ATOMIC_RELEASE, __ATOMIC_RELAXED);
    return expected;
}

uint32_t __aarch64_cas4_relax(uint32_t expected, uint32_t desired, uint32_t *ptr) {
    __atomic_compare_exchange_n(ptr, &expected, desired, 0,
                                __ATOMIC_RELAXED, __ATOMIC_RELAXED);
    return expected;
}

/* ---- 8-byte (64-bit) operations ---- */

uint64_t __aarch64_ldadd8_acq_rel(uint64_t val, uint64_t *ptr) {
    return __atomic_fetch_add(ptr, val, __ATOMIC_ACQ_REL);
}

uint64_t __aarch64_ldclr8_acq_rel(uint64_t val, uint64_t *ptr) {
    return __atomic_fetch_and(ptr, ~val, __ATOMIC_ACQ_REL);
}

uint64_t __aarch64_ldset8_acq_rel(uint64_t val, uint64_t *ptr) {
    return __atomic_fetch_or(ptr, val, __ATOMIC_ACQ_REL);
}

uint64_t __aarch64_swp8_acq_rel(uint64_t val, uint64_t *ptr) {
    return __atomic_exchange_n(ptr, val, __ATOMIC_ACQ_REL);
}

uint64_t __aarch64_cas8_acq_rel(uint64_t expected, uint64_t desired, uint64_t *ptr) {
    __atomic_compare_exchange_n(ptr, &expected, desired, 0,
                                __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE);
    return expected;
}

uint64_t __aarch64_cas8_acq(uint64_t expected, uint64_t desired, uint64_t *ptr) {
    __atomic_compare_exchange_n(ptr, &expected, desired, 0,
                                __ATOMIC_ACQUIRE, __ATOMIC_ACQUIRE);
    return expected;
}

uint64_t __aarch64_cas8_rel(uint64_t expected, uint64_t desired, uint64_t *ptr) {
    __atomic_compare_exchange_n(ptr, &expected, desired, 0,
                                __ATOMIC_RELEASE, __ATOMIC_RELAXED);
    return expected;
}

uint64_t __aarch64_cas8_relax(uint64_t expected, uint64_t desired, uint64_t *ptr) {
    __atomic_compare_exchange_n(ptr, &expected, desired, 0,
                                __ATOMIC_RELAXED, __ATOMIC_RELAXED);
    return expected;
}

#endif /* __aarch64__ */
