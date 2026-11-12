//===- GuestRT/DynCast.cpp - dynamic_cast bridge (guest half) -------------===//
//
// The Echo compiler routes every dynamic_cast through __echo_dynamic_cast. In a
// FACE module the object and both RTTI operands all resolve inside the guest world
// (the object's redirected vptr points at the face vtable, whose typeinfo slot is
// a self-consistent face-world type_info), so the cast is an ordinary same-world
// one: forward to the stock __dynamic_cast. The cross-world bridging lives in the
// host half, HostRuntime/DynCast.cpp.
//
// SPDX-License-Identifier: MIT
//
//===----------------------------------------------------------------------===//
#include <cstddef>

extern "C" void *__dynamic_cast(const void *sub, const void *src, const void *dst,
                                std::ptrdiff_t src2dst);

extern "C" void *__echo_dynamic_cast(const void *sub, const void *src,
                                     const void *dst, std::ptrdiff_t src2dst)
{
    return __dynamic_cast(sub, src, dst, src2dst);
}
