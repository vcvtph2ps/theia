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

    // @note: the pagedb only ever tracks pages that are actually backed by tracked memory
    size_t system_page_count = 0;
    PMM_MAP_FOREACH(node) {
        if(!pagedb_entry_is_tracked(node->entry.type)) continue;
        size_t end = node->entry.base + node->entry.length;
        system_page_count = MATH_MAX(system_page_count, MATH_DIV_CEIL(end, PTM_PAGE_GRANULARITY));
    }

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
    for(size_t page_index = 0; page_index < pagedb_page_count; page_index++) {
        uint64_t start_pfn = page_index * entries_per_page;
        uint64_t start_addr = start_pfn * PTM_PAGE_GRANULARITY;
        uint64_t end_addr = MATH_MIN(start_pfn + entries_per_page, system_page_count) * PTM_PAGE_GRANULARITY;

        PMM_MAP_FOREACH(node) {
            pmm_map_entry_t* entry = &node->entry;
            if(entry->base >= end_addr) break; // map is sorted by base
            if(!pagedb_entry_is_tracked(entry->type)) continue;
            if(entry->base + entry->length > start_addr) {
                bitmap[page_index / 8] |= (uint8_t) (1 << (page_index % 8));
                break;
            }
        }
    }

    // @note: count the present pages first so the bookkeeping list can be allocated in one go
    size_t present_count = 0;
    for(size_t page_index = 0; page_index < pagedb_page_count; page_index++) {
        if((bitmap[page_index / 8] & (1u << (page_index % 8))) != 0) present_count++;
    }

    struct list_node {
        struct list_node* next;
        size_t page_index;
        uintptr_t paddr;
    };

    struct list_node* head = nullptr;
    struct list_node* tail = nullptr;

    if(present_count != 0) {
        size_t list_size = MATH_ALIGN_UP(present_count * sizeof(struct list_node), PTM_PAGE_GRANULARITY);
        struct list_node* nodes = (struct list_node*) ((uintptr_t) pmm_alloc_ext(list_size / PTM_PAGE_GRANULARITY, PTM_PAGE_GRANULARITY, PMM_MAP_TYPE_USED) + g_globals_boot_info->hhdm_offset);

        size_t node_index = 0;
        for(size_t page_index = 0; page_index < pagedb_page_count; page_index++) {
            if((bitmap[page_index / 8] & (1u << (page_index % 8))) == 0) continue;

            uintptr_t paddr = (uintptr_t) pmm_alloc_ext(1, PTM_PAGE_GRANULARITY, PMM_MAP_TYPE_USED);
            memset((void*) (paddr + g_globals_boot_info->hhdm_offset), 0, PTM_PAGE_GRANULARITY);

            nodes[node_index] = (struct list_node) { .next = nullptr, .page_index = page_index, .paddr = paddr };
            if(tail == nullptr) {
                head = &nodes[node_index];
            } else {
                tail->next = &nodes[node_index];
            }
            tail = &nodes[node_index];
            node_index++;
        }
    }

    struct list_node* node = head;
    while(node != nullptr) {
        uintptr_t run_paddr = node->paddr;
        uintptr_t run_vaddr = pagedb_base + node->page_index * PTM_PAGE_GRANULARITY;
        size_t run_pages = 1;

        struct list_node* next = node->next;
        while(next != nullptr && next->page_index == node->page_index + run_pages && next->paddr == run_paddr + run_pages * PTM_PAGE_GRANULARITY) {
            run_pages++;
            next = next->next;
        }

        ptm_map(run_vaddr, (uint64_t) run_paddr, run_pages * PTM_PAGE_GRANULARITY, PTM_FLAG_READ | PTM_FLAG_WRITE);
        node = next;
    }


    log_print("pagedb pages: %zu/%zu present\n", present_count, pagedb_page_count);

    *pfndb_start = pagedb_base;
    *pfndb_size = pagedb_size;
    *pfndb_bitmap_start = bitmap_base;
    *pfndb_bitmap_size = bitmap_size;
}
