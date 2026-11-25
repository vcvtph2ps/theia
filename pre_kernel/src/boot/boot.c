#include <boot/tartarus.h>
#include <stdint.h>

[[noreturn]] void prekernel_entry(void* param1, void* param2);

__attribute__((no_sanitize("undefined"))) // @todo: tartarus misaligned pointer bug
[[noreturn, gnu::used]] void prekernel_entry(void* param1, void* param2) {
    // @todo: support for other than tartarus, limine uses the entry point request
    prekernel_entry_tartarus(param1, (uint16_t) (uintptr_t) param2);
}
