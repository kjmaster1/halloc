#include "halloc.h"

#include <stdio.h>
#include <string.h>
#include <stdint.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <sys/mman.h>
#endif

/* =========================================================================
 * INTERNAL TYPES AND CONSTANTS
 * ========================================================================= */

/*
 * A block_header sits at the start of every block.
 * 'size_and_flags' encodes both the block size and the free bit:
 *
 *   bits 63..1  — block size (always a multiple of 2)
 *   bit  0      — 1 if free, 0 if allocated
 *
 * We can pack them together because size is always even (16-byte aligned),
 * so bit 0 is always unused by the size value.
 */
typedef struct {
    size_t size_and_flags;
    size_t _pad;            /* pad to 16 bytes so user data is always aligned */
} block_header;

/* A block_footer is identical in layout — placed at the end of every block */
typedef struct {
    size_t size_and_flags;
    size_t _pad;
} block_footer;

/* Overhead = header + footer */
#define BLOCK_OVERHEAD  (sizeof(block_header) + sizeof(block_footer))

/* Flag masks */
#define FREE_BIT        ((size_t)1)
#define SIZE_MASK       (~FREE_BIT)

/* =========================================================================
 * BIT MANIPULATION HELPERS
 * ========================================================================= */

static inline size_t get_size(block_header* h) {
    return h->size_and_flags & SIZE_MASK;
}

static inline int is_free(block_header* h) {
    return (int)(h->size_and_flags & FREE_BIT);
}

static inline void set_header(block_header* h, size_t size, int free) {
    h->size_and_flags = size | (free ? FREE_BIT : 0);
    h->_pad = 0;
}

static inline void set_footer(block_header* h, size_t size, int free) {
    block_footer* f = (block_footer*)((char*)h + sizeof(block_header) + size);
    f->size_and_flags = size | (free ? FREE_BIT : 0);
    f->_pad = 0;
}

/*
 * Given a header pointer, return a pointer to the user data region.
 * The user data starts immediately after the header.
 */
static inline void* header_to_user(block_header* h) {
    return (void*)((char*)h + sizeof(block_header));
}

/*
 * Given a user pointer (returned by halloc), get back the header.
 */
static inline block_header* user_to_header(void* ptr) {
    return (block_header*)((char*)ptr - sizeof(block_header));
}

/*
 * Given a header, find the header of the next block in memory.
 * Next block starts at: header + sizeof(header) + size + sizeof(footer)
 */
static inline block_header* next_block(block_header* h) {
    return (block_header*)((char*)h + sizeof(block_header) + get_size(h) + sizeof(block_footer));
}

/*
 * Given a header, find the header of the previous block.
 * We read the previous block's footer (which is immediately before us)
 * to find its size, then step back by that amount.
 */
static inline block_header* prev_block(block_header* h) {
    block_footer* prev_foot = (block_footer*)((char*)h - sizeof(block_footer));
    size_t prev_size = prev_foot->size_and_flags & SIZE_MASK;
    return (block_header*)((char*)h - sizeof(block_header) - prev_size - sizeof(block_footer));
}

/* =========================================================================
 * HEAP STATE
 * ========================================================================= */

static void*         heap_start  = NULL;  /* base of the heap region     */
static size_t        heap_size   = 0;     /* total bytes allocated from OS*/
static block_header* free_list   = NULL;  /* head of the free list        */

/* Sentinel blocks — a prologue at the start and epilogue at the end
 * of the heap. They're always marked as allocated so coalescing never
 * tries to walk off the end of the heap. */
static block_header* prologue  = NULL;
static block_header* epilogue  = NULL;

/* =========================================================================
 * ALIGNMENT
 * ========================================================================= */

/*
 * Round 'size' up to the nearest multiple of HALLOC_ALIGNMENT.
 * Example: align(17) with HALLOC_ALIGNMENT=16 → 32
 */
static inline size_t align(size_t size) {
    return (size + HALLOC_ALIGNMENT - 1) & ~(HALLOC_ALIGNMENT - 1);
}

/* =========================================================================
 * FREE LIST OPERATIONS
 * ========================================================================= */

/*
 * We use an explicit doubly-linked free list stored inside the free blocks
 * themselves. Since a free block's user data region is unused, we repurpose
 * the first 16 bytes to store prev/next pointers.
 *
 * Layout of a FREE block's user data region:
 *   [prev pointer (8 bytes)][next pointer (8 bytes)][...unused...]
 */
