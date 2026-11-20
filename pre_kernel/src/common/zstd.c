// NOLINTBEGIN
#include <common/zstd.h>
#include <globals.h>
#include <lib/math.h>
#include <memory/pmm.h>
#include <memory/ptm.h>
#include <stdint.h>

#define ZSTD_STATIC_LINKING_ONLY
#include <zstd.h>

void* ZSTD_malloc(size_t size);
void* ZSTD_calloc(size_t num, size_t size);
void ZSTD_free(void* ptr);

#define ZSTD_ALLOC_ALIGN ((size_t) 16)

typedef struct {
    uintptr_t raw_phys;
    size_t page_count;
} zstd_alloc_header_t;

void* ZSTD_malloc(size_t size) {
    if(size == 0) { return nullptr; }
    if(size > SIZE_MAX - sizeof(zstd_alloc_header_t) - ZSTD_ALLOC_ALIGN - (PTM_PAGE_GRANULARITY - 1)) { return nullptr; }

    size_t total = MATH_ALIGN_UP(sizeof(zstd_alloc_header_t) + ZSTD_ALLOC_ALIGN + size, PTM_PAGE_GRANULARITY);
    size_t pages = total / PTM_PAGE_GRANULARITY;

    void* phys = pmm_alloc(pages);
    if(phys == nullptr) { return nullptr; }

    uintptr_t raw = (uintptr_t) phys + g_globals_boot_info->hhdm_offset;
    uintptr_t user = MATH_ALIGN_UP(raw + sizeof(zstd_alloc_header_t), ZSTD_ALLOC_ALIGN);
    zstd_alloc_header_t* header = (zstd_alloc_header_t*) (user - sizeof(zstd_alloc_header_t));
    header->raw_phys = (uintptr_t) phys;
    header->page_count = pages;
    return (void*) user;
}

void* ZSTD_calloc(size_t num, size_t size) {
    if(num != 0 && size > SIZE_MAX / num) { return nullptr; }

    size_t total = num * size;
    void* ptr = ZSTD_malloc(total);
    if(ptr != nullptr) { __builtin_memset(ptr, 0, total); }
    return ptr;
}

void ZSTD_free(void* ptr) {
    if(ptr == nullptr) { return; }

    zstd_alloc_header_t* header = (zstd_alloc_header_t*) ((uintptr_t) ptr - sizeof(zstd_alloc_header_t));
    pmm_free((void*) header->raw_phys, header->page_count);
}

void* zstd_decompress(const void* src, size_t src_size, size_t* buffer_size) {
    unsigned long long frame_size = ZSTD_getFrameContentSize(src, src_size);
    if(frame_size == ZSTD_CONTENTSIZE_ERROR) { return nullptr; }
    if(frame_size == ZSTD_CONTENTSIZE_UNKNOWN) {
        frame_size = ZSTD_decompressBound(src, src_size);
        if(frame_size == ZSTD_CONTENTSIZE_ERROR) { return nullptr; }
    }
    if(frame_size > SIZE_MAX) { return nullptr; }

    size_t capacity = MATH_ALIGN_UP((size_t) frame_size, PTM_PAGE_GRANULARITY);
    if(capacity == 0) { capacity = PTM_PAGE_GRANULARITY; }

    size_t output_pages = capacity / PTM_PAGE_GRANULARITY;
    void* output_phys = pmm_alloc(output_pages);
    if(output_phys == nullptr) { return nullptr; }

    size_t workspace_size = ZSTD_estimateDCtxSize();
    size_t workspace_total = workspace_size + ZSTD_ALLOC_ALIGN;
    size_t workspace_pages = MATH_ALIGN_UP(workspace_total, PTM_PAGE_GRANULARITY) / PTM_PAGE_GRANULARITY;
    void* workspace_phys = pmm_alloc(workspace_pages);
    if(workspace_phys == nullptr) {
        pmm_free(output_phys, output_pages);
        return nullptr;
    }

    void* output = (void*) ((uintptr_t) output_phys + g_globals_boot_info->hhdm_offset);
    void* workspace = (void*) MATH_ALIGN_UP((uintptr_t) workspace_phys + g_globals_boot_info->hhdm_offset,
        ZSTD_ALLOC_ALIGN);
    ZSTD_DCtx* dctx = ZSTD_initStaticDCtx(workspace, workspace_size);
    if(dctx == nullptr) {
        pmm_free(workspace_phys, workspace_pages);
        pmm_free(output_phys, output_pages);
        return nullptr;
    }

    size_t got = ZSTD_decompressDCtx(dctx, output, capacity, src, src_size);
    pmm_free(workspace_phys, workspace_pages);
    if(ZSTD_isError(got)) {
        pmm_free(output_phys, output_pages);
        return nullptr;
    }

    if(buffer_size != nullptr) { *buffer_size = capacity; }
    return output;
}
// NOLINTEND