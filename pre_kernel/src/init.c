#include <arch.h>
#include <boot/boot.h>
#include <boot/core.h>
#include <init.h>
#include <lib/helpers.h>
#include <lib/math.h>
#include <loader/elfldr.h>
#include <log.h>
#include <memory/pagedb.h>
#include <memory/pmm.h>
#include <memory/ptm.h>
#include <panic.h>
#include <protocol/bootinfo.h>
#include <runtime/mem.h>
#include <smp.h>
#include <uacpi/acpi.h>
#include <uacpi/status.h>
#include <uacpi/tables.h>
#include <uacpi/types.h>
#include <uacpi/uacpi.h>

static bootinfo_kernel_entry_point_t g_kernel_entry_point;

bootinfo_t* g_globals_boot_info = nullptr;

extern uint8_t _binary_kernel_elf_start[]; // NOLINT

#define ANSI_CLEAR "\x1b[2J"
#define ANSI_HOME "\x1b[H"
#define ANSI_RESET "\x1b[0m"

static void find_serial_from_acpi(void) {
    uacpi_table spcr_table;
    if(uacpi_table_find_by_signature(ACPI_SPCR_SIGNATURE, &spcr_table) != UACPI_STATUS_OK) {
        log_print("acpi: SPCR table not found\n");
        return;
    }

    struct acpi_spcr* spcr = (struct acpi_spcr*) spcr_table.hdr;

    bootinfo_serial_t serial = { .type = BOOTINFO_SERIAL_TYPE_NONE };
    if(spcr->base_address.address != 0) {
        if(spcr->base_address.address_space_id == UACPI_ADDRESS_SPACE_SYSTEM_MEMORY) {
            serial.type = BOOTINFO_SERIAL_TYPE_MMIO;
            serial.base = spcr->base_address.address;
            serial.size = 0x1000;
        } else if(spcr->base_address.address_space_id == UACPI_ADDRESS_SPACE_SYSTEM_IO) {
            serial.type = BOOTINFO_SERIAL_TYPE_PIO;
            serial.base = spcr->base_address.address;
        }
    }

    g_globals_boot_info->serial = serial;
    if(serial.type != BOOTINFO_SERIAL_TYPE_NONE) { log_print("acpi: serial %s base=0x%lx size=0x%lx\n", serial.type == BOOTINFO_SERIAL_TYPE_MMIO ? "mmio" : "pio", serial.base, serial.size); }

    uacpi_table_unref(&spcr_table);
}

