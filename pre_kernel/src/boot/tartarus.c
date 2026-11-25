#include <boot/boot.h>
#include <boot/bootinfo.h>
#include <boot/tartarus.h>
#include <init.h>
#include <log.h>
#include <memory/pmm.h>
#include <memory/ptm.h>
#include <panic.h>
#include <runtime/mem.h>
#include <stddef.h>
#include <stdint.h>
#include <tartarus.h>

__attribute__((no_sanitize("undefined"))) // @todo: tartarus misaligned pointer bug
[[noreturn, gnu::used]] void prekernel_entry_tartarus(tartarus_boot_info_t* tartarus_boot_info, uint16_t version) {
    uint8_t major = version >> 8;
    uint8_t minor = version & 0xff;

    log_print_raw("\n");
    log_print("Tartarus protcol version: %u.%u\n", major, minor);
    if(major != 5) { panic("Unsupported Tartarus protocol version\n"); }

    const uintptr_t hhdm_offset = tartarus_boot_info->hhdm_offset;
    pmm_init(hhdm_offset);

    for(size_t i = 0; i < tartarus_boot_info->mm_entry_count; i++) {
        tartarus_mm_entry_t* mm_entry = &tartarus_boot_info->mm_entries[i];
        pmm_map_type_t type;
        switch(mm_entry->type) {
            case TARTARUS_MM_TYPE_EFI_RECLAIMABLE:        [[fallthrough]];
            case TARTARUS_MM_TYPE_USABLE:                 type = PMM_MAP_TYPE_FREE; break;
            case TARTARUS_MM_TYPE_ACPI_TABLES:            type = PMM_MAP_TYPE_USED; break;
            case TARTARUS_MM_TYPE_DEVICE_TREE:            type = PMM_MAP_TYPE_USED; break;
            case TARTARUS_MM_TYPE_RESERVED:               type = PMM_MAP_TYPE_RESERVED; break;
            case TARTARUS_MM_TYPE_BOOTLOADER_RECLAIMABLE: type = PMM_MAP_TYPE_BOOTLOADER_RECLAIMABLE; break;
            case TARTARUS_MM_TYPE_ACPI_RECLAIMABLE:       type = PMM_MAP_TYPE_ACPI_RECLAIMABLE; break;
            case TARTARUS_MM_TYPE_ACPI_NVS:               type = PMM_MAP_TYPE_ACPI_NVS; break;
            case TARTARUS_MM_TYPE_BAD:                    type = PMM_MAP_TYPE_BAD; break;
            default:                                      panic("Invalid memory map entry type");
        }
        pmm_map_add(mm_entry->base, mm_entry->length, type);
    }

    size_t module_name_bytes = 0;
    for(size_t i = 0; i < tartarus_boot_info->module_count; i++) { module_name_bytes += strlen(tartarus_boot_info->modules[i].name) + 1; }

    size_t block_size = bootinfo_block_size(tartarus_boot_info->framebuffer_count, tartarus_boot_info->module_count, module_name_bytes, 0);
    uintptr_t physical = (uintptr_t) pmm_alloc(block_size / PTM_PAGE_GRANULARITY);

    bootinfo_builder_t builder;
    bootinfo_builder_init(&builder, physical, hhdm_offset, hhdm_offset, tartarus_boot_info->framebuffer_count, tartarus_boot_info->module_count, 0);
    bootinfo_init(builder.info, tartarus_boot_info->boot_timestamp, hhdm_offset, tartarus_boot_info->acpi_rsdp_address, tartarus_boot_info->device_tree_address);

    if(builder.info->rdsp_physical) { log_print("acpi: supported\n"); }
    if(builder.info->dtb_physical) { log_print("dtb: supported\n"); }

#if defined(__riscv) && __riscv_xlen == 64
    for(size_t i = 0; i < tartarus_boot_info->cpu_count; i++) {
        if(tartarus_boot_info->cpus[i].type == TARTARUS_CPU_TYPE_BSP) {
            g_boot_bsp_arch_id = tartarus_boot_info->cpus[i].hart_id;
            log_print("riscv: boot hart id %lu\n", g_boot_bsp_arch_id);
        }
    }
#endif

    builder.info->framebuffer_count = tartarus_boot_info->framebuffer_count;
    builder.info->framebuffers = (bootinfo_framebuffer_t*) bootinfo_builder_store(&builder, builder.framebuffer_cursor);
    for(size_t i = 0; i < tartarus_boot_info->framebuffer_count; i++) {
        tartarus_framebuffer_t* tartarus_fb = &tartarus_boot_info->framebuffers[i];
        boot_framebuffer_t framebuffer = {
            .vaddr = (void*) tartarus_fb->vaddr,
            .paddr = tartarus_fb->paddr,
            .size = tartarus_fb->size,
            .width = tartarus_fb->width,
            .height = tartarus_fb->height,
            .pitch = tartarus_fb->pitch,
            .bpp = tartarus_fb->bpp,
            .red_position = tartarus_fb->mask.red_position,
            .red_size = tartarus_fb->mask.red_size,
            .green_position = tartarus_fb->mask.green_position,
            .green_size = tartarus_fb->mask.green_size,
            .blue_position = tartarus_fb->mask.blue_position,
            .blue_size = tartarus_fb->mask.blue_size,
        };
        bootinfo_builder_add_framebuffer(&builder, &framebuffer);
    }

    builder.info->module_count = tartarus_boot_info->module_count;
    builder.info->modules = (bootinfo_module_t*) bootinfo_builder_store(&builder, builder.module_cursor);
    for(size_t i = 0; i < tartarus_boot_info->module_count; i++) {
        tartarus_module_t* tartarus_module = &tartarus_boot_info->modules[i];
        boot_module_t module = {
            .name = tartarus_module->name,
            .data = (void*) (uintptr_t) (tartarus_module->paddr + hhdm_offset),
            .size = tartarus_module->size,
            .destination_offset = hhdm_offset,
        };
        bootinfo_builder_add_module(&builder, &module);
    }

    prekernel_init(builder.info);
    while(1);
}
