project(EchoGuestInitObj)

# The face's per-image init object (crt1.o analog): a tiny static archive linked
# into every face .so (with --whole-archive, so its constructor is kept). It runs
# the bootstrap handshake and calls into libEchoGuestRuntime.so; it stays
# per-image because it touches per-library symbols (__echo_gen_resolve, the
# echo_stubs section bounds).
add_library(${PROJECT_NAME} STATIC
    Bootstrap.c
)
target_include_directories(${PROJECT_NAME} PUBLIC ${ECHO_SOURCE_DIR}/include)
