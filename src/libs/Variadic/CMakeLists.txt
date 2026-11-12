project(EchoVariadic)

# The variadic adaptor: architecture-neutral CVargEntry[] capture (Extract.c, via
# the vendored printf parser) + native-call replay (Replay.c, via libffi).
# One shared runtime linked by BOTH worlds: the face variadic stub calls the
# capture half, the native entry thunk calls the replay half, and a user's
# ECHO_VAOPS pair (compiled into both face and native) references both symbols.
#
# libffi is linked STATICALLY (the PIC archive) so libEchoVariadic is self-
# contained -- no extra runtime .so to ship, and cross-builds just point at the
# aarch64 archive in the sysroot. libffi handles the aarch64 sret register (x8)
# natively, which the previous avcall backend botched for >16-byte returns.
# Prefer the PIC static archive (libffi_pic.a) so it links cleanly into the shared
# libEchoVariadic; fall back to a plain/shared libffi if that is all that exists.
find_library(ECHO_FFI_LIB NAMES ffi_pic ffi REQUIRED)

add_library(${PROJECT_NAME} SHARED
    Extract.c
    Replay.c
    ${ECHO_SOURCE_DIR}/src/3rdparty/printf/printf.c
    ${ECHO_SOURCE_DIR}/src/3rdparty/printf/printf_scanf.c
)
target_include_directories(${PROJECT_NAME}
    PUBLIC ${ECHO_SOURCE_DIR}/include
    PRIVATE ${ECHO_SOURCE_DIR}/src/3rdparty)
target_link_libraries(${PROJECT_NAME} PRIVATE ${ECHO_FFI_LIB})
