/**
 * @file
 * @brief ARM Cortex-M4 GCC specific floating point register macros
 */

/*
 * Copyright (c) 2016, Wind River Systems, Inc.
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

#ifndef _FLOAT_REGS_ARM_GCC_H
#define _FLOAT_REGS_ARM_GCC_H

#if !defined(__GNUC__) || !defined(CONFIG_CPU_CORTEX_M4)
#error __FILE__ goes only with Cortex-M4 GCC
#endif

#include <toolchain.h>
#include "float_context.h"

/**
 *
 * @brief Load all floating point registers
 *
 * This function loads ALL floating point registers pointed to by @a regs.
 * It is expected that a subsequent call to _store_all_float_registers()
 * will be issued to dump the floating point registers to memory.
 *
 * The format/organization of 'struct fp_register_set'; the generic C test
 * code (main.c) merely treat the register set as an array of bytes.
 *
 * The only requirement is that the arch specific implementations of
 * _load_all_float_registers() and _store_all_float_registers() agree
 * on the format.
 *
 * @return N/A
 */

static inline void _load_all_float_registers(struct fp_register_set *regs)
{
	__asm__ volatile (
		"vldmia %0, {s0-s15};\n\t"
		"vldmia %1, {s16-s31};\n\t"
		:: "r" (&regs->fp_volatile), "r" (&regs->fp_non_volatile)
		);
}

/**
 *
 * @brief Dump all floating point registers to memory
 *
 * This function stores ALL floating point registers to the memory buffer
 * specified by @a regs. It is expected that a previous invocation of
 * _load_all_float_registers() occurred to load all the floating point
 * registers from a memory buffer.
 *
 * @return N/A
 */

static inline void _store_all_float_registers(struct fp_register_set *regs)
{
	__asm__ volatile (
		"vstmia %0, {s0-s15};\n\t"
		"vstmia %1, {s16-s31};\n\t"
		:: "r" (&regs->fp_volatile), "r" (&regs->fp_non_volatile)
		: "memory"
		);
}

/**
 *
 * @brief Load then dump all float registers to memory
 *
 * This function loads ALL floating point registers from the memory buffer
 * specified by @a regs, and then stores them back to that buffer.
 *
 * This routine is called by a high priority thread prior to calling a primitive
 * that pends and triggers a co-operative context switch to a low priority
 * thread.
 *
 * @return N/A
 */

static inline void _load_then_store_all_float_registers(struct fp_register_set *regs)
{
	_load_all_float_registers(regs);
	_store_all_float_registers(regs);
}
#endif /* _FLOAT_REGS_ARM_GCC_H */
