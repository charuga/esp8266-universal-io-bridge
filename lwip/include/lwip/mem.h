/*
 * Copyright (c) 2001-2004 Swedish Institute of Computer Science.
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without modification,
 * are permitted provided that the following conditions are met:
 *
 * 1. Redistributions of source code must retain the above copyright notice,
 *    this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright notice,
 *    this list of conditions and the following disclaimer in the documentation
 *    and/or other materials provided with the distribution.
 * 3. The name of the author may not be used to endorse or promote products
 *    derived from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR ``AS IS'' AND ANY EXPRESS OR IMPLIED
 * WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT
 * SHALL THE AUTHOR BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
 * EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT
 * OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING
 * IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY
 * OF SUCH DAMAGE.
 *
 * This file is part of the lwIP TCP/IP stack.
 *
 * Author: Adam Dunkels <adam@sics.se>
 *
 */
#ifndef __LWIP_MEM_H__
#define __LWIP_MEM_H__

#include "lwip/opt.h"

#ifdef __cplusplus
extern "C" {
#endif

#if MEM_LIBC_MALLOC

#include <stddef.h> /* for size_t */

typedef size_t mem_size_t;

/* aliases for C library malloc() */
#define mem_init()

static __attribute__((always_inline)) inline void mem_free(void *chunk)
{
	extern void vPortFree (void *chunk, const char *file, unsigned int line);

	vPortFree(chunk, "", 0);
}

static __attribute__((always_inline)) inline void *mem_malloc(mem_size_t size)
{
	extern void *pvPortMalloc(size_t size, const char *file, unsigned int line, unsigned char use_iram);

	return(pvPortMalloc(size, "", 0, false));
}

static __attribute__((always_inline)) inline void *mem_calloc(mem_size_t amount, mem_size_t size)
{
	extern void *pvPortCalloc(size_t count, size_t size, const char *file, unsigned int line);

	return(pvPortCalloc(amount, size, "", 0));
}

static __attribute__((always_inline)) inline void *mem_realloc(void *previous, mem_size_t amount)
{
	extern void *pvPortRealloc(void *chunk, size_t size, const char *file, unsigned int line);

	return(pvPortRealloc(previous, amount, "", 0));
}

static __attribute__((always_inline)) inline void *mem_zalloc(mem_size_t size)
{
	extern void *pvPortZalloc(size_t size, const char *file, unsigned int line);

	return(pvPortZalloc(size, "", 0));
}

#undef os_zalloc
static __attribute__((always_inline)) inline void *os_zalloc(size_t size)
{
	return(mem_zalloc(size));
}

#undef os_free
static __attribute__((always_inline)) inline void os_free(void *chunk)
{
	mem_free(chunk);
}

/* Since there is no C library allocation function to shrink memory without
   moving it, define this to nothing. */
#ifndef mem_trim
#define mem_trim(mem, size) (mem)
#endif
#else /* MEM_LIBC_MALLOC */

/* MEM_SIZE would have to be aligned, but using 64000 here instead of
 * 65535 leaves some room for alignment...
 */
#if MEM_SIZE > 64000l
typedef u32_t mem_size_t;
#define MEM_SIZE_F U32_F
#else
typedef u16_t mem_size_t;
#define MEM_SIZE_F U16_F
#endif /* MEM_SIZE > 64000 */

#if MEM_USE_POOLS
/** mem_init is not used when using pools instead of a heap */
#define mem_init()
/** mem_trim is not used when using pools instead of a heap:
    we can't free part of a pool element and don't want to copy the rest */
#define mem_trim(mem, size) (mem)
#else /* MEM_USE_POOLS */
/* lwIP alternative malloc */
void  mem_init(void);
void *mem_trim(void *mem, mem_size_t size);
#endif /* MEM_USE_POOLS */
void *mem_malloc(mem_size_t size);
void *mem_calloc(mem_size_t count, mem_size_t size);
void  mem_free(void *mem);
#endif /* MEM_LIBC_MALLOC */

/** Calculate memory size for an aligned buffer - returns the next highest
 * multiple of MEM_ALIGNMENT (e.g. LWIP_MEM_ALIGN_SIZE(3) and
 * LWIP_MEM_ALIGN_SIZE(4) will both yield 4 for MEM_ALIGNMENT == 4).
 */
#ifndef LWIP_MEM_ALIGN_SIZE
#define LWIP_MEM_ALIGN_SIZE(size) (((size) + MEM_ALIGNMENT - 1) & ~(MEM_ALIGNMENT-1))
#endif

/** Calculate safe memory size for an aligned buffer when using an unaligned
 * type as storage. This includes a safety-margin on (MEM_ALIGNMENT - 1) at the
 * start (e.g. if buffer is u8_t[] and actual data will be u32_t*)
 */
#ifndef LWIP_MEM_ALIGN_BUFFER
#define LWIP_MEM_ALIGN_BUFFER(size) (((size) + MEM_ALIGNMENT - 1))
#endif

/** Align a memory pointer to the alignment defined by MEM_ALIGNMENT
 * so that ADDR % MEM_ALIGNMENT == 0
 */
#ifndef LWIP_MEM_ALIGN
#define LWIP_MEM_ALIGN(addr) ((void *)(((mem_ptr_t)(addr) + MEM_ALIGNMENT - 1) & ~(mem_ptr_t)(MEM_ALIGNMENT-1)))
#endif

#ifdef __cplusplus
}
#endif

#endif /* __LWIP_MEM_H__ */
