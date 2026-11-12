project(EchoHostInitObj)

# Reserved: the native side's per-image init object (symmetric to GuestInitObj).
# Currently empty — the native .so needs no per-image startup (s_boundary is a
# process singleton in libEchoHostRuntime.so, and the binding table is filled by
# the guest's handshake). Kept as a wired-in placeholder for future per-native
# host-side startup.
add_library(${PROJECT_NAME} STATIC
    Placeholder.c
)
target_include_directories(${PROJECT_NAME} PUBLIC ${ECHO_SOURCE_DIR}/include)
