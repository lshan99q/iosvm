// SPDX-License-Identifier: MIT
/*
 * p2.cpp — stage-1 MMU, synchronous aborts and ERET for the P2 engine.
 *
 * The P2 path is an interpreter over the same neutral Insn model used by the JIT. It is entered
 * only while SCTLR_EL1.M is set, so the M1 flat-RAM fast path is unchanged. Keeping the walker and
 * the exception model in the engine makes the first milestone testable without changing the
 * committed QEMU patch series.
 */
#include "p2.h"

#include "decode.h"
#include "xlt_insn.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>
#include <string>

namespace xlt
{

namespace
{

constexpr uint64_t kRamBase = 0x40000000ULL;
constexpr uint64_t kRamSize = 256ULL * 1024 * 1024;
constexpr uint64_t kPageSize = 4096;
constexpr uint32_t kPageMask = 0xfff;

P2Engine *g_engine = nullptr;

const Operand &none_operand()
{
    static const Operand none{};
    return none;
}

const Operand &op(const Insn &insn, unsigned index)
{
    return index < insn.operand_count ? insn.operands[index] : none_operand();
}

bool is_imm(const Insn &insn, unsigned index)
{
    return index < insn.operand_count && insn.operands[index].kind == OperandKind::kImm;
}

bool is_reg(const Insn &insn, unsigned index)
{
    return index < insn.operand_count && insn.operands[index].kind == OperandKind::kReg &&
           insn.operands[index].reg.valid();
}

bool is_mem(const Insn &insn, unsigned index)
{
    return index < insn.operand_count && insn.operands[index].kind == OperandKind::kMem;
}

uint64_t mask_width(unsigned bits)
{
    return bits >= 64 ? ~0ULL : ((1ULL << bits) - 1);
}

uint64_t sext(uint64_t value, unsigned bits)
{
    if (bits == 0)
        return 0;
    if (bits >= 64)
        return value;
    const uint64_t m = 1ULL << (bits - 1);
    return (value ^ m) - m;
}

} // namespace

struct P2Engine::Impl {
    const HostOps *host = nullptr;
    CpuRegs *regs = nullptr;

    /* System registers. */
    uint64_t sctlr = 0;
    uint64_t ttbr0 = 0;
    uint64_t ttbr1 = 0;
    uint64_t tcr = 0;
    uint64_t mair = 0;
    uint64_t vbar = 0;
    uint64_t esr = 0;
    uint64_t far = 0;
    uint64_t elr = 0;
    uint64_t spsr = 0;
    uint64_t sp_el0 = 0;
    uint64_t sp_el1 = 0;
    uint64_t mdscr = 0;
    uint32_t daif = 0x3c0;
    bool spsel = true;
    bool last_fault_write = false;
    uint32_t last_fault_fsc = 0;

    /* Direct-mapped last-N TLB: 4 sets x 128 entries via a 512-entry table. */
    struct TlbEntry {
        uint64_t va_page = 0;
        uint64_t pa_page = 0;
        bool read = false;
        bool write = false;
        bool execute = false;
        uint32_t generation = 0;
        bool valid = false;
    };
    TlbEntry tlb[512];
    uint32_t generation = 1;

    bool has_exception = false;
    uint64_t cur_pc = 0;

    uint64_t read_phys(uint64_t addr, unsigned size, uint8_t *out) const
    {
        if (addr >= kRamBase && addr <= kRamBase + kRamSize && size <= kRamBase + kRamSize - addr &&
            host != nullptr && host->ram != nullptr) {
            std::memcpy(out, host->ram + (addr - kRamBase), size);
            return size;
        }
        if (host == nullptr || host->read_mem == nullptr) {
            if (host != nullptr && host->fatal != nullptr) {
                host->fatal(host->opaque, "xlt: physical read outside guest RAM", 70);
            }
            return 0;
        }
        const uint64_t value = host->read_mem(host->opaque, addr, size);
        for (unsigned i = 0; i < size; i++)
            out[i] = static_cast<uint8_t>(value >> (8 * i));
        return size;
    }

    void write_phys(uint64_t addr, unsigned size, uint64_t value)
    {
        if (addr >= kRamBase && addr <= kRamBase + kRamSize && size <= kRamBase + kRamSize - addr &&
            host != nullptr && host->ram != nullptr) {
            for (unsigned i = 0; i < size; i++)
                host->ram[(addr - kRamBase) + i] = static_cast<uint8_t>(value >> (8 * i));
            return;
        }
        if (host == nullptr || host->write_mem == nullptr) {
            if (host != nullptr && host->fatal != nullptr) {
                host->fatal(host->opaque, "xlt: physical write outside guest RAM", 70);
            }
            return;
        }
        host->write_mem(host->opaque, addr, value, size);
    }

    uint64_t read_reg(RegRef reg) const
    {
        if (!reg.valid() || reg.is_zero())
            return 0;
        uint64_t value;
        if (reg.is_sp()) {
            value = regs->sp;
        } else {
            const int index = static_cast<int>(reg.gpr);
            value = index >= 0 && index < 31 ? regs->x[index] : 0;
        }
        return reg.width == 32 ? static_cast<uint32_t>(value) : value;
    }

    void write_reg(RegRef reg, uint64_t value)
    {
        if (!reg.valid() || reg.is_zero())
            return;
        if (reg.width == 32)
            value = static_cast<uint32_t>(value);
        if (reg.is_sp())
            regs->sp = value;
        else {
            const int index = static_cast<int>(reg.gpr);
            if (index >= 0 && index < 31)
                regs->x[index] = value;
        }
    }

