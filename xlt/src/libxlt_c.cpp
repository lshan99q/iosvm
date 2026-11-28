// SPDX-License-Identifier: MIT
/*
 * libxlt_c.cpp — C ABI bridge over xlt::LibXlt for QEMU and other C hosts.
 */
#include "libxlt_c.h"

#include "libxlt.h"

#include <cstring>
#include <memory>
#include <new>
#include <string>

static_assert(sizeof(xlt_cpu_regs) == sizeof(xlt::CpuRegs), "xlt_cpu_regs must match xlt::CpuRegs");
static_assert(offsetof(xlt_cpu_regs, sp) == offsetof(xlt::CpuRegs, sp), "xlt_cpu_regs.sp offset");
static_assert(offsetof(xlt_cpu_regs, pc) == offsetof(xlt::CpuRegs, pc), "xlt_cpu_regs.pc offset");
static_assert(offsetof(xlt_cpu_regs, nzcv) == offsetof(xlt::CpuRegs, nzcv),
              "xlt_cpu_regs.nzcv offset");

struct xlt_engine {
    xlt_host_ops cops{};
    xlt::HostOps ops{};
    std::unique_ptr<xlt::LibXlt> lib;
};

namespace
{

void copy_error(char *error, size_t error_size, const std::string &message)
{
    if (error == nullptr || error_size == 0) {
        return;
    }
    std::strncpy(error, message.c_str(), error_size - 1);
    error[error_size - 1] = '\0';
}

xlt::CpuRegs *as_cpp(xlt_cpu_regs *regs)
{
    return reinterpret_cast<xlt::CpuRegs *>(regs);
}

xlt_cpu_regs *as_c(xlt::CpuRegs *regs)
{
    return reinterpret_cast<xlt_cpu_regs *>(regs);
}

uint64_t bridge_read(void *opaque, uint64_t addr, unsigned size)
{
    auto *engine = static_cast<xlt_engine *>(opaque);
    return engine->cops.read_mem(engine->cops.opaque, addr, size);
}

void bridge_write(void *opaque, uint64_t addr, uint64_t value, unsigned size)
{
    auto *engine = static_cast<xlt_engine *>(opaque);
    engine->cops.write_mem(engine->cops.opaque, addr, value, size);
}

uint64_t bridge_read_sysreg(void *opaque, uint32_t reg)
{
    auto *engine = static_cast<xlt_engine *>(opaque);
    return engine->cops.read_sysreg(engine->cops.opaque, reg);
}

void bridge_write_sysreg(void *opaque, uint32_t reg, uint64_t value)
{
    auto *engine = static_cast<xlt_engine *>(opaque);
    engine->cops.write_sysreg(engine->cops.opaque, reg, value);
}

void bridge_semihost(void *opaque, xlt::CpuRegs *regs)
{
    auto *engine = static_cast<xlt_engine *>(opaque);
    engine->cops.semihost(engine->cops.opaque, as_c(regs));
}

void bridge_hvc(void *opaque, xlt::CpuRegs *regs, uint32_t imm)
{
    auto *engine = static_cast<xlt_engine *>(opaque);
    engine->cops.hvc(engine->cops.opaque, as_c(regs), imm);
}

uint64_t bridge_cntvct(void *opaque)
{
    auto *engine = static_cast<xlt_engine *>(opaque);
    return engine->cops.cntvct(engine->cops.opaque);
}

void bridge_fatal(void *opaque, const char *message, int exit_code)
{
    auto *engine = static_cast<xlt_engine *>(opaque);
    engine->cops.fatal(engine->cops.opaque, message, exit_code);
}

} // namespace

extern "C" xlt_engine *xlt_engine_create(const xlt_host_ops *ops)
{
    if (ops == nullptr) {
        return nullptr;
    }
    auto *engine = new (std::nothrow) xlt_engine;
    if (engine == nullptr) {
        return nullptr;
    }
    engine->cops = *ops;
    engine->ops.opaque = engine;
    engine->ops.ram = ops->ram;
    engine->ops.ram_base = ops->ram_base;
    engine->ops.ram_size = ops->ram_size;
    engine->ops.read_mem = ops->read_mem != nullptr ? bridge_read : nullptr;
    engine->ops.write_mem = ops->write_mem != nullptr ? bridge_write : nullptr;
    engine->ops.read_sysreg = ops->read_sysreg != nullptr ? bridge_read_sysreg : nullptr;
    engine->ops.write_sysreg = ops->write_sysreg != nullptr ? bridge_write_sysreg : nullptr;
    engine->ops.semihost = ops->semihost != nullptr ? bridge_semihost : nullptr;
    engine->ops.hvc = ops->hvc != nullptr ? bridge_hvc : nullptr;
    engine->ops.cntvct = ops->cntvct != nullptr ? bridge_cntvct : nullptr;
    engine->ops.fatal = bridge_fatal;
    return engine;
}

extern "C" void xlt_engine_destroy(xlt_engine *engine)
{
    delete engine;
}

extern "C" int xlt_engine_init(xlt_engine *engine, char *error, size_t error_size)
{
    if (engine == nullptr) {
        copy_error(error, error_size, "xlt: null engine");
        return -1;
    }
    engine->lib = std::make_unique<xlt::LibXlt>(engine->ops);
    std::string message;
    if (!engine->lib->init(&message)) {
        copy_error(error, error_size, message);
        return -1;
    }
    return 0;
}

extern "C" xlt_block_fn xlt_engine_get(xlt_engine *engine, uint64_t pc, char *error,
                                       size_t error_size)
{
    if (engine == nullptr || engine->lib == nullptr) {
        copy_error(error, error_size, "xlt: engine not initialised");
        return nullptr;
    }
    std::string message;
    uint64_t instructions = 0;
    xlt::BlockFn block = engine->lib->get(pc, &instructions, &message);
    if (block == nullptr) {
        copy_error(error, error_size, message);
    }
    return reinterpret_cast<xlt_block_fn>(block);
}

extern "C" int xlt_engine_run(xlt_engine *engine, xlt_cpu_regs *regs, uint64_t max_instructions,
                              char *error, size_t error_size)
{
    if (engine == nullptr || engine->lib == nullptr || regs == nullptr) {
        copy_error(error, error_size, "xlt: engine not initialised");
        return -1;
    }
    std::string message;
    int rc = engine->lib->run(as_cpp(regs), max_instructions, false, nullptr, &message);
    if (rc != 0) {
        copy_error(error, error_size, message);
    }
    return rc;
}

extern "C" uint64_t xlt_engine_dispatches(const xlt_engine *engine)
{
    return engine != nullptr && engine->lib != nullptr ? engine->lib->dispatches() : 0;
}

extern "C" uint64_t xlt_engine_translate_ns(const xlt_engine *engine)
{
    return engine != nullptr && engine->lib != nullptr ? engine->lib->translate_ns() : 0;
}

extern "C" uint64_t xlt_engine_opt_jit_ns(const xlt_engine *engine)
{
    return engine != nullptr && engine->lib != nullptr ? engine->lib->opt_jit_ns() : 0;
}

extern "C" uint64_t xlt_engine_wall_ns(const xlt_engine *engine)
{
    return engine != nullptr && engine->lib != nullptr ? engine->lib->wall_ns() : 0;
}
