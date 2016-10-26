/*
 * Copyright (c) 2013-2014 Wind River Systems, Inc.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

/**
 * @file
 * @brief Common linker sections
 *
 * This script defines the memory location of the various sections that make up
 * a Zephyr Kernel image. This file is used by the linker.
 *
 * This script places the various sections of the image according to what
 * features are enabled by the kernel's configuration options.
 *
 * For a build that does not use the execute in place (XIP) feature, the script
 * generates an image suitable for loading into and executing from RAM by
 * placing all the sections adjacent to each other.  There is also no separate
 * load address for the DATA section which means it doesn't have to be copied
 * into RAM.
 *
 * For builds using XIP, there is a different load memory address (LMA) and
 * virtual memory address (VMA) for the DATA section.  In this case the DATA
 * section is copied into RAM at runtime.
 *
 * When building an XIP image the data section is placed into ROM.  In this
 * case, the LMA is set to __data_rom_start so the data section is concatenated
 * at the end of the RODATA section.  At runtime, the DATA section is copied
 * into the RAM region so it can be accessed with read and write permission.
 *
 * Most symbols defined in the sections below are subject to be referenced in
 * the Zephyr Kernel image. If a symbol is used but not defined the linker will
 * emit an undefined symbol error.
 *
 * Please do not change the order of the section as the nanokernel expects this
 * order when programming the MMU.
 */

#define _LINKER

#define _ASMLANGUAGE
#include <linker-defs.h>
#include <offsets.h>
#include <misc/util.h>
#define MMU_PAGE_SIZE KB(4)

#include <linker-tool.h>

