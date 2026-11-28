// SPDX-License-Identifier: MIT
// Benchmark harness for comparing the ARM64 execution engines:
//   * qemu-system-aarch64 -M virt -cpu cortex-a76 -kernel bench.a64.elf
//   * xlt (the prototype AArch64->x86_64 JIT), same ELF
//   * native x86_64 (-DNATIVE) for the host reference point
//
// Everything is fixed-width and uses indices instead of pointers, so the
// aarch64 and x86_64 builds compute the same result for the same input.

#include <stdint.h>

#ifdef NATIVE
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#else
#define ITERS_ALU  (30u * 1000u * 1000u)
#endif

#ifndef ITERS_ALU
#define ITERS_ALU  (30u * 1000u * 1000u)
#endif
#ifndef ITERS_HASH
#define ITERS_HASH (1u * 1000u * 1000u)
#endif
#ifndef ITERS_PTR
#define ITERS_PTR  (8u * 1000u * 1000u)
#endif
#ifndef ITERS_CALL
#define ITERS_CALL (8u * 1000u * 1000u)
#endif
#ifndef MEM_COPIES
#define MEM_COPIES 16
#endif
#define MEM_SIZE   (4u * 1024u * 1024u)
#define HASH_BITS  20u
#define HASH_SIZE  (1u << HASH_BITS)
#define NODES      (1u << 20)

static volatile uint64_t g_sink;

