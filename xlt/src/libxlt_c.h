// SPDX-License-Identifier: MIT
/*
 * libxlt_c.h — C-compatible wrapper for the C++ libxlt engine.
 *
 * QEMU is a C code base, so the accelerator links against this small ABI instead of including
 * libxlt.h. The register layout is identical to xlt::CpuRegs; the C++ side asserts that.
 */
#ifndef XLT_LIBXLT_C_H
#define XLT_LIBXLT_C_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct xlt_engine xlt_engine;

typedef struct xlt_cpu_regs {
    uint64_t x[31];
    uint64_t sp;
    uint64_t pc;
    uint32_t nzcv;
    uint32_t halted;
    uint64_t exit_code;
} xlt_cpu_regs;

typedef struct xlt_host_ops {
    void *opaque;
    uint8_t *ram;
    uint64_t ram_base;
    uint64_t ram_size;
    uint64_t (*read_mem)(void *opaque, uint64_t addr, unsigned size);
    void (*write_mem)(void *opaque, uint64_t addr, uint64_t value, unsigned size);
    uint64_t (*read_sysreg)(void *opaque, uint32_t reg);
    void (*write_sysreg)(void *opaque, uint32_t reg, uint64_t value);
    void (*semihost)(void *opaque, xlt_cpu_regs *regs);
    void (*hvc)(void *opaque, xlt_cpu_regs *regs, uint32_t imm);
    uint64_t (*cntvct)(void *opaque);
    void (*fatal)(void *opaque, const char *message, int exit_code);
} xlt_host_ops;

typedef uint64_t (*xlt_block_fn)(xlt_cpu_regs *regs);

/* creates an engine but does not initialise the JIT; call xlt_engine_init next. */
xlt_engine *xlt_engine_create(const xlt_host_ops *ops);
void xlt_engine_destroy(xlt_engine *engine);
int xlt_engine_init(xlt_engine *engine, char *error, size_t error_size);

xlt_block_fn xlt_engine_get(xlt_engine *engine, uint64_t pc, char *error, size_t error_size);
int xlt_engine_run(xlt_engine *engine, xlt_cpu_regs *regs, uint64_t max_instructions, char *error,
                   size_t error_size);

uint64_t xlt_engine_dispatches(const xlt_engine *engine);
uint64_t xlt_engine_translate_ns(const xlt_engine *engine);
uint64_t xlt_engine_opt_jit_ns(const xlt_engine *engine);
uint64_t xlt_engine_wall_ns(const xlt_engine *engine);

#ifdef __cplusplus
}
#endif

#endif /* XLT_LIBXLT_C_H */
