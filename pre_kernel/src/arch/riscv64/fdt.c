#include <common/libfdt.h>
#include <globals.h>
#include <lib/math.h>
#include <log.h>
#include <memory/pmm.h>
#include <memory/ptm.h>
#include <panic.h>
#include <protocol/bootinfo.h>
#include <runtime/mem.h>

void parse_isa_string(const char* isa_str);

#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wmissing-prototypes"

size_t arch_fdt_detect_mmu_levels() {
    if(!g_globals_boot_info || g_globals_boot_info->dtb_physical == 0) {
        log_print("fdt: no DTB provided in boot info\n");
        return 0;
    }

    const void* fdt = (const void*) (g_globals_boot_info->dtb_physical + g_globals_boot_info->hhdm_offset);
    if(fdt_check_header(fdt) != 0) {
        log_print("fdt: invalid FDT header at 0x%lx\n", g_globals_boot_info->dtb_physical);
        return 0;
    }

    int cpus_node = fdt_path_offset(fdt, "/cpus");
    if(cpus_node < 0) {
        log_print("fdt: /cpus node not found (err=%d)\n", cpus_node);
        return 0;
    }

    int node;
    fdt_for_each_subnode(node, fdt, cpus_node) {
        const char* devtype = (const char*) fdt_getprop(fdt, node, "device_type", nullptr);
        if(!devtype || strcmp(devtype, "cpu") != 0) { continue; }

        const char* status = (const char*) fdt_getprop(fdt, node, "status", nullptr);
        if(status && strcmp(status, "okay") != 0 && strcmp(status, "ok") != 0) { continue; }

        int len = 0;
        const char* mmu = (const char*) fdt_getprop(fdt, node, "mmu-type", &len);
        if(mmu && len > 0) {
            if(strcmp(mmu, "riscv,sv57") == 0) {
                log_print("fdt: mmu-type = \"%s\"\n", mmu);
                return 5;
            }
            if(strcmp(mmu, "riscv,sv48") == 0) {
                log_print("fdt: mmu-type = \"%s\"\n", mmu);
                return 4;
            }
            if(strcmp(mmu, "riscv,sv39") == 0) {
                log_print("fdt: mmu-type = \"%s\"\n", mmu);
                return 3;
            }
            log_print("fdt: unknown mmu-type \"%s\"\n", mmu);
            panic("Unsupported MMU type");
        }

        break;
    }

    log_print("fdt: could not determine MMU level count from DTB\n");
    return 0;
}

bool arch_fdt_parse_extentions() {
    const void* fdt = (const void*) (g_globals_boot_info->dtb_physical + g_globals_boot_info->hhdm_offset);

    int rc = fdt_check_header(fdt);
    if(rc != 0) {
        log_print("fdt: invalid FDT header at 0x%lx (err=%d)\n", g_globals_boot_info->dtb_physical, rc);
        return false;
    }

    log_print("fdt: FDT at phys 0x%lx, size=%d\n", g_globals_boot_info->dtb_physical, fdt_totalsize(fdt));

    int cpus_node = fdt_path_offset(fdt, "/cpus");
    if(cpus_node < 0) {
        log_print("fdt: /cpus node not found (err=%d)\n", cpus_node);
        return false;
    }

    const char* isa_string = nullptr;

    int node;
    fdt_for_each_subnode(node, fdt, cpus_node) {
        const char* compatible = (const char*) fdt_getprop(fdt, node, "device_type", nullptr);
        if(!compatible || strcmp(compatible, "cpu") != 0) { continue; }

        const char* status = (const char*) fdt_getprop(fdt, node, "status", nullptr);
        if(status && strcmp(status, "okay") != 0 && strcmp(status, "ok") != 0) { continue; }

        int len = 0;
        const char* isa = (const char*) fdt_getprop(fdt, node, "riscv,isa", &len);
        if(isa && len > 0) {
            isa_string = isa;
            break;
        }
    }

    if(!isa_string) {
        log_print("fdt: riscv,isa property not found in any cpu node\n");
        return false;
    }

    log_print("fdt: riscv,isa = \"%s\"\n", isa_string);
    parse_isa_string(isa_string);

    return true;
}
#pragma clang diagnostic pop
