// SPDX-License-Identifier: MIT
/*
 * libxlt.cpp — host-neutral engine core, host callback trampolines and dispatcher.
 *
 * The generated code calls the fixed C symbols below. Each one forwards to the HostOps table
 * installed by the embedding host. This keeps the engine free of stdio, QEMU and loader
 * dependencies while preserving the M1 fast path through a host RAM pointer.
 */
#include "libxlt.h"

#include "jit.h"

#include <chrono>
#include <cstdlib>
#include <limits>
#include <memory>
#include <utility>
#include <vector>

namespace xlt
{

namespace
{

const HostOps *g_host = nullptr;

uint64_t now_ns()
{
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                     std::chrono::steady_clock::now().time_since_epoch())
                                     .count());
}

std::string hex_value(uint64_t value)
{
    static const char digits[] = "0123456789abcdef";
    char buffer[19];
    buffer[0] = '0';
    buffer[1] = 'x';
    for (int i = 0; i < 16; i++) {
        buffer[2 + i] = digits[(value >> (60 - i * 4)) & 0xf];
    }
    buffer[18] = '\0';
    return std::string(buffer);
}

[[noreturn]] void host_fatal(const std::string &message, int exit_code)
{
    if (g_host != nullptr && g_host->fatal != nullptr) {
        g_host->fatal(g_host->opaque, message.c_str(), exit_code);
    }
    std::_Exit(exit_code);
}

} // namespace

void set_host_ops(const HostOps *ops)
{
    g_host = ops;
}

const HostOps *host_ops()
{
    return g_host;
}

} // namespace xlt

extern "C" uint64_t xlt_host_read(uint64_t addr, uint64_t size)
{
    const xlt::HostOps *ops = xlt::host_ops();
    if (ops == nullptr || ops->read_mem == nullptr) {
        xlt::host_fatal("xlt: unsupported guest MMIO read at " + xlt::hex_value(addr), 70);
    }
    return ops->read_mem(ops->opaque, addr, static_cast<unsigned>(size));
}

extern "C" void xlt_host_write(uint64_t addr, uint64_t value, uint64_t size)
{
    const xlt::HostOps *ops = xlt::host_ops();
    if (ops == nullptr || ops->write_mem == nullptr) {
        xlt::host_fatal("xlt: unsupported guest MMIO write at " + xlt::hex_value(addr), 70);
    }
    ops->write_mem(ops->opaque, addr, value, static_cast<unsigned>(size));
}

extern "C" uint64_t xlt_sysreg_read(uint32_t reg)
{
    const xlt::HostOps *ops = xlt::host_ops();
    if (ops == nullptr || ops->read_sysreg == nullptr) {
        return 0;
    }
    return ops->read_sysreg(ops->opaque, reg);
}

extern "C" void xlt_sysreg_write(uint32_t reg, uint64_t value)
{
    const xlt::HostOps *ops = xlt::host_ops();
    if (ops != nullptr && ops->write_sysreg != nullptr) {
        ops->write_sysreg(ops->opaque, reg, value);
    }
}

extern "C" void xlt_hvc(xlt::CpuRegs *regs, uint64_t imm)
{
    const xlt::HostOps *ops = xlt::host_ops();
    if (ops == nullptr || ops->hvc == nullptr) {
        xlt::host_fatal("xlt: unsupported HVC/SMC at pc " + xlt::hex_value(regs->pc), 71);
    }
    ops->hvc(ops->opaque, regs, static_cast<uint32_t>(imm));
}

extern "C" void xlt_semihost(xlt::CpuRegs *regs)
{
    const xlt::HostOps *ops = xlt::host_ops();
    if (ops != nullptr && ops->semihost != nullptr) {
        ops->semihost(ops->opaque, regs);
    }
}

extern "C" uint64_t xlt_cntvct()
{
    const xlt::HostOps *ops = xlt::host_ops();
    if (ops == nullptr || ops->cntvct == nullptr) {
        return 0;
    }
    return ops->cntvct(ops->opaque);
}

extern "C" void xlt_bad_mem(uint64_t addr, uint64_t size)
{
    (void)size;
    xlt::host_fatal("xlt: bad guest memory access at " + xlt::hex_value(addr), 70);
}

extern "C" void xlt_unsupported(uint64_t pc, uint64_t insn)
{
    (void)insn;
    xlt::host_fatal("xlt: unsupported instruction at " + xlt::hex_value(pc), 71);
}

