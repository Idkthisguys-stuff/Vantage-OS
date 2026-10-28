/* Source:
        https://rpi4os.com/part1-bootstrapping/
*/

.section ".text.boot"
.global _start

_start:
    // Zero checker for processor ID, else hang
    mrs x1, mpidr_el1
    and x1, x1, #3
    cbz x1, 2f
    
1: // Hang would make an inf loop 
    wfe
    b 1b

2:
    mrs x0, CurrentEL
    lsr x0, x0, #2
    cmp x0, #2
    bne el1_entry

    mov x0, #0x33ff
    msr cptr_el2, x0
    mov x0, #(1 << 31)
    msr hcr_el2, x0
    ldr x0, =0x30D00800
    msr sctlr_el1, x0
    mov x0, #0x3c5
    msr spsr_el2, x0
    ldr x0, =el1_entry
    msr elr_el2, x0
    eret

el1_entry:
    mov x0, #(3 << 20)
    msr cpacr_el1, x0
    isb

    ldr x1, =0x800000
    mov sp, x1

    ldr x1, =__bss_start
    ldr w2, =__bss_size
3:
    cbz w2, 4f
    str xzr, [x1], #8
    sub w2, w2, #1
    cbnz w2, 3b

4:
    ldr x0, =vector_table
    msr vbar_el1, x0
    isb

    bl main
    b 1b

.macro vector target
    .balign 0x80
    b \target
.endm

.balign 0x800
vector_table:
    vector default_handler
    vector default_handler
    vector default_handler
    vector default_handler
    vector default_handler
    vector default_handler
    vector default_handler
    vector default_handler
    vector el0_sync_exception
    vector default_handler
    vector default_handler
    vector default_handler
    vector default_handler
    vector default_handler
    vector default_handler
    vector default_handler

default_handler:
    wfe
    b default_handler

.global el0_sync_exception
el0_sync_exception:
    sub sp, sp, #256
    stp x0, x1, [sp, #0]
    stp x2, x3, [sp, #16]
    stp x4, x5, [sp, #32]
    stp x6, x7, [sp, #48]
    stp x8, x9, [sp, #64]
    stp x10, x11, [sp, #80]
    stp x12, x13, [sp, #96]
    stp x14, x15, [sp, #112]
    stp x16, x17, [sp, #128]
    stp x18, x19, [sp, #144]
    stp x20, x21, [sp, #160]
    stp x22, x23, [sp, #176]
    stp x24, x25, [sp, #192]
    stp x26, x27, [sp, #208]
    stp x28, x29, [sp, #224]
    str x30, [sp, #240]

    mrs x9, esr_el1
    lsr x9, x9, #26
    cmp x9, #0x15
    bne default_handler

    ldr x0, [sp, #64]
    ldr x1, [sp, #0]

    bl handle_sys_call

    str x0, [sp, #0]

    ldr x30, [sp, #240]
    ldp x28, x29, [sp, #224]
    ldp x26, x27, [sp, #208]
    ldp x24, x25, [sp, #192]
    ldp x22, x23, [sp, #176]
    ldp x20, x21, [sp, #160]
    ldp x18, x19, [sp, #144]
    ldp x16, x17, [sp, #128]
    ldp x14, x15, [sp, #112]
    ldp x12, x13, [sp, #96]
    ldp x10, x11, [sp, #80]
    ldp x8, x9, [sp, #64]
    ldp x6, x7, [sp, #48]
    ldp x4, x5, [sp, #32]
    ldp x2, x3, [sp, #16]
    ldp x0, x1, [sp, #0]
    add sp, sp, #256

    eret

.global enter_user_space
enter_user_space:
    msr elr_el1, x0
    mov x2, #0x0
    msr spsr_el1, x2
    msr sp_el0, x1
    eret
    