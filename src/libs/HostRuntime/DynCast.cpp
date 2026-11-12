//===- HostRT/DynCast.cpp - cross-world dynamic_cast (host half) ----------===//
//
// The Echo compiler routes every dynamic_cast through __echo_dynamic_cast. In a
// NATIVE module the object it is applied to was constructed with a C-1 redirected
// vptr pointing at the FACE (guest-world) vtable, so the vtable's typeinfo slot
// holds a face-world type_info. libstdc++'s __dynamic_cast walks the class
// hierarchy comparing __class_type_info by POINTER, and would dispatch virtuals on
// that face type_info (guest-world code) -- wrong, and fatal cross-arch.
//
// The bridge mirrors the exception Tier-2 name channel (__echo_exc$): the object's
// most-derived type is identified by its mangled type_info::name(), the host's own
// _ZTI<name> for that type is recovered by dlsym, and the real cast engine
// (__class_type_info::__do_dyncast) is run against THAT host type_info, so the whole
// walk stays in the host world with the caller's host src/dst type_infos. Pure
// native objects (type_info already host, or unexported) fall back to the stock
// __dynamic_cast. The guest half (GuestRuntime/DynCast.cpp) just forwards, since a
// face module's cast is entirely same-world.
//
// SPDX-License-Identifier: MIT
//
//===----------------------------------------------------------------------===//
#include <cstddef>
#include <string>
#include <typeinfo>

#include <cxxabi.h>
#include <dlfcn.h>

namespace {

using __cxxabiv1::__class_type_info;

/// The Itanium vtable prefix, immediately before a vtable's address point:
/// [ offset_to_top, typeinfo, <address point> ]. The vptr holds `origin`.
struct VtablePrefix {
    std::ptrdiff_t whole_object;          // subobject -> complete object offset
    const __class_type_info *whole_type;  // most-derived type_info
    const void *origin;                   // the address point (what the vptr holds)
};

/// ABI layout of __class_type_info::__dyncast_result (declared but not defined in
/// the public <cxxabi.h>); __do_dyncast writes its verdict here.
struct DyncastResult {
    const void *dst_ptr;
    int whole2dst;
    int whole2src;
    int dst2src;
    int whole_details;
};

/// Access-path predicates (mirror libstdc++ tinfo2.cc).
inline bool contained_public_p(int k)
{
    return (k & __class_type_info::__contained_public) ==
           __class_type_info::__contained_public;
}
inline bool contained_nonvirtual_p(int k)
{
    return (k & (__class_type_info::__contained_mask |
                 __class_type_info::__contained_virtual_mask)) ==
           __class_type_info::__contained_mask;
}

/// Replicates __class_type_info::__find_public_src. That helper is declared inline
/// in <cxxabi.h> and is NOT an exported libstdc++ symbol, so referencing it
/// out-of-line leaves libEchoHostRuntime with an unresolvable undefined symbol
/// (which fails the whole load). Its slow path is the exported virtual
/// __do_find_public_src, which we call directly.
inline int find_public_src(const __class_type_info *dst, std::ptrdiff_t src2dst,
                           const void *obj_ptr, const __class_type_info *src_type,
                           const void *src_ptr)
{
    if (src2dst >= 0)
        return (reinterpret_cast<const char *>(obj_ptr) + src2dst) == src_ptr
                   ? __class_type_info::__contained_public
                   : __class_type_info::__not_contained;
    if (src2dst == -2)
        return __class_type_info::__not_contained;
    return dst->__do_find_public_src(src2dst, obj_ptr, src_type, src_ptr);
}

} // namespace

extern "C" void *__dynamic_cast(const void *, const __class_type_info *,
                                const __class_type_info *, std::ptrdiff_t);

