#include <arch.h>
#include <arch/csr.h>
#include <globals.h>
#include <lib/helpers.h>
#include <lib/math.h>
#include <log.h>
#include <memory/pmm.h>
#include <memory/ptm.h>
#include <panic.h>
#include <runtime/mem.h>
#include <stdint.h>

void arch_spin_hint() {
    __asm__ volatile("pause");
}

#define SBI_EXT_LEGACY_PUTCHAR 0x01

static inline long sbi_call1(long ext, long fid, long a0_val) {
    register long a0 asm("a0") = a0_val;
    register long a6 asm("a6") = fid;
    register long a7 asm("a7") = ext;
    asm volatile("ecall" : "+r"(a0) : "r"(a6), "r"(a7) : "memory");
    return a0;
}


void arch_init_early() {}

void arch_debug_putc(char c) {
    sbi_call1(SBI_EXT_LEGACY_PUTCHAR, 0, (unsigned char) c);
}

typedef void (*riscv64_kernel_entry_t)(bootinfo_kernel_entry_point_t entry, uintptr_t stack, uint64_t satp_val, bootinfo_t* boot_info);

extern uint8_t riscv64_kernel_handoff[]; // NOLINT
extern uint8_t riscv64_kernel_handoff_end[]; // NOLINT

static riscv64_kernel_entry_t g_boot_trampoline = nullptr;

static uint64_t make_satp_value(uintptr_t root_pa) {
    uint64_t mode;
    switch(g_ptm.level_count) {
        case 5:  mode = ARCH_CSR_SATP_MODE_SV57; break;
        case 4:  mode = ARCH_CSR_SATP_MODE_SV48; break;
        case 3:  [[fallthrough]];
        default: mode = ARCH_CSR_SATP_MODE_SV39; break;
    }
    return ARCH_CSR_SATP_MAKE(mode, root_pa);
}

bool arch_fdt_parse_extentions();
bool arch_acpi_parse_extentions();

bool arch_parse_extentions() {
    // try ACPI first if supported
    if(g_globals_boot_info->rdsp_physical) {
        if(arch_acpi_parse_extentions()) { return true; }
    }

    if(g_globals_boot_info->dtb_physical) { return arch_fdt_parse_extentions(); }

    return false;
}

void arch_prepare_handoff(void) {
    size_t trampoline_bytes = (size_t) (riscv64_kernel_handoff_end - riscv64_kernel_handoff);
    size_t pages = MATH_ALIGN_UP(trampoline_bytes, PTM_PAGE_GRANULARITY) / PTM_PAGE_GRANULARITY;
    log_print("riscv64: handoff trampoline size: %zu byte(s), %zu page(s)\n", trampoline_bytes, pages);

    void* trampoline_phys = pmm_alloc(pages);
    void* trampoline_virt = (void*) ((uintptr_t) trampoline_phys + g_globals_boot_info->hhdm_offset);
    memcpy(trampoline_virt, (void*) riscv64_kernel_handoff, trampoline_bytes);

    uintptr_t trampoline_pa = (uintptr_t) trampoline_phys;
    size_t trampoline_sz = pages * PTM_PAGE_GRANULARITY;

    uint64_t satp_now = ARCH_CSR_READ(satp);
    uintptr_t cur_root = (uintptr_t) ((satp_now & ARCH_CSR_SATP_PPN_MASK) << 12);
    uint64_t cur_mode = satp_now >> 60;
    size_t cur_levels;
    switch(cur_mode) {
        case 10: cur_levels = 5; break; // Sv57
        case 9:  cur_levels = 4; break; // Sv48
        case 8:  cur_levels = 3; break; // Sv39
        default: panic("Unsupported SATP mode %lu", cur_mode);
    }

    ptm_map_at(cur_root, cur_levels, MEMORY_KERNELSPACE_END, trampoline_pa, trampoline_sz, PTM_FLAG_READ | PTM_FLAG_EXEC);
    ptm_map(MEMORY_KERNELSPACE_END, trampoline_pa, trampoline_pa, PTM_FLAG_READ | PTM_FLAG_EXEC);

    g_boot_trampoline = (riscv64_kernel_entry_t) MEMORY_KERNELSPACE_END;
    log_print("riscv64: handoff trampoline at phys 0x%lx\n", trampoline_pa);
}

__attribute__((no_sanitize("undefined"))) [[noreturn]] void arch_handoff_to_kernel(bootinfo_kernel_entry_point_t entry, uintptr_t stack, bootinfo_t* boot_info) {
    uint64_t satp_val = make_satp_value(g_ptm.tplt);
    g_boot_trampoline(entry, stack, satp_val, boot_info);
    __builtin_unreachable();
}
