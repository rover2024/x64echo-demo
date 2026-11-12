/*
 * x64echo coroutine primitives (riscv64) — host side (04-reentry §2.4).
 *
 * The riscv64 counterpart of aarch64/Coroutine.S: save/restore the LP64D
 * callee-saved state (s0-s11, ra, sp, fs0-fs11) and swap stacks. Field offsets
 * match the __riscv EchoRegState in Entry.c. The C glue is architecture-neutral;
 * only this file and the register-state layout are arch-specific.
 *
 * gp and tp are deliberately NOT saved. gp is a link-time constant for the whole
 * process and tp is the thread pointer: a coroutine switch stays on the same
 * thread, so both are already correct on the other stack. Saving them would be
 * harmless but restoring a stale tp after a thread migration would not be.
 *
 * Assumes the D extension (rv64gc), which is the Linux riscv64 baseline; without
 * it the fsd/fld below would not assemble, which is the failure we want rather
 * than silently dropping callee-saved FP state.
 *
 * SPDX-License-Identifier: MIT
 */
    .text

// int64_t echo_coro_start(void *arg1, void *arg2,
//                         int64_t (*proc)(void *, void *),
//                         void *new_stack, EchoRegState *from)
//   a0=arg1 a1=arg2 a2=proc a3=new_stack a4=from
    .global echo_coro_start
    .type echo_coro_start, @function
    .hidden echo_coro_start
echo_coro_start:
    sd   s0,   0(a4)
    sd   s1,   8(a4)
    sd   s2,  16(a4)
    sd   s3,  24(a4)
    sd   s4,  32(a4)
    sd   s5,  40(a4)
    sd   s6,  48(a4)
    sd   s7,  56(a4)
    sd   s8,  64(a4)
    sd   s9,  72(a4)
    sd   s10, 80(a4)
    sd   s11, 88(a4)
    sd   ra,  96(a4)
    sd   sp, 104(a4)
    fsd  fs0, 112(a4)
    fsd  fs1, 120(a4)
    fsd  fs2, 128(a4)
    fsd  fs3, 136(a4)
    fsd  fs4, 144(a4)
    fsd  fs5, 152(a4)
    fsd  fs6, 160(a4)
    fsd  fs7, 168(a4)
    fsd  fs8, 176(a4)
    fsd  fs9, 184(a4)
    fsd  fs10,192(a4)
    fsd  fs11,200(a4)

    mv   sp, a3                     // switch to the new stack
    mv   s0, a4                     // keep `from` across the call (s0 callee-saved)
    jalr a2                         // proc(arg1=a0, arg2=a1)

    mv   a4, s0                     // recover `from` BEFORE reloading s0 over it
    ld   s0,   0(a4)
    ld   s1,   8(a4)
    ld   s2,  16(a4)
    ld   s3,  24(a4)
    ld   s4,  32(a4)
    ld   s5,  40(a4)
    ld   s6,  48(a4)
    ld   s7,  56(a4)
    ld   s8,  64(a4)
    ld   s9,  72(a4)
    ld   s10, 80(a4)
    ld   s11, 88(a4)
    ld   ra,  96(a4)
    ld   sp, 104(a4)
    fld  fs0, 112(a4)
    fld  fs1, 120(a4)
    fld  fs2, 128(a4)
    fld  fs3, 136(a4)
    fld  fs4, 144(a4)
    fld  fs5, 152(a4)
    fld  fs6, 160(a4)
    fld  fs7, 168(a4)
    fld  fs8, 176(a4)
    fld  fs9, 184(a4)
    fld  fs10,192(a4)
    fld  fs11,200(a4)
    ret

// int64_t echo_coro_switch(EchoRegState *from, EchoRegState *to, int64_t ret)
//   a0=from a1=to a2=ret
    .global echo_coro_switch
    .type echo_coro_switch, @function
    .hidden echo_coro_switch
echo_coro_switch:
    sd   s0,   0(a0)
    sd   s1,   8(a0)
    sd   s2,  16(a0)
    sd   s3,  24(a0)
    sd   s4,  32(a0)
    sd   s5,  40(a0)
    sd   s6,  48(a0)
    sd   s7,  56(a0)
    sd   s8,  64(a0)
    sd   s9,  72(a0)
    sd   s10, 80(a0)
    sd   s11, 88(a0)
    sd   ra,  96(a0)
    sd   sp, 104(a0)
    fsd  fs0, 112(a0)
    fsd  fs1, 120(a0)
    fsd  fs2, 128(a0)
    fsd  fs3, 136(a0)
    fsd  fs4, 144(a0)
    fsd  fs5, 152(a0)
    fsd  fs6, 160(a0)
    fsd  fs7, 168(a0)
    fsd  fs8, 176(a0)
    fsd  fs9, 184(a0)
    fsd  fs10,192(a0)
    fsd  fs11,200(a0)

    ld   s0,   0(a1)
    ld   s1,   8(a1)
    ld   s2,  16(a1)
    ld   s3,  24(a1)
    ld   s4,  32(a1)
    ld   s5,  40(a1)
    ld   s6,  48(a1)
    ld   s7,  56(a1)
    ld   s8,  64(a1)
    ld   s9,  72(a1)
    ld   s10, 80(a1)
    ld   s11, 88(a1)
    ld   ra,  96(a1)
    ld   sp, 104(a1)
    fld  fs0, 112(a1)
    fld  fs1, 120(a1)
    fld  fs2, 128(a1)
    fld  fs3, 136(a1)
    fld  fs4, 144(a1)
    fld  fs5, 152(a1)
    fld  fs6, 160(a1)
    fld  fs7, 168(a1)
    fld  fs8, 176(a1)
    fld  fs9, 184(a1)
    fld  fs10,192(a1)
    fld  fs11,200(a1)

    mv   a0, a2                     // return value
    ret

    .section .note.GNU-stack,"",@progbits
