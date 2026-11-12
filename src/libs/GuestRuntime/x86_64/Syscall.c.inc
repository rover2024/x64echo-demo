//===- Guest/x86_64/Syscall.c.inc - raw dlcall syscall (x86_64) -----------===//
//
// Issues the dlcall magic syscall from guest code. Mirrors the convention qemu's
// dlcall plugin expects: rax=num, rdi.. = args, 4th arg in r10 (syscall ABI).
// Impl-reuse include (.c.inc): pulled into the guest .c that needs it.
//
// SPDX-License-Identifier: MIT
//
//===----------------------------------------------------------------------===//

#include <stdint.h>

static inline uint64_t echo_syscall2(uint64_t num, uint64_t a1, uint64_t a2)
{
    uint64_t ret;
    __asm__ volatile("mov %1, %%rax\n mov %2, %%rdi\n mov %3, %%rsi\n syscall\n mov %%rax, %0"
                     : "=r"(ret)
                     : "r"(num), "r"(a1), "r"(a2)
                     : "%rax", "%rdi", "%rsi", "%rcx", "%r11", "memory");
    return ret;
}

static inline uint64_t echo_syscall3(uint64_t num, uint64_t a1, uint64_t a2, uint64_t a3)
{
    uint64_t ret;
    __asm__ volatile("mov %1, %%rax\n mov %2, %%rdi\n mov %3, %%rsi\n mov %4, %%rdx\n"
                     "syscall\n mov %%rax, %0"
                     : "=r"(ret)
                     : "r"(num), "r"(a1), "r"(a2), "r"(a3)
                     : "%rax", "%rdi", "%rsi", "%rdx", "%rcx", "%r11", "memory");
    return ret;
}

static inline uint64_t echo_syscall4(uint64_t num, uint64_t a1, uint64_t a2, uint64_t a3,
                                     uint64_t a4)
{
    uint64_t ret;
    register uint64_t r10 __asm__("r10") = a4;
    __asm__ volatile("mov %1, %%rax\n mov %2, %%rdi\n mov %3, %%rsi\n mov %4, %%rdx\n"
                     "syscall\n mov %%rax, %0"
                     : "=r"(ret)
                     : "r"(num), "r"(a1), "r"(a2), "r"(a3), "r"(r10)
                     : "%rax", "%rdi", "%rsi", "%rdx", "%rcx", "%r11", "memory");
    return ret;
}