[[noreturn]] void prekernel_init(bootinfo_t* boot_info) {
    g_globals_boot_info = boot_info;
    arch_init_early();

    if(boot_info->rdsp_physical) {
        uintptr_t temporary_buffer = (uintptr_t) pmm_alloc(2) + g_globals_boot_info->hhdm_offset;
        uacpi_status status = uacpi_setup_early_table_access((void*) temporary_buffer, PTM_PAGE_GRANULARITY * 2);
        if(status != UACPI_STATUS_OK) { panic("failed to setup acpi tables"); }

        find_serial_from_acpi();
    }

#if defined(__x86_64__)
    if(g_globals_boot_info->serial.type == BOOTINFO_SERIAL_TYPE_NONE) { arch_probe_serial(&g_globals_boot_info->serial); }
#endif

    // Discover the cpus and hand the list to the kernel. The kernel is responsible for actually bringing up the application processors.
    smp_discover_cpus();
    boot_info->core_count = g_smp_cpu_count;
    boot_info->cpus = g_smp_cpus;

    ptm_init();
    ptm_map_framebuffers();
    log_framebuffer_init();

    // log_print(ANSI_CLEAR ANSI_HOME ANSI_RESET);
    log_print_raw("\n");
    log_print("Hai :333\n");
    log_print("Core count: %ld\n", boot_info->core_count);

    size_t physical_memory_size = 0;
    size_t pmm_entry_index = 0;
    PMM_MAP_FOREACH(node) {
        pmm_map_entry_t* entry = &node->entry;
        log_print("pmm_entry[%zu]: base=0x%016lx, length=0x%016lx, type=%s (%u)\n", pmm_entry_index++, entry->base, entry->length, pmm_entry_type_to_string(entry->type), entry->type);
        if(entry->type != PMM_MAP_TYPE_RESERVED && entry->type != PMM_MAP_TYPE_BAD) { physical_memory_size += entry->length; }
    }
    log_print("Total physical memory: %zu bytes\n", physical_memory_size);

    log_print("rsdp: 0x%lx\n", boot_info->rdsp_physical);

    elfldr_loader_info_t kernel_image_info;
    if(!elfldr_load_kernel(&kernel_image_info)) { panic("failed to load kernel elf image"); }

#if defined(__riscv) && __riscv_xlen == 64
    if(!arch_parse_extentions()) { panic("failed to parse extentions"); }
#endif

    log_print("Kernel cpu local size: %zu\n", kernel_image_info.kernel_info->cpu_local_size);
    log_print("Kernel pagedb entry size: %zu\n", kernel_image_info.kernel_info->pagedb_entry_size);

    size_t hhdm_size = 0;
    PMM_MAP_FOREACH(node) {
        hhdm_size = MATH_MAX(hhdm_size, node->entry.base + node->entry.length);
    }
    boot_info->hhdm_size = hhdm_size;

    uintptr_t pfndb_start;
    size_t pfndb_size;
    uintptr_t pfndb_bitmap_start;
    size_t pfndb_bitmap_size;
    pagedb_setup(kernel_image_info.kernel_base, kernel_image_info.kernel_info->pagedb_entry_size, &pfndb_start, &pfndb_size, &pfndb_bitmap_start, &pfndb_bitmap_size);
    boot_info->pfndb_start = pfndb_start;
    boot_info->pfndb_size = pfndb_size;
    boot_info->pfndb_bitmap_start = pfndb_bitmap_start;
    boot_info->pfndb_bitmap_size = pfndb_bitmap_size;

    smp_allocate_cpu_local(kernel_image_info.kernel_info);

    core_start_info_t bsp_core_info = { 0 };
    bsp_core_info.core_id = 0;
    bsp_core_info.cpu_local = boot_info->cpulocal_start;
    arch_machine_init(&bsp_core_info, kernel_image_info.kernel_info);

    ptm_create_hhdm_mappings();

    arch_prepare_handoff();

    pmm_sanitize();
    size_t pmm_map_entries = g_pmm_map_size;
    void* memory_map_block;
    while(true) {
        size_t memory_map_block_size = MATH_ALIGN_UP(pmm_map_entries * sizeof(bootinfo_mm_entry_t), PTM_PAGE_GRANULARITY);
        void* memory_map_phys = pmm_alloc(memory_map_block_size / PTM_PAGE_GRANULARITY);
        memory_map_block = (void*) ((uintptr_t) memory_map_phys + boot_info->hhdm_offset);
        if(g_pmm_map_size > pmm_map_entries) {
            pmm_free(memory_map_phys, memory_map_block_size / PTM_PAGE_GRANULARITY);
            pmm_map_entries *= 2;
            log_print("memory map block too small, trying again with %zu entries\n", pmm_map_entries);
        } else {
            break;
        }
    }

    boot_info->mm_entries = (bootinfo_mm_entry_t*) memory_map_block;
    boot_info->mm_entry_count = pmm_map_write_bootinfo((bootinfo_mm_entry_t*) memory_map_block, pmm_map_entries);

    g_kernel_entry_point = kernel_image_info.entry_point;
    boot_info->kernel_segments = kernel_image_info.segments;
    boot_info->kernel_segment_count = kernel_image_info.segment_count;

    arch_handoff_to_kernel(g_kernel_entry_point, bsp_core_info.stack, boot_info);
}
