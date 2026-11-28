// SPDX-License-Identifier: MIT
#include "decode.h"

#include <cstdio>
#include <cstring>

namespace
{

using namespace xlt;

int g_checks = 0;
int g_failures = 0;

#define CHECK(condition)                                                                           \
    do {                                                                                           \
        ++g_checks;                                                                                \
        if (!(condition)) {                                                                        \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition);              \
            ++g_failures;                                                                          \
        }                                                                                          \
    } while (0)

Insn at(uint32_t word, uint64_t pc = 0x1000)
{
    return decode(pc, word);
}

void reg(const Operand &op, Gpr gpr, uint8_t width)
{
    CHECK(op.kind == OperandKind::kReg);
    CHECK(op.kind == OperandKind::kReg && op.reg.gpr == gpr);
    CHECK(op.kind == OperandKind::kReg && op.reg.width == width);
}

void imm(const Operand &op, uint64_t value)
{
    CHECK(op.kind == OperandKind::kImm);
    CHECK(op.kind == OperandKind::kImm && op.imm == value);
}

void mem(const Operand &op, Gpr base, int64_t disp)
{
    CHECK(op.kind == OperandKind::kMem);
    CHECK(op.kind == OperandKind::kMem && op.base.gpr == base);
    CHECK(op.kind == OperandKind::kMem && op.base.width == 64);
    CHECK(op.kind == OperandKind::kMem && op.disp == disp);
}

void cond(const Operand &op, Cond value)
{
    CHECK(op.kind == OperandKind::kCond);
    CHECK(op.kind == OperandKind::kCond && op.cond == value);
}

void sysreg(const Operand &op, uint32_t value)
{
    CHECK(op.kind == OperandKind::kSysReg);
    CHECK(op.kind == OperandKind::kSysReg && op.sysreg == value);
}

void test_arithmetic()
{
    Insn i = at(0x8b020020u);
    CHECK(!i.unsupported);
    CHECK(i.opcode == Opcode::kAdd);
    CHECK(i.mnemonic != nullptr && std::strcmp(i.mnemonic, "add") == 0);
    CHECK(i.operand_count == 3);
    reg(i.operands[0], Gpr::k0, 64);
    reg(i.operands[1], Gpr::k1, 64);
    reg(i.operands[2], Gpr::k2, 64);
    CHECK(!i.sets_flags);
    CHECK(!i.is_branch);

    i = at(0x8b020c20u);
    CHECK(i.opcode == Opcode::kAdd);
    reg(i.operands[2], Gpr::k2, 64);
    CHECK(i.operands[2].shift == ShiftType::kLsl);
    CHECK(i.operands[2].shift_amount == 3);
    CHECK(i.operands[2].extend == ExtendType::kNone);

    i = at(0x91048c20u);
    CHECK(i.opcode == Opcode::kAdd);
    CHECK(i.operand_count == 3);
    reg(i.operands[0], Gpr::k0, 64);
    reg(i.operands[1], Gpr::k1, 64);
    imm(i.operands[2], 0x123);

    i = at(0xcb050083u);
    CHECK(i.opcode == Opcode::kSub);
    CHECK(i.operand_count == 3);
    reg(i.operands[0], Gpr::k3, 64);
    reg(i.operands[1], Gpr::k4, 64);
    reg(i.operands[2], Gpr::k5, 64);

    i = at(0xf100041fu);
    CHECK(i.opcode == Opcode::kCmp);
    CHECK(i.operand_count == 2);
    reg(i.operands[0], Gpr::k0, 64);
    imm(i.operands[1], 1);
    CHECK(i.sets_flags);

    i = at(0x8b224c20u);
    CHECK(i.opcode == Opcode::kAdd);
    CHECK(i.operand_count == 3);
    reg(i.operands[0], Gpr::k0, 64);
    reg(i.operands[1], Gpr::k1, 64);
    reg(i.operands[2], Gpr::k2, 32);
    CHECK(i.operands[2].extend == ExtendType::kUxtw);
    CHECK(i.operands[2].extend_amount == 3);
    CHECK(i.operands[2].shift == ShiftType::kNone);

    i = at(0x910043e0u);
    CHECK(i.opcode == Opcode::kAdd);
    reg(i.operands[0], Gpr::k0, 64);
    reg(i.operands[1], Gpr::kSp, 64);
    imm(i.operands[2], 16);

    i = at(0xab020020u);
    CHECK(i.opcode == Opcode::kAdd);
    CHECK(i.sets_flags);
    reg(i.operands[0], Gpr::k0, 64);

    i = at(0xaa0103e0u);
    CHECK(i.opcode == Opcode::kMov);
    CHECK(i.operand_count == 2);
    reg(i.operands[0], Gpr::k0, 64);
    reg(i.operands[1], Gpr::k1, 64);

    i = at(0xaa1f03e0u);
    CHECK(i.opcode == Opcode::kMov);
    CHECK(i.operand_count == 2);
    reg(i.operands[0], Gpr::k0, 64);
    reg(i.operands[1], Gpr::kZr, 64);
}

