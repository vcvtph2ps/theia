#include <boot/boot.h>
#include <boot/core.h>
#include <init.h>
#include <lib/math.h>
#include <log.h>
#include <memory/pmm.h>
#include <memory/ptm.h>
#include <panic.h>
#include <protocol/bootinfo.h>
#include <runtime/mem.h>
#include <stddef.h>
#include <stdint.h>
#include <tartarus.h>

static tartarus_boot_info_t* g_tartarus_boot_info;

static bool tartarus_core_is_bsp(uint64_t tartarus_core_index) {
    return (g_tartarus_boot_info->cpus[tartarus_core_index].flags & TARTARUS_CPU_FLAG_IS_BSP) != 0;
}

__attribute__((no_sanitize("undefined"))) // @todo: tartarus misaligned pointer bug
static void tartarus_start_ap(uint64_t tartarus_core_index, core_start_info_t* boot_info) {
    *g_tartarus_boot_info->cpus[tartarus_core_index].argument = (uint64_t) boot_info;
    *g_tartarus_boot_info->cpus[tartarus_core_index].park_address = (uintptr_t) prekernel_init_ap; // @note: this can be used directly since tartarus just passes argument in rdi
}

#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wmissing-prototypes"

__attribute__((no_sanitize("undefined"))) // @todo: tartarus misaligned pointer bug
[[noreturn, gnu::used]] void prekernel_entry_tartarus(tartarus_boot_info_t* tartarus_boot_info, uint16_t version) {
    g_tartarus_boot_info = tartarus_boot_info;
    uint8_t major = version >> 8;
    uint8_t minor = version & 0xff;

    log_print_raw("\n");
    log_print("Tartarus protcol version: %u.%u\n", major, minor);
    if(major != 3) { panic("Unsupported Tartarus protocol version\n"); }

    for(size_t i = 0; i < tartarus_boot_info->mm_entry_count; i++) {
        tartarus_mm_entry_t* mm_entry = &tartarus_boot_info->mm_entries[i];
        pmm_map_type_t type;
        switch(mm_entry->type) {
            case TARTARUS_MM_TYPE_EFI_RECLAIMABLE:        [[fallthrough]];
            case TARTARUS_MM_TYPE_USABLE:                 type = PMM_MAP_TYPE_FREE; break;
            case TARTARUS_MM_TYPE_ACPI_TABLES:            type = PMM_MAP_TYPE_USED; break;
            case TARTARUS_MM_TYPE_RESERVED:               type = PMM_MAP_TYPE_RESERVED; break;
            case TARTARUS_MM_TYPE_BOOTLOADER_RECLAIMABLE: type = PMM_MAP_TYPE_BOOTLOADER_RECLAIMABLE; break;
            case TARTARUS_MM_TYPE_ACPI_RECLAIMABLE:       type = PMM_MAP_TYPE_ACPI_RECLAIMABLE; break;
            case TARTARUS_MM_TYPE_ACPI_NVS:               type = PMM_MAP_TYPE_ACPI_NVS; break;
            case TARTARUS_MM_TYPE_BAD:                    type = PMM_MAP_TYPE_BAD; break;
            default:                                      panic("Invalid memory map entry type");
        }
        pmm_map_add(mm_entry->base, mm_entry->length, type);
    }

    size_t boot_info_block_size = sizeof(bootinfo_t);
    boot_info_block_size += sizeof(bootinfo_framebuffer_t) * tartarus_boot_info->framebuffer_count;
    boot_info_block_size += sizeof(bootinfo_module_t) * tartarus_boot_info->module_count;

    for(size_t i = 0; i < tartarus_boot_info->module_count; i++) { boot_info_block_size += strlen(tartarus_boot_info->modules[i].name) + 1; }

    boot_info_block_size = MATH_ALIGN_UP(boot_info_block_size, PTM_PAGE_GRANULARITY);

    bootinfo_t* boot_info = (bootinfo_t*) ((uintptr_t) pmm_alloc(boot_info_block_size / PTM_PAGE_GRANULARITY) + tartarus_boot_info->hhdm_offset);
    boot_info->core_count = tartarus_boot_info->cpu_count;
    boot_info->boot_timestamp = tartarus_boot_info->boot_timestamp;
    boot_info->rdsp_physical = tartarus_boot_info->acpi_rsdp_address;
    boot_info->hhdm_offset = tartarus_boot_info->hhdm_offset;

    uintptr_t boot_info_block_pointer = (uintptr_t) boot_info + sizeof(bootinfo_t);

    boot_info->framebuffer_count = tartarus_boot_info->framebuffer_count;
    boot_info->framebuffers = (bootinfo_framebuffer_t*) boot_info_block_pointer;
    for(size_t i = 0; i < tartarus_boot_info->framebuffer_count; i++) {
        bootinfo_framebuffer_t* framebuffer = (bootinfo_framebuffer_t*) boot_info_block_pointer;
        tartarus_framebuffer_t* tartarus_framebuffer = &tartarus_boot_info->framebuffers[i];

        framebuffer->vaddr = tartarus_framebuffer->vaddr;
        framebuffer->paddr = tartarus_framebuffer->paddr;
        framebuffer->size = tartarus_framebuffer->size;

        framebuffer->width = tartarus_framebuffer->width;
        framebuffer->height = tartarus_framebuffer->height;
        framebuffer->pitch = tartarus_framebuffer->pitch;
        framebuffer->bpp = tartarus_framebuffer->bpp;

        framebuffer->red_position = tartarus_framebuffer->mask.red_position;
        framebuffer->red_size = tartarus_framebuffer->mask.red_size;
        framebuffer->green_position = tartarus_framebuffer->mask.green_position;
        framebuffer->green_size = tartarus_framebuffer->mask.green_size;
        framebuffer->blue_position = tartarus_framebuffer->mask.blue_position;
        framebuffer->blue_size = tartarus_framebuffer->mask.blue_size;

        boot_info_block_pointer += sizeof(bootinfo_framebuffer_t);
    }


    boot_info->module_count = tartarus_boot_info->module_count;
    boot_info->modules = (bootinfo_module_t*) boot_info_block_pointer;
    for(size_t i = 0; i < tartarus_boot_info->module_count; i++) {
        bootinfo_module_t* module = (bootinfo_module_t*) boot_info_block_pointer;
        tartarus_module_t* tartarus_module = &g_tartarus_boot_info->modules[i];

        module->name = (char*) boot_info_block_pointer + sizeof(bootinfo_module_t);
        memcpy((void*) module->name, tartarus_module->name, strlen(tartarus_module->name) + 1);
        boot_info_block_pointer += sizeof(bootinfo_module_t) + strlen(tartarus_module->name) + 1;

        module->phys_addr = tartarus_module->paddr;
        module->size = tartarus_module->size;
    }

    g_boot_start_ap = tartarus_start_ap;
    g_boot_core_is_bsp = tartarus_core_is_bsp;

    prekernel_init(boot_info);
    while(1);
}

#pragma clang diagnostic pop
