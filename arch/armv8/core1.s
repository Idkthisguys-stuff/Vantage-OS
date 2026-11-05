/* Pi 4 firmware spin-table slot for core 1; cores 2 and 3 are to be done soon. */
.equ CORE1_RELEASE, 0xe0
.equ SYSTEM_TIMER_LOW, 0xfe003004
.equ CORE1_TIMEOUT_US, 1000000
.equ CORE1_WAIT_LIMIT, 0x4000000

.section ".text"
.global core1_start
.type core1_start, %function
core1_start:
    /* This one-shot handshake runs at EL1 before the MMU or data cache is on. */
    mrs x0, sctlr_el1
    tbnz x0, #0, .Lfailed
    tbnz x0, #2, .Lfailed

    ldr x1, =core1_ready
    str wzr, [x1]
    dsb sy

    ldr x2, =core1_entry
    mov x3, #CORE1_RELEASE
    str x2, [x3]
    dsb sy
    sev

    ldr x3, =SYSTEM_TIMER_LOW
    ldr w4, [x3]
    ldr w5, =CORE1_TIMEOUT_US
    ldr w2, =CORE1_WAIT_LIMIT
.Lwait:
    ldr w0, [x1]
    cbnz w0, .Lready
    ldr w6, [x3]
    sub w6, w6, w4
    cmp w6, w5
    b.hs .Lfailed
    subs w2, w2, #1
    b.ne .Lwait
.Lfailed:
    mov w0, #0
    ret
.Lready:
    dsb sy
    mov w0, #1
    ret
.size core1_start, . - core1_start

.type core1_entry, %function
core1_entry:
    /* Firmware must enter with translation and data cache disabled. */
    mrs x0, CurrentEL
    cmp x0, #8
    b.eq .Lcheck_el2
    cmp x0, #4
    b.ne .Lpark
    mrs x0, sctlr_el1
    b .Lcheck_control
.Lcheck_el2:
    mrs x0, sctlr_el2
.Lcheck_control:
    tbnz x0, #0, .Lpark
    tbnz x0, #2, .Lpark
    mrs x0, mpidr_el1
    and x0, x0, #3
    cmp x0, #1
    b.ne .Lpark
    msr daifset, #0xf
    ldr x0, =__core1_stack_top
    mov sp, x0
    /* Exercise the reserved stack before reporting that the entry was reached. */
    stp xzr, xzr, [sp, #-16]!
    ldp x2, x3, [sp], #16
    ldr x0, =core1_ready
    mov w1, #1
    str w1, [x0]
    dsb sy
    sev
.Lpark:
    wfe
    b .Lpark
.size core1_entry, . - core1_entry

.section ".bss"
.balign 64
core1_ready:
    .skip 64
