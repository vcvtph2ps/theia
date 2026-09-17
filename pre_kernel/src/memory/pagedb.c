#include <globals.h>
#include <lib/math.h>
#include <log.h>
#include <memory/pagedb.h>
#include <memory/pmm.h>
#include <memory/ptm.h>
#include <panic.h>
#include <protocol/bootinfo.h>
#include <runtime/mem.h>

static bool pagedb_entry_is_tracked(pmm_map_type_t type) {
    switch(type) {
        case PMM_MAP_TYPE_FREE:
        case PMM_MAP_TYPE_ALLOCATED:
        case PMM_MAP_TYPE_USED:
        case PMM_MAP_TYPE_BOOTLOADER_RECLAIMABLE:
        case PMM_MAP_TYPE_MODULE:
        case PMM_MAP_TYPE_EFI_RECLAIMABLE:
        case PMM_MAP_TYPE_ACPI_RECLAIMABLE:
        case PMM_MAP_TYPE_ACPI_NVS:               return true;
        case PMM_MAP_TYPE_RESERVED:
        case PMM_MAP_TYPE_BAD:                    return false;
    }
    return false;
}

void pagedb_setup(uintptr_t kernel_base, size_t pagedb_entry_size, uintptr_t* pfndb_start, size_t* pfndb_size, uintptr_t* pfndb_bitmap_start, size_t* pfndb_bitmap_size) {
    if(pagedb_entry_size == 0 || PTM_PAGE_GRANULARITY % pagedb_entry_size != 0) {
        panic("invalid pagedb entry size: %zu", pagedb_entry_size);
    }

    size_t system_page_count = MATH_ALIGN_UP(g_globals_boot_info->hhdm_size, PTM_PAGE_GRANULARITY) / PTM_PAGE_GRANULARITY;
    size_t entries_per_page = PTM_PAGE_GRANULARITY / pagedb_entry_size;
    size_t pagedb_page_count = MATH_DIV_CEIL(system_page_count, entries_per_page);
    size_t pagedb_size = pagedb_page_count * PTM_PAGE_GRANULARITY;
    size_t bitmap_size = MATH_ALIGN_UP(MATH_DIV_CEIL(pagedb_page_count, 8), PTM_PAGE_GRANULARITY);
    log_print("pagedb entry size: %zu\n", pagedb_entry_size);

    uintptr_t pagedb_base = g_globals_boot_info->hhdm_offset + g_globals_boot_info->hhdm_size + PTM_PAGE_GRANULARITY;
    uintptr_t bitmap_base = pagedb_base + pagedb_size;
    if(bitmap_base + bitmap_size > kernel_base) {
        panic("pagedb overlaps with kernel image! pagedb_end=0x%016lx, kernel_base=0x%016lx", bitmap_base + bitmap_size, kernel_base);
    }

    void* bitmap_phys = pmm_alloc_ext(bitmap_size / PTM_PAGE_GRANULARITY, PTM_PAGE_GRANULARITY, PMM_MAP_TYPE_USED);
    memset((void*) ((uintptr_t) bitmap_phys + g_globals_boot_info->hhdm_offset), 0, bitmap_size);
    ptm_map(bitmap_base, (uint64_t) (uintptr_t) bitmap_phys, bitmap_size, PTM_FLAG_READ | PTM_FLAG_WRITE);
    uint8_t* bitmap = (uint8_t*) ((uintptr_t) bitmap_phys + g_globals_boot_info->hhdm_offset);

    // @note: find which pagedb pages are backed by actual physical memory
    size_t map_entry_count = g_pmm_map_size;
    for(size_t page_index = 0; page_index < pagedb_page_count; page_index++) {
        uint64_t start_pfn = page_index * entries_per_page;
        uint64_t start_addr = start_pfn * PTM_PAGE_GRANULARITY;
        uint64_t end_addr = MATH_MIN(start_pfn + entries_per_page, system_page_count) * PTM_PAGE_GRANULARITY;

        for(size_t i = 0; i < map_entry_count; i++) {
            pmm_map_entry_t* entry = &g_pmm_map[i];
            if(!pagedb_entry_is_tracked(entry->type)) continue;
            if(entry->base >= end_addr) break; // map is sorted by base
            if(entry->base + entry->length > start_addr) {
                bitmap[page_index / 8] |= (uint8_t) (1 << (page_index % 8));
                break;
            }
        }
    }

    // @note: allocate the pagedb
    size_t present_count = 0;
    for(size_t page_index = 0; page_index < pagedb_page_count; page_index++) {
        if((bitmap[page_index / 8] & (1 << (page_index % 8))) == 0) continue;

        uintptr_t vaddr = pagedb_base + page_index * PTM_PAGE_GRANULARITY;
        void* paddr = pmm_alloc_ext(1, PTM_PAGE_GRANULARITY, PMM_MAP_TYPE_USED);
        memset((void*) ((uintptr_t) paddr + g_globals_boot_info->hhdm_offset), 0, PTM_PAGE_GRANULARITY);
        ptm_map(vaddr, (uint64_t) (uintptr_t) paddr, PTM_PAGE_GRANULARITY, PTM_FLAG_READ | PTM_FLAG_WRITE);
        present_count++;
    }

    log_print("pagedb pages: %zu/%zu present\n", present_count, pagedb_page_count);

    *pfndb_start = pagedb_base;
    *pfndb_size = pagedb_size;
    *pfndb_bitmap_start = bitmap_base;
    *pfndb_bitmap_size = bitmap_size;
}
