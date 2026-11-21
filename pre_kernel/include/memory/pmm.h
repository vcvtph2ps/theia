#pragma once
#include <protocol/bootinfo.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// Minorly inspired from https://github.com/elysium-os/tartarus-bootloader
typedef enum {
    PMM_MAP_TYPE_FREE,
    PMM_MAP_TYPE_ALLOCATED,

    PMM_MAP_TYPE_USED,

    PMM_MAP_TYPE_BOOTLOADER_RECLAIMABLE,
    PMM_MAP_TYPE_MODULE,
    PMM_MAP_TYPE_EFI_RECLAIMABLE,
    PMM_MAP_TYPE_ACPI_RECLAIMABLE,
    PMM_MAP_TYPE_ACPI_NVS,
    PMM_MAP_TYPE_RESERVED,
    PMM_MAP_TYPE_BAD,
} pmm_map_type_t;

typedef struct {
    uint64_t base;
    uint64_t length;
    pmm_map_type_t type;
} pmm_map_entry_t;

typedef struct pmm_map_node {
    struct pmm_map_node* next;
    pmm_map_entry_t entry;
} pmm_map_node_t;

extern pmm_map_node_t* g_pmm_map_head;
extern size_t g_pmm_map_size;

#define PMM_MAP_FOREACH(NODE) for(pmm_map_node_t* NODE = g_pmm_map_head; (NODE) != nullptr; (NODE) = (NODE)->next)

const char* pmm_entry_type_to_string(pmm_map_type_t type);

void pmm_init(uintptr_t hhdm_offset);
void pmm_sanitize(void);

bootinfo_mm_type_t pmm_map_type_to_bootinfo(pmm_map_type_t type);
size_t pmm_map_write_bootinfo(bootinfo_mm_entry_t* entries, size_t capacity);

void pmm_map_add(uint64_t base, uint64_t length, pmm_map_type_t type);
void pmm_map_set(uint64_t base, uint64_t length, pmm_map_type_t type, bool force);

[[nodiscard]] bool pmm_alloc_at(uint64_t address, size_t page_count, pmm_map_type_t type);
[[nodiscard]] void* pmm_alloc_ext(size_t page_count, size_t alignment, pmm_map_type_t type);
[[nodiscard]] void* pmm_alloc(size_t page_count);
void pmm_free(void* address, size_t count);

typedef struct {
    pmm_map_entry_t* entries;
    size_t count;
    struct {
        void* phys;
        size_t bytes;
    } allocation;
} pmm_map_snapshot_t;

[[nodiscard]] pmm_map_snapshot_t pmm_create_snapshot();
void pmm_free_snapshot(pmm_map_snapshot_t* map);
