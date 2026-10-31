#include <arch.h>
#include <boot/efi.h>
#include <efi.h>
#include <globals.h>
#include <init.h>
#include <lib/math.h>
#include <log.h>
#include <memory/pmm.h>
#include <memory/ptm.h>
#include <panic.h>
#include <protocol/bootinfo.h>
#include <runtime/mem.h>
#include <stddef.h>
#include <stdint.h>

#define EFI_HHDM_OFFSET ((uintptr_t) MEMORY_KERNELSPACE_START)

static bootinfo_t g_stub_boot_info;

typedef struct {
    const CHAR16* efi_path;
    const char* boot_name;
} efi_module_def_t;

static const efi_module_def_t g_module_defs[] = {
    { L"\\boot\\initramfs.rdk.gz", "/boot/initramfs.rdk.gz" },
    { L"\\boot\\kernel.ksym",      "/boot/kernel.ksym"      },
};
#define EFI_MODULE_COUNT (sizeof(g_module_defs) / sizeof(g_module_defs[0]))

typedef struct {
    uintptr_t phys;
    size_t size;
    const char* name;
} efi_module_t;

static efi_module_t g_modules[EFI_MODULE_COUNT];

static char* g_cmdline_data = nullptr;
static size_t g_cmdline_size = 0;

static bool g_has_framebuffer = false;
static bootinfo_framebuffer_t g_framebuffer;

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

static bootinfo_mm_type_t pmm_type_to_bootinfo(pmm_map_type_t type) {
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

        size_t page_count = MATH_ALIGN_UP(size, PTM_PAGE_GRANULARITY) / PTM_PAGE_GRANULARITY;
        void* phys = pmm_alloc_ext(page_count, PTM_PAGE_GRANULARITY, PMM_MAP_TYPE_MODULE);
        memcpy((void*) ((uintptr_t) phys + g_stub_boot_info.hhdm_offset), data, size);
        efi_free_pool(data);

        g_modules[i].phys = (uintptr_t) phys;
        g_modules[i].size = size;
        g_modules[i].name = g_module_defs[i].boot_name;
        log_print("module %s: phys=0x%lx size=0x%lx\n", g_modules[i].name, g_modules[i].phys, g_modules[i].size);
    }
}

