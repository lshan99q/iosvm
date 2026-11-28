// SPDX-License-Identifier: MIT
/*
 * jit.cpp — LLVM ORC LLJIT wrapper.
 *
 * Each region is a separate module. The analysis managers and PassBuilder are created per
 * module (not cached in the JIT object): LLVM's managers key results by module/function pointer
 * addresses, and ORC recycles those addresses after taking ownership of a module. A long-lived
 * manager was the cause of a prototype SIGSEGV.
 */
#include "jit.h"

#include "libxlt.h"
#include "lift.h"

#include "llvm/ExecutionEngine/Orc/LLJIT.h"
#include "llvm/ExecutionEngine/Orc/ThreadSafeModule.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"
#include "llvm/Passes/PassBuilder.h"
#include "llvm/Support/Error.h"
#include "llvm/Support/TargetSelect.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Target/TargetMachine.h"

#include <chrono>
#include <cstdlib>
#include <memory>
#include <string>

using namespace llvm;
using namespace llvm::orc;

namespace xlt
{
namespace
{

uint64_t nowNs()
{
    auto now = std::chrono::steady_clock::now().time_since_epoch();
    return (uint64_t)std::chrono::duration_cast<std::chrono::nanoseconds>(now).count();
}

OptimizationLevel optimizationLevel()
{
    const char *env = std::getenv("XLT_OPT");
    if (!env)
        return OptimizationLevel::O2;
    switch (std::atoi(env)) {
    case 0:
        return OptimizationLevel::O0;
    case 1:
        return OptimizationLevel::O1;
    case 3:
        return OptimizationLevel::O3;
    default:
        return OptimizationLevel::O2;
    }
}

} // namespace

struct Jit::Impl {
    std::unique_ptr<LLJIT> jit;
    std::unique_ptr<ThreadSafeContext> tsctx;
    Timing timing;
};

Jit::Jit() : impl_(std::make_unique<Impl>()) {}

Jit::~Jit() = default;

bool Jit::init(std::string *error)
{
    static bool targets_initialized = false;
    if (!targets_initialized) {
        InitializeNativeTarget();
        InitializeNativeTargetAsmPrinter();
        InitializeNativeTargetAsmParser();
        targets_initialized = true;
    }

    auto jtmb = JITTargetMachineBuilder::detectHost();
    if (!jtmb) {
        if (error)
            *error = "target machine: " + toString(jtmb.takeError());
        return false;
    }
    jtmb->setCodeGenOptLevel(CodeGenOptLevel::Default);
    auto jit = LLJITBuilder().setJITTargetMachineBuilder(std::move(*jtmb)).create();
    if (!jit) {
        if (error)
            *error = "LLJIT: " + toString(jit.takeError());
        return false;
    }
    impl_->jit = std::move(*jit);
    impl_->tsctx = std::make_unique<ThreadSafeContext>(std::make_unique<LLVMContext>());

    SymbolMap symbols;
    auto add_symbol = [&](const char *name, auto fn) {
        symbols[impl_->jit->mangleAndIntern(name)] =
            ExecutorSymbolDef(ExecutorAddr::fromPtr(fn), JITSymbolFlags::Exported);
    };
    add_symbol("xlt_bad_mem", &xlt_bad_mem);
    add_symbol("xlt_unsupported", &xlt_unsupported);
    add_symbol("xlt_semihost", &xlt_semihost);
    add_symbol("xlt_cntvct", &xlt_cntvct);
    add_symbol("xlt_host_read", &xlt_host_read);
    add_symbol("xlt_host_write", &xlt_host_write);
    add_symbol("xlt_sysreg_read", &xlt_sysreg_read);
    add_symbol("xlt_sysreg_write", &xlt_sysreg_write);
    add_symbol("xlt_pstate_write", &xlt_pstate_write);
    add_symbol("xlt_hvc", &xlt_hvc);
    if (auto err = impl_->jit->getMainJITDylib().define(absoluteSymbols(std::move(symbols)))) {
        if (error)
            *error = "define host symbols: " + toString(std::move(err));
        return false;
    }
    return true;
}

BlockFn Jit::get(uint64_t pc, uint8_t *ram, uint64_t ram_base, uint64_t ram_size,
                 uint64_t *instruction_count, std::string *error)
{
    uint64_t t0 = nowNs();
    auto module = translate_region(*impl_->tsctx->getContext(), pc, ram, ram_base, ram_size,
                                   instruction_count, error);
    uint64_t t1 = nowNs();
    if (!module)
        return nullptr;

    LoopAnalysisManager lam;
    FunctionAnalysisManager fam;
    CGSCCAnalysisManager cgam;
    ModuleAnalysisManager mam;
    PassBuilder pb;
    pb.registerModuleAnalyses(mam);
    pb.registerCGSCCAnalyses(cgam);
    pb.registerFunctionAnalyses(fam);
    pb.registerLoopAnalyses(lam);
    pb.crossRegisterProxies(lam, fam, cgam, mam);
    ModulePassManager mpm = pb.buildPerModuleDefaultPipeline(optimizationLevel());
    mpm.run(*module, mam);

    if (const char *dump = std::getenv("XLT_DUMP_IR")) {
        if (!dump[0] || std::atoll(dump) == (long long)pc || !std::strcmp(dump, "all"))
            module->print(errs(), nullptr);
    }

    std::string name = "g" + std::to_string(pc);
    if (auto err = impl_->jit->addIRModule(ThreadSafeModule(std::move(module), *impl_->tsctx))) {
        if (error)
            *error = "addIRModule: " + toString(std::move(err));
        return nullptr;
    }
    auto sym = impl_->jit->lookup(name);
    if (!sym) {
        if (error)
            *error = "lookup " + name + ": " + toString(sym.takeError());
        return nullptr;
    }

    uint64_t t2 = nowNs();
    impl_->timing.translate_ns = t1 - t0;
    impl_->timing.opt_jit_ns = t2 - t1;
    return sym->toPtr<BlockFn>();
}

const Jit::Timing &Jit::timing() const
{
    return impl_->timing;
}

} // namespace xlt
