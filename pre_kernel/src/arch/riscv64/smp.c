#include <arch.h>
#include <boot/boot.h>
#include <common/libfdt.h>
#include <globals.h>
#include <log.h>
#include <panic.h>
#include <runtime/mem.h>
#include <smp.h>
#include <stdint.h>

uint64_t arch_smp_bsp_id() {
    // Unlike x86_64, riscv64 has no way to read the current hart id from s-mode, so we have to rely on the bootloader telling us.
    if(g_boot_bsp_arch_id == BOOT_BSP_ARCH_ID_UNKNOWN) { panic("riscv64: boot hart id is unknown"); }
    return g_boot_bsp_arch_id;
}

void arch_smp_discover_dtb() {
    if(g_globals_boot_info->dtb_physical == 0) { return; }

    const void* fdt = (const void*) (g_globals_boot_info->dtb_physical + g_globals_boot_info->hhdm_offset);
    if(fdt_check_header(fdt) != 0) {
        log_print("riscv64: invalid device tree for CPU discovery\n");
        return;
    }

    int cpus_node = fdt_path_offset(fdt, "/cpus");
    if(cpus_node < 0) {
        log_print("riscv64: /cpus node not found for CPU discovery\n");
        return;
    }

    int node;
    fdt_for_each_subnode(node, fdt, cpus_node) {
        const char* devtype = (const char*) fdt_getprop(fdt, node, "device_type", nullptr);
        if(!devtype || strcmp(devtype, "cpu") != 0) { continue; }

        const char* status = (const char*) fdt_getprop(fdt, node, "status", nullptr);
        if(status && strcmp(status, "okay") != 0 && strcmp(status, "ok") != 0) { continue; }

        int len = 0;
        const fdt32_t* reg = (const fdt32_t*) fdt_getprop(fdt, node, "reg", &len);
        if(!reg || len < (int) sizeof(fdt32_t)) { continue; }

        uint64_t hart_id;
        if(len >= 2 * (int) sizeof(fdt32_t)) {
            hart_id = ((uint64_t) fdt32_to_cpu(reg[0]) << 32) | fdt32_to_cpu(reg[1]);
        } else {
            hart_id = fdt32_to_cpu(reg[0]);
        }

        smp_add_cpu(hart_id);
    }
}
