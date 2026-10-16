#pragma once
#include <protocol/bootinfo.h>
#include <stdint.h>

typedef struct {
    uintptr_t elf_data;

    bootinfo_kernel_entry_point_t entry_point;
    uintptr_t kernel_base;

    bootinfo_segment_t* segments;
    size_t segment_count;

    bootinfo_kernel_info_t* kernel_info;
} elfldr_loader_info_t;

bool elfldr_load_kernel(elfldr_loader_info_t* out_info);
