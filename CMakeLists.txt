cmake_minimum_required(VERSION 3.19)

project(x64echo VERSION 0.1.0 LANGUAGES C CXX ASM)

# ----------------------------------
# Build Options
# ----------------------------------
# Classify host vs guest by dladdr instead of the default boundary B (docs/23).
option(ECHO_CHECK_DLADDR "classify host vs guest via dladdr instead of the boundary-B symbol" OFF)
option(ECHO_BUILD_GUEST_TARGETS "Build guest targets" ON)
# The QEMU-side thread-bridge modules (bespoke, not upstreamed) need the QEMU
# plugin headers + glib, so they are off by default:
#   cmake -DECHO_BUILD_QEMU_MODULES=ON -DQEMU_PLUGIN_INCLUDE_DIR=<qemu>/include/plugins
option(ECHO_BUILD_QEMU_MODULES "Build the QEMU thread-bridge modules" OFF)

# ----------------------------------
# CMake Settings
# ----------------------------------
# code-layout.md: C++20 for the two genuinely-C++ files (the C-3 exception tunnel);
# everything else in src/libs is plain C. Exceptions are off by default (the tunnel
# files re-enable -fexceptions per-source). PIC: the runtime archives are linked
# into the test .so's.
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
add_compile_options($<$<COMPILE_LANGUAGE:CXX>:-fno-exceptions>)

if(NOT DEFINED CMAKE_RUNTIME_OUTPUT_DIRECTORY)
    set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/bin)
endif()

if(NOT DEFINED CMAKE_LIBRARY_OUTPUT_DIRECTORY)
    set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/lib)
endif()

if(NOT DEFINED CMAKE_ARCHIVE_OUTPUT_DIRECTORY)
    set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/lib)
endif()

if(MSVC)
    set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} /manifest:no")
    set(CMAKE_MODULE_LINKER_FLAGS "${CMAKE_MODULE_LINKER_FLAGS} /manifest:no")
    set(CMAKE_SHARED_LINKER_FLAGS "${CMAKE_SHARED_LINKER_FLAGS} /manifest:no")
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} /utf-8")

    if(NOT DEFINED CMAKE_DEBUG_POSTFIX)
        set(CMAKE_DEBUG_POSTFIX "d")
    endif()
elseif(MINGW)
    set(CMAKE_STATIC_LIBRARY_PREFIX "")
    set(CMAKE_SHARED_LIBRARY_PREFIX "")
endif()

# ----------------------------------
# Project Variables
# ----------------------------------
set(ECHO_VERSION ${PROJECT_VERSION})
set(ECHO_INSTALL_NAME ${PROJECT_NAME})
set(ECHO_SOURCE_DIR ${CMAKE_CURRENT_SOURCE_DIR})

# ----------------------------------
# Main Project
# ----------------------------------
add_subdirectory(src)
