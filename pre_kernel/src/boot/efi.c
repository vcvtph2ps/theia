#include <boot/efi.h>
#include <efi.h>
#include <log.h>
#include <panic.h>
#include <runtime/mem.h>
#include <stddef.h>
#include <stdint.h>

static void* g_memmap_buffer = nullptr;
static size_t g_memmap_buffer_size = 0;

void* efi_alloc_pool(size_t size) {
    void* buffer = nullptr;
    EFI_STATUS status = g_efi_system_table->BootServices->AllocatePool(EfiLoaderData, size, &buffer);
    if(EFI_ERROR(status) || buffer == nullptr) { panic("failed to allocate %zu bytes from the firmware pool (status=0x%lx)", size, (uintptr_t) status); }
    return buffer;
}

void efi_free_pool(void* ptr) {
    g_efi_system_table->BootServices->FreePool(ptr);
}

static void memmap_fetch(efi_memmap_t* out) {
    EFI_BOOT_SERVICES* bs = g_efi_system_table->BootServices;
    UINTN map_size = g_memmap_buffer_size;
    UINTN map_key = 0;
    UINTN descriptor_size = 0;
    UINT32 descriptor_version = 0;

    for(;;) {
        EFI_STATUS status = bs->GetMemoryMap(&map_size, (EFI_MEMORY_DESCRIPTOR*) g_memmap_buffer, &map_key, &descriptor_size, &descriptor_version);
        if(status == EFI_SUCCESS) { break; }
        if(status != EFI_BUFFER_TOO_SMALL) { panic("GetMemoryMap failed (status=0x%lx)", (uintptr_t) status); }

        // firmware wants more room than we have
        map_size += 8 * descriptor_size + 0x1000;
        if(g_memmap_buffer != nullptr) { efi_free_pool(g_memmap_buffer); }
        g_memmap_buffer = efi_alloc_pool(map_size);
        g_memmap_buffer_size = map_size;
    }

    g_memmap_buffer_size = map_size;
    out->descriptors = (EFI_MEMORY_DESCRIPTOR*) g_memmap_buffer;
    out->descriptor_size = descriptor_size;
    out->entry_count = descriptor_size == 0 ? 0 : map_size / descriptor_size;
    out->map_key = map_key;
}

void efi_get_memory_map(efi_memmap_t* out) {
    if(g_memmap_buffer == nullptr) {
        g_memmap_buffer_size = 0x4000;
        g_memmap_buffer = efi_alloc_pool(g_memmap_buffer_size);
    }

    memmap_fetch(out);
}

EFI_STATUS efi_exit_boot_services(EFI_HANDLE image_handle, efi_memmap_t* out) {
    EFI_BOOT_SERVICES* bs = g_efi_system_table->BootServices;

    for(;;) {
        efi_get_memory_map(out);
        EFI_STATUS status = bs->ExitBootServices(image_handle, out->map_key);
        if(status == EFI_SUCCESS) { return status; }
        if(status != EFI_INVALID_PARAMETER) { return status; }
        // the map key changed underneath us, oops
    }
}

static bool open_root(EFI_FILE_HANDLE* out_root) {
    EFI_BOOT_SERVICES* bs = g_efi_system_table->BootServices;

    EFI_LOADED_IMAGE_PROTOCOL* loaded_image = nullptr;
    EFI_GUID loaded_image_guid = EFI_LOADED_IMAGE_PROTOCOL_GUID;
    EFI_STATUS status = bs->HandleProtocol(g_efi_image_handle, &loaded_image_guid, (void**) &loaded_image);
    if(EFI_ERROR(status) || loaded_image == nullptr) { panic("failed to locate the loaded image protocol (status=0x%lx)", (uintptr_t) status); }

    EFI_SIMPLE_FILE_SYSTEM_PROTOCOL* file_system = nullptr;
    EFI_GUID file_system_guid = EFI_SIMPLE_FILE_SYSTEM_PROTOCOL_GUID;
    status = bs->HandleProtocol(loaded_image->DeviceHandle, &file_system_guid, (void**) &file_system);
    if(EFI_ERROR(status) || file_system == nullptr) { panic("failed to locate the ESP file system (status=0x%lx)", (uintptr_t) status); }

    EFI_FILE_HANDLE root = nullptr;
    status = file_system->OpenVolume(file_system, &root);
    if(EFI_ERROR(status) || root == nullptr) { panic("failed to open the ESP root directory (status=0x%lx)", (uintptr_t) status); }

    *out_root = root;
    return true;
}

bool efi_try_read_file(const CHAR16* path, void** out_data, size_t* out_size) {
    EFI_FILE_HANDLE root = nullptr;
    if(!open_root(&root)) { return false; }

    EFI_FILE_HANDLE file = nullptr;
    EFI_STATUS status = root->Open(root, &file, (CHAR16*) path, EFI_FILE_MODE_READ, 0);
    if(status == EFI_NOT_FOUND) {
        root->Close(root);
        return false;
    }
    if(EFI_ERROR(status) || file == nullptr) { panic("failed to open %ls (status=0x%lx)", path, (uintptr_t) status); }

    EFI_GUID file_info_guid = EFI_FILE_INFO_ID;
    UINTN info_size = 256;
    EFI_FILE_INFO* info = (EFI_FILE_INFO*) efi_alloc_pool(info_size);
    status = file->GetInfo(file, &file_info_guid, &info_size, info);
    if(status == EFI_BUFFER_TOO_SMALL) {
        efi_free_pool(info);
        info = (EFI_FILE_INFO*) efi_alloc_pool(info_size);
        status = file->GetInfo(file, &file_info_guid, &info_size, info);
    }
    if(EFI_ERROR(status)) { panic("failed to query info for %ls (status=0x%lx)", path, (uintptr_t) status); }

    size_t size = info->FileSize;
    efi_free_pool(info);

    void* data = efi_alloc_pool(size);
    UINTN read_size = size;
    status = file->Read(file, &read_size, data);
    if(EFI_ERROR(status)) { panic("failed to read %ls (status=0x%lx)", path, (uintptr_t) status); }

    file->Close(file);
    root->Close(root);

    *out_data = data;
    *out_size = read_size;
    return true;
}

bool efi_read_file(const CHAR16* path, void** out_data, size_t* out_size) {
    if(!efi_try_read_file(path, out_data, out_size)) { panic("required file %ls is missing from the ESP", path); }
    return true;
}