    uint64_t apply_extend(uint64_t value, ExtendType ext) const
    {
        switch (ext) {
        case ExtendType::kUxtb:
            return value & 0xff;
        case ExtendType::kUxth:
            return value & 0xffff;
        case ExtendType::kUxtw:
            return value & 0xffffffffULL;
        case ExtendType::kUxtx:
            return value;
        case ExtendType::kSxtb:
            return sext(value, 8);
        case ExtendType::kSxth:
            return sext(value, 16);
        case ExtendType::kSxtw:
            return sext(value, 32);
        case ExtendType::kSxtx:
            return value;
        default:
            return value;
        }
    }

    uint64_t apply_shift(uint64_t value, ShiftType type, unsigned amount, bool w) const
    {
        const unsigned bits = w ? 32 : 64;
        const uint64_t mask = mask_width(bits);
        amount &= bits - 1;
        value &= mask;
        switch (type) {
        case ShiftType::kLsl:
            return (value << amount) & mask;
        case ShiftType::kLsr:
            return value >> amount;
        case ShiftType::kAsr:
            return static_cast<uint64_t>(static_cast<int64_t>(sext(value, bits)) >> amount) & mask;
        case ShiftType::kRor: {
            if (amount == 0)
                return value;
            return ((value >> amount) | (value << (bits - amount))) & mask;
        }
        default:
            return value;
        }
    }

    uint64_t eval(const Operand &operand, bool w) const
    {
        if (operand.kind == OperandKind::kImm) {
            uint64_t value = operand.imm;
            if (operand.shift != ShiftType::kNone)
                value = apply_shift(value, operand.shift, operand.shift_amount, w);
            return w ? static_cast<uint32_t>(value) : value;
        }
        if (operand.kind == OperandKind::kReg) {
            uint64_t value = read_reg(operand.reg);
            if (operand.extend != ExtendType::kNone) {
                value = apply_extend(value, operand.extend);
                if (operand.extend_amount != 0)
                    value = apply_shift(value, ShiftType::kLsl, operand.extend_amount, w);
            } else if (operand.shift != ShiftType::kNone) {
                value = apply_shift(value, operand.shift, operand.shift_amount, w);
            }
            return w ? static_cast<uint32_t>(value) : value;
        }
        return 0;
    }

    uint64_t mem_addr(const Operand &mem) const
    {
        uint64_t addr = read_reg(mem.base) + static_cast<uint64_t>(mem.disp);
        if (mem.index.valid()) {
            uint64_t index = read_reg(mem.index);
            if (mem.extend != ExtendType::kNone) {
                index = apply_extend(index, mem.extend);
                if (mem.extend_amount != 0)
                    index = apply_shift(index, ShiftType::kLsl, mem.extend_amount, false);
            } else if (mem.shift != ShiftType::kNone) {
                index = apply_shift(index, mem.shift, mem.shift_amount, false);
            }
            addr += index;
        }
        return addr;
    }

    bool tlb_lookup(uint64_t va, bool write, bool execute, uint64_t *pa) const
    {
        const TlbEntry &entry = tlb[(va >> 12) & 511];
        if (!entry.valid || entry.generation != generation || entry.va_page != (va & ~0xfffULL))
            return false;
        if (write && !entry.write)
            return false;
        if (execute && !entry.execute)
            return false;
        if (!write && !execute && !entry.read)
            return false;
        *pa = entry.pa_page | (va & kPageMask);
        return true;
    }

    void tlb_store(uint64_t va, uint64_t pa, bool read, bool write, bool execute)
    {
        TlbEntry &entry = tlb[(va >> 12) & 511];
        entry.va_page = va & ~0xfffULL;
        entry.pa_page = pa & ~0xfffULL;
        entry.read = read;
        entry.write = write;
        entry.execute = execute;
        entry.generation = generation;
        entry.valid = true;
    }

    void invalidate_tlb()
    {
        generation++;
        for (auto &entry : tlb)
            entry.valid = false;
    }

    int start_level() const
    {
        const unsigned va_bits = 64 - static_cast<unsigned>(tcr & 0x3f);
        if (va_bits >= 48)
            return 0;
        if (va_bits >= 39)
            return 1;
        if (va_bits >= 30)
            return 2;
        return 3;
    }

    bool translate(uint64_t va, bool write, bool execute, uint64_t *pa, uint32_t *fsc)
    {
        if ((sctlr & 1) == 0) {
            *pa = va;
            *fsc = 0;
            return true;
        }
        if (tlb_lookup(va, write, execute, pa))
            return true;

        const int first = start_level();
        const bool use_ttbr1 = (va >> 63) != 0;
        uint64_t table = use_ttbr1 ? ttbr1 : ttbr0;
        table &= 0x0000fffffffff000ULL;
        uint32_t fault = 0;
        bool valid = false;
        uint64_t result = 0;
        bool read_ok = true;
        bool write_ok = true;
        bool exec_ok = true;

        for (int level = first; level <= 3; level++) {
            const unsigned shift = 12 + 9 * (3 - level);
            const uint64_t index = (va >> shift) & 0x1ff;
            uint8_t raw[8];
            if (read_phys(table + index * 8, 8, raw) != 8) {
                fault = 0x04 + static_cast<uint32_t>(level);
                break;
            }
            uint64_t desc = 0;
            for (unsigned i = 0; i < 8; i++)
                desc |= static_cast<uint64_t>(raw[i]) << (8 * i);

            if ((desc & 1) == 0) {
                fault = 0x04 + static_cast<uint32_t>(level);
                break;
            }
            const bool is_block = (desc & 3) == 1;
            const bool is_table = (desc & 3) == 3;
            if (level == 3) {
                if (!is_table) {
                    fault = 0x04 + static_cast<uint32_t>(level);
                    break;
                }
                result = (desc & 0x0000fffffffff000ULL) | (va & kPageMask);
                valid = true;
            } else if (is_block) {
                const unsigned block_bits = 12 + 9 * (3 - level);
                const uint64_t block_mask = mask_width(block_bits);
                result = (desc & 0x0000fffffffff000ULL & ~block_mask) | (va & block_mask);
                valid = true;
            } else if (!is_table) {
                fault = 0x04 + static_cast<uint32_t>(level);
                break;
            }

            if (valid) {
                const bool af = (desc & (1ULL << 10)) != 0;
                const bool ap_readonly = (desc & (1ULL << 7)) != 0;
                const bool ap_user = (desc & (1ULL << 6)) != 0;
                /* In the EL1&0 regime AP=01 is user-RW and, like QEMU's get_S1prot(), is
                 * execute-never at EL1 (PXN || user_rw). User-RO pages (AP=10/11) stay
                 * executable at EL1 subject to PXN; EL0/UXN handling is P2.2. */
                const bool user_rw = ap_user && !ap_readonly;
                const bool xn = (desc & (1ULL << 54)) != 0 || user_rw;
                read_ok = af;
                write_ok = af && !ap_readonly;
                exec_ok = af && !xn;
                if (execute && !exec_ok) {
                    fault = 0x0c + static_cast<uint32_t>(level);
                    valid = false;
                } else if (write && !write_ok) {
                    fault = 0x0c + static_cast<uint32_t>(level);
                    valid = false;
                } else if (!write && !execute && !read_ok) {
                    fault = 0x0c + static_cast<uint32_t>(level);
                    valid = false;
                }
                break;
            }
            table = desc & 0x0000fffffffff000ULL;
        }

        if (!valid) {
            *fsc = fault != 0 ? fault : 0x04;
            return false;
        }
        *pa = result;
        *fsc = 0;
        tlb_store(va, result, read_ok, write_ok, exec_ok);
        return true;
    }

