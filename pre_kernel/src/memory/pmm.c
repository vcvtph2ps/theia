#include <globals.h>
#include <lib/math.h>
#include <log.h>
#include <memory/pmm.h>
#include <memory/ptm.h>
#include <panic.h>
#include <runtime/mem.h>

size_t g_pmm_map_size;
pmm_map_node_t* g_pmm_map_head;

#define PMM_STATIC_NODE_COUNT 16
static pmm_map_node_t g_pmm_static_nodes[PMM_STATIC_NODE_COUNT];
static pmm_map_node_t* g_pmm_free_nodes;
static bool g_pmm_free_nodes_initialized;

static uintptr_t g_pmm_node_hhdm_offset;

void pmm_init(uintptr_t hhdm_offset) {
    g_pmm_node_hhdm_offset = hhdm_offset;
}

static pmm_map_node_t* pmm_node_carve(void);

static pmm_map_node_t* pmm_node_alloc(void) {
    if(!g_pmm_free_nodes_initialized) {
        for(size_t i = 0; i < PMM_STATIC_NODE_COUNT; i++) {
            g_pmm_static_nodes[i].next = g_pmm_free_nodes;
            g_pmm_free_nodes = &g_pmm_static_nodes[i];
        }
        g_pmm_free_nodes_initialized = true;
    }

    if(g_pmm_free_nodes == nullptr) { return pmm_node_carve(); }

    pmm_map_node_t* node = g_pmm_free_nodes;
    g_pmm_free_nodes = node->next;
    return node;
}

static void pmm_node_free(pmm_map_node_t* node) {
    node->next = g_pmm_free_nodes;
    g_pmm_free_nodes = node;
}

static pmm_map_node_t* pmm_node_carve(void) {
    const size_t node_count = PTM_PAGE_GRANULARITY / sizeof(pmm_map_node_t);

    uint64_t slab_base = 0;
    bool found = false;
    PMM_MAP_FOREACH(node) {
        if(node->entry.type != PMM_MAP_TYPE_FREE) continue;
        uint64_t candidate = MATH_CEIL(node->entry.base, PTM_PAGE_GRANULARITY);
        if(candidate + PTM_PAGE_GRANULARITY <= node->entry.base + node->entry.length) {
            slab_base = candidate;
            found = true;
            break;
        }
    }
    if(!found) panic("out of memory for pmm map nodes");

    pmm_map_node_t* slab = (pmm_map_node_t*) (uintptr_t) (slab_base + g_pmm_node_hhdm_offset);
    for(size_t i = 0; i < node_count; i++) {
        slab[i].next = g_pmm_free_nodes;
        g_pmm_free_nodes = &slab[i];
    }

    pmm_map_set(slab_base, PTM_PAGE_GRANULARITY, PMM_MAP_TYPE_ALLOCATED, true);

    pmm_map_node_t* node = g_pmm_free_nodes;
    g_pmm_free_nodes = node->next;
    return node;
}

static void map_unlink(pmm_map_node_t** link) {
    pmm_map_node_t* node = *link;
    *link = node->next;
    pmm_node_free(node);
    g_pmm_map_size--;
}

static void map_insert(pmm_map_node_t** link, pmm_map_entry_t entry) {
    pmm_map_node_t* node = pmm_node_alloc();
    node->entry = entry;
    node->next = *link;
    *link = node;
    g_pmm_map_size++;
}

void pmm_map_set(uint64_t base, uint64_t length, pmm_map_type_t type, bool force) {
    if(length == 0) return;

    pmm_map_node_t** link = &g_pmm_map_head;
    while(*link != nullptr) {
        pmm_map_node_t* node = *link;
        pmm_map_entry_t* map_entry = &node->entry;

        bool override = type > map_entry->type || force;
        bool overlap_start = base > map_entry->base && base < map_entry->base + map_entry->length;
        bool overlap_end = base + length > map_entry->base && base + length < map_entry->base + map_entry->length;

        // Map entry falls over the entry
        if(overlap_start && overlap_end) {
            if(!override) return;

            uint64_t start_base = map_entry->base;
            uint64_t start_length = base - start_base;

            uint64_t end_base = base + length;
            uint64_t end_length = (map_entry->base + map_entry->length) - end_base;

            map_entry->base = start_base;
            map_entry->length = start_length;
            pmm_map_set(end_base, end_length, map_entry->type, true);
            link = &node->next;
            continue;
        }

        // Start of entry overlaps with map entry
        if(overlap_start) {
            uint64_t amount = (map_entry->base + map_entry->length) - base;
            if(override) {
                map_entry->length -= amount;
                if(map_entry->length == 0) {
                    map_unlink(link);
                    continue;
                }
            } else {
                length -= amount;
                base += amount;
            }
        }

        // End of entry overlaps with map entry
        if(overlap_end) {
            uint64_t amount = (base + length) - map_entry->base;
            if(override) {
                map_entry->length -= amount;
                map_entry->base += amount;
                if(map_entry->length == 0) {
                    map_unlink(link);
                    continue;
                }
            } else {
                length -= amount;
            }
        }

        // Entry falls over map entry
        if(base < map_entry->base && base + length > map_entry->base + map_entry->length) {
            if(override) {
                map_unlink(link);
                continue;
            }

            uint64_t start_base = base;
            uint64_t start_length = map_entry->base - start_base;

            uint64_t end_base = map_entry->base + map_entry->length;
            uint64_t end_length = (base + length) - end_base;

            pmm_map_set(start_base, start_length, type, force);
            pmm_map_set(end_base, end_length, type, force);
            return;
        }

        // Entry equals map entry
        if(base == map_entry->base && length == map_entry->length) {
            if(!override) return;
            map_unlink(link);
            continue;
        }

        link = &node->next;
    }
    if(length == 0) return;

    pmm_map_node_t* previous = nullptr;
    link = &g_pmm_map_head;
    while(*link != nullptr && (*link)->entry.base <= base) {
        previous = *link;
        link = &(*link)->next;
    }

    if(previous != nullptr && previous->entry.type == type && previous->entry.base + previous->entry.length == base) {
        previous->entry.length += length;
        return;
    }
    if(*link != nullptr && (*link)->entry.type == type && (*link)->entry.base == base + length) {
        (*link)->entry.base = base;
        (*link)->entry.length += length;
        return;
    }

    map_insert(link, (pmm_map_entry_t) { .base = base, .length = length, .type = type });
}

void pmm_map_add(uint64_t base, uint64_t length, pmm_map_type_t type) {
    if(length == 0) return;
    pmm_map_set(base, length, type, false);
}

void pmm_sanitize(void) {
    pmm_map_node_t* previous = nullptr;
    pmm_map_node_t** link = &g_pmm_map_head;
    while(*link != nullptr) {
        pmm_map_entry_t* map_entry = &(*link)->entry;

        uint64_t base = MATH_ALIGN_UP(map_entry->base, PTM_PAGE_GRANULARITY);
        uint64_t end = MATH_ALIGN_DOWN(map_entry->base + map_entry->length, PTM_PAGE_GRANULARITY);
        if(end <= base) {
            map_unlink(link);
            continue;
        }

        map_entry->base = base;
        map_entry->length = end - base;

        if(previous != nullptr && previous->entry.type == map_entry->type && previous->entry.base + previous->entry.length == map_entry->base) {
            previous->entry.length += map_entry->length;
            map_unlink(link);
            continue;
        }

        previous = *link;
        link = &(*link)->next;
    }
}

bootinfo_mm_type_t pmm_map_type_to_bootinfo(pmm_map_type_t type) {
    switch(type) {
        case PMM_MAP_TYPE_FREE:                   [[fallthrough]];
        case PMM_MAP_TYPE_BOOTLOADER_RECLAIMABLE: [[fallthrough]];
        case PMM_MAP_TYPE_EFI_RECLAIMABLE:        return BOOTINFO_MM_TYPE_USABLE;
        case PMM_MAP_TYPE_ALLOCATED:              return BOOTINFO_MM_TYPE_RECLAIMABLE;
        case PMM_MAP_TYPE_MODULE:                 return BOOTINFO_MM_TYPE_MODULE;
        case PMM_MAP_TYPE_USED:                   return BOOTINFO_MM_TYPE_USED;
        case PMM_MAP_TYPE_ACPI_RECLAIMABLE:       return BOOTINFO_MM_TYPE_ACPI_RECLAIMABLE;
        case PMM_MAP_TYPE_ACPI_NVS:               return BOOTINFO_MM_TYPE_ACPI_NVS;
        case PMM_MAP_TYPE_RESERVED:               return BOOTINFO_MM_TYPE_RESERVED;
        case PMM_MAP_TYPE_BAD:                    return BOOTINFO_MM_TYPE_BAD;
    }
    return BOOTINFO_MM_TYPE_RESERVED;
}

