#pragma once
#include <arch.h>
#include <boot/core.h>
#include <stdint.h>

void arch_machine_init(core_start_info_t* core_info, bootinfo_kernel_info_t* kernel_info);
