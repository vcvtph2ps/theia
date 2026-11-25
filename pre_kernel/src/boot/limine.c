#include <boot/boot.h>
#include <boot/bootinfo.h>
#include <boot/limine.h>
#include <init.h>
#include <limine.h>
#include <log.h>
#include <memory/pmm.h>
#include <memory/ptm.h>
#include <panic.h>
#include <runtime/mem.h>
#include <stddef.h>
#include <stdint.h>

#define LIMINE_REQUEST [[gnu::used, gnu::section(".limine_requests")]]

LIMINE_REQUEST volatile struct limine_bootloader_info_request g_limine_bootloader_info_request = { .id = LIMINE_BOOTLOADER_INFO_REQUEST_ID, .revision = 0 };
LIMINE_REQUEST volatile struct limine_framebuffer_request g_framebuffer_request = { .id = LIMINE_FRAMEBUFFER_REQUEST_ID, .revision = 0 };
LIMINE_REQUEST volatile struct limine_hhdm_request g_hhdm_request = { .id = LIMINE_HHDM_REQUEST_ID, .revision = 0 };
LIMINE_REQUEST volatile struct limine_memmap_request g_memmap_request = { .id = LIMINE_MEMMAP_REQUEST_ID, .revision = 0 };
LIMINE_REQUEST volatile struct limine_entry_point_request g_entrypoint_request = { .id = LIMINE_ENTRY_POINT_REQUEST_ID, .entry = prekernel_entry_limine, .revision = 0 };
LIMINE_REQUEST volatile struct limine_executable_cmdline_request g_cmdline_request = { .id = LIMINE_EXECUTABLE_CMDLINE_REQUEST_ID, .revision = 0 };

#ifdef __x86_64__
LIMINE_REQUEST volatile struct limine_paging_mode_request g_paging_mode_request = {
    .id = LIMINE_PAGING_MODE_REQUEST_ID,
    .revision = 0,
    .mode = LIMINE_PAGING_MODE_X86_64_5LVL,
    .max_mode = LIMINE_PAGING_MODE_X86_64_5LVL,
    .min_mode = LIMINE_PAGING_MODE_X86_64_4LVL,
};
#endif

#if defined(__riscv) && __riscv_xlen == 64
LIMINE_REQUEST volatile struct limine_riscv_bsp_hartid_request g_riscv_bsp_hartid_request = {
    .id = LIMINE_RISCV_BSP_HARTID_REQUEST_ID,
    .revision = 0,
};
#endif

LIMINE_REQUEST volatile struct limine_rsdp_request g_rsdp_request = {
    .id = LIMINE_RSDP_REQUEST_ID,
    .revision = 0,
};

LIMINE_REQUEST volatile struct limine_dtb_request g_dtb_request = {
    .id = LIMINE_DTB_REQUEST_ID,
    .revision = 0,
};

LIMINE_REQUEST volatile struct limine_date_at_boot_request g_boottime_request = {
    .id = LIMINE_DATE_AT_BOOT_REQUEST_ID,
    .revision = 0,
};

LIMINE_REQUEST volatile struct limine_internal_module g_initramfs = {
    .path = "initramfs.rdk.zst",
    .string = "initramfs.rdk.zst",
    .flags = LIMINE_INTERNAL_MODULE_REQUIRED,
};

LIMINE_REQUEST volatile struct limine_internal_module g_ksym = {
    .path = "kernel.ksym",
    .string = "kernel.ksym",
    .flags = 0,
};

LIMINE_REQUEST volatile struct limine_internal_module* g_modules[] = { &g_initramfs, &g_ksym };

LIMINE_REQUEST volatile struct limine_module_request g_module_request = { .id = LIMINE_MODULE_REQUEST_ID, .revision = 1, .internal_modules = (struct limine_internal_module**) &g_modules, .internal_module_count = 2 };

LIMINE_REQUEST volatile uint64_t g_limine_base_revision[] = LIMINE_BASE_REVISION(6);
[[gnu::used, gnu::section(".limine_requests_start")]] volatile uint64_t g_limine_requests_start_marker[] = LIMINE_REQUESTS_START_MARKER;
[[gnu::used, gnu::section(".limine_requests_end")]] volatile uint64_t g_limine_requests_end_marker[] = LIMINE_REQUESTS_END_MARKER;