    uint32_t pack_spsr() const
    {
        return (regs->nzcv & 0xf0000000u) | (daif & 0x3c0u) | 0x5u;
    }

    void save_active_sp()
    {
        if (spsel)
            sp_el1 = regs->sp;
        else
            sp_el0 = regs->sp;
    }

    void load_active_sp()
    {
        regs->sp = spsel ? sp_el1 : sp_el0;
    }

    void deliver_fault(bool instruction, uint64_t fault_pc, uint64_t address, bool write,
                       uint32_t fsc)
    {
        save_active_sp();
        const uint32_t ec = instruction ? 0x21 : 0x25;
        esr =
            (static_cast<uint64_t>(ec) << 26) | (1ULL << 25) | (write ? 0x40u : 0u) | (fsc & 0x3f);
        far = address;
        elr = fault_pc;
        spsr = pack_spsr();
        daif |= 0x3c0;
        spsel = true;
        regs->sp = sp_el1;
        regs->pc = (vbar & ~0x7ffULL) + 0x200;
        has_exception = true;
    }

    bool read_data(uint64_t address, unsigned size, uint64_t *value)
    {
        uint64_t result = 0;
        /* Keep a device access as one transaction: splitting a 32-bit PL011 write into four
         * byte writes would enqueue three NUL characters after the real byte. */
        if (size <= 8 && (address & kPageMask) + size <= kPageSize) {
            uint64_t pa = 0;
            uint32_t fsc = 0;
            if (!translate(address, false, false, &pa, &fsc)) {
                last_fault_write = false;
                last_fault_fsc = fsc;
                deliver_fault(false, cur_pc, address, false, fsc);
                return false;
            }
            uint8_t bytes[8] = {};
            if (read_phys(pa, size, bytes) != size) {
                if (error_on_phys_failure())
                    return false;
            }
            for (unsigned i = 0; i < size; i++)
                result |= static_cast<uint64_t>(bytes[i]) << (8 * i);
            *value = result;
            return true;
        }
        for (unsigned i = 0; i < size; i++) {
            uint64_t pa = 0;
            uint32_t fsc = 0;
            if (!translate(address + i, false, false, &pa, &fsc)) {
                last_fault_write = false;
                last_fault_fsc = fsc;
                deliver_fault(false, cur_pc, address + i, false, fsc);
                return false;
            }
            uint8_t byte = 0;
            read_phys(pa, 1, &byte);
            result |= static_cast<uint64_t>(byte) << (8 * i);
        }
        *value = result;
        return true;
    }

    bool write_data(uint64_t address, unsigned size, uint64_t value)
    {
        if (size <= 8 && (address & kPageMask) + size <= kPageSize) {
            uint64_t pa = 0;
            uint32_t fsc = 0;
            if (!translate(address, true, false, &pa, &fsc)) {
                last_fault_write = true;
                last_fault_fsc = fsc;
                deliver_fault(false, cur_pc, address, true, fsc);
                return false;
            }
            write_phys(pa, size, value);
            return true;
        }
        for (unsigned i = 0; i < size; i++) {
            uint64_t pa = 0;
            uint32_t fsc = 0;
            if (!translate(address + i, true, false, &pa, &fsc)) {
                last_fault_write = true;
                last_fault_fsc = fsc;
                deliver_fault(false, cur_pc, address + i, true, fsc);
                return false;
            }
            write_phys(pa, 1, static_cast<uint8_t>(value >> (8 * i)));
        }
        return true;
    }

    static bool error_on_phys_failure()
    {
        return true;
    }

    bool fetch(uint64_t pc, uint32_t *word)
    {
        uint8_t bytes[4];
        for (unsigned i = 0; i < 4; i++) {
            uint64_t pa = 0;
            uint32_t fsc = 0;
            if (!translate(pc + i, false, true, &pa, &fsc)) {
                deliver_fault(true, pc, pc + i, false, fsc);
                return false;
            }
            read_phys(pa, 1, &bytes[i]);
        }
        *word = static_cast<uint32_t>(bytes[0]) | (static_cast<uint32_t>(bytes[1]) << 8) |
                (static_cast<uint32_t>(bytes[2]) << 16) | (static_cast<uint32_t>(bytes[3]) << 24);
        return true;
    }

