#pragma once

#include <stddef.h>  /* size_t */

/*
 * halloc — a custom heap allocator with boundary tags and coalescing
 *
 * Block layout:
 *
 *   [HEADER 8 bytes][USER DATA (aligned to 16 bytes)][FOOTER 8 bytes]
 *
 * The header and footer store the block size and a free/used flag
 * packed into the lowest bit of the size field (size is always even
 * so the lowest bit is always available).
 *
 * FREE bit: 1 = free, 0 = allocated
 */

/* Minimum allocation size in bytes */
#define HALLOC_MIN_ALLOC    16

/* Alignment requirement — all user pointers are 16-byte aligned */
#define HALLOC_ALIGNMENT    16

/* Size of the heap we request from the OS upfront (16MB) */
#define HALLOC_HEAP_SIZE    (16 * 1024 * 1024)

/* Public API */
void  halloc_init(void);
void* halloc(size_t size);
void  hfree(void* ptr);
void* hcalloc(size_t n, size_t size);
void* hrealloc(void* ptr, size_t size);

/* Debug — prints every block in the heap with its size and status */
void  halloc_dump(void);

/* Reset the allocator — for testing only */
void halloc_reset(void);