// SPDX-License-Identifier: MIT
/*
 * libxlt.h — host-neutral xlt engine interface.
 *
 * The engine core never performs I/O. Guest memory outside the flat RAM window, system
 * registers, semihosting, and PSCI/HVC traps are routed through HostOps callbacks. The CLI
 * installs a process-oriented implementation; the QEMU accelerator installs a Machine/AddressSpace
 * implementation. No QEMU or stdio headers are included here.
 */
#ifndef XLT_LIBXLT_H
#define XLT_LIBXLT_H

#include "xlt.h"

#include <cstdint>
#include <memory>
#include <string>

namespace xlt
{

/* Callbacks used by generated code. All callbacks run on the thread executing the engine. */
struct HostOps {
    void *opaque = nullptr;
    uint8_t *ram = nullptr;
    uint64_t ram_base = 0x40000000ULL;
    uint64_t ram_size = 256ULL * 1024 * 1024;

    /* Guest physical access outside the RAM window (MMIO and bus errors). */
    uint64_t (*read_mem)(void *opaque, uint64_t addr, unsigned size) = nullptr;
    void (*write_mem)(void *opaque, uint64_t addr, uint64_t value, unsigned size) = nullptr;

    /* AArch64 system register access, keyed by the raw capstone sysreg encoding. */
    uint64_t (*read_sysreg)(void *opaque, uint32_t reg) = nullptr;
    void (*write_sysreg)(void *opaque, uint32_t reg, uint64_t value) = nullptr;

    /* HLT #0xf000 semihosting. A no-op keeps guest execution going (QEMU P1). */
    void (*semihost)(void *opaque, CpuRegs *regs) = nullptr;

    /* HVC/SMC. QEMU P1 handles PSCI SYSTEM_OFF here and exits the process. */
    void (*hvc)(void *opaque, CpuRegs *regs, uint32_t imm) = nullptr;

    uint64_t (*cntvct)(void *opaque) = nullptr;

    /* Last-resort failure hook. Must not return. */
    void (*fatal)(void *opaque, const char *message, int exit_code) = nullptr;
};

/* Install the process-wide callback table used by JIT-generated code. The table must outlive
 * every LibXlt instance and every call into a translated block. */
void set_host_ops(const HostOps *ops);
const HostOps *host_ops();

/* Generated-code entry points; these dispatch through the installed HostOps. */
extern "C" {
uint64_t xlt_host_read(uint64_t addr, uint64_t size);
void xlt_host_write(uint64_t addr, uint64_t value, uint64_t size);
uint64_t xlt_sysreg_read(uint32_t reg);
void xlt_sysreg_write(uint32_t reg, uint64_t value);
void xlt_pstate_write(uint32_t pstate, uint64_t immediate);
void xlt_hvc(CpuRegs *regs, uint64_t imm);
void xlt_semihost(CpuRegs *regs);
uint64_t xlt_cntvct();
void xlt_bad_mem(uint64_t addr, uint64_t size);
void xlt_unsupported(uint64_t pc, uint64_t insn);
}

/* A host-neutral translation engine over one flat guest RAM window. */
class LibXlt
{
  public:
    explicit LibXlt(const HostOps &ops);
    ~LibXlt();
    LibXlt(const LibXlt &) = delete;
    LibXlt &operator=(const LibXlt &) = delete;

    /* Initialise the JIT and publish the host callback table. */
    bool init(std::string *error);

    /* Translate the region at pc and return its entry point. */
    BlockFn get(uint64_t pc, uint64_t *instruction_count, std::string *error);

    /* Execute blocks from regs->pc until halted or the instruction budget is reached.
     * Returns 0 on success, -1 on an engine error (error is set). */
    int run(CpuRegs *regs, uint64_t max_instructions, bool trace, Stats *stats, std::string *error);

    const Stats &stats() const;
    uint64_t wall_ns() const;
    uint64_t translate_ns() const;
    uint64_t opt_jit_ns() const;
    uint64_t dispatches() const;

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace xlt

#endif /* XLT_LIBXLT_H */
