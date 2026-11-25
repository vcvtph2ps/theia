#include <boot/bootinfo.h>
#include <lib/math.h>
#include <memory/pmm.h>
#include <memory/ptm.h>
#include <runtime/mem.h>
#include <stddef.h>
#include <stdint.h>

size_t bootinfo_block_size(size_t framebuffer_count, size_t module_count, size_t module_name_bytes, size_t command_line_bytes) {
    size_t size = sizeof(bootinfo_t);
    size += framebuffer_count * sizeof(bootinfo_framebuffer_t);
    size += command_line_bytes;
    size += module_count * sizeof(bootinfo_module_t);
    size += module_name_bytes;
    return MATH_ALIGN_UP(size, PTM_PAGE_GRANULARITY);
}

void boot_framebuffer_from_mask(uint32_t mask, uint8_t* position, uint8_t* size) {
    if(mask == 0) {
        *position = 0;
        *size = 0;
        return;
    }

    *position = (uint8_t) __builtin_ctz(mask);
    *size = (uint8_t) __builtin_popcount(mask);
}

void bootinfo_init(bootinfo_t* info, uint64_t boot_timestamp, uintptr_t hhdm_offset, uintptr_t rsdp_physical, uintptr_t dtb_physical) {
    memset(info, 0, sizeof(*info));
    info->boot_timestamp = boot_timestamp;
    info->rdsp_physical = rsdp_physical;
    info->dtb_physical = dtb_physical;
    info->hhdm_offset = hhdm_offset;
    info->core_count = 1;
    info->serial = (bootinfo_serial_t) { .type = BOOTINFO_SERIAL_TYPE_NONE };
}

void bootinfo_builder_init(bootinfo_builder_t* builder, uintptr_t physical, uintptr_t hhdm_offset, uintptr_t access_offset, size_t framebuffer_count, size_t module_count, size_t command_line_bytes) {
    uintptr_t base = physical + access_offset;
    builder->info = (bootinfo_t*) base;
    builder->pointer_delta = hhdm_offset - access_offset;

    uintptr_t cursor = base + sizeof(bootinfo_t);
    builder->framebuffer_cursor = cursor;
    cursor += framebuffer_count * sizeof(bootinfo_framebuffer_t);
    builder->command_line_cursor = cursor;
    cursor += command_line_bytes;
    builder->module_cursor = cursor;
    cursor += module_count * sizeof(bootinfo_module_t);
    builder->module_name_cursor = cursor;
}

void bootinfo_builder_add_framebuffer(bootinfo_builder_t* builder, const boot_framebuffer_t* framebuffer) {
    bootinfo_framebuffer_t* written = (bootinfo_framebuffer_t*) builder->framebuffer_cursor;

    *written = (bootinfo_framebuffer_t) {
        .vaddr = framebuffer->vaddr,
        .paddr = framebuffer->paddr,
        .size = framebuffer->size,
        .width = framebuffer->width,
        .height = framebuffer->height,
        .pitch = framebuffer->pitch,
        .bpp = framebuffer->bpp,
        .red_position = framebuffer->red_position,
        .red_size = framebuffer->red_size,
        .green_position = framebuffer->green_position,
        .green_size = framebuffer->green_size,
        .blue_position = framebuffer->blue_position,
        .blue_size = framebuffer->blue_size,
    };

    builder->framebuffer_cursor += sizeof(bootinfo_framebuffer_t);
}

uintptr_t bootinfo_builder_add_module(bootinfo_builder_t* builder, const boot_module_t* module) {
    size_t name_size = strlen(module->name) + 1;
    size_t pages = MATH_ALIGN_UP(module->size, PTM_PAGE_GRANULARITY) / PTM_PAGE_GRANULARITY;
    uintptr_t physical = (uintptr_t) pmm_alloc_ext(pages, PTM_PAGE_GRANULARITY, PMM_MAP_TYPE_MODULE);

    bootinfo_module_t* written = (bootinfo_module_t*) builder->module_cursor;
    *written = (bootinfo_module_t) {
        .name = (char*) bootinfo_builder_store(builder, builder->module_name_cursor),
        .phys_addr = physical,
        .size = module->size,
    };
    memcpy((void*) builder->module_name_cursor, module->name, name_size);
    memcpy((void*) (physical + module->destination_offset), module->data, module->size);

    builder->module_cursor += sizeof(bootinfo_module_t);
    builder->module_name_cursor += name_size;
    return physical;
}

void bootinfo_builder_set_command_line(bootinfo_builder_t* builder, const char* data, size_t size) {
    if(data == nullptr) {
        builder->info->command_line = nullptr;
        return;
    }

    builder->info->command_line = (char*) bootinfo_builder_store(builder, builder->command_line_cursor);
    memcpy((void*) builder->command_line_cursor, data, size);
    ((char*) builder->command_line_cursor)[size] = '\0';
}
