project(EchoHostHooks)

# HOST-side boundary "hook" modules: libc-family bridges kept out of the core
# EchoHostRuntime so it stays libc-agnostic. Currently the stdio bridge (Stdio.c,
# docs/07 stream family), the environment bridge (Env.c) and the locale bridge
# (Locale.c). The native
# library is linked with --wrap=<fn> (list in echo_stdio.wrap) so its stdio calls
# bind to this module's __wrap_<fn>, which remaps guest standard streams to the
# host's and forwards to real libc via RTLD_NEXT. Guest counterpart: libEchoGuestHooks.
add_library(${PROJECT_NAME} SHARED Stdio.c Env.c Locale.c)
target_include_directories(${PROJECT_NAME} PUBLIC ${ECHO_SOURCE_DIR}/include)
target_link_libraries(${PROJECT_NAME} PUBLIC ${CMAKE_DL_LIBS})

# Install the wrap-lists next to the library so the native link step can read them.
configure_file(echo_stdio.wrap ${CMAKE_BINARY_DIR}/lib/echo_stdio.wrap COPYONLY)
configure_file(echo_callback.wrap ${CMAKE_BINARY_DIR}/lib/echo_callback.wrap COPYONLY)
configure_file(echo_env.wrap ${CMAKE_BINARY_DIR}/lib/echo_env.wrap COPYONLY)

# Callback.c is deliberately NOT part of this library. Its trampolines have to be
# compiled by echo-cc INTO the library being ported, because their whole job is to
# turn a callback into an ordinary native indirect call and let the compiler pass
# classify it. Built here as plain host code they would do nothing. See the file.
