#include <globals.h>
#include <lib/math.h>
#include <log.h>
#include <memory/pmm.h>
#include <memory/ptm.h>
#include <panic.h>
#include <runtime/mem.h>

size_t g_pmm_map_size;
pmm_map_entry_t g_pmm_map[PMM_MAP_MAX_ENTRIES];

static void map_insert(int index, pmm_map_entry_t entry) {
    if(g_pmm_map_size == PMM_MAP_MAX_ENTRIES) panic("memory map overflow");
    for(int i = g_pmm_map_size; i > index; i--) g_pmm_map[i] = g_pmm_map[i - 1];
    g_pmm_map[index] = entry;
    g_pmm_map_size++;
}

static void map_delete(size_t index) {
    for(size_t i = index; i < g_pmm_map_size - 1; i++) g_pmm_map[i] = g_pmm_map[i + 1];
    g_pmm_map_size--;
}

void pmm_map_set(uint64_t base, uint64_t length, pmm_map_type_t type, bool force) {
    for(size_t i = 0; i < g_pmm_map_size; i++) {
        pmm_map_entry_t* map_entry = &g_pmm_map[i];

        bool override = type > map_entry->type || force;
        bool overlap_start = false, overlap_end = false;
        if(base > map_entry->base && base < map_entry->base + map_entry->length) overlap_start = true;
        if(base + length > map_entry->base && base + length < map_entry->base + map_entry->length) overlap_end = true;

        // Map entry falls over the entry
        if(overlap_start && overlap_end) {
            if(!override) return;

            size_t start_base = map_entry->base;
            size_t start_length = base - start_base;

            size_t end_base = base + length;
            size_t end_length = (map_entry->base + map_entry->length) - end_base;

            map_entry->base = start_base;
            map_entry->length = start_length;
            pmm_map_set(end_base, end_length, map_entry->type, true);
            continue;
        }

        // Start of entry overlaps with map entry
        if(overlap_start) {
            size_t amount = (map_entry->base + map_entry->length) - base;
            if(override) {
                map_entry->length -= amount;
                if(map_entry->length == 0) {
                    map_delete(i);
                    --i;
                }
            } else {
                length -= amount;
                base += amount;
            }
        }

        // End of entry overlaps with map entry
        if(overlap_end) {
            size_t amount = (base + length) - map_entry->base;
            if(override) {
                map_entry->length -= amount;
                map_entry->base += amount;
                if(map_entry->length == 0) {
                    map_delete(i);
                    --i;
                }
            } else {
                length -= amount;
            }
        }

        // Entry falls over map entry
        if(base < map_entry->base && base + length > map_entry->base + map_entry->length) {
            if(override) {
                map_delete(i);
                --i;
                continue;
            }

            size_t start_base = base;
            size_t start_length = map_entry->base - start_base;

            size_t end_base = map_entry->base + map_entry->length;
            size_t end_length = (base + length) - end_base;

            pmm_map_set(start_base, start_length, type, force);
            pmm_map_set(end_base, end_length, type, force);
            return;
        }

        // Entry equals map entry
        if(base == map_entry->base && length == map_entry->length) {
            if(!override) return;
            map_delete(i);
            --i;
        }
    }
    if(length == 0) return;

    size_t i = 0;
    for(; i < g_pmm_map_size && g_pmm_map[i].base <= base; i++);
    if(i != 0 && g_pmm_map[i - 1].type == type && g_pmm_map[i - 1].base + g_pmm_map[i - 1].length == base) {
        g_pmm_map[i - 1].length += length;
        return;
    }
    if(g_pmm_map[i].type == type && g_pmm_map[i].base == base + length) {
        g_pmm_map[i].base = base;
        g_pmm_map[i].length += length;
        return;
    }

    map_insert(i, (pmm_map_entry_t) { .base = base, .length = length, .type = type });
}

void pmm_map_add(uint64_t base, uint64_t length, pmm_map_type_t type) {
    if(length == 0) return;
    pmm_map_set(base, length, type, false);
}

