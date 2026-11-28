// SPDX-License-Identifier: MIT
/*
 * decode.cpp — the capstone 4 backend for xlt::decode().
 *
 * The capstone handle and its reusable cs_insn live in thread-local storage: decoding is a hot
 * path, and cs_disasm_iter() can decode into a caller-provided instruction without allocating.
 * Every architectural field is copied into the neutral model before the next call overwrites the
 * capstone buffer; no capstone pointer escapes this file.
 */
#include "decode.h"

#include <capstone/capstone.h>

namespace xlt
{
namespace
{

/* A capstone handle and one reusable disassembly buffer per thread. */
struct DecoderState {
    csh handle = 0;
    cs_insn *insn = nullptr;

    DecoderState()
    {
        if (cs_open(CS_ARCH_ARM64, CS_MODE_ARM, &handle) != CS_ERR_OK) {
            handle = 0;
            return;
        }
        if (cs_option(handle, CS_OPT_DETAIL, CS_OPT_ON) != CS_ERR_OK) {
            cs_close(&handle);
            handle = 0;
            return;
        }
        insn = cs_malloc(handle);
        if (insn == nullptr) {
            cs_close(&handle);
            handle = 0;
        }
    }

    ~DecoderState()
    {
        if (insn != nullptr) {
            cs_free(insn, 1);
        }
        if (handle != 0) {
            cs_close(&handle);
        }
    }

