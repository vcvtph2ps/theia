#include <lib/math.h>
#include <log.h>
#include <memory/pmm.h>
#include <memory/ptm.h>
#include <runtime/mem.h>

#define KGZ_USE_OWN_MACROS
#define KGZ_MALLOC(size) pmm_alloc(size / PTM_PAGE_GRANULARITY)
#define KGZ_FREE(ptr, size) pmm_free(ptr, size / PTM_PAGE_GRANULARITY)
#define KGZ_MEMCPY(dst, src, n) memcpy((dst), (src), (n))
#define KGZ_MEMSET(ptr, val, size) memset((ptr), (val), (size))
#define KGZ_PRINTF(...) log_print(__VA_ARGS__)

#define KGZ_GRANULE 4096

#define KGZ_IMPLEMENTATION
#include <common/kgz.h>
