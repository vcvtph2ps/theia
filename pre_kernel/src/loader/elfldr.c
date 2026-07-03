#include <globals.h>
#include <lib/math.h>
#include <loader/elfldr.h>
#include <log.h>
#include <memory/pmm.h>
#include <memory/ptm.h>
#include <protocol/bootinfo.h>
#include <runtime/mem.h>
#include <stdint.h>

typedef struct {
    unsigned char e_ident[16];
    uint16_t e_type;
    uint16_t e_machine;
    uint32_t e_version;
    uint64_t e_entry;
    uint64_t e_phoff;
    uint64_t e_shoff;
    uint32_t e_flags;
    uint16_t e_ehsize;
    uint16_t e_phentsize;
    uint16_t e_phnum;
    uint16_t e_shentsize;
    uint16_t e_shnum;
    uint16_t e_shstrndx;
} elf64_elf_header_t;

#define ELF_CLASS_IDX 4
#define ELF_CLASS_64_BIT 2

#define ELF_DATA_IDX 5
#define ELF_DATA_2LSB 1

#define ETYPE_REL 1
#define ETYPE_EXEC 2
#define ETYPE_DYN 3

#define EMACHINE_X86_64 62
#define EMACHINE_RISCV 243

#ifdef __ARCH_X86_64__
#define EMACHINE_EXPECTED EMACHINE_X86_64
#elif defined(__ARCH_RISCV64__)
#define EMACHINE_EXPECTED EMACHINE_RISCV
#else
#error "Unknown architecture"
#endif

typedef struct {
    uint32_t p_type;
    uint32_t p_flags;
    uint64_t p_offset;
    uint64_t p_vaddr;
    uint64_t p_paddr;
    uint64_t p_filesz;
    uint64_t p_memsz;
    uint64_t p_align;
} elf64_program_header_t;

typedef struct {
    uint32_t sh_name;
    uint32_t sh_type;
    uint64_t sh_flags;
    uint64_t sh_addr;
    uint64_t sh_offset;
    uint64_t sh_size;
    uint32_t sh_link;
    uint32_t sh_info;
    uint64_t sh_addralign;
    uint64_t sh_entsize;
} elf64_section_header_t;

#define PTYPE_LOAD 1
#define PTYPE_INTERP 3
#define PTYPE_PHDR 6

#define PFLAGS_EXECUTE (1 << 0)
#define PFLAGS_WRITE (1 << 1)
#define PFLAGS_READ (1 << 2)
#define PTYPE_NOTE 4

static bool elf_supported(const elf64_elf_header_t* elf_header) {
    if(!elf_header) { return false; }
    if(elf_header->e_ident[0] != 0x7f || elf_header->e_ident[1] != 'E' || elf_header->e_ident[2] != 'L' || elf_header->e_ident[3] != 'F') { return false; }
    if(elf_header->e_ident[ELF_CLASS_IDX] != ELF_CLASS_64_BIT) { return false; }
    if(elf_header->e_ident[ELF_DATA_IDX] != ELF_DATA_2LSB) { return false; }
    if(elf_header->e_machine != EMACHINE_EXPECTED) { return false; }
    if(elf_header->e_type != ETYPE_EXEC && elf_header->e_type != ETYPE_DYN) { return false; }
    return true;
}

extern uint8_t _binary_kernel_elf_start[]; // NOLINT
extern uint8_t _binary_kernel_elf_end[]; // NOLINT

static void internal_elf_handle_pt_load(elf64_program_header_t* phdr, elfldr_loader_info_t* loader_info) {
    uint64_t flags = 0;

    if(phdr->p_flags & PFLAGS_READ) { flags |= BOOTINFO_SEGMENT_FLAG_READ; }
    if(phdr->p_flags & PFLAGS_WRITE) { flags |= BOOTINFO_SEGMENT_FLAG_WRITE; }
    if(phdr->p_flags & PFLAGS_EXECUTE) { flags |= BOOTINFO_SEGMENT_FLAG_EXECUTE; }

    uintptr_t start_vaddr = MATH_FLOOR(phdr->p_vaddr, PTM_PAGE_GRANULARITY);
    uintptr_t end_vaddr = MATH_ALIGN_UP(phdr->p_vaddr + phdr->p_memsz, PTM_PAGE_GRANULARITY);

    uintptr_t paddr = (uintptr_t) pmm_alloc_ext((end_vaddr - start_vaddr) / PTM_PAGE_GRANULARITY, PTM_PAGE_GRANULARITY, PMM_MAP_TYPE_ALLOCATED);

    for(uintptr_t j = start_vaddr; j < end_vaddr; j += PTM_PAGE_GRANULARITY) {
        uint64_t ptm_flags = 0;
        if(phdr->p_flags & PFLAGS_READ) ptm_flags |= PTM_FLAG_READ;
        if(phdr->p_flags & PFLAGS_WRITE) ptm_flags |= PTM_FLAG_WRITE;
        if(phdr->p_flags & PFLAGS_EXECUTE) ptm_flags |= PTM_FLAG_EXEC;
        ptm_map(j, paddr + (j - start_vaddr), PTM_PAGE_GRANULARITY, ptm_flags);
    }

    memset((void*) (paddr + g_globals_boot_info->hhdm_offset), 0, end_vaddr - start_vaddr);
    memcpy((void*) (paddr + (phdr->p_vaddr - start_vaddr) + g_globals_boot_info->hhdm_offset), (void*) (phdr->p_offset + (uintptr_t) _binary_kernel_elf_start), phdr->p_filesz);

    size_t segment_idx = loader_info->segment_count++;
    loader_info->segments[segment_idx].paddr = paddr + (phdr->p_vaddr - start_vaddr);
    loader_info->segments[segment_idx].vaddr = phdr->p_vaddr;
    loader_info->segments[segment_idx].size = phdr->p_memsz;
    loader_info->segments[segment_idx].flags = flags;

    if(loader_info->kernel_base > phdr->p_vaddr) { loader_info->kernel_base = phdr->p_vaddr; }
}

