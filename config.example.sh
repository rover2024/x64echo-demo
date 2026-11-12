# Copy this file to .cache/config.sh and edit the paths for your machine.

export X64ECHO_CLANG=/path/to/x64echo/bin/clang++
export X64ECHO_TARGET=x86_64-unknown-linux-echo
export X64ECHO_FACE_TARGET=x86_64-unknown-linux-echo

export X64ECHO_HOST_CC=cc
export X64ECHO_HOST_CXX=c++
export X64ECHO_GUEST_CC=x86_64-linux-gnu-gcc
export X64ECHO_GUEST_CXX=x86_64-linux-gnu-g++

export X64ECHO_QEMU=/path/to/qemu-x86_64
export X64ECHO_PLUGIN=/path/to/libdlcall.so
export X64ECHO_X86_SYSROOT=/path/to/x86_64/sysroot
export X64ECHO_BUILD_RUNTIME=1

# Optional. Defaults are under the repository's build/ directory.
# export X64ECHO_RUNTIME_BUILD=$PWD/build/runtime
# export X64ECHO_BUILD_DIR=$PWD/build/hello-cpp