typedef struct free_node {
    struct free_node* prev;
    struct free_node* next;
} free_node;

static inline free_node* block_to_node(block_header* h) {
    return (free_node*)header_to_user(h);
}

static inline block_header* node_to_block(free_node* n) {
    return user_to_header((void*)n);
}

static void free_list_insert(block_header* h) {
    free_node* node = block_to_node(h);
    node->next = free_list ? block_to_node(free_list) : NULL;
    node->prev = NULL;
    if (free_list) {
        block_to_node(free_list)->prev = node;
    }
    free_list = h;
}

static void free_list_remove(block_header* h) {
    free_node* node = block_to_node(h);
    if (node->prev) {
        node->prev->next = node->next;
    } else {
        free_list = node->next ? node_to_block(node->next) : NULL;
    }
    if (node->next) {
        node->next->prev = node->prev;
    }
    node->prev = node->next = NULL;
}

/* =========================================================================
 * COALESCING
 * ========================================================================= */

/*
 * Try to merge a free block with its neighbours.
 * Returns the (possibly new) header of the coalesced block.
 *
 * Four cases:
 *   1. Both neighbours allocated — no merge
 *   2. Next block free — merge with next
 *   3. Prev block free — merge with prev
 *   4. Both free — merge all three into one
 */
static block_header* coalesce(block_header* h) {
    block_header* next = next_block(h);
    block_header* prev = (h == prologue) ? NULL : prev_block(h);

    int next_free = (next != epilogue) && is_free(next);
    int prev_free = (prev != NULL) && (prev != prologue) && is_free(prev);

    /* Capture sizes before any free list removal corrupts free_node data */
    size_t size      = get_size(h);
    size_t next_size = next_free ? get_size(next) : 0;
    size_t prev_size = prev_free ? get_size(prev) : 0;

    if (!prev_free && !next_free) {
        return h;
    }

    if (!prev_free && next_free) {
        /* Case 2 — merge with next */
        free_list_remove(h);
        free_list_remove(next);
        size += BLOCK_OVERHEAD + next_size;
        set_header(h, size, 1);
        set_footer(h, size, 1);
        free_list_insert(h);
        return h;
    }

    if (prev_free && !next_free) {
        /* Case 3 — merge with prev */
        free_list_remove(h);
        free_list_remove(prev);
        size += BLOCK_OVERHEAD + prev_size;
        set_header(prev, size, 1);
        set_footer(prev, size, 1);
        free_list_insert(prev);
        return prev;
    }

    /* Case 4 — merge with both */
    free_list_remove(h);
    free_list_remove(prev);
    free_list_remove(next);
    size += 2 * BLOCK_OVERHEAD + prev_size + next_size;
    set_header(prev, size, 1);
    set_footer(prev, size, 1);
    free_list_insert(prev);
    return prev;
}

/* =========================================================================
 * SPLITTING
 * ========================================================================= */

/*
 * If a free block is large enough to be split — i.e. after carving out
 * 'needed' bytes there's enough left for a minimum-sized free block —
 * split it and return the remainder to the free list.
 */
static void split(block_header* h, size_t needed) {
    size_t total = get_size(h);
    size_t remainder = total - needed - BLOCK_OVERHEAD;

    if (remainder < HALLOC_MIN_ALLOC) {
        /* Not worth splitting — internal fragmentation but unavoidable */
        return;
    }

    /* Shrink this block to 'needed' */
    set_header(h, needed, 0);
    set_footer(h, needed, 0);

    /* Create a new free block for the remainder */
    block_header* new_block = next_block(h);
    set_header(new_block, remainder, 1);
    set_footer(new_block, remainder, 1);
    free_list_insert(new_block);
}

/* =========================================================================
 * PUBLIC API
 * ========================================================================= */

