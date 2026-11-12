# Echo toolchain (wrapper mode, docs/06 §7): ONE build produces both halves. The
# compiler is qt/echo-cc.py, which emits the native library (real name, primary
# output) + the face (echo-face/ shadow + embedded .echo_face section). The wrapper
# owns Echo runtime linkage, so no runtime linker flags are set here.
#
# The wrapper reads ECHO_REAL_CXX / ECHO_RT / ECHO_STUBS from the environment; set
# them in the shell that runs cmake configure AND build.

# The NATIVE half's arch drives CMake (it is the primary output CMake probes and
# links). Same-arch (x64->x64) is the default; a cross build (x86 guest -> aarch64
# host) sets ECHO_NATIVE_TARGET=aarch64-unknown-linux-echo in the environment, which
# both this file (for CMake's processor/target/sysroot) and echo-cc.py (for the
# actual native compile) read — one env var keeps the two in lockstep. riscv64 works
# the same way; the processor name is taken from the triple rather than hard-coded so
# a third host arch needs no edit here.
set(CMAKE_SYSTEM_NAME Linux)
if(DEFINED ENV{ECHO_NATIVE_TARGET} AND "$ENV{ECHO_NATIVE_TARGET}" MATCHES "^(aarch64|riscv64)")
  set(CMAKE_SYSTEM_PROCESSOR "${CMAKE_MATCH_1}")
  set(_echo_native_target "$ENV{ECHO_NATIVE_TARGET}")
  # CMake's own compile/link probes must see the cross sysroot the same way the
  # wrapper's NATIVE_FLAGS do; point find_library/find_path at it, not the host.
  if(DEFINED ENV{ECHO_SYSROOT})
    set(CMAKE_SYSROOT "$ENV{ECHO_SYSROOT}")
    set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
    set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
    set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
    set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)
    # A dependent Qt module (qtdeclarative) has to find the ARM64 Echo qtbase, which lives
    # outside the sysroot -- and PACKAGE mode ONLY would not look there. Naming the roots
    # explicitly keeps that restriction: relaxing the mode to BOTH would instead let this
    # host's own x86 Qt6 satisfy find_package(Qt6) and silently produce a mixed build.
    if(DEFINED ENV{ECHO_FIND_ROOTS})
      list(APPEND CMAKE_FIND_ROOT_PATH "$ENV{ECHO_SYSROOT}")
      foreach(_echo_root $ENV{ECHO_FIND_ROOTS})
        list(APPEND CMAKE_FIND_ROOT_PATH "${_echo_root}")
      endforeach()
    endif()
  endif()
else()
  set(CMAKE_SYSTEM_PROCESSOR x86_64)
  set(_echo_native_target x86_64-unknown-linux-echo)
endif()

# One wrapper for both; it picks clang vs clang++ by source extension.
set(CMAKE_C_COMPILER   "${CMAKE_CURRENT_LIST_DIR}/echo-cc.py")
set(CMAKE_CXX_COMPILER "${CMAKE_CURRENT_LIST_DIR}/echo-cc.py")

# Mirror static-archive creation into echo-face/ so face links pull face archives.
set(CMAKE_AR     "${CMAKE_CURRENT_LIST_DIR}/echo-ar.py" CACHE FILEPATH "" FORCE)
set(CMAKE_RANLIB "${CMAKE_CURRENT_LIST_DIR}/echo-ar.py" CACHE FILEPATH "" FORCE)
# Match CMake's --target to the native half. echo-cc.py prepends its own
# --target=ECHO_NATIVE_TARGET; keeping these equal avoids a conflicting override.
set(CMAKE_C_COMPILER_TARGET   "${_echo_native_target}")
set(CMAKE_CXX_COMPILER_TARGET "${_echo_native_target}")
