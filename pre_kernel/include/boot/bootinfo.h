#pragma once
#include <protocol/bootinfo.h>
#include <stddef.h>
#include <stdint.h>

typedef struct {
    void* vaddr;
    uintptr_t paddr;
    size_t size;

    uint32_t width;
    uint32_t height;
    uint32_t pitch;
    uint32_t bpp;

    uint8_t red_position;
    uint8_t red_size;
    uint8_t green_position;
    uint8_t green_size;
    uint8_t blue_position;
    uint8_t blue_size;
} boot_framebuffer_t;

typedef struct {
    const char* name;
    const void* data;
    size_t size;
    uintptr_t destination_offset;
} boot_module_t;

typedef struct {
    bootinfo_t* info;
    uintptr_t pointer_delta;
    uintptr_t framebuffer_cursor;
    uintptr_t command_line_cursor;
    uintptr_t module_cursor;
    uintptr_t module_name_cursor;
} bootinfo_builder_t;

/// @brief Size of the boot info block, aligned to a page.
/// @param command_line_bytes Bytes to reserve for the command line including its terminator, 0 if absent
size_t bootinfo_block_size(size_t framebuffer_count, size_t module_count, size_t module_name_bytes, size_t command_line_bytes);

/// @brief Convert an rgb bit mask into a position and size
void boot_framebuffer_from_mask(uint32_t mask, uint8_t* position, uint8_t* size);

/// @brief Initialise fields shared by every boot path
void bootinfo_init(bootinfo_t* info, uint64_t boot_timestamp, uintptr_t hhdm_offset, uintptr_t rsdp_physical, uintptr_t dtb_physical);

/// @brief Convert a writable address into the address that should be stored in the block
static inline uintptr_t bootinfo_builder_store(const bootinfo_builder_t* builder, uintptr_t write_address) {
    return write_address + builder->pointer_delta;
}

/// @brief Start a boot info builder at an already allocated physical block.
/// @param access_offset Offset used to write to the block
void bootinfo_builder_init(bootinfo_builder_t* builder, uintptr_t physical, uintptr_t hhdm_offset, uintptr_t access_offset, size_t framebuffer_count, size_t module_count, size_t command_line_bytes);

/// @brief Append a framebuffer to the boot info block
void bootinfo_builder_add_framebuffer(bootinfo_builder_t* builder, const boot_framebuffer_t* framebuffer);

/// @brief Copy a module into newly allocated pages and append it to the boot info block
/// @return Physical address of the allocated module pages
uintptr_t bootinfo_builder_add_module(bootinfo_builder_t* builder, const boot_module_t* module);

/// @brief Append a command line, or leave it null when data is null
/// @param size Number of command line bytes, excluding the terminator
void bootinfo_builder_set_command_line(bootinfo_builder_t* builder, const char* data, size_t size);
