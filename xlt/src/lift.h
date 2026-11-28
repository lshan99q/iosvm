// SPDX-License-Identifier: MIT
/*
 * lift.h — translate AArch64 guest regions into LLVM IR.
 *
 * A region is a straight-line run of at most 256 instructions with internal basic blocks for
 * direct branches. It ends at the first instruction that cannot fall through (BL/BLR/BR/RET/
 * HLT) or after the region-size cap. The generated function returns the next guest PC; the
 * dispatcher in engine.cpp decides whether to call it again.
 */
#ifndef XLT_LIFT_H
#define XLT_LIFT_H

#include "xlt.h"

#include <cstdint>
#include <memory>
#include <string>

namespace llvm
{
class LLVMContext;
class Module;
} // namespace llvm

namespace xlt
{

/* Flat guest address space shared by the loader, the lifter and the generated code. */
constexpr uint64_t kRamBase = 0x40000000ULL;
constexpr uint64_t kRamSize = 256ULL * 1024 * 1024;

/* Translate the region starting at entry_pc. On success returns a module containing the
 * function "g<entry_pc>" and writes the number of guest instructions that were translated to
 * *instruction_count. On failure returns nullptr and fills *error. */
std::unique_ptr<llvm::Module> translate_region(llvm::LLVMContext &context, uint64_t entry_pc,
                                               uint8_t *ram, uint64_t ram_base, uint64_t ram_size,
                                               uint64_t *instruction_count, std::string *error);

/* Host helpers called from generated code. The JIT registers these addresses as absolute
 * symbols; they are declared here so lift.cpp can emit calls with the right prototypes. */
extern "C" {
void xlt_bad_mem(uint64_t addr, uint64_t size);
void xlt_unsupported(uint64_t pc, uint64_t insn);
void xlt_semihost(CpuRegs *regs);
uint64_t xlt_cntvct();
uint64_t xlt_host_read(uint64_t addr, uint64_t size);
void xlt_host_write(uint64_t addr, uint64_t value, uint64_t size);
uint64_t xlt_sysreg_read(uint32_t reg);
void xlt_sysreg_write(uint32_t reg, uint64_t value);
void xlt_hvc(CpuRegs *regs, uint64_t imm);
}

} // namespace xlt

#endif /* XLT_LIFT_H */
