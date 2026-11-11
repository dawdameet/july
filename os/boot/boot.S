.set MULTIBOOT_MAGIC,    0x1BADB002
.set MULTIBOOT_FLAGS,    0x00000003
.set MULTIBOOT_CHECKSUM, -(MULTIBOOT_MAGIC + MULTIBOOT_FLAGS)

.section .multiboot, "a"
.align 4
    .long MULTIBOOT_MAGIC
    .long MULTIBOOT_FLAGS
    .long MULTIBOOT_CHECKSUM

.section .bss
.align 4096
pml4_table:
    .skip 4096
pdpt_table:
    .skip 4096
pd_table_0:
    .skip 4096
pd_table_1:
    .skip 4096
pd_table_2:
    .skip 4096
pd_table_3:
    .skip 4096

.align 16
stack32_bottom:
    .skip 4096
stack32_top:

.align 16
stack64_bottom:
    .skip 65536
stack64_top:

.align 8
mb_magic_saved:
    .long 0
mb_info_saved:
    .long 0

.section .rodata
.align 8
gdt64:
    .quad 0x0000000000000000
    .quad 0x00AF9A000000FFFF /* 0x08: 64-bit Kernel Code */
    .quad 0x00CF92000000FFFF /* 0x10: 64-bit Kernel Data */
gdt64_end:

gdt64_ptr:
    .word gdt64_end - gdt64 - 1
    .quad gdt64

.section .text
.code32
.global _start
_start:
    cli
    movl $stack32_top, %esp
    movl %eax, mb_magic_saved
    movl %ebx, mb_info_saved

    /* Zero 6 page-table pages (pml4 + pdpt + 4 PDs = 6 * 4096 bytes = 6144 dwords) */
    movl $pml4_table, %edi
    xorl %eax, %eax
    movl $6144, %ecx
    rep stosl

    /* pml4[0] -> pdpt */
    movl $pdpt_table, %eax
    orl $0x03, %eax
    movl %eax, pml4_table

    /* pdpt[0..3] -> pd_table_0..3 (identity map full 4 GiB) */
    movl $pd_table_0, %eax
    orl $0x03, %eax
    movl %eax, pdpt_table + 0

    movl $pd_table_1, %eax
    orl $0x03, %eax
    movl %eax, pdpt_table + 8

    movl $pd_table_2, %eax
    orl $0x03, %eax
    movl %eax, pdpt_table + 16

    movl $pd_table_3, %eax
    orl $0x03, %eax
    movl %eax, pdpt_table + 24

    /* Fill 2048 2MiB entries (4 tables * 512 entries) with huge page flags 0x83 */
    movl $pd_table_0, %edi
    xorl %esi, %esi      /* low 32-bit phys addr */
    xorl %edx, %edx      /* high 32-bit phys addr */
    movl $2048, %ecx
.fill_pd:
    movl %esi, %eax
    orl $0x83, %eax      /* Present | RW | PS (2MiB) */
    movl %eax, (%edi)
    movl %edx, 4(%edi)
    addl $0x200000, %esi
    adcl $0, %edx
    addl $8, %edi
    loop .fill_pd

    /* Load CR3 with PML4 */
    movl $pml4_table, %eax
    movl %eax, %cr3

    /* Enable PAE (bit 5) + SSE OSFXSR/OSXMMEXCPT (bits 9, 10) in CR4 */
    movl %cr4, %eax
    orl $((1 << 5) | (1 << 9) | (1 << 10)), %eax
    movl %eax, %cr4

    /* Clear EM (bit 2), set MP (bit 1) in CR0 for SSE */
    movl %cr0, %eax
    andl $~(1 << 2), %eax
    orl $(1 << 1), %eax
    movl %eax, %cr0

    /* Enable Long Mode in IA32_EFER MSR (0xC0000080, bit 8) */
    movl $0xC0000080, %ecx
    rdmsr
    orl $(1 << 8), %eax
    wrmsr

    /* Enable Paging (CR0.PG bit 31) */
    movl %cr0, %eax
    orl $(1 << 31), %eax
    movl %eax, %cr0

    lgdt gdt64_ptr
    ljmp $0x08, $long_mode_entry

.code64
long_mode_entry:
    movw $0x10, %ax
    movw %ax, %ds
    movw %ax, %es
    movw %ax, %fs
    movw %ax, %gs
    movw %ax, %ss

    movabs $stack64_top, %rsp
    movl mb_magic_saved(%rip), %edi
    movl mb_info_saved(%rip), %esi

    .extern kmain
    call kmain

.halt_loop:
    cli
    hlt
    jmp .halt_loop

