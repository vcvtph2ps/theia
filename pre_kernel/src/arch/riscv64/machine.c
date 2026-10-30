#include <arch.h>
#include <arch/csr.h>
#include <arch/machine.h>
#include <boot/core.h>
#include <globals.h>
#include <log.h>
#include <memory/pmm.h>
#include <memory/ptm.h>
#include <runtime/mem.h>
#include <stdint.h>

#define CORE_STACK_PGCNT 16

extern uint8_t riscv64_early_trap_handler[];
extern void riscv64_init_gp(uint64_t gp);

void arch_machine_init(core_start_info_t* core_info, bootinfo_kernel_info_t* kernel_info) {
    core_info->stack = ((uintptr_t) pmm_alloc(CORE_STACK_PGCNT) + g_globals_boot_info->hhdm_offset) + (CORE_STACK_PGCNT * PTM_PAGE_GRANULARITY);

    // Only the bsp is brought up by the prekernel, the kernel is responsible for allocating the machine state of the application processors.
    arch_machine_core_start_info_t* core_arch_info = (arch_machine_core_start_info_t*) ((uintptr_t) pmm_alloc(1) + g_globals_boot_info->hhdm_offset);
    memset(core_arch_info, 0, PTM_PAGE_GRANULARITY);
    core_arch_info->global_pointer = kernel_info->global_pointer;
    core_info->arch_pointer = core_arch_info;

    ARCH_CSR_CLEAR_BITS(sstatus, ARCH_CSR_SSTATUS_FS_MASK | ARCH_CSR_SSTATUS_SUM | ARCH_CSR_SSTATUS_MXR | ARCH_CSR_SSTATUS_SIE);
    ARCH_CSR_SET_BITS(sstatus, ARCH_CSR_SSTATUS_FS_INITIAL);
    ARCH_CSR_WRITE(sie, 0);

    // @todo: should we do this?
    ARCH_CSR_WRITE(stvec, (uintptr_t) riscv64_early_trap_handler | ARCH_CSR_STVEC_MODE_DIRECT);

    ARCH_CSR_WRITE(sscratch, core_info->cpu_local);
    riscv64_init_gp(core_arch_info->global_pointer);

    log_print("arch_machine_init: core %lu  sscratch=0x%lx\n", core_info->core_id, core_info->cpu_local);
}
