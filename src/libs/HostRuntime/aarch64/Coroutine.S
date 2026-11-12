/*
 * x64echo coroutine primitives (aarch64) — host side (04-reentry §2.4).
 *
 * The aarch64 counterpart of x86_64/Coroutine.S: save/restore the AAPCS64
 * callee-saved state (x19-x28, fp/lr, sp, d8-d15) and swap stacks. Field offsets
 * match the __aarch64__ EchoRegState in Entry.c. The C glue is architecture-
 * neutral; only this file and the register-state layout are arch-specific.
 *
 * SPDX-License-Identifier: MIT
 */
    .text

// int64_t echo_coro_start(void *arg1, void *arg2,
//                         int64_t (*proc)(void *, void *),
//                         void *new_stack, EchoRegState *from)
//   x0=arg1 x1=arg2 x2=proc x3=new_stack x4=from
    .global echo_coro_start
    .type echo_coro_start, %function
    .hidden echo_coro_start
echo_coro_start:
    stp x19, x20, [x4, #0]
    stp x21, x22, [x4, #16]
    stp x23, x24, [x4, #32]
    stp x25, x26, [x4, #48]
    stp x27, x28, [x4, #64]
    stp x29, x30, [x4, #80]         // fp, lr
    mov x9, sp
    str x9, [x4, #96]              // sp
    stp d8,  d9,  [x4, #104]
    stp d10, d11, [x4, #120]
    stp d12, d13, [x4, #136]
    stp d14, d15, [x4, #152]

    mov sp, x3                      // switch to the new stack
    mov x19, x4                     // keep `from` across the call (x19 callee-saved)
    blr x2                          // proc(arg1=x0, arg2=x1)

    mov x4, x19                     // restore `from`
    ldp x19, x20, [x4, #0]
    ldp x21, x22, [x4, #16]
    ldp x23, x24, [x4, #32]
    ldp x25, x26, [x4, #48]
    ldp x27, x28, [x4, #64]
    ldp x29, x30, [x4, #80]
    ldr x9, [x4, #96]
    mov sp, x9
    ldp d8,  d9,  [x4, #104]
    ldp d10, d11, [x4, #120]
    ldp d12, d13, [x4, #136]
    ldp d14, d15, [x4, #152]
    ret

// int64_t echo_coro_switch(EchoRegState *from, EchoRegState *to, int64_t ret)
//   x0=from x1=to x2=ret
    .global echo_coro_switch
    .type echo_coro_switch, %function
    .hidden echo_coro_switch
echo_coro_switch:
    stp x19, x20, [x0, #0]
    stp x21, x22, [x0, #16]
    stp x23, x24, [x0, #32]
    stp x25, x26, [x0, #48]
    stp x27, x28, [x0, #64]
    stp x29, x30, [x0, #80]
    mov x9, sp
    str x9, [x0, #96]
    stp d8,  d9,  [x0, #104]
    stp d10, d11, [x0, #120]
    stp d12, d13, [x0, #136]
    stp d14, d15, [x0, #152]

    ldp x19, x20, [x1, #0]
    ldp x21, x22, [x1, #16]
    ldp x23, x24, [x1, #32]
    ldp x25, x26, [x1, #48]
    ldp x27, x28, [x1, #64]
    ldp x29, x30, [x1, #80]
    ldr x9, [x1, #96]
    mov sp, x9
    ldp d8,  d9,  [x1, #104]
    ldp d10, d11, [x1, #120]
    ldp d12, d13, [x1, #136]
    ldp d14, d15, [x1, #152]

    mov x0, x2                      // return value
    ret

    .section .note.GNU-stack,"",%progbits
