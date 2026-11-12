# x64echo QEMU-side thread-bridge modules (bespoke; will not be upstreamed).
# ECHO_BUILD_QEMU_MODULES is defined in the root CMakeLists; the modules need the
# QEMU plugin headers + glib, so they are built only on request.
if(ECHO_BUILD_QEMU_MODULES)
    add_subdirectory(EmuThreadHook)
    add_subdirectory(QEMUSyscallObserverPlugin)
endif()
