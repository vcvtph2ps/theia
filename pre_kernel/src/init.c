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
#include <uacpi/status.h>
#include <uacpi/uacpi.h>

static bootinfo_kernel_entry_point_t g_kernel_entry_point;

ATOMIC static uint32_t g_ap_init_lock = 0;
ATOMIC static uint32_t g_running_cores = 0;
static uint32_t g_core_count = 0;

[[noreturn]] __attribute__((no_sanitize("undefined"))) static void sync_and_handoff(bootinfo_kernel_entry_point_t entry, uintptr_t stack, bootinfo_t* boot_info, uint64_t core_id) {
    ATOMIC_LOAD_ADD(&g_running_cores, 1, ATOMIC_RELEASE);

    while(ATOMIC_LOAD(&g_running_cores, ATOMIC_ACQUIRE) < g_core_count) { arch_spin_hint(); }

    arch_handoff_to_kernel(entry, stack, boot_info, core_id);
}


bootinfo_t* g_globals_boot_info = nullptr;

[[noreturn]] void prekernel_init_ap(core_start_info_t* boot_info) {
    while(ATOMIC_LOAD(&g_ap_init_lock, ATOMIC_ACQUIRE) == 0);

    arch_machine_init(boot_info);

    sync_and_handoff(g_kernel_entry_point, boot_info->stack, nullptr, boot_info->core_id);
}

extern uint8_t _binary_kernel_elf_start[]; // NOLINT

#define ANSI_CLEAR "\x1b[2J"
#define ANSI_HOME "\x1b[H"
#define ANSI_RESET "\x1b[0m"

[[noreturn]] void prekernel_init(bootinfo_t* boot_info) {
    g_globals_boot_info = boot_info;
    arch_init_early();
    log_framebuffer_init();

    log_print(ANSI_CLEAR ANSI_HOME ANSI_RESET);
    log_print("Hai :333\n");
    log_print("Core count: %ld\n", boot_info->core_count);

    size_t physical_memory_size = 0;
    for(size_t i = 0; i < g_pmm_map_size; i++) {
        pmm_map_entry_t* entry = &g_pmm_map[i];
        log_print("pmm_entry[%zu]: base=0x%016lx, length=0x%016lx, type=%s (%u)\n", i, entry->base, entry->length, pmm_entry_type_to_string(entry->type), entry->type);
        physical_memory_size += entry->length;
    }
    log_print("Total physical memory: %zu bytes\n", physical_memory_size);

    log_print("rsdp: 0x%lx\n", boot_info->rdsp_physical);
    if(boot_info->rdsp_physical) {
        uintptr_t temporary_buffer = (uintptr_t) pmm_alloc(PTM_PAGE_GRANULARITY * 2) + g_globals_boot_info->hhdm_offset;
        uacpi_status status = uacpi_setup_early_table_access((void*) temporary_buffer, PTM_PAGE_GRANULARITY * 2);
        if(status != UACPI_STATUS_OK) { panic("failed to setup acpi tables"); }
    }

    ptm_init();

    elfldr_loader_info_t kernel_image_info;
    if(!elfldr_load_kernel(&kernel_image_info)) { panic("failed to load kernel elf image"); }

#ifdef __ARCH_RISCV64__
    if(!arch_parse_extentions()) { panic("failed to parse extentions"); }
#endif

    log_print("Kernel cpu local size: %zu\n", kernel_image_info.kernel_info->cpu_local_size);
    log_print("Kernel pagedb entry size: %zu\n", kernel_image_info.kernel_info->pagedb_entry_size);

    size_t hhdm_size = 0;
    for(size_t i = 0; i < g_pmm_map_size; i++) { hhdm_size = MATH_MAX(hhdm_size, g_pmm_map[i].base + g_pmm_map[i].length); }
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

    // Allocate per core start info block
    size_t start_info_block_size = MATH_ALIGN_UP(sizeof(core_start_info_t) * boot_info->core_count, PTM_PAGE_GRANULARITY);
    core_start_info_t* start_info_block = (core_start_info_t*) ((uintptr_t) pmm_alloc(start_info_block_size / PTM_PAGE_GRANULARITY) + g_globals_boot_info->hhdm_offset);
    memset(start_info_block, 0, start_info_block_size);

    arch_setup_cpus(start_info_block, boot_info->core_count, kernel_image_info.kernel_info);

    ptm_create_hhdm_mappings();

    if(boot_info->framebuffer_count > 0) {
        for(size_t i = 0; i < boot_info->framebuffer_count; i++) {
            bootinfo_framebuffer_t* fb = &boot_info->framebuffers[i];
            if(fb->paddr < boot_info->hhdm_offset || fb->paddr + fb->size > boot_info->hhdm_offset + boot_info->hhdm_size) {
                ptm_map(fb->paddr + boot_info->hhdm_offset, fb->paddr, MATH_ALIGN_UP(fb->size, PTM_PAGE_GRANULARITY), PTM_FLAG_READ | PTM_FLAG_WRITE);
            }
        }
    }

    arch_prepare_handoff();

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

    boot_info->mm_entry_count = g_pmm_map_size;
    boot_info->mm_entries = (bootinfo_mm_entry_t*) memory_map_block;
    for(size_t i = 0; i < g_pmm_map_size; i++) {
        pmm_map_entry_t* entry = &g_pmm_map[i];
        bootinfo_mm_type_t type;
        switch(entry->type) {
            case PMM_MAP_TYPE_BOOTLOADER_RECLAIMABLE: [[fallthrough]];
            case PMM_MAP_TYPE_EFI_RECLAIMABLE:        [[fallthrough]];
            case PMM_MAP_TYPE_FREE:                   type = BOOTINFO_MM_TYPE_USABLE; break;
            case PMM_MAP_TYPE_ALLOCATED:              type = BOOTINFO_MM_TYPE_RECLAIMABLE; break;
            case PMM_MAP_TYPE_MODULE:                 type = BOOTINFO_MM_TYPE_MODULE; break;
            case PMM_MAP_TYPE_USED:                   type = BOOTINFO_MM_TYPE_USED; break;
            case PMM_MAP_TYPE_ACPI_RECLAIMABLE:       type = BOOTINFO_MM_TYPE_ACPI_RECLAIMABLE; break;
            case PMM_MAP_TYPE_ACPI_NVS:               type = BOOTINFO_MM_TYPE_ACPI_NVS; break;
            case PMM_MAP_TYPE_RESERVED:               type = BOOTINFO_MM_TYPE_RESERVED; break;
            case PMM_MAP_TYPE_BAD:                    type = BOOTINFO_MM_TYPE_BAD; break;
        }

        boot_info->mm_entries[i].phys_base = entry->base;
        boot_info->mm_entries[i].length = entry->length;
        boot_info->mm_entries[i].type = type;
    }

    arch_machine_init(&start_info_block[0]);

    g_kernel_entry_point = kernel_image_info.entry_point;
    boot_info->kernel_segments = kernel_image_info.segments;
    boot_info->kernel_segment_count = kernel_image_info.segment_count;

    g_core_count = boot_info->core_count;
    ATOMIC_STORE(&g_ap_init_lock, 1, ATOMIC_RELEASE);
    sync_and_handoff(g_kernel_entry_point, start_info_block[0].stack, boot_info, 0);
}