static inline uint64_t rdcycle(void)
{
    uint64_t v;
#ifdef NATIVE
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    v = (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
#else
    __asm__ volatile("mrs %0, cntvct_el0" : "=r"(v));
#endif
    return v;
}

#ifdef NATIVE
static void out(const char *s) { fputs(s, stdout); fflush(stdout); }
static void out_hex(uint64_t v) { printf("0x%016llx", (unsigned long long)v); }
static void out_dec(uint64_t v) { printf("%llu", (unsigned long long)v); }
#else
static void semi_call(long op, const void *arg)
{
    register long x0 __asm__("x0") = op;
    register const void *x1 __asm__("x1") = arg;
    __asm__ volatile("hlt #0xf000" : : "r"(x0), "r"(x1) : "memory");
}
static void out(const char *s) { semi_call(0x04, s); }
static void out_hex(uint64_t v)
{
    static const char hex[] = "0123456789abcdef";
    char b[19];
    b[0] = '0'; b[1] = 'x';
    for (int i = 0; i < 16; i++) b[2 + i] = hex[(v >> (60 - 4 * i)) & 15];
    b[18] = 0;
    out(b);
}
static void out_dec(uint64_t v)
{
    char b[24];
    int i = 23;
    b[i] = 0;
    do { b[--i] = (char)('0' + v % 10); v /= 10; } while (v);
    out(&b[i]);
}
#endif

static uint64_t xs64(uint64_t *s)
{
    uint64_t x = *s;
    x ^= x << 13;
    x ^= x >> 7;
    x ^= x << 17;
    *s = x;
    return x;
}

/* 1. ALU: dependent 64-bit arithmetic, shifts, multiplies, branches. */
static uint64_t bench_alu(void)
{
    uint64_t x = 0x243f6a8885a308d3ull, h = 0;
    for (uint32_t i = 0; i < ITERS_ALU; i++) {
        x ^= x << 13; x ^= x >> 7; x ^= x << 17;
        h += x;
        h ^= h >> 29;
        h = (h << 9) | (h >> 55);
        h += (x * 0x9e3779b97f4a7c15ull);
        if (x & 1) h -= 0x123456789abcdefull; else h += 0xfedcba987654321ull;
        h ^= h >> 31;
    }
    return h;
}

/* 2. memcpy-like: 4 MiB buffer copied 16 times, then summed as u64. */
static uint64_t bench_mem(uint8_t *a, uint8_t *b)
{
    for (uint32_t i = 0; i < MEM_SIZE; i++) a[i] = (uint8_t)(i * 131u + 7u);
    for (uint32_t c = 0; c < MEM_COPIES; c++) {
        for (uint32_t i = 0; i < MEM_SIZE; i++) b[i] = a[i];
        for (uint32_t i = 0; i < MEM_SIZE; i += 64) b[i] ^= (uint8_t)c;
        uint8_t *t = a; a = b; b = t;
    }
    uint64_t h = 0;
    for (uint32_t i = 0; i < MEM_SIZE; i += 8) {
        uint64_t v;
        __builtin_memcpy(&v, &a[i], 8);
        h += v;
    }
    return h;
}

/* 3. Hash table: random insert + lookup over a 1M-entry table. */
static uint64_t bench_hash(uint32_t *tab)
{
    uint64_t s = 0x9e3779b97f4a7c15ull, h = 0;
    const uint32_t mask = HASH_SIZE - 1;
    for (uint32_t i = 0; i < ITERS_HASH; i++) {
        uint32_t k = (uint32_t)(xs64(&s) >> 16) + 1;
        uint32_t j = k & mask;
        while (tab[j] != 0 && tab[j] != k) j = (j + 1) & mask;
        tab[j] = k;
    }
    for (uint32_t i = 0; i < ITERS_HASH; i++) {
        uint32_t k = (uint32_t)(xs64(&s) >> 16) + 1;
        uint32_t j = k & mask;
        while (tab[j] != 0 && tab[j] != k) j = (j + 1) & mask;
        h += tab[j];
    }
    g_sink = h;
    return h;
}

/* 4. Pointer chase through a permuted linked list (indices, not pointers). */
static uint64_t bench_ptr(uint32_t *next, uint32_t *order)
{
    uint64_t s = 0x2545f4914f6cdd1dull;
    for (uint32_t i = 0; i < NODES; i++) order[i] = i;
    for (uint32_t i = NODES - 1; i > 0; i--) {
        uint32_t j = (uint32_t)(xs64(&s) % (i + 1));
        uint32_t t = order[i]; order[i] = order[j]; order[j] = t;
    }
    for (uint32_t i = 0; i < NODES; i++) next[order[i]] = order[(i + 1) & (NODES - 1)];
    uint32_t p = 0;
    uint64_t h = 0;
    for (uint32_t i = 0; i < ITERS_PTR; i++) {
        p = next[p];
        h += p;
    }
    return h;
}

/* 5. Indirect calls through a function table (ObjC-msgSend-like). */
static uint64_t call_tab_0(uint64_t x) { return x * 3 + 1; }
static uint64_t call_tab_1(uint64_t x) { return x ^ (x >> 7); }
static uint64_t call_tab_2(uint64_t x) { return (x << 5) | (x >> 59); }
static uint64_t call_tab_3(uint64_t x) { return x + 0x9e3779b97f4a7c15ull; }
static uint64_t call_tab_4(uint64_t x) { return x - 0x123456789abcdefull; }
static uint64_t (*const call_tab[5])(uint64_t) = {
    call_tab_0, call_tab_1, call_tab_2, call_tab_3, call_tab_4
};

static uint64_t bench_call(void)
{
    uint64_t h = 0x1234567890abcdefull;
    for (uint32_t i = 0; i < ITERS_CALL; i++) {
        h = call_tab[h % 5](h);
    }
    return h;
}

/* Static arenas (the bare-metal build has no allocator). */
static uint8_t  mem_a[MEM_SIZE] __attribute__((aligned(64)));
static uint8_t  mem_b[MEM_SIZE] __attribute__((aligned(64)));
static uint32_t hash_tab[HASH_SIZE] __attribute__((aligned(64)));
static uint32_t link_next[NODES] __attribute__((aligned(64)));
static uint32_t link_order[NODES] __attribute__((aligned(64)));

struct test { const char *name; uint64_t result; uint64_t cycles; };

#ifndef NATIVE
// The compiler may emit calls to these for struct/aggregate moves and the
// like.  There is no libc in the bare-metal build.
void *memcpy(void *d, const void *s, unsigned long n)
{
    uint8_t *dd = d; const uint8_t *ss = s;
    while (n--) *dd++ = *ss++;
    return d;
}
void *memset(void *d, int c, unsigned long n)
{
    uint8_t *dd = d;
    while (n--) *dd++ = (uint8_t)c;
    return d;
}
void *memmove(void *d, const void *s, unsigned long n) { return memcpy(d, s, n); }
int memcmp(const void *a, const void *b, unsigned long n)
{
    const uint8_t *x = a, *y = b;
    while (n--) { if (*x != *y) return (int)*x - (int)*y; x++; y++; }
    return 0;
}
#endif

int main(void)
{
    struct test t[5];
    uint64_t c0, c1;

    out("XLT-BENCH v1\n");

    c0 = rdcycle(); t[0].result = bench_alu(); c1 = rdcycle();
    t[0].name = "alu64"; t[0].cycles = c1 - c0;

    c0 = rdcycle(); t[1].result = bench_mem(mem_a, mem_b); c1 = rdcycle();
    t[1].name = "memcpy"; t[1].cycles = c1 - c0;

    c0 = rdcycle(); t[2].result = bench_hash(hash_tab); c1 = rdcycle();
    t[2].name = "hash"; t[2].cycles = c1 - c0;

    c0 = rdcycle(); t[3].result = bench_ptr(link_next, link_order); c1 = rdcycle();
    t[3].name = "ptrchase"; t[3].cycles = c1 - c0;

    c0 = rdcycle(); t[4].result = bench_call(); c1 = rdcycle();
    t[4].name = "indcall"; t[4].cycles = c1 - c0;

    for (int i = 0; i < 5; i++) {
        out(t[i].name); out(": "); out_hex(t[i].result);
        out(" cycles="); out_dec(t[i].cycles); out("\n");
    }
    out("sink="); out_hex(g_sink); out("\n");
    return 0;
}