void halloc_init(void) {
    if (heap_start != NULL) return;  /* already initialised */

#ifdef _WIN32
    heap_start = VirtualAlloc(NULL, HALLOC_HEAP_SIZE,
                              MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!heap_start) {
        fprintf(stderr, "halloc: VirtualAlloc failed\n");
        return;
    }
#else
    heap_start = mmap(NULL, HALLOC_HEAP_SIZE,
                      PROT_READ | PROT_WRITE,
                      MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (heap_start == MAP_FAILED) {
        fprintf(stderr, "halloc: mmap failed\n");
        return;
    }
#endif

    heap_size = HALLOC_HEAP_SIZE;

    /*
     * Lay out the heap:
     *
     * [prologue header][prologue footer][main block header][...data...][main block footer][epilogue header]
     *
     * Prologue and epilogue are zero-size allocated blocks that act as
     * sentinels — coalescing stops when it hits them.
     */
    char* p = (char*)heap_start;

    /* Prologue — zero size, marked allocated */
    prologue = (block_header*)p;
    set_header(prologue, 0, 0);
    set_footer(prologue, 0, 0);
    p += sizeof(block_header) + sizeof(block_footer);

    /* One large free block covering all usable heap space */
    size_t usable = heap_size
                  - 2 * BLOCK_OVERHEAD   /* prologue + epilogue */
                  - BLOCK_OVERHEAD;      /* the main block's own overhead */

    /* Align usable size down */
    usable = usable & ~(HALLOC_ALIGNMENT - 1);

    block_header* main_block = (block_header*)p;
    set_header(main_block, usable, 1);
    set_footer(main_block, usable, 1);
    free_list_insert(main_block);
    p += sizeof(block_header) + usable + sizeof(block_footer);

    /* Epilogue — zero size, marked allocated */
    epilogue = (block_header*)p;
    set_header(epilogue, 0, 0);
    /* No footer on epilogue — nothing comes after it */
}

void halloc_reset(void) {
#ifdef _WIN32
    if (heap_start) {
        VirtualFree(heap_start, 0, MEM_RELEASE);
    }
#else
    if (heap_start) {
        munmap(heap_start, heap_size);
    }
#endif
    heap_start = NULL;
    heap_size  = 0;
    free_list  = NULL;
    prologue   = NULL;
    epilogue   = NULL;
    halloc_init();
}

void* halloc(size_t size) {
    if (size == 0) return NULL;
    if (!heap_start) halloc_init();

    size_t needed = align(size);
    if (needed < HALLOC_MIN_ALLOC) needed = HALLOC_MIN_ALLOC;

    block_header* current = free_list;
    while (current != NULL) {
        if (get_size(current) >= needed) {
            free_list_remove(current);
            split(current, needed);
            set_header(current, get_size(current), 0);
            set_footer(current, get_size(current), 0);
            return header_to_user(current);
        }
        free_node* node = block_to_node(current);
        current = node->next ? node_to_block(node->next) : NULL;
    }

    fprintf(stderr, "halloc: out of memory\n");
    return NULL;
}

void hfree(void* ptr) {
    if (!ptr) return;

    block_header* h = user_to_header(ptr);
    size_t size = get_size(h);

    set_header(h, size, 1);
    set_footer(h, size, 1);
    free_list_insert(h);
    coalesce(h);
}

void* hcalloc(size_t n, size_t size) {
    size_t total = n * size;
    void* ptr = halloc(total);
    if (ptr) memset(ptr, 0, total);
    return ptr;
}

void* hrealloc(void* ptr, size_t size) {
    if (!ptr) return halloc(size);
    if (size == 0) { hfree(ptr); return NULL; }

    block_header* h = user_to_header(ptr);
    size_t current_size = get_size(h);
    size_t needed = align(size);

    /* Already large enough — no-op */
    if (current_size >= needed) return ptr;

    /* Allocate new block, copy, free old */
    void* new_ptr = halloc(size);
    if (!new_ptr) return NULL;
    memcpy(new_ptr, ptr, current_size);
    hfree(ptr);
    return new_ptr;
}

void halloc_dump(void) {
    printf("\n=== halloc heap dump ===\n");
    if (!heap_start || !prologue) {
        printf("  (not initialised)\n");
        printf("========================\n\n");
        return;
    }

    char* heap_end = (char*)heap_start + heap_size;

    block_header* current = (block_header*)((char*)prologue
                          + sizeof(block_header)
                          + sizeof(block_footer));

    int block_num = 0;
    while ((char*)current < heap_end - sizeof(block_header)) {
        size_t size = get_size(current);

        if (size == 0) break;

        if (size > heap_size) break;

        printf("  block %3d | addr %p | size %6zu | %s\n",
               block_num++,
               (void*)current,
               size,
               is_free(current) ? "FREE" : "USED");

        current = next_block(current);
    }
    printf("========================\n\n");
}