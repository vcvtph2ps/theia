#include <arch.h>
#include <arch/16550uart.h>
#include <arch/cr.h>
#include <arch/gdt.h>
#include <arch/io.h>
#include <boot/boot.h>
#include <globals.h>
#include <lib/helpers.h>
#include <lib/math.h>
#include <log.h>
#include <memory/pmm.h>
#include <memory/ptm.h>
#include <runtime/mem.h>

#define CORE_STACK_PGCNT 16

typedef void (*x86_64_kernel_entry_t)(bootinfo_kernel_entry_point_t entry, uintptr_t stack, uintptr_t page_tables, bootinfo_t* boot_info, uint64_t core_id);

extern uint8_t x86_64_kernel_handoff[]; // NOLINT
extern uint8_t x86_64_kernel_handoff_end[]; // NOLINT

static x86_64_kernel_entry_t g_boot_trampoline = nullptr;

void arch_spin_hint() {
    __builtin_ia32_pause();
}

void arch_init_early() {
    arch_16550uart_early_setup();
}

void arch_debug_putc(char c) {
    arch_io_port_write_u8(0xe9, (uint8_t) c);
    if(g_arch_16550uart_works) { arch_16550uart_send(c); }
}

void arch_prepare_handoff() {
    size_t size = MATH_ALIGN_UP((uintptr_t) (x86_64_kernel_handoff_end - x86_64_kernel_handoff), PTM_PAGE_GRANULARITY) / PTM_PAGE_GRANULARITY;
    log_print("boot trampoline size: %zu page(s)\n", size);

    void* boot_trampoline_alloc = pmm_alloc(size);
    memcpy((void*) ((uintptr_t) boot_trampoline_alloc + g_globals_boot_info->hhdm_offset), (void*) x86_64_kernel_handoff, (uintptr_t) (x86_64_kernel_handoff_end - x86_64_kernel_handoff));

    // @todo: this fucking sucks...
    uintptr_t current_cr3 = arch_cr_read_cr3();
    uintptr_t level_count = (arch_cr_read_cr4() & (1u << 12)) ? 5u : 4u;

    ptm_map_at(current_cr3, level_count, MEMORY_KERNELSPACE_END, (uintptr_t) boot_trampoline_alloc, size * PTM_PAGE_GRANULARITY, PTM_FLAG_READ | PTM_FLAG_EXEC);
    ptm_map(MEMORY_KERNELSPACE_END, (uintptr_t) boot_trampoline_alloc, size * PTM_PAGE_GRANULARITY, PTM_FLAG_READ | PTM_FLAG_EXEC);

    g_boot_trampoline = (x86_64_kernel_entry_t) MEMORY_KERNELSPACE_END;
}

__attribute__((no_sanitize("undefined"))) [[noreturn]] void arch_handoff_to_kernel(bootinfo_kernel_entry_point_t entry, uintptr_t stack, bootinfo_t* boot_info, uint64_t core_id) {
    g_boot_trampoline(entry, stack, g_ptm.tplt, boot_info, core_id);
    __builtin_unreachable();
}

void arch_setup_cpus(core_start_info_t* core_start_info_block, size_t core_count, bootinfo_kernel_info_t* kernel_info) {
    size_t cpu_local_block_size = MATH_ALIGN_UP(core_count * kernel_info->cpu_local_size, PTM_PAGE_GRANULARITY);
    void* cpu_local_block = (void*) ((uintptr_t) pmm_alloc_ext(cpu_local_block_size / PTM_PAGE_GRANULARITY, PTM_PAGE_GRANULARITY, PMM_MAP_TYPE_USED) + g_globals_boot_info->hhdm_offset);
    memset(cpu_local_block, 0, cpu_local_block_size);

    g_globals_boot_info->cpulocal_start = (uintptr_t) cpu_local_block;
    g_globals_boot_info->cpulocal_size = cpu_local_block_size;

    size_t arch_start_info_block_size = MATH_ALIGN_UP(sizeof(arch_core_start_info_t) * core_count, PTM_PAGE_GRANULARITY);
    arch_core_start_info_t* arch_start_info_block = (arch_core_start_info_t*) ((uintptr_t) pmm_alloc(arch_start_info_block_size / PTM_PAGE_GRANULARITY) + g_globals_boot_info->hhdm_offset);
    memset(core_start_info_block, 0, arch_start_info_block_size);

    // @note: setup start info block for bsp here instead of in the loop because core_id
    core_start_info_block[0].core_id = 0;
    core_start_info_block[0].cpu_local = ((uintptr_t) cpu_local_block);
    core_start_info_block[0].stack = ((uintptr_t) pmm_alloc(CORE_STACK_PGCNT) + g_globals_boot_info->hhdm_offset) + (CORE_STACK_PGCNT * PTM_PAGE_GRANULARITY);
    // @note: bsp will fill this in later
    core_start_info_block[0].arch_pointer = (void*) &arch_start_info_block[0];

    uint64_t core_id = 1;
    for(uint64_t i = 0; i < core_count; i++) {
        if(g_boot_core_is_bsp(i)) { continue; }

        core_start_info_t* core_start_info = &core_start_info_block[core_id];
        core_start_info->cpu_local = ((uintptr_t) cpu_local_block) + (core_id * kernel_info->cpu_local_size);
        core_start_info->stack = ((uintptr_t) pmm_alloc(CORE_STACK_PGCNT) + g_globals_boot_info->hhdm_offset) + (CORE_STACK_PGCNT * PTM_PAGE_GRANULARITY);
        core_start_info->core_id = core_id++;

        // @note: we let the aps handle setting up the gdt and tss, we just need to allocate one for them
        core_start_info->arch_pointer = (void*) &arch_start_info_block[core_id];

        log_print("starting ap %lu\n", core_start_info->core_id);
        g_boot_start_ap(i, core_start_info);
    }
}