/* SECTIONS definitions */
SECTIONS
	{
	GROUP_START(ROMABLE_REGION)

	_image_rom_start = PHYS_LOAD_ADDR;
	_image_text_start = PHYS_LOAD_ADDR;


	SECTION_PROLOGUE(_TEXT_SECTION_NAME, (OPTIONAL),)
	{
	. = CONFIG_TEXT_SECTION_OFFSET;
	*(.text_start)
	*(".text_start.*")
	*(.text)
	*(".text.*")
	*(.gnu.linkonce.t.*)
	*(.eh_frame)
	*(.init)
	*(.fini)
	*(.eini)
	KEXEC_PGALIGN_PAD(MMU_PAGE_SIZE)
	} GROUP_LINK_IN(ROMABLE_REGION)

	_image_text_end = .;

#ifdef CONFIG_CPLUSPLUS
	SECTION_PROLOGUE(_CTOR_SECTION_NAME, ,)
	{
	/*
	 * The compiler fills the constructor pointers table below, hence symbol
	 * __CTOR_LIST__ must be aligned on 4 byte boundary.
	 * To align with the C++ standard, the first elment of the array
	 * contains the number of actual constructors. The last element is
	 * NULL.
	 */
	. = ALIGN(4);
	__CTOR_LIST__ = .;
	LONG((__CTOR_END__ - __CTOR_LIST__) / 4 - 2)
	KEEP(*(SORT_BY_NAME(".ctors*")))
	LONG(0)
	__CTOR_END__ = .;
	} GROUP_LINK_IN(ROMABLE_REGION)

	SECTION_PROLOGUE(init_array, (OPTIONAL),)
	{
	. = ALIGN(4);
	__init_array_start = .;
	KEEP(*(SORT_BY_NAME(".init_array*")))
	__init_array_end = .;
	} GROUP_LINK_IN(ROMABLE_REGION)
#endif

	SECTION_PROLOGUE(devconfig, (OPTIONAL),)
	{
		__devconfig_start = .;
		*(".devconfig.*")
		KEEP(*(SORT_BY_NAME(".devconfig*")))
		__devconfig_end = .;
	} GROUP_LINK_IN(ROMABLE_REGION)

#ifdef CONFIG_NET_YAIP
	SECTION_PROLOGUE(net_l2, (OPTIONAL),)
	{
		__net_l2_start = .;
		*(".net_l2.init")
		KEEP(*(SORT_BY_NAME(".net_l2.init*")))
		__net_l2_end = .;
	} GROUP_LINK_IN(ROMABLE_REGION)
#endif

	SECTION_PROLOGUE(_RODATA_SECTION_NAME, (OPTIONAL),)
	{
	*(.rodata)
	*(".rodata.*")
	*(.gnu.linkonce.r.*)

	. = ALIGN(8);
	_idt_base_address = .;
	KEEP(*(staticIdt))

#ifndef CONFIG_X86_FIXED_IRQ_MAPPING
	. = ALIGN(4);
	_irq_to_interrupt_vector = .;
	KEEP(*(irq_int_vector_map))
#endif

	KEXEC_PGALIGN_PAD(MMU_PAGE_SIZE)
	} GROUP_LINK_IN(ROMABLE_REGION)

	_image_rom_end = .;
	__data_rom_start = ALIGN(4);		/* XIP imaged DATA ROM start addr */

	GROUP_END(ROMABLE_REGION)

	/* RAM */
	GROUP_START(RAM)

	SECTION_DATA_PROLOGUE(_DATA_SECTION_NAME, (OPTIONAL),)
	{
	KEXEC_PGALIGN_PAD(MMU_PAGE_SIZE)
	_image_ram_start = .;
	__data_ram_start = .;
	*(.top_of_image_ram)
	*(.top_of_image_ram.*)
	*(.data)
	*(".data.*")
	. = ALIGN(4);
	} GROUP_DATA_LINK_IN(RAM, ROM)

	SECTION_DATA_PROLOGUE(initlevel, (OPTIONAL),)
	{
		DEVICE_INIT_SECTIONS()
		KEXEC_PGALIGN_PAD(MMU_PAGE_SIZE)
	} GROUP_DATA_LINK_IN(RAM, ROM)

#ifdef CONFIG_NET_YAIP
	SECTION_DATA_PROLOGUE(net_if, (OPTIONAL),)
	{
		__net_if_start = .;
		*(".net_if.*")
		KEEP(*(SORT_BY_NAME(".net_if*")))
		__net_if_end = .;
	} GROUP_DATA_LINK_IN(RAM, ROM)

	SECTION_DATA_PROLOGUE(net_l2_data, (OPTIONAL),)
	{
		__net_l2_data_start = .;
		*(".net_l2.data")
		KEEP(*(SORT_BY_NAME(".net_l2.data*")))
		__net_l2_data_end = .;
	} GROUP_DATA_LINK_IN(RAM, ROM)
#endif

	SECTION_DATA_PROLOGUE(_k_task_list, (OPTIONAL),)
	{
		_k_task_list_start = .;
			*(._k_task_list.public.*)
			*(._k_task_list.private.*)
		_k_task_list_idle_start = .;
			*(._k_task_list.idle.*)
		KEEP(*(SORT_BY_NAME("._k_task_list*")))
		_k_task_list_end = .;
	} GROUP_DATA_LINK_IN(RAM, ROM)

	SECTION_DATA_PROLOGUE(_k_task_ptr, (OPTIONAL),)
	{
		_k_task_ptr_start = .;
			*(._k_task_ptr.public.*)
			*(._k_task_ptr.private.*)
			*(._k_task_ptr.idle.*)
		KEEP(*(SORT_BY_NAME("._k_task_ptr*")))
		_k_task_ptr_end = .;
	} GROUP_DATA_LINK_IN(RAM, ROM)

	SECTION_DATA_PROLOGUE(_k_pipe_ptr, (OPTIONAL),)
	{
		_k_pipe_ptr_start = .;
			*(._k_pipe_ptr.public.*)
			*(._k_pipe_ptr.private.*)
		KEEP(*(SORT_BY_NAME("._k_pipe_ptr*")))
		_k_pipe_ptr_end = .;
	} GROUP_DATA_LINK_IN(RAM, ROM)

	SECTION_DATA_PROLOGUE(_k_mem_map_ptr, (OPTIONAL),)
	{
		_k_mem_map_ptr_start = .;
			*(._k_mem_map_ptr.public.*)
			*(._k_mem_map_ptr.private.*)
		KEEP(*(SORT_BY_NAME("._k_mem_map_ptr*")))
		_k_mem_map_ptr_end = .;
	} GROUP_DATA_LINK_IN(RAM, ROM)

	SECTION_DATA_PROLOGUE(_k_event_list, (OPTIONAL),)
	{
		_k_event_list_start = .;
			*(._k_event_list.event.*)
		KEEP(*(SORT_BY_NAME("._k_event_list*")))
		_k_event_list_end = .;
	} GROUP_DATA_LINK_IN(RAM, ROM)

	SECTION_DATA_PROLOGUE(_k_memory_pool, (OPTIONAL),)
	{
		*(._k_memory_pool.struct*)
		KEEP(*(SORT_BY_NAME("._k_memory_pool.struct*")))

		_k_mem_pool_start = .;
		*(._k_memory_pool.*)
		KEEP(*(SORT_BY_NAME("._k_memory_pool*")))
		_k_mem_pool_end = .;
	} GROUP_DATA_LINK_IN(RAM, ROM)

	__data_ram_end = .;

	SECTION_PROLOGUE(_BSS_SECTION_NAME, (NOLOAD OPTIONAL),)
	{
	/*
	 * For performance, BSS section is forced to be both 4 byte aligned and
	 * a multiple of 4 bytes.
	 */

	. = ALIGN(4);

	__bss_start = .;

	*(.bss)
	*(".bss.*")
	COMMON_SYMBOLS
	/*
	 * As memory is cleared in words only, it is simpler to ensure the BSS
	 * section ends on a 4 byte boundary. This wastes a maximum of 3 bytes.
	 */
	. = ALIGN(4);
	__bss_end = .;
	KEXEC_PGALIGN_PAD(MMU_PAGE_SIZE)
	} GROUP_LINK_IN(RAM)
#ifdef CONFIG_XIP
	/*
	 * Ensure linker keeps sections in correct order, despite the fact
	 * the previous section specified a load address and this no-load
	 * section doesn't.
	 */
	  GROUP_FOLLOWS_AT(RAM)
#endif

	SECTION_PROLOGUE(_NOINIT_SECTION_NAME, (NOLOAD OPTIONAL),)
	{
	/*
	 * This section is used for non-initialized objects that
	 * will not be cleared during the boot process.
	 */
	*(.noinit)
	*(".noinit.*")

	*(.bottom_of_image_ram)
	*(.bottom_of_image_ram.*)
	} GROUP_LINK_IN(RAM)

	/* Define linker symbols */
	_image_ram_end = .;
	_end = .; /* end of image */

	. = ALIGN(MMU_PAGE_SIZE);

	__bss_num_words	= (__bss_end - __bss_start) >> 2;

	GROUP_END(RAM)

	/* static interrupts */
	SECTION_PROLOGUE(intList, (OPTIONAL),)
	{
	KEEP(*(.spurIsr))
	KEEP(*(.spurNoErrIsr))
	__INT_LIST_START__ = .;
	LONG((__INT_LIST_END__ - __INT_LIST_START__) / __ISR_LIST_SIZEOF)
	KEEP(*(.intList))
	KEEP(*(.gnu.linkonce.intList.*))
	__INT_LIST_END__ = .;
	} > IDT_LIST

	/* verify we don't have rogue .init_<something> initlevel sections */
	SECTION_PROLOGUE(initlevel_error, (OPTIONAL), )
	{
		DEVICE_INIT_UNDEFINED_SECTION()
	}
	ASSERT(SIZEOF(initlevel_error) == 0, "Undefined initialization levels used.")

	}

#ifdef CONFIG_XIP
/*
 * Round up number of words for DATA section to ensure that XIP copies the
 * entire data section. XIP copy is done in words only, so there may be up
 * to 3 extra bytes copied in next section (BSS). At run time, the XIP copy
 * is done first followed by clearing the BSS section.
 */
__data_size = (__data_ram_end - __data_ram_start);
__data_num_words = (__data_size + 3) >> 2;

#endif
