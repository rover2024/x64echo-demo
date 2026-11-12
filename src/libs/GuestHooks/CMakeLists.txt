project(EchoGuestHooks)

# GUEST-side boundary "hook" modules, the counterpart of libEchoHostHooks and the
# symmetric peer of the core (kept out of GuestInitObj so the bootstrap stays pure).
# A tiny STATIC archive linked into every face with --whole-archive: its modules
# register EchoBootHook entries in the linker-collected `echo_boot` section (per-.so
# section bounds require static linkage into the face), which the bootstrap runs.
# Currently the stdio stream registration (GuestStdio.c, docs/07 stream family), the
# environment vector registration (GuestEnv.c) and the locale bridge (GuestLocale.c).
add_library(${PROJECT_NAME} STATIC
    GuestStdio.c
    GuestEnv.c
    GuestLocale.c
)
target_include_directories(${PROJECT_NAME} PUBLIC ${ECHO_SOURCE_DIR}/include)
