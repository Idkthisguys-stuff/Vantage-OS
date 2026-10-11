/* Source:   
        https://rpi4os.com/part1-bootstrapping/
*/

.section ".text.boot"
.global _start // Execution starts here

_start:
    // Zero checker for processor ID, else hang
    mrs x1, mpidr_el1
    and x1, x1, #3
    cbz x1, 2f
    
1:    // Hang would make an inf loop 
    wfe
    b 1b
2: // Main core
    // Set stack below code
    ldr x1, = 0x800000
    mov sp, x1

    // Clean bss
    ldr x1, = __bss_start
    ldr w2, = __bss_size
3:  cbz w2, 4f // Quit loop if zero
    str xzr, [x1], #8
    sub w2, w2, #1
    cbnz w2, 3b // Loop if non-zero

// Jump to main in C (to make sure it doesn't return)
4:  bl main
    // If it DOES return, halt the master core
    b 1b