    bool read_flag_n() const
    {
        return (regs->nzcv & kN) != 0;
    }
    bool read_flag_z() const
    {
        return (regs->nzcv & kZ) != 0;
    }
    bool read_flag_c() const
    {
        return (regs->nzcv & kC) != 0;
    }
    bool read_flag_v() const
    {
        return (regs->nzcv & kV) != 0;
    }

    void set_flags(bool n, bool z, bool c, bool v)
    {
        regs->nzcv = (n ? kN : 0) | (z ? kZ : 0) | (c ? kC : 0) | (v ? kV : 0);
    }

    bool condition(Cond cond) const
    {
        const bool n = read_flag_n();
        const bool z = read_flag_z();
        const bool c = read_flag_c();
        const bool v = read_flag_v();
        switch (cond) {
        case Cond::kEq:
            return z;
        case Cond::kNe:
            return !z;
        case Cond::kHs:
            return c;
        case Cond::kLo:
            return !c;
        case Cond::kMi:
            return n;
        case Cond::kPl:
            return !n;
        case Cond::kVs:
            return v;
        case Cond::kVc:
            return !v;
        case Cond::kHi:
            return c && !z;
        case Cond::kLs:
            return !c || z;
        case Cond::kGe:
            return n == v;
        case Cond::kLt:
            return n != v;
        case Cond::kGt:
            return !z && n == v;
        case Cond::kLe:
            return z || n != v;
        case Cond::kAl:
            return true;
        case Cond::kNv:
            return false;
        }
        return false;
    }

    void add_flags(uint64_t a, uint64_t b, bool sub, bool use_carry, bool carry_in, bool w,
                   uint64_t *result)
    {
        const unsigned bits = w ? 32 : 64;
        const uint64_t mask = mask_width(bits);
        a &= mask;
        b &= mask;
        const uint64_t carry = use_carry && carry_in ? 1 : 0;
        const uint64_t b2 = sub ? (mask - b + (use_carry && !carry_in ? 1 : 0)) & mask : b;
        const unsigned __int128 ua = a;
        const unsigned __int128 ub = b;
        unsigned __int128 full;
        if (sub) {
            const unsigned __int128 subtrahend = static_cast<unsigned __int128>(b) + carry;
            full = (static_cast<unsigned __int128>(mask) + 1 + ua) - subtrahend;
            *result = static_cast<uint64_t>(full) & mask;
        } else {
            full = ua + ub + carry;
            *result = static_cast<uint64_t>(full) & mask;
        }
        const bool c =
            sub ? (ua >= (static_cast<unsigned __int128>(b) + carry)) : ((full >> bits) != 0);
        __int128 sa = static_cast<__int128>(sext(a, bits));
        __int128 sb = static_cast<__int128>(sext(b, bits)) +
                      (sub ? static_cast<__int128>(carry) : -static_cast<__int128>(carry));
        if (!sub)
            sb = static_cast<__int128>(sext(b, bits)) + static_cast<__int128>(carry);
        const __int128 sres = static_cast<__int128>(sext(*result, bits));
        const bool v = (sa >= 0) != (sb >= 0) && (sres >= 0) != (sa >= 0);
        set_flags((*result >> (bits - 1)) & 1, *result == 0, c, v);
        (void)b2;
    }

    bool execute(const Insn &insn, bool *pc_written);
    void eret()
    {
        const uint32_t saved = static_cast<uint32_t>(spsr);
        save_active_sp();
        regs->nzcv = saved & 0xf0000000u;
        daif = saved & 0x3c0u;
        spsel = (saved & 1u) != 0;
        load_active_sp();
        regs->pc = elr;
    }

    bool sysreg_read(uint32_t reg, uint64_t *value) const
    {
        switch (reg) {
        case 0xc080:
            *value = sctlr;
            return true;
        case 0xc100:
            *value = ttbr0;
            return true;
        case 0xc101:
            *value = ttbr1;
            return true;
        case 0xc102:
            *value = tcr;
            return true;
        case 0xc510:
            *value = mair;
            return true;
        case 0xc600:
            *value = vbar;
            return true;
        case 0xc290:
            *value = esr;
            return true;
        case 0xc300:
            *value = far;
            return true;
        case 0xc201:
            *value = elr;
            return true;
        case 0xc200:
            *value = spsr;
            return true;
        case 0xc208:
            *value = spsel ? sp_el0 : regs->sp;
            return true;
        case 0xe208:
            *value = spsel ? regs->sp : sp_el1;
            return true;
        case 0xc212:
            *value = 4;
            return true;
        case 0xda11:
            *value = daif;
            return true;
        case 0x8012:
            *value = mdscr;
            return true;
        case 0xc038:
            *value = 0;
            return true;
        case 0xc039:
            *value = 0;
            return true;
        default:
            return false;
        }
    }

    bool sysreg_write(uint32_t reg, uint64_t value)
    {
        if (host != nullptr && host->write_sysreg != nullptr) {
            host->write_sysreg(host->opaque, reg, value);
        }
        switch (reg) {
        case 0xc080:
            sctlr = value;
            invalidate_tlb();
            return true;
        case 0xc100:
            ttbr0 = value;
            invalidate_tlb();
            return true;
        case 0xc101:
            ttbr1 = value;
            invalidate_tlb();
            return true;
        case 0xc102:
            tcr = value;
            invalidate_tlb();
            return true;
        case 0xc510:
            mair = value;
            invalidate_tlb();
            return true;
        case 0xc600:
            vbar = value;
            invalidate_tlb();
            return true;
        case 0xc290:
            esr = value;
            return true;
        case 0xc300:
            far = value;
            return true;
        case 0xc201:
            elr = value;
            return true;
        case 0xc200:
            spsr = value;
            return true;
        case 0xc208:
            sp_el0 = value;
            if (!spsel)
                regs->sp = value;
            return true;
        case 0xe208:
            sp_el1 = value;
            if (spsel)
                regs->sp = value;
            return true;
        case 0xda11:
            daif = static_cast<uint32_t>(value) & 0x3c0;
            return true;
        case 0x8012:
            mdscr = value;
            return true;
        case 0xc212:
        case 0xc038:
        case 0xc039:
            return true;
        default:
            return false;
        }
    }

