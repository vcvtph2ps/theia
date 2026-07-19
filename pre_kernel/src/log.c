#include <arch.h>
#include <flanterm.h>
#include <flanterm_backends/fb.h>
#include <globals.h>
#include <log.h>
#include <nanoprintf/nanoprintf.h>
#include <spinlock.h>
struct flanterm_context* g_log_framebuffer_context = nullptr;

void log_framebuffer_init() {
    if(g_globals_boot_info->framebuffer_count == 0) { return; }
    bootinfo_framebuffer_t framebuffer = g_globals_boot_info->framebuffers[0];
    struct flanterm_context* ft_ctx = flanterm_fb_init(
        nullptr,
        nullptr,
        framebuffer.vaddr,
        framebuffer.width,
        framebuffer.height,
        framebuffer.pitch,
        framebuffer.red_size,
        framebuffer.red_position,
        framebuffer.green_size,
        framebuffer.green_position,
        framebuffer.blue_size,
        framebuffer.blue_position,
        nullptr,
        nullptr,
        nullptr,
        nullptr,
        nullptr,
        nullptr,
        nullptr,
        nullptr,
        0,
        0,
        1,
        0,
        0,
        0,
        0
    );
    if(ft_ctx != nullptr) { g_log_framebuffer_context = ft_ctx; }
}

static void putc(int c, void* ctx) {
    (void) ctx;
    arch_debug_putc((char) c);
    if(g_log_framebuffer_context != nullptr) {
        if(c == '\n') { flanterm_write(g_log_framebuffer_context, "\r", 1); }
        flanterm_write(g_log_framebuffer_context, (const char*) &c, 1);
    }
}

spinlock_t g_log_lock = SPINLOCK_INIT;

void log_vprint_raw(const char* fmt, va_list val) {
    spinlock_lock(&g_log_lock);
    npf_vpprintf(putc, nullptr, fmt, val);
    spinlock_unlock(&g_log_lock);
}

void log_print_raw(const char* fmt, ...) {
    va_list val;
    va_start(val, fmt);
    spinlock_lock(&g_log_lock);
    npf_vpprintf(putc, nullptr, fmt, val);
    spinlock_unlock(&g_log_lock);
    va_end(val);
}


void log_vprint(const char* fmt, va_list val) {
    spinlock_lock(&g_log_lock);
    npf_vpprintf(putc, nullptr, "prekernel | ", nullptr);
    npf_vpprintf(putc, nullptr, fmt, val);
    spinlock_unlock(&g_log_lock);
}

void log_print(const char* fmt, ...) {
    va_list val;
    va_start(val, fmt);
    spinlock_lock(&g_log_lock);
    npf_vpprintf(putc, nullptr, "prekernel | ", nullptr);
    npf_vpprintf(putc, nullptr, fmt, val);
    spinlock_unlock(&g_log_lock);
    va_end(val);
}
