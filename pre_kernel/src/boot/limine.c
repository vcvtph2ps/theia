#include <boot/boot.h>
#include <boot/core.h>
#include <init.h>
#include <lib/math.h>
#include <limine.h>
#include <log.h>
#include <memory/pmm.h>
#include <memory/ptm.h>
#include <panic.h>
#include <protocol/bootinfo.h>
#include <runtime/mem.h>
#include <stddef.h>
#include <stdint.h>

[[noreturn]] static void limine_ap_entry(struct limine_mp_info* mp_info) {
    prekernel_init_ap((core_start_info_t*) mp_info->extra_argument);
}

#define LIMINE_REQUEST [[gnu::used, gnu::section(".limine_requests")]]

LIMINE_REQUEST volatile struct limine_bootloader_info_request g_limine_bootloader_info_request = { .id = LIMINE_BOOTLOADER_INFO_REQUEST_ID, .revision = 0 };
LIMINE_REQUEST volatile struct limine_framebuffer_request g_framebuffer_request = { .id = LIMINE_FRAMEBUFFER_REQUEST_ID, .revision = 0 };
LIMINE_REQUEST volatile struct limine_hhdm_request g_hhdm_request = { .id = LIMINE_HHDM_REQUEST_ID, .revision = 0 };
LIMINE_REQUEST volatile struct limine_memmap_request g_memmap_request = { .id = LIMINE_MEMMAP_REQUEST_ID, .revision = 0 };

#ifdef __ARCH_X86_64__
LIMINE_REQUEST volatile struct limine_mp_request g_mp_request = { .id = LIMINE_MP_REQUEST_ID, .revision = 0, .response = nullptr, .flags = LIMINE_MP_REQUEST_X86_64_X2APIC };
#else
LIMINE_REQUEST volatile struct limine_mp_request g_mp_request = { .id = LIMINE_MP_REQUEST_ID, .revision = 0, .response = nullptr, .flags = 0 };
#endif

LIMINE_REQUEST volatile struct limine_rsdp_request g_rsdp_request = {
    .id = LIMINE_RSDP_REQUEST_ID,
    .revision = 0,
};

#ifdef __ARCH_RISCV64__
LIMINE_REQUEST volatile struct limine_dtb_request g_dtb_request = {
    .id = LIMINE_DTB_REQUEST_ID,
    .revision = 0,
};
#endif

LIMINE_REQUEST volatile struct limine_date_at_boot_request g_boottime_request = {
    .id = LIMINE_DATE_AT_BOOT_REQUEST_ID,
    .revision = 0,
};

LIMINE_REQUEST volatile struct limine_internal_module g_initramfs = {
    .path = "initramfs.rdk",
    .string = "initramfs.rdk",
    .flags = LIMINE_INTERNAL_MODULE_REQUIRED,
};

LIMINE_REQUEST volatile struct limine_internal_module g_ksym = {
    .path = "kernel.ksym",
    .string = "kernel.ksym",
    .flags = 0,
};

LIMINE_REQUEST volatile struct limine_internal_module* g_modules[] = { &g_initramfs, &g_ksym };

LIMINE_REQUEST volatile struct limine_module_request g_module_request = { .id = LIMINE_MODULE_REQUEST_ID, .revision = 1, .internal_modules = (struct limine_internal_module**) &g_modules, .internal_module_count = 2 };

LIMINE_REQUEST volatile uint64_t g_limine_base_revision[] = LIMINE_BASE_REVISION(6);
[[gnu::used, gnu::section(".limine_requests_start")]] volatile uint64_t g_limine_requests_start_marker[] = LIMINE_REQUESTS_START_MARKER;
[[gnu::used, gnu::section(".limine_requests_end")]] volatile uint64_t g_limine_requests_end_marker[] = LIMINE_REQUESTS_END_MARKER;


