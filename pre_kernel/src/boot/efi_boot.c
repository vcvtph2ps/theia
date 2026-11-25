#include <arch.h>
#include <boot/bootinfo.h>
#include <boot/efi.h>
#include <efi.h>
#include <init.h>
#include <lib/math.h>
#include <log.h>
#include <memory/pmm.h>
#include <memory/ptm.h>
#include <panic.h>
#include <runtime/mem.h>
#include <stddef.h>
#include <stdint.h>

#define EFI_HHDM_OFFSET ((uintptr_t) MEMORY_KERNELSPACE_START)

typedef struct {
    const CHAR16* efi_path;
    const char* boot_name;
} efi_module_def_t;

static const efi_module_def_t g_module_defs[] = {
    { u"\\boot\\initramfs.rdk.zst", "/boot/initramfs.rdk.zst" },
    { u"\\boot\\kernel.ksym",       "/boot/kernel.ksym"       },
};
#define EFI_MODULE_COUNT (sizeof(g_module_defs) / sizeof(g_module_defs[0]))

static boot_module_t g_modules[EFI_MODULE_COUNT];

static char* g_cmdline_data = nullptr;
static size_t g_cmdline_size = 0;

static bool g_has_framebuffer = false;
static boot_framebuffer_t g_framebuffer;

static pmm_map_type_t efi_type_to_pmm(uint32_t type) {
    switch(type) {
        case EfiConventionalMemory: return PMM_MAP_TYPE_FREE;
        case EfiLoaderCode:         [[fallthrough]];
        case EfiLoaderData:         [[fallthrough]];
        case EfiBootServicesCode:   [[fallthrough]];
        case EfiBootServicesData:   return PMM_MAP_TYPE_BOOTLOADER_RECLAIMABLE;
        case EfiACPIReclaimMemory:  return PMM_MAP_TYPE_ACPI_RECLAIMABLE;
        case EfiACPIMemoryNVS:      return PMM_MAP_TYPE_ACPI_NVS;
        case EfiUnusableMemory:     return PMM_MAP_TYPE_BAD;
        default:                    return PMM_MAP_TYPE_RESERVED;
    }
}

static void build_memory_map(const efi_memmap_t* map) {
    for(size_t i = 0; i < map->entry_count; i++) {
        EFI_MEMORY_DESCRIPTOR* descriptor = (EFI_MEMORY_DESCRIPTOR*) ((uintptr_t) map->descriptors + i * map->descriptor_size);
        uint64_t base = descriptor->PhysicalStart;
        uint64_t length = descriptor->NumberOfPages * PTM_PAGE_GRANULARITY;
        if(length == 0) { continue; }

        // @note: Firmware reports the low memory as usable (including 0!!), all lunar needs atleast 1 page below 1MiB
        const uint64_t low_limit = 0x1000;
        if(base < low_limit) {
            uint64_t delta = low_limit - base;
            if(delta >= length) { continue; }
            base += delta;
            length -= delta;
        }

        pmm_map_add(base, length, efi_type_to_pmm(descriptor->Type));
    }
}

static void load_modules(void) {
    for(size_t i = 0; i < EFI_MODULE_COUNT; i++) {
        void* data = nullptr;
        size_t size = 0;
        (void) efi_read_file(g_module_defs[i].efi_path, &data, &size);

        g_modules[i] = (boot_module_t) {
            .name = g_module_defs[i].boot_name,
            .data = data,
            .size = size,
            .destination_offset = 0,
        };
    }
}

static void release_loaded_files(void) {
    for(size_t i = 0; i < EFI_MODULE_COUNT; i++) {
        efi_free_pool((void*) g_modules[i].data);
        g_modules[i].data = nullptr;
    }
    if(g_cmdline_data != nullptr) {
        efi_free_pool(g_cmdline_data);
        g_cmdline_data = nullptr;
    }
}

static void read_cmdline(void) {
    void* data = nullptr;
    size_t size = 0;
    if(!efi_try_read_file(u"\\boot\\cmdline.txt", &data, &size)) {
        log_print("cmdline: could not find \\boot\\cmdline.txt\n");
        return;
    }

    // Trim trailing whitespace/newlines
    while(size > 0 && (data != nullptr) && (((char*) data)[size - 1] == '\n' || ((char*) data)[size - 1] == '\r' || ((char*) data)[size - 1] == ' ' || ((char*) data)[size - 1] == '\t')) { size--; }

    g_cmdline_data = (char*) data;
    g_cmdline_size = size;
    log_print("cmdline: %zu byte(s)\n", g_cmdline_size);
}

