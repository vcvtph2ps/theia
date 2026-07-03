#include <globals.h>
#include <log.h>
#include <stddef.h>
#include <stdint.h>
#include <uacpi/acpi.h>
#include <uacpi/tables.h>
#include <uacpi/types.h>
#include <uacpi/uacpi.h>
void parse_isa_string(const char* isa_str);

#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wmissing-prototypes"

size_t arch_acpi_detect_mmu_levels() {
    if(!g_globals_boot_info->rdsp_physical) { return 0; }
    uacpi_table rhct_table;
    if(uacpi_table_find_by_signature(ACPI_RHCT_SIGNATURE, &rhct_table) != UACPI_STATUS_OK) {
        log_print("ACPI: RHCT table not found\n");
        return 0;
    }

    size_t mmu_type = 0xffff;
    struct acpi_rhct* rhct = (struct acpi_rhct*) rhct_table.hdr;
    struct rhct_node {
        uint16_t type;
        uint16_t length;
    };

    size_t base = (size_t) rhct->nodes_offset;
    for(size_t i = 0; i < rhct->node_count; i++) {
        struct rhct_node* hdr = (struct rhct_node*) ((uintptr_t) rhct + base);
        base += hdr->length;
        if(hdr->type == ACPI_RHCT_ENTRY_TYPE_MMU) {
            struct acpi_rhct_mmu* mmu_subtable = (struct acpi_rhct_mmu*) hdr;
            mmu_type = mmu_subtable->type;
            break;
        }
    }

    if(mmu_type == ACPI_RHCT_MMU_TYPE_SV57) {
        log_print("RHCT: mmu-type = \"SV57\"\n");
        return 5;
    }
    if(mmu_type == ACPI_RHCT_MMU_TYPE_SV48) {
        log_print("RHCT: mmu-type = \"SV48\"\n");
        return 4;
    }
    if(mmu_type == ACPI_RHCT_MMU_TYPE_SV39) {
        log_print("RHCT: mmu-type = \"SV39\"\n");
        return 3;
    }
    if(mmu_type == 0xffff) {
        log_print("RHCT: mmu-type not found\n");
        return 0;
    }
    log_print("RHCT: unknown mmu-type 0x%lx\n", mmu_type);
    return 0;
}

bool arch_acpi_parse_extentions() {
    if(!g_globals_boot_info->rdsp_physical) { return 0; }
    uacpi_table rhct_table;
    if(uacpi_table_find_by_signature(ACPI_RHCT_SIGNATURE, &rhct_table) != UACPI_STATUS_OK) {
        log_print("ACPI: RHCT table not found\n");
        return false;
    }

    struct acpi_rhct* rhct = (struct acpi_rhct*) rhct_table.hdr;
    struct rhct_node {
        uint16_t type;
        uint16_t length;
    };

    struct acpi_rhct_isa_string* isa_string_subtable = nullptr;

    size_t base = (size_t) rhct->nodes_offset;
    for(size_t i = 0; i < rhct->node_count; i++) {
        struct rhct_node* hdr = (struct rhct_node*) ((uintptr_t) rhct + base);
        base += hdr->length;
        if(hdr->type == ACPI_RHCT_ENTRY_TYPE_ISA_STRING) {
            isa_string_subtable = (struct acpi_rhct_isa_string*) hdr;
            break;
        }
    }

    parse_isa_string((const char*) isa_string_subtable->isa);

    return true;
}

#pragma clang diagnostic pop
