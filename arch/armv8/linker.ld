SECTIONS {
    . = 0x80000; /* Kernel load address */
    .text : {
        KEEP(*(.text.boot))
        *(.text .text* .gnu.linkonce.t*)
    }
    .rodata : {
        *(.rodata .rodata.* .gnu.linkonce.r*)
    }
    . = ALIGN(0x200000);
    .data : {
        *(.data .data.*)
    }
    .bss (NOLOAD) : {
        . = ALIGN(16);
        __bss_start = .;
        *(.bss .bss.*)
        *(COMMON)
        . = ALIGN(16);
        . += 0x4000; /* Privileged 16 KiB stack, outside user-writable memory. */
        __kernel_stack_top = .;
        . += 0x1000; /* Core 1 has its own privileged 4 KiB stack. */
        __core1_stack_top = .;
        . += 0x1000; /* Core 2 has its own privileged 4 KiB stack. */
        __core2_stack_top = .;
        . += 0x1000; /* Core 3 has its own privileged 4 KiB stack. */
        __core3_stack_top = .;
    }
    .user.bss ALIGN(0x200000) (NOLOAD) : {
        __user_start = .;
        *(.user.bss .user.bss.*)
        . = ALIGN(0x200000);
        __user_end = .;
    }
    /* MMU-off workers and the cached primary only share this Device block
     * during the transition to the common translation tables. */
    .smp.boot ALIGN(0x200000) (NOLOAD) : {
        __smp_boot_start = .;
        *(.smp.boot)
        . = ALIGN(0x200000);
        __smp_boot_end = .;
    }
    /* Boot clears kernel/user BSS, padding and the boot gate together. */
    __bss_end = .;
    _end = .;

    /DISCARD/ : {
        *(.comment)
        *(.gnu)
        *(.note)
        *(.eh_frame*)
    }
}

__bss_size = (__bss_end - __bss_start)>>3;