void test_logical()
{
    Insn i = at(0xf24001ffu);
    CHECK(!i.unsupported);
    CHECK(i.opcode == Opcode::kTst);
    CHECK(i.mnemonic != nullptr && std::strcmp(i.mnemonic, "tst") == 0);
    CHECK(i.operand_count == 2);
    reg(i.operands[0], Gpr::k15, 64);
    imm(i.operands[1], 1);
    CHECK(i.sets_flags);
    CHECK(!i.writeback);

    i = at(0x6a01001fu);
    CHECK(!i.unsupported);
    CHECK(i.opcode == Opcode::kTst);
    CHECK(i.operand_count == 2);
    reg(i.operands[0], Gpr::k0, 32);
    reg(i.operands[1], Gpr::k1, 32);
    CHECK(i.sets_flags);
}

void test_moves_and_shifts()
{
    Insn i = at(0xd2824680u);
    CHECK(i.opcode == Opcode::kMovz);
    CHECK(i.operand_count == 2);
    reg(i.operands[0], Gpr::k0, 64);
    imm(i.operands[1], 0x1234);
    CHECK(i.operands[1].shift == ShiftType::kNone);

    i = at(0x52a24680u);
    CHECK(i.opcode == Opcode::kMovz);
    reg(i.operands[0], Gpr::k0, 32);
    imm(i.operands[1], 0x1234);
    CHECK(i.operands[1].shift == ShiftType::kLsl);
    CHECK(i.operands[1].shift_amount == 16);

    i = at(0xf2cacf00u);
    CHECK(i.opcode == Opcode::kMovk);
    reg(i.operands[0], Gpr::k0, 64);
    imm(i.operands[1], 0x5678);
    CHECK(i.operands[1].shift == ShiftType::kLsl);
    CHECK(i.operands[1].shift_amount == 32);

    i = at(0xd37cec20u);
    CHECK(i.opcode == Opcode::kLsl);
    CHECK(i.operand_count == 3);
    reg(i.operands[0], Gpr::k0, 64);
    reg(i.operands[1], Gpr::k1, 64);
    imm(i.operands[2], 4);
}

void test_conditions()
{
    Insn i = at(0x9a820020u);
    CHECK(i.opcode == Opcode::kCsel);
    CHECK(i.operand_count == 4);
    reg(i.operands[0], Gpr::k0, 64);
    reg(i.operands[1], Gpr::k1, 64);
    reg(i.operands[2], Gpr::k2, 64);
    cond(i.operands[3], Cond::kEq);
    CHECK(i.cond == Cond::kEq);
    CHECK(i.reads_flags);
    CHECK(!i.sets_flags);

    i = at(0xfa410000u);
    CHECK(i.opcode == Opcode::kCcmp);
    CHECK(i.operand_count == 4);
    reg(i.operands[0], Gpr::k0, 64);
    reg(i.operands[1], Gpr::k1, 64);
    imm(i.operands[2], 0);
    cond(i.operands[3], Cond::kEq);
    CHECK(i.cond == Cond::kEq);
    CHECK(i.reads_flags);
    CHECK(i.sets_flags);
}

