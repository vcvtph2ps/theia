#include <arch/csr.h>
#include <boot/boot.h>
#include <boot/efi.h>
#include <common/libfdt.h>
#include <efi.h>
#include <efi/efi_riscv64.h>
#include <log.h>
#include <memory/ptm.h>
#include <panic.h>
#include <stddef.h>
#include <stdint.h>

static bool boot_hartid_from_protocol(uint64_t* out) {
    EFI_GUID guid = RISCV_EFI_BOOT_PROTOCOL_GUID;
    RISCV_EFI_BOOT_PROTOCOL* protocol = nullptr;
    EFI_STATUS status = g_efi_system_table->BootServices->LocateProtocol(&guid, nullptr, (void**) &protocol);
    if(EFI_ERROR(status) || protocol == nullptr) { return false; }

    UINTN hart_id = 0;
    status = protocol->GetBootHartId(protocol, &hart_id);
    if(EFI_ERROR(status)) { return false; }

    *out = hart_id;
    return true;
}

static bool boot_hartid_from_dtb(uint64_t* out) {
    const void* fdt = (const void*) efi_find_dtb();
    if(fdt == nullptr || fdt_check_header(fdt) != 0) { return false; }

    int chosen = fdt_path_offset(fdt, "/chosen");
    if(chosen < 0) { return false; }

    int len = 0;
    const void* prop = fdt_getprop(fdt, chosen, "boot-hartid", &len);
    if(prop == nullptr) { return false; }

    if(len == (int) sizeof(fdt32_t)) {
        *out = fdt32_to_cpu(*(const fdt32_t*) prop);
        return true;
    }
    if(len == (int) sizeof(fdt64_t)) {
        *out = fdt64_to_cpu(*(const fdt64_t*) prop);
        return true;
    }
    return false;
}

void arch_efi_discover_boot_hartid(void) {
    uint64_t hart_id;
    if(boot_hartid_from_protocol(&hart_id)) {
        log_print("efi: boot hart id %lu (boot protocol)\n", hart_id);
    } else if(boot_hartid_from_dtb(&hart_id)) {
        log_print("efi: boot hart id %lu (device tree)\n", hart_id);
    } else {
        panic("efi: failed to determine the boot hart id");
    }

    g_boot_bsp_arch_id = hart_id;
}

size_t arch_efi_paging_level(void) {
    uint64_t satp = ARCH_CSR_READ(satp);

    switch(satp >> 60) {
        case 10: return 5; // Sv57
        case 9:  return 4; // Sv48
        case 8:  return 3; // Sv39
        default: panic("efi: unsupported SATP mode");
    }
}

void arch_efi_switch_page_tables(uintptr_t root) {
    uint64_t mode;
    switch(g_ptm.level_count) {
        case 5:  mode = ARCH_CSR_SATP_MODE_SV57; break;
        case 4:  mode = ARCH_CSR_SATP_MODE_SV48; break;
        default: mode = ARCH_CSR_SATP_MODE_SV39; break;
    }

    asm volatile("sfence.vma zero, zero" ::: "memory");
    ARCH_CSR_WRITE(satp, ARCH_CSR_SATP_MAKE(mode, root));
    asm volatile("sfence.vma zero, zero" ::: "memory");
}
