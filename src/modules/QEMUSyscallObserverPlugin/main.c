/*
 * QEMUSyscallObserver — a QEMU user-mode plugin that publishes the syscall the
 * current vCPU thread is handling, so the EmuThreadHook LD_PRELOAD interposer can
 * tell "host currency" (inside the dlcall magic syscall, 4096) from guest currency
 * (anything else, a clone included). This is qemu's answer to that question; the
 * hook has a second source for the emulators with no plugin interface -- the Echo
 * host runtime's own __echo_in_host_currency() -- so this plugin is qemu-only.
 *
 * The interposer lives in QEMU's process but cannot see this plugin's symbols
 * (plugins load RTLD_LOCAL). So the direction is inverted: the interposer, being
 * LD_PRELOAD'd, exports echo_set_syscall_num(); this plugin weak-refs it and
 * relays every syscall's number. Both the syscall callback and the interposed
 * pthread_create run on the same vCPU OS thread, so a thread-local number in the
 * interposer is consistent.
 *
 * Together with EmuThreadHook this is x64echo's own bespoke plugin (it will not
 * be upstreamed) that sits alongside the upstream dlcall plugin. Run:
 *   qemu -plugin .../libdlcall.so -plugin .../libqemu_syscall_observer.so ...
 *
 * SPDX-License-Identifier: MIT
 */
#include <stdint.h>

#include <qemu-plugin.h>

QEMU_PLUGIN_EXPORT int qemu_plugin_version = QEMU_PLUGIN_VERSION;

/* Defined by the EmuThreadHook LD_PRELOAD lib; weak so the plugin still loads
 * (as a no-op) when the hook is not preloaded. */
extern void echo_set_syscall_num(int64_t num) __attribute__((weak));

static void on_syscall(unsigned int vcpu, int64_t num, uint64_t a1, uint64_t a2,
                       uint64_t a3, uint64_t a4, uint64_t a5, uint64_t a6,
                       uint64_t a7, uint64_t a8, void *ud)
{
    (void) vcpu; (void) a1; (void) a2; (void) a3; (void) a4;
    (void) a5; (void) a6; (void) a7; (void) a8; (void) ud;
    if (echo_set_syscall_num)
        echo_set_syscall_num(num);
}

static void on_syscall_ret(unsigned int vcpu, int64_t num, int64_t ret, void *ud)
{
    (void) vcpu; (void) num; (void) ret; (void) ud;
    if (echo_set_syscall_num)
        echo_set_syscall_num(-1); /* no syscall in flight on this thread */
}

QEMU_PLUGIN_EXPORT int qemu_plugin_install(qemu_plugin_id_t id,
                                           const qemu_info_t *info, int argc,
                                           char **argv)
{
    (void) info; (void) argc; (void) argv;
    qemu_plugin_register_vcpu_syscall_cb(id, on_syscall, NULL);
    qemu_plugin_register_vcpu_syscall_ret_cb(id, on_syscall_ret, NULL);
    return 0;
}
