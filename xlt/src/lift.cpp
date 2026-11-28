// SPDX-License-Identifier: MIT
/*
 * lift.cpp — lower decoded AArch64 instructions to LLVM IR.
 *
 * The register file lives in allocas and is promoted by mem2reg. NZCV is kept lazily: a block
 * only materialises the four i1 values when something reads them, and packs them back into the
 * guest nzcv word when it writes them. Generated loads and stores are bounds checked against the
 * flat guest RAM window; a miss calls the noreturn xlt_bad_mem helper.
 */
#include "lift.h"

#include "decode.h"
#include "xlt_insn.h"

#include "llvm/IR/BasicBlock.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/DerivedTypes.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Intrinsics.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Verifier.h"
#include "llvm/Support/raw_ostream.h"

#include <climits>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <deque>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <tuple>
#include <vector>

using namespace llvm;

namespace xlt
{
namespace
{

struct Flags {
    Value *n = nullptr;
    Value *z = nullptr;
    Value *c = nullptr;
    Value *v = nullptr;
    bool valid = false;
};

struct Region {
    LLVMContext &C;
    Module *M;
    IRBuilder<> B;
    Function *F;
    uint8_t *ram = nullptr;
    uint64_t ram_base_addr = kRamBase;
    uint64_t ram_size_addr = kRamSize;
    Value *cpu_arg = nullptr;
    Value *ram_base = nullptr;

    AllocaInst *a_x[31] = {};
    AllocaInst *a_sp = nullptr;
    AllocaInst *a_nz = nullptr;
    BasicBlock *exit_bb = nullptr;
    std::vector<std::pair<Value *, BasicBlock *>> exits;

    std::map<uint64_t, BasicBlock *> bbs;
    std::set<uint64_t> done;
    std::deque<uint64_t> wl;

    Value *rv[31] = {};
    bool rvalid[31] = {};
    Value *spv = nullptr;
    bool spvalid = false;
    Flags fl;

    std::set<int> written;
    bool sp_written = false;
    bool any_flag_write = false;

    uint64_t *instruction_count = nullptr;

