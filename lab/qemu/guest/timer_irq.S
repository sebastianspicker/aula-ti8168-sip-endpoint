/* SPDX-License-Identifier: GPL-3.0-or-later
 * Project-owned synthetic CPU/UART/timer/IRQ probe. No vendor firmware.
 * Public DM816x MMIO/IRQ numbers; exercises the maintained QEMU model only.
 */
.syntax unified
.cpu cortex-a8
.arm

.equ UART1,       0x48020000
.equ INTC,        0x48200000
.equ TIMER1,      0x4802e000
.equ TIMER_IRQ,   67
.equ OVERFLOW,    2

.section .vectors, "ax", %progbits
.balign 32
.global _start
_start:
    b reset
    b failed
    b failed
    b failed
    b failed
    b failed
    b irq_entry
    b failed

.section .text, "ax", %progbits
reset:
    cpsid if
    /* ARM vectors at VBAR, MMU/caches off, little endian ARM exceptions. */
    mrc p15, 0, r0, c1, c0, 0
    bic r0, r0, #1
    bic r0, r0, #4
    bic r0, r0, #0x1000
    bic r0, r0, #0x2000
    bic r0, r0, #0x02000000
    bic r0, r0, #0x40000000
    mcr p15, 0, r0, c1, c0, 0
    ldr r0, =_start
    mcr p15, 0, r0, c12, c0, 0
    isb
    cps #0x12
    ldr sp, =irq_stack_top
    cps #0x13
    ldr sp, =svc_stack_top
    ldr r0, =irq_count
    mov r1, #0
    str r1, [r0]

    /* 16550-compatible UART1 at 4-byte register spacing, divisor 1, 8N1. */
    ldr r0, =UART1
    mov r1, #0
    str r1, [r0, #4]
    mov r1, #0x80
    str r1, [r0, #12]
    mov r1, #1
    str r1, [r0]
    mov r1, #0
    str r1, [r0, #4]
    mov r1, #3
    str r1, [r0, #12]
    ldr r0, =boot_message
    bl uart_puts

    /* INTC reset; IRQ67 routed to IRQ at priority zero, unmask bank2 bit3. */
    ldr r0, =INTC
    mov r1, #2
    str r1, [r0, #0x10]
    mov r1, #0
    str r1, [r0, #0x20c]
    mov r1, #0xff
    str r1, [r0, #0x68]
    mov r1, #8
    str r1, [r0, #0xc8]

    /* PRCM: select CLKIN (27 MHz), then enable TIMER1 before its MMIO. */
    ldr r0, =0x48180390
    mov r1, #2
    str r1, [r0]
    ldr r0, =0x48181570
    str r1, [r0]
    dsb sy

    /* DMTIMER TIOCP_CFG.SOFTRESET is bit0 (bit1 is EMUFREE). */
    ldr r0, =TIMER1
    mov r1, #1
    str r1, [r0, #0x10]
    mov r1, #OVERFLOW
    str r1, [r0, #0x28]
    str r1, [r0, #0x2c]
    dsb sy
    cpsie i
    bl arm_timer
    mov r2, #1
    bl await_irq
    ldr r0, =first_message
    bl uart_puts

    /* A second delivery needs the first source cleared and INTC acknowledged. */
    bl arm_timer
    mov r2, #2
    bl await_irq
    cpsid i
    ldr r0, =TIMER1
    ldr r1, [r0, #0x28]
    cmp r1, #0
    bne failed
    ldr r1, [r0, #0x38]
    tst r1, #1
    bne failed
    ldr r0, =INTC
    ldr r1, [r0, #0xd8]
    tst r1, #8
    bne failed
    ldr r0, =second_message
    bl uart_puts
    ldr r0, =passed_message
    bl uart_puts
    ldr r1, =exit_success
    b semihost_exit

arm_timer:
    ldr r0, =TIMER1
    ldr r1, =0xffff8000
    str r1, [r0, #0x3c]
    mov r1, #1
    str r1, [r0, #0x38]
    dsb sy
    bx lr

await_irq:
    /* Busy wait permits virtual time and IRQ delivery without a WFI race. */
    ldr r0, =irq_count
1:  ldr r1, [r0]
    cmp r1, r2
    bne 1b
    bx lr

irq_entry:
    sub lr, lr, #4
    stmdb sp!, {r0-r12, lr}
    ldr r0, =INTC
    ldr r1, [r0, #0x40]
    cmp r1, #TIMER_IRQ
    bne failed
    ldr r0, =TIMER1
    ldr r1, [r0, #0x28]
    cmp r1, #OVERFLOW
    bne failed
    /* One-shot overflow must have stopped the counter. */
    ldr r1, [r0, #0x38]
    tst r1, #1
    bne failed
    mov r1, #OVERFLOW
    str r1, [r0, #0x28]
    ldr r1, [r0, #0x28]
    cmp r1, #0
    bne failed
    dsb sy
    ldr r0, =INTC
    mov r1, #1
    str r1, [r0, #0x48]
    dsb sy
    ldr r0, =irq_count
    ldr r1, [r0]
    add r1, r1, #1
    str r1, [r0]
    ldmia sp!, {r0-r12, pc}^

uart_puts:
    ldr r3, =UART1
1:  ldrb r1, [r0], #1
    cmp r1, #0
    bxeq lr
2:  ldr r2, [r3, #20]
    tst r2, #0x20
    beq 2b
    str r1, [r3]
    b 1b

failed:
    cpsid if
    ldr r0, =failed_message
    bl uart_puts
    ldr r1, =exit_failure
semihost_exit:
    mov r0, #0x20              /* SYS_EXIT_EXTENDED: [reason, exit_status]. */
    svc #0x123456
1:  b 1b                       /* Host timeout fails if semihosting is absent. */

.ltorg
.section .rodata, "a", %progbits
boot_message:   .asciz "AULA:GUEST:CPU_UART\n"
first_message:  .asciz "AULA:GUEST:TIMER_IRQ_1_CLEARED_ACKED\n"
second_message: .asciz "AULA:GUEST:TIMER_IRQ_2_CLEARED_ACKED\n"
passed_message: .asciz "AULA:GUEST:PASS\n"
failed_message: .asciz "AULA:GUEST:FAIL\n"
.balign 4
exit_success: .word 0x20026, 0
exit_failure: .word 0x20026, 1

.section .bss, "aw", %nobits
.balign 8
irq_count: .space 4
.balign 8
.space 1024
irq_stack_top:
.space 1024
svc_stack_top:
