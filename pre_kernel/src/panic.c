#include <log.h>
#include <panic.h>
#include <stdarg.h>

[[noreturn]] void panic(const char* format, ...) {
    va_list args;
    va_start(args, format);
    log_print("prekernel panic: ");
    log_vprint_raw(format, args);
    log_print_raw("\n");
    va_end(args);
    while(1);
}
