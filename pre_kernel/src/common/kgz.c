#include <globals.h>
#include <lib/math.h>
#include <log.h>
#include <memory/pmm.h>
#include <memory/ptm.h>
#include <runtime/mem.h>

static void* kgz_malloc(uint64_t size) {
    return (void*) ((uintptr_t) pmm_alloc(size / PTM_PAGE_GRANULARITY) + g_globals_boot_info->hhdm_offset);
}

static void kgz_free(void* ptr, uint64_t size) {
    pmm_free((void*) ((uintptr_t) ptr - g_globals_boot_info->hhdm_offset), size / PTM_PAGE_GRANULARITY);
}

#define KGZ_USE_OWN_MACROS
#define KGZ_MALLOC(size) kgz_malloc(size)
#define KGZ_FREE(ptr, size) kgz_free(ptr, size)
#define KGZ_MEMCPY(dst, src, n) memcpy((dst), (src), (n))
#define KGZ_MEMSET(ptr, val, size) memset((ptr), (val), (size))
#define KGZ_PRINTF(...) log_print(__VA_ARGS__)

#define KGZ_GRANULE 4096

#define KGZ_IMPLEMENTATION
#include <common/kgz.h>