bool pmm_alloc_at(uint64_t address, size_t page_count, pmm_map_type_t type) {
    size_t length = page_count * PTM_PAGE_GRANULARITY;
    for(size_t i = 0; i < g_pmm_map_size; i++) {
        if(g_pmm_map[i].type != PMM_MAP_TYPE_FREE) continue;
        if(g_pmm_map[i].base > address || g_pmm_map[i].base + g_pmm_map[i].length < address + length) continue;

        pmm_map_set(address, length, type, true);
        return true;
    }
    return false;
}

void* pmm_alloc_ext(size_t page_count, size_t alignment, pmm_map_type_t type) {
    size_t length = page_count * PTM_PAGE_GRANULARITY;
    for(size_t i = 0; i < g_pmm_map_size; i++) {
        if(g_pmm_map[i].type != PMM_MAP_TYPE_FREE) continue;
        uint64_t ue_base = MATH_CEIL(g_pmm_map[i].base, alignment);
        if(ue_base >= g_pmm_map[i].base + g_pmm_map[i].length) continue;
        uint64_t ue_length = g_pmm_map[i].length - (ue_base - g_pmm_map[i].base);
        if(ue_length < length) continue; // claim does not fit inside entry
        pmm_map_set(ue_base, length, type, true);
        return (void*) (uintptr_t) ue_base;
    }
    panic("out of memory");
    __builtin_unreachable();
}

void* pmm_alloc(size_t page_count) {
    return pmm_alloc_ext(page_count, PTM_PAGE_GRANULARITY, PMM_MAP_TYPE_ALLOCATED);
}

void pmm_free(void* address, size_t page_count) {
    pmm_map_set((uint64_t) (uintptr_t) address, page_count * PTM_PAGE_GRANULARITY, PMM_MAP_TYPE_FREE, true);
}


[[nodiscard]] pmm_map_snapshot_t pmm_create_snapshot() {
    size_t count = g_pmm_map_size + 2;
    size_t bytes = MATH_ALIGN_UP(sizeof(pmm_map_entry_t) * count, PTM_PAGE_GRANULARITY);
    void* phys = pmm_alloc(bytes / PTM_PAGE_GRANULARITY);
    if(phys == NULL) { panic("failed to allocate memory for pmm snapshot"); }
    if(MATH_ALIGN_UP(sizeof(pmm_map_entry_t) * g_pmm_map_size, PTM_PAGE_GRANULARITY) > bytes) { panic("pmm snapshot size exceeds allocated memory"); }

    pmm_map_entry_t* entries = (pmm_map_entry_t*) ((uintptr_t) phys + g_globals_boot_info->hhdm_offset);
    memcpy(entries, &g_pmm_map, sizeof(pmm_map_entry_t) * count);
    return (pmm_map_snapshot_t) {
        .entries = entries,
        .count = count,
        .allocation = { .phys = phys, .bytes = bytes }
    };
}

void pmm_free_snapshot(pmm_map_snapshot_t* map) {
    pmm_free(map->allocation.phys, map->allocation.bytes / PTM_PAGE_GRANULARITY);
}


const char* pmm_entry_type_to_string(pmm_map_type_t type) {
    switch(type) {
        case PMM_MAP_TYPE_FREE: return "free";
        case PMM_MAP_TYPE_ALLOCATED: return "allocated";
        case PMM_MAP_TYPE_USED: return "used";
        case PMM_MAP_TYPE_BOOTLOADER_RECLAIMABLE: return "bootloader reclaimable";
        case PMM_MAP_TYPE_MODULE: return "module";
        case PMM_MAP_TYPE_EFI_RECLAIMABLE: return "efi reclaimable";
        case PMM_MAP_TYPE_ACPI_RECLAIMABLE: return "acpi reclaimable";
        case PMM_MAP_TYPE_ACPI_NVS: return "acpi nvs";
        case PMM_MAP_TYPE_RESERVED: return "reserved";
        case PMM_MAP_TYPE_BAD: return "bad";
        default: return "??";
    }
}