static bool limine_core_is_bsp(uint64_t limine_core_index) {
#if defined(__ARCH_X86_64__)
    return (g_mp_request.response->cpus[limine_core_index]->lapic_id == g_mp_request.response->bsp_lapic_id);
#elif defined(__ARCH_RISCV64__)
    return (g_mp_request.response->cpus[limine_core_index]->hartid == g_mp_request.response->bsp_hartid);
#else
#error "Unknown architecture"
#endif
}

static void limine_start_ap(uint64_t limine_core_index, core_start_info_t* boot_info) {
    struct limine_mp_info* cpu = g_mp_request.response->cpus[limine_core_index];
    __atomic_store_n(&cpu->extra_argument, (uint64_t) boot_info, __ATOMIC_SEQ_CST);
    __atomic_store_n(&cpu->goto_address, limine_ap_entry, __ATOMIC_SEQ_CST);
}

#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wmissing-prototypes"
[[noreturn, gnu::used]] void prekernel_entry_limine() {
    if(LIMINE_LOADED_BASE_REVISION_VALID(g_limine_base_revision)) {
        log_print("Booted via limine protocol version %ld", LIMINE_LOADED_BASE_REVISION(g_limine_base_revision));
    } else {
        panic("Booted with invalid limine base revision");
    }
    if(!LIMINE_BASE_REVISION_SUPPORTED(g_limine_base_revision)) { panic("Booted with unsupported limine base revision"); }
    log_print_raw("\n");
    if(g_limine_bootloader_info_request.response) {
        log_print("bootloader info: name=\"%s\" version=\"%s\"\n", g_limine_bootloader_info_request.response->name, g_limine_bootloader_info_request.response->version);
    } else {
        log_print("Limine bootloader info request failed");
    }

    for(size_t i = 0; i < g_memmap_request.response->entry_count; i++) {
        struct limine_memmap_entry* mm_entry = g_memmap_request.response->entries[i];
        pmm_map_type_t type;
        switch(mm_entry->type) {
            case LIMINE_MEMMAP_USABLE:                 type = PMM_MAP_TYPE_FREE; break;
            case LIMINE_MEMMAP_RESERVED:               type = PMM_MAP_TYPE_RESERVED; break;
            case LIMINE_MEMMAP_FRAMEBUFFER:            type = PMM_MAP_TYPE_RESERVED; break;
            case LIMINE_MEMMAP_ACPI_RECLAIMABLE:       type = PMM_MAP_TYPE_ACPI_RECLAIMABLE; break;
            case LIMINE_MEMMAP_ACPI_NVS:               type = PMM_MAP_TYPE_ACPI_NVS; break;
            case LIMINE_MEMMAP_BAD_MEMORY:             type = PMM_MAP_TYPE_BAD; break;
            case LIMINE_MEMMAP_BOOTLOADER_RECLAIMABLE: type = PMM_MAP_TYPE_BOOTLOADER_RECLAIMABLE; break;
            case LIMINE_MEMMAP_EXECUTABLE_AND_MODULES: type = PMM_MAP_TYPE_BOOTLOADER_RECLAIMABLE; break;
            case LIMINE_MEMMAP_RESERVED_MAPPED:        type = PMM_MAP_TYPE_USED; break;
            default:                                   panic("Invalid memory map entry type");
        }
        pmm_map_add(mm_entry->base, mm_entry->length, type);
    }

    size_t boot_info_block_size = sizeof(bootinfo_t);
    if(g_framebuffer_request.response != nullptr) { boot_info_block_size += sizeof(bootinfo_framebuffer_t) * g_framebuffer_request.response->framebuffer_count; }
    boot_info_block_size += sizeof(bootinfo_module_t) * g_module_request.response->module_count;

    for(size_t i = 0; i < g_module_request.response->module_count; i++) { boot_info_block_size += strlen(g_module_request.response->modules[i]->path) + 1; }

    boot_info_block_size = MATH_ALIGN_UP(boot_info_block_size, PTM_PAGE_GRANULARITY);

    bootinfo_t* boot_info = (bootinfo_t*) ((uintptr_t) pmm_alloc(boot_info_block_size / PTM_PAGE_GRANULARITY) + g_hhdm_request.response->offset);
    boot_info->core_count = g_mp_request.response->cpu_count;
    boot_info->boot_timestamp = g_boottime_request.response->timestamp;
    boot_info->rdsp_physical = 0;
    if(g_rsdp_request.response && g_rsdp_request.response->address) {
        log_print("acpi: supported\n");
        boot_info->rdsp_physical = (uintptr_t) g_rsdp_request.response->address - g_hhdm_request.response->offset;
    }
    boot_info->hhdm_offset = g_hhdm_request.response->offset;

#ifdef __ARCH_RISCV64__
    boot_info->dtb_physical = 0;
    boot_info->riscv_base_isa_string = nullptr;
    boot_info->riscv_extension_count = 0;
    boot_info->riscv_extentions = nullptr;

    if(g_dtb_request.response && g_dtb_request.response->dtb_ptr) {
        log_print("dtb: supported\n");
        boot_info->dtb_physical = (uintptr_t) g_dtb_request.response->dtb_ptr - g_hhdm_request.response->offset;
    }
#endif

    uintptr_t boot_info_block_pointer = (uintptr_t) boot_info + sizeof(bootinfo_t);

    if(g_framebuffer_request.response != nullptr) {
        boot_info->framebuffer_count = g_framebuffer_request.response->framebuffer_count;
        boot_info->framebuffers = (bootinfo_framebuffer_t*) boot_info_block_pointer;
        for(size_t i = 0; i < g_framebuffer_request.response->framebuffer_count; i++) {
            bootinfo_framebuffer_t* fb = (bootinfo_framebuffer_t*) boot_info_block_pointer;
            struct limine_framebuffer* limine_fb = g_framebuffer_request.response->framebuffers[i];

            fb->vaddr = limine_fb->address;
            fb->paddr = ((uintptr_t) limine_fb->address) - g_hhdm_request.response->offset;
            fb->size = limine_fb->pitch * limine_fb->height;

            fb->width = limine_fb->width;
            fb->height = limine_fb->height;
            fb->pitch = limine_fb->pitch;
            fb->bpp = limine_fb->bpp;

            fb->red_position = limine_fb->red_mask_shift;
            fb->red_size = limine_fb->red_mask_size;
            fb->green_position = limine_fb->green_mask_shift;
            fb->green_size = limine_fb->green_mask_size;
            fb->blue_position = limine_fb->blue_mask_shift;
            fb->blue_size = limine_fb->blue_mask_size;

            boot_info_block_pointer += sizeof(bootinfo_framebuffer_t);
        }
    } else {
        boot_info->framebuffer_count = 0;
        boot_info->framebuffers = nullptr;
    }

    boot_info->module_count = g_module_request.response->module_count;
    boot_info->modules = (bootinfo_module_t*) boot_info_block_pointer;
    uintptr_t module_name_block = boot_info_block_pointer + sizeof(bootinfo_module_t) * g_module_request.response->module_count;

    for(size_t i = 0; i < g_module_request.response->module_count; i++) {
        bootinfo_module_t* module = (bootinfo_module_t*) boot_info_block_pointer;
        struct limine_file* limine_module = g_module_request.response->modules[i];

        module->name = (char*) module_name_block;
        module->phys_addr = (uintptr_t) pmm_alloc(MATH_ALIGN_UP(limine_module->size, PTM_PAGE_GRANULARITY) / PTM_PAGE_GRANULARITY);
        module->size = limine_module->size;

        memcpy((void*) module->name, limine_module->path, strlen(limine_module->path) + 1);
        memcpy((void*) (module->phys_addr + g_hhdm_request.response->offset), (void*) limine_module->address, limine_module->size);

        boot_info_block_pointer += sizeof(bootinfo_module_t);
        module_name_block += strlen(limine_module->path) + 1;
    }

    g_boot_core_is_bsp = limine_core_is_bsp;
    g_boot_start_ap = limine_start_ap;

    prekernel_init(boot_info);
    while(1);
}
#pragma clang diagnostic pop
