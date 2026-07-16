#pragma once
#include <stdarg.h>

void log_vprint_raw(const char* fmt, va_list val);
[[gnu::format(printf, 1, 2)]] void log_print_raw(const char* fmt, ...);

void log_vprint(const char* fmt, va_list val);
[[gnu::format(printf, 1, 2)]] void log_print(const char* fmt, ...);

void log_framebuffer_init();
