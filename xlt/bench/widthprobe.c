// SPDX-License-Identifier: MIT
// Copy-width probe: same 4 MiB copy with 1/2/4/8-byte elements and ldp/stp.
#include <stdint.h>
#define MEM_SIZE (4u * 1024u * 1024u)
static uint8_t a[MEM_SIZE] __attribute__((aligned(64)));
static uint8_t b[MEM_SIZE] __attribute__((aligned(64)));
static volatile uint64_t sink;

static inline uint64_t rdcycle(void) { uint64_t v; __asm__ volatile("mrs %0, cntvct_el0":"=r"(v)); return v; }
static void semi_call(long op, const void *arg)
{
    register long x0 __asm__("x0") = op;
    register const void *x1 __asm__("x1") = arg;
    __asm__ volatile("hlt #0xf000" : : "r"(x0), "r"(x1) : "memory");
}
static void out(const char *s) { semi_call(0x04, s); }
static void out_dec(uint64_t v)
{
    char bb[24]; int i=23; bb[i]=0;
    do { bb[--i]=(char)('0'+v%10); v/=10; } while (v);
    out(&bb[i]);
}
static void out_hex(uint64_t v)
{
    static const char h[]="0123456789abcdef";
    char bb[19]; bb[0]='0'; bb[1]='x';
    for (int i=0;i<16;i++) bb[2+i]=h[(v>>(60-4*i))&15];
    bb[18]=0; out(bb);
}
void *memcpy(void *d, const void *s, unsigned long n) { uint8_t *dd=d; const uint8_t *ss=s; while(n--)*dd++=*ss++; return d; }
void *memset(void *d, int c, unsigned long n) { uint8_t *dd=d; while(n--)*dd++=(uint8_t)c; return d; }
void *memmove(void *d, const void *s, unsigned long n){ return memcpy(d,s,n); }
int memcmp(const void *a, const void *b, unsigned long n){ const uint8_t*x=a,*y=b; while(n--){if(*x!=*y)return (int)*x-(int)*y;x++;y++;} return 0; }

#define RUN(NAME, STMT) do { \
    uint64_t c0 = rdcycle(); \
    uint8_t *pa = a, *pb = b; \
    for (int rep = 0; rep < 16; rep++) { \
        for (uint32_t i = 0; i < MEM_SIZE / (W); i++) { STMT } \
        uint8_t *t = pa; pa = pb; pb = t; \
    } \
    uint64_t c1 = rdcycle(); \
    out(NAME); out(" "); out_dec(c1 - c0); out("\n"); \
} while (0)

int main(void)
{
    for (uint32_t i = 0; i < MEM_SIZE; i++) a[i] = (uint8_t)(i * 131u + 7u);
    uint64_t h = 0;

    {   // 8-byte elements
        uint64_t c0 = rdcycle();
        uint64_t *pa = (uint64_t *)a, *pb = (uint64_t *)b;
        for (int rep = 0; rep < 16; rep++) {
            for (uint32_t i = 0; i < MEM_SIZE / 8; i++) pb[i] = pa[i];
            uint64_t *t = pa; pa = pb; pb = t;
        }
        uint64_t c1 = rdcycle();
        out("copy8 "); out_dec(c1 - c0); out("\n");
        h += pa[123];
    }
    {   // 4-byte elements
        uint64_t c0 = rdcycle();
        uint32_t *pa = (uint32_t *)a, *pb = (uint32_t *)b;
        for (int rep = 0; rep < 16; rep++) {
            for (uint32_t i = 0; i < MEM_SIZE / 4; i++) pb[i] = pa[i];
            uint32_t *t = pa; pa = pb; pb = t;
        }
        uint64_t c1 = rdcycle();
        out("copy4 "); out_dec(c1 - c0); out("\n");
        h += pa[123];
    }
    {   // 2-byte elements
        uint64_t c0 = rdcycle();
        uint16_t *pa = (uint16_t *)a, *pb = (uint16_t *)b;
        for (int rep = 0; rep < 16; rep++) {
            for (uint32_t i = 0; i < MEM_SIZE / 2; i++) pb[i] = pa[i];
            uint16_t *t = pa; pa = pb; pb = t;
        }
        uint64_t c1 = rdcycle();
        out("copy2 "); out_dec(c1 - c0); out("\n");
        h += pa[123];
    }
    {   // 1-byte elements
        uint64_t c0 = rdcycle();
        uint8_t *pa = a, *pb = b;
        for (int rep = 0; rep < 16; rep++) {
            for (uint32_t i = 0; i < MEM_SIZE; i++) pb[i] = pa[i];
            uint8_t *t = pa; pa = pb; pb = t;
        }
        uint64_t c1 = rdcycle();
        out("copy1 "); out_dec(c1 - c0); out("\n");
        h += pa[123];
    }
    {   // ldp/stp via builtin over one 4MiB copy, repeated 16x in place
        uint64_t c0 = rdcycle();
        for (int rep = 0; rep < 16; rep++) __builtin_memcpy(b, a, MEM_SIZE);
        uint64_t c1 = rdcycle();
        out("builtin "); out_dec(c1 - c0); out("\n");
        h += b[123];
    }
    out("h="); out_hex(h); out("\n");
    sink = h;
    return 0;
}
