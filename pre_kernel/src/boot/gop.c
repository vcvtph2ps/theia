#include <boot/bootinfo.h>
#include <boot/efi.h>
#include <efi.h>
#include <log.h>
#include <stddef.h>
#include <stdint.h>

bool efi_gop_get_framebuffer(boot_framebuffer_t* out) {
    EFI_GUID gop_guid = EFI_GRAPHICS_OUTPUT_PROTOCOL_GUID;
    EFI_GRAPHICS_OUTPUT_PROTOCOL* gop = nullptr;

    EFI_STATUS status = g_efi_system_table->BootServices->LocateProtocol(&gop_guid, nullptr, (void**) &gop);
    if(EFI_ERROR(status) || gop == nullptr || gop->Mode == nullptr || gop->Mode->Info == nullptr) {
        log_print("gop: no graphics output protocol available\n");
        return false;
    }

    EFI_GRAPHICS_OUTPUT_MODE_INFORMATION* info = gop->Mode->Info;
    if(info->PixelFormat == PixelBltOnly) {
        log_print("gop: only blt-only modes are supported by the firmware\n");
        return false;
    }

    out->vaddr = nullptr;
    out->paddr = (uintptr_t) gop->Mode->FrameBufferBase;
    out->width = info->HorizontalResolution;
    out->height = info->VerticalResolution;
    out->bpp = 32;
    out->pitch = info->PixelsPerScanLine * 4;
    out->size = (uint64_t) out->pitch * out->height;

    switch(info->PixelFormat) {
        case PixelRedGreenBlueReserved8BitPerColor:
            out->red_position = 0;
            out->green_position = 8;
            out->blue_position = 16;
            out->red_size = 8;
            out->green_size = 8;
            out->blue_size = 8;
            break;
        case PixelBlueGreenRedReserved8BitPerColor:
            out->red_position = 16;
            out->green_position = 8;
            out->blue_position = 0;
            out->red_size = 8;
            out->green_size = 8;
            out->blue_size = 8;
            break;
        case PixelBitMask:
            boot_framebuffer_from_mask(info->PixelInformation.RedMask, &out->red_position, &out->red_size);
            boot_framebuffer_from_mask(info->PixelInformation.GreenMask, &out->green_position, &out->green_size);
            boot_framebuffer_from_mask(info->PixelInformation.BlueMask, &out->blue_position, &out->blue_size);
            break;
        default: log_print("gop: unsupported pixel format %u\n", info->PixelFormat); return false;
    }

    log_print("gop: framebuffer paddr=0x%lx %ux%u pitch=0x%x\n", out->paddr, out->width, out->height, out->pitch);
    return true;
}
