#include <vantage/arch/trap.h>
#include <vantage/arch/irq.h>
#include <vantage/kernel/process.h>
#include <vantage/kernel/syscall.h>
#include <vantage/kernel/task_port.h>
#include <vantage/platform/pi4/io.h>

typedef char frame_size_must_match_assembly[(sizeof(TrapFrame) == 800) ? 1 : -1];
typedef char frame_x30_must_match[(__builtin_offsetof(TrapFrame, x[30]) == 240) ? 1 : -1];
typedef char frame_sp_must_match[(__builtin_offsetof(TrapFrame, sp_el0) == 248) ? 1 : -1];
typedef char frame_elr_must_match[(__builtin_offsetof(TrapFrame, elr) == 256) ? 1 : -1];
typedef char frame_fpcr_must_match[(__builtin_offsetof(TrapFrame, fpcr) == 272) ? 1 : -1];
typedef char frame_v_must_match[(__builtin_offsetof(TrapFrame, v) == 288) ? 1 : -1];

#define SYNDROME_CLASS_SHIFT 26
#define CLASS_UNKNOWN 0x00U
#define CLASS_SYSTEM_REGISTER 0x18U
#define CLASS_SVC64 0x15U
#define CLASS_INSTRUCTION_ABORT_EL0 0x20U
#define CLASS_DATA_ABORT_EL0 0x24U

void trap_unhandled_el1(void);

void trap_sync_el0(TrapFrame* frame) {
    uint64_t syndrome;
    uint64_t far;

    __asm__ volatile("mrs %0, esr_el1" : "=r"(syndrome));
    __asm__ volatile("mrs %0, far_el1" : "=r"(far));

    unsigned int class =
        (unsigned int)((syndrome >> SYNDROME_CLASS_SHIFT) & 0x3fU);

    static uint32_t first_trap = 1;

    if (first_trap) {
        first_trap = 0;

        uart_writeText("\n[TRAP] EL0 exception reached\n");

        uart_writeText("[TRAP] EC: ");
        uart_writeHex(class);

        uart_writeText("[TRAP] ESR: ");
        uart_writeHex((uint32_t)syndrome);

        uart_writeText("[TRAP] FAR: ");
        uart_writeHex((uint32_t)far);

        uart_writeText("[TRAP] ELR high: ");
        uart_writeHex((uint32_t)(frame->elr >> 32));

        uart_writeText("[TRAP] ELR low: ");
        uart_writeHex((uint32_t)frame->elr);

        uart_writeText("\n");
    }

    task_trap_boundary();

    if (class == CLASS_SVC64) {
        uart_writeText("[TRAP] SVC received. ID: ");
        uart_writeHex((uint32_t)frame->x[8]);
        uart_writeText("\n");

        uart_writeText("[TRAP] Dispatching syscall...\n");

        uint64_t result =
            handle_sys_call(frame->x[8], frame->x[0]);

        uart_writeText("[TRAP] Syscall returned\n");

        frame->x[0] = result;

        task_trap_boundary();
        return;
    }

    process_fault(
        class == CLASS_DATA_ABORT_EL0 ? PROCESS_FAULT_DATA : class == CLASS_INSTRUCTION_ABORT_EL0 ? PROCESS_FAULT_INSTRUCTION : class == CLASS_UNKNOWN || class == CLASS_SYSTEM_REGISTER ? PROCESS_FAULT_UNDEFINED : PROCESS_FAULT_OTHER
    );

    task_port_halt("[ERR] EL0 exception outside a process; halted\n");
}

void trap_irq(TrapFrame* frame) {
    (void)frame;
    /* The interrupt is completed on this core before the task can migrate. */
    if (irq_service() == IRQ_TIMER) task_tick();
    task_trap_boundary();
}

void trap_unhandled_el1(void) {
    uint64_t esr, elr, far, spsr;
    __asm__ volatile("mrs %0, esr_el1" : "=r"(esr));
    __asm__ volatile("mrs %0, elr_el1" : "=r"(elr));
    __asm__ volatile("mrs %0, far_el1" : "=r"(far));
    __asm__ volatile("mrs %0, spsr_el1" : "=r"(spsr));

    uart_writeText("\n[ERR] Unhandled EL1 exception!\n");

    uart_writeText("[ERR] ESR: ");
    uart_writeHex((uint32_t)esr);

    uart_writeText("[ERR] ELR high: ");
    uart_writeHex((uint32_t)(elr >> 32));

    uart_writeText("[ERR] ELR low: ");
    uart_writeHex((uint32_t)elr);

    uart_writeText("[ERR] FAR: ");
    uart_writeHex((uint32_t)far);

    uart_writeText("[ERR] SPSR: ");
    uart_writeHex((uint32_t)spsr);

    uart_writeText("\n");
}

void trap_serror_el0(TrapFrame* frame) {
    uint64_t syndrome;
    __asm__ volatile("mrs %0, esr_el1" : "=r"(syndrome));

    uart_writeText("\n[ERR] EL0 SError received!\n");

    uart_writeText("[ERR] ESR: ");
    uart_writeHex((uint32_t)syndrome);
    uart_writeText("[ERR] ELR high: ");
    uart_writeHex((uint32_t)(frame->elr >> 32));
    uart_writeText("[ERR] ELR low: ");
    uart_writeHex((uint32_t)frame->elr);
    uart_writeText("[ERR] SPSR: ");
    uart_writeHex((uint32_t)frame->spsr);
    uart_writeText("[ERR] User SP high: ");
    uart_writeHex((uint32_t)(frame->sp_el0 >> 32));
    uart_writeText("[ERR] User SP low: ");
    uart_writeHex((uint32_t)frame->sp_el0);
    uart_writeText("\n");

    task_port_halt("[ERR] Fatal EL0 SError; halted\n");
}