#include <globals.h>
#include <log.h>
#include <uacpi/kernel_api.h>
#include <uacpi/log.h>
#include <uacpi/status.h>

uacpi_status uacpi_kernel_get_rsdp(uacpi_phys_addr* out_rsdp_address) {
    if(!g_globals_boot_info->rdsp_physical) { return UACPI_STATUS_NOT_FOUND; }
    *out_rsdp_address = g_globals_boot_info->rdsp_physical;
    return UACPI_STATUS_OK;
}

// Please forgive me...
void* uacpi_kernel_map(uacpi_phys_addr paddr, uacpi_size length) {
    (void) length;
    return (void*) ((uintptr_t) paddr + g_globals_boot_info->hhdm_offset);
}

void uacpi_kernel_unmap(void* addr, uacpi_size length) {
    (void) addr;
    (void) length;
}

static void uacpi_kernel_vlog(uacpi_log_level level, const uacpi_char* fmt, uacpi_va_list args) {
    switch(level) {
        case UACPI_LOG_ERROR: log_print("uacpi error: "); break;
        case UACPI_LOG_WARN:  log_print("uacpi warn: "); break;
        case UACPI_LOG_DEBUG: log_print("uacpi debug: "); break;
        case UACPI_LOG_TRACE: log_print("uacpi trace: "); break;
        case UACPI_LOG_INFO:  log_print("uacpi info: "); break;
    }
    log_vprint_raw(fmt, args);
}

UACPI_PRINTF_DECL(2, 3)
void uacpi_kernel_log(uacpi_log_level level, const uacpi_char* fmt, ...) {
    va_list val;
    va_start(val, fmt);
    uacpi_kernel_vlog(level, fmt, val);
    va_end(val);
}
