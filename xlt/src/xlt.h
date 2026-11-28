// SPDX-License-Identifier: MIT
/*
 * xlt — the cross-architecture dynamic translator (M1).
 *
 * xlt runs AArch64 guest code on this x86-64 host by translating it at run time, one basic block
 * at a time: decode with capstone, lift to a small IR, emit x86-64 with LLVM 18, execute. Blocks
 * are cached by guest PC, so the second execution of a block runs host code directly.
 *
 * This header is the only surface the CLI and the test binaries use. Everything behind it — the
 * ELF loader, the decoder, the lifter, the block cache and the JIT — is an implementation detail
 * and lives in the other translation units under xlt/src.
 */
#ifndef XLT_H
#define XLT_H

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <type_traits>

namespace xlt
{

/* The AArch64 register state a translated block reads and writes.

 * Kept standard-layout and free of pointers into host memory: the JIT computes field offsets with
 * offsetof on this struct, and a test can inspect the result without knowing the engine. `pc` is
 * not part of the architectural register file but is carried here so a block can hand its
 * successor back to the dispatcher without a second argument. */
struct CpuRegs {
    uint64_t x[31];  /* x0..x30; x30 is the link register */
    uint64_t sp;     /* stack pointer, SP_EL0 */
    uint64_t pc;     /* guest address of the next instruction to execute */
    uint32_t nzcv;   /* condition flags: N in bit 31, Z in 30, C in 29, V in 28 */
    uint32_t halted; /* non-zero once the guest has asked to stop */
    uint64_t exit_code;
};

static_assert(std::is_standard_layout<CpuRegs>::value, "the JIT takes offsetof(CpuRegs, ...)");

/* The `nzcv` bit positions, named for the lifter that sets them. */
enum : uint32_t {
    kN = 1u << 31,
    kZ = 1u << 30,
    kC = 1u << 29,
    kV = 1u << 28,
};

/* A translated block: guest code for one basic block, called with the guest state.
 *
 * Returns the guest PC at which execution continues — the dispatcher looks the block up there.
 * A block whose last instruction has no successor (or asks to stop) sets CpuRegs::halted and the
 * dispatcher stops instead of following the returned PC. */
using BlockFn = uint64_t (*)(CpuRegs *regs);

/* How one run is started. Zero uses the default: the ELF's entry point and an 8 MiB stack. */
struct RunOptions {
    uint64_t entry = 0;
    uint64_t stack_size = 8u << 20;
    uint64_t max_instructions = 0; /* 0: no limit; a bound for tests that must terminate */
    bool trace = false;            /* log each block's PC and exit to stderr */
};

/* Counters, so a benchmark can report what the translator actually did. */
struct Stats {
    uint64_t blocks_translated = 0;
    uint64_t blocks_executed = 0;
    uint64_t instructions_translated = 0;
    uint64_t instructions_executed = 0;
};

/* One guest process: its memory, its translated blocks and its register state.
 *
 * The loader, decoder, lifter and JIT that fill in these methods land in the M1 tasks that follow
 * the scaffold; until then the class is a declaration the CLI links against without running. */
class Engine
{
  public:
    Engine();
    ~Engine();
    Engine(Engine &&) noexcept;
    Engine &operator=(Engine &&) noexcept;
    Engine(const Engine &) = delete;
    Engine &operator=(const Engine &) = delete;

    /* Maps the ELF at `elf_path` as AArch64 guest memory. Returns false and fills *error on a
     * malformed, truncated or wrong-architecture image. */
    bool load(const std::string &elf_path, std::string *error = nullptr);

    /* Translates and executes from options.entry until the guest halts or the instruction budget
     * runs out. Returns the guest exit code, or -1 when the engine itself fails (*error is set). */
    int run(const RunOptions &options, std::string *error = nullptr);

    const CpuRegs &regs() const noexcept
    {
        return regs_;
    }
    CpuRegs &regs() noexcept
    {
        return regs_;
    }
    const Stats &stats() const noexcept
    {
        return stats_;
    }

    static constexpr const char *version()
    {
        return "0.1";
    }

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    CpuRegs regs_{};
    Stats stats_{};
};

} // namespace xlt

#endif /* XLT_H */