    bool pstate_write(uint32_t pstate, uint64_t immediate)
    {
        switch (pstate) {
        case 0x05: /* SPSel */
            save_active_sp();
            spsel = (immediate & 1) != 0;
            load_active_sp();
            return true;
        case 0x1e: /* DAIFSet */
            daif |= (static_cast<uint32_t>(immediate) & 0xf) << 6;
            return true;
        case 0x1f: /* DAIFClr */
            daif &= ~((static_cast<uint32_t>(immediate) & 0xf) << 6);
            return true;
        default:
            return false;
        }
    }

    bool run(uint64_t max_instructions, uint64_t *executed, std::string *error)
    {
        uint64_t count = 0;
        unsigned fault_streak = 0;
        while (regs->halted == 0 && enabled()) {
            cur_pc = regs->pc;
            uint32_t word = 0;
            if (!fetch(cur_pc, &word)) {
                if (++fault_streak > 4) {
                    if (error != nullptr)
                        *error = "instruction abort loop at a vector";
                    return -1;
                }
                continue;
            }
            fault_streak = 0;
            const Insn insn = decode(cur_pc, word);
            if (insn.unsupported) {
                if (error != nullptr) {
                    char buf[64];
                    std::snprintf(buf, sizeof(buf), "unsupported P2 instruction %08x at %llx", word,
                                  static_cast<unsigned long long>(cur_pc));
                    *error = buf;
                }
                return -1;
            }
            bool pc_written = false;
            if (!execute(insn, &pc_written)) {
                if (error != nullptr)
                    *error = "P2 execution error";
                return -1;
            }
            if (!pc_written && regs->halted == 0)
                regs->pc = cur_pc + 4;
            count++;
            if (max_instructions != 0 && count > max_instructions) {
                if (error != nullptr)
                    *error = "P2 instruction limit exceeded";
                return -1;
            }
        }
        *executed = count;
        return 0;
    }

