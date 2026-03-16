#include "../halloc.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* =========================================================================
 * Timing
 * ========================================================================= */

#ifdef _WIN32
#include <windows.h>
static double now_seconds(void) {
    LARGE_INTEGER freq, count;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&count);
    return (double)count.QuadPart / (double)freq.QuadPart;
}
#else
static double now_seconds(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec * 1e-9;
}
#endif

/* =========================================================================
 * Benchmark runner
 * ========================================================================= */

#define ITERATIONS 50000

static void print_result(const char* name, double elapsed, int iterations) {
    double ops_per_sec = iterations / elapsed;
    double ns_per_op   = (elapsed / iterations) * 1e9;
    printf("  %-35s %10.0f ops/sec  %6.1f ns/op\n",
           name, ops_per_sec, ns_per_op);
}

/* =========================================================================
 * Benchmarks
 * ========================================================================= */

static void bench_small_alloc_free(void) {
    halloc_reset();
    void* ptrs[ITERATIONS];

    double start = now_seconds();
    for (int i = 0; i < ITERATIONS; i++) {
        ptrs[i] = halloc(64);
    }
    double mid = now_seconds();
    for (int i = 0; i < ITERATIONS; i++) {
        hfree(ptrs[i]);
    }
    double end = now_seconds();

    print_result("halloc(64)", mid - start, ITERATIONS);
    print_result("hfree(64)", end - mid, ITERATIONS);
}

static void bench_system_small_alloc_free(void) {
    void* ptrs[ITERATIONS];

    double start = now_seconds();
    for (int i = 0; i < ITERATIONS; i++) {
        ptrs[i] = malloc(64);
    }
    double mid = now_seconds();
    for (int i = 0; i < ITERATIONS; i++) {
        free(ptrs[i]);
    }
    double end = now_seconds();

    print_result("malloc(64)", mid - start, ITERATIONS);
    print_result("free(64)", end - mid, ITERATIONS);
}

static void bench_mixed_sizes(void) {
    halloc_reset();
    static const size_t sizes[] = {8, 16, 32, 64, 128, 256, 512, 1024};
    const int n_sizes = 8;
    void* ptrs[ITERATIONS];

    double start = now_seconds();
    for (int i = 0; i < ITERATIONS; i++) {
        ptrs[i] = halloc(sizes[i % n_sizes]);
    }
    for (int i = 0; i < ITERATIONS; i++) {
        hfree(ptrs[i]);
    }
    double end = now_seconds();

    print_result("halloc+hfree mixed sizes", end - start, ITERATIONS * 2);
}

static void bench_system_mixed_sizes(void) {
    static const size_t sizes[] = {8, 16, 32, 64, 128, 256, 512, 1024};
    const int n_sizes = 8;
    void* ptrs[ITERATIONS];

    double start = now_seconds();
    for (int i = 0; i < ITERATIONS; i++) {
        ptrs[i] = malloc(sizes[i % n_sizes]);
    }
    for (int i = 0; i < ITERATIONS; i++) {
        free(ptrs[i]);
    }
    double end = now_seconds();

    print_result("malloc+free mixed sizes", end - start, ITERATIONS * 2);
}

static void bench_interleaved(void) {
    halloc_reset();
    void* ptrs[ITERATIONS];
    memset(ptrs, 0, sizeof(ptrs));

    double start = now_seconds();
    for (int i = 0; i < ITERATIONS; i++) {
        ptrs[i] = halloc(64);
        if (i > 0 && i % 2 == 0) {
            hfree(ptrs[i - 1]);
            ptrs[i - 1] = NULL;
        }
    }
    for (int i = 0; i < ITERATIONS; i++) {
        if (ptrs[i]) hfree(ptrs[i]);
    }
    double end = now_seconds();

    print_result("halloc+hfree interleaved", end - start, ITERATIONS);
}

static void bench_system_interleaved(void) {
    void* ptrs[ITERATIONS];
    memset(ptrs, 0, sizeof(ptrs));

    double start = now_seconds();
    for (int i = 0; i < ITERATIONS; i++) {
        ptrs[i] = malloc(64);
        if (i > 0 && i % 2 == 0) {
            free(ptrs[i - 1]);
            ptrs[i - 1] = NULL;
        }
    }
    for (int i = 0; i < ITERATIONS; i++) {
        if (ptrs[i]) free(ptrs[i]);
    }
    double end = now_seconds();

    print_result("malloc+free interleaved", end - start, ITERATIONS);
}

/* =========================================================================
 * Main
 * ========================================================================= */

int main(void) {
    printf("halloc benchmark — %d iterations per test\n\n", ITERATIONS);

    printf("[ Small allocations (64 bytes) ]\n");
    bench_small_alloc_free();
    bench_system_small_alloc_free();

    printf("\n[ Mixed sizes (8 - 1024 bytes) ]\n");
    bench_mixed_sizes();
    bench_system_mixed_sizes();

    printf("\n[ Interleaved alloc/free ]\n");
    bench_interleaved();
    bench_system_interleaved();

    printf("\nDone.\n");
    return 0;
}