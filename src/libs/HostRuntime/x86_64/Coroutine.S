/*
 * x64echo coroutine primitives (x86_64) — host side (04-reentry §2.4).
 *
 * Lifted verbatim from qemu-passthrough-test/2-callback/host/Arch/x86_64/
 * Coroutine_x86_64.S: a minimal callee-saved + rsp swap. coroutine_start runs
 * `proc(arg1, arg2)` on `new_stack`, saving the caller's state into `from`;
 * coroutine_switch swaps between two saved RegStates. This is host-arch code;
 * the x64->arm64 port supplies an aarch64 variant, the C glue stays neutral.
 *
 * SPDX-License-Identifier: MIT
 */
    .text

// int64_t echo_coro_start(void *arg1, void *arg2,
//                         int64_t (*proc)(void *, void *),
//                         void *new_stack, EchoRegState *from)
    .global echo_coro_start
    .type echo_coro_start, @function
    .hidden echo_coro_start
echo_coro_start:
    // rdi=arg1 rsi=arg2 rdx=proc rcx=new_stack r8=from
    mov %rbx, (%r8)
    mov %rsp, 8(%r8)
    mov %rbp, 16(%r8)
    mov %r12, 24(%r8)
    mov %r13, 32(%r8)
    mov %r14, 40(%r8)
    mov %r15, 48(%r8)

    mov %rcx, %rsp          // switch stack
    mov %r8, %rbp           // keep `from` across the call

    call *%rdx

    mov %rbp, %r8           // restore `from`
    mov (%r8), %rbx
    mov 8(%r8), %rsp
    mov 16(%r8), %rbp
    mov 24(%r8), %r12
    mov 32(%r8), %r13
    mov 40(%r8), %r14
    mov 48(%r8), %r15
    ret

// int64_t echo_coro_switch(EchoRegState *from, EchoRegState *to, int64_t ret)
    .global echo_coro_switch
    .type echo_coro_switch, @function
    .hidden echo_coro_switch
echo_coro_switch:
    // rdi=from rsi=to rdx=ret
    mov %rbx, (%rdi)
    mov %rsp, 8(%rdi)
    mov %rbp, 16(%rdi)
    mov %r12, 24(%rdi)
    mov %r13, 32(%rdi)
    mov %r14, 40(%rdi)
    mov %r15, 48(%rdi)

    mov (%rsi), %rbx
    mov 8(%rsi), %rsp
    mov 16(%rsi), %rbp
    mov 24(%rsi), %r12
    mov 32(%rsi), %r13
    mov 40(%rsi), %r14
    mov 48(%rsi), %r15

    mov %rdx, %rax          // return value
    ret

    .section .note.GNU-stack,"",@progbits
