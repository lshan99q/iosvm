// SPDX-License-Identifier: MIT
/*
 * decode.h — the AArch64 instruction decoder interface.
 *
 * decode() turns one 32-bit instruction word into the neutral xlt::Insn model. The current M1
 * backend is capstone 4; a later table-driven decoder can replace decode.cpp without touching
 * lifters, because the interface and xlt_insn.h do not expose capstone types.
 *
 * pc is the guest virtual address of the instruction. pc-relative operands are normalized to
 * absolute guest addresses using pc. Callers must check Insn::unsupported before using the
 * instruction; an unsupported word still has pc and raw filled in and consumes exactly four
 * bytes.
 */
#ifndef XLT_DECODE_H
#define XLT_DECODE_H

#include "xlt_insn.h"

#include <cstdint>

namespace xlt
{

/* Decodes exactly one AArch64 instruction. Never throws. */
Insn decode(uint64_t pc, uint32_t word);

} // namespace xlt

#endif /* XLT_DECODE_H */