namespace xlt
{

struct LibXlt::Impl {
    HostOps host;
    std::unique_ptr<Jit> jit;
    Stats stats;
    uint64_t wall_ns_value = 0;
    uint64_t translate_ns_value = 0;
    uint64_t opt_jit_ns_value = 0;
    uint64_t dispatch_count = 0;
};

LibXlt::LibXlt(const HostOps &ops) : impl_(std::make_unique<Impl>())
{
    impl_->host = ops;
    set_host_ops(&impl_->host);
}

LibXlt::~LibXlt()
{
    if (host_ops() == &impl_->host) {
        set_host_ops(nullptr);
    }
}

bool LibXlt::init(std::string *error)
{
    if (impl_->jit == nullptr) {
        impl_->jit = std::make_unique<Jit>();
    }
    return impl_->jit->init(error);
}

BlockFn LibXlt::get(uint64_t pc, uint64_t *instruction_count, std::string *error)
{
    return impl_->jit->get(pc, impl_->host.ram, impl_->host.ram_base, impl_->host.ram_size,
                           instruction_count, error);
}

int LibXlt::run(CpuRegs *regs, uint64_t max_instructions, bool trace, Stats *stats,
                std::string *error)
{
    if (impl_->jit == nullptr) {
        impl_->jit = std::make_unique<Jit>();
    }
    if (!impl_->jit->init(error)) {
        return -1;
    }
    impl_->stats = Stats{};
    impl_->translate_ns_value = 0;
    impl_->opt_jit_ns_value = 0;
    impl_->dispatch_count = 0;

    struct DCacheEntry {
        uint64_t pc;
        BlockFn fn;
        uint64_t insns;
    };
    static DCacheEntry dc[1u << 13];
    for (auto &entry : dc) {
        entry.pc = std::numeric_limits<uint64_t>::max();
        entry.fn = nullptr;
        entry.insns = 0;
    }

    uint64_t pc = regs->pc;
    const uint64_t begin = now_ns();
    while (regs->halted == 0) {
        DCacheEntry &slot = dc[(pc ^ (pc >> 12)) & 0x1fffu];
        if (slot.pc != pc || slot.fn == nullptr) {
            uint64_t insns = 0;
            const uint64_t t0 = now_ns();
            const BlockFn fn = get(pc, &insns, error);
            const uint64_t t1 = now_ns();
            impl_->translate_ns_value += impl_->jit->timing().translate_ns;
            impl_->opt_jit_ns_value += impl_->jit->timing().opt_jit_ns;
            if (fn == nullptr) {
                if (error != nullptr && error->empty()) {
                    *error = "cannot translate pc " + hex_value(pc);
                }
                return -1;
            }
            if (insns == 0) {
                if (error != nullptr) {
                    *error = "empty region at " + hex_value(pc);
                }
                return -1;
            }
            slot.pc = pc;
            slot.fn = fn;
            slot.insns = insns;
            impl_->stats.blocks_translated++;
            impl_->stats.instructions_translated += insns;
            if (trace) {
                (void)t0;
                (void)t1;
            }
        }

        const BlockFn fn = slot.fn;
        const uint64_t insns = slot.insns;
        const uint64_t next = fn(regs);
        impl_->stats.blocks_executed++;
        impl_->stats.instructions_executed += insns;
        if (max_instructions != 0 && impl_->stats.instructions_executed > max_instructions) {
            if (error != nullptr) {
                *error = "instruction limit exceeded at " + hex_value(pc);
            }
            return -1;
        }
        if (regs->halted != 0) {
            break;
        }
        if (next == 0) {
            if (error != nullptr) {
                *error = "block at " + hex_value(pc) + " returned no successor";
            }
            return -1;
        }
        impl_->dispatch_count++;
        pc = next;
        regs->pc = pc;
    }

    impl_->wall_ns_value = now_ns() - begin;
    if (stats != nullptr) {
        *stats = impl_->stats;
    }
    return 0;
}

const Stats &LibXlt::stats() const
{
    return impl_->stats;
}

uint64_t LibXlt::wall_ns() const
{
    return impl_->wall_ns_value;
}

uint64_t LibXlt::translate_ns() const
{
    return impl_->translate_ns_value;
}

uint64_t LibXlt::opt_jit_ns() const
{
    return impl_->opt_jit_ns_value;
}

uint64_t LibXlt::dispatches() const
{
    return impl_->dispatch_count;
}

} // namespace xlt