static void read_framebuffer(void) {
    g_has_framebuffer = efi_gop_get_framebuffer(&g_framebuffer);
}

/// @brief build page tables with identity and higher half direct map
static void create_boot_mappings(void) {
    uint64_t max_phys = 0;
    PMM_MAP_FOREACH(node) {
        if(node->entry.type == PMM_MAP_TYPE_RESERVED || node->entry.type == PMM_MAP_TYPE_BAD) { continue; }
        max_phys = MATH_MAX(max_phys, node->entry.base + node->entry.length);
    }
    if(max_phys == 0) { return; }
    max_phys = MATH_ALIGN_UP(max_phys, PTM_PAGE_GRANULARITY);

    log_print("creating boot mappings for 0x0-0x%lx\n", max_phys);

    ptm_map(0, 0, max_phys, PTM_FLAG_READ | PTM_FLAG_WRITE | PTM_FLAG_EXEC);
    ptm_map(EFI_HHDM_OFFSET, 0, max_phys, PTM_FLAG_READ | PTM_FLAG_WRITE);
}

static bootinfo_t* build_bootinfo(void) {
    size_t module_name_bytes = 0;
    for(size_t i = 0; i < EFI_MODULE_COUNT; i++) { module_name_bytes += strlen(g_modules[i].name) + 1; }

    const size_t command_line_bytes = g_cmdline_size + 1;
    size_t block_size = bootinfo_block_size(g_has_framebuffer ? 1 : 0, EFI_MODULE_COUNT, module_name_bytes, command_line_bytes);
    uintptr_t physical = (uintptr_t) pmm_alloc(block_size / PTM_PAGE_GRANULARITY);

    // @note: we are still running on firmware page tables, so write through the identity mapping. we only store direct map addresses inside the block
    bootinfo_builder_t builder;
    bootinfo_builder_init(&builder, physical, EFI_HHDM_OFFSET, 0, g_has_framebuffer ? 1 : 0, EFI_MODULE_COUNT, command_line_bytes);
    bootinfo_init(builder.info, 0, EFI_HHDM_OFFSET, efi_find_acpi_rsdp(), efi_find_dtb());

    builder.info->framebuffer_count = g_has_framebuffer ? 1 : 0;
    builder.info->framebuffers = g_has_framebuffer ? (bootinfo_framebuffer_t*) bootinfo_builder_store(&builder, builder.framebuffer_cursor) : nullptr;
    if(g_has_framebuffer) { bootinfo_builder_add_framebuffer(&builder, &g_framebuffer); }

    builder.info->module_count = EFI_MODULE_COUNT;
    builder.info->modules = (bootinfo_module_t*) bootinfo_builder_store(&builder, builder.module_cursor);
    for(size_t i = 0; i < EFI_MODULE_COUNT; i++) {
        uintptr_t phys = bootinfo_builder_add_module(&builder, &g_modules[i]);
        log_print("module %s: phys=0x%lx size=0x%lx\n", g_modules[i].name, phys, g_modules[i].size);
    }

    bootinfo_builder_set_command_line(&builder, g_cmdline_data == nullptr ? "" : g_cmdline_data, g_cmdline_size);

    return builder.info;
}

[[noreturn]] void prekernel_efi_boot(void) {
    arch_init_early();
    log_print("Theia EFI\n");

#if defined(__riscv) && __riscv_xlen == 64
    arch_efi_discover_boot_hartid();
#endif

    // @note: we run on the firmware identity map here, so node slabs carved from the map are accessed by physical address
    pmm_init(0);

    efi_memmap_t memmap;
    efi_get_memory_map(&memmap);
    build_memory_map(&memmap);

    ptm_init_at_level(arch_efi_paging_level(), EFI_HHDM_OFFSET, 0);
    create_boot_mappings();

    load_modules();
    read_cmdline();
    read_framebuffer();

    bootinfo_t* boot_info = build_bootinfo();
    release_loaded_files();

    log_print("exiting boot services\n");

    efi_memmap_t final_map;
    EFI_STATUS status = efi_exit_boot_services(g_efi_image_handle, &final_map);
    if(EFI_ERROR(status)) { panic("failed to exit boot services (status=0x%lx)", (uintptr_t) status); }

    arch_efi_switch_page_tables(g_ptm.tplt);

    prekernel_init((bootinfo_t*) ((uintptr_t) boot_info + EFI_HHDM_OFFSET));
    __builtin_unreachable();
}
