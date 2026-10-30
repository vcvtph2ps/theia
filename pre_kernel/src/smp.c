#include <arch.h>
#include <globals.h>
#include <lib/math.h>
#include <log.h>
#include <memory/pmm.h>
#include <memory/ptm.h>
#include <panic.h>
#include <protocol/bootinfo.h>
#include <runtime/mem.h>
#include <smp.h>
#include <stddef.h>
#include <stdint.h>
#include <uacpi/acpi.h>
#include <uacpi/tables.h>

typedef struct smp_cpu_node smp_cpu_node_t;
struct smp_cpu_node {
    bootinfo_cpu_t cpu;
    smp_cpu_node_t* next;
};

bootinfo_cpu_t* g_smp_cpus = nullptr;
size_t g_smp_cpu_count = 0;

static smp_cpu_node_t* g_smp_cpu_list = nullptr;
static smp_cpu_node_t* g_smp_cpu_list_tail = nullptr;
static size_t g_smp_cpu_list_count = 0;

void smp_add_cpu(uint64_t arch_id) {
    smp_cpu_node_t* node = (smp_cpu_node_t*) ((uintptr_t) pmm_alloc(1) + g_globals_boot_info->hhdm_offset);

    node->cpu = (bootinfo_cpu_t) {
        .arch_id = arch_id,
        .is_bsp = arch_id == arch_smp_bsp_id(),
    };
    node->next = nullptr;

    if(g_smp_cpu_list_tail != nullptr) {
        g_smp_cpu_list_tail->next = node;
    } else {
        g_smp_cpu_list = node;
    }
    g_smp_cpu_list_tail = node;
    g_smp_cpu_list_count++;
}

static void smp_collapse_cpu_list() {
    if(g_smp_cpu_list_count == 0) { return; }

    // We have to know which cpu we are currently running on so the kernel can handoff on it and bring up the rest.
    bool found_bsp = false;
    for(smp_cpu_node_t* node = g_smp_cpu_list; node != nullptr; node = node->next) {
        if(node->cpu.is_bsp) {
            found_bsp = true;
            break;
        }
    }
    if(!found_bsp) { panic("smp: no bootstrap processor found in the discovered cpu list"); }

    size_t array_size = MATH_ALIGN_UP(g_smp_cpu_list_count * sizeof(bootinfo_cpu_t), PTM_PAGE_GRANULARITY);
    g_smp_cpus = (bootinfo_cpu_t*) ((uintptr_t) pmm_alloc(array_size / PTM_PAGE_GRANULARITY) + g_globals_boot_info->hhdm_offset);
    memset(g_smp_cpus, 0, array_size);

    // The bsp has to be the first cpu so the kernel can use the array index as the core id.
    size_t index = 0;
    for(smp_cpu_node_t* node = g_smp_cpu_list; node != nullptr; node = node->next) {
        if(!node->cpu.is_bsp) { continue; }
        g_smp_cpus[index++] = node->cpu;
        break;
    }
    for(smp_cpu_node_t* node = g_smp_cpu_list; node != nullptr; node = node->next) {
        if(node->cpu.is_bsp) { continue; }
        g_smp_cpus[index++] = node->cpu;
    }

    smp_cpu_node_t* node = g_smp_cpu_list;
    while(node != nullptr) {
        smp_cpu_node_t* next = node->next;
        pmm_free((void*) ((uintptr_t) node - g_globals_boot_info->hhdm_offset), 1);
        node = next;
    }

    g_smp_cpu_count = g_smp_cpu_list_count;
    g_smp_cpu_list = nullptr;
    g_smp_cpu_list_tail = nullptr;
    g_smp_cpu_list_count = 0;
}

static uacpi_iteration_decision smp_madt_pass(uacpi_handle handle, struct acpi_entry_hdr* hdr) {
    (void) handle;

#if defined(__x86_64__)
    if(hdr->type == ACPI_MADT_ENTRY_TYPE_LAPIC) {
        struct acpi_madt_lapic* lapic = (struct acpi_madt_lapic*) hdr;
        if((lapic->flags & ACPI_PIC_ENABLED) == 0) { return UACPI_ITERATION_DECISION_CONTINUE; }
        smp_add_cpu(lapic->id);
    }
#elif defined(__riscv) && __riscv_xlen == 64
    if(hdr->type == ACPI_MADT_ENTRY_TYPE_RINTC) {
        struct acpi_madt_rintc* rintc = (struct acpi_madt_rintc*) hdr;
        smp_add_cpu(rintc->hart_id);
    }
#endif

    return UACPI_ITERATION_DECISION_CONTINUE;
}

static void smp_discover_acpi() {
    uacpi_table madt;
    if(uacpi_table_find_by_signature(ACPI_MADT_SIGNATURE, &madt) != UACPI_STATUS_OK) {
        log_print("smp: MADT not found\n");
        return;
    }

    uacpi_for_each_subtable(madt.hdr, sizeof(struct acpi_madt), smp_madt_pass, nullptr);
    uacpi_table_unref(&madt);
}

void smp_discover_cpus() {
    g_smp_cpu_list = nullptr;
    g_smp_cpu_list_tail = nullptr;
    g_smp_cpu_list_count = 0;

    if(g_globals_boot_info->rdsp_physical != 0) {
        smp_discover_acpi();
    } else if(g_globals_boot_info->dtb_physical != 0) {
        arch_smp_discover_dtb();
    } else {
        log_print("smp: no ACPI or device tree available for CPU discovery\n");
    }

    if(g_smp_cpu_list_count == 0) { smp_add_cpu(arch_smp_bsp_id()); }

    smp_collapse_cpu_list();

    log_print("smp: discovered %zu cpu(s)\n", g_smp_cpu_count);
}

void smp_allocate_cpu_local(bootinfo_kernel_info_t* kernel_info) {
    size_t cpu_local_block_size = MATH_ALIGN_UP(g_smp_cpu_count * kernel_info->cpu_local_size, PTM_PAGE_GRANULARITY);
    void* cpu_local_block = (void*) ((uintptr_t) pmm_alloc_ext(cpu_local_block_size / PTM_PAGE_GRANULARITY, PTM_PAGE_GRANULARITY, PMM_MAP_TYPE_USED) + g_globals_boot_info->hhdm_offset);
    memset(cpu_local_block, 0, cpu_local_block_size);

    g_globals_boot_info->cpulocal_start = (uintptr_t) cpu_local_block;
    g_globals_boot_info->cpulocal_size = cpu_local_block_size;
}
