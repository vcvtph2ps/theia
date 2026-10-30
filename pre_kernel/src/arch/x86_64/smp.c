#include <arch.h>
#include <arch/cpuid.h>
#include <boot/boot.h>
#include <log.h>

uint64_t arch_smp_bsp_id() {
    if(g_boot_bsp_arch_id == BOOT_BSP_ARCH_ID_UNKNOWN) { g_boot_bsp_arch_id = arch_cpuid(ARCH_CPUID_GET_FEATURES, 0, ARCH_CPUID_EBX) >> 24; }
    return g_boot_bsp_arch_id;
}

void arch_smp_discover_dtb() {
    log_print("x86_64: device tree CPU discovery is not supported\n");
}