static void read_cmdline(void) {
    void* data = nullptr;
    size_t size = 0;
    if(!efi_try_read_file(L"\\boot\\cmdline.txt", &data, &size)) {
        log_print("cmdline: could not fine \\boot\\cmdline.txt\n");
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
    for(size_t i = 0; i < g_pmm_map_size; i++) {
        if(g_pmm_map[i].type == PMM_MAP_TYPE_RESERVED || g_pmm_map[i].type == PMM_MAP_TYPE_BAD) { continue; }
        max_phys = MATH_MAX(max_phys, g_pmm_map[i].base + g_pmm_map[i].length);
    }
    if(max_phys == 0) { return; }
    max_phys = MATH_ALIGN_UP(max_phys, PTM_PAGE_GRANULARITY);

    log_print("creating boot mappings for 0x0-0x%lx\n", max_phys);

    ptm_map(0, 0, max_phys, PTM_FLAG_READ | PTM_FLAG_WRITE | PTM_FLAG_EXEC);
    ptm_map(EFI_HHDM_OFFSET, 0, max_phys, PTM_FLAG_READ | PTM_FLAG_WRITE);
}

static uintptr_t find_config_table(const EFI_GUID* guid) {
    for(size_t i = 0; i < g_efi_system_table->NumberOfTableEntries; i++) {
        EFI_CONFIGURATION_TABLE* table = &g_efi_system_table->ConfigurationTable[i];
        if(memcmp(&table->VendorGuid, guid, sizeof(EFI_GUID)) == 0) { return (uintptr_t) table->VendorTable; }
    }
    return 0;
}

static uintptr_t find_acpi_rsdp(void) {
    EFI_GUID acpi20 = ACPI_20_TABLE_GUID;
    EFI_GUID acpi10 = ACPI_TABLE_GUID;
    uintptr_t rsdp = find_config_table(&acpi20);
    if(rsdp == 0) { rsdp = find_config_table(&acpi10); }
    return rsdp;
}

static uintptr_t find_dtb(void) {
    EFI_GUID dtb = EFI_DTB_TABLE_GUID;
    return find_config_table(&dtb);
}

static bootinfo_t* build_bootinfo(void) {
    size_t names_size = 0;
    for(size_t i = 0; i < EFI_MODULE_COUNT; i++) { names_size += strlen(g_modules[i].name) + 1; }

    size_t block_size = sizeof(bootinfo_t);
    block_size += g_pmm_map_size * sizeof(bootinfo_mm_entry_t);
    block_size += (g_has_framebuffer ? 1 : 0) * sizeof(bootinfo_framebuffer_t);
    block_size += EFI_MODULE_COUNT * sizeof(bootinfo_module_t);
    block_size += names_size;
    block_size += g_cmdline_size + 1;
    block_size = MATH_ALIGN_UP(block_size, PTM_PAGE_GRANULARITY);

    uintptr_t phys = (uintptr_t) pmm_alloc(block_size / PTM_PAGE_GRANULARITY);
    // @note: we are still running on firmware page tables, so write through the identity mapping. we only store direct map addresses inside the block
    uintptr_t virt = phys + EFI_HHDM_OFFSET;
    bootinfo_t* boot_info = (bootinfo_t*) phys;
    memset((void*) phys, 0, block_size);

    boot_info->core_count = 1;
    boot_info->cpus = nullptr;
    boot_info->boot_timestamp = 0;
    boot_info->hhdm_offset = EFI_HHDM_OFFSET;
    boot_info->rdsp_physical = find_acpi_rsdp();
    boot_info->dtb_physical = find_dtb();

    uintptr_t cursor_phys = phys + sizeof(bootinfo_t);
    uintptr_t cursor_virt = virt + sizeof(bootinfo_t);

    boot_info->mm_entry_count = g_pmm_map_size;
    boot_info->mm_entries = (bootinfo_mm_entry_t*) cursor_virt;
    bootinfo_mm_entry_t* mm_entries = (bootinfo_mm_entry_t*) cursor_phys;
    cursor_phys += g_pmm_map_size * sizeof(bootinfo_mm_entry_t);
    cursor_virt += g_pmm_map_size * sizeof(bootinfo_mm_entry_t);
    for(size_t i = 0; i < g_pmm_map_size; i++) {
        mm_entries[i].phys_base = g_pmm_map[i].base;
        mm_entries[i].length = g_pmm_map[i].length;
        mm_entries[i].type = pmm_type_to_bootinfo(g_pmm_map[i].type);
    }

    boot_info->framebuffer_count = g_has_framebuffer ? 1 : 0;
    boot_info->framebuffers = g_has_framebuffer ? (bootinfo_framebuffer_t*) cursor_virt : nullptr;
    if(g_has_framebuffer) {
        memcpy((void*) cursor_phys, &g_framebuffer, sizeof(bootinfo_framebuffer_t));
        cursor_phys += sizeof(bootinfo_framebuffer_t);
        cursor_virt += sizeof(bootinfo_framebuffer_t);
    }

    boot_info->module_count = EFI_MODULE_COUNT;
    boot_info->modules = (bootinfo_module_t*) cursor_virt;
    bootinfo_module_t* modules = (bootinfo_module_t*) cursor_phys;
    cursor_phys += EFI_MODULE_COUNT * sizeof(bootinfo_module_t);
    cursor_virt += EFI_MODULE_COUNT * sizeof(bootinfo_module_t);
    for(size_t i = 0; i < EFI_MODULE_COUNT; i++) {
        modules[i].phys_addr = g_modules[i].phys;
        modules[i].size = g_modules[i].size;
        modules[i].name = (char*) cursor_virt;

        size_t len = strlen(g_modules[i].name) + 1;
        memcpy((void*) cursor_phys, g_modules[i].name, len);
        cursor_phys += len;
        cursor_virt += len;
    }

    boot_info->command_line = (char*) cursor_virt;
    if(g_cmdline_size > 0) { memcpy((void*) cursor_phys, g_cmdline_data, g_cmdline_size); }
    ((char*) cursor_phys)[g_cmdline_size] = '\0';
    cursor_phys += g_cmdline_size + 1;
    cursor_virt += g_cmdline_size + 1;

    return (bootinfo_t*) phys;
}

[[noreturn]] void prekernel_efi_boot(void) {
    arch_init_early();
    log_print("Theia EFI\n");

#if defined(__riscv) && __riscv_xlen == 64
    arch_efi_discover_boot_hartid();
#endif

    g_stub_boot_info.hhdm_offset = 0;
    g_globals_boot_info = &g_stub_boot_info;

    efi_memmap_t memmap;
    efi_get_memory_map(&memmap);
    build_memory_map(&memmap);

    ptm_init_at_level(arch_efi_paging_level());
    create_boot_mappings();

    load_modules();
    read_cmdline();
    read_framebuffer();

    bootinfo_t* boot_info = build_bootinfo();

    log_print("exiting boot services\n");

    efi_memmap_t final_map;
    EFI_STATUS status = efi_exit_boot_services(g_efi_image_handle, &final_map);
    if(EFI_ERROR(status)) { panic("failed to exit boot services (status=0x%lx)", (uintptr_t) status); }

    arch_efi_switch_page_tables(g_ptm.tplt);

    prekernel_init((bootinfo_t*) ((uintptr_t) boot_info + EFI_HHDM_OFFSET));
    __builtin_unreachable();
}