/// Cross-world dynamic_cast. Same signature as __dynamic_cast.
extern "C" void *__echo_dynamic_cast(const void *src_ptr,
                                     const __class_type_info *src_type,
                                     const __class_type_info *dst_type,
                                     std::ptrdiff_t src2dst)
{
    if (!src_ptr)
        return nullptr;

    const void *vtable = *reinterpret_cast<const void *const *>(src_ptr);
    auto *prefix = reinterpret_cast<const VtablePrefix *>(
        reinterpret_cast<const char *>(vtable) - offsetof(VtablePrefix, origin));
    const void *whole_ptr =
        reinterpret_cast<const char *>(src_ptr) + prefix->whole_object;
    const __class_type_info *face_whole = prefix->whole_type;

    // Construction-in-progress guard (as __dynamic_cast does): if the complete
    // object's vptr disagrees with this subobject's whole_type, bail. Compared with
    // the object's own (face-world) type_infos, so it stays self-consistent.
    const void *whole_vtable = *reinterpret_cast<const void *const *>(whole_ptr);
    auto *whole_prefix = reinterpret_cast<const VtablePrefix *>(
        reinterpret_cast<const char *>(whole_vtable) -
        offsetof(VtablePrefix, origin));
    if (whole_prefix->whole_type != face_whole)
        return nullptr;

    // Recover the HOST type_info for the most-derived type by its mangled name
    // (_ZTI<name>). If unavailable, see the world check below.
    const __class_type_info *whole_type = face_whole;
    if (const char *name = face_whole->name()) {
        std::string sym = std::string("_ZTI") + name;
        if (auto *host = reinterpret_cast<const __class_type_info *>(
                dlsym(RTLD_DEFAULT, sym.c_str())))
            whole_type = host;
    }
    if (whole_type == face_whole) {
        // No host type_info for the most-derived type. Which world the object's
        // type_info lives in decides what is safe:
        //
        //  - HOST: this is a stand-alone native object (no guest involved, or a type
        //    whose _ZTI is simply not exported). The stock cast is correct -- every
        //    type_info it will touch is host-world. dladdr resolves the pointer
        //    because host DSOs are in the host link_map.
        //
        //  - GUEST (face): dladdr fails, because the face libraries are mapped by the
        //    emulator, not by the host loader. The stock cast would dispatch
        //    __do_dyncast -- a VIRTUAL call -- on this face type_info, whose own vptr
        //    points into the GUEST's libstdc++; the host would then execute guest
        //    memory (a non-executable mapping, or the wrong ISA cross-arch) and die.
        //    That is unrecoverable, so report "cast failed" instead. It happens when
        //    the most-derived class is hidden -- Qt builds its plugins with
        //    -fvisibility=hidden, so e.g. QOffscreenWindow's type_info is a LOCAL
        //    symbol that no dlsym can reach (the same hidden-symbol shape that the
        //    QMetaObject SMO homing had to handle). Returning null is the right answer
        //    for the cast that actually occurs there -- QtWidgets probing a platform
        //    window with dynamic_cast<QXcbWindow*> while running offscreen -- but a
        //    genuine downcast to a hidden type would also fail. Homing type_infos
        //    across worlds would fix that class properly; see docs/07 §5c.
        Dl_info info;
        if (dladdr(const_cast<__class_type_info *>(face_whole), &info) != 0)
            return __dynamic_cast(src_ptr, src_type, dst_type, src2dst);
        return nullptr;
    }

    // Run the real cast engine against the host most-derived type_info; src_type /
    // dst_type are the caller's host type_infos, so the whole walk is host-world.
    DyncastResult result = {nullptr, 0, 0, 0,
                            __cxxabiv1::__vmi_class_type_info::__flags_unknown_mask};
    whole_type->__do_dyncast(
        src2dst, __class_type_info::__contained_public, dst_type, whole_ptr,
        src_type, src_ptr,
        reinterpret_cast<__class_type_info::__dyncast_result &>(result));

    if (!result.dst_ptr)
        return nullptr;
    if (contained_public_p(result.dst2src))
        return const_cast<void *>(result.dst_ptr); // src is a public base of dst
    if (contained_public_p(result.whole2src & result.whole2dst))
        return const_cast<void *>(result.dst_ptr); // valid public cross-cast
    if (contained_nonvirtual_p(result.whole2src))
        return nullptr; // invalid cross-cast, cannot also be a downcast
    if (result.dst2src == __class_type_info::__unknown)
        result.dst2src =
            find_public_src(dst_type, src2dst, result.dst_ptr, src_type, src_ptr);
    if (contained_public_p(result.dst2src))
        return const_cast<void *>(result.dst_ptr); // valid downcast
    return nullptr;
}
