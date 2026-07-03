#include <arch.h>
#include <arch/csr.h>
#include <arch/machine.h>
#include <boot/core.h>
#include <log.h>
#include <stdint.h>

extern uint8_t riscv64_early_trap_handler[];
extern void riscv64_init_gp(uint64_t gp);

void arch_machine_init(core_start_info_t* core_info) {
    ARCH_CSR_CLEAR_BITS(sstatus, ARCH_CSR_SSTATUS_FS_MASK | ARCH_CSR_SSTATUS_SUM | ARCH_CSR_SSTATUS_MXR | ARCH_CSR_SSTATUS_SIE);
    ARCH_CSR_SET_BITS(sstatus, ARCH_CSR_SSTATUS_FS_INITIAL);
    ARCH_CSR_WRITE(sie, 0);

    // @todo: should we do this?
    ARCH_CSR_WRITE(stvec, (uintptr_t) riscv64_early_trap_handler | ARCH_CSR_STVEC_MODE_DIRECT);

    ARCH_CSR_WRITE(sscratch, core_info->cpu_local);
    riscv64_init_gp(((arch_machine_core_start_info_t*) core_info->arch_pointer)->global_pointer);

    log_print("arch_machine_init: core %lu  sscratch=0x%lx\n", core_info->core_id, core_info->cpu_local);
}
