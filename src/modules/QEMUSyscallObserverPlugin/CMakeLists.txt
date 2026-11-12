# A QEMU user-mode plugin that publishes the current syscall to the thread hook.
find_package(PkgConfig REQUIRED)
pkg_check_modules(GLIB REQUIRED glib-2.0)
if(NOT QEMU_PLUGIN_INCLUDE_DIR)
    message(FATAL_ERROR "set -DQEMU_PLUGIN_INCLUDE_DIR=<qemu>/include/plugins")
endif()
add_library(qemu_syscall_observer SHARED main.c)
target_include_directories(qemu_syscall_observer PRIVATE
    ${QEMU_PLUGIN_INCLUDE_DIR} ${GLIB_INCLUDE_DIRS})
