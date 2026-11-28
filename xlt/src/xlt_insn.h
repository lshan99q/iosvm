// SPDX-License-Identifier: MIT
/*
 * xlt_insn.h — the architecture-neutral instruction model shared by the decoder, the lifter and
 * the JIT.
 *
 * The decoder fills one Insn per 32-bit AArch64 instruction. Nothing in this header mentions
 * capstone: the decoder's backend may change without changing the lifters, and the model is
 * deliberately small enough to be produced by a table-driven decoder later.
 *
 * Conventions:
 *   - pc-relative operands (branches, ADR/ADRP and literal loads) are stored as absolute guest
 *     virtual addresses, not as byte offsets;
 *   - `access_width` is the number of bytes transferred by one element of a load/store, while
 *     `access_count` is the number of registers transferred (1, or 2 for LDP/STP);
 *   - post-indexed addressing is represented by a MEM operand with disp 0 followed by the
 *     trailing immediate operand; `post_index` and `writeback_disp` make that explicit;
 *   - an unsupported instruction is still returned with pc/raw filled in and `unsupported` set,
 *     so a caller can diagnose or skip exactly four bytes without looping.
 */
#ifndef XLT_INSN_H
#define XLT_INSN_H

#include <cstdint>

namespace xlt
{

/* General-purpose register identity. kSp is the stack pointer and kZr the zero register; the
 * 32/64-bit view lives in RegRef::width, so there is exactly one kZr for both WZR and XZR. */
enum class Gpr : uint8_t {
    k0,
    k1,
    k2,
    k3,
    k4,
    k5,
    k6,
    k7,
    k8,
    k9,
    k10,
    k11,
    k12,
    k13,
    k14,
    k15,
    k16,
    k17,
    k18,
    k19,
    k20,
    k21,
    k22,
    k23,
    k24,
    k25,
    k26,
    k27,
    k28,
    k29,
    k30,
    kSp,
    kZr,
};

/* A register reference. width is 32 or 64; width == 0 means "not present" (an absent memory
 * index, for example). */
struct RegRef {
    Gpr gpr = Gpr::k0;
    uint8_t width = 0;

    constexpr bool valid() const
    {
        return width == 32 || width == 64;
    }
    constexpr bool is_zero() const
    {
        return gpr == Gpr::kZr;
    }
    constexpr bool is_sp() const
    {
        return gpr == Gpr::kSp;
    }
};

/* AArch64 condition codes, in architectural encoding order (EQ == 0 ... NV == 15). The decoder
 * maps capstone's one-based cc values onto this enum. kAl means "no condition" for instructions
 * that do not read the flags. */
enum class Cond : uint8_t {
    kEq = 0,
    kNe = 1,
    kHs = 2,
    kLo = 3,
    kMi = 4,
    kPl = 5,
    kVs = 6,
    kVc = 7,
    kHi = 8,
    kLs = 9,
    kGe = 10,
    kLt = 11,
    kGt = 12,
    kLe = 13,
    kAl = 14,
    kNv = 15,
};

enum class ShiftType : uint8_t {
    kNone = 0,
    kLsl,
    kLsr,
    kAsr,
    kRor,
    kMsl,
};

/* AArch64 extenders. The values are the architectural ones (not capstone's), so a later decoder
 * can fill this field straight from the instruction word. */
enum class ExtendType : uint8_t {
    kNone = 0,
    kUxtb = 1,
    kUxth = 2,
    kUxtw = 3,
    kUxtx = 4,
    kSxtb = 5,
    kSxth = 6,
    kSxtw = 7,
    kSxtx = 8,
};

enum class OperandKind : uint8_t {
    kNone = 0,
    kReg,
    kImm,
    kMem,
    kSysReg,
    kCond,
    kPstate,
};

/* One operand. Only the members selected by `kind` are meaningful:
 *   kReg     -> reg
 *   kImm     -> imm (absolute address for pc-relative forms)
 *   kMem     -> base, index (may be !valid), disp, shift/shift_amount or extend/extend_amount
 *   kSysReg  -> sysreg (the raw MRS/MSR encoding, e.g. NZCV == 0xda10)
 *   kCond    -> cond
 *   kPstate  -> pstate (raw capstone PSTATE field, e.g. SPSel/DAIFSet/DAIFClr)
 * Shift and extend are mutually exclusive; the decoder clears `shift` when it sets `extend`. */
struct Operand {
    OperandKind kind = OperandKind::kNone;
    RegRef reg;
    RegRef base;
    RegRef index;
    uint64_t imm = 0;
    int64_t disp = 0;
    ShiftType shift = ShiftType::kNone;
    uint8_t shift_amount = 0;
    ExtendType extend = ExtendType::kNone;
    uint8_t extend_amount = 0;
    uint16_t sysreg = 0;
    uint16_t pstate = 0;
    Cond cond = Cond::kAl;
};

/* The neutral opcode set M1 needs. Instructions outside this set are reported as kUnsupported
 * rather than guessed at. `sets_flags` distinguishes ADD from ADDS, SUB from SUBS, etc. */
enum class Opcode : uint16_t {
    kUnsupported = 0,
    kNop,