    bool enabled() const
    {
        return (sctlr & 1) != 0;
    }
};

P2Engine::P2Engine(const HostOps *host, CpuRegs *regs) : impl_(std::make_unique<Impl>())
{
    impl_->host = host;
    impl_->regs = regs;
    impl_->sp_el1 = regs->sp;
    impl_->sp_el0 = 0;
    impl_->spsel = true;
    g_engine = this;
}

P2Engine::~P2Engine()
{
    if (g_engine == this)
        g_engine = nullptr;
}

bool P2Engine::enabled() const
{
    return impl_->enabled();
}

int P2Engine::run(uint64_t max_instructions, uint64_t *executed, std::string *error)
{
    return impl_->run(max_instructions, executed, error);
}

bool P2Engine::sysreg_read(uint32_t reg, uint64_t *value) const
{
    return impl_->sysreg_read(reg, value);
}

bool P2Engine::sysreg_write(uint32_t reg, uint64_t value)
{
    return impl_->sysreg_write(reg, value);
}

bool P2Engine::pstate_write(uint32_t pstate, uint64_t immediate)
{
    return impl_->pstate_write(pstate, immediate);
}

void P2Engine::eret()
{
    impl_->eret();
}

bool p2_sysreg_read(uint32_t reg, uint64_t *value)
{
    return g_engine != nullptr && g_engine->sysreg_read(reg, value);
}

bool p2_sysreg_write(uint32_t reg, uint64_t value)
{
    return g_engine != nullptr && g_engine->sysreg_write(reg, value);
}

bool p2_pstate_write(uint32_t pstate, uint64_t immediate)
{
    return g_engine != nullptr && g_engine->pstate_write(pstate, immediate);
}

bool P2Engine::Impl::execute(const Insn &insn, bool *pc_written)
{
    *pc_written = false;
    const bool w = is_reg(insn, 0) && op(insn, 0).reg.width == 32;
    const unsigned n = insn.operand_count;
    auto rd = [&]() { return op(insn, 0).reg; };
    auto read = [&](unsigned i) { return read_reg(op(insn, i).reg); };
    auto write = [&](unsigned i, uint64_t value) { write_reg(op(insn, i).reg, value); };
    auto carry = [&]() { return read_flag_c(); };

    switch (insn.opcode) {
    case Opcode::kNop:
    case Opcode::kWfe:
    case Opcode::kWfi:
    case Opcode::kSev:
    case Opcode::kSevl:
    case Opcode::kYield:
    case Opcode::kIsb:
    case Opcode::kDsb:
    case Opcode::kDmb:
        return true;

    case Opcode::kB:
        regs->pc = insn.target;
        *pc_written = true;
        return true;
    case Opcode::kBCond:
        regs->pc = condition(insn.cond) ? insn.target : cur_pc + 4;
        *pc_written = true;
        return true;
    case Opcode::kBl:
        regs->x[30] = cur_pc + 4;
        regs->pc = insn.target;
        *pc_written = true;
        return true;
    case Opcode::kBr:
        regs->pc = read(0);
        *pc_written = true;
        return true;
    case Opcode::kBlr:
        regs->x[30] = cur_pc + 4;
        regs->pc = read(0);
        *pc_written = true;
        return true;
    case Opcode::kRet:
        regs->pc = n > 0 ? read(0) : regs->x[30];
        *pc_written = true;
        return true;
    case Opcode::kCbz:
    case Opcode::kCbnz: {
        const uint64_t value = read(0);
        bool zero = w ? static_cast<uint32_t>(value) == 0 : value == 0;
        if (insn.opcode == Opcode::kCbnz)
            zero = !zero;
        regs->pc = zero ? insn.target : cur_pc + 4;
        *pc_written = true;
        return true;
    }
    case Opcode::kTbz:
    case Opcode::kTbnz: {
        const uint64_t value = read(0);
        const unsigned bit = static_cast<unsigned>(op(insn, 1).imm);
        const bool set = ((value >> bit) & 1) != 0;
        const bool take = insn.opcode == Opcode::kTbz ? !set : set;
        regs->pc = take ? insn.target : cur_pc + 4;
        *pc_written = true;
        return true;
    }
    case Opcode::kAdr:
    case Opcode::kAdrp:
        write(0, op(insn, 1).imm);
        return true;

    case Opcode::kMov:
        write(0, eval(op(insn, 1), w));
        return true;
    case Opcode::kMovz:
    case Opcode::kMovn:
    case Opcode::kMovk: {
        const uint64_t imm = op(insn, 1).imm << op(insn, 1).shift_amount;
        uint64_t value;
        if (insn.opcode == Opcode::kMovz)
            value = imm;
        else if (insn.opcode == Opcode::kMovn)
            value = ~imm;
        else {
            const uint64_t keep = ~(0xffffULL << op(insn, 1).shift_amount);
            value = (read(0) & keep) | imm;
        }
        write(0, value);
        return true;
    }

    case Opcode::kCmp:
    case Opcode::kCmn: {
        const uint64_t a = read(0);
        const uint64_t b = eval(op(insn, 1), w);
        uint64_t result = 0;
        add_flags(a, b, insn.opcode == Opcode::kCmp, false, false, w, &result);
        return true;
    }
    case Opcode::kAdd:
    case Opcode::kSub: {
        const uint64_t a = read(1);
        const uint64_t b = eval(op(insn, 2), w);
        uint64_t result = 0;
        if (insn.sets_flags) {
            add_flags(a, b, insn.opcode == Opcode::kSub, false, false, w, &result);
        } else {
            result = insn.opcode == Opcode::kSub ? a - b : a + b;
            if (w)
                result = static_cast<uint32_t>(result);
        }
        write(0, result);
        return true;
    }
    case Opcode::kNeg: {
        const uint64_t a = read(1);
        uint64_t result = 0;
        if (insn.sets_flags)
            add_flags(0, a, true, false, false, w, &result);
        else
            result = static_cast<uint64_t>(-static_cast<int64_t>(a));
        write(0, result);
        return true;
    }
    case Opcode::kAdc:
    case Opcode::kSbc: {
        const uint64_t a = read(1);
        const uint64_t b = read(2);
        const bool sub = insn.opcode == Opcode::kSbc;
        const uint64_t result = sub ? a - b - (carry() ? 0 : 1) : a + b + (carry() ? 1 : 0);
        if (insn.sets_flags) {
            uint64_t flags_result = 0;
            add_flags(a, b, sub, true, carry(), w, &flags_result);
        }
        write(0, result);
        return true;
    }

    case Opcode::kAnd:
    case Opcode::kOrr:
    case Opcode::kEor:
    case Opcode::kOrn:
    case Opcode::kBic: {
        const uint64_t a = read(1);
        const uint64_t b = eval(op(insn, 2), w);
        uint64_t result;
        switch (insn.opcode) {
        case Opcode::kAnd:
            result = a & b;
            break;
        case Opcode::kOrr:
            result = a | b;
            break;
        case Opcode::kEor:
            result = a ^ b;
            break;
        case Opcode::kOrn:
            result = a | ~b;
            break;
        default:
            result = a & ~b;
            break;
        }
        if (insn.sets_flags)
            set_flags(((result >> (w ? 31 : 63)) & 1) != 0, result == 0, false, false);
        write(0, result);
        return true;
    }
    case Opcode::kMvn:
        write(0, ~read(1));
        return true;
    case Opcode::kTst: {
        const uint64_t a = read(0);
        const uint64_t b = eval(op(insn, 1), w);
        const uint64_t result = a & b;
        set_flags(((result >> (w ? 31 : 63)) & 1) != 0, result == 0, false, false);
        return true;
    }

    case Opcode::kLsl:
    case Opcode::kLsr:
    case Opcode::kAsr:
    case Opcode::kRor: {
        const uint64_t a = read(1);
        unsigned amount = static_cast<unsigned>(is_imm(insn, 2) ? op(insn, 2).imm : read(2));
        amount &= w ? 31 : 63;
        ShiftType type = ShiftType::kLsl;
        if (insn.opcode == Opcode::kLsr)
            type = ShiftType::kLsr;
        else if (insn.opcode == Opcode::kAsr)
            type = ShiftType::kAsr;
        else if (insn.opcode == Opcode::kRor)
            type = ShiftType::kRor;
        const uint64_t result = apply_shift(a, type, amount, w);
        if (insn.sets_flags)
            set_flags(((result >> (w ? 31 : 63)) & 1) != 0, result == 0, false, false);
        write(0, result);
        return true;
    }

    case Opcode::kUbfm:
    case Opcode::kSbfm:
    case Opcode::kBfm: {
        const uint64_t src = read(1);
        const unsigned bits = w ? 32 : 64;
        const unsigned immr = static_cast<unsigned>(op(insn, 2).imm) & (bits - 1);
        const unsigned imms = static_cast<unsigned>(op(insn, 3).imm) & (bits - 1);
        const uint64_t mask = bits == 32 ? 0xffffffffULL : ~0ULL;
        uint64_t result = 0;
        if (insn.opcode == Opcode::kBfm) {
            const uint64_t dst = read(0);
            unsigned width;
            unsigned shift;
            if (imms >= immr) {
                width = imms - immr + 1;
                shift = immr;
                result =
                    (dst & ~(mask_width(width) << shift)) | ((src & mask_width(width)) << shift);
            } else {
                width = imms + 1;
                shift = bits - immr;
                result =
                    (dst & ~(mask_width(width) << shift)) | ((src & mask_width(width)) << shift);
            }
        } else {
            unsigned width;
            if (imms >= immr) {
                width = imms - immr + 1;
                result = (src >> immr) & mask_width(width);
            } else {
                width = imms + 1;
                result = (src & mask_width(width)) << (bits - immr);
            }
            if (insn.opcode == Opcode::kSbfm)
                result = sext(result, width);
        }
        write(0, result & mask);
        return true;
    }
    case Opcode::kUbfx:
    case Opcode::kSbfx: {
        const uint64_t src = read(1);
        const unsigned lsb = static_cast<unsigned>(op(insn, 2).imm);
        const unsigned width = static_cast<unsigned>(op(insn, 3).imm);
        uint64_t result = (src >> lsb) & mask_width(width);
        if (insn.opcode == Opcode::kSbfx)
            result = sext(result, width);
        write(0, result);
        return true;
    }
    case Opcode::kBfxil: {
        const uint64_t src = read(1);
        const uint64_t dst = read(0);
        const unsigned lsb = static_cast<unsigned>(op(insn, 2).imm);
        const unsigned width = static_cast<unsigned>(op(insn, 3).imm);
        write(0, (dst & ~(mask_width(width) << lsb)) | ((src & mask_width(width)) << lsb));
        return true;
    }
    case Opcode::kExtr: {
        const uint64_t a = read(1);
        const uint64_t b = read(2);
        const unsigned lsb = static_cast<unsigned>(op(insn, 3).imm) & (w ? 31 : 63);
        const unsigned bits = w ? 32 : 64;
        const uint64_t mask = mask_width(bits);
        const uint64_t result = lsb == 0 ? b : (((a & mask) >> lsb) | ((b & mask) << (bits - lsb)));
        write(0, result & mask);
        return true;
    }

    case Opcode::kMul: {
        const uint64_t result = read(1) * read(2);
        write(0, result);
        return true;
    }
    case Opcode::kMadd:
    case Opcode::kMsub: {
        const uint64_t product = read(1) * read(2);
        const uint64_t add = read(3);
        write(0, insn.opcode == Opcode::kMadd ? product + add : add - product);
        return true;
    }
    case Opcode::kSmulh:
    case Opcode::kUmulh: {
        const __int128 product =
            insn.opcode == Opcode::kSmulh
                ? static_cast<__int128>(static_cast<int64_t>(read(1))) *
                      static_cast<__int128>(static_cast<int64_t>(read(2)))
                : static_cast<__int128>(static_cast<unsigned __int128>(read(1)) *
                                        static_cast<unsigned __int128>(read(2)));
        write(0, static_cast<uint64_t>(product >> 64));
        return true;
    }
    case Opcode::kSdiv:
    case Opcode::kUdiv: {
        const uint64_t a = read(1);
        const uint64_t b = read(2);
        uint64_t result = 0;
        if (b != 0) {
            if (insn.opcode == Opcode::kUdiv) {
                result = a / b;
            } else {
                const int64_t sa = static_cast<int64_t>(a);
                const int64_t sb = static_cast<int64_t>(b);
                if (sa == std::numeric_limits<int64_t>::min() && sb == -1)
                    result = a;
                else
                    result = static_cast<uint64_t>(sa / sb);
            }
        }
        write(0, result);
        return true;
    }

    case Opcode::kCsel:
    case Opcode::kCsinc:
    case Opcode::kCsinv:
    case Opcode::kCsneg: {
        const uint64_t t = read(1);
        uint64_t f = read(2);
        if (!condition(insn.cond)) {
            if (insn.opcode == Opcode::kCsinc)
                f += 1;
            else if (insn.opcode == Opcode::kCsinv)
                f = ~f;
            else if (insn.opcode == Opcode::kCsneg)
                f = static_cast<uint64_t>(-static_cast<int64_t>(f));
        }
        write(0, condition(insn.cond) ? t : f);
        return true;
    }
    case Opcode::kCset:
        write(0, condition(insn.cond) ? 1 : 0);
        return true;
    case Opcode::kCsetm:
        write(0, condition(insn.cond) ? ~0ULL : 0);
        return true;
    case Opcode::kCinc:
    case Opcode::kCinv:
    case Opcode::kCneg: {
        uint64_t value = read(1);
        if (!condition(insn.cond)) {
            if (insn.opcode == Opcode::kCinc)
                value += 1;
            else if (insn.opcode == Opcode::kCinv)
                value = ~value;
            else
                value = static_cast<uint64_t>(-static_cast<int64_t>(value));
        }
        write(0, value);
        return true;
    }
    case Opcode::kCcmp:
    case Opcode::kCcmn: {
        const uint64_t a = read(0);
        const uint64_t b = eval(op(insn, 1), w);
        if (condition(insn.cond)) {
            uint64_t result = 0;
            add_flags(a, b, insn.opcode == Opcode::kCcmp, false, false, w, &result);
        } else if (is_imm(insn, 2)) {
            regs->nzcv = static_cast<uint32_t>(op(insn, 2).imm & 0xfu) << 28;
        }
        return true;
    }

    case Opcode::kClz: {
        uint64_t value = read(1) & (w ? 0xffffffffULL : ~0ULL);
        unsigned count = 0;
        const unsigned bits = w ? 32 : 64;
        while (count < bits && ((value >> (bits - 1 - count)) & 1) == 0)
            count++;
        write(0, count);
        return true;
    }
    case Opcode::kRbit: {
        uint64_t value = read(1);
        uint64_t result = 0;
        const unsigned bits = w ? 32 : 64;
        for (unsigned i = 0; i < bits; i++)
            result |= ((value >> i) & 1) << (bits - 1 - i);
        write(0, result);
        return true;
    }
    case Opcode::kRev: {
        uint64_t value = read(1);
        uint64_t result = 0;
        const unsigned bytes = w ? 4 : 8;
        for (unsigned i = 0; i < bytes; i++)
            result |= ((value >> (8 * i)) & 0xff) << (8 * (bytes - 1 - i));
        write(0, result);
        return true;
    }
    case Opcode::kRev16: {
        const uint64_t value = read(1);
        write(0, ((value & 0xff00ff00ff00ff00ULL) >> 8) | ((value & 0x00ff00ff00ff00ffULL) << 8));
        return true;
    }
    case Opcode::kRev32: {
        const uint64_t value = read(1);
        const uint64_t result =
            ((value & 0xff000000ff000000ULL) >> 24) | ((value & 0x00ff000000ff0000ULL) >> 8) |
            ((value & 0x0000ff000000ff00ULL) << 8) | ((value & 0x000000ff000000ffULL) << 24);
        write(0, result);
        return true;
    }

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
        const bool load = insn.opcode == Opcode::kLdr || insn.opcode == Opcode::kLdrb ||
                          insn.opcode == Opcode::kLdrh || insn.opcode == Opcode::kLdrsb ||
                          insn.opcode == Opcode::kLdrsh || insn.opcode == Opcode::kLdrsw ||
                          insn.opcode == Opcode::kLdp || insn.opcode == Opcode::kLdpsw ||
                          insn.opcode == Opcode::kLdur || insn.opcode == Opcode::kLdurb ||
                          insn.opcode == Opcode::kLdurh || insn.opcode == Opcode::kLdursb ||
                          insn.opcode == Opcode::kLdursh || insn.opcode == Opcode::kLdursw;
        const bool pair = insn.opcode == Opcode::kLdp || insn.opcode == Opcode::kStp ||
                          insn.opcode == Opcode::kLdpsw || insn.access_count == 2;
        int mem_index = -1;
        for (unsigned i = 0; i < n; i++) {
            if (is_mem(insn, i)) {
                mem_index = static_cast<int>(i);
                break;
            }
        }
        uint64_t addr = 0;
        RegRef base_reg;
        if (mem_index >= 0) {
            const Operand &mem = op(insn, static_cast<unsigned>(mem_index));
            base_reg = mem.base;
            addr = mem_addr(mem);
        } else {
            addr = op(insn, 1).imm;
        }
        unsigned size = insn.access_width;
        if (size == 0)
            size = is_reg(insn, 0) && op(insn, 0).reg.width == 32 ? 4 : 8;
        auto load_one = [&](unsigned reg_index, uint64_t address) -> bool {
            uint64_t value = 0;
            if (!read_data(address, size, &value))
                return false;
            switch (insn.opcode) {
            case Opcode::kLdrsb:
            case Opcode::kLdursb:
                value = sext(value & 0xff, 8);
                break;
            case Opcode::kLdrsh:
            case Opcode::kLdursh:
                value = sext(value & 0xffff, 16);
                break;
            case Opcode::kLdrsw:
            case Opcode::kLdursw:
                value = sext(value & 0xffffffffULL, 32);
                break;
            case Opcode::kLdpsw:
                value = sext(value & 0xffffffffULL, 32);
                break;
            default:
                break;
            }
            write_reg(op(insn, reg_index).reg, value);
            return true;
        };
        if (load) {
            if (!load_one(0, addr)) {
                *pc_written = true;
                return true;
            }
            if (pair && !load_one(1, addr + size)) {
                *pc_written = true;
                return true;
            }
        } else {
            const uint64_t v0 = read(0);
            write_data(addr, size, v0);
            if (regs->pc != cur_pc && has_exception) {
                *pc_written = true;
                return true;
            }
            if (pair) {
                const uint64_t v1 = read(1);
                write_data(addr + size, size, v1);
            }
        }
        if (base_reg.valid() && insn.writeback) {
            const uint64_t next = addr + (insn.post_index ? insn.writeback_disp : 0);
            write_reg(base_reg, next);
        }
        return true;
    }

    case Opcode::kMrs: {
        const uint32_t reg = op(insn, 1).sysreg;
        uint64_t value = 0;
        if (!sysreg_read(reg, &value)) {
            if (host != nullptr && host->read_sysreg != nullptr)
                value = host->read_sysreg(host->opaque, reg);
            else
                value = 0;
        }
        write(0, value);
        return true;
    }
    case Opcode::kMsr: {
        const Operand &first = op(insn, 0);
        if (first.kind == OperandKind::kPstate) {
            pstate_write(first.pstate, op(insn, 1).imm);
            return true;
        }
        const uint32_t reg = first.sysreg;
        const uint64_t value = is_reg(insn, 1) ? read(1) : op(insn, 1).imm;
        if (!sysreg_write(reg, value) && host != nullptr && host->write_sysreg != nullptr)
            host->write_sysreg(host->opaque, reg, value);
        return true;
    }
    case Opcode::kHlt:
        if (host != nullptr && host->semihost != nullptr)
            host->semihost(host->opaque, regs);
        return true;
    case Opcode::kHvc:
    case Opcode::kSmc:
        if (host != nullptr && host->hvc != nullptr)
            host->hvc(host->opaque, regs,
                      static_cast<uint32_t>(is_imm(insn, 0) ? op(insn, 0).imm : 0));
        return true;
    case Opcode::kEret:
        eret();
        *pc_written = true;
        return true;
    case Opcode::kTlbi:
    case Opcode::kDc:
    case Opcode::kIc:
        invalidate_tlb();
        return true;
    case Opcode::kBrk:
    case Opcode::kSvc:
        return true;

    case Opcode::kUnsupported:
    default:
        return false;
    }
}

} // namespace xlt
