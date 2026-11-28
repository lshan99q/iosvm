// SPDX-License-Identifier: MIT
// Memory-access cost probe: how expensive is a guest load/store under TCG
// vs native?  All arrays live in .bss; results are printed and the harness
// exits through semihosting.
#include <stdint.h>

#define N  (8u * 1024u * 1024u)   /* 8M accesses */
static uint8_t  b1[N];
static uint16_t b2[N];
static uint32_t b4[N];
static uint64_t b8[N];
static volatile uint64_t sink;

static inline uint64_t rdcycle(void)
{
    uint64_t v;
    __asm__ volatile("mrs %0, cntvct_el0" : "=r"(v));
    return v;
}
static void semi_call(long op, const void *arg)
{
    register long x0 __asm__("x0") = op;
    register const void *x1 __asm__("x1") = arg;
    __asm__ volatile("hlt #0xf000" : : "r"(x0), "r"(x1) : "memory");
}
static void out(const char *s) { semi_call(0x04, s); }
static void out_dec(uint64_t v)
{
    char b[24]; int i = 23; b[i] = 0;
    do { b[--i] = (char)('0' + v % 10); v /= 10; } while (v);
    out(&b[i]);
}
static void out_hex(uint64_t v)
{
    static const char h[] = "0123456789abcdef";
    char b[19]; b[0]='0'; b[1]='x';
    for (int i=0;i<16;i++) b[2+i]=h[(v>>(60-4*i))&15];
    b[18]=0; out(b);
}

int main(void)
{
    uint64_t c0, c1, h = 0;

    c0 = rdcycle();
    for (uint32_t i = 0; i < N; i++) b1[i] = (uint8_t)i;
    c1 = rdcycle();
    out("st1  "); out_dec(c1-c0); out("\n");

    c0 = rdcycle();
    for (uint32_t i = 0; i < N; i++) h += b1[i];
    c1 = rdcycle();
    out("ld1  "); out_dec(c1-c0); out("\n");

    c0 = rdcycle();
    for (uint32_t i = 0; i < N; i++) b2[i] = (uint16_t)i;
    c1 = rdcycle();
    out("st2  "); out_dec(c1-c0); out("\n");

    c0 = rdcycle();
    for (uint32_t i = 0; i < N; i++) h += b2[i];
    c1 = rdcycle();
    out("ld2  "); out_dec(c1-c0); out("\n");

    c0 = rdcycle();
    for (uint32_t i = 0; i < N; i++) b4[i] = (uint16_t)i;
    c1 = rdcycle();
    out("st4  "); out_dec(c1-c0); out("\n");

    c0 = rdcycle();
    for (uint32_t i = 0; i < N; i++) h += b4[i];
    c1 = rdcycle();
    out("ld4  "); out_dec(c1-c0); out("\n");

    c0 = rdcycle();
    for (uint32_t i = 0; i < N; i++) b8[i] = (uint16_t)i;
    c1 = rdcycle();
    out("st8  "); out_dec(c1-c0); out("\n");

    c0 = rdcycle();
    for (uint32_t i = 0; i < N; i++) h += b8[i];
    c1 = rdcycle();
    out("ld8  "); out_dec(c1-c0); out("\n");

    c0 = rdcycle();
    for (uint32_t i = 0; i < N; i += 2) { uint64_t v; __builtin_memcpy(&v,&b1[i],8); __builtin_memcpy(&b1[i],&b8[i>>3],8); h+=v; }
    c1 = rdcycle();
    out("rdwr8 "); out_dec(c1-c0); out("\n");

    out("h="); out_hex(h); out("\n");
    sink = h;
    return 0;
}

// Bare-metal libc shims (the probe links without a C library).
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
