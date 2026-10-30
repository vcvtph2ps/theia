#pragma once
#include <protocol/bootinfo.h>
#include <stddef.h>
#include <stdint.h>

/// @note index 0 is always the BSP
extern bootinfo_cpu_t* g_smp_cpus;
extern size_t g_smp_cpu_count;

/**
 * @brief discover all cores
 */
void smp_discover_cpus();

/**
 * @brief append a discovered core to the global cpu list
 */
void smp_add_cpu(uint64_t arch_id);

/**
 * @brief allocate the cpu local block for all discovered cpus and store it in the boot info
 */
void smp_allocate_cpu_local(bootinfo_kernel_info_t* kernel_info);