size_t pmm_map_write_bootinfo(bootinfo_mm_entry_t* entries, size_t capacity) {
    size_t count = 0;
    PMM_MAP_FOREACH(node) {
        bootinfo_mm_type_t type = pmm_map_type_to_bootinfo(node->entry.type);
        if(count != 0 && entries[count - 1].type == type && entries[count - 1].phys_base + entries[count - 1].length == node->entry.base) {
            entries[count - 1].length += node->entry.length;
            continue;
        }
        if(count == capacity) { panic("bootinfo memory map exceeds capacity"); }
        entries[count].type = type;
        entries[count].phys_base = node->entry.base;
        entries[count].length = node->entry.length;
        count++;
    }
    return count;
}

bool pmm_alloc_at(uint64_t address, size_t page_count, pmm_map_type_t type) {
    size_t length = page_count * PTM_PAGE_GRANULARITY;
    PMM_MAP_FOREACH(node) {
        pmm_map_entry_t* map_entry = &node->entry;
        if(map_entry->type != PMM_MAP_TYPE_FREE) continue;
        if(map_entry->base > address || map_entry->base + map_entry->length < address + length) continue;

        pmm_map_set(address, length, type, true);
        return true;
    }
    return false;
}

void* pmm_alloc_ext(size_t page_count, size_t alignment, pmm_map_type_t type) {
    size_t length = page_count * PTM_PAGE_GRANULARITY;
    PMM_MAP_FOREACH(node) {
        pmm_map_entry_t* map_entry = &node->entry;
        if(map_entry->type != PMM_MAP_TYPE_FREE) continue;
        uint64_t ue_base = MATH_CEIL(map_entry->base, alignment);
        if(ue_base >= map_entry->base + map_entry->length) continue;
        uint64_t ue_length = map_entry->length - (ue_base - map_entry->base);
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
    size_t capacity = g_pmm_map_size + 8;
    size_t bytes = MATH_ALIGN_UP(sizeof(pmm_map_entry_t) * capacity, PTM_PAGE_GRANULARITY);
    void* phys = pmm_alloc(bytes / PTM_PAGE_GRANULARITY);
    if(phys == nullptr) { panic("failed to allocate memory for pmm snapshot"); }
    if(g_pmm_map_size > capacity) { panic("pmm snapshot size exceeds allocated memory"); }

    size_t count = g_pmm_map_size;
    pmm_map_entry_t* entries = (pmm_map_entry_t*) ((uintptr_t) phys + g_globals_boot_info->hhdm_offset);
    size_t index = 0;
    PMM_MAP_FOREACH(node) {
        entries[index++] = node->entry;
    }
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
        case PMM_MAP_TYPE_FREE:                   return "free";
        case PMM_MAP_TYPE_ALLOCATED:              return "allocated";
        case PMM_MAP_TYPE_USED:                   return "used";
        case PMM_MAP_TYPE_BOOTLOADER_RECLAIMABLE: return "bootloader reclaimable";
        case PMM_MAP_TYPE_MODULE:                 return "module";
        case PMM_MAP_TYPE_EFI_RECLAIMABLE:        return "efi reclaimable";
        case PMM_MAP_TYPE_ACPI_RECLAIMABLE:       return "acpi reclaimable";
        case PMM_MAP_TYPE_ACPI_NVS:               return "acpi nvs";
        case PMM_MAP_TYPE_RESERVED:               return "reserved";
        case PMM_MAP_TYPE_BAD:                    return "bad";
        default:                                  return "??";
    }
}
