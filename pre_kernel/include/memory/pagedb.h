#pragma once
#include <stddef.h>
#include <stdint.h>

void pagedb_setup(uintptr_t kernel_base, size_t pagedb_entry_size, uintptr_t* pfndb_start, size_t* pfndb_size, uintptr_t* pfndb_bitmap_start, size_t* pfndb_bitmap_size);