[[noreturn, gnu::used]] void prekernel_entry_limine(void) {
    if(LIMINE_LOADED_BASE_REVISION_VALID(g_limine_base_revision)) {
        log_print("Booted via limine protocol version %ld", LIMINE_LOADED_BASE_REVISION(g_limine_base_revision));
    } else {
        panic("Booted with invalid limine base revision");
    }
    if(!LIMINE_BASE_REVISION_SUPPORTED(g_limine_base_revision)) { panic("Booted with unsupported limine base revision"); }
    log_print_raw("\n");
    if(g_limine_bootloader_info_request.response) {
        log_print("bootloader info: name=\"%s\" version=\"%s\"\n", g_limine_bootloader_info_request.response->name, g_limine_bootloader_info_request.response->version);
    } else {
        log_print("Limine bootloader info request failed");
    }

    if(g_hhdm_request.response == nullptr) { panic("limine did not provide a higher half direct map"); }
    const uintptr_t hhdm_offset = g_hhdm_request.response->offset;
    pmm_init(hhdm_offset);

    if(g_memmap_request.response == nullptr) { panic("limine did not provide a memory map"); }
    for(size_t i = 0; i < g_memmap_request.response->entry_count; i++) {
        struct limine_memmap_entry* mm_entry = g_memmap_request.response->entries[i];
        pmm_map_type_t type;
        switch(mm_entry->type) {
            case LIMINE_MEMMAP_USABLE:                 type = PMM_MAP_TYPE_FREE; break;
            case LIMINE_MEMMAP_RESERVED:               type = PMM_MAP_TYPE_RESERVED; break;
            case LIMINE_MEMMAP_FRAMEBUFFER:            type = PMM_MAP_TYPE_RESERVED; break;
            case LIMINE_MEMMAP_ACPI_RECLAIMABLE:       type = PMM_MAP_TYPE_ACPI_RECLAIMABLE; break;
            case LIMINE_MEMMAP_ACPI_NVS:               type = PMM_MAP_TYPE_ACPI_NVS; break;
            case LIMINE_MEMMAP_BAD_MEMORY:             type = PMM_MAP_TYPE_BAD; break;
            case LIMINE_MEMMAP_BOOTLOADER_RECLAIMABLE: type = PMM_MAP_TYPE_BOOTLOADER_RECLAIMABLE; break;
            case LIMINE_MEMMAP_EXECUTABLE_AND_MODULES: type = PMM_MAP_TYPE_BOOTLOADER_RECLAIMABLE; break;
            case LIMINE_MEMMAP_RESERVED_MAPPED:        type = PMM_MAP_TYPE_USED; break;
            default:                                   panic("Invalid memory map entry type");
        }
        pmm_map_add(mm_entry->base, mm_entry->length, type);
    }

    if(g_module_request.response == nullptr) { panic("limine did not provide modules"); }
    const size_t module_count = g_module_request.response->module_count;
    const size_t framebuffer_count = g_framebuffer_request.response == nullptr ? 0 : g_framebuffer_request.response->framebuffer_count;

    size_t module_name_bytes = 0;
    for(size_t i = 0; i < module_count; i++) { module_name_bytes += strlen(g_module_request.response->modules[i]->path) + 1; }

    const char* cmdline = g_cmdline_request.response == nullptr ? nullptr : g_cmdline_request.response->cmdline;
    const size_t cmdline_size = cmdline == nullptr ? 0 : strlen(cmdline);
    const size_t command_line_bytes = cmdline == nullptr ? 0 : cmdline_size + 1;

    size_t block_size = bootinfo_block_size(framebuffer_count, module_count, module_name_bytes, command_line_bytes);
    uintptr_t physical = (uintptr_t) pmm_alloc(block_size / PTM_PAGE_GRANULARITY);

    uintptr_t rsdp_physical = 0;
    if(g_rsdp_request.response && g_rsdp_request.response->address) {
        log_print("acpi: supported\n");
        rsdp_physical = (uintptr_t) g_rsdp_request.response->address - hhdm_offset;
    }
    uintptr_t dtb_physical = 0;
    if(g_dtb_request.response && g_dtb_request.response->dtb_ptr) {
        log_print("dtb: supported\n");
        dtb_physical = (uintptr_t) g_dtb_request.response->dtb_ptr - hhdm_offset;
    }

    const uint64_t boot_timestamp = g_boottime_request.response == nullptr ? 0 : g_boottime_request.response->timestamp;
    bootinfo_builder_t builder;
    bootinfo_builder_init(&builder, physical, hhdm_offset, hhdm_offset, framebuffer_count, module_count, command_line_bytes);
    bootinfo_init(builder.info, boot_timestamp, hhdm_offset, rsdp_physical, dtb_physical);
    bootinfo_builder_set_command_line(&builder, cmdline, cmdline_size);

#if defined(__riscv) && __riscv_xlen == 64
    if(g_riscv_bsp_hartid_request.response) {
        g_boot_bsp_arch_id = g_riscv_bsp_hartid_request.response->bsp_hartid;
        log_print("riscv: boot hart id %lu\n", g_boot_bsp_arch_id);
    }
#endif

    builder.info->framebuffer_count = framebuffer_count;
    builder.info->framebuffers = framebuffer_count == 0 ? nullptr : (bootinfo_framebuffer_t*) bootinfo_builder_store(&builder, builder.framebuffer_cursor);
    for(size_t i = 0; i < framebuffer_count; i++) {
        struct limine_framebuffer* limine_fb = g_framebuffer_request.response->framebuffers[i];
        boot_framebuffer_t framebuffer = {
            .vaddr = limine_fb->address,
            .paddr = (uintptr_t) limine_fb->address - hhdm_offset,
            .size = limine_fb->pitch * limine_fb->height,
            .width = limine_fb->width,
            .height = limine_fb->height,
            .pitch = limine_fb->pitch,
            .bpp = limine_fb->bpp,
            .red_position = limine_fb->red_mask_shift,
            .red_size = limine_fb->red_mask_size,
            .green_position = limine_fb->green_mask_shift,
            .green_size = limine_fb->green_mask_size,
            .blue_position = limine_fb->blue_mask_shift,
            .blue_size = limine_fb->blue_mask_size,
        };
        bootinfo_builder_add_framebuffer(&builder, &framebuffer);
    }

    builder.info->module_count = module_count;
    builder.info->modules = (bootinfo_module_t*) bootinfo_builder_store(&builder, builder.module_cursor);
    for(size_t i = 0; i < module_count; i++) {
        struct limine_file* limine_module = g_module_request.response->modules[i];
        boot_module_t module = {
            .name = limine_module->path,
            .data = limine_module->address,
            .size = limine_module->size,
            .destination_offset = hhdm_offset,
        };
        bootinfo_builder_add_module(&builder, &module);
    }

    prekernel_init(builder.info);
    while(1);
}
