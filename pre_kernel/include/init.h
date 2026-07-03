#pragma once
#include <boot/core.h>
#include <protocol/bootinfo.h>

[[noreturn]] void prekernel_init(bootinfo_t* boot_info); // NOLINT
[[noreturn]] void prekernel_init_ap(core_start_info_t* boot_info); // NOLINT
