#pragma once
#include <protocol/bootinfo.h>

[[noreturn]] void prekernel_init(bootinfo_t* boot_info); // NOLINT
