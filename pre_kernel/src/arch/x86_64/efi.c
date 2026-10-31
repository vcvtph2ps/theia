#include <arch/cr.h>
#include <boot/efi.h>
#include <stddef.h>
#include <stdint.h>

size_t arch_efi_paging_level(void) {
    return (arch_cr_read_cr4() & (1u << 12)) ? 5 : 4;
}

void arch_efi_switch_page_tables(uintptr_t root) {
    __asm__ volatile("mov %0, %%cr3" : : "r"(root) : "memory");
}
