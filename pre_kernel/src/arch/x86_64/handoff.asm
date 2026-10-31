global x86_64_kernel_handoff
global x86_64_kernel_handoff_end

extern g_x86_64_cpu_la57_support

; rdi = entry point
; rsi = stack pointer
; rdx = top level page tables
; rcx = boot info pointer
x86_64_kernel_handoff:
    cli

    ; test for cpu la57 support
    mov rax, g_x86_64_cpu_la57_support
    movzx ebx, byte [rax]
    test ebx, ebx
    jz .nola57

    ; test if we have la57 enabled
    mov rax, cr4
    test rax, (1 << 12)
    jnz .nola57

    ; drop to protected mode to enable la57... thanks intel
    lea rax, [rel .entry_protected]
    lea rbx, [rel .entry_long]
    lea rbp, [rel .compat_stack]

    ; far return to code32:.entry_protected
    push qword 0x08
    push rax
    retfq

bits 32
.entry_protected:
    ; @note: we load the data32 segment while paging is still enabled because the gdt lives at a high virtual address
    mov ax, 0x10
    mov ds, ax
    mov ss, ax

    ; clear cr0.PG
    mov eax, cr0
    btr eax, 31
    mov cr0, eax

    ; set cr4.LA57
    mov eax, cr4
    or eax, (1 << 12)
    mov cr4, eax

    ; load the new page tables before enabling paging again
    mov cr3, edx

    ; set cr0.PG
    mov eax, cr0
    or eax, 0x80000000
    mov cr0, eax

    ; far return to code64:.entry_long using the trampoline scratch stack
    mov esp, ebp
    push dword 0x18
    push ebx
    retf
bits 64
.entry_long:
    ; set segments to data64
    mov ax, 0x20
    mov ds, rax
    mov ss, rax
.nola57:
    mov cr3, rdx ; load the page tables

    ; setup stack
    xor rbp, rbp
    mov rsp, rsi
    push qword 0
    push qword 0

    ; push entry to stack for ret
    push rdi

    mov rdi, rcx

    xor rbx, rbx
    xor rcx, rcx
    xor rdx, rdx
    xor r8, r8
    xor r9, r9
    xor r10, r10
    xor r11, r11
    xor r12, r12
    xor r13, r13
    xor r14, r14
    xor r15, r15
    cld

    push qword 0x2
    popfq

    ret

    ; scratch stack used while running in 32-bit protected mode
    times 32 db 0
.compat_stack:
x86_64_kernel_handoff_end:
