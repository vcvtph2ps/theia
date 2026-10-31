#include <boot/efi.h>
#include <efi.h>

EFI_SYSTEM_TABLE* g_efi_system_table;
EFI_HANDLE g_efi_image_handle;

[[noreturn]] EFI_STATUS efi_main(EFI_HANDLE image_handle, EFI_SYSTEM_TABLE* system_table) {
    g_efi_image_handle = image_handle;
    g_efi_system_table = system_table;

    prekernel_efi_boot();
}
