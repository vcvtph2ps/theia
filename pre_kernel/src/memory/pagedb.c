#include <globals.h>
#include <lib/math.h>
#include <log.h>
#include <memory/pagedb.h>
#include <memory/pmm.h>
#include <memory/ptm.h>
#include <panic.h>
#include <protocol/bootinfo.h>
#include <runtime/mem.h>

void pagedb_setup(uintptr_t kernel_base, size_t pagedb_entry_size, uintptr_t* pfndb_start, size_t* pfndb_size) {
    size_t system_page_count = MATH_ALIGN_UP(g_globals_boot_info->hhdm_size, PTM_PAGE_GRANULARITY) / PTM_PAGE_GRANULARITY;
    size_t pagedb_size = MATH_ALIGN_UP(system_page_count * pagedb_entry_size, PTM_PAGE_GRANULARITY);
    log_print("pagedb entry size: %zu\n", pagedb_entry_size);

    uintptr_t pagedb_base = g_globals_boot_info->hhdm_offset + g_globals_boot_info->hhdm_size + PTM_PAGE_GRANULARITY;
    uintptr_t pagedb_end = pagedb_base + pagedb_size;
    if(pagedb_end > kernel_base) { panic("pagedb overlaps with kernel image! pagedb_end=0x%016lx, kernel_base=0x%016lx", pagedb_end, kernel_base); }

    // @note: we really need to make a phyiscally spare array but for some reason, some mmio isn't in the memory map, meaning that would be unmapped in the pagedb and fault the kernel
    // @todo: figure out a way to avoid doing this
    for(uintptr_t vaddr = pagedb_base; vaddr < pagedb_end; vaddr += PTM_PAGE_GRANULARITY) { ptm_map(vaddr, (uint64_t) pmm_alloc_ext(1, PTM_PAGE_GRANULARITY, PMM_MAP_TYPE_USED), PTM_PAGE_GRANULARITY, PTM_FLAG_READ | PTM_FLAG_WRITE); }

    *pfndb_start = pagedb_base;
    *pfndb_size = pagedb_size;
}