void test_branches()
{
    Insn i = at(0x54000060u);
    CHECK(i.opcode == Opcode::kBCond);
    CHECK(i.operand_count == 2);
    imm(i.operands[0], 0x100c);
    cond(i.operands[1], Cond::kEq);
    CHECK(i.cond == Cond::kEq);
    CHECK(i.is_branch);
    CHECK(i.has_target);
    CHECK(i.target == 0x100c);
    CHECK(!i.is_call);
    CHECK(i.reads_flags);

    i = at(0x34000060u);
    CHECK(i.opcode == Opcode::kCbz);
    CHECK(i.operand_count == 2);
    reg(i.operands[0], Gpr::k0, 32);
    imm(i.operands[1], 0x100c);
    CHECK(i.target == 0x100c);
    CHECK(i.is_branch);
    CHECK(i.has_target);

    i = at(0x36180061u);
    CHECK(i.opcode == Opcode::kTbz);
    CHECK(i.operand_count == 3);
    reg(i.operands[0], Gpr::k1, 32);
    imm(i.operands[1], 3);
    imm(i.operands[2], 0x100c);
    CHECK(i.target == 0x100c);
    CHECK(i.is_branch);
    CHECK(i.has_target);

    i = at(0x10000060u);
    CHECK(i.opcode == Opcode::kAdr);
    CHECK(i.operand_count == 2);
    reg(i.operands[0], Gpr::k0, 64);
    imm(i.operands[1], 0x100c);
    CHECK(!i.is_branch);
    CHECK(!i.has_target);

    i = at(0xd0000000u);
    CHECK(i.opcode == Opcode::kAdrp);
    CHECK(i.operand_count == 2);
    reg(i.operands[0], Gpr::k0, 64);
    imm(i.operands[1], 0x3000);
    CHECK(!i.is_branch);

    i = at(0x14000003u);
    CHECK(i.opcode == Opcode::kB);
    CHECK(i.operand_count == 1);
    imm(i.operands[0], 0x100c);
    CHECK(i.target == 0x100c);
    CHECK(i.is_branch);
    CHECK(i.has_target);
    CHECK(!i.is_call);

    i = at(0x94000003u);
    CHECK(i.opcode == Opcode::kBl);
    CHECK(i.operand_count == 1);
    imm(i.operands[0], 0x100c);
    CHECK(i.target == 0x100c);
    CHECK(i.is_branch);
    CHECK(i.is_call);

    i = at(0xd61f0000u);
    CHECK(i.opcode == Opcode::kBr);
    CHECK(i.operand_count == 1);
    reg(i.operands[0], Gpr::k0, 64);
    CHECK(i.is_branch);
    CHECK(!i.has_target);
    CHECK(!i.is_call);

    i = at(0xd63f0020u);
    CHECK(i.opcode == Opcode::kBlr);
    CHECK(i.operand_count == 1);
    reg(i.operands[0], Gpr::k1, 64);
    CHECK(i.is_branch);
    CHECK(i.is_call);
    CHECK(!i.has_target);

    i = at(0xd65f03c0u);
    CHECK(i.opcode == Opcode::kRet);
    CHECK(i.operand_count == 0);
    CHECK(i.is_branch);
    CHECK(i.is_return);
    CHECK(!i.has_target);
}