static bool internal_elf_load_image(elfldr_loader_info_t* loader_info) {
    // cache phdrs so we don't have to read them multiple times
    elf64_elf_header_t* elf_header = (elf64_elf_header_t*) _binary_kernel_elf_start;

    elf64_program_header_t* phdrs = (elf64_program_header_t*) (_binary_kernel_elf_start + elf_header->e_phoff);
    size_t load_count = 0;
    for(size_t i = 0; i < elf_header->e_phnum; i++) {
        if(phdrs[i].p_type == PTYPE_LOAD) load_count++;
    }

    loader_info->segments = (bootinfo_segment_t*) ((uintptr_t) pmm_alloc(MATH_ALIGN_UP(load_count * sizeof(bootinfo_segment_t), PTM_PAGE_GRANULARITY) / PTM_PAGE_GRANULARITY) + g_globals_boot_info->hhdm_offset);
    loader_info->segment_count = 0;

    for(size_t i = 0; i < elf_header->e_phnum; i++) {
        log_print("phdr[%ld].p_type = 0x%x\n", i, phdrs[i].p_type);
        log_print("phdr[%ld].p_vaddr = 0x%lx, p_memsz = 0x%lx, p_filesz = 0x%lx\n", i, phdrs[i].p_vaddr, phdrs[i].p_memsz, phdrs[i].p_filesz);
    }

    // fill out loader info
    loader_info->entry_point = (bootinfo_kernel_entry_point_t) elf_header->e_entry;
    log_print("elf entry point: 0x%lx\n", (uintptr_t) elf_header->e_entry);

    // load phdrs
    for(size_t i = 0; i < elf_header->e_phnum; i++) {
        if(phdrs[i].p_type != PTYPE_LOAD) { continue; }
        log_print("Loading segment %zu: vaddr=0x%lx, size=%zu\n", i, phdrs[i].p_vaddr, phdrs[i].p_filesz);
        internal_elf_handle_pt_load(&phdrs[i], loader_info);
    }

    if(elf_header->e_shoff == 0 || elf_header->e_shstrndx == 0) {
        log_print("elf: no section headers or shstrtab; cannot locate prekernel_boot_info\n");
        return false;
    }

    elf64_section_header_t* shdrs = (elf64_section_header_t*) (_binary_kernel_elf_start + elf_header->e_shoff);
    const char* shstrtab = (const char*) (_binary_kernel_elf_start + shdrs[elf_header->e_shstrndx].sh_offset);

    loader_info->kernel_info = nullptr;
    for(size_t i = 0; i < elf_header->e_shnum; i++) {
        const char* name = shstrtab + shdrs[i].sh_name;
        if(memcmp(name, "prekernel_boot_info", sizeof("prekernel_boot_info")) != 0) { continue; }

        uint64_t sh_addr = shdrs[i].sh_addr;
        for(size_t j = 0; j < loader_info->segment_count; j++) {
            bootinfo_segment_t* seg = &loader_info->segments[j];
            if(sh_addr >= seg->vaddr && sh_addr < seg->vaddr + seg->size) {
                loader_info->kernel_info = (bootinfo_kernel_info_t*) ((uintptr_t) seg->paddr + (sh_addr - seg->vaddr) + g_globals_boot_info->hhdm_offset);
                break;
            }
        }
        break;
    }

    if(loader_info->kernel_info == nullptr) {
        log_print("elf: failed to locate prekernel_boot_info section in kernel image\n");
        return false;
    }

    log_print("elf: kernel_info at %p (pagedb_entry_size=%zu, cpu_local_size=%zu)\n", (void*) loader_info->kernel_info, loader_info->kernel_info->pagedb_entry_size, loader_info->kernel_info->cpu_local_size);

    return true;
}


bool elfldr_load_kernel(elfldr_loader_info_t* out_info) {
    elf64_elf_header_t* elf_data = (elf64_elf_header_t*) _binary_kernel_elf_start;
    if(!elf_supported(elf_data)) {
        log_print("Unsupported elf file\n");
        return false;
    }

    memset(out_info, 0, sizeof(elfldr_loader_info_t));
    out_info->kernel_base = UINTPTR_MAX;
    if(!internal_elf_load_image(out_info)) {
        log_print("Failed to load elf image\n");
        return false;
    }

    return true;
}
