// a0 = entry point
// a1 = stack pointer
// a2 = new satp value
// a3 = boot info pointer
.section .text
.balign 4
.global riscv64_kernel_handoff
.type   riscv64_kernel_handoff, @function
riscv64_kernel_handoff:
    mv      sp, a1

    // save kernel entry point
    mv      t0, a0

    // switch to the kernel page tables.
    csrw    satp, a2
    sfence.vma zero, zero

    mv      a0, a3

    li      a2,  0
    li      a3,  0
    li      a4,  0
    li      a5,  0
    li      a6,  0
    li      a7,  0
    li      t3,  0
    li      t4,  0
    li      t5,  0
    li      t6,  0

    jr      t0

.global riscv64_kernel_handoff_end
riscv64_kernel_handoff_end:
.size riscv64_kernel_handoff, riscv64_kernel_handoff_end - riscv64_kernel_handoff

.balign 4
.global riscv64_early_trap_handler
.type   riscv64_early_trap_handler, @function
riscv64_early_trap_handler:
    wfi
    j       riscv64_early_trap_handler
.size riscv64_early_trap_handler, . - riscv64_early_trap_handler

.balign 4
.global riscv64_init_gp
.type   riscv64_init_gp, @function
riscv64_init_gp:
    la      gp, 0
    ret
.size riscv64_init_gp, . - riscv64_init_gp
