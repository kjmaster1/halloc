#include "../halloc.h"
#include <stdio.h>
#include <string.h>
#include <stdint.h>

/* =========================================================================
 * Minimal test framework
 * ========================================================================= */

static int tests_run    = 0;
static int tests_passed = 0;
static int tests_failed = 0;

#define ASSERT(cond, msg)                                           \
    do {                                                            \
        tests_run++;                                                \
        if (cond) {                                                 \
            tests_passed++;                                         \
            printf("  [PASS] %s\n", msg);                          \
        } else {                                                    \
            tests_failed++;                                         \
            printf("  [FAIL] %s (line %d)\n", msg, __LINE__);      \
        }                                                           \
        fflush(stdout);                                             \
    } while (0)

#define TEST(name)  printf("\n--- %s ---\n", name); fflush(stdout)
#define SUMMARY()                                                   \
    printf("\n========================================\n");          \
    printf("  %d tests run | %d passed | %d failed\n",             \
           tests_run, tests_passed, tests_failed);                  \
    printf("========================================\n");            \
    return tests_failed > 0 ? 1 : 0

/* =========================================================================
 * Alignment check helper
 * ========================================================================= */

static int is_aligned(void* ptr) {
    return ((uintptr_t)ptr % 16) == 0;
}

/* =========================================================================
 * Tests
 * ========================================================================= */

int main(void) {
    halloc_init();

    /* ---- Basic allocation ---- */
    TEST("Basic allocation");

    void* p1 = halloc(8);
    ASSERT(p1 != NULL, "halloc(8) returns non-null");
    ASSERT(is_aligned(p1), "halloc(8) is 16-byte aligned");

    void* p2 = halloc(100);
    ASSERT(p2 != NULL, "halloc(100) returns non-null");
    ASSERT(is_aligned(p2), "halloc(100) is 16-byte aligned");
    ASSERT(p2 != p1, "Two allocations return different pointers");

    void* p3 = halloc(1024);
    ASSERT(p3 != NULL, "halloc(1024) returns non-null");
    ASSERT(is_aligned(p3), "halloc(1024) is 16-byte aligned");

    /* ---- Zero size ---- */
    TEST("Zero size allocation");
    void* p_zero = halloc(0);
    ASSERT(p_zero == NULL, "halloc(0) returns NULL");

    /* ---- Write and read back ---- */
    TEST("Write and read back");
    int* arr = (int*)halloc(10 * sizeof(int));
    ASSERT(arr != NULL, "halloc array non-null");
    for (int i = 0; i < 10; i++) arr[i] = i * i;
    int correct = 1;
    for (int i = 0; i < 10; i++) {
        if (arr[i] != i * i) { correct = 0; break; }
    }
    ASSERT(correct, "Values written and read back correctly");

    /* ---- hfree and reuse ---- */
    TEST("Free and reuse memory");
    void* a = halloc(64);
    ASSERT(a != NULL, "First halloc(64) non-null");
    hfree(a);
    void* b = halloc(64);
    ASSERT(b != NULL, "halloc(64) after free non-null");
    ASSERT(b == a, "Freed memory is reused");

    /* ---- hcalloc ---- */
    TEST("hcalloc zero-initialisation");
    halloc_reset();
    int* carr = (int*)hcalloc(10, sizeof(int));
    ASSERT(carr != NULL, "hcalloc returns non-null");
    ASSERT(is_aligned(carr), "hcalloc is 16-byte aligned");
    int all_zero = 1;
    for (int i = 0; i < 10; i++) {
        if (carr[i] != 0) { all_zero = 0; break; }
    }
    ASSERT(all_zero, "hcalloc memory is zero-initialised");

    /* ---- hrealloc ---- */
    TEST("hrealloc");
    halloc_reset();
    char* str = (char*)halloc(8);
    ASSERT(str != NULL, "Initial halloc for realloc non-null");
    memcpy(str, "hello", 6);

    char* str2 = (char*)hrealloc(str, 128);
    ASSERT(str2 != NULL, "hrealloc returns non-null");
    ASSERT(is_aligned(str2), "hrealloc result is 16-byte aligned");
    ASSERT(strcmp(str2, "hello") == 0, "hrealloc preserves existing data");
    halloc_reset();
    void* r_null = hrealloc(NULL, 64);
    ASSERT(r_null != NULL, "hrealloc(NULL, size) acts like halloc");
    hfree(r_null);

    /* ---- Coalescing ---- */
    TEST("Coalescing — adjacent free blocks merge");
    halloc_reset();

    void* c1 = halloc(64);
    void* c2 = halloc(64);
    void* c3 = halloc(64);
    ASSERT(c1 && c2 && c3, "Three 64-byte allocations succeed");

    /* Free middle block, then neighbours — should coalesce */
    hfree(c2);
    hfree(c1);
    hfree(c3);
    void* big = halloc(192);
    ASSERT(big != NULL, "Can allocate combined size after coalescing");
    hfree(big);

    /* ---- Alignment stress ---- */
    TEST("Alignment — all sizes from 1 to 256");
    halloc_reset();
    int align_ok = 1;
    void* ptrs[256];
    for (int i = 1; i <= 256; i++) {
        ptrs[i-1] = halloc((size_t)i);
        if (!ptrs[i-1] || !is_aligned(ptrs[i-1])) {
            align_ok = 0;
            break;
        }
    }
    ASSERT(align_ok, "All allocations sizes 1-256 are 16-byte aligned");
    for (int i = 0; i < 256; i++) hfree(ptrs[i]);

    /* ---- Stress test ---- */
    TEST("Stress — 10000 random allocations and frees");
    halloc_init();

    #define STRESS_COUNT 10000
    #define STRESS_MAX   512
    void*  stress_ptrs[STRESS_COUNT];
    size_t stress_sizes[STRESS_COUNT];
    int    stress_ok = 1;

    /* Allocate */
    for (int i = 0; i < STRESS_COUNT; i++) {
        size_t sz = (size_t)((i * 37 + 13) % STRESS_MAX) + 1;
        stress_sizes[i] = sz;
        stress_ptrs[i] = halloc(sz);
        if (!stress_ptrs[i] || !is_aligned(stress_ptrs[i])) {
            stress_ok = 0;
            break;
        }
        /* Write a pattern to detect corruption */
        memset(stress_ptrs[i], (int)(i & 0xFF), sz);
    }
    ASSERT(stress_ok, "10000 allocations all succeed and are aligned");

    /* Verify patterns — no neighbouring block stomped on our data */
    int no_corruption = 1;
    for (int i = 0; i < STRESS_COUNT; i++) {
        if (!stress_ptrs[i]) continue;
        unsigned char* p = (unsigned char*)stress_ptrs[i];
        unsigned char expected = (unsigned char)(i & 0xFF);
        for (size_t j = 0; j < stress_sizes[i]; j++) {
            if (p[j] != expected) {
                no_corruption = 0;
                break;
            }
        }
        if (!no_corruption) break;
    }
    ASSERT(no_corruption, "No memory corruption detected across 10000 blocks");

    /* Free every other block to create fragmentation, then free the rest */
    for (int i = 0; i < STRESS_COUNT; i += 2) hfree(stress_ptrs[i]);
    for (int i = 1; i < STRESS_COUNT; i += 2) hfree(stress_ptrs[i]);
    ASSERT(1, "10000 frees complete without crash");

    /* After freeing everything, the heap should be back to one big block */
    void* reclaimed = halloc(1024 * 1024);
    ASSERT(reclaimed != NULL, "1MB allocation succeeds after full stress cycle");
    hfree(reclaimed);

    SUMMARY();
}