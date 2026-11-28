// SPDX-License-Identifier: MIT
/*
 * p2.h — stage-1 MMU and synchronous exception support for the P2 engine.
 *
 * This is deliberately a separate execution path from the flat JIT used by M1: when
 * SCTLR_EL1.M is set the engine interprets guest instructions using the same neutral decoder and
 * routes every fetch and data access through the page-table walker. Flat-RAM blocks keep using the
 * JIT and are entered again as soon as M is cleared.
 */
#ifndef XLT_P2_H
#define XLT_P2_H

#include "libxlt.h"

#include <cstdint>
#include <memory>
#include <string>

namespace xlt
{

class P2Engine
{
  public:
    P2Engine(const HostOps *host, CpuRegs *regs);
    ~P2Engine();
    P2Engine(const P2Engine &) = delete;
    P2Engine &operator=(const P2Engine &) = delete;

    bool enabled() const;

    /* Execute until halted or until SCTLR_EL1.M is cleared. Returns 0, or -1 with error set. */
    int run(uint64_t max_instructions, uint64_t *executed, std::string *error);

    /* System-register model shared with the JIT path through the xlt_sysreg_* trampolines. */
    bool sysreg_read(uint32_t reg, uint64_t *value) const;
    bool sysreg_write(uint32_t reg, uint64_t value);
    bool pstate_write(uint32_t pstate, uint64_t immediate);

    /* Used by the ERET trap helper from generated code. */
    void eret();

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

/* Process-wide hooks called by libxlt.cpp. The engine is single-threaded in this milestone. */
bool p2_sysreg_read(uint32_t reg, uint64_t *value);
bool p2_sysreg_write(uint32_t reg, uint64_t value);
bool p2_pstate_write(uint32_t pstate, uint64_t immediate);

} // namespace xlt

#endif /* XLT_P2_H */
