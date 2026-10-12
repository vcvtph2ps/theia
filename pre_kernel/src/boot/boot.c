#include <boot/boot.h>

#include "tartarus.h"

boot_core_is_bsp_fn g_boot_core_is_bsp = nullptr;
boot_start_ap_fn g_boot_start_ap = nullptr;

[[noreturn, gnu::used]] void prekernel_entry_tartarus(tartarus_boot_info_t* tartarus_boot_info, uint16_t version);

#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wmissing-prototypes"

__attribute__((no_sanitize("undefined"))) // @todo: tartarus misaligned pointer bug
[[noreturn, gnu::used]] void prekernel_entry(void* param1, void* param2) {
    // @todo: support for other than tartarus, limine uses the entry point request
    prekernel_entry_tartarus(param1, (uint16_t) (uintptr_t) param2);
}

#pragma clang diagnostic pop