    Region(LLVMContext &ctx, Module *module, Function *fn) : C(ctx), M(module), B(ctx), F(fn) {}
};

Type *i64T(Region &r)
{
    return r.B.getInt64Ty();
}
Type *i32T(Region &r)
{
    return r.B.getInt32Ty();
}
Type *i8T(Region &r)
{
    return r.B.getInt8Ty();
}
Type *intT(Region &r, unsigned bits)
{
    return IntegerType::get(r.C, bits);
}
Type *ptrT(Region &r)
{
    return PointerType::getUnqual(r.C);
}
Value *c64(Region &r, uint64_t v)
{
    return r.B.getInt64(v);
}
Value *c32(Region &r, uint32_t v)
{
    return r.B.getInt32(v);
}

Value *cpuField(Region &r, unsigned off)
{
    return r.B.CreateGEP(i8T(r), r.cpu_arg, c64(r, off));
}

Value *loadX(Region &r, int i)
{
    return r.B.CreateAlignedLoad(i64T(r), r.a_x[i], Align(8));
}
void storeX(Region &r, int i, Value *v)
{
    r.B.CreateAlignedStore(v, r.a_x[i], Align(8));
}
Value *loadSP(Region &r)
{
    return r.B.CreateAlignedLoad(i64T(r), r.a_sp, Align(8));
}
void storeSP(Region &r, Value *v)
{
    r.B.CreateAlignedStore(v, r.a_sp, Align(8));
}
Value *loadNZ(Region &r)
{
    return r.B.CreateAlignedLoad(i32T(r), r.a_nz, Align(4));
}
void storeNZ(Region &r, Value *v)
{
    r.B.CreateAlignedStore(v, r.a_nz, Align(4));
}

Value *getReg(Region &r, int i)
{
    if (!r.rvalid[i]) {
        r.rv[i] = loadX(r, i);
        r.rvalid[i] = true;
    }
    return r.rv[i];
}
void setReg(Region &r, int i, Value *v)
{
    r.rv[i] = v;
    r.rvalid[i] = true;
    r.written.insert(i);
    storeX(r, i, v);
}
Value *getSPV(Region &r)
{
    if (!r.spvalid) {
        r.spv = loadSP(r);
        r.spvalid = true;
    }
    return r.spv;
}
void setSPV(Region &r, Value *v)
{
    r.spv = v;
    r.spvalid = true;
    r.sp_written = true;
    storeSP(r, v);
}

Value *trunc32(Region &r, Value *v)
{
    return r.B.CreateTrunc(v, i32T(r));
}
Value *zext32(Region &r, Value *v)
{
    return r.B.CreateZExt(v, i64T(r));
}

int gpr_index(Gpr g)
{
    int i = static_cast<int>(g);
    return i >= 0 && i <= 30 ? i : -1;
}

Value *readRegRef(Region &r, RegRef reg)
{
    if (!reg.valid())
        return c64(r, 0);
    if (reg.is_sp()) {
        Value *v = getSPV(r);
        return reg.width == 32 ? zext32(r, trunc32(r, v)) : v;
    }
    if (reg.is_zero())
        return c64(r, 0);
    int i = gpr_index(reg.gpr);
    if (i < 0)
        return c64(r, 0);
    Value *v = getReg(r, i);
    return reg.width == 32 ? zext32(r, trunc32(r, v)) : v;
}

void writeRegRef(Region &r, RegRef reg, Value *v)
{
    if (!reg.valid() || reg.is_zero())
        return;
    if (reg.width == 32)
        v = zext32(r, trunc32(r, v));
    if (reg.is_sp()) {
        setSPV(r, v);
        return;
    }
    int i = gpr_index(reg.gpr);
    if (i >= 0)
        setReg(r, i, v);
}

// flags ---------------------------------------------------------------------
void ensureFlags(Region &r)
{
    if (r.fl.valid)
        return;
    Value *p = loadNZ(r);
    r.fl.n = r.B.CreateTrunc(r.B.CreateLShr(p, c32(r, 31)), r.B.getInt1Ty());
    r.fl.z = r.B.CreateTrunc(r.B.CreateLShr(p, c32(r, 30)), r.B.getInt1Ty());
    r.fl.c = r.B.CreateTrunc(r.B.CreateLShr(p, c32(r, 29)), r.B.getInt1Ty());
    r.fl.v = r.B.CreateTrunc(r.B.CreateLShr(p, c32(r, 28)), r.B.getInt1Ty());
    r.fl.valid = true;
}

void setFlags(Region &r, Value *n, Value *z, Value *c, Value *v)
{
    r.fl.n = n;
    r.fl.z = z;
    r.fl.c = c;
    r.fl.v = v;
    r.fl.valid = true;
    r.any_flag_write = true;
    Value *p = r.B.CreateOr(r.B.CreateOr(r.B.CreateShl(r.B.CreateZExt(n, i32T(r)), c32(r, 31)),
                                         r.B.CreateShl(r.B.CreateZExt(z, i32T(r)), c32(r, 30))),
                            r.B.CreateOr(r.B.CreateShl(r.B.CreateZExt(c, i32T(r)), c32(r, 29)),
                                         r.B.CreateShl(r.B.CreateZExt(v, i32T(r)), c32(r, 28))));
    storeNZ(r, p);
}

Value *evalCond(Region &r, Cond cond)
{
    ensureFlags(r);
    Value *n = r.fl.n;
    Value *z = r.fl.z;
    Value *c = r.fl.c;
    Value *v = r.fl.v;
    Value *one = r.B.getTrue();
    switch (cond) {
    case Cond::kEq:
        return z;
    case Cond::kNe:
        return r.B.CreateXor(z, one);
    case Cond::kHs:
        return c;
    case Cond::kLo:
        return r.B.CreateXor(c, one);
    case Cond::kMi:
        return n;
    case Cond::kPl:
        return r.B.CreateXor(n, one);
    case Cond::kVs:
        return v;
    case Cond::kVc:
        return r.B.CreateXor(v, one);
    case Cond::kHi:
        return r.B.CreateAnd(r.B.CreateXor(z, one), c);
    case Cond::kLs:
        return r.B.CreateOr(r.B.CreateXor(c, one), z);
    case Cond::kGe:
        return r.B.CreateXor(n, v);
    case Cond::kLt:
        return r.B.CreateXor(r.B.CreateXor(n, v), one);
    case Cond::kGt:
        return r.B.CreateAnd(r.B.CreateXor(z, one), r.B.CreateXor(n, v));
    case Cond::kLe:
        return r.B.CreateOr(z, r.B.CreateXor(n, v));
    default:
        return one;
    }
}

std::tuple<Value *, Value *, Value *, Value *> addSubFlags(Region &r, Value *a, Value *b, bool w,
                                                           bool sub)
{
    unsigned bits = w ? 32 : 64;
    Type *ty = intT(r, bits);
    Type *wy = intT(r, 128);
    Value *at = w ? trunc32(r, a) : a;
    Value *bt = w ? trunc32(r, b) : b;
    Value *aw = r.B.CreateSExt(at, wy);
    Value *bw = r.B.CreateSExt(bt, wy);
    Value *s = sub ? r.B.CreateSub(aw, bw) : r.B.CreateAdd(aw, bw);
    Value *res = r.B.CreateTrunc(s, ty);
    Value *n =
        r.B.CreateTrunc(r.B.CreateLShr(res, ConstantInt::get(ty, bits - 1)), r.B.getInt1Ty());
    Value *z = r.B.CreateICmpEQ(res, ConstantInt::get(ty, 0));
    Value *ua = r.B.CreateZExt(at, wy);
    Value *ub = r.B.CreateZExt(bt, wy);
    Value *carry;
    if (sub) {
        carry = r.B.CreateICmpUGE(ua, ub);
    } else {
        Value *u = r.B.CreateAdd(ua, ub);
        carry = r.B.CreateTrunc(r.B.CreateLShr(u, ConstantInt::get(wy, bits)), r.B.getInt1Ty());
    }
    int64_t maxv = bits == 64 ? INT64_MAX : (int64_t)((1ull << (bits - 1)) - 1);
    int64_t minv = bits == 64 ? INT64_MIN : -(int64_t)(1ull << (bits - 1));
    Value *vhi = r.B.CreateICmpSGT(s, ConstantInt::getSigned(wy, maxv));
    Value *vlo = r.B.CreateICmpSLT(s, ConstantInt::getSigned(wy, minv));
    return {n, z, carry, r.B.CreateOr(vhi, vlo)};
}

Value *carryIn(Region &r, Type *ty)
{
    ensureFlags(r);
    return r.B.CreateZExt(r.fl.c, ty);
}

std::tuple<Value *, Value *, Value *, Value *, Value *> adcFlags(Region &r, Value *a, Value *b,
                                                                 bool w)
{
    unsigned bits = w ? 32 : 64;
    Type *ty = intT(r, bits);
    Type *wy = intT(r, 128);
    Value *at = w ? trunc32(r, a) : a;
    Value *bt = w ? trunc32(r, b) : b;
    Value *ci = carryIn(r, ty);
    Value *res = r.B.CreateTrunc(r.B.CreateAdd(r.B.CreateAdd(at, bt), ci), ty);
    Value *s = r.B.CreateAdd(r.B.CreateAdd(r.B.CreateSExt(at, wy), r.B.CreateSExt(bt, wy)),
                             r.B.CreateZExt(ci, wy));
    Value *u = r.B.CreateAdd(r.B.CreateAdd(r.B.CreateZExt(at, wy), r.B.CreateZExt(bt, wy)),
                             r.B.CreateZExt(ci, wy));
    Value *carry = r.B.CreateTrunc(r.B.CreateLShr(u, ConstantInt::get(wy, bits)), r.B.getInt1Ty());
    Value *n =
        r.B.CreateTrunc(r.B.CreateLShr(res, ConstantInt::get(ty, bits - 1)), r.B.getInt1Ty());
    Value *z = r.B.CreateICmpEQ(res, ConstantInt::get(ty, 0));
    int64_t maxv = bits == 64 ? INT64_MAX : (int64_t)((1ull << (bits - 1)) - 1);
    int64_t minv = bits == 64 ? INT64_MIN : -(int64_t)(1ull << (bits - 1));
    Value *vhi = r.B.CreateICmpSGT(s, ConstantInt::getSigned(wy, maxv));
    Value *vlo = r.B.CreateICmpSLT(s, ConstantInt::getSigned(wy, minv));
    return {res, n, z, carry, r.B.CreateOr(vhi, vlo)};
}

std::tuple<Value *, Value *, Value *, Value *, Value *> sbcFlags(Region &r, Value *a, Value *b,
                                                                 bool w)
{
    unsigned bits = w ? 32 : 64;
    Type *ty = intT(r, bits);
    Type *wy = intT(r, 128);
    Value *at = w ? trunc32(r, a) : a;
    Value *bt = w ? trunc32(r, b) : b;
    Value *ci = carryIn(r, ty);
    Value *nb = r.B.CreateXor(bt, ConstantInt::get(ty, w ? 0xffffffffull : ~0ull, true));
    Value *res = r.B.CreateTrunc(r.B.CreateAdd(r.B.CreateAdd(at, nb), ci), ty);
    Value *ss = r.B.CreateSub(r.B.CreateSub(r.B.CreateSExt(at, wy), r.B.CreateSExt(bt, wy)),
                              ConstantInt::get(wy, 1));
    ss = r.B.CreateAdd(ss, r.B.CreateZExt(ci, wy));
    Value *u = r.B.CreateAdd(r.B.CreateAdd(r.B.CreateZExt(at, wy), r.B.CreateZExt(nb, wy)),
                             r.B.CreateZExt(ci, wy));
    Value *carry = r.B.CreateTrunc(r.B.CreateLShr(u, ConstantInt::get(wy, bits)), r.B.getInt1Ty());
    Value *n =
        r.B.CreateTrunc(r.B.CreateLShr(res, ConstantInt::get(ty, bits - 1)), r.B.getInt1Ty());
    Value *z = r.B.CreateICmpEQ(res, ConstantInt::get(ty, 0));
    int64_t maxv = bits == 64 ? INT64_MAX : (int64_t)((1ull << (bits - 1)) - 1);
    int64_t minv = bits == 64 ? INT64_MIN : -(int64_t)(1ull << (bits - 1));
    Value *vhi = r.B.CreateICmpSGT(ss, ConstantInt::getSigned(wy, maxv));
    Value *vlo = r.B.CreateICmpSLT(ss, ConstantInt::getSigned(wy, minv));
    return {res, n, z, carry, r.B.CreateOr(vhi, vlo)};
}

// operand helpers -----------------------------------------------------------
Value *applyExt(Region &r, Value *v, ExtendType e)
{
    switch (e) {
    case ExtendType::kUxtb:
        return r.B.CreateZExt(r.B.CreateTrunc(v, intT(r, 8)), i64T(r));
    case ExtendType::kUxth:
        return r.B.CreateZExt(r.B.CreateTrunc(v, intT(r, 16)), i64T(r));
    case ExtendType::kUxtw:
        return zext32(r, trunc32(r, v));
    case ExtendType::kSxtb:
        return r.B.CreateSExt(r.B.CreateTrunc(v, intT(r, 8)), i64T(r));
    case ExtendType::kSxth:
        return r.B.CreateSExt(r.B.CreateTrunc(v, intT(r, 16)), i64T(r));
    case ExtendType::kSxtw:
        return r.B.CreateSExt(trunc32(r, v), i64T(r));
    default:
        return v;
    }
}

Value *shiftValue(Region &r, Value *v, ShiftType type, unsigned amt, bool w)
{
    if (amt == 0)
        return v;
    if (w) {
        Value *t = trunc32(r, v);
        Value *a = c32(r, amt);
        Value *res;
        switch (type) {
        case ShiftType::kLsl:
            res = r.B.CreateShl(t, a);
            break;
        case ShiftType::kLsr:
            res = r.B.CreateLShr(t, a);
            break;
        case ShiftType::kAsr:
            res = r.B.CreateAShr(t, a);
            break;
        case ShiftType::kRor: {
            Function *fn = Intrinsic::getDeclaration(r.M, Intrinsic::fshr, {i32T(r)});
            res = r.B.CreateCall(fn, {t, t, a});
            break;
        }
        default:
            res = t;
            break;
        }
        return zext32(r, res);
    }
    Value *a = c64(r, amt);
    switch (type) {
    case ShiftType::kLsl:
        return r.B.CreateShl(v, a);
    case ShiftType::kLsr:
        return r.B.CreateLShr(v, a);
    case ShiftType::kAsr:
        return r.B.CreateAShr(v, a);
    case ShiftType::kRor: {
        Function *fn = Intrinsic::getDeclaration(r.M, Intrinsic::fshr, {i64T(r)});
        return r.B.CreateCall(fn, {v, v, a});
    }
    default:
        return v;
    }
}

uint64_t immEff(const Operand &o)
{
    uint64_t v = o.imm;
    unsigned sh = o.shift_amount;
    if (sh == 0)
        return v;
    switch (o.shift) {
    case ShiftType::kLsl:
        return v << sh;
    case ShiftType::kLsr:
        return v >> sh;
    case ShiftType::kAsr:
        return (uint64_t)((int64_t)v >> sh);
    default:
        return v;
    }
}

Value *opValue(Region &r, const Operand &o, bool w)
{
    if (o.kind == OperandKind::kImm) {
        uint64_t v = immEff(o);
        if (w)
            v = (uint32_t)v;
        return c64(r, v);
    }
    if (o.kind == OperandKind::kReg) {
        Value *v = readRegRef(r, o.reg);
        if (o.extend != ExtendType::kNone) {
            v = applyExt(r, v, o.extend);
            if (o.extend_amount)
                v = shiftValue(r, v, ShiftType::kLsl, o.extend_amount, w);
        } else if (o.shift != ShiftType::kNone) {
            v = shiftValue(r, v, o.shift, o.shift_amount, w);
        }
        return v;
    }
    return c64(r, 0);
}

// memory ---------------------------------------------------------------------
Function *declFn(Region &r, const char *name, Type *ret, std::vector<Type *> params,
                 bool noreturn_fn = false)
{
    if (Function *f = r.M->getFunction(name))
        return f;
    FunctionType *ft = FunctionType::get(ret, params, false);
    Function *f = Function::Create(ft, Function::ExternalLinkage, name, r.M);
    if (noreturn_fn)
        f->addFnAttr(Attribute::NoReturn);
    return f;
}

Value *emitLoad(Region &r, Value *addr, unsigned size)
{
    Value *off = r.B.CreateSub(addr, c64(r, r.ram_base_addr));
    Value *ok = r.B.CreateICmpULE(off, c64(r, r.ram_size_addr - size));
    BasicBlock *okbb = BasicBlock::Create(r.C, "m.ok", r.F);
    BasicBlock *mmiobb = BasicBlock::Create(r.C, "m.mmio", r.F);
    BasicBlock *mergebb = BasicBlock::Create(r.C, "m.merge", r.F);
    r.B.CreateCondBr(ok, okbb, mmiobb);
    r.B.SetInsertPoint(okbb);
    Value *p = r.B.CreateGEP(i8T(r), r.ram_base, off);
    Value *direct = r.B.CreateAlignedLoad(intT(r, size * 8), p, Align(1));
    r.B.CreateBr(mergebb);
    r.B.SetInsertPoint(mmiobb);
    Function *host = declFn(r, "xlt_host_read", i64T(r), {i64T(r), i64T(r)});
    Value *mmio = r.B.CreateCall(host, {addr, c64(r, size)});
    mmio = r.B.CreateTrunc(mmio, intT(r, size * 8));
    r.B.CreateBr(mergebb);
    r.B.SetInsertPoint(mergebb);
    PHINode *phi = r.B.CreatePHI(intT(r, size * 8), 2);
    phi->addIncoming(direct, okbb);
    phi->addIncoming(mmio, mmiobb);
    return phi;
}

void emitStore(Region &r, Value *addr, Value *val, unsigned size)
{
    Value *off = r.B.CreateSub(addr, c64(r, r.ram_base_addr));
    Value *ok = r.B.CreateICmpULE(off, c64(r, r.ram_size_addr - size));
    BasicBlock *okbb = BasicBlock::Create(r.C, "m.ok", r.F);
    BasicBlock *mmiobb = BasicBlock::Create(r.C, "m.mmio", r.F);
    BasicBlock *donebb = BasicBlock::Create(r.C, "m.done", r.F);
    r.B.CreateCondBr(ok, okbb, mmiobb);
    r.B.SetInsertPoint(okbb);
    Value *p = r.B.CreateGEP(i8T(r), r.ram_base, off);
    Value *v = size == 8 ? val : r.B.CreateTrunc(val, intT(r, size * 8));
    r.B.CreateAlignedStore(v, p, Align(1));
    r.B.CreateBr(donebb);
    r.B.SetInsertPoint(mmiobb);
    Function *host = declFn(r, "xlt_host_write", r.B.getVoidTy(), {i64T(r), i64T(r), i64T(r)});
    r.B.CreateCall(host, {addr, val, c64(r, size)});
    r.B.CreateBr(donebb);
    r.B.SetInsertPoint(donebb);
}

// blocks / exits -------------------------------------------------------------
BasicBlock *ensureBB(Region &r, uint64_t pc)
{
    auto it = r.bbs.find(pc);
    if (it != r.bbs.end())
        return it->second;
    BasicBlock *bb = BasicBlock::Create(r.C, "g" + std::to_string(pc), r.F);
    r.bbs[pc] = bb;
    r.wl.push_back(pc);
    return bb;
}

void emitExit(Region &r, Value *next_pc)
{
    r.exits.push_back({next_pc, r.B.GetInsertBlock()});
    r.B.CreateBr(r.exit_bb);
}

bool lift(Region &r, const Insn &insn, uint64_t pc);

bool lift(Region &r, const Insn &insn, uint64_t pc)
{
    IRBuilder<> &b = r.B;
    unsigned n = insn.operand_count;
    auto is_reg = [&](unsigned i) {
        return i < n && insn.operands[i].kind == OperandKind::kReg && insn.operands[i].reg.valid();
    };
    auto is_imm = [&](unsigned i) { return i < n && insn.operands[i].kind == OperandKind::kImm; };
    auto is_mem = [&](unsigned i) { return i < n && insn.operands[i].kind == OperandKind::kMem; };
    auto op = [&](unsigned i) -> const Operand & { return insn.operands[i]; };

    auto read = [&](unsigned i) { return readRegRef(r, op(i).reg); };
    auto write = [&](unsigned i, Value *v) { writeRegRef(r, op(i).reg, v); };

    switch (insn.opcode) {
    // --- no-ops -------------------------------------------------------------
    case Opcode::kNop:
    case Opcode::kWfe:
    case Opcode::kWfi:
    case Opcode::kSev:
    case Opcode::kSevl:
    case Opcode::kYield:
    case Opcode::kIsb:
    case Opcode::kDsb:
    case Opcode::kDmb:
        return false;

    // --- branches -----------------------------------------------------------
    case Opcode::kB:
        b.CreateBr(ensureBB(r, insn.target));
        return true;
    case Opcode::kBCond: {
        Value *cond = evalCond(r, insn.cond);
        BasicBlock *tt = ensureBB(r, insn.target);
        BasicBlock *ft = ensureBB(r, pc + 4);
        b.CreateCondBr(cond, tt, ft);
        return true;
    }
    case Opcode::kBl:
        setReg(r, 30, c64(r, pc + 4));
        emitExit(r, c64(r, insn.target));
        return true;
    case Opcode::kBr:
        emitExit(r, is_reg(0) ? read(0) : getReg(r, 30));
        return true;
    case Opcode::kBlr: {
        Value *target = is_reg(0) ? read(0) : getReg(r, 30);
        setReg(r, 30, c64(r, pc + 4));
        emitExit(r, target);
        return true;
    }
    case Opcode::kRet:
        emitExit(r, is_reg(0) ? read(0) : getReg(r, 30));
        return true;
    case Opcode::kCbz:
    case Opcode::kCbnz: {
        bool w = op(0).reg.width == 32;
        Value *v = read(0);
        if (w)
            v = trunc32(r, v);
        Value *cond = b.CreateICmpEQ(v, w ? (Value *)c32(r, 0) : (Value *)c64(r, 0));
        if (insn.opcode == Opcode::kCbnz)
            cond = b.CreateXor(cond, b.getTrue());
        b.CreateCondBr(cond, ensureBB(r, insn.target), ensureBB(r, pc + 4));
        return true;
    }
    case Opcode::kTbz:
    case Opcode::kTbnz: {
        bool w = op(0).reg.width == 32;
        unsigned bit = (unsigned)op(1).imm;
        Value *v = read(0);
        if (w)
            v = trunc32(r, v);
        Value *bitv = b.CreateTrunc(
            b.CreateLShr(v, w ? (Value *)c32(r, bit) : (Value *)c64(r, bit)), b.getInt1Ty());
        Value *cond = insn.opcode == Opcode::kTbz ? b.CreateXor(bitv, b.getTrue()) : bitv;
        b.CreateCondBr(cond, ensureBB(r, insn.target), ensureBB(r, pc + 4));
        return true;
    }
    case Opcode::kHlt: {
        for (int i = 0; i < 31; i++)
            b.CreateAlignedStore(getReg(r, i), cpuField(r, (unsigned)offsetof(CpuRegs, x) + i * 8),
                                 Align(8));
        b.CreateAlignedStore(getSPV(r), cpuField(r, (unsigned)offsetof(CpuRegs, sp)), Align(8));
        Function *sh = declFn(r, "xlt_semihost", b.getVoidTy(), {ptrT(r)});
        b.CreateCall(sh, {r.cpu_arg});
        Value *v0 =
            b.CreateAlignedLoad(i64T(r), cpuField(r, (unsigned)offsetof(CpuRegs, x)), Align(8));
        storeX(r, 0, v0);
        r.rv[0] = v0;
        r.rvalid[0] = true;
        r.written.insert(0);
        Value *halted = b.CreateAlignedLoad(
            i32T(r), cpuField(r, (unsigned)offsetof(CpuRegs, halted)), Align(4));
        BasicBlock *cont = ensureBB(r, pc + 4);
        BasicBlock *halt = BasicBlock::Create(r.C, "halt", r.F);
        b.CreateCondBr(b.CreateICmpNE(halted, c32(r, 0)), halt, cont);
        b.SetInsertPoint(halt);
        emitExit(r, c64(r, 0));
        return true;
    }

    // --- system registers ---------------------------------------------------
    case Opcode::kMrs: {
        uint64_t sr = op(1).sysreg;
        Value *v;
        if (sr == 0xDA10) {
            ensureFlags(r);
            Value *p =
                b.CreateOr(b.CreateOr(b.CreateShl(b.CreateZExt(r.fl.n, i32T(r)), c32(r, 31)),
                                      b.CreateShl(b.CreateZExt(r.fl.z, i32T(r)), c32(r, 30))),
                           b.CreateOr(b.CreateShl(b.CreateZExt(r.fl.c, i32T(r)), c32(r, 29)),
                                      b.CreateShl(b.CreateZExt(r.fl.v, i32T(r)), c32(r, 28))));
            v = b.CreateZExt(p, i64T(r));
        } else {
            Function *f = declFn(r, "xlt_sysreg_read", i64T(r), {i32T(r)});
            v = b.CreateCall(f, {c32(r, (uint32_t)sr)});
        }
        write(0, v);
        return false;
    }
    case Opcode::kMsr: {
        uint64_t sr = op(0).sysreg;
        Value *v = is_reg(1) ? read(1) : c64(r, 0);
        Function *f = declFn(r, "xlt_sysreg_write", b.getVoidTy(), {i32T(r), i64T(r)});
        b.CreateCall(f, {c32(r, (uint32_t)sr), v});
        return false;
    }
    case Opcode::kHvc: {
        for (int i = 0; i < 31; i++) {
            b.CreateAlignedStore(getReg(r, i), cpuField(r, (unsigned)offsetof(CpuRegs, x) + i * 8),
                                 Align(8));
        }
        b.CreateAlignedStore(getSPV(r), cpuField(r, (unsigned)offsetof(CpuRegs, sp)), Align(8));
        Function *f = declFn(r, "xlt_hvc", b.getVoidTy(), {ptrT(r), i64T(r)});
        b.CreateCall(f, {r.cpu_arg, c64(r, is_imm(0) ? op(0).imm : 0)});
        Value *v0 =
            b.CreateAlignedLoad(i64T(r), cpuField(r, (unsigned)offsetof(CpuRegs, x)), Align(8));
        storeX(r, 0, v0);
        r.rv[0] = v0;
        r.rvalid[0] = true;
        r.written.insert(0);
        emitExit(r, c64(r, pc + 4));
        return true;
    }

    // --- moves --------------------------------------------------------------
    case Opcode::kMov:
        write(0, opValue(r, op(1), op(0).reg.width == 32));
        return false;
    case Opcode::kMovz:
    case Opcode::kMovn:
    case Opcode::kMovk: {
        bool w = op(0).reg.width == 32;
        uint64_t imm = op(1).imm << op(1).shift_amount;
        uint64_t mask = w ? 0xffffffffull : ~0ull;
        Value *v;
        if (insn.opcode == Opcode::kMovz) {
            v = c64(r, imm & mask);
        } else if (insn.opcode == Opcode::kMovn) {
            v = c64(r, (~imm) & mask);
        } else {
            Value *old = read(0);
            Value *keep = c64(r, ~(0xffffull << op(1).shift_amount) & mask);
            v = b.CreateOr(b.CreateAnd(old, keep), c64(r, imm & mask));
        }
        write(0, v);
        return false;
    }
    case Opcode::kAdr:
    case Opcode::kAdrp:
        write(0, c64(r, op(1).imm));
        return false;

    // --- add/sub ------------------------------------------------------------
    case Opcode::kCmp:
    case Opcode::kCmn: {
        bool w = op(0).reg.width == 32;
        Value *a = read(0);
        Value *bv = opValue(r, op(1), w);
        auto fl = addSubFlags(r, a, bv, w, insn.opcode == Opcode::kCmp);
        setFlags(r, std::get<0>(fl), std::get<1>(fl), std::get<2>(fl), std::get<3>(fl));
        return false;
    }
    case Opcode::kAdd:
    case Opcode::kSub: {
        bool w = op(0).reg.width == 32;
        Value *a = read(1);
        Value *bv = opValue(r, op(2), w);
        if (w) {
            a = trunc32(r, a);
            bv = trunc32(r, bv);
        }
        bool sub = insn.opcode == Opcode::kSub;
        Value *res = sub ? b.CreateSub(a, bv) : b.CreateAdd(a, bv);
        if (insn.sets_flags) {
            auto fl = addSubFlags(r, a, bv, w, sub);
            setFlags(r, std::get<0>(fl), std::get<1>(fl), std::get<2>(fl), std::get<3>(fl));
        }
        write(0, res);
        return false;
    }
    case Opcode::kNeg: {
        bool w = op(0).reg.width == 32;
        Value *a = read(1);
        if (w)
            a = trunc32(r, a);
        if (insn.sets_flags) {
            auto fl = addSubFlags(r, w ? (Value *)c32(r, 0) : (Value *)c64(r, 0), a, w, true);
            setFlags(r, std::get<0>(fl), std::get<1>(fl), std::get<2>(fl), std::get<3>(fl));
        }
        write(0, b.CreateNeg(a));
        return false;
    }

    // --- logical ------------------------------------------------------------
    case Opcode::kAnd:
    case Opcode::kOrr:
    case Opcode::kEor:
    case Opcode::kOrn:
    case Opcode::kBic: {
        bool w = op(0).reg.width == 32;
        Value *a = read(1);
        Value *bv = opValue(r, op(2), w);
        if (w) {
            a = trunc32(r, a);
            bv = trunc32(r, bv);
        }
        Value *res;
        switch (insn.opcode) {
        case Opcode::kAnd:
            res = b.CreateAnd(a, bv);
            break;
        case Opcode::kOrr:
            res = b.CreateOr(a, bv);
            break;
        case Opcode::kEor:
            res = b.CreateXor(a, bv);
            break;
        case Opcode::kOrn:
            res = b.CreateOr(a, b.CreateNot(bv));
            break;
        default:
            res = b.CreateAnd(a, b.CreateNot(bv));
            break;
        }
        if (insn.sets_flags) {
            Value *nf = b.CreateTrunc(
                b.CreateLShr(res, w ? (Value *)c32(r, 31) : (Value *)c64(r, 63)), b.getInt1Ty());
            Value *zf = b.CreateICmpEQ(res, w ? (Value *)c32(r, 0) : (Value *)c64(r, 0));
            setFlags(r, nf, zf, b.getFalse(), b.getFalse());
        }
        write(0, res);
        return false;
    }
    case Opcode::kMvn: {
        bool w = op(0).reg.width == 32;
        Value *a = read(1);
        if (w)
            a = trunc32(r, a);
        write(0, b.CreateNot(a));
        return false;
    }
    case Opcode::kTst: {
        bool w = op(0).reg.width == 32;
        Value *a = read(0);
        Value *bv = opValue(r, op(1), w);
        if (w) {
            a = trunc32(r, a);
            bv = trunc32(r, bv);
        }
        Value *res = b.CreateAnd(a, bv);
        Value *nf = b.CreateTrunc(b.CreateLShr(res, w ? (Value *)c32(r, 31) : (Value *)c64(r, 63)),
                                  b.getInt1Ty());
        Value *zf = b.CreateICmpEQ(res, w ? (Value *)c32(r, 0) : (Value *)c64(r, 0));
        setFlags(r, nf, zf, b.getFalse(), b.getFalse());
        return false;
    }

    // --- shifts -------------------------------------------------------------
    case Opcode::kLsl:
    case Opcode::kLsr:
    case Opcode::kAsr:
    case Opcode::kRor: {
        bool w = op(0).reg.width == 32;
        Value *a = read(1);
        Value *amt;
        if (is_imm(2)) {
            amt = w ? (Value *)c32(r, (uint32_t)op(2).imm) : (Value *)c64(r, op(2).imm);
        } else {
            Value *rv = read(2);
            if (w)
                rv = trunc32(r, rv);
            amt = b.CreateAnd(rv, w ? (Value *)c32(r, 31) : (Value *)c64(r, 63));
        }
        if (w) {
            a = trunc32(r, a);
            Value *amtw = amt->getType()->isIntegerTy(32) ? amt : b.CreateTrunc(amt, i32T(r));
            Value *res;
            switch (insn.opcode) {
            case Opcode::kLsl:
                res = b.CreateShl(a, amtw);
                break;
            case Opcode::kLsr:
                res = b.CreateLShr(a, amtw);
                break;
            case Opcode::kAsr:
                res = b.CreateAShr(a, amtw);
                break;
            default: {
                Function *fn = Intrinsic::getDeclaration(r.M, Intrinsic::fshr, {i32T(r)});
                res = b.CreateCall(fn, {a, a, amtw});
                break;
            }
            }
            write(0, res);
        } else {
            Value *amt64 = amt->getType()->isIntegerTy(64) ? amt : b.CreateZExt(amt, i64T(r));
            Value *res;
            switch (insn.opcode) {
            case Opcode::kLsl:
                res = b.CreateShl(a, amt64);
                break;
            case Opcode::kLsr:
                res = b.CreateLShr(a, amt64);
                break;
            case Opcode::kAsr:
                res = b.CreateAShr(a, amt64);
                break;
            default: {
                Function *fn = Intrinsic::getDeclaration(r.M, Intrinsic::fshr, {i64T(r)});
                res = b.CreateCall(fn, {a, a, amt64});
                break;
            }
            }
            write(0, res);
        }
        return false;
    }

    // --- bitfields ----------------------------------------------------------
    case Opcode::kUbfx:
    case Opcode::kSbfx:
    case Opcode::kBfxil:
    case Opcode::kUbfm:
    case Opcode::kSbfm:
    case Opcode::kBfm: {
        bool w = op(0).reg.width == 32;
        unsigned bits = w ? 32 : 64;
        Value *a = read(1);
        if (w)
            a = trunc32(r, a);
        unsigned lsb = (unsigned)op(2).imm;
        unsigned width = (unsigned)op(3).imm;
        uint64_t vmask = width >= bits ? ~0ull : ((1ull << width) - 1);
        if (w)
            vmask &= 0xffffffffull;
        Value *vm = w ? (Value *)c32(r, (uint32_t)vmask) : (Value *)c64(r, vmask);
        Value *res;
        bool sign = insn.opcode == Opcode::kSbfx || insn.opcode == Opcode::kSbfm;
        bool insert = insn.opcode == Opcode::kBfxil || insn.opcode == Opcode::kBfm;
        if (!insert) {
            res = b.CreateAnd(b.CreateLShr(a, w ? (Value *)c32(r, lsb) : (Value *)c64(r, lsb)), vm);
            if (sign) {
                unsigned top = bits - (lsb + width);
                res =
                    b.CreateAShr(b.CreateShl(res, w ? (Value *)c32(r, top) : (Value *)c64(r, top)),
                                 w ? (Value *)c32(r, top) : (Value *)c64(r, top));
            }
        } else {
            Value *old = read(0);
            if (w)
                old = trunc32(r, old);
            Value *t =
                b.CreateAnd(b.CreateLShr(a, w ? (Value *)c32(r, lsb) : (Value *)c64(r, lsb)), vm);
            t = b.CreateShl(t, w ? (Value *)c32(r, lsb) : (Value *)c64(r, lsb));
            uint64_t keep = ~(vmask << lsb);
            if (w)
                keep &= 0xffffffffull;
            res = b.CreateOr(b.CreateAnd(old, c64(r, keep)), b.CreateZExt(t, i64T(r)));
            if (w)
                res = b.CreateTrunc(res, i32T(r));
        }
        write(0, res);
        return false;
    }

    case Opcode::kExtr: {
        bool w = op(0).reg.width == 32;
        unsigned lsb = (unsigned)op(3).imm;
        if (w) {
            Function *fn = Intrinsic::getDeclaration(r.M, Intrinsic::fshr, {i32T(r)});
            Value *res = b.CreateCall(fn, {trunc32(r, read(1)), trunc32(r, read(2)), c32(r, lsb)});
            write(0, res);
        } else {
            Function *fn = Intrinsic::getDeclaration(r.M, Intrinsic::fshr, {i64T(r)});
            write(0, b.CreateCall(fn, {read(1), read(2), c64(r, lsb)}));
        }
        return false;
    }

    // --- multiply/divide ----------------------------------------------------
    case Opcode::kMul:
    case Opcode::kMadd:
    case Opcode::kMsub: {
        bool w = op(0).reg.width == 32;
        Value *a = read(1);
        Value *bv = read(2);
        Value *acc = insn.opcode == Opcode::kMul ? c64(r, 0) : read(3);
        if (w) {
            a = trunc32(r, a);
            bv = trunc32(r, bv);
            acc = trunc32(r, acc);
        }
        Value *prod = b.CreateMul(a, bv);
        Value *res = insn.opcode == Opcode::kMsub ? b.CreateSub(acc, prod) : b.CreateAdd(prod, acc);
        write(0, res);
        return false;
    }
    case Opcode::kSmulh:
    case Opcode::kUmulh: {
        Value *a = read(1);
        Value *bv = read(2);
        Type *wy = intT(r, 128);
        Value *aw = insn.opcode == Opcode::kSmulh ? b.CreateSExt(a, wy) : b.CreateZExt(a, wy);
        Value *bw = insn.opcode == Opcode::kSmulh ? b.CreateSExt(bv, wy) : b.CreateZExt(bv, wy);
        Value *hi =
            b.CreateTrunc(b.CreateLShr(b.CreateMul(aw, bw), ConstantInt::get(wy, 64)), i64T(r));
        write(0, hi);
        return false;
    }
    case Opcode::kUdiv:
    case Opcode::kSdiv: {
        bool w = op(0).reg.width == 32;
        Value *a = read(1);
        Value *bv = read(2);
        if (w) {
            a = trunc32(r, a);
            bv = trunc32(r, bv);
        }
        Value *zero = w ? (Value *)c32(r, 0) : (Value *)c64(r, 0);
        Value *div = insn.opcode == Opcode::kUdiv ? b.CreateUDiv(a, bv) : b.CreateSDiv(a, bv);
        Value *res = b.CreateSelect(b.CreateICmpEQ(bv, zero), zero, div);
        if (insn.opcode == Opcode::kSdiv) {
            Value *minv = w ? (Value *)c32(r, 0x80000000u) : (Value *)c64(r, 0x8000000000000000ull);
            Value *neg1 = w ? (Value *)c32(r, 0xffffffffu) : (Value *)c64(r, 0xffffffffffffffffull);
            Value *ovf = b.CreateAnd(b.CreateICmpEQ(a, minv), b.CreateICmpEQ(bv, neg1));
            res = b.CreateSelect(ovf, minv, res);
        }
        write(0, res);
        return false;
    }

    // --- carry arithmetic ---------------------------------------------------
    case Opcode::kAdc:
    case Opcode::kSbc: {
        bool w = op(0).reg.width == 32;
        Value *a = read(1);
        Value *bv = read(2);
        if (w) {
            a = trunc32(r, a);
            bv = trunc32(r, bv);
        }
        Value *ci = carryIn(r, w ? i32T(r) : i64T(r));
        Value *res;
        if (insn.opcode == Opcode::kAdc) {
            res = b.CreateAdd(b.CreateAdd(a, bv), ci);
            if (insn.sets_flags) {
                auto fl = adcFlags(r, a, bv, w);
                setFlags(r, std::get<1>(fl), std::get<2>(fl), std::get<3>(fl), std::get<4>(fl));
            }
        } else {
            Value *nb = b.CreateXor(bv, w ? (Value *)c32(r, 0xffffffffu)
                                          : (Value *)c64(r, 0xffffffffffffffffull));
            res = b.CreateAdd(b.CreateAdd(a, nb), ci);
            if (insn.sets_flags) {
                auto fl = sbcFlags(r, a, bv, w);
                setFlags(r, std::get<1>(fl), std::get<2>(fl), std::get<3>(fl), std::get<4>(fl));
            }
        }
        write(0, res);
        return false;
    }

    // --- conditional select ------------------------------------------------
    case Opcode::kCsel:
    case Opcode::kCsinc:
    case Opcode::kCsinv:
    case Opcode::kCsneg: {
        bool w = op(0).reg.width == 32;
        Value *a = read(1);
        Value *bv = read(2);
        if (w) {
            a = trunc32(r, a);
            bv = trunc32(r, bv);
        }
        Value *cond = evalCond(r, insn.cond);
        Value *alt;
        switch (insn.opcode) {
        case Opcode::kCsel:
            alt = bv;
            break;
        case Opcode::kCsinc:
            alt = b.CreateAdd(bv, w ? (Value *)c32(r, 1) : (Value *)c64(r, 1));
            break;
        case Opcode::kCsinv:
            alt = b.CreateNot(bv);
            break;
        default:
            alt = b.CreateNeg(bv);
            break;
        }
        write(0, b.CreateSelect(cond, a, alt));
        return false;
    }
    case Opcode::kCset:
    case Opcode::kCsetm:
    case Opcode::kCinc:
    case Opcode::kCinv:
    case Opcode::kCneg: {
        bool w = op(0).reg.width == 32;
        Value *cond = evalCond(r, insn.cond);
        Value *one = w ? (Value *)c32(r, 1) : (Value *)c64(r, 1);
        Value *zero = w ? (Value *)c32(r, 0) : (Value *)c64(r, 0);
        Value *all = w ? (Value *)c32(r, 0xffffffffu) : (Value *)c64(r, 0xffffffffffffffffull);
        Value *res;
        if (insn.opcode == Opcode::kCset) {
            res = b.CreateSelect(cond, one, zero);
        } else if (insn.opcode == Opcode::kCsetm) {
            res = b.CreateSelect(cond, all, zero);
        } else {
            Value *src = read(1);
            if (w)
                src = trunc32(r, src);
            if (insn.opcode == Opcode::kCinc)
                res = b.CreateSelect(cond, src, b.CreateAdd(src, one));
            else if (insn.opcode == Opcode::kCinv)
                res = b.CreateSelect(cond, src, b.CreateNot(src));
            else
                res = b.CreateSelect(cond, src, b.CreateNeg(src));
        }
        write(0, res);
        return false;
    }
    case Opcode::kCcmp:
    case Opcode::kCcmn: {
        bool w = op(0).reg.width == 32;
        Value *a = read(0);
        Value *bv = opValue(r, op(1), w);
        if (w) {
            a = trunc32(r, a);
            bv = trunc32(r, bv);
        }
        auto fl = addSubFlags(r, a, bv, w, insn.opcode == Opcode::kCcmp);
        uint64_t imm = op(2).imm & 0xf;
        Value *n2 = b.getInt1((imm >> 3) & 1);
        Value *z2 = b.getInt1((imm >> 2) & 1);
        Value *c2 = b.getInt1((imm >> 1) & 1);
        Value *v2 = b.getInt1(imm & 1);
        Value *cond = evalCond(r, insn.cond);
        setFlags(
            r, b.CreateSelect(cond, std::get<0>(fl), n2), b.CreateSelect(cond, std::get<1>(fl), z2),
            b.CreateSelect(cond, std::get<2>(fl), c2), b.CreateSelect(cond, std::get<3>(fl), v2));
        return false;
    }

    // --- clz / rbit / rev ---------------------------------------------------
    case Opcode::kClz:
    case Opcode::kRbit:
    case Opcode::kRev: {
        bool w = op(0).reg.width == 32;
        Type *ty = w ? i32T(r) : i64T(r);
        Value *a = read(1);
        if (w)
            a = trunc32(r, a);
        Value *res;
        if (insn.opcode == Opcode::kClz) {
            res = b.CreateCall(Intrinsic::getDeclaration(r.M, Intrinsic::ctlz, {ty}),
                               {a, b.getFalse()});
        } else if (insn.opcode == Opcode::kRbit) {
            res = b.CreateCall(Intrinsic::getDeclaration(r.M, Intrinsic::bitreverse, {ty}), {a});
        } else {
            res = b.CreateCall(Intrinsic::getDeclaration(r.M, Intrinsic::bswap, {ty}), {a});
        }
        write(0, res);
        return false;
    }
    case Opcode::kRev16: {
        bool w = op(0).reg.width == 32;
        Value *a = read(1);
        if (w)
            a = trunc32(r, a);
        uint64_t maskv = w ? 0x00ff00ffull : 0x00ff00ff00ff00ffull;
        Value *mask = w ? (Value *)c32(r, (uint32_t)maskv) : (Value *)c64(r, maskv);
        Value *sh = w ? (Value *)c32(r, 8) : (Value *)c64(r, 8);
        write(0, b.CreateOr(b.CreateShl(b.CreateAnd(a, mask), sh),
                            b.CreateAnd(b.CreateLShr(a, sh), mask)));
        return false;
    }
    case Opcode::kRev32: {
        bool w = op(0).reg.width == 32;
        Value *a = read(1);
        if (w) {
            a = trunc32(r, a);
            write(0,
                  b.CreateCall(Intrinsic::getDeclaration(r.M, Intrinsic::bswap, {i32T(r)}), {a}));
        } else {
            Value *lo = b.CreateAnd(a, c64(r, 0xffffffffull));
            Value *hi = b.CreateAnd(b.CreateLShr(a, c64(r, 32)), c64(r, 0xffffffffull));
            write(0, b.CreateOr(b.CreateShl(lo, c64(r, 32)), hi));
        }
        return false;
    }

    // --- loads/stores -------------------------------------------------------
    case Opcode::kLdr:
    case Opcode::kStr:
    case Opcode::kLdrb:
    case Opcode::kStrb:
    case Opcode::kLdrh:
    case Opcode::kStrh:
    case Opcode::kLdrsb:
    case Opcode::kLdrsh:
    case Opcode::kLdrsw:
    case Opcode::kLdp:
    case Opcode::kStp:
    case Opcode::kLdpsw:
    case Opcode::kLdur:
    case Opcode::kStur:
    case Opcode::kLdurb:
    case Opcode::kSturb:
    case Opcode::kLdurh:
    case Opcode::kSturh:
    case Opcode::kLdursb:
    case Opcode::kLdursh:
    case Opcode::kLdursw: {
        bool load = insn.opcode == Opcode::kLdr || insn.opcode == Opcode::kLdrb ||
                    insn.opcode == Opcode::kLdrh || insn.opcode == Opcode::kLdrsb ||
                    insn.opcode == Opcode::kLdrsh || insn.opcode == Opcode::kLdrsw ||
                    insn.opcode == Opcode::kLdp || insn.opcode == Opcode::kLdpsw ||
                    insn.opcode == Opcode::kLdur || insn.opcode == Opcode::kLdurb ||
                    insn.opcode == Opcode::kLdurh || insn.opcode == Opcode::kLdursb ||
                    insn.opcode == Opcode::kLdursh || insn.opcode == Opcode::kLdursw;
        bool pair = insn.opcode == Opcode::kLdp || insn.opcode == Opcode::kStp ||
                    insn.opcode == Opcode::kLdpsw;
        bool sext = insn.opcode == Opcode::kLdrsb || insn.opcode == Opcode::kLdrsh ||
                    insn.opcode == Opcode::kLdrsw || insn.opcode == Opcode::kLdpsw ||
                    insn.opcode == Opcode::kLdursb || insn.opcode == Opcode::kLdursh ||
                    insn.opcode == Opcode::kLdursw;
        unsigned size = insn.access_width;
        if (size == 0) {
            if (insn.opcode == Opcode::kLdrb || insn.opcode == Opcode::kStrb ||
                insn.opcode == Opcode::kLdrsb || insn.opcode == Opcode::kLdurb ||
                insn.opcode == Opcode::kSturb || insn.opcode == Opcode::kLdursb)
                size = 1;
            else if (insn.opcode == Opcode::kLdrh || insn.opcode == Opcode::kStrh ||
                     insn.opcode == Opcode::kLdrsh || insn.opcode == Opcode::kLdurh ||
                     insn.opcode == Opcode::kSturh || insn.opcode == Opcode::kLdursh)
                size = 2;
            else if (insn.opcode == Opcode::kLdrsw || insn.opcode == Opcode::kLdursw ||
                     insn.opcode == Opcode::kLdpsw)
                size = 4;
            else
                size = is_reg(0) && op(0).reg.width == 32 ? 4 : 8;
        }

        int mi = -1;
        for (unsigned i = 0; i < n; ++i) {
            if (is_mem(i)) {
                mi = (int)i;
                break;
            }
        }

        Value *addr = nullptr;
        Value *base_val = nullptr;
        RegRef wb_reg;
        if (mi < 0) {
            int li = -1;
            for (unsigned i = 1; i < n; ++i) {
                if (is_imm(i)) {
                    li = (int)i;
                    break;
                }
            }
            addr = c64(r, li >= 0 ? op((unsigned)li).imm : 0);
        } else {
            const Operand &mo = insn.operands[mi];
            base_val = readRegRef(r, mo.base);
            addr = base_val;
            if (mo.index.valid()) {
                Value *idx = readRegRef(r, mo.index);
                if (mo.extend != ExtendType::kNone) {
                    idx = applyExt(r, idx, mo.extend);
                    if (mo.extend_amount)
                        idx = shiftValue(r, idx, ShiftType::kLsl, mo.extend_amount, false);
                } else if (mo.shift != ShiftType::kNone) {
                    idx = shiftValue(r, idx, mo.shift, mo.shift_amount, false);
                }
                addr = b.CreateAdd(addr, idx);
            }
            if (mo.disp)
                addr = b.CreateAdd(addr, c64(r, (uint64_t)(int64_t)mo.disp));
            if (insn.writeback && mo.base.valid()) {
                wb_reg = mo.base;
            }
        }

        if (!pair) {
            if (load) {
                Value *v = emitLoad(r, addr, size);
                if (sext)
                    v = b.CreateSExt(v, i64T(r));
                else if (size < 8)
                    v = b.CreateZExt(v, i64T(r));
                write(0, v);
            } else {
                emitStore(r, addr, read(0), size);
            }
        } else {
            Value *addr2 = b.CreateAdd(addr, c64(r, size));
            bool w = is_reg(0) && op(0).reg.width == 32;
            if (load) {
                Value *v0 = emitLoad(r, addr, size);
                Value *v1 = emitLoad(r, addr2, size);
                if (sext) {
                    v0 = b.CreateSExt(v0, i64T(r));
                    v1 = b.CreateSExt(v1, i64T(r));
                } else {
                    v0 = b.CreateZExt(v0, i64T(r));
                    v1 = b.CreateZExt(v1, i64T(r));
                }
                write(0, v0);
                write(1, v1);
            } else {
                emitStore(r, addr, read(0), size);
                emitStore(r, addr2, read(1), size);
            }
            (void)w;
        }

        if (wb_reg.valid() && insn.writeback) {
            Value *nv = b.CreateAdd(base_val, c64(r, (uint64_t)insn.writeback_disp));
            writeRegRef(r, wb_reg, nv);
        }
        return false;
    }

    // --- unsupported / traps ------------------------------------------------
    case Opcode::kUnsupported:
    case Opcode::kBrk:
    case Opcode::kSvc:
    default: {
        Function *f = declFn(r, "xlt_unsupported", b.getVoidTy(), {i64T(r), i64T(r)}, true);
        b.CreateCall(f, {c64(r, pc), c64(r, insn.raw)});
        emitExit(r, c64(r, 0));
        return true;
    }
    }
}

void finalizeRegion(Region &r)
{
    if (r.exits.empty()) {
        r.B.SetInsertPoint(r.exit_bb);
        r.B.CreateRet(c64(r, 0));
        return;
    }
    r.B.SetInsertPoint(r.exit_bb);
    PHINode *phi = r.B.CreatePHI(i64T(r), (unsigned)r.exits.size(), "ret");
    for (auto &e : r.exits)
        phi->addIncoming(e.first, e.second);
    for (int i : r.written) {
        Value *v = r.B.CreateAlignedLoad(i64T(r), r.a_x[i], Align(8));
        r.B.CreateAlignedStore(v, cpuField(r, (unsigned)offsetof(CpuRegs, x) + i * 8), Align(8));
    }
    if (r.sp_written) {
        Value *v = r.B.CreateAlignedLoad(i64T(r), r.a_sp, Align(8));
        r.B.CreateAlignedStore(v, cpuField(r, (unsigned)offsetof(CpuRegs, sp)), Align(8));
    }
    if (r.any_flag_write) {
        Value *v = r.B.CreateAlignedLoad(i32T(r), r.a_nz, Align(4));
        r.B.CreateAlignedStore(v, cpuField(r, (unsigned)offsetof(CpuRegs, nzcv)), Align(4));
    }
    r.B.CreateRet(phi);
}

void buildEntry(Region &r, uint64_t entry_pc)
{
    BasicBlock *entry = BasicBlock::Create(r.C, "entry", r.F);
    r.B.SetInsertPoint(entry);
    r.exit_bb = BasicBlock::Create(r.C, "exit", r.F);
    r.ram_base = r.B.CreateIntToPtr(c64(r, (uint64_t)r.ram), ptrT(r));
    for (int i = 0; i < 31; i++) {
        r.a_x[i] = r.B.CreateAlloca(i64T(r), nullptr, "a" + std::to_string(i));
        Value *v = r.B.CreateAlignedLoad(
            i64T(r), cpuField(r, (unsigned)offsetof(CpuRegs, x) + i * 8), Align(8));
        r.B.CreateAlignedStore(v, r.a_x[i], Align(8));
    }
    r.a_sp = r.B.CreateAlloca(i64T(r), nullptr, "asp");
    r.a_nz = r.B.CreateAlloca(i32T(r), nullptr, "anz");
    r.B.CreateAlignedStore(
        r.B.CreateAlignedLoad(i64T(r), cpuField(r, (unsigned)offsetof(CpuRegs, sp)), Align(8)),
        r.a_sp, Align(8));
    r.B.CreateAlignedStore(
        r.B.CreateAlignedLoad(i32T(r), cpuField(r, (unsigned)offsetof(CpuRegs, nzcv)), Align(4)),
        r.a_nz, Align(4));
    r.B.CreateBr(ensureBB(r, entry_pc));
}

} // namespace

std::unique_ptr<llvm::Module> translate_region(LLVMContext &context, uint64_t entry_pc,
                                               uint8_t *ram, uint64_t ram_base, uint64_t ram_size,
                                               uint64_t *instruction_count, std::string *error)
{
    auto module = std::make_unique<Module>("r" + std::to_string(entry_pc), context);
    FunctionType *ft =
        FunctionType::get(Type::getInt64Ty(context), {PointerType::getUnqual(context)}, false);
    Function *fn = Function::Create(ft, Function::ExternalLinkage, "g" + std::to_string(entry_pc),
                                    module.get());
    fn->getArg(0)->setName("cpu");

    Region r(context, module.get(), fn);
    r.ram = ram;
    r.ram_base_addr = ram_base;
    r.ram_size_addr = ram_size;
    r.cpu_arg = fn->getArg(0);
    uint64_t count = 0;
    r.instruction_count = &count;
    buildEntry(r, entry_pc);

    while (!r.wl.empty()) {
        uint64_t pc = r.wl.front();
        r.wl.pop_front();
        if (r.done.count(pc))
            continue;
        r.done.insert(pc);
        r.B.SetInsertPoint(r.bbs[pc]);
        for (int i = 0; i < 31; i++) {
            r.rvalid[i] = false;
            r.rv[i] = nullptr;
        }
        r.spvalid = false;
        r.fl = Flags();
        uint64_t p = pc;
        bool ended = false;
        while (!ended) {
            if (p != pc && r.bbs.count(p)) {
                r.B.CreateBr(r.bbs[p]);
                break;
            }
            if (p < r.ram_base_addr || p + 4 > r.ram_base_addr + r.ram_size_addr) {
                Function *f =
                    declFn(r, "xlt_unsupported", r.B.getVoidTy(), {i64T(r), i64T(r)}, true);
                r.B.CreateCall(f, {c64(r, p), c64(r, 0)});
                emitExit(r, c64(r, 0));
                break;
            }
            uint32_t word = 0;
            std::memcpy(&word, ram + (p - r.ram_base_addr), 4);
            Insn insn = decode(p, word);
            ++count;
            ended = lift(r, insn, p);
            p += 4;
            // Bound straight-line runs; huge single blocks make LLVM's scheduler and
            // register allocator superlinear, so re-enter through the dispatcher.
            if (!ended && p - pc > 256 * 4) {
                emitExit(r, c64(r, p));
                ended = true;
            }
        }
    }

    finalizeRegion(r);
    if (instruction_count)
        *instruction_count = count;
    std::string verify_error;
    raw_string_ostream os(verify_error);
    if (verifyModule(*module, &os)) {
        if (error)
            *error = "LLVM verification failed for region 0x" +
                     std::to_string((unsigned long long)entry_pc) + ": " + os.str();
        return nullptr;
    }
    return module;
}

} // namespace xlt
