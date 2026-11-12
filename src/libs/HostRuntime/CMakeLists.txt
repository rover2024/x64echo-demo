project(EchoHostRuntime)

# The reentry coroutine is the one architecture-specific runtime file; pick the
# variant for the host arch (x86_64 native, or aarch64 / riscv64 for the cross ports).
if(CMAKE_SYSTEM_PROCESSOR MATCHES "aarch64|arm64")
    set(ECHO_CORO_ASM aarch64/Coroutine.S)
elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "riscv64")
    set(ECHO_CORO_ASM riscv64/Coroutine.S)
else()
    set(ECHO_CORO_ASM x86_64/Coroutine.S)
endif()

# The host-world shared runtime: one copy per process, DT_NEEDED by each native.
# s_boundary's constructor (CheckCall.c) runs once here as a process singleton.
add_library(${PROJECT_NAME} SHARED
    Entry.c
    CheckCall.c
    Catch.cpp
    ReentryRethrow.cpp
    DynCast.cpp
    Publish.c
    ArenaHost.c
    ${ECHO_SOURCE_DIR}/src/libs/Arena/Arena.c
    ${ECHO_SOURCE_DIR}/src/libs/Arena/Operators.cpp
    ${ECHO_CORO_ASM}
)
target_include_directories(${PROJECT_NAME}
    PUBLIC ${ECHO_SOURCE_DIR}/include
    PRIVATE ${ECHO_SOURCE_DIR}/include/echo/HostRuntime)
set_source_files_properties(Catch.cpp PROPERTIES COMPILE_OPTIONS -fexceptions)
set_source_files_properties(ReentryRethrow.cpp PROPERTIES COMPILE_OPTIONS -fexceptions)
target_link_libraries(${PROJECT_NAME} PUBLIC ${CMAKE_DL_LIBS})

if(ECHO_CHECK_DLADDR)
    target_compile_definitions(${PROJECT_NAME} PRIVATE ECHO_CHECK_DLADDR)
endif()
