#include <arch/gdt.h>

// We need to have 32 bit code/data segements for switching to level 5 paging
#define DEFINE_SEGMENT(ACCESS, FLAGS) { .limit_low = 0, .base_low = 0, .base_mid = 0, .access = (ACCESS), .limit_high_flags = ((FLAGS) << 4), .base_high = 0 }
#define DEFINE_SEGMENT32(ACCESS, FLAGS) { .limit_low = 0xffff, .base_low = 0, .base_mid = 0, .access = (ACCESS), .limit_high_flags = ((FLAGS) << 4) | 0xf, .base_high = 0 }

const arch_gdt_t g_arch_gdt_static_data = {
    {}, // null @ 0x00
    DEFINE_SEGMENT32(0x9b, 0xC), // code32 @ 0x08
    DEFINE_SEGMENT32(0x93, 0xC), // data32 @ 0x10
    DEFINE_SEGMENT(0x9b, 0xA), // code64 @ 0x18
    DEFINE_SEGMENT(0x93, 0xC), // data64 @ 0x20
    {} // tss @ 0x28 (unused)
};
