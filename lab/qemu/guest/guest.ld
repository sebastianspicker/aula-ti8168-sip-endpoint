/* SPDX-License-Identifier: GPL-3.0-or-later */
OUTPUT_FORMAT("elf32-littlearm")
OUTPUT_ARCH(arm)
ENTRY(_start)
PHDRS { ram PT_LOAD FLAGS(7); }
SECTIONS
{
    . = 0x80000000;
    .vectors : { KEEP(*(.vectors)) } :ram
    .text : { *(.text*) } :ram
    .rodata : { *(.rodata*) } :ram
    .data : { *(.data*) } :ram
    .bss (NOLOAD) : { *(.bss*) *(COMMON) } :ram
    __guest_end = .;
    .ARM.attributes 0 : { *(.ARM.attributes) }
    /DISCARD/ : { *(.comment) }
}
ASSERT(SIZEOF(.vectors) == 32, "ARM vector table must have eight entries")
ASSERT(__guest_end < 0x80010000, "Synthetic probe must fit within its first 64 KiB RAM window")
