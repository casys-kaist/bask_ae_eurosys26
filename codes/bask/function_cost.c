#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <x86intrin.h>
#include <xxhash.h>

#define COMPARE_SIZE 4096  // 4KB page size
#define ITERATIONS 1000
#define CACHE_FLUSH_SIZE 64 * 1024 * 1024  // 64MB to force L3 eviction

void flush_cache(unsigned char *flush_buffer, size_t size) {
    memset(flush_buffer, 0, size); // Overwrite to force eviction
    for (size_t i = 0; i < size; i += 64) {
        _mm_clflush(&flush_buffer[i]);  // Flush each cache line
    }
    asm volatile("mfence" ::: "memory");  // Ensure completion
}

long measure_memcmp(unsigned char *buffer1, unsigned char *buffer2, unsigned char *flush_buffer, int flush) {
    struct timespec start, end;
    long total_time = 0;

    for (int i = 0; i < ITERATIONS; i++) {
        if (flush) flush_cache(flush_buffer, CACHE_FLUSH_SIZE);

        clock_gettime(CLOCK_MONOTONIC, &start);
        volatile int result = memcmp(buffer1, buffer2, COMPARE_SIZE);
        clock_gettime(CLOCK_MONOTONIC, &end);

        total_time += (end.tv_sec * 1e9 + end.tv_nsec) - (start.tv_sec * 1e9 + start.tv_nsec);
    }
    return total_time / ITERATIONS;
}

long measure_xxhash(unsigned char *buffer, unsigned char *flush_buffer, int flush) {
    struct timespec start, end;
    long total_time = 0;

    for (int i = 0; i < ITERATIONS; i++) {
        if (flush) flush_cache(flush_buffer, CACHE_FLUSH_SIZE);

        clock_gettime(CLOCK_MONOTONIC, &start);
        volatile XXH64_hash_t hash = XXH64(buffer, COMPARE_SIZE, 0);
        clock_gettime(CLOCK_MONOTONIC, &end);

        total_time += (end.tv_sec * 1e9 + end.tv_nsec) - (start.tv_sec * 1e9 + start.tv_nsec);
    }
    return total_time / ITERATIONS;
}

int main() {
    // Allocate memory
    unsigned char *buffer1 = aligned_alloc(64, COMPARE_SIZE);
    unsigned char *buffer2 = aligned_alloc(64, COMPARE_SIZE);
    unsigned char *flush_buffer = malloc(CACHE_FLUSH_SIZE);

    if (!buffer1 || !buffer2 || !flush_buffer) {
        perror("Memory allocation failed");
        return 1;
    }

    // Initialize buffers
    memset(buffer1, 'A', COMPARE_SIZE);
    memset(buffer2, 'A', COMPARE_SIZE);

    // Introduce differences across multiple cache lines
    for (size_t i = 0; i < COMPARE_SIZE; i += 512) {
        buffer2[i] = 'B';
    }

    // Measure performance
    long memcmp_time_flush = measure_memcmp(buffer1, buffer2, flush_buffer, 1);
    long memcmp_time_no_flush = measure_memcmp(buffer1, buffer2, flush_buffer, 0);
    long xxhash_time_flush = measure_xxhash(buffer1, flush_buffer, 1);
    long xxhash_time_no_flush = measure_xxhash(buffer1, flush_buffer, 0);

    // Print results in nanoseconds
    printf("memcmp (4KB, cache flushed):     %ld ns\n", memcmp_time_flush);
    printf("memcmp (4KB, no cache flush):    %ld ns\n", memcmp_time_no_flush);
    printf("xxHash (4KB, cache flushed):     %ld ns\n", xxhash_time_flush);
    printf("xxHash (4KB, no cache flush):    %ld ns\n", xxhash_time_no_flush);

    // Free memory
    free(buffer1);
    free(buffer2);
    free(flush_buffer);

    return 0;
}
