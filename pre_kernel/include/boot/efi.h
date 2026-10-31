#pragma once
#include <efi.h>
#include <protocol/bootinfo.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

extern EFI_SYSTEM_TABLE* g_efi_system_table;
extern EFI_HANDLE g_efi_image_handle;

typedef struct {
    EFI_MEMORY_DESCRIPTOR* descriptors;
    UINTN descriptor_size;
    UINTN entry_count;
    UINTN map_key;
} efi_memmap_t;

/// @brief Fetch and cache the UEFI memory map into a pool allocation
void efi_get_memory_map(efi_memmap_t* out);

/// @brief Refetch the memory map and exit boot services
/// @return EFI_SUCCESS or the firmware status
EFI_STATUS efi_exit_boot_services(EFI_HANDLE image_handle, efi_memmap_t* out);

void* efi_alloc_pool(size_t size);
void efi_free_pool(void* ptr);

/// @brief Read an entire file into a allocated pool buffer
/// @note Allocates out_data
/// @note Panics if the file cannot be read
[[nodiscard]] bool efi_read_file(const CHAR16* path, void** out_data, size_t* out_size);

/// @brief efi_read_file but returns false without panicking if the file is absent
[[nodiscard]] bool efi_try_read_file(const CHAR16* path, void** out_data, size_t* out_size);

/// @brief query the UEFI GOP and fill a framebuffer description for the boot info
/// @return false if no usable GOP is present
[[nodiscard]] bool efi_gop_get_framebuffer(bootinfo_framebuffer_t* out);

/// @brief Paging level count the firmware currently has enabled
/// @note The EFI boot path's page tables must match this until the kernel handoff
[[nodiscard]] size_t arch_efi_paging_level(void);

/// @brief Load the given top level page table as the active address space
void arch_efi_switch_page_tables(uintptr_t root);

#if defined(__riscv) && __riscv_xlen == 64
/// @brief Determine the boot hart id and store it in g_boot_bsp_arch_id
void arch_efi_discover_boot_hartid(void);
#endif

/// @brief Entry point into the generic EFI boot flow
[[noreturn]] void prekernel_efi_boot(void);