    bool ok() const
    {
        return handle != 0 && insn != nullptr;
    }
};

DecoderState &decoder_state()
{
    thread_local DecoderState state;
    return state;
}

/* Map capstone's arm64_reg enum onto the neutral register model. Vector and predicate registers
 * are not part of M1, so they become an invalid RegRef and the instruction is unsupported. */
RegRef map_reg(arm64_reg reg)
{
    switch (reg) {
    case ARM64_REG_SP:
        return {Gpr::kSp, 64};
    case ARM64_REG_WSP:
        return {Gpr::kSp, 32};
    case ARM64_REG_XZR:
        return {Gpr::kZr, 64};
    case ARM64_REG_WZR:
        return {Gpr::kZr, 32};

#define XLT_W_CASE(n)                                                                              \
    case ARM64_REG_W##n:                                                                           \
        return {Gpr::k##n, 32};
#define XLT_X_CASE(n)                                                                              \
    case ARM64_REG_X##n:                                                                           \
        return {Gpr::k##n, 64};
        XLT_W_CASE(0)
        XLT_W_CASE(1)
        XLT_W_CASE(2)
        XLT_W_CASE(3)
        XLT_W_CASE(4)
        XLT_W_CASE(5)
        XLT_W_CASE(6)
        XLT_W_CASE(7)
        XLT_W_CASE(8)
        XLT_W_CASE(9)
        XLT_W_CASE(10)
        XLT_W_CASE(11)
        XLT_W_CASE(12)
        XLT_W_CASE(13)
        XLT_W_CASE(14)
        XLT_W_CASE(15)
        XLT_W_CASE(16)
        XLT_W_CASE(17)
        XLT_W_CASE(18)
        XLT_W_CASE(19)
        XLT_W_CASE(20)
        XLT_W_CASE(21)
        XLT_W_CASE(22)
        XLT_W_CASE(23)
        XLT_W_CASE(24)
        XLT_W_CASE(25)
        XLT_W_CASE(26)
        XLT_W_CASE(27)
        XLT_W_CASE(28)
        XLT_W_CASE(29)
        XLT_W_CASE(30)
        XLT_X_CASE(0)
        XLT_X_CASE(1)
        XLT_X_CASE(2)
        XLT_X_CASE(3)
        XLT_X_CASE(4)
        XLT_X_CASE(5)
        XLT_X_CASE(6)
        XLT_X_CASE(7)
        XLT_X_CASE(8)
        XLT_X_CASE(9)
        XLT_X_CASE(10)
        XLT_X_CASE(11)
        XLT_X_CASE(12)
        XLT_X_CASE(13)
        XLT_X_CASE(14)
        XLT_X_CASE(15)
        XLT_X_CASE(16)
        XLT_X_CASE(17)
        XLT_X_CASE(18)
        XLT_X_CASE(19)
        XLT_X_CASE(20)
        XLT_X_CASE(21)
        XLT_X_CASE(22)
        XLT_X_CASE(23)
        XLT_X_CASE(24)
        XLT_X_CASE(25)
        XLT_X_CASE(26)
        XLT_X_CASE(27)
        XLT_X_CASE(28)
        XLT_X_CASE(29)
        XLT_X_CASE(30)
#undef XLT_W_CASE
#undef XLT_X_CASE
    default:
        return {};
    }
}

Cond map_cond(arm64_cc cc)
{
    switch (cc) {
    case ARM64_CC_EQ:
        return Cond::kEq;
    case ARM64_CC_NE:
        return Cond::kNe;
    case ARM64_CC_HS:
        return Cond::kHs;
    case ARM64_CC_LO:
        return Cond::kLo;
    case ARM64_CC_MI:
        return Cond::kMi;
    case ARM64_CC_PL:
        return Cond::kPl;
    case ARM64_CC_VS:
        return Cond::kVs;
    case ARM64_CC_VC:
        return Cond::kVc;
    case ARM64_CC_HI:
        return Cond::kHi;
    case ARM64_CC_LS:
        return Cond::kLs;
    case ARM64_CC_GE:
        return Cond::kGe;
    case ARM64_CC_LT:
        return Cond::kLt;
    case ARM64_CC_GT:
        return Cond::kGt;
    case ARM64_CC_LE:
        return Cond::kLe;
    case ARM64_CC_AL:
        return Cond::kAl;
    case ARM64_CC_NV:
        return Cond::kNv;
    default:
        return Cond::kAl;
    }
}

ShiftType map_shift(arm64_shifter shift)
{
    switch (shift) {
    case ARM64_SFT_LSL:
        return ShiftType::kLsl;
    case ARM64_SFT_LSR:
        return ShiftType::kLsr;
    case ARM64_SFT_ASR:
        return ShiftType::kAsr;
    case ARM64_SFT_ROR:
        return ShiftType::kRor;
    case ARM64_SFT_MSL:
        return ShiftType::kMsl;
    default:
        return ShiftType::kNone;
    }
}

ExtendType map_extend(arm64_extender extend)
{
    switch (extend) {
    case ARM64_EXT_UXTB:
        return ExtendType::kUxtb;
    case ARM64_EXT_UXTH:
        return ExtendType::kUxth;
    case ARM64_EXT_UXTW:
        return ExtendType::kUxtw;
    case ARM64_EXT_UXTX:
        return ExtendType::kUxtx;
    case ARM64_EXT_SXTB:
        return ExtendType::kSxtb;
    case ARM64_EXT_SXTH:
        return ExtendType::kSxth;
    case ARM64_EXT_SXTW:
        return ExtendType::kSxtw;
    case ARM64_EXT_SXTX:
        return ExtendType::kSxtx;
    default:
        return ExtendType::kNone;
    }
}

/* Copy the shift/extend annotation capstone puts on a register, immediate or memory operand.
 * Capstone reuses shift.value as the extend amount, so the neutral model keeps the two concepts
 * separate and never reports both. */
void copy_shift_extend(Operand *dst, const cs_arm64_op &src)
{
    if (src.ext != ARM64_EXT_INVALID) {
        dst->extend = map_extend(src.ext);
        dst->extend_amount = static_cast<uint8_t>(src.shift.value);
        dst->shift = ShiftType::kNone;
        dst->shift_amount = 0;
        return;
    }
    if (src.shift.type != ARM64_SFT_INVALID) {
        dst->shift = map_shift(src.shift.type);
        dst->shift_amount = static_cast<uint8_t>(src.shift.value);
    }
}

Opcode map_opcode(unsigned int id)
{
    switch (id) {
    case ARM64_INS_NOP:
        return Opcode::kNop;

    case ARM64_INS_MOV:
        return Opcode::kMov;
    case ARM64_INS_MOVZ:
        return Opcode::kMovz;
    case ARM64_INS_MOVN:
        return Opcode::kMovn;
    case ARM64_INS_MOVK:
        return Opcode::kMovk;

    case ARM64_INS_ADD:
        return Opcode::kAdd;
    case ARM64_INS_SUB:
        return Opcode::kSub;
    case ARM64_INS_CMP:
        return Opcode::kCmp;
    case ARM64_INS_CMN:
        return Opcode::kCmn;
    case ARM64_INS_NEG:
    case ARM64_INS_NEGS:
        return Opcode::kNeg;
    case ARM64_INS_ADC:
        return Opcode::kAdc;
    case ARM64_INS_SBC:
        return Opcode::kSbc;
    case ARM64_INS_MUL:
        return Opcode::kMul;
    case ARM64_INS_MADD:
        return Opcode::kMadd;
    case ARM64_INS_MSUB:
        return Opcode::kMsub;
    case ARM64_INS_SMULH:
        return Opcode::kSmulh;
    case ARM64_INS_UMULH:
        return Opcode::kUmulh;
    case ARM64_INS_SDIV:
        return Opcode::kSdiv;
    case ARM64_INS_UDIV:
        return Opcode::kUdiv;

    case ARM64_INS_AND:
        return Opcode::kAnd;
    case ARM64_INS_ORR:
        return Opcode::kOrr;
    case ARM64_INS_EOR:
        return Opcode::kEor;
    case ARM64_INS_ORN:
        return Opcode::kOrn;
    case ARM64_INS_BIC:
        return Opcode::kBic;
    case ARM64_INS_MVN:
        return Opcode::kMvn;
    case ARM64_INS_TST:
        return Opcode::kTst;

    case ARM64_INS_LSL:
        return Opcode::kLsl;
    case ARM64_INS_LSR:
        return Opcode::kLsr;
    case ARM64_INS_ASR:
        return Opcode::kAsr;
    case ARM64_INS_ROR:
        return Opcode::kRor;
    case ARM64_INS_UBFM:
        return Opcode::kUbfm;
    case ARM64_INS_SBFM:
        return Opcode::kSbfm;
    case ARM64_INS_BFM:
        return Opcode::kBfm;
    case ARM64_INS_UBFX:
        return Opcode::kUbfx;
    case ARM64_INS_SBFX:
        return Opcode::kSbfx;
    case ARM64_INS_BFXIL:
        return Opcode::kBfxil;
    case ARM64_INS_EXTR:
        return Opcode::kExtr;
    case ARM64_INS_CLZ:
        return Opcode::kClz;
    case ARM64_INS_RBIT:
        return Opcode::kRbit;
    case ARM64_INS_REV:
        return Opcode::kRev;
    case ARM64_INS_REV16:
        return Opcode::kRev16;
    case ARM64_INS_REV32:
        return Opcode::kRev32;

    case ARM64_INS_CSEL:
        return Opcode::kCsel;
    case ARM64_INS_CSINC:
        return Opcode::kCsinc;
    case ARM64_INS_CSINV:
        return Opcode::kCsinv;
    case ARM64_INS_CSNEG:
        return Opcode::kCsneg;
    case ARM64_INS_CSET:
        return Opcode::kCset;
    case ARM64_INS_CSETM:
        return Opcode::kCsetm;
    case ARM64_INS_CINC:
        return Opcode::kCinc;
    case ARM64_INS_CINV:
        return Opcode::kCinv;
    case ARM64_INS_CNEG:
        return Opcode::kCneg;
    case ARM64_INS_CCMP:
        return Opcode::kCcmp;
    case ARM64_INS_CCMN:
        return Opcode::kCcmn;

    case ARM64_INS_B:
        return Opcode::kB;
    case ARM64_INS_BL:
        return Opcode::kBl;
    case ARM64_INS_BR:
        return Opcode::kBr;
    case ARM64_INS_BLR:
        return Opcode::kBlr;
    case ARM64_INS_RET:
        return Opcode::kRet;
    case ARM64_INS_CBZ:
        return Opcode::kCbz;
    case ARM64_INS_CBNZ:
        return Opcode::kCbnz;
    case ARM64_INS_TBZ:
        return Opcode::kTbz;
    case ARM64_INS_TBNZ:
        return Opcode::kTbnz;
    case ARM64_INS_ADR:
        return Opcode::kAdr;
    case ARM64_INS_ADRP:
        return Opcode::kAdrp;

    case ARM64_INS_LDR:
        return Opcode::kLdr;
    case ARM64_INS_STR:
        return Opcode::kStr;
    case ARM64_INS_LDRB:
        return Opcode::kLdrb;
    case ARM64_INS_STRB:
        return Opcode::kStrb;
    case ARM64_INS_LDRH:
        return Opcode::kLdrh;
    case ARM64_INS_STRH:
        return Opcode::kStrh;
    case ARM64_INS_LDRSB:
        return Opcode::kLdrsb;
    case ARM64_INS_LDRSH:
        return Opcode::kLdrsh;
    case ARM64_INS_LDRSW:
        return Opcode::kLdrsw;
    case ARM64_INS_LDP:
        return Opcode::kLdp;
    case ARM64_INS_STP:
        return Opcode::kStp;
    case ARM64_INS_LDPSW:
        return Opcode::kLdpsw;
    case ARM64_INS_LDUR:
        return Opcode::kLdur;
    case ARM64_INS_STUR:
        return Opcode::kStur;
    case ARM64_INS_LDURB:
        return Opcode::kLdurb;
    case ARM64_INS_STURB:
        return Opcode::kSturb;
    case ARM64_INS_LDURH:
        return Opcode::kLdurh;
    case ARM64_INS_STURH:
        return Opcode::kSturh;
    case ARM64_INS_LDURSB:
        return Opcode::kLdursb;
    case ARM64_INS_LDURSH:
        return Opcode::kLdursh;
    case ARM64_INS_LDURSW:
        return Opcode::kLdursw;

    case ARM64_INS_MRS:
        return Opcode::kMrs;
    case ARM64_INS_MSR:
        return Opcode::kMsr;
    case ARM64_INS_HLT:
        return Opcode::kHlt;
    case ARM64_INS_BRK:
        return Opcode::kBrk;
    case ARM64_INS_SVC:
        return Opcode::kSvc;
    case ARM64_INS_WFE:
        return Opcode::kWfe;
    case ARM64_INS_WFI:
        return Opcode::kWfi;
    case ARM64_INS_SEV:
        return Opcode::kSev;
    case ARM64_INS_SEVL:
        return Opcode::kSevl;
    case ARM64_INS_YIELD:
        return Opcode::kYield;
    case ARM64_INS_ISB:
        return Opcode::kIsb;
    case ARM64_INS_DSB:
        return Opcode::kDsb;
    case ARM64_INS_DMB:
        return Opcode::kDmb;

    default:
        return Opcode::kUnsupported;
    }
}

bool has_cond_operand(Opcode opcode)
{
    switch (opcode) {
    case Opcode::kBCond:
    case Opcode::kCsel:
    case Opcode::kCsinc:
    case Opcode::kCsinv:
    case Opcode::kCsneg:
    case Opcode::kCset:
    case Opcode::kCsetm:
    case Opcode::kCinc:
    case Opcode::kCinv:
    case Opcode::kCneg:
    case Opcode::kCcmp:
    case Opcode::kCcmn:
        return true;
    default:
        return false;
    }
}

void set_access_width(Insn *insn)
{
    if (insn->operand_count == 0) {
        return;
    }
    const Operand &dst = insn->operands[0];
    const uint8_t reg_width = dst.kind == OperandKind::kReg ? dst.reg.width : 0;

    switch (insn->opcode) {
    case Opcode::kLdr:
    case Opcode::kStr:
    case Opcode::kLdur:
    case Opcode::kStur:
        insn->access_width = static_cast<uint8_t>(reg_width / 8);
        insn->access_count = 1;
        break;
    case Opcode::kLdp:
    case Opcode::kStp:
        insn->access_width = static_cast<uint8_t>(reg_width / 8);
        insn->access_count = 2;
        break;
    case Opcode::kLdpsw:
        insn->access_width = 4;
        insn->access_count = 2;
        break;
    case Opcode::kLdrb:
    case Opcode::kStrb:
    case Opcode::kLdrsb:
    case Opcode::kLdurb:
    case Opcode::kSturb:
    case Opcode::kLdursb:
        insn->access_width = 1;
        insn->access_count = 1;
        break;
    case Opcode::kLdrh:
    case Opcode::kStrh:
    case Opcode::kLdrsh:
    case Opcode::kLdurh:
    case Opcode::kSturh:
    case Opcode::kLdursh:
        insn->access_width = 2;
        insn->access_count = 1;
        break;
    case Opcode::kLdrsw:
    case Opcode::kLdursw:
        insn->access_width = 4;
        insn->access_count = 1;
        break;
    default:
        break;
    }
}

void set_control_flow(Insn *insn)
{
    switch (insn->opcode) {
    case Opcode::kB:
    case Opcode::kBCond:
    case Opcode::kBl:
    case Opcode::kCbz:
    case Opcode::kCbnz:
    case Opcode::kTbz:
    case Opcode::kTbnz:
        insn->is_branch = true;
        insn->is_call = insn->opcode == Opcode::kBl;
        for (int i = static_cast<int>(insn->operand_count) - 1; i >= 0; --i) {
            if (insn->operands[i].kind == OperandKind::kImm) {
                insn->target = insn->operands[i].imm;
                insn->has_target = true;
                break;
            }
        }
        break;
    case Opcode::kBr:
    case Opcode::kBlr:
    case Opcode::kRet:
        insn->is_branch = true;
        insn->is_call = insn->opcode == Opcode::kBlr;
        insn->is_return = insn->opcode == Opcode::kRet;
        break;
    default:
        break;
    }
}

void set_flag_usage(Insn *insn, bool capstone_update_flags)
{
    insn->sets_flags = capstone_update_flags;
    switch (insn->opcode) {
    case Opcode::kTst:
        insn->sets_flags = true;
        break;
    case Opcode::kAdc:
    case Opcode::kSbc:
    case Opcode::kBCond:
    case Opcode::kCsel:
    case Opcode::kCsinc:
    case Opcode::kCsinv:
    case Opcode::kCsneg:
    case Opcode::kCset:
    case Opcode::kCsetm:
    case Opcode::kCinc:
    case Opcode::kCinv:
    case Opcode::kCneg:
    case Opcode::kCcmp:
    case Opcode::kCcmn:
        insn->reads_flags = true;
        break;
    default:
        break;
    }
}

} // namespace

Insn decode(uint64_t pc, uint32_t word)
{
    Insn out;
    out.pc = pc;
    out.raw = word;

    DecoderState &state = decoder_state();
    if (!state.ok()) {
        out.unsupported = true;
        return out;
    }

    uint8_t bytes[4] = {
        static_cast<uint8_t>(word),
        static_cast<uint8_t>(word >> 8),
        static_cast<uint8_t>(word >> 16),
        static_cast<uint8_t>(word >> 24),
    };
    const uint8_t *code = bytes;
    size_t size = sizeof(bytes);
    uint64_t address = pc;
    if (cs_disasm_iter(state.handle, &code, &size, &address, state.insn) == 0 ||
        state.insn->size != 4) {
        out.unsupported = true;
        return out;
    }

    const cs_insn *ci = state.insn;
    const cs_arm64 &detail = ci->detail->arm64;

    Opcode opcode = map_opcode(ci->id);
    if (opcode == Opcode::kB && detail.cc != ARM64_CC_INVALID) {
        opcode = Opcode::kBCond;
    }
    if (opcode == Opcode::kUnsupported) {
        out.unsupported = true;
        return out;
    }
    out.opcode = opcode;
    out.mnemonic = opcode_name(opcode);
    out.cond = map_cond(detail.cc);

    if (detail.op_count > kMaxOperands) {
        out.unsupported = true;
        return out;
    }

    for (uint8_t i = 0; i < detail.op_count; ++i) {
        const cs_arm64_op &src = detail.operands[i];
        Operand &dst = out.operands[out.operand_count];
        switch (src.type) {
        case ARM64_OP_REG:
            dst.kind = OperandKind::kReg;
            dst.reg = map_reg(src.reg);
            if (!dst.reg.valid()) {
                out.unsupported = true;
                return out;
            }
            copy_shift_extend(&dst, src);
            break;
        case ARM64_OP_IMM:
            dst.kind = OperandKind::kImm;
            dst.imm = static_cast<uint64_t>(src.imm);
            copy_shift_extend(&dst, src);
            break;
        case ARM64_OP_MEM:
            dst.kind = OperandKind::kMem;
            dst.base = map_reg(src.mem.base);
            if (!dst.base.valid()) {
                out.unsupported = true;
                return out;
            }
            if (src.mem.index != ARM64_REG_INVALID) {
                dst.index = map_reg(src.mem.index);
                if (!dst.index.valid()) {
                    out.unsupported = true;
                    return out;
                }
            }
            dst.disp = src.mem.disp;
            copy_shift_extend(&dst, src);
            break;
        case ARM64_OP_REG_MRS:
        case ARM64_OP_REG_MSR:
            dst.kind = OperandKind::kSysReg;
            dst.sysreg = static_cast<uint16_t>(src.sys);
            break;
        default:
            /* PSTATE, SYS and FP/SIMD operands are outside M1. */
            out.unsupported = true;
            return out;
        }
        ++out.operand_count;
    }

    set_access_width(&out);
    set_control_flow(&out);
    set_flag_usage(&out, detail.update_flags != 0);

    out.writeback = detail.writeback != 0;
    if (out.writeback) {
        int mem_index = -1;
        for (uint8_t i = 0; i < out.operand_count; ++i) {
            if (out.operands[i].kind == OperandKind::kMem) {
                mem_index = i;
            }
        }
        if (mem_index >= 0) {
            const Operand &last = out.operands[out.operand_count - 1];
            if (static_cast<int>(out.operand_count) - 1 > mem_index &&
                last.kind == OperandKind::kImm) {
                out.post_index = true;
                out.writeback_disp = static_cast<int64_t>(last.imm);
            } else {
                out.writeback_disp = out.operands[mem_index].disp;
            }
        }
    }

    if (has_cond_operand(out.opcode) && detail.cc != ARM64_CC_INVALID) {
        if (out.operand_count >= kMaxOperands) {
            out.unsupported = true;
            return out;
        }
        Operand &cond = out.operands[out.operand_count];
        cond.kind = OperandKind::kCond;
        cond.cond = out.cond;
        ++out.operand_count;
    }

    if (out.opcode == Opcode::kHlt || out.opcode == Opcode::kBrk || out.opcode == Opcode::kSvc) {
        out.trap = true;
    }

    return out;
}

const char *opcode_name(Opcode opcode)
{
    switch (opcode) {
    case Opcode::kUnsupported:
        return "unsupported";
    case Opcode::kNop:
        return "nop";
    case Opcode::kMov:
        return "mov";
    case Opcode::kMovz:
        return "movz";
    case Opcode::kMovn:
        return "movn";
    case Opcode::kMovk:
        return "movk";
    case Opcode::kAdd:
        return "add";
    case Opcode::kSub:
        return "sub";
    case Opcode::kCmp:
        return "cmp";
    case Opcode::kCmn:
        return "cmn";
    case Opcode::kNeg:
        return "neg";
    case Opcode::kAdc:
        return "adc";
    case Opcode::kSbc:
        return "sbc";
    case Opcode::kMul:
        return "mul";
    case Opcode::kMadd:
        return "madd";
    case Opcode::kMsub:
        return "msub";
    case Opcode::kSmulh:
        return "smulh";
    case Opcode::kUmulh:
        return "umulh";
    case Opcode::kSdiv:
        return "sdiv";
    case Opcode::kUdiv:
        return "udiv";
    case Opcode::kAnd:
        return "and";
    case Opcode::kOrr:
        return "orr";
    case Opcode::kEor:
        return "eor";
    case Opcode::kOrn:
        return "orn";
    case Opcode::kBic:
        return "bic";
    case Opcode::kMvn:
        return "mvn";
    case Opcode::kTst:
        return "tst";
    case Opcode::kLsl:
        return "lsl";
    case Opcode::kLsr:
        return "lsr";
    case Opcode::kAsr:
        return "asr";
    case Opcode::kRor:
        return "ror";
    case Opcode::kUbfm:
        return "ubfm";
    case Opcode::kSbfm:
        return "sbfm";
    case Opcode::kBfm:
        return "bfm";
    case Opcode::kUbfx:
        return "ubfx";
    case Opcode::kSbfx:
        return "sbfx";
    case Opcode::kBfxil:
        return "bfxil";
    case Opcode::kExtr:
        return "extr";
    case Opcode::kClz:
        return "clz";
    case Opcode::kRbit:
        return "rbit";
    case Opcode::kRev:
        return "rev";
    case Opcode::kRev16:
        return "rev16";
    case Opcode::kRev32:
        return "rev32";
    case Opcode::kCsel:
        return "csel";
    case Opcode::kCsinc:
        return "csinc";
    case Opcode::kCsinv:
        return "csinv";
    case Opcode::kCsneg:
        return "csneg";
    case Opcode::kCset:
        return "cset";
    case Opcode::kCsetm:
        return "csetm";
    case Opcode::kCinc:
        return "cinc";
    case Opcode::kCinv:
        return "cinv";
    case Opcode::kCneg:
        return "cneg";
    case Opcode::kCcmp:
        return "ccmp";
    case Opcode::kCcmn:
        return "ccmn";
    case Opcode::kB:
        return "b";
    case Opcode::kBCond:
        return "b.cond";
    case Opcode::kBl:
        return "bl";
    case Opcode::kBr:
        return "br";
    case Opcode::kBlr:
        return "blr";
    case Opcode::kRet:
        return "ret";
    case Opcode::kCbz:
        return "cbz";
    case Opcode::kCbnz:
        return "cbnz";
    case Opcode::kTbz:
        return "tbz";
    case Opcode::kTbnz:
        return "tbnz";
    case Opcode::kAdr:
        return "adr";
    case Opcode::kAdrp:
        return "adrp";
    case Opcode::kLdr:
        return "ldr";
    case Opcode::kStr:
        return "str";
    case Opcode::kLdrb:
        return "ldrb";
    case Opcode::kStrb:
        return "strb";
    case Opcode::kLdrh:
        return "ldrh";
    case Opcode::kStrh:
        return "strh";
    case Opcode::kLdrsb:
        return "ldrsb";
    case Opcode::kLdrsh:
        return "ldrsh";
    case Opcode::kLdrsw:
        return "ldrsw";
    case Opcode::kLdp:
        return "ldp";
    case Opcode::kStp:
        return "stp";
    case Opcode::kLdpsw:
        return "ldpsw";
    case Opcode::kLdur:
        return "ldur";
    case Opcode::kStur:
        return "stur";
    case Opcode::kLdurb:
        return "ldurb";
    case Opcode::kSturb:
        return "sturb";
    case Opcode::kLdurh:
        return "ldurh";
    case Opcode::kSturh:
        return "sturh";
    case Opcode::kLdursb:
        return "ldursb";
    case Opcode::kLdursh:
        return "ldursh";
    case Opcode::kLdursw:
        return "ldursw";
    case Opcode::kMrs:
        return "mrs";
    case Opcode::kMsr:
        return "msr";
    case Opcode::kHlt:
        return "hlt";
    case Opcode::kBrk:
        return "brk";
    case Opcode::kSvc:
        return "svc";
    case Opcode::kWfe:
        return "wfe";
    case Opcode::kWfi:
        return "wfi";
    case Opcode::kSev:
        return "sev";
    case Opcode::kSevl:
        return "sevl";
    case Opcode::kYield:
        return "yield";
    case Opcode::kIsb:
        return "isb";
    case Opcode::kDsb:
        return "dsb";
    case Opcode::kDmb:
        return "dmb";
    default:
        return "unsupported";
    }
}

} // namespace xlt
