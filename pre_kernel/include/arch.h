#pragma once
#include <boot/core.h>
#include <protocol/bootinfo.h>
#include <stddef.h>
#include <stdint.h>

/**
 * @brief Called very early in BSP startup.
 */
void arch_init_early();

/**
 * @brief Output a character to the debug console (e.g. serial port).
 */
void arch_debug_putc(char c);

/**
 * @brief Allocate per core resources and start APs
 * @note index 0 for the info block is reserved for the BSP
 */
void arch_setup_cpus(core_start_info_t* core_start_info_block, size_t core_count, bootinfo_kernel_info_t* kernel_image_info);

/**
 * @brief Per core machine initialisation
 * @note Called on every core (BSP and each AP) before handoff.
 */
void arch_machine_init(core_start_info_t* core_info);

/**
 * @brief Prepare the kernel handoff trampoline.
 * @note Called once on the BSP after HHDM mappings have been created.
 */
void arch_prepare_handoff();

/**
 * @brief Handoff to the kernel
 * @param boot_info only passed to the kernel on the BSP
 */
[[noreturn]] void arch_handoff_to_kernel(bootinfo_kernel_entry_point_t entry, uintptr_t stack, bootinfo_t* boot_info, uint64_t core_id);

/**
 * @brief Hint to the CPU that we are in a spin wait loop.
 */
void arch_spin_hint();


#ifdef __x86_64__
/**
 * @brief Setup PAT, needs to happen before any write combining mappings are used
 */
void arch_setup_page_table_attributes();
#endif

#if defined(__riscv) && __riscv_xlen == 64
/**
 * @brief Parse architecture specific information from the FDT/ACPI and store it in the bootinfo struct for the kernel.
 */
bool arch_parse_extentions();
#endif