void test_memory()
{
    Insn i = at(0xf9400020u);
    CHECK(i.opcode == Opcode::kLdr);
    CHECK(i.operand_count == 2);
    reg(i.operands[0], Gpr::k0, 64);
    mem(i.operands[1], Gpr::k1, 0);
    CHECK(i.access_width == 8);
    CHECK(i.access_count == 1);
    CHECK(!i.writeback);
    CHECK(!i.post_index);

    i = at(0xb9000862u);
    CHECK(i.opcode == Opcode::kStr);
    CHECK(i.operand_count == 2);
    reg(i.operands[0], Gpr::k2, 32);
    mem(i.operands[1], Gpr::k3, 8);
    CHECK(i.access_width == 4);
    CHECK(i.access_count == 1);

    i = at(0xf8410c20u);
    CHECK(i.opcode == Opcode::kLdr);
    CHECK(i.operand_count == 2);
    reg(i.operands[0], Gpr::k0, 64);
    mem(i.operands[1], Gpr::k1, 16);
    CHECK(i.writeback);
    CHECK(!i.post_index);
    CHECK(i.writeback_disp == 16);
    CHECK(i.access_width == 8);

    i = at(0xf8408420u);
    CHECK(i.opcode == Opcode::kLdr);
    CHECK(i.operand_count == 3);
    reg(i.operands[0], Gpr::k0, 64);
    mem(i.operands[1], Gpr::k1, 0);
    imm(i.operands[2], 8);
    CHECK(i.writeback);
    CHECK(i.post_index);
    CHECK(i.writeback_disp == 8);

    i = at(0xf8626820u);
    CHECK(i.opcode == Opcode::kLdr);
    CHECK(i.operand_count == 2);
    reg(i.operands[0], Gpr::k0, 64);
    mem(i.operands[1], Gpr::k1, 0);
    CHECK(i.operands[1].kind == OperandKind::kMem);
    CHECK(i.operands[1].index.gpr == Gpr::k2);
    CHECK(i.operands[1].index.width == 64);
    CHECK(i.operands[1].shift == ShiftType::kNone);
    CHECK(i.operands[1].extend == ExtendType::kNone);

    i = at(0xf8627820u);
    CHECK(i.opcode == Opcode::kLdr);
    mem(i.operands[1], Gpr::k1, 0);
    CHECK(i.operands[1].kind == OperandKind::kMem);
    CHECK(i.operands[1].index.gpr == Gpr::k2);
    CHECK(i.operands[1].index.width == 64);
    CHECK(i.operands[1].shift == ShiftType::kLsl);
    CHECK(i.operands[1].shift_amount == 3);
    CHECK(i.operands[1].extend == ExtendType::kNone);
    CHECK(i.access_width == 8);

    i = at(0xf8625820u);
    CHECK(i.opcode == Opcode::kLdr);
    mem(i.operands[1], Gpr::k1, 0);
    CHECK(i.operands[1].kind == OperandKind::kMem);
    CHECK(i.operands[1].index.gpr == Gpr::k2);
    CHECK(i.operands[1].index.width == 32);
    CHECK(i.operands[1].extend == ExtendType::kUxtw);
    CHECK(i.operands[1].extend_amount == 3);
    CHECK(i.operands[1].shift == ShiftType::kNone);
    CHECK(i.access_width == 8);

    i = at(0x18000060u);
    CHECK(i.opcode == Opcode::kLdr);
    CHECK(i.operand_count == 2);
    reg(i.operands[0], Gpr::k0, 32);
    imm(i.operands[1], 0x100c);
    CHECK(i.access_width == 4);
    CHECK(i.access_count == 1);
    CHECK(!i.writeback);

    i = at(0xa9410440u);
    CHECK(i.opcode == Opcode::kLdp);
    CHECK(i.operand_count == 3);
    reg(i.operands[0], Gpr::k0, 64);
    reg(i.operands[1], Gpr::k1, 64);
    mem(i.operands[2], Gpr::k2, 16);
    CHECK(i.access_width == 8);
    CHECK(i.access_count == 2);

    i = at(0xa9bf0440u);
    CHECK(i.opcode == Opcode::kStp);
    CHECK(i.operand_count == 3);
    reg(i.operands[0], Gpr::k0, 64);
    reg(i.operands[1], Gpr::k1, 64);
    mem(i.operands[2], Gpr::k2, -16);
    CHECK(i.writeback);
    CHECK(!i.post_index);
    CHECK(i.writeback_disp == -16);
    CHECK(i.access_width == 8);
    CHECK(i.access_count == 2);

    i = at(0x39400420u);
    CHECK(i.opcode == Opcode::kLdrb);
    CHECK(i.operand_count == 2);
    reg(i.operands[0], Gpr::k0, 32);
    mem(i.operands[1], Gpr::k1, 1);
    CHECK(i.access_width == 1);

    i = at(0x79400420u);
    CHECK(i.opcode == Opcode::kLdrh);
    reg(i.operands[0], Gpr::k0, 32);
    mem(i.operands[1], Gpr::k1, 2);
    CHECK(i.access_width == 2);

    i = at(0x39800020u);
    CHECK(i.opcode == Opcode::kLdrsb);
    reg(i.operands[0], Gpr::k0, 64);
    mem(i.operands[1], Gpr::k1, 0);
    CHECK(i.access_width == 1);

    i = at(0xb9800420u);
    CHECK(i.opcode == Opcode::kLdrsw);
    reg(i.operands[0], Gpr::k0, 64);
    mem(i.operands[1], Gpr::k1, 4);
    CHECK(i.access_width == 4);

    i = at(0xf94007e0u);
    CHECK(i.opcode == Opcode::kLdr);
    reg(i.operands[0], Gpr::k0, 64);
    mem(i.operands[1], Gpr::kSp, 8);
    CHECK(i.access_width == 8);

    i = at(0xf90007ffu);
    CHECK(i.opcode == Opcode::kStr);
    reg(i.operands[0], Gpr::kZr, 64);
    mem(i.operands[1], Gpr::kSp, 8);
}

void test_system()
{
    Insn i = at(0xd53b4200u);
    CHECK(i.opcode == Opcode::kMrs);
    CHECK(i.operand_count == 2);
    reg(i.operands[0], Gpr::k0, 64);
    sysreg(i.operands[1], 0xda10);
    CHECK(!i.unsupported);

    i = at(0xd51b4200u);
    CHECK(i.opcode == Opcode::kMsr);
    CHECK(i.operand_count == 2);
    sysreg(i.operands[0], 0xda10);
    reg(i.operands[1], Gpr::k0, 64);

    i = at(0xd4400000u);
    CHECK(i.opcode == Opcode::kHlt);
    CHECK(i.operand_count == 1);
    imm(i.operands[0], 0);
    CHECK(i.trap);
    CHECK(!i.unsupported);

    i = at(0xd503205fu);
    CHECK(i.opcode == Opcode::kWfe);
    CHECK(i.operand_count == 0);
    CHECK(!i.unsupported);
}

void test_unsupported()
{
    Insn i = at(0x00000000u);
    CHECK(i.unsupported);
    CHECK(i.opcode == Opcode::kUnsupported);
    CHECK(i.operand_count == 0);
    CHECK(i.pc == 0x1000);
    CHECK(i.raw == 0);
}

} // namespace

int main()
{
    test_arithmetic();
    test_logical();
    test_moves_and_shifts();
    test_conditions();
    test_branches();
    test_memory();
    test_system();
    test_unsupported();

    if (g_failures != 0) {
        std::fprintf(stderr, "decode_test: %d of %d checks failed\n", g_failures, g_checks);
        return 1;
    }
    std::printf("decode_test: %d checks passed\n", g_checks);
    return 0;
}
