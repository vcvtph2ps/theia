#pragma once
#include <stdint.h>

#define BOOT_BSP_ARCH_ID_UNKNOWN UINT64_MAX

// architecture specific id of the bootstrap processor (lapic id on x86_64, hart id on riscv64).
extern uint64_t g_boot_bsp_arch_id;
