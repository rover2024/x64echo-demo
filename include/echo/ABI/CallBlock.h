//===- echo/ABI/CallBlock.h - Echo cross-boundary ABI (data only) ---------===//
//
// The shared data layout of the cross-boundary protocol: the structs, enums,
// magic, and typedefs that BOTH the guest face and the host native side agree
// on. A guest stub packs a call's arguments into an EchoCallBlock and hands it to
// the host via the dlcall INVOKE_PROC syscall.
//
// SPDX-License-Identifier: MIT
//
//===----------------------------------------------------------------------===//
//
// This header holds NO function declarations: a side must not see the other
// side's entry points. Guest-only runtime functions live in echo/GuestRuntime/*.h,
// host-only ones in echo/HostRuntime/*.h. Every type/offset/magic here is an ABI
// contract shared with the Echo compiler pass (X86EchoCallLowering hardcodes the
// same layout); the names are contracts and are not subject to the project's C++
// naming style.
//
//===----------------------------------------------------------------------===//
#ifndef ECHO_ABI_CALLBLOCK_H
#define ECHO_ABI_CALLBLOCK_H

#ifdef __cplusplus
extern "C" {
#endif

/// __echo_entry's first argument: which kind of invocation this is.
enum EchoInvocationId {
    ECHO_IID_CALL = 0, ///< start a native call from a guest stub
    ECHO_IID_RESUME,   ///< resume a parked host coroutine after a guest reentry
    ECHO_IID_THREAD_ENTRY, ///< run blk->native_target(blk->argblock) on a fresh
                           ///< guest-context thread (thread bridge): the host
                           ///< start_routine of a host-library worker thread that
                           ///< was inverted onto the emulator's clone path so it can
                           ///< call back into the guest.
    ECHO_IID_RELEASE_EXCEPTION, ///< drop the host keep-ref after the guest has
                                ///< acquired its own exception_ptr reference
    ECHO_IID_THREAD_DRAIN, ///< drain host TSD while an ordinary guest pthread
                           ///< still has a live guest servicing loop
    ECHO_IID_ERRNO_LOC, ///< ask the host for &errno on THIS thread. errno is a
                        ///< per-thread libc variable and the two worlds have one
                        ///< each: a native body that fails sets the HOST's, while
                        ///< the guest then reads its OWN and sees a stale value.
                        ///< The guest fetches this address once per thread and
                        ///< copies through it after every crossing (docs/07).
    ECHO_IID_GUEST_EXITING, ///< the guest process is running its own destructors:
                            ///< drain the main thread's host TSD, and record that no
                            ///< further reentry can be serviced (whether the host's
                            ///< own exit-time code then runs is the emulator's
                            ///< choice, and qemu and box64 disagree -- see
                            ///< docs/04-reentry.md §6.5)
};

/// Block status, written by __echo_entry / the entry thunk.
enum EchoStatus {
    ECHO_DONE = 0,          ///< call complete, ret valid
    ECHO_NEED_REENTRY,      ///< host parked, a guest reentry is pending
    ECHO_PENDING_EXCEPTION, ///< cross-boundary exception, eptr -> EchoException
    ECHO_NEST_OVERFLOW,     ///< runtime nesting limit reached; no call was started
};

/// Cross-boundary exception channel (01-abi-spec §7).
///
/// Tier-2 (faithful): if the concrete type is registered on both sides (the Echo
/// compiler emits a __echo_exc$<type_info::name()> descriptor at each throw site),
/// the host copy-constructs the in-flight object into a unified-heap buffer
/// (`obj`) and records the type name; the guest copy-constructs a real object of
/// that type, sets its guest vtable, and __cxa_throws it — so a guest
/// `catch (Concrete&)` matches and the object's members survive.
///
/// Tier-1 (fallback): when the type is not registered (host-internal type the
/// guest never sees), only `type`/`what` are filled and the guest rethrows a
/// matching std:: exception carrying the message.
enum EchoExcType {
    ECHO_EXC_NONE = 0,
    ECHO_EXC_INVALID_ARGUMENT = 1,
    ECHO_EXC_UNKNOWN = 2,
    /// A GUEST-origin exception whose concrete type the guest binary defines but does
    /// not register (__echo_exc$) -- e.g. a plain-g++ guest throwing its own type into
    /// a native callback. It cannot be marshalled faithfully, so the guest stashes the
    /// live exception_ptr and puts its retrieval token in `obj`; when the exception
    /// round-trips back the guest rethrows the ORIGINAL by token, preserving its true
    /// type. See GuestRT/ReentryCatch.cpp + Rethrow.cpp, HostRT/{ReentryRethrow,Catch}.
    ECHO_EXC_GUEST_ORIGIN = 3,
    /// A GUEST exception OBJECT caught on the host side (a native function rethrew a guest
    /// std::exception_ptr -- e.g. QFutureInterface::reportException/result). The host must
    /// not run its EH machinery on it (guest type_info/vtable/dtor = guest code); instead it
    /// keeps the shared __cxa exception alive (refcount++) and hands its object pointer back,
    /// and the guest rethrows it by its true type via __cxa_rethrow_primary_exception. `obj`
    /// is the exception object pointer. See HostRT/Catch.cpp + GuestRT/Rethrow.cpp.
    ECHO_EXC_HOST_RETHROW = 4,
};

typedef struct {
    int type;               ///< 0:  enum EchoExcType (Tier-1 fallback)
    const char *type_name;  ///< 8:  Tier-2: type_info::name(), else NULL
    void *obj;              ///< 16: Tier-2: copied object (unified heap), else NULL
    unsigned long size;     ///< 24: Tier-2: object size
    char what[256];         ///< 32: Tier-1 copied what() text
} EchoException;            ///< total 288 bytes (EchoExceptionSize in the pass)

typedef struct EchoCallBlock EchoCallBlock;

/// Per-signature unpacker: reads blk->argblock, calls blk->native_target with the
/// host ABI, writes blk->ret. Emitted by the Echo compiler per signature.
typedef void (*EchoEntryThunk)(EchoCallBlock *blk);

struct EchoCallBlock {
    void *native_target;        ///< 0:  the real native function body
    EchoEntryThunk entry_thunk; ///< 8:  per-signature unpack+call (handshake-bound)
    void *argblock;             ///< 16: flat arg block, 8-byte slots
    void *ret;                  ///< 24: -> return-value buffer, or NULL for void
    int status;                 ///< 32: enum EchoStatus, set by __echo_entry

    void *reentry; ///< 40: -> EchoReentry while parked (04-reentry)
    void *eptr;    ///< 48: -> EchoException payload (01-abi-spec §7)
    void *varargs; ///< 56: -> CVargEntry[], or NULL (03-varargs)
};

/// Per-function binding, filled at the bootstrap handshake: the face stub reads
/// its slot to find the native body + its entry thunk.
typedef struct {
    void *native_target;
    EchoEntryThunk entry_thunk;
} EchoBinding;

/// One entry of a native module's directory (B-3a, name-based binding): the
/// neutral name of a boundary function plus its native body + entry thunk. The
/// native side builds this with DIRECT references to its own functions (so it
/// works for functions the linker keeps local/hidden, which dlsym could not
/// reach), and the handshake matches it by name against the face's list. The
/// name is echoNeutralName(F) so a va_list function whose mangling differs by
/// architecture (P13__va_list_tag vs St9__va_list) still matches.
typedef struct {
    const char *name;      ///< echoNeutralName of the function
    void *native_target;   ///< the native body
    EchoEntryThunk thunk;  ///< its entry thunk
} EchoNatDirEntry;

/// Reentry descriptor (04-reentry). When host native code must call back into the
/// guest mid-call, the host reenter path fills one of these, points
/// EchoCallBlock::reentry at it, sets status = ECHO_NEED_REENTRY, and parks the
/// coroutine. The guest reentry loop runs `dispatcher`, the guest-side unpacker
/// that calls `guest_fn` with argblock/ret. Mirror of the outbound block.
typedef struct EchoReentry {
    void *guest_fn;   ///< guest code address to run (a guest vtable slot / fn ptr)
    void *dispatcher; ///< GUEST-side unpacker for this signature (face-emitted)
    void *argblock;   ///< flat 8-byte-slot args, packed by the host reenter thunk
    void *ret;        ///< where the dispatcher writes the return value
    void *varargs;    ///< CVargEntry[] for a variadic callback, else NULL
} EchoReentry;

/// The guest-side dispatcher signature (the face's echo_redispatch_<key>).
typedef void (*EchoDispatcher)(void *guest_fn, void *argblock, void *ret,
                               void *varargs);

/// Variadic tail (03-varargs). EchoCallBlock/EchoReentry::varargs point at a
/// CVargEntry[] (echo/ABI/VarArgs.h): the face variadic stub boxes the tail with
/// the registered capture op, the native thunk rebuilds the call with replay via
/// ffcall. Architecture-neutral — no x86_64 register-save image. (The old V-1
/// EchoVarArgs snapshot + __echo_vreplay are retired.)

/// B-3a publish handshake (02-implementation, B-3). The native side exports
/// __echo_publish_<srchash>; one INVOKE_PROC matches this module's native
/// directory against the face's neutral-name list and fills the binding table in
/// guest memory by NAME (so arch-conditional code that leaves the face and native
/// worklists legitimately different — e.g. an x86-only class under #ifdef — binds
/// the common functions and leaves each side's extras unbound). Offsets are
/// hardcoded by the Echo compiler.
typedef struct {
    void *table;                     ///< 0:  guest EchoBinding[count]
    unsigned int count;              ///< 8:  number of face slots / names[] entries
    unsigned int _pad;               ///< 12
    unsigned long long host_version; ///< 16: out; native writes its own hash
    const char **names;              ///< 24: face's neutral-name list [count], sorted
} EchoPublish;

/// ECHO_STUB_MAGIC marks a self-identifying stub so the host __echo_check_call
/// recognizes one of our stubs by fixed offset. Must match the compiler's value.
#define ECHO_STUB_MAGIC 0xECC0DE5731B0FECEULL

/// Self-identifying stub (02-implementation 地基 D). The face holds one per escaping function; a
/// function's address-as-a-value (vtable slot, &fn) IS the stub's start.
///   instr[8]    = `jmp *[rip+2]` -> pack_target (a guest `call` crosses)
///   pack_target = the per-signature pack+cross helper (face_pack_*)
///   target_slot = &echo_bind[i].native_target; *target_slot is the real native
///                 body, filled at the handshake
///   magic       = ECHO_STUB_MAGIC
typedef struct {
    unsigned char instr[8];   ///< 0
    void *pack_target;        ///< 8
    void **target_slot;       ///< 16: -> &echo_bind[i].native_target
    unsigned long long magic; ///< 24
} EchoStub;

/// Reverse-published name->pointer entry (02-implementation C-1 / 04-reentry §2.5). Used for
/// both face vtable address points (__echo_facevt$<vtable>) and guest
/// redispatchers (__echo_redisp$<key>).
typedef struct {
    const char *name; ///< native slot symbol
    void *ap;         ///< value to store into it
} EchoFaceVtEntry;

#ifdef __cplusplus
}
#endif

#endif // ECHO_ABI_CALLBLOCK_H
