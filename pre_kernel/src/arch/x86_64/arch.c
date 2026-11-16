#include <arch.h>
#include <arch/16550uart.h>
#include <arch/cr.h>
#include <arch/io.h>
#include <globals.h>
#include <lib/helpers.h>
#include <lib/math.h>
#include <log.h>
#include <memory/pmm.h>
#include <memory/ptm.h>
#include <panic.h>
#include <runtime/mem.h>

typedef void (*x86_64_kernel_entry_t)(bootinfo_kernel_entry_point_t entry, uintptr_t stack, uintptr_t page_tables, bootinfo_t* boot_info, uintptr_t la57_support);

extern uint8_t x86_64_kernel_handoff[]; // NOLINT
extern uint8_t x86_64_kernel_handoff_end[]; // NOLINT

static x86_64_kernel_entry_t g_boot_trampoline = nullptr;

void arch_spin_hint() {
    __builtin_ia32_pause();
}

bool arch_probe_serial(bootinfo_serial_t* out) {
    if(!g_arch_16550uart_works) { return false; }

    *out = (bootinfo_serial_t) {
        .type = BOOTINFO_SERIAL_TYPE_PIO,
        .base = g_arch_16550uart_port,
        .size = 8,
    };
    return true;
}

void arch_init_early() {
    arch_16550uart_early_setup();
    arch_setup_page_table_attributes();
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

    // @note: when enabling 5 level paging we have to drop to protected mode with paging disabled, which means the trampoline must live below 4 GiB and be identity mapped
    uintptr_t trampoline_pa = (uintptr_t) boot_trampoline_alloc;
    uintptr_t trampoline_size = size * PTM_PAGE_GRANULARITY;
    if(trampoline_pa + trampoline_size > 0x1'0000'0000) { panic("boot trampoline must be below 4GiB"); }

    uintptr_t current_cr3 = arch_cr_read_cr3();
    uintptr_t level_count = (arch_cr_read_cr4() & (1u << 12)) ? 5u : 4u;
    uint8_t trampoline_flags = PTM_FLAG_READ | PTM_FLAG_WRITE | PTM_FLAG_EXEC;

    ptm_map_at(current_cr3, level_count, trampoline_pa, trampoline_pa, trampoline_size, trampoline_flags);
    ptm_map(trampoline_pa, trampoline_pa, trampoline_size, trampoline_flags);

    g_boot_trampoline = (x86_64_kernel_entry_t) trampoline_pa;
}

__attribute__((no_sanitize("undefined"))) [[noreturn]] void arch_handoff_to_kernel(bootinfo_kernel_entry_point_t entry, uintptr_t stack, bootinfo_t* boot_info) {
    g_boot_trampoline(entry, stack, g_ptm.tplt, boot_info, g_ptm.level_count == 5);
    __builtin_unreachable();
}
