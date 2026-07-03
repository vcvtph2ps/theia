#include <globals.h>
#include <lib/math.h>
#include <memory/pmm.h>
#include <memory/ptm.h>
#include <runtime/mem.h>
#include <stddef.h>

#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wmissing-prototypes"

void parse_isa_string(const char* isa_string) {
    size_t isa_len = strlen(isa_string);
    size_t isa_string_len = 0;
    size_t extention_count = 0;
    for(size_t i = 0; i < (size_t) isa_len; i++) {
        if(isa_string[i] == '_') { extention_count++; }
        if(isa_string[i] == '_' && isa_string_len == 0) { isa_string_len = i; }
    }

    size_t info_size = isa_len + 1;
    info_size += (extention_count + 1) * sizeof(char*);

    info_size = MATH_ALIGN_UP(info_size, PTM_PAGE_GRANULARITY);
    void* info_buffer = (void*) ((uintptr_t) pmm_alloc(info_size / PTM_PAGE_GRANULARITY) + g_globals_boot_info->hhdm_offset);

    char* cursor = info_buffer;

    char* base_isa = cursor;
    cursor += isa_string_len + 1;
    cursor = (char*) MATH_ALIGN_UP((uintptr_t) cursor, alignof(char*));

    char** extensions = (char**) cursor;
    cursor += (extention_count + 1) * sizeof(char*);

    memcpy(base_isa, isa_string, isa_string_len);
    base_isa[isa_string_len] = '\0';

    size_t ext_index = 0;

    const char* ext_start = isa_string + isa_string_len + 1;
    const char* p = ext_start;

    while(*p) {
        const char* end = p;

        while(*end && *end != '_') { end++; }

        size_t len = (size_t) (end - p);

        extensions[ext_index] = cursor;

        memcpy(cursor, p, len);
        cursor[len] = '\0';

        cursor += len + 1;
        ext_index++;

        if(*end == '\0') { break; }

        p = end + 1;
    }

    g_globals_boot_info->riscv_base_isa_string = base_isa;
    g_globals_boot_info->riscv_extension_count = ext_index;
    g_globals_boot_info->riscv_extentions = (char*(*) []) extensions;
}

#pragma clang diagnostic pop
