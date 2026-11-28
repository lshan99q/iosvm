// SPDX-License-Identifier: MIT
/*
 * jit.h — the LLVM ORC wrapper used by the xlt engine.
 *
 * Every module gets a fresh analysis-manager set and a fresh PassBuilder. A long-lived
 * ModuleAnalysisManager caches results keyed by pointers that ORC recycles, which is the crash
 * the prototype hit; keeping the managers local to get() avoids it.
 */
#ifndef XLT_JIT_H
#define XLT_JIT_H

#include "xlt.h"

#include <cstdint>
#include <memory>
#include <string>

namespace xlt
{

class Jit
{
  public:
    Jit();
    ~Jit();
    Jit(const Jit &) = delete;
    Jit &operator=(const Jit &) = delete;

    /* Initialise LLJIT and register the host helper symbols. */
    bool init(std::string *error);

    /* Translate, optimise and JIT the region at pc. Returns nullptr on failure. */
    BlockFn get(uint64_t pc, uint8_t *ram, uint64_t ram_base, uint64_t ram_size,
                uint64_t *instruction_count, std::string *error);

    /* Timings of the most recent get(), in nanoseconds. */
    struct Timing {
        uint64_t translate_ns = 0;
        uint64_t opt_jit_ns = 0;
    };
    const Timing &timing() const;

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace xlt

#endif /* XLT_JIT_H */