    /* Moves. */
    kMov,
    kMovz,
    kMovn,
    kMovk,

    /* Integer arithmetic. */
    kAdd,
    kSub,
    kCmp,
    kCmn,
    kNeg,
    kAdc,
    kSbc,
    kMul,
    kMadd,
    kMsub,
    kSmulh,
    kUmulh,
    kSdiv,
    kUdiv,

    /* Logical. */
    kAnd,
    kOrr,
    kEor,
    kOrn,
    kBic,
    kMvn,
    kTst,

    /* Shifts and bitfields. */
    kLsl,
    kLsr,
    kAsr,
    kRor,
    kUbfm,
    kSbfm,
    kBfm,
    kUbfx,
    kSbfx,
    kBfxil,
    kExtr,
    kClz,
    kRbit,
    kRev,
    kRev16,
    kRev32,

    /* Conditional select and compare. */
    kCsel,
    kCsinc,
    kCsinv,
    kCsneg,
    kCset,
    kCsetm,
    kCinc,
    kCinv,
    kCneg,
    kCcmp,
    kCcmn,

    /* Control flow and pc-relative address formation. */
    kB,
    kBCond,
    kBl,
    kBr,
    kBlr,
    kRet,
    kCbz,
    kCbnz,
    kTbz,
    kTbnz,
    kAdr,
    kAdrp,

    /* Loads and stores. */
    kLdr,
    kStr,
    kLdrb,
    kStrb,
    kLdrh,
    kStrh,
    kLdrsb,
    kLdrsh,
    kLdrsw,
    kLdp,
    kStp,
    kLdpsw,
    kLdur,
    kStur,
    kLdurb,
    kSturb,
    kLdurh,
    kSturh,
    kLdursb,
    kLdursh,
    kLdursw,

    /* System and hints. */
    kMrs,
    kMsr,
    kHlt,
    kBrk,
    kSvc,
    kHvc,
    kSmc,
    kEret,
    kTlbi,
    kDc,
    kIc,
    kWfe,
    kWfi,
    kSev,
    kSevl,
    kYield,
    kIsb,
    kDsb,
    kDmb,
};

constexpr uint8_t kMaxOperands = 8;

/* One decoded instruction. */
struct Insn {
    uint64_t pc = 0;
    uint32_t raw = 0;
    Opcode opcode = Opcode::kUnsupported;
    const char *mnemonic = "unsupported";
    uint8_t operand_count = 0;
    Operand operands[kMaxOperands];

    /* Memory access size: bytes per element (1/2/4/8) and number of registers (1 or 2). */
    uint8_t access_width = 0;
    uint8_t access_count = 0;

    /* Condition of the instruction, kAl when none. It is also appended as a kCond operand for
     * instructions whose assembly syntax carries one. */
    Cond cond = Cond::kAl;

    bool sets_flags = false;
    bool reads_flags = false;

    /* Writeback and post-index addressing. */
    bool writeback = false;
    bool post_index = false;
    int64_t writeback_disp = 0;

    /* Control flow. Direct branches have has_target and target set; BR/BLR/RET only have the
     * register operand. */
    bool is_branch = false;
    bool is_call = false;
    bool is_return = false;
    bool has_target = false;
    uint64_t target = 0;

    /* HLT/BRK/SVC raise a guest trap; the lifter decides whether it can resume. */
    bool trap = false;

    bool unsupported = false;
};

/* Canonical, static name for an opcode (e.g. "add", "b.cond", "ldp"). */
const char *opcode_name(Opcode opcode);

} // namespace xlt

#endif /* XLT_INSN_H */
