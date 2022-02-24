/*
 * Copyright (C) 2014 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "assembler_x86_64.h"

#include "base/casts.h"
#include "entrypoints/quick/quick_entrypoints.h"
#include "memory_region.h"
#include "thread.h"

namespace art {
namespace x86_64 {

std::ostream& operator<<(std::ostream& os, const CpuRegister& reg) {
  return os << reg.AsRegister();
}

std::ostream& operator<<(std::ostream& os, const XmmRegister& reg) {
  return os << reg.AsFloatRegister();
}

std::ostream& operator<<(std::ostream& os, const X87Register& reg) {
  return os << "ST" << static_cast<int>(reg);
}

void X86_64Assembler::call(CpuRegister reg) {
  AssemblerBuffer::EnsureCapacity ensured(&buffer_);
  EmitOptionalRex32(reg);
  EmitUint8(0xFF);
  EmitRegisterOperand(2, reg.LowBits());
}


void X86_64Assembler::call(const Address& address) {
  AssemblerBuffer::EnsureCapacity ensured(&buffer_);
  EmitOptionalRex32(address);
  EmitUint8(0xFF);
  EmitOperand(2, address);
}


void X86_64Assembler::call(Label* label) {
  AssemblerBuffer::EnsureCapacity ensured(&buffer_);
  EmitUint8(0xE8);
  static const int kSize = 5;
  // Offset by one because we already have emitted the opcode.
  EmitLabel(label, kSize - 1);
}

void X86_64Assembler::pushq(CpuRegister reg) {
  AssemblerBuffer::EnsureCapacity ensured(&buffer_);
  EmitOptionalRex32(reg);
  EmitUint8(0x50 + reg.LowBits());
}


void X86_64Assembler::pushq(const Address& address) {
  AssemblerBuffer::EnsureCapacity ensured(&buffer_);
  EmitOptionalRex32(address);
  EmitUint8(0xFF);
  EmitOperand(6, address);
}


void X86_64Assembler::pushq(const Immediate& imm) {
  AssemblerBuffer::EnsureCapacity ensured(&buffer_);
  CHECK(imm.is_int32());  // pushq only supports 32b immediate.
  if (imm.is_int8()) {
    EmitUint8(0x6A);
    EmitUint8(imm.value() & 0xFF);
  } else {
    EmitUint8(0x68);
    EmitImmediate(imm);
  }
}


void X86_64Assembler::popq(CpuRegister reg) {
  AssemblerBuffer::EnsureCapacity ensured(&buffer_);
  EmitOptionalRex32(reg);
  EmitUint8(0x58 + reg.LowBits());
}


void X86_64Assembler::popq(const Address& address) {
  AssemblerBuffer::EnsureCapacity ensured(&buffer_);
  EmitOptionalRex32(address);
  EmitUint8(0x8F);
  EmitOperand(0, address);
}


void X86_64Assembler::movq(CpuRegister dst, const Immediate& imm) {
  AssemblerBuffer::EnsureCapacity ensured(&buffer_);
  if (imm.is_int32()) {
    // 32 bit. Note: sign-extends.
    EmitRex64(dst);
    EmitUint8(0xC7);
    EmitRegisterOperand(0, dst.LowBits());
    EmitInt32(static_cast<int32_t>(imm.value()));
  } else {
    EmitRex64(dst);
    EmitUint8(0xB8 + dst.LowBits());
    EmitInt64(imm.value());
  }
}


void X86_64Assembler::movl(CpuRegister dst, const Immediate& imm) {
  CHECK(imm.is_int32());
  AssemblerBuffer::EnsureCapacity ensured(&buffer_);
  EmitOptionalRex32(dst);
  EmitUint8(0xB8 + dst.LowBits());
  EmitImmediate(imm);
}


void X86_64Assembler::movq(const Address& dst, const Immediate& imm) {
  CHECK(imm.is_int32());
  AssemblerBuffer::EnsureCapacity ensured(&buffer_);
  EmitRex64(dst);
  EmitUint8(0xC7);
  EmitOperand(0, dst);
  EmitImmediate(imm);
}


void X86_64Assembler::movq(CpuRegister dst, CpuRegister src) {
  AssemblerBuffer::EnsureCapacity ensured(&buffer_);
  // 0x89 is movq r/m64 <- r64, with op1 in r/m and op2 in reg: so reverse EmitRex64
  EmitRex64(src, dst);
  EmitUint8(0x89);
  EmitRegisterOperand(src.LowBits(), dst.LowBits());
}


void X86_64Assembler::movl(CpuRegister dst, CpuRegister src) {
  AssemblerBuffer::EnsureCapacity ensured(&buffer_);
  EmitOptionalRex32(dst, src);
  EmitUint8(0x8B);
  EmitRegisterOperand(dst.LowBits(), src.LowBits());
}


void X86_64Assembler::movq(CpuRegister dst, const Address& src) {
  AssemblerBuffer::EnsureCapacity ensured(&buffer_);
  EmitRex64(dst, src);
  EmitUint8(0x8B);
  EmitOperand(dst.LowBits(), src);
}


void X86_64Assembler::movl(CpuRegister dst, const Address& src) {
  AssemblerBuffer::EnsureCapacity ensured(&buffer_);
  EmitOptionalRex32(dst, src);
  EmitUint8(0x8B);
  EmitOperand(dst.LowBits(), src);
}


void X86_64Assembler::movq(const Address& dst, CpuRegister src) {
  AssemblerBuffer::EnsureCapacity ensured(&buffer_);
  EmitRex64(src, dst);
  EmitUint8(0x89);
  EmitOperand(src.LowBits(), dst);
}


void X86_64Assembler::movl(const Address& dst, CpuRegister src) {
  AssemblerBuffer::EnsureCapacity ensured(&buffer_);
  EmitOptionalRex32(src, dst);
  EmitUint8(0x89);
  EmitOperand(src.LowBits(), dst);
}

void X86_64Assembler::movl(const Address& dst, const Immediate& imm) {
  AssemblerBuffer::EnsureCapacity ensured(&buffer_);
  EmitOptionalRex32(dst);
  EmitUint8(0xC7);
  EmitOperand(0, dst);
  EmitImmediate(imm);
}


void X86_64Assembler::cmov(Condition c, CpuRegister dst, CpuRegister src) {
  cmov(c, dst, src, true);
}

void X86_64Assembler::cmov(Condition c, CpuRegister dst, CpuRegister src, bool is64bit) {
  AssemblerBuffer::EnsureCapacity ensured(&buffer_);
  EmitOptionalRex(false, is64bit, dst.NeedsRex(), false, src.NeedsRex());
  EmitUint8(0x0F);
  EmitUint8(0x40 + c);
  EmitRegisterOperand(dst.LowBits(), src.LowBits());
}


void X86_64Assembler::movzxb(CpuRegister dst, CpuRegister src) {
  AssemblerBuffer::EnsureCapacity ensured(&buffer_);
  EmitOptionalByteRegNormalizingRex32(dst, src);
  EmitUint8(0x0F);
  EmitUint8(0xB6);
  EmitRegisterOperand(dst.LowBits(), src.LowBits());
}


void X86_64Assembler::movzxb(CpuRegister dst, const Address& src) {
  AssemblerBuffer::EnsureCapacity ensured(&buffer_);
  // Byte register is only in the source register form, so we don't use
  // EmitOptionalByteRegNormalizingRex32(dst, src);
  EmitOptionalRex32(dst, src);
  EmitUint8(0x0F);
  EmitUint8(0xB6);
  EmitOperand(dst.LowBits(), src);
}


void X86_64Assembler::movsxb(CpuRegister dst, CpuRegister src) {
  AssemblerBuffer::EnsureCapacity ensured(&buffer_);
  EmitOptionalByteRegNormalizingRex32(dst, src);
  EmitUint8(0x0F);
  EmitUint8(0xBE);
  EmitRegisterOperand(dst.LowBits(), src.LowBits());
}


void X86_64Assembler::movsxb(CpuRegister dst, const Address& src) {
  AssemblerBuffer::EnsureCapacity ensured(&buffer_);
  // Byte register is only in the source register form, so we don't use
  // EmitOptionalByteRegNormalizingRex32(dst, src);
  EmitOptionalRex32(dst, src);
  EmitUint8(0x0F);
  EmitUint8(0xBE);
  EmitOperand(dst.LowBits(), src);
}


void X86_64Assembler::movb(CpuRegister /*dst*/, const Address& /*src*/) {
  LOG(FATAL) << "Use movzxb or movsxb instead.";
}


void X86_64Assembler::movb(const Address& dst, CpuRegister src) {
  AssemblerBuffer::EnsureCapacity ensured(&buffer_);
  EmitOptionalByteRegNormalizingRex32(src, dst);
  EmitUint8(0x88);
  EmitOperand(src.LowBits(), dst);
}


void X86_64Assembler::movb(const Address& dst, const Immediate& imm) {
  AssemblerBuffer::EnsureCapacity ensured(&buffer_);
  EmitOptionalRex32(dst);
  EmitUint8(0xC6);
  EmitOperand(Register::RAX, dst);
  CHECK(imm.is_int8());
  EmitUint8(imm.value() & 0xFF);
}


void X86_64Assembler::movzxw(CpuRegister dst, CpuRegister src) {
  AssemblerBuffer::EnsureCapacity ensured(&buffer_);
  EmitOptionalRex32(dst, src);
  EmitUint8(0x0F);
  EmitUint8(0xB7);
  EmitRegisterOperand(dst.LowBits(), src.LowBits());
}


void X86_64Assembler::movzxw(CpuRegister dst, const Address& src) {
  AssemblerBuffer::EnsureCapacity ensured(&buffer_);
  EmitOptionalRex32(dst, src);
  EmitUint8(0x0F);
  EmitUint8(0xB7);
  EmitOperand(dst.LowBits(), src);
}


void X86_64Assembler::movsxw(CpuRegister dst, CpuRegister src) {
  AssemblerBuffer::EnsureCapacity ensured(&buffer_);
  EmitOptionalRex32(dst, src);
  EmitUint8(0x0F);
  EmitUint8(0xBF);
  EmitRegisterOperand(dst.LowBits(), src.LowBits());
}


void X86_64Assembler::movsxw(CpuRegister dst, const Address& src) {
  AssemblerBuffer::EnsureCapacity ensured(&buffer_);
  EmitOptionalRex32(dst, src);
  EmitUint8(0x0F);
  EmitUint8(0xBF);
  EmitOperand(dst.LowBits(), src);
}


void X86_64Assembler::movw(CpuRegister /*dst*/, const Address& /*src*/) {
  LOG(FATAL) << "Use movzxw or movsxw instead.";
}


void X86_64Assembler::movw(const Address& dst, CpuRegister src) {
  AssemblerBuffer::EnsureCapacity ensured(&buffer_);
  EmitOperandSizeOverride();
  EmitOptionalRex32(src, dst);
  EmitUint8(0x89);
  EmitOperand(src.LowBits(), dst);
}


void X86_64Assembler::movw(const Address& dst, const Immediate& imm) {
  AssemblerBuffer::EnsureCapacity ensured(&buffer_);
  EmitOperandSizeOverride();
  EmitOptionalRex32(dst);
  EmitUint8(0xC7);
  EmitOperand(Register::RAX, dst);
  CHECK(imm.is_uint16() || imm.is_int16());
  EmitUint8(imm.value() & 0xFF);
  EmitUint8(imm.value() >> 8);
}


void X86_64Assembler::leaq(CpuRegister dst, const Address& src) {
  AssemblerBuffer::EnsureCapacity ensured(&buffer_);
  EmitRex64(dst, src);
  EmitUint8(0x8D);
  EmitOperand(dst.LowBits(), src);
}


void X86_64Assembler::leal(CpuRegister dst, const Address& src) {
  AssemblerBuffer::EnsureCapacity ensured(&buffer_);
  EmitOptionalRex32(dst, src);
  EmitUint8(0x8D);
  EmitOperand(dst.LowBits(), src);
}


void X86_64Assembler::movaps(XmmRegister dst, XmmRegister src) {
  AssemblerBuffer::EnsureCapacity ensured(&buffer_);
  EmitOptionalRex32(dst, src);
  EmitUint8(0x0F);
  EmitUint8(0x28);
  EmitXmmRegisterOperand(dst.LowBits(), src);
}


void X86_64Assembler::movss(XmmRegister dst, const Address& src) {
  AssemblerBuffer::EnsureCapacity ensured(&buffer_);
  EmitUint8(0xF3);
  EmitOptionalRex32(dst, src);
  EmitUint8(0x0F);
  EmitUint8(0x10);
  EmitOperand(dst.LowBits(), src);
}


void X86_64Assembler::movss(const Address& dst, XmmRegister src) {
  AssemblerBuffer::EnsureCapacity ensured(&buffer_);
  EmitUint8(0xF3);
  EmitOptionalRex32(src, dst);
  EmitUint8(0x0F);
  EmitUint8(0x11);
  EmitOperand(src.LowBits(), dst);
}


void X86_64Assembler::movss(XmmRegister dst, XmmRegister src) {
  AssemblerBuffer::EnsureCapacity ensured(&buffer_);
  EmitUint8(0xF3);
  EmitOptionalRex32(src, dst);  // Movss is MR encoding instead of the usual RM.
  EmitUint8(0x0F);
  EmitUint8(0x11);
  EmitXmmRegisterOperand(src.LowBits(), dst);
}


void X86_64Assembler::movsxd(CpuRegister dst, CpuRegister src) {
  AssemblerBuffer::EnsureCapacity ensured(&buffer_);
  EmitRex64(dst, src);
  EmitUint8(0x63);
  EmitRegisterOperand(dst.LowBits(), src.LowBits());
}


void X86_64Assembler::movsxd(CpuRegister dst, const Address& src) {
  AssemblerBuffer::EnsureCapacity ensured(&buffer_);
  EmitRex64(dst, src);
  EmitUint8(0x63);
  EmitOperand(dst.LowBits(), src);
}


void X86_64Assembler::movd(XmmRegister dst, CpuRegister src) {
  movd(dst, src, true);
}

void X86_64Assembler::movd(CpuRegister dst, XmmRegister src) {
  movd(dst, src, true);
}

void X86_64Assembler::movd(XmmRegister dst, CpuRegister src, bool is64bit) {
  AssemblerBuffer::EnsureCapacity ensured(&buffer_);
  EmitUint8(0x66);
  EmitOptionalRex(false, is64bit, dst.NeedsRex(), false, src.NeedsRex());
  EmitUint8(0x0F);
  EmitUint8(0x6E);
  EmitOperand(dst.LowBits(), Operand(src));
}

void X86_64Assembler::movd(CpuRegister dst, XmmRegister src, bool is64bit) {
  AssemblerBuffer::EnsureCapacity ensured(&buffer_);
  EmitUint8(0x66);
  EmitOptionalRex(false, is64bit, src.NeedsRex(), false, dst.NeedsRex());
  EmitUint8(0x0F);
  EmitUint8(0x7E);
  EmitOperand(src.LowBits(), Operand(dst));
}


void X86_64Assembler::addss(XmmRegister dst, XmmRegister src) {
  AssemblerBuffer::EnsureCapacity ensured(&buffer_);
  EmitUint8(0xF3);
  EmitOptionalRex32(dst, src);
  EmitUint8(0x0F);
  EmitUint8(0x58);
  EmitXmmRegisterOperand(dst.LowBits(), src);
}


void X86_64Assembler::addss(XmmRegister dst, const Address& src) {
  AssemblerBuffer::EnsureCapacity ensured(&buffer_);
  EmitUint8(0xF3);
  EmitOptionalRex32(dst, src);
  EmitUint8(0x0F);
  EmitUint8(0x58);
  EmitOperand(dst.LowBits(), src);
}


void X86_64Assembler::subss(XmmRegister dst, XmmRegister src) {
  AssemblerBuffer::EnsureCapacity ensured(&buffer_);
  EmitUint8(0xF3);
  EmitOptionalRex32(dst, src);
  EmitUint8(0x0F);
  EmitUint8(0x5C);
  EmitXmmRegisterOperand(dst.LowBits(), src);
}


void X86_64Assembler::subss(XmmRegister dst, const Address& src) {
  AssemblerBuffer::EnsureCapacity ensured(&buffer_);
  EmitUint8(0xF3);
  EmitOptionalRex32(dst, src);
  EmitUint8(0x0F);
  EmitUint8(0x5C);
  EmitOperand(dst.LowBits(), src);
}


void X86_64Assembler::mulss(XmmRegister dst, XmmRegister src) {
  AssemblerBuffer::EnsureCapacity ensured(&buffer_);
  EmitUint8(0xF3);
  EmitOptionalRex32(dst, src);
  EmitUint8(0x0F);
  EmitUint8(0x59);
  EmitXmmRegisterOperand(dst.LowBits(), src);
}


void X86_64Assembler::mulss(XmmRegister dst, const Address& src) {
  AssemblerBuffer::EnsureCapacity ensured(&buffer_);
  EmitUint8(0xF3);
  EmitOptionalRex32(dst, src);
  EmitUint8(0x0F);
  EmitUint8(0x59);
  EmitOperand(dst.LowBits(), src);
}


void X86_64Assembler::divss(XmmRegister dst, XmmRegister src) {
  AssemblerBuffer::EnsureCapacity ensured(&buffer_);
  EmitUint8(0xF3);
  EmitOptionalRex32(dst, src);
  EmitUint8(0x0F);
  EmitUint8(0x5E);
  EmitXmmRegisterOperand(dst.LowBits(), src);
}


void X86_64Assembler::divss(XmmRegister dst, const Address& src) {
  AssemblerBuffer::EnsureCapacity ensured(&buffer_);
  EmitUint8(0xF3);
  EmitOptionalRex32(dst, src);
  EmitUint8(0x0F);
  EmitUint8(0x5E);
  EmitOperand(dst.LowBits(), src);
}


void X86_64Assembler::flds(const Address& src) {
  AssemblerBuffer::EnsureCapacity ensured(&buffer_);
  EmitUint8(0xD9);
  EmitOperand(0, src);
}


void X86_64Assembler::fsts(const Address& dst) {
  AssemblerBuffer::EnsureCapacity ensured(&buffer_);
  EmitUint8(0xD9);
  EmitOperand(2, dst);
}


void X86_64Assembler::fstps(const Address& dst) {
  AssemblerBuffer::EnsureCapacity ensured(&buffer_);
  EmitUint8(0xD9);
  EmitOperand(3, dst);
}


void X86_64Assembler::movsd(XmmRegister dst, const Address& src) {
  AssemblerBuffer::EnsureCapacity ensured(&buffer_);
  EmitUint8(0xF2);
  EmitOptionalRex32(dst, src);
  EmitUint8(0x0F);
  EmitUint8(0x10);
  EmitOperand(dst.LowBits(), src);
}


void X86_64Assembler::movsd(const Address& dst, XmmRegister src) {
  AssemblerBuffer::EnsureCapacity ensured(&buffer_);
  EmitUint8(0xF2);
  EmitOptionalRex32(src, dst);
  EmitUint8(0x0F);
  EmitUint8(0x11);
  EmitOperand(src.LowBits(), dst);
}


void X86_64Assembler::movsd(XmmRegister dst, XmmRegister src) {
  AssemblerBuffer::EnsureCapacity ensured(&buffer_);
  EmitUint8(0xF2);
  EmitOptionalRex32(src, dst);  // Movsd is MR encoding instead of the usual RM.
  EmitUint8(0x0F);
  EmitUint8(0x11);
  EmitXmmRegisterOperand(src.LowBits(), dst);
}


void X86_64Assembler::addsd(XmmRegister dst, XmmRegister src) {
  AssemblerBuffer::EnsureCapacity ensured(&buffer_);
  EmitUint8(0xF2);
  EmitOptionalRex32(dst, src);
  EmitUint8(0x0F);
  EmitUint8(0x58);
  EmitXmmRegisterOperand(dst.LowBits(), src);
}


void X86_64Assembler::addsd(XmmRegister dst, const Address& src) {
  AssemblerBuffer::EnsureCapacity ensured(&buffer_);
  EmitUint8(0xF2);
  EmitOptionalRex32(dst, src);
  EmitUint8(0x0F);
  EmitUint8(0x58);
  EmitOperand(dst.LowBits(), src);
}


void X86_64Assembler::subsd(XmmRegister dst, XmmRegister src) {
  AssemblerBuffer::EnsureCapacity ensured(&buffer_);
  EmitUint8(0xF2);
  EmitOptionalRex32(dst, src);
  EmitUint8(0x0F);
  EmitUint8(0x5C);
  EmitXmmRegisterOperand(dst.LowBits(), src);
}


void X86_64Assembler::subsd(XmmRegister dst, const Address& src) {
  AssemblerBuffer::EnsureCapacity ensured(&buffer_);
  EmitUint8(0xF2);
  EmitOptionalRex32(dst, src);
  EmitUint8(0x0F);
  EmitUint8(0x5C);
  EmitOperand(dst.LowBits(), src);
}


void X86_64Assembler::mulsd(XmmRegister dst, XmmRegister src) {
  AssemblerBuffer::EnsureCapacity ensured(&buffer_);
  EmitUint8(0xF2);
  EmitOptionalRex32(dst, src);
  EmitUint8(0x0F);
  EmitUint8(0x59);
  EmitXmmRegisterOperand(dst.LowBits(), src);
}


void X86_64Assembler::mulsd(XmmRegister dst, const Address& src) {
  AssemblerBuffer::EnsureCapacity ensured(&buffer_);
  EmitUint8(0xF2);
  EmitOptionalRex32(dst, src);
  EmitUint8(0x0F);
  EmitUint8(0x59);
  EmitOperand(dst.LowBits(), src);
}


void X86_64Assembler::divsd(XmmRegister dst, XmmRegister src) {
  AssemblerBuffer::EnsureCapacity ensured(&buffer_);
  EmitUint8(0xF2);
  EmitOptionalRex32(dst, src);
  EmitUint8(0x0F);
  EmitUint8(0x5E);
  EmitXmmRegisterOperand(dst.LowBits(), src);
}


void X86_64Assembler::divsd(XmmRegister dst, const Address& src) {
  AssemblerBuffer::EnsureCapacity ensured(&buffer_);
  EmitUint8(0xF2);
  EmitOptionalRex32(dst, src);
  EmitUint8(0x0F);
  EmitUint8(0x5E);
  EmitOperand(dst.LowBits(), src);
}


void X86_64Assembler::cvtsi2ss(XmmRegister dst, CpuRegister src) {
  cvtsi2ss(dst, src, false);
}


void X86_64Assembler::cvtsi2ss(XmmRegister dst, CpuRegister src, bool is64bit) {
  AssemblerBuffer::EnsureCapacity ensured(&buffer_);
  EmitUint8(0xF3);
  if (is64bit) {
    // Emit a REX.W prefix if the operand size is 64 bits.
    EmitRex64(dst, src);
  } else {
    EmitOptionalRex32(dst, src);
  }
  EmitUint8(0x0F);
  EmitUint8(0x2A);
  EmitOperand(dst.LowBits(), Operand(src));
}


void X86_64Assembler::cvtsi2ss(XmmRegister dst, const Address& src, bool is64bit) {
  AssemblerBuffer::EnsureCapacity ensured(&buffer_);
  EmitUint8(0xF3);
  if (is64bit) {
    // Emit a REX.W prefix if the operand size is 64 bits.
    EmitRex64(dst, src);
  } else {
    EmitOptionalRex32(dst, src);
  }
  EmitUint8(0x0F);
  EmitUint8(0x2A);
  EmitOperand(dst.LowBits(), src);
}


void X86_64Assembler::cvtsi2sd(XmmRegister dst, CpuRegister src) {
  cvtsi2sd(dst, src, false);
}


void X86_64Assembler::cvtsi2sd(XmmRegister dst, CpuRegister src, bool is64bit) {
  AssemblerBuffer::EnsureCapacity ensured(&buffer_);
  EmitUint8(0xF2);
  if (is64bit) {
    // Emit a REX.W prefix if the operand size is 64 bits.
    EmitRex64(dst, src);
  } else {
    EmitOptionalRex32(dst, src);
  }
  EmitUint8(0x0F);
  EmitUint8(0x2A);
  EmitOperand(dst.LowBits(), Operand(src));
}


void X86_64Assembler::cvtsi2sd(XmmRegister dst, const Address& src, bool is64bit) {
  AssemblerBuffer::EnsureCapacity ensured(&buffer_);
  EmitUint8(0xF2);
  if (is64bit) {
    // Emit a REX.W prefix if the operand size is 64 bits.
    EmitRex64(dst, src);
  } else {
    EmitOptionalRex32(dst, src);
  }
  EmitUint8(0x0F);
  EmitUint8(0x2A);
  EmitOperand(dst.LowBits(), src);
}


void X86_64Assembler::cvtss2si(CpuRegister dst, XmmRegister src) {
  AssemblerBuffer::EnsureCapacity ensured(&buffer_);
  EmitUint8(0xF3);
  EmitOptionalRex32(dst, src);
  EmitUint8(0x0F);
  EmitUint8(0x2D);
  EmitXmmRegisterOperand(dst.LowBits(), src);
}


void X86_64Assembler::cvtss2sd(XmmRegister dst, XmmRegister src) {
  AssemblerBuffer::EnsureCapacity ensured(&buffer_);
  EmitUint8(0xF3);
  EmitOptionalRex32(dst, src);
  EmitUint8(0x0F);
  EmitUint8(0x5A);
  EmitXmmRegisterOperand(dst.LowBits(), src);
}


void X86_64Assembler::cvtss2sd(XmmRegister dst, const Address& src) {
  AssemblerBuffer::EnsureCapacity ensured(&buffer_);
  EmitUint8(0xF3);
  EmitOptionalRex32(dst, src);
  EmitUint8(0x0F);
  EmitUint8(0x5A);
  EmitOperand(dst.LowBits(), src);
}


void X86_64Assembler::cvtsd2si(CpuRegister dst, XmmRegister src) {
  AssemblerBuffer::EnsureCapacity ensured(&buffer_);
  EmitUint8(0xF2);
  EmitOptionalRex32(dst, src);
  EmitUint8(0x0F);
  EmitUint8(0x2D);
  EmitXmmRegisterOperand(dst.LowBits(), src);
}


void X86_64Assembler::cvttss2si(CpuRegister dst, XmmRegister src) {
  cvttss2si(dst, src, false);
}


void X86_64Assembler::cvttss2si(CpuRegister dst, XmmRegister src, bool is64bit) {
  AssemblerBuffer::EnsureCapacity ensured(&buffer_);
  EmitUint8(0xF3);
  if (is64bit) {
    // Emit a REX.W prefix if the operand size is 64 bits.
    EmitRex64(dst, src);
  } else {
    EmitOptionalRex32(dst, src);
  }
  EmitUint8(0x0F);
  EmitUint8(0x2C);
  EmitXmmRegisterOperand(dst.LowBits(), src);
}


void X86_64Assembler::cvttsd2si(CpuRegister dst, XmmRegister src) {
  cvttsd2si(dst, src, false);
}


void X86_64Assembler::cvttsd2si(CpuRegister dst, XmmRegister src, bool is64bit) {
  AssemblerBuffer::EnsureCapacity ensured(&buffer_);
  EmitUint8(0xF2);
  if (is64bit) {
    // Emit a REX.W prefix if the operand size is 64 bits.
    EmitRex64(dst, src);
  } else {
    EmitOptionalRex32(dst, src);
  }
  EmitUint8(0x0F);
  EmitUint8(0x2C);
  EmitXmmRegisterOperand(dst.LowBits(), src);
}


void X86_64Assembler::cvtsd2ss(XmmRegister dst, XmmRegister src) {
  AssemblerBuffer::EnsureCapacity ensured(&buffer_);
  EmitUint8(0xF2);
  EmitOptionalRex32(dst, src);
  EmitUint8(0x0F);
  EmitUint8(0x5A);
  EmitXmmRegisterOperand(dst.LowBits(), src);
}


void X86_64Assembler::cvtsd2ss(XmmRegister dst, const Address& src) {
  AssemblerBuffer::EnsureCapacity ensured(&buffer_);
  EmitUint8(0xF2);
  EmitOptionalRex32(dst, src);
  EmitUint8(0x0F);
  EmitUint8(0x5A);
  EmitOperand(dst.LowBits(), src);
}


void X86_64Assembler::cvtdq2pd(XmmRegister dst, XmmRegister src) {
  AssemblerBuffer::EnsureCapacity ensured(&buffer_);
  EmitUint8(0xF3);
  EmitOptionalRex32(dst, src);
  EmitUint8(0x0F);
  EmitUint8(0xE6);
  EmitXmmRegisterOperand(dst.LowBits(), src);
}


void X86_64Assembler::comiss(XmmRegister a, XmmRegister b) {
  AssemblerBuffer::EnsureCapacity ensured(&buffer_);
  EmitOptionalRex32(a, b);
  EmitUint8(0x0F);
  EmitUint8(0x2F);
  EmitXmmRegisterOperand(a.LowBits(), b);
}


void X86_64Assembler::comiss(XmmRegister a, const Address& b) {
  AssemblerBuffer::EnsureCapacity ensured(&buffer_);
  EmitOptionalRex32(a, b);
  EmitUint8(0x0F);
  EmitUint8(0x2F);
  EmitOperand(a.LowBits(), b);
}


void X86_64Assembler::comisd(XmmRegister a, XmmRegister b) {
  AssemblerBuffer::EnsureCapacity ensured(&buffer_);
  EmitUint8(0x66);
  EmitOptionalRex32(a, b);
  EmitUint8(0x0F);
  EmitUint8(0x2F);
  EmitXmmRegisterOperand(a.LowBits(), b);
}


void X86_64Assembler::comisd(XmmRegister a, const Address& b) {
  AssemblerBuffer::EnsureCapacity ensured(&buffer_);
  EmitUint8(0x66);
  EmitOptionalRex32(a, b);
  EmitUint8(0x0F);
  EmitUint8(0x2F);
  EmitOperand(a.LowBits(), b);
}


void X86_64Assembler::ucomiss(XmmRegister a, XmmRegister b) {
  AssemblerBuffer::EnsureCapacity ensured(&buffer_);
  EmitOptionalRex32(a, b);
  EmitUint8(0x0F);
  EmitUint8(0x2E);
  EmitXmmRegisterOperand(a.LowBits(), b);
}


void X86_64Assembler::ucomiss(XmmRegister a, const Address& b) {
  AssemblerBuffer::EnsureCapacity ensured(&buffer_);
  EmitOptionalRex32(a, b);
  EmitUint8(0x0F);
  EmitUint8(0x2E);
  EmitOperand(a.LowBits(), b);
}


void X86_64Assembler::ucomisd(XmmRegister a, XmmRegister b) {
  AssemblerBuffer::EnsureCapacity ensured(&buffer_);
  EmitUint8(0x66);
  EmitOptionalRex32(a, b);
  EmitUint8(0x0F);
  EmitUint8(0x2E);
  EmitXmmRegisterOperand(a.LowBits(), b);
}


void X86_64Assembler::ucomisd(XmmRegister a, const Address& b) {
  AssemblerBuffer::EnsureCapacity ensured(&buffer_);
  EmitUint8(0x66);
  EmitOptionalRex32(a, b);
  EmitUint8(0x0F);
  EmitUint8(0x2E);
  EmitOperand(a.LowBits(), b);
}


void X86_64Assembler::roundsd(XmmRegister dst, XmmRegister src, const Immediate& imm) {
  AssemblerBuffer::EnsureCapacity ensured(&buffer_);
  EmitUint8(0x66);
  EmitOptionalRex32(dst, src);
  EmitUint8(0x0F);
  EmitUint8(0x3A);
  EmitUint8(0x0B);
  EmitXmmRegisterOperand(dst.LowBits(), src);
  EmitUint8(imm.value());
}


void X86_64Assembler::roundss(XmmRegister dst, XmmRegister src, const Immediate& imm) {
  AssemblerBuffer::EnsureCapacity ensured(&buffer_);
  EmitUint8(0x66);
  EmitOptionalRex32(dst, src);
  EmitUint8(0x0F);
  EmitUint8(0x3A);
  EmitUint8(0x0A);
  EmitXmmRegisterOperand(dst.LowBits(), src);
  EmitUint8(imm.value());
}


void X86_64Assembler::sqrtsd(XmmRegister dst, XmmRegister src) {
  AssemblerBuffer::EnsureCapacity ensured(&buffer_);
  EmitUint8(0xF2);
  EmitOptionalRex32(dst, src);
  EmitUint8(0x0F);
  EmitUint8(0x51);
  EmitXmmRegisterOperand(dst.LowBits(), src);
}


void X86_64Assembler::sqrtss(XmmRegister dst, XmmRegister src) {
  AssemblerBuffer::EnsureCapacity ensured(&buffer_);
  EmitUint8(0xF3);
  EmitOptionalRex32(dst, src);
  EmitUint8(0x0F);
  EmitUint8(0x51);
  EmitXmmRegisterOperand(dst.LowBits(), src);
}


void X86_64Assembler::xorpd(XmmRegister dst, const Address& src) {
  AssemblerBuffer::EnsureCapacity ensured(&buffer_);
  EmitUint8(0x66);
  EmitOptionalRex32(dst, src);
  EmitUint8(0x0F);
  EmitUint8(0x57);
  EmitOperand(dst.LowBits(), src);
}


void X86_64Assembler::xorpd(XmmRegister dst, XmmRegister src) {
  AssemblerBuffer::EnsureCapacity ensured(&buffer_);
  EmitUint8(0x66);
  EmitOptionalRex32(dst, src);
  EmitUint8(0x0F);
  EmitUint8(0x57);
  EmitXmmRegisterOperand(dst.LowBits(), src);
}


void X86_64Assembler::xorps(XmmRegister dst, const Address& src) {
  AssemblerBuffer::EnsureCapacity ensured(&buffer_);
  EmitOptionalRex32(dst, src);
  EmitUint8(0x0F);
  EmitUint8(0x57);
  EmitOperand(dst.LowBits(), src);
}


void X86_64Assembler::xorps(XmmRegister dst, XmmRegister src) {
  AssemblerBuffer::EnsureCapacity ensured(&buffer_);
  EmitOptionalRex32(dst, src);
  EmitUint8(0x0F);
  EmitUint8(0x57);
  EmitXmmRegisterOperand(dst.LowBits(), src);
}


void X86_64Assembler::andpd(XmmRegister dst, const Address& src) {
  AssemblerBuffer::EnsureCapacity ensured(&buffer_);
  EmitUint8(0x66);
  EmitOptionalRex32(dst, src);
  EmitUint8(0x0F);
  EmitUint8(0x54);
  EmitOperand(dst.LowBits(), src);
}

void X86_64Assembler::andpd(XmmRegister dst, XmmRegister src) {
  AssemblerBuffer::EnsureCapacity ensured(&buffer_);
  EmitUint8(0x66);
  EmitOptionalRex32(dst, src);
  EmitUint8(0x0F);
  EmitUint8(0x54);
  EmitXmmRegisterOperand(dst.LowBits(), src);
}

void X86_64Assembler::andps(XmmRegister dst, XmmRegister src) {
  AssemblerBuffer::EnsureCapacity ensured(&buffer_);
  EmitOptionalRex32(dst, src);
  EmitUint8(0x0F);
  EmitUint8(0x54);
  EmitXmmRegisterOperand(dst.LowBits(), src);
}

void X86_64Assembler::orpd(XmmRegister dst, XmmRegister src) {
  AssemblerBuffer::EnsureCapacity ensured(&buffer_);
  EmitUint8(0x66);
  EmitOptionalRex32(dst, src);
  EmitUint8(0x0F);
  EmitUint8(0x56);
  EmitXmmRegisterOperand(dst.LowBits(), src);
}

void X86_64Assembler::orps(XmmRegister dst, XmmRegister src) {
  AssemblerBuffer::EnsureCapacity ensured(&buffer_);
  EmitOptionalRex32(dst, src);
  EmitUint8(0x0F);
  EmitUint8(0x56);
  EmitXmmRegisterOperand(dst.LowBits(), src);
}

void X86_64Assembler::fldl(const Address& src) {
  AssemblerBuffer::EnsureCapacity ensured(&buffer_);
  EmitUint8(0xDD);
  EmitOperand(0, src);
}


void X86_64Assembler::fstl(const Address& dst) {
  AssemblerBuffer::EnsureCapacity ensured(&buffer_);
  EmitUint8(0xDD);
  EmitOperand(2, dst);
}


void X86_64Assembler::fstpl(const Address& dst) {
  AssemblerBuffer::EnsureCapacity ensured(&buffer_);
  EmitUint8(0xDD);
  EmitOperand(3, dst);
}


void X86_64Assembler::fstsw() {
  AssemblerBuffer::EnsureCapacity ensured(&buffer_);
  EmitUint8(0x9B);
  EmitUint8(0xDF);
  EmitUint8(0xE0);
}


void X86_64Assembler::fnstcw(const Address& dst) {
  AssemblerBuffer::EnsureCapacity ensured(&buffer_);
  EmitUint8(0xD9);
  EmitOperand(7, dst);
}


void X86_64Assembler::fldcw(const Address& src) {
  AssemblerBuffer::EnsureCapacity ensured(&buffer_);
  EmitUint8(0xD9);
  EmitOperand(5, src);
}


void X86_64Assembler::fistpl(const Address& dst) {
  AssemblerBuffer::EnsureCapacity ensured(&buffer_);
  EmitUint8(0xDF);
  EmitOperand(7, dst);
}


void X86_64Assembler::fistps(const Address& dst) {
  AssemblerBuffer::EnsureCapacity ensured(&buffer_);
  EmitUint8(0xDB);
  EmitOperand(3, dst);
}


void X86_64Assembler::fildl(const Address& src) {
  AssemblerBuffer::EnsureCapacity ensured(&buffer_);
  EmitUint8(0xDF);
  EmitOperand(5, src);
}


void X86_64Assembler::filds(const Address& src) {
  AssemblerBuffer::EnsureCapacity ensured(&buffer_);
  EmitUint8(0xDB);
  EmitOperand(0, src);
}


void X86_64Assembler::fincstp() {
  AssemblerBuffer::EnsureCapacity ensured(&buffer_);
  EmitUint8(0xD9);
  EmitUint8(0xF7);
}


void X86_64Assembler::ffree(const Immediate& index) {
  CHECK_LT(index.value(), 7);
  AssemblerBuffer::EnsureCapacity ensured(&buffer_);
  EmitUint8(0xDD);
  EmitUint8(0xC0 + index.value());
}


void X86_64Assembler::fsin() {
  AssemblerBuffer::EnsureCapacity ensured(&buffer_);
  EmitUint8(0xD9);
  EmitUint8(0xFE);
}


void X86_64Assembler::fcos() {
  AssemblerBuffer::EnsureCapacity ensured(&buffer_);
  EmitUint8(0xD9);
  EmitUint8(0xFF);
}


void X86_64Assembler::fptan() {
  AssemblerBuffer::EnsureCapacity ensured(&buffer_);
  EmitUint8(0xD9);
  EmitUint8(0xF2);
}

void X86_64Assembler::fucompp() {
  AssemblerBuffer::EnsureCapacity ensured(&buffer_);
  EmitUint8(0xDA);
  EmitUint8(0xE9);
}


void X86_64Assembler::fprem() {
  AssemblerBuffer::EnsureCapacity ensured(&buffer_);
  EmitUint8(0xD9);
  EmitUint8(0xF8);
}


void X86_64Assembler::xchgl(CpuRegister dst, CpuRegister src) {
  AssemblerBuffer::EnsureCapacity ensured(&buffer_);
  // There is a short version for rax.
  // It's a bit awkward, as CpuRegister has a const field, so assignment and thus swapping doesn't
  // work.
  const bool src_rax = src.AsRegister() == RAX;
  const bool dst_rax = dst.AsRegister() == RAX;
  if (src_rax || dst_rax) {
    EmitOptionalRex32(src_rax ? dst : src);
    EmitUint8(0x90 + (src_rax ? dst.LowBits() : src.LowBits()));
    return;
  }

  // General case.
  EmitOptionalRex32(src, dst);
  EmitUint8(0x87);
  EmitRegisterOperand(src.LowBits(), dst.LowBits());
}


void X86_64Assembler::xchgq(CpuRegister dst, CpuRegister src) {
  AssemblerBuffer::EnsureCapacity ensured(&buffer_);
  // There is a short version for rax.
  // It's a bit awkward, as CpuRegister has a const field, so assignment and thus swapping doesn't
  // work.
  const bool src_rax = src.AsRegister() == RAX;
  const bool dst_rax = dst.AsRegister() == RAX;
  if (src_rax || dst_rax) {
    // If src == target, emit a nop instead.
    if (src_rax && dst_rax) {
      EmitUint8(0x90);
    } else {
      EmitRex64(src_rax ? dst : src);
      EmitUint8(0x90 + (src_rax ? dst.LowBits() : src.LowBits()));
    }
    return;
  }

  // General case.
  EmitRex64(src, dst);
  EmitUint8(0x87);
  EmitRegisterOperand(src.LowBits(), dst.LowBits());
}


void X86_64Assembler::xchgl(CpuRegister reg, const Address& address) {
  AssemblerBuffer::EnsureCapacity ensured(&buffer_);
  EmitOptionalRex32(reg, address);
  EmitUint8(0x87);
  EmitOperand(reg.LowBits(), address);
}


void X86_64Assembler::cmpw(const Address& address, const Immediate& imm) {
  AssemblerBuffer::EnsureCapacity ensured(&buffer_);
  EmitOptionalRex32(address);
  EmitUint8(0x66);
  EmitComplex(7, address, imm);
}


void X86_64Assembler::cmpl(CpuRegister reg, const Immediate& imm) {
  AssemblerBuffer::EnsureCapacity ensured(&buffer_);
  EmitOptionalRex32(reg);
  EmitComplex(7, Operand(reg), imm);
}


void X86_64Assembler::cmpl(CpuRegister reg0, CpuRegister reg1) {
  AssemblerBuffer::EnsureCapacity ensured(&buffer_);
  EmitOptionalRex32(reg0, reg1);
  EmitUint8(0x3B);
  EmitOperand(reg0.LowBits(), Operand(reg1));
}


void X86_64Assembler::cmpl(CpuRegister reg, const Address& address) {
  AssemblerBuffer::EnsureCapacity ensured(&buffer_);
  EmitOptionalRex32(reg, address);
  EmitUint8(0x3B);
  EmitOperand(reg.LowBits(), address);
}


void X86_64Assembler::cmpl(const Address& address, CpuRegister reg) {
  AssemblerBuffer::EnsureCapacity ensured(&buffer_);
  EmitOptionalRex32(reg, address);
  EmitUint8(0x39);
  EmitOperand(reg.LowBits(), address);
}


void X86_64Assembler::cmpl(const Address& address, const Immediate& imm) {
  AssemblerBuffer::EnsureCapacity ensured(&buffer_);
  EmitOptionalRex32(address);
  EmitComplex(7, address, imm);
}


void X86_64Assembler::cmpq(CpuRegister reg0, CpuRegister reg1) {
  AssemblerBuffer::EnsureCapacity ensured(&buffer_);
  EmitRex64(reg0, reg1);
  EmitUint8(0x3B);
  EmitOperand(reg0.LowBits(), Operand(reg1));
}


void X86_64Assembler::cmpq(CpuRegister reg, const Immediate& imm) {
  AssemblerBuffer::EnsureCapacity ensured(&buffer_);
  CHECK(imm.is_int32());  // cmpq only supports 32b immediate.
  EmitRex64(reg);
  EmitComplex(7, Operand(reg), imm);
}


void X86_64Assembler::cmpq(CpuRegister reg, const Address& address) {
  AssemblerBuffer::EnsureCapacity ensured(&buffer_);
  EmitRex64(reg, address);
  EmitUint8(0x3B);
  EmitOperand(reg.LowBits(), address);
}


void X86_64Assembler::cmpq(const Address& address, const Immediate& imm) {
  CHECK(imm.is_int32());  // cmpq only supports 32b immediate.
  AssemblerBuffer::EnsureCapacity ensured(&buffer_);
  EmitRex64(address);
  EmitComplex(7, address, imm);
}


void X86_64Assembler::addl(CpuRegister dst, CpuRegister src) {
  AssemblerBuffer::EnsureCapacity ensured(&buffer_);
  EmitOptionalRex32(dst, src);
  EmitUint8(0x03);
  EmitRegisterOperand(dst.LowBits(), src.LowBits());
}


void X86_64Assembler::addl(CpuRegister reg, const Address& address) {
  AssemblerBuffer::EnsureCapacity ensured(&buffer_);
  EmitOptionalRex32(reg, address);
  EmitUint8(0x03);
  EmitOperand(reg.LowBits(), address);
}


void X86_64Assembler::testl(CpuRegister reg1, CpuRegister reg2) {
  AssemblerBuffer::EnsureCapacity ensured(&buffer_);
  EmitOptionalRex32(reg1, reg2);
  EmitUint8(0x85);
  EmitRegisterOperand(reg1.LowBits(), reg2.LowBits());
}


void X86_64Assembler::testl(CpuRegister reg, const Address& address) {
  AssemblerBuffer::EnsureCapacity ensured(&buffer_);
  EmitOptionalRex32(reg, address);
  EmitUint8(0x85);
  EmitOperand(reg.LowBits(), address);
}


void X86_64Assembler::testl(CpuRegister reg, const Immediate& immediate) {
  AssemblerBuffer::EnsureCapacity ensured(&buffer_);
  // For registers that have a byte variant (RAX, RBX, RCX, and RDX)
  // we only test the byte CpuRegister to keep the encoding short.
  if (immediate.is_uint8() && reg.AsRegister() < 4) {
    // Use zero-extended 8-bit immediate.
    if (reg.AsRegister() == RAX) {
      EmitUint8(0xA8);
    } else {
      EmitUint8(0xF6);
      EmitUint8(0xC0 + reg.AsRegister());
    }
    EmitUint8(immediate.value() & 0xFF);
  } else if (reg.AsRegister() == RAX) {
    // Use short form if the destination is RAX.
    EmitUint8(0xA9);
    EmitImmediate(immediate);
  } else {
    EmitOptionalRex32(reg);
    EmitUint8(0xF7);
    EmitOperand(0, Operand(reg));
    EmitImmediate(immediate);
  }
}


void X86_64Assembler::testq(CpuRegister reg1, CpuRegister reg2) {
  AssemblerBuffer::EnsureCapacity ensured(&buffer_);
  EmitRex64(reg1, reg2);
  EmitUint8(0x85);
  EmitRegisterOperand(reg1.LowBits(), reg2.LowBits());
}


void X86_64Assembler::testq(CpuRegister reg, const Address& address) {
  AssemblerBuffer::EnsureCapacity ensured(&buffer_);
  EmitRex64(reg, address);
  EmitUint8(0x85);
  EmitOperand(reg.LowBits(), address);
}


void X86_64Assembler::andl(CpuRegister dst, CpuRegister src) {
  AssemblerBuffer::EnsureCapacity ensured(&buffer_);
  EmitOptionalRex32(dst, src);
  EmitUint8(0x23);
  EmitOperand(dst.LowBits(), Operand(src));
}


void X86_64Assembler::andl(CpuRegister reg, const Address& address) {
  AssemblerBuffer::EnsureCapacity ensured(&buffer_);
  EmitOptionalRex32(reg, address);
  EmitUint8(0x23);
  EmitOperand(reg.LowBits(), address);
}


void X86_64Assembler::andl(CpuRegister dst, const Immediate& imm) {
  AssemblerBuffer::EnsureCapacity ensured(&buffer_);
  EmitOptionalRex32(dst);
  EmitComplex(4, Operand(dst), imm);
}


void X86_64Assembler::andq(CpuRegister reg, const Immediate& imm) {
  AssemblerBuffer::EnsureCapacity ensured(&buffer_);
  CHECK(imm.is_int32());  // andq only supports 32b immediate.
  EmitRex64(reg);
  EmitComplex(4, Operand(reg), imm);
}


void X86_64Assembler::andq(CpuRegister dst, CpuRegister src) {
  AssemblerBuffer::EnsureCapacity ensured(&buffer_);
  EmitRex64(dst, src);
  EmitUint8(0x23);
  EmitOperand(dst.LowBits(), Operand(src));
}


void X86_64Assembler::andq(CpuRegister dst, const Address& src) {
  AssemblerBuffer::EnsureCapacity ensured(&buffer_);
  EmitRex64(dst, src);
  EmitUint8(0x23);
  EmitOperand(dst.LowBits(), src);
}


void X86_64Assembler::orl(CpuRegister dst, CpuRegister src) {
  AssemblerBuffer::EnsureCapacity ensured(&buffer_);
  EmitOptionalRex32(dst, src);
  EmitUint8(0x0B);
  EmitOperand(dst.LowBits(), Operand(src));
}


void X86_64Assembler::orl(CpuRegister reg, const Address& address) {
  AssemblerBuffer::EnsureCapacity ensured(&buffer_);
  EmitOptionalRex32(reg, address);
  EmitUint8(0x0B);
  EmitOperand(reg.LowBits(), address);
}


void X86_64Assembler::orl(CpuRegister dst, const Immediate& imm) {
  AssemblerBuffer::EnsureCapacity ensured(&buffer_);
  EmitOptionalRex32(dst);
  EmitComplex(1, Operand(dst), imm);
}


void X86_64Assembler::orq(CpuRegister dst, const Immediate& imm) {
  AssemblerBuffer::EnsureCapacity ensured(&buffer_);
  CHECK(imm.is_int32());  // orq only supports 32b immediate.
  EmitRex64(dst);
  EmitComplex(1, Operand(dst), imm);
}


void X86_64Assembler::orq(CpuRegister dst, CpuRegister src) {
  AssemblerBuffer::EnsureCapacity ensured(&buffer_);
  EmitRex64(dst, src);
  EmitUint8(0x0B);
  EmitOperand(dst.LowBits(), Operand(src));
}


void X86_64Assembler::orq(CpuRegister dst, const Address& src) {
  AssemblerBuffer::EnsureCapacity ensured(&buffer_);
  EmitRex64(dst, src);
  EmitUint8(0x0B);
  EmitOperand(dst.LowBits(), src);
}


void X86_64Assembler::xorl(CpuRegister dst, CpuRegister src) {
  AssemblerBuffer::EnsureCapacity ensured(&buffer_);
  EmitOptionalRex32(dst, src);
  EmitUint8(0x33);
  EmitOperand(dst.LowBits(), Operand(src));
}


void X86_64Assembler::xorl(CpuRegister reg, const Address& address) {
  AssemblerBuffer::EnsureCapacity ensured(&buffer_);
  EmitOptionalRex32(reg, address);
  EmitUint8(0x33);
  EmitOperand(reg.LowBits(), address);
}


void X86_64Assembler::xorl(CpuRegister dst, const Immediate& imm) {
  AssemblerBuffer::EnsureCapacity ensured(&buffer_);
  EmitOptionalRex32(dst);
  EmitComplex(6, Operand(dst), imm);
}


void X86_64Assembler::xorq(CpuRegister dst, CpuRegister src) {
  AssemblerBuffer::EnsureCapacity ensured(&buffer_);
  EmitRex64(dst, src);
  EmitUint8(0x33);
  EmitOperand(dst.LowBits(), Operand(src));
}


void X86_64Assembler::xorq(CpuRegister dst, const Immediate& imm) {
  AssemblerBuffer::EnsureCapacity ensured(&buffer_);
  CHECK(imm.is_int32());  // xorq only supports 32b immediate.
  EmitRex64(dst);
  EmitComplex(6, Operand(dst), imm);
}

void X86_64Assembler::xorq(CpuRegister dst, const Address& src) {
  AssemblerBuffer::EnsureCapacity ensured(&buffer_);
  EmitRex64(dst, src);
  EmitUint8(0x33);
  EmitOperand(dst.LowBits(), src);
}


#if 0
void X86_64Assembler::rex(bool force, bool w, Register* r, Register* x, Register* b) {
  // REX.WRXB
  // W - 64-bit operand
  // R - MODRM.reg
  // X - SIB.index
  // B - MODRM.rm/SIB.base
  uint8_t rex = force ? 0x40 : 0;
  if (w) {
    rex |= 0x48;  // REX.W000
  }
  if (r != nullptr && *r >= Register::R8 && *r < Register::kNumberOfCpuRegisters) {
    rex |= 0x44;  // REX.0R00
    *r = static_cast<Register>(*r - 8);
  }
  if (x != nullptr && *x >= Register::R8 && *x < Register::kNumberOfCpuRegisters) {
    rex |= 0x42;  // REX.00X0
    *x = static_cast<Register>(*x - 8);
  }
  if (b != nullptr && *b >= Register::R8 && *b < Register::kNumberOfCpuRegisters) {
    rex |= 0x41;  // REX.000B
    *b = static_cast<Register>(*b - 8);
  }
  if (rex != 0) {
    EmitUint8(rex);
  }
}

void X86_64Assembler::rex_reg_mem(bool force, bool w, Register* dst, const Address& mem) {
  // REX.WRXB
  // W - 64-bit operand
  // R - MODRM.reg
  // X - SIB.index
  // B - MODRM.rm/SIB.base
  uint8_t rex = mem->rex();
  if (force) {
    rex |= 0x40;  // REX.0000
  }
  if (w) {
    rex |= 0x48;  // REX.W000
  }
  if (dst != nullptr && *dst >= Register::R8 && *dst < Register::kNumberOfCpuRegisters) {
    rex |= 0x44;  // REX.0R00
    *dst = static_cast<Register>(*dst - 8);
  }
  if (rex != 0) {
    EmitUint8(rex);
  }
}

void rex_mem_reg(bool force, bool w, Address* mem, Register* src);
#endif

void X86_64Assembler::addl(CpuRegister reg, const Immediate& imm) {
  AssemblerBuffer::EnsureCapacity ensured(&buffer_);
  EmitOptionalRex32(reg);
  EmitComplex(0, Operand(reg), imm);
}


void X86_64Assembler::addq(CpuRegister reg, const Immediate& imm) {
  AssemblerBuffer::EnsureCapacity ensured(&buffer_);
  CHECK(imm.is_int32());  // addq only supports 32b immediate.
  EmitRex64(reg);
  EmitComplex(0, Operand(reg), imm);
}


void X86_64Assembler::addq(CpuRegister dst, const Address& address) {
  AssemblerBuffer::EnsureCapacity ensured(&buffer_);
  EmitRex64(dst, address);
  EmitUint8(0x03);
  EmitOperand(dst.LowBits(), address);
}


void X86_64Assembler::addq(CpuRegister dst, CpuRegister src) {
  AssemblerBuffer::EnsureCapacity ensured(&buffer_);
  // 0x01 is addq r/m64 <- r/m64 + r64, with op1 in r/m and op2 in reg: so reverse EmitRex64
  EmitRex64(src, dst);
  EmitUint8(0x01);
  EmitRegisterOperand(src.LowBits(), dst.LowBits());
}


void X86_64Assembler::addl(const Address& address, CpuRegister reg) {
  AssemblerBuffer::EnsureCapacity ensured(&buffer_);
  EmitOptionalRex32(reg, address);
  EmitUint8(0x01);
  EmitOperand(reg.LowBits(), address);
}


void X86_64Assembler::addl(const Address& address, const Immediate& imm) {
  AssemblerBuffer::EnsureCapacity ensured(&buffer_);
  EmitOptionalRex32(address);
  EmitComplex(0, address, imm);
}


void X86_64Assembler::subl(CpuRegister dst, CpuRegister src) {
  AssemblerBuffer::EnsureCapacity ensured(&buffer_);
  EmitOptionalRex32(dst, src);
  EmitUint8(0x2B);
  EmitOperand(dst.LowBits(), Operand(src));
}


void X86_64Assembler::subl(CpuRegister reg, const Immediate& imm) {
  AssemblerBuffer::EnsureCapacity ensured(&buffer_);
  EmitOptionalRex32(reg);
  EmitComplex(5, Operand(reg), imm);
}


void X86_64Assembler::subq(CpuRegister reg, const Immediate& imm) {
  AssemblerBuffer::EnsureCapacity ensured(&buffer_);
  CHECK(imm.is_int32());  // subq only supports 32b immediate.
  EmitRex64(reg);
  EmitComplex(5, Operand(reg), imm);
}


void X86_64Assembler::subq(CpuRegister dst, CpuRegister src) {
  AssemblerBuffer::EnsureCapacity ensured(&buffer_);
  EmitRex64(dst, src);
  EmitUint8(0x2B);
  EmitRegisterOperand(dst.LowBits(), src.LowBits());
}


void X86_64Assembler::subq(CpuRegister reg, const Address& address) {
  AssemblerBuffer::EnsureCapacity ensured(&buffer_);
  EmitRex64(reg, address);
  EmitUint8(0x2B);
  EmitOperand(reg.LowBits() & 7, address);
}


void X86_64Assembler::subl(CpuRegister reg, const Address& address) {
  AssemblerBuffer::EnsureCapacity ensured(&buffer_);
  EmitOptionalRex32(reg, address);
  EmitUint8(0x2B);
  EmitOperand(reg.LowBits(), address);
}


void X86_64Assembler::cdq() {
  AssemblerBuffer::EnsureCapacity ensured(&buffer_);
  EmitUint8(0x99);
}


void X86_64Assembler::cqo() {
  AssemblerBuffer::EnsureCapacity ensured(&buffer_);
  EmitRex64();
  EmitUint8(0x99);
}


void X86_64Assembler::idivl(CpuRegister reg) {
  AssemblerBuffer::EnsureCapacity ensured(&buffer_);
  EmitOptionalRex32(reg);
  EmitUint8(0xF7);
  EmitUint8(0xF8 | reg.LowBits());
}


void X86_64Assembler::idivq(CpuRegister reg) {
  AssemblerBuffer::EnsureCapacity ensured(&buffer_);
  EmitRex64(reg);
  EmitUint8(0xF7);
  EmitUint8(0xF8 | reg.LowBits());
}


void X86_64Assembler::imull(CpuRegister dst, CpuRegister src) {
  AssemblerBuffer::EnsureCapacity ensured(&buffer_);
  EmitOptionalRex32(dst, src);
  EmitUint8(0x0F);
  EmitUint8(0xAF);
  EmitOperand(dst.LowBits(), Operand(src));
}

void X86_64Assembler::imull(CpuRegister reg, const Immediate& imm) {
  AssemblerBuffer::EnsureCapacity ensured(&buffer_);
  CHECK(imm.is_int32());  // imull only supports 32b immediate.

  EmitOptionalRex32(reg, reg);

  // See whether imm can be represented as a sign-extended 8bit value.
  int32_t v32 = static_cast<int32_t>(imm.value());
  if (IsInt<8>(v32)) {
    // Sign-extension works.
    EmitUint8(0x6B);
    EmitOperand(reg.LowBits(), Operand(reg));
    EmitUint8(static_cast<uint8_t>(v32 & 0xFF));
  } else {
    // Not representable, use full immediate.
    EmitUint8(0x69);
    EmitOperand(reg.LowBits(), Operand(reg));
    EmitImmediate(imm);
  }
}


void X86_64Assembler::imull(CpuRegister reg, const Address& address) {
  AssemblerBuffer::EnsureCapacity ensured(&buffer_);
  EmitOptionalRex32(reg, address);
  EmitUint8(0x0F);
  EmitUint8(0xAF);
  EmitOperand(reg.LowBits(), address);
}


void X86_64Assembler::imulq(CpuRegister dst, CpuRegister src) {
  AssemblerBuffer::EnsureCapacity ensured(&buffer_);
  EmitRex64(dst, src);
  EmitUint8(0x0F);
  EmitUint8(0xAF);
  EmitRegisterOperand(dst.LowBits(), src.LowBits());
}


void X86_64Assembler::imulq(CpuRegister reg, const Immediate& imm) {
  imulq(reg, reg, imm);
}

void X86_64Assembler::imulq(CpuRegister dst, CpuRegister reg, const Immediate& imm) {
  AssemblerBuffer::EnsureCapacity ensured(&buffer_);
  CHECK(imm.is_int32());  // imulq only supports 32b immediate.

  EmitRex64(dst, reg);

  // See whether imm can be represented as a sign-extended 8bit value.
  int64_t v64 = imm.value();
  if (IsInt<8>(v64)) {
    // Sign-extension works.
    EmitUint8(0x6B);
    EmitOperand(dst.LowBits(), Operand(reg));
    EmitUint8(static_cast<uint8_t>(v64 & 0xFF));
  } else {
    // Not representable, use full immediate.
    EmitUint8(0x69);
    EmitOperand(dst.LowBits(), Operand(reg));
    EmitImmediate(imm);
  }
}

void X86_64Assembler::imulq(CpuRegister reg, const Address& address) {
  AssemblerBuffer::EnsureCapacity ensured(&buffer_);
  EmitRex64(reg, address);
  EmitUint8(0x0F);
  EmitUint8(0xAF);
  EmitOperand(reg.LowBits(), address);
}


void X86_64Assembler::imull(CpuRegister reg) {
  AssemblerBuffer::EnsureCapacity ensured(&buffer_);
  EmitOptionalRex32(reg);
  EmitUint8(0xF7);
  EmitOperand(5, Operand(reg));
}


void X86_64Assembler::imulq(CpuRegister reg) {
  AssemblerBuffer::EnsureCapacity ensured(&buffer_);
  EmitRex64(reg);
  EmitUint8(0xF7);
  EmitOperand(5, Operand(reg));
}


void X86_64Assembler::imull(const Address& address) {
  AssemblerBuffer::EnsureCapacity ensured(&buffer_);
  EmitOptionalRex32(address);
  EmitUint8(0xF7);
  EmitOperand(5, address);
}


void X86_64Assembler::mull(CpuRegister reg) {
  AssemblerBuffer::EnsureCapacity ensured(&buffer_);
  EmitOptionalRex32(reg);
  EmitUint8(0xF7);
  EmitOperand(4, Operand(reg));
}


void X86_64Assembler::mull(const Address& address) {
  AssemblerBuffer::EnsureCapacity ensured(&buffer_);
  EmitOptionalRex32(address);
  EmitUint8(0xF7);
  EmitOperand(4, address);
}


void X86_64Assembler::shll(CpuRegister reg, const Immediate& imm) {
  EmitGenericShift(false, 4, reg, imm);
}


void X86_64Assembler::shlq(CpuRegister reg, const Immediate& imm) {
  EmitGenericShift(true, 4, reg, imm);
}


void X86_64Assembler::shll(CpuRegister operand, CpuRegister shifter) {
  EmitGenericShift(false, 4, operand, shifter);
}


void X86_64Assembler::shlq(CpuRegister operand, CpuRegister shifter) {
  EmitGenericShift(true, 4, operand, shifter);
}


void X86_64Assembler::shrl(CpuRegister reg, const Immediate& imm) {
  EmitGenericShift(false, 5, reg, imm);
}


void X86_64Assembler::shrq(CpuRegister reg, const Immediate& imm) {
  EmitGenericShift(true, 5, reg, imm);
}


void X86_64Assembler::shrl(CpuRegister operand, CpuRegister shifter) {
  EmitGenericShift(false, 5, operand, shifter);
}


void X86_64Assembler::shrq(CpuRegister operand, CpuRegister shifter) {
  EmitGenericShift(true, 5, operand, shifter);
}


void X86_64Assembler::sarl(CpuRegister reg, const Immediate& imm) {
  EmitGenericShift(false, 7, reg, imm);
}


void X86_64Assembler::sarl(CpuRegister operand, CpuRegister shifter) {
  EmitGenericShift(false, 7, operand, shifter);
}


void X86_64Assembler::sarq(CpuRegister reg, const Immediate& imm) {
  EmitGenericShift(true, 7, reg, imm);
}


void X86_64Assembler::sarq(CpuRegister operand, CpuRegister shifter) {
  EmitGenericShift(true, 7, operand, shifter);
}


void X86_64Assembler::negl(CpuRegister reg) {
  AssemblerBuffer::EnsureCapacity ensured(&buffer_);
  EmitOptionalRex32(reg);
  EmitUint8(0xF7);
  EmitOperand(3, Operand(reg));
}


void X86_64Assembler::negq(CpuRegister reg) {
  AssemblerBuffer::EnsureCapacity ensured(&buffer_);
  EmitRex64(reg);
  EmitUint8(0xF7);
  EmitOperand(3, Operand(reg));
}


void X86_64Assembler::notl(CpuRegister reg) {
  AssemblerBuffer::EnsureCapacity ensured(&buffer_);
  EmitOptionalRex32(reg);
  EmitUint8(0xF7);
  EmitUint8(0xD0 | reg.LowBits());
}


void X86_64Assembler::notq(CpuRegister reg) {
  AssemblerBuffer::EnsureCapacity ensured(&buffer_);
  EmitRex64(reg);
  EmitUint8(0xF7);
  EmitOperand(2, Operand(reg));
}


void X86_64Assembler::enter(const Immediate& imm) {
  AssemblerBuffer::EnsureCapacity ensured(&buffer_);
  EmitUint8(0xC8);
  CHECK(imm.is_uint16()) << imm.value();
  EmitUint8(imm.value() & 0xFF);
  EmitUint8((imm.value() >> 8) & 0xFF);
  EmitUint8(0x00);
}


void X86_64Assembler::leave() {
  AssemblerBuffer::EnsureCapacity ensured(&buffer_);
  EmitUint8(0xC9);
}


void X86_64Assembler::ret() {
  AssemblerBuffer::EnsureCapacity ensured(&buffer_);
  EmitUint8(0xC3);
}


void X86_64Assembler::ret(const Immediate& imm) {
  AssemblerBuffer::EnsureCapacity ensured(&buffer_);
  EmitUint8(0xC2);
  CHECK(imm.is_uint16());
  EmitUint8(imm.value() & 0xFF);
  EmitUint8((imm.value() >> 8) & 0xFF);
}



void X86_64Assembler::nop() {
  AssemblerBuffer::EnsureCapacity ensured(&buffer_);
  EmitUint8(0x90);
}


void X86_64Assembler::int3() {
  AssemblerBuffer::EnsureCapacity ensured(&buffer_);
  EmitUint8(0xCC);
}


void X86_64Assembler::hlt() {
  AssemblerBuffer::EnsureCapacity ensured(&buffer_);
  EmitUint8(0xF4);
}


void X86_64Assembler::j(Condition condition, Label* label) {
  AssemblerBuffer::EnsureCapacity ensured(&buffer_);
  if (label->IsBound()) {
    static const int kShortSize = 2;
    static const int kLongSize = 6;
    int offset = label->Position() - buffer_.Size();
    CHECK_LE(offset, 0);
    if (IsInt<8>(offset - kShortSize)) {
      EmitUint8(0x70 + condition);
      EmitUint8((offset - kShortSize) & 0xFF);
    } else {
      EmitUint8(0x0F);
      EmitUint8(0x80 + condition);
      EmitInt32(offset - kLongSize);
    }
  } else {
    EmitUint8(0x0F);
    EmitUint8(0x80 + condition);
    EmitLabelLink(label);
  }
}


void X86_64Assembler::jmp(CpuRegister reg) {
  AssemblerBuffer::EnsureCapacity ensured(&buffer_);
  EmitOptionalRex32(reg);
  EmitUint8(0xFF);
  EmitRegisterOperand(4, reg.LowBits());
}

void X86_64Assembler::jmp(const Address& address) {
  AssemblerBuffer::EnsureCapacity ensured(&buffer_);
  EmitOptionalRex32(address);
  EmitUint8(0xFF);
  EmitOperand(4, address);
}

void X86_64Assembler::jmp(Label* label) {
  AssemblerBuffer::EnsureCapacity ensured(&buffer_);
  if (label->IsBound()) {
    static const int kShortSize = 2;
    static const int kLongSize = 5;
    int offset = label->Position() - buffer_.Size();
    CHECK_LE(offset, 0);
    if (IsInt<8>(offset - kShortSize)) {
      EmitUint8(0xEB);
      EmitUint8((offset - kShortSize) & 0xFF);
    } else {
      EmitUint8(0xE9);
      EmitInt32(offset - kLongSize);
    }
  } else {
    EmitUint8(0xE9);
    EmitLabelLink(label);
  }
}


X86_64Assembler* X86_64Assembler::lock() {
  AssemblerBuffer::EnsureCapacity ensured(&buffer_);
  EmitUint8(0xF0);
  return this;
}


void X86_64Assembler::cmpxchgl(const Address& address, CpuRegister reg) {
  AssemblerBuffer::EnsureCapacity ensured(&buffer_);
  EmitOptionalRex32(reg, address);
  EmitUint8(0x0F);
  EmitUint8(0xB1);
  EmitOperand(reg.LowBits(), address);
}


void X86_64Assembler::cmpxchgq(const Address& address, CpuRegister reg) {
  AssemblerBuffer::EnsureCapacity ensured(&buffer_);
  EmitRex64(reg, address);
  EmitUint8(0x0F);
  EmitUint8(0xB1);
  EmitOperand(reg.LowBits(), address);
}


void X86_64Assembler::mfence() {
  AssemblerBuffer::EnsureCapacity ensured(&buffer_);
  EmitUint8(0x0F);
  EmitUint8(0xAE);
  EmitUint8(0xF0);
}


X86_64Assembler* X86_64Assembler::gs() {
  // TODO: gs is a prefix and not an instruction
  AssemblerBuffer::EnsureCapacity ensured(&buffer_);
  EmitUint8(0x65);
  return this;
}


void X86_64Assembler::AddImmediate(CpuRegister reg, const Immediate& imm) {
  int value = imm.value();
  if (value != 0) {
    if (value > 0) {
      addl(reg, imm);
    } else {
      subl(reg, Immediate(value));
    }
  }
}


void X86_64Assembler::setcc(Condition condition, CpuRegister dst) {
  AssemblerBuffer::EnsureCapacity ensured(&buffer_);
  // RSP, RBP, RDI, RSI need rex prefix (else the pattern encodes ah/bh/ch/dh).
  if (dst.NeedsRex() || dst.AsRegister() > 3) {
    EmitOptionalRex(true, false, false, false, dst.NeedsRex());
  }
  EmitUint8(0x0F);
  EmitUint8(0x90 + condition);
  EmitUint8(0xC0 + dst.LowBits());
}

void X86_64Assembler::bswapl(CpuRegister dst) {
  AssemblerBuffer::EnsureCapacity ensured(&buffer_);
  EmitOptionalRex(false, false, false, false, dst.NeedsRex());
  EmitUint8(0x0F);
  EmitUint8(0xC8 + dst.LowBits());
}

void X86_64Assembler::bswapq(CpuRegister dst) {
  AssemblerBuffer::EnsureCapacity ensured(&buffer_);
  EmitOptionalRex(false, true, false, false, dst.NeedsRex());
  EmitUint8(0x0F);
  EmitUint8(0xC8 + dst.LowBits());
}


void X86_64Assembler::repne_scasw() {
  AssemblerBuffer::EnsureCapacity ensured(&buffer_);
  EmitUint8(0x66);
  EmitUint8(0xF2);
  EmitUint8(0xAF);
}


void X86_64Assembler::LoadDoubleConstant(XmmRegister dst, double value) {
  // TODO: Need to have a code constants table.
  int64_t constant = bit_cast<int64_t, double>(value);
  pushq(Immediate(High32Bits(constant)));
  pushq(Immediate(Low32Bits(constant)));
  movsd(dst, Address(CpuRegister(RSP), 0));
  addq(CpuRegister(RSP), Immediate(2 * sizeof(intptr_t)));
}


void X86_64Assembler::Align(int alignment, int offset) {
  CHECK(IsPowerOfTwo(alignment));
  // Emit nop instruction until the real position is aligned.
  while (((offset + buffer_.GetPosition()) & (alignment-1)) != 0) {
    nop();
  }
}


void X86_64Assembler::Bind(Label* label) {
  int bound = buffer_.Size();
  CHECK(!label->IsBound());  // Labels can only be bound once.
  while (label->IsLinked()) {
    int position = label->LinkPosition();
    int next = buffer_.Load<int32_t>(position);
    buffer_.Store<int32_t>(position, bound - (position + 4));
    label->position_ = next;
  }
  label->BindTo(bound);
}


void X86_64Assembler::EmitOperand(uint8_t reg_or_opcode, const Operand& operand) {
  CHECK_GE(reg_or_opcode, 0);
  CHECK_LT(reg_or_opcode, 8);
  const int length = operand.length_;
  CHECK_GT(length, 0);
  // Emit the ModRM byte updated with the given reg value.
  CHECK_EQ(operand.encoding_[0] & 0x38, 0);
  EmitUint8(operand.encoding_[0] + (reg_or_opcode << 3));
  // Emit the rest of the encoded operand.
  for (int i = 1; i < length; i++) {
    EmitUint8(operand.encoding_[i]);
  }
  AssemblerFixup* fixup = operand.GetFixup();
  if (fixup != nullptr) {
    EmitFixup(fixup);
  }
}


void X86_64Assembler::EmitImmediate(const Immediate& imm) {
  if (imm.is_int32()) {
    EmitInt32(static_cast<int32_t>(imm.value()));
  } else {
    EmitInt64(imm.value());
  }
}


void X86_64Assembler::EmitComplex(uint8_t reg_or_opcode,
                                  const Operand& operand,
                                  const Immediate& immediate) {
  CHECK_GE(reg_or_opcode, 0);
  CHECK_LT(reg_or_opcode, 8);
  if (immediate.is_int8()) {
    // Use sign-extended 8-bit immediate.
    EmitUint8(0x83);
    EmitOperand(reg_or_opcode, operand);
    EmitUint8(immediate.value() & 0xFF);
  } else if (operand.IsRegister(CpuRegister(RAX))) {
    // Use short form if the destination is eax.
    EmitUint8(0x05 + (reg_or_opcode << 3));
    EmitImmediate(immediate);
  } else {
    EmitUint8(0x81);
    EmitOperand(reg_or_opcode, operand);
    EmitImmediate(immediate);
  }
}


void X86_64Assembler::EmitLabel(Label* label, int instruction_size) {
  if (label->IsBound()) {
    int offset = label->Position() - buffer_.Size();
    CHECK_LE(offset, 0);
    EmitInt32(offset - instruction_size);
  } else {
    EmitLabelLink(label);
  }
}


void X86_64Assembler::EmitLabelLink(Label* label) {
  CHECK(!label->IsBound());
  int position = buffer_.Size();
  EmitInt32(label->position_);
  label->LinkTo(position);
}


void X86_64Assembler::EmitGenericShift(bool wide,
                                       int reg_or_opcode,
                                       CpuRegister reg,
                                       const Immediate& imm) {
  AssemblerBuffer::EnsureCapacity ensured(&buffer_);
  CHECK(imm.is_int8());
  if (wide) {
    EmitRex64(reg);
  } else {
    EmitOptionalRex32(reg);
  }
  if (imm.value() == 1) {
    EmitUint8(0xD1);
    EmitOperand(reg_or_opcode, Operand(reg));
  } else {
    EmitUint8(0xC1);
    EmitOperand(reg_or_opcode, Operand(reg));
    EmitUint8(imm.value() & 0xFF);
  }
}


void X86_64Assembler::EmitGenericShift(bool wide,
                                       int reg_or_opcode,
                                       CpuRegister operand,
                                       CpuRegister shifter) {
  AssemblerBuffer::EnsureCapacity ensured(&buffer_);
  CHECK_EQ(shifter.AsRegister(), RCX);
  if (wide) {
    EmitRex64(operand);
  } else {
    EmitOptionalRex32(operand);
  }
  EmitUint8(0xD3);
  EmitOperand(reg_or_opcode, Operand(operand));
}

void X86_64Assembler::EmitOptionalRex(bool force, bool w, bool r, bool x, bool b) {
  // REX.WRXB
  // W - 64-bit operand
  // R - MODRM.reg
  // X - SIB.index
  // B - MODRM.rm/SIB.base
  uint8_t rex = force ? 0x40 : 0;
  if (w) {
    rex |= 0x48;  // REX.W000
  }
  if (r) {
    rex |= 0x44;  // REX.0R00
  }
  if (x) {
    rex |= 0x42;  // REX.00X0
  }
  if (b) {
    rex |= 0x41;  // REX.000B
  }
  if (rex != 0) {
    EmitUint8(rex);
  }
}

void X86_64Assembler::EmitOptionalRex32(CpuRegister reg) {
  EmitOptionalRex(false, false, false, false, reg.NeedsRex());
}

void X86_64Assembler::EmitOptionalRex32(CpuRegister dst, CpuRegister src) {
  EmitOptionalRex(false, false, dst.NeedsRex(), false, src.NeedsRex());
}

void X86_64Assembler::EmitOptionalRex32(XmmRegister dst, XmmRegister src) {
  EmitOptionalRex(false, false, dst.NeedsRex(), false, src.NeedsRex());
}

void X86_64Assembler::EmitOptionalRex32(CpuRegister dst, XmmRegister src) {
  EmitOptionalRex(false, false, dst.NeedsRex(), false, src.NeedsRex());
}

void X86_64Assembler::EmitOptionalRex32(XmmRegister dst, CpuRegister src) {
  EmitOptionalRex(false, false, dst.NeedsRex(), false, src.NeedsRex());
}

void X86_64Assembler::EmitOptionalRex32(const Operand& operand) {
  uint8_t rex = operand.rex();
  if (rex != 0) {
    EmitUint8(rex);
  }
}

void X86_64Assembler::EmitOptionalRex32(CpuRegister dst, const Operand& operand) {
  uint8_t rex = operand.rex();
  if (dst.NeedsRex()) {
    rex |= 0x44;  // REX.0R00
  }
  if (rex != 0) {
    EmitUint8(rex);
  }
}

void X86_64Assembler::EmitOptionalRex32(XmmRegister dst, const Operand& operand) {
  uint8_t rex = operand.rex();
  if (dst.NeedsRex()) {
    rex |= 0x44;  // REX.0R00
  }
  if (rex != 0) {
    EmitUint8(rex);
  }
}

void X86_64Assembler::EmitRex64() {
  EmitOptionalRex(false, true, false, false, false);
}

void X86_64Assembler::EmitRex64(CpuRegister reg) {
  EmitOptionalRex(false, true, false, false, reg.NeedsRex());
}

void X86_64Assembler::EmitRex64(const Operand& operand) {
  uint8_t rex = operand.rex();
  rex |= 0x48;  // REX.W000
  EmitUint8(rex);
}

void X86_64Assembler::EmitRex64(CpuRegister dst, CpuRegister src) {
  EmitOptionalRex(false, true, dst.NeedsRex(), false, src.NeedsRex());
}

void X86_64Assembler::EmitRex64(XmmRegister dst, CpuRegister src) {
  EmitOptionalRex(false, true, dst.NeedsRex(), false, src.NeedsRex());
}

void X86_64Assembler::EmitRex64(CpuRegister dst, XmmRegister src) {
  EmitOptionalRex(false, true, dst.NeedsRex(), false, src.NeedsRex());
}

void X86_64Assembler::EmitRex64(CpuRegister dst, const Operand& operand) {
  uint8_t rex = 0x48 | operand.rex();  // REX.W000
  if (dst.NeedsRex()) {
    rex |= 0x44;  // REX.0R00
  }
  EmitUint8(rex);
}

void X86_64Assembler::EmitRex64(XmmRegister dst, const Operand& operand) {
  uint8_t rex = 0x48 | operand.rex();  // REX.W000
  if (dst.NeedsRex()) {
    rex |= 0x44;  // REX.0R00
  }
  EmitUint8(rex);
}

void X86_64Assembler::EmitOptionalByteRegNormalizingRex32(CpuRegister dst, CpuRegister src) {
  // For src, SPL, BPL, SIL, DIL need the rex prefix.
  bool force = src.AsRegister() > 3;
  EmitOptionalRex(force, false, dst.NeedsRex(), false, src.NeedsRex());
}

void X86_64Assembler::EmitOptionalByteRegNormalizingRex32(CpuRegister dst, const Operand& operand) {
  uint8_t rex = operand.rex();
  // For dst, SPL, BPL, SIL, DIL need the rex prefix.
  bool force = dst.AsRegister() > 3;
  if (force) {
    rex |= 0x40;  // REX.0000
  }
  if (dst.NeedsRex()) {
    rex |= 0x44;  // REX.0R00
  }
  if (rex != 0) {
    EmitUint8(rex);
  }
}

static dwarf::Reg DWARFReg(Register reg) {
  return dwarf::Reg::X86_64Core(static_cast<int>(reg));
}
static dwarf::Reg DWARFReg(FloatRegister reg) {
  return dwarf::Reg::X86_64Fp(static_cast<int>(reg));
}

constexpr size_t kFramePointerSize = 8;

void X86_64Assembler::BuildFrame(size_t frame_size, ManagedRegister method_reg,
                                 const std::vector<ManagedRegister>& spill_regs,
                                 const ManagedRegisterEntrySpills& entry_spills) {
  DCHECK_EQ(buffer_.Size(), 0U);  // Nothing emitted yet.
  cfi_.SetCurrentCFAOffset(8);  // Return address on stack.
  CHECK_ALIGNED(frame_size, kStackAlignment);
  int gpr_count = 0;
  for (int i = spill_regs.size() - 1; i >= 0; --i) {
    x86_64::X86_64ManagedRegister spill = spill_regs.at(i).AsX86_64();
    if (spill.IsCpuRegister()) {
      pushq(spill.AsCpuRegister());
      gpr_count++;
      cfi_.AdjustCFAOffset(kFramePointerSize);
      cfi_.RelOffset(DWARFReg(spill.AsCpuRegister().AsRegister()), 0);
    }
  }
  // return address then method on stack.
  int64_t rest_of_frame = static_cast<int64_t>(frame_size)
                          - (gpr_count * kFramePointerSize)
                          - kFramePointerSize /*return address*/;
  subq(CpuRegister(RSP), Immediate(rest_of_frame));
  cfi_.AdjustCFAOffset(rest_of_frame);

  // spill xmms
  int64_t offset = rest_of_frame;
  for (int i = spill_regs.size() - 1; i >= 0; --i) {
    x86_64::X86_64ManagedRegister spill = spill_regs.at(i).AsX86_64();
    if (spill.IsXmmRegister()) {
      offset -= sizeof(double);
      movsd(Address(CpuRegister(RSP), offset), spill.AsXmmRegister());
      cfi_.RelOffset(DWARFReg(spill.AsXmmRegister().AsFloatRegister()), offset);
    }
  }

  DCHECK_EQ(kX86_64PointerSize, kFramePointerSize);

  movq(Address(CpuRegister(RSP), 0), method_reg.AsX86_64().AsCpuRegister());

  for (size_t i = 0; i < entry_spills.size(); ++i) {
    ManagedRegisterSpill spill = entry_spills.at(i);
    if (spill.AsX86_64().IsCpuRegister()) {
      if (spill.getSize() == 8) {
        movq(Address(CpuRegister(RSP), frame_size + spill.getSpillOffset()),
             spill.AsX86_64().AsCpuRegister());
      } else {
        CHECK_EQ(spill.getSize(), 4);
        movl(Address(CpuRegister(RSP), frame_size + spill.getSpillOffset()), spill.AsX86_64().AsCpuRegister());
      }
    } else {
      if (spill.getSize() == 8) {
        movsd(Address(CpuRegister(RSP), frame_size + spill.getSpillOffset()), spill.AsX86_64().AsXmmRegister());
      } else {
        CHECK_EQ(spill.getSize(), 4);
        movss(Address(CpuRegister(RSP), frame_size + spill.getSpillOffset()), spill.AsX86_64().AsXmmRegister());
      }
    }
  }
}

void X86_64Assembler::RemoveFrame(size_t frame_size,
                            const std::vector<ManagedRegister>& spill_regs) {
  CHECK_ALIGNED(frame_size, kStackAlignment);
  cfi_.RememberState();
  int gpr_count = 0;
  // unspill xmms
  int64_t offset = static_cast<int64_t>(frame_size) - (spill_regs.size() * kFramePointerSize) - 2 * kFramePointerSize;
  for (size_t i = 0; i < spill_regs.size(); ++i) {
    x86_64::X86_64ManagedRegister spill = spill_regs.at(i).AsX86_64();
    if (spill.IsXmmRegister()) {
      offset += sizeof(double);
      movsd(spill.AsXmmRegister(), Address(CpuRegister(RSP), offset));
      cfi_.Restore(DWARFReg(spill.AsXmmRegister().AsFloatRegister()));
    } else {
      gpr_count++;
    }
  }
  int adjust = static_cast<int>(frame_size) - (gpr_count * kFramePointerSize) - kFramePointerSize;
  addq(CpuRegister(RSP), Immediate(adjust));
  cfi_.AdjustCFAOffset(-adjust);
  for (size_t i = 0; i < spill_regs.size(); ++i) {
    x86_64::X86_64ManagedRegister spill = spill_regs.at(i).AsX86_64();
    if (spill.IsCpuRegister()) {
      popq(spill.AsCpuRegister());
      cfi_.AdjustCFAOffset(-static_cast<int>(kFramePointerSize));
      cfi_.Restore(DWARFReg(spill.AsCpuRegister().AsRegister()));
    }
  }
  ret();
  // The CFI should be restored for any code that follows the exit block.
  cfi_.RestoreState();
  cfi_.DefCFAOffset(frame_size);
}

void X86_64Assembler::IncreaseFrameSize(size_t adjust) {
  CHECK_ALIGNED(adjust, kStackAlignment);
  addq(CpuRegister(RSP), Immediate(-static_cast<int64_t>(adjust)));
  cfi_.AdjustCFAOffset(adjust);
}

void X86_64Assembler::DecreaseFrameSize(size_t adjust) {
  CHECK_ALIGNED(adjust, kStackAlignment);
  addq(CpuRegister(RSP), Immediate(adjust));
  cfi_.AdjustCFAOffset(-adjust);
}

void X86_64Assembler::Store(FrameOffset offs, ManagedRegister msrc, size_t size) {
  X86_64ManagedRegister src = msrc.AsX86_64();
  if (src.IsNoRegister()) {
    CHECK_EQ(0u, size);
  } else if (src.IsCpuRegister()) {
    if (size == 4) {
      CHECK_EQ(4u, size);
      movl(Address(CpuRegister(RSP), offs), src.AsCpuRegister());
    } else {
      CHECK_EQ(8u, size);
      movq(Address(CpuRegister(RSP), offs), src.AsCpuRegister());
    }
  } else if (src.IsRegisterPair()) {
    CHECK_EQ(0u, size);
    movq(Address(CpuRegister(RSP), offs), src.AsRegisterPairLow());
    movq(Address(CpuRegister(RSP), FrameOffset(offs.Int32Value()+4)),
         src.AsRegisterPairHigh());
  } else if (src.IsX87Register()) {
    if (size == 4) {
      fstps(Address(CpuRegister(RSP), offs));
    } else {
      fstpl(Address(CpuRegister(RSP), offs));
    }
  } else {
    CHECK(src.IsXmmRegister());
    if (size == 4) {
      movss(Address(CpuRegister(RSP), offs), src.AsXmmRegister());
    } else {
      movsd(Address(CpuRegister(RSP), offs), src.AsXmmRegister());
    }
  }
}

void X86_64Assembler::StoreRef(FrameOffset dest, ManagedRegister msrc) {
  X86_64ManagedRegister src = msrc.AsX86_64();
  CHECK(src.IsCpuRegister());
  movl(Address(CpuRegister(RSP), dest), src.AsCpuRegister());
}

void X86_64Assembler::StoreRawPtr(FrameOffset dest, ManagedRegister msrc) {
  X86_64ManagedRegister src = msrc.AsX86_64();
  CHECK(src.IsCpuRegister());
  movq(Address(CpuRegister(RSP), dest), src.AsCpuRegister());
}

void X86_64Assembler::StoreImmediateToFrame(FrameOffset dest, uint32_t imm,
                                            ManagedRegister) {
  movl(Address(CpuRegister(RSP), dest), Immediate(imm));  // TODO(64) movq?
}

void X86_64Assembler::StoreImmediateToThread64(ThreadOffset<8> dest, uint32_t imm,
                                               ManagedRegister) {
  gs()->movl(Address::Absolute(dest, true), Immediate(imm));  // TODO(64) movq?
}

void X86_64Assembler::StoreStackOffsetToThread64(ThreadOffset<8> thr_offs,
                                                 FrameOffset fr_offs,
                                                 ManagedRegister mscratch) {
  X86_64ManagedRegister scratch = mscratch.AsX86_64();
  CHECK(scratch.IsCpuRegister());
  leaq(scratch.AsCpuRegister(), Address(CpuRegister(RSP), fr_offs));
  gs()->movq(Address::Absolute(thr_offs, true), scratch.AsCpuRegister());
}

void X86_64Assembler::StoreStackPointerToThread64(ThreadOffset<8> thr_offs) {
  gs()->movq(Address::Absolute(thr_offs, true), CpuRegister(RSP));
}

void X86_64Assembler::StoreSpanning(FrameOffset /*dst*/, ManagedRegister /*src*/,
                                 FrameOffset /*in_off*/, ManagedRegister /*scratch*/) {
  UNIMPLEMENTED(FATAL);  // this case only currently exists for ARM
}

void X86_64Assembler::Load(ManagedRegister mdest, FrameOffset src, size_t size) {
  X86_64ManagedRegister dest = mdest.AsX86_64();
  if (dest.IsNoRegister()) {
    CHECK_EQ(0u, size);
  } else if (dest.IsCpuRegister()) {
    if (size == 4) {
      CHECK_EQ(4u, size);
      movl(dest.AsCpuRegister(), Address(CpuRegister(RSP), src));
    } else {
      CHECK_EQ(8u, size);
      movq(dest.AsCpuRegister(), Address(CpuRegister(RSP), src));
    }
  } else if (dest.IsRegisterPair()) {
    CHECK_EQ(0u, size);
    movq(dest.AsRegisterPairLow(), Address(CpuRegister(RSP), src));
    movq(dest.AsRegisterPairHigh(), Address(CpuRegister(RSP), FrameOffset(src.Int32Value()+4)));
  } else if (dest.IsX87Register()) {
    if (size == 4) {
      flds(Address(CpuRegister(RSP), src));
    } else {
      fldl(Address(CpuRegister(RSP), src));
    }
  } else {
    CHECK(dest.IsXmmRegister());
    if (size == 4) {
      movss(dest.AsXmmRegister(), Address(CpuRegister(RSP), src));
    } else {
      movsd(dest.AsXmmRegister(), Address(CpuRegister(RSP), src));
    }
  }
}

void X86_64Assembler::LoadFromThread64(ManagedRegister mdest, ThreadOffset<8> src, size_t size) {
  X86_64ManagedRegister dest = mdest.AsX86_64();
  if (dest.IsNoRegister()) {
    CHECK_EQ(0u, size);
  } else if (dest.IsCpuRegister()) {
    CHECK_EQ(4u, size);
    gs()->movl(dest.AsCpuRegister(), Address::Absolute(src, true));
  } else if (dest.IsRegisterPair()) {
    CHECK_EQ(8u, size);
    gs()->movq(dest.AsRegisterPairLow(), Address::Absolute(src, true));
  } else if (dest.IsX87Register()) {
    if (size == 4) {
      gs()->flds(Address::Absolute(src, true));
    } else {
      gs()->fldl(Address::Absolute(src, true));
    }
  } else {
    CHECK(dest.IsXmmRegister());
    if (size == 4) {
      gs()->movss(dest.AsXmmRegister(), Address::Absolute(src, true));
    } else {
      gs()->movsd(dest.AsXmmRegister(), Address::Absolute(src, true));
    }
  }
}

void X86_64Assembler::LoadRef(ManagedRegister mdest, FrameOffset src) {
  X86_64ManagedRegister dest = mdest.AsX86_64();
  CHECK(dest.IsCpuRegister());
  movq(dest.AsCpuRegister(), Address(CpuRegister(RSP), src));
}

void X86_64Assembler::LoadRef(ManagedRegister mdest, ManagedRegister base, MemberOffset offs,
                              bool poison_reference) {
  X86_64ManagedRegister dest = mdest.AsX86_64();
  CHECK(dest.IsCpuRegister() && dest.IsCpuRegister());
  movl(dest.AsCpuRegister(), Address(base.AsX86_64().AsCpuRegister(), offs));
  if (kPoisonHeapReferences && poison_reference) {
    negl(dest.AsCpuRegister());
  }
}

void X86_64Assembler::LoadRawPtr(ManagedRegister mdest, ManagedRegister base,
                              Offset offs) {
  X86_64ManagedRegister dest = mdest.AsX86_64();
  CHECK(dest.IsCpuRegister() && dest.IsCpuRegister());
  movq(dest.AsCpuRegister(), Address(base.AsX86_64().AsCpuRegister(), offs));
}

void X86_64Assembler::LoadRawPtrFromThread64(ManagedRegister mdest, ThreadOffset<8> offs) {
  X86_64ManagedRegister dest = mdest.AsX86_64();
  CHECK(dest.IsCpuRegister());
  gs()->movq(dest.AsCpuRegister(), Address::Absolute(offs, true));
}

void X86_64Assembler::SignExtend(ManagedRegister mreg, size_t size) {
  X86_64ManagedRegister reg = mreg.AsX86_64();
  CHECK(size == 1 || size == 2) << size;
  CHECK(reg.IsCpuRegister()) << reg;
  if (size == 1) {
    movsxb(reg.AsCpuRegister(), reg.AsCpuRegister());
  } else {
    movsxw(reg.AsCpuRegister(), reg.AsCpuRegister());
  }
}

void X86_64Assembler::ZeroExtend(ManagedRegister mreg, size_t size) {
  X86_64ManagedRegister reg = mreg.AsX86_64();
  CHECK(size == 1 || size == 2) << size;
  CHECK(reg.IsCpuRegister()) << reg;
  if (size == 1) {
    movzxb(reg.AsCpuRegister(), reg.AsCpuRegister());
  } else {
    movzxw(reg.AsCpuRegister(), reg.AsCpuRegister());
  }
}

void X86_64Assembler::Move(ManagedRegister mdest, ManagedRegister msrc, size_t size) {
  X86_64ManagedRegister dest = mdest.AsX86_64();
  X86_64ManagedRegister src = msrc.AsX86_64();
  if (!dest.Equals(src)) {
    if (dest.IsCpuRegister() && src.IsCpuRegister()) {
      movq(dest.AsCpuRegister(), src.AsCpuRegister());
    } else if (src.IsX87Register() && dest.IsXmmRegister()) {
      // Pass via stack and pop X87 register
      subl(CpuRegister(RSP), Immediate(16));
      if (size == 4) {
        CHECK_EQ(src.AsX87Register(), ST0);
        fstps(Address(CpuRegister(RSP), 0));
        movss(dest.AsXmmRegister(), Address(CpuRegister(RSP), 0));
      } else {
        CHECK_EQ(src.AsX87Register(), ST0);
        fstpl(Address(CpuRegister(RSP), 0));
        movsd(dest.AsXmmRegister(), Address(CpuRegister(RSP), 0));
      }
      addq(CpuRegister(RSP), Immediate(16));
    } else {
      // TODO: x87, SSE
      UNIMPLEMENTED(FATAL) << ": Move " << dest << ", " << src;
    }
  }
}

void X86_64Assembler::CopyRef(FrameOffset dest, FrameOffset src, ManagedRegister mscratch) {
  X86_64ManagedRegister scratch = mscratch.AsX86_64();
  CHECK(scratch.IsCpuRegister());
  movl(scratch.AsCpuRegister(), Address(CpuRegister(RSP), src));
  movl(Address(CpuRegister(RSP), dest), scratch.AsCpuRegister());
}

void X86_64Assembler::CopyRawPtrFromThread64(FrameOffset fr_offs,
                                             ThreadOffset<8> thr_offs,
                                             ManagedRegister mscratch) {
  X86_64ManagedRegister scratch = mscratch.AsX86_64();
  CHECK(scratch.IsCpuRegister());
  gs()->movq(scratch.AsCpuRegister(), Address::Absolute(thr_offs, true));
  Store(fr_offs, scratch, 8);
}

void X86_64Assembler::CopyRawPtrToThread64(ThreadOffset<8> thr_offs,
                                           FrameOffset fr_offs,
                                           ManagedRegister mscratch) {
  X86_64ManagedRegister scratch = mscratch.AsX86_64();
  CHECK(scratch.IsCpuRegister());
  Load(scratch, fr_offs, 8);
  gs()->movq(Address::Absolute(thr_offs, true), scratch.AsCpuRegister());
}

void X86_64Assembler::Copy(FrameOffset dest, FrameOffset src, ManagedRegister mscratch,
                           size_t size) {
  X86_64ManagedRegister scratch = mscratch.AsX86_64();
  if (scratch.IsCpuRegister() && size == 8) {
    Load(scratch, src, 4);
    Store(dest, scratch, 4);
    Load(scratch, FrameOffset(src.Int32Value() + 4), 4);
    Store(FrameOffset(dest.Int32Value() + 4), scratch, 4);
  } else {
    Load(scratch, src, size);
    Store(dest, scratch, size);
  }
}

void X86_64Assembler::Copy(FrameOffset /*dst*/, ManagedRegister /*src_base*/, Offset /*src_offset*/,
                        ManagedRegister /*scratch*/, size_t /*size*/) {
  UNIMPLEMENTED(FATAL);
}

void X86_64Assembler::Copy(ManagedRegister dest_base, Offset dest_offset, FrameOffset src,
                        ManagedRegister scratch, size_t size) {
  CHECK(scratch.IsNoRegister());
  CHECK_EQ(size, 4u);
  pushq(Address(CpuRegister(RSP), src));
  popq(Address(dest_base.AsX86_64().AsCpuRegister(), dest_offset));
}

void X86_64Assembler::Copy(FrameOffset dest, FrameOffset src_base, Offset src_offset,
                        ManagedRegister mscratch, size_t size) {
  CpuRegister scratch = mscratch.AsX86_64().AsCpuRegister();
  CHECK_EQ(size, 4u);
  movq(scratch, Address(CpuRegister(RSP), src_base));
  movq(scratch, Address(scratch, src_offset));
  movq(Address(CpuRegister(RSP), dest), scratch);
}

void X86_64Assembler::Copy(ManagedRegister dest, Offset dest_offset,
                        ManagedRegister src, Offset src_offset,
                        ManagedRegister scratch, size_t size) {
  CHECK_EQ(size, 4u);
  CHECK(scratch.IsNoRegister());
  pushq(Address(src.AsX86_64().AsCpuRegister(), src_offset));
  popq(Address(dest.AsX86_64().AsCpuRegister(), dest_offset));
}

void X86_64Assembler::Copy(FrameOffset dest, Offset dest_offset, FrameOffset src, Offset src_offset,
                        ManagedRegister mscratch, size_t size) {
  CpuRegister scratch = mscratch.AsX86_64().AsCpuRegister();
  CHECK_EQ(size, 4u);
  CHECK_EQ(dest.Int32Value(), src.Int32Value());
  movq(scratch, Address(CpuRegister(RSP), src));
  pushq(Address(scratch, src_offset));
  popq(Address(scratch, dest_offset));
}

void X86_64Assembler::MemoryBarrier(ManagedRegister) {
  mfence();
}

void X86_64Assembler::CreateHandleScopeEntry(ManagedRegister mout_reg,
                                   FrameOffset handle_scope_offset,
                                   ManagedRegister min_reg, bool null_allowed) {
  X86_64ManagedRegister out_reg = mout_reg.AsX86_64();
  X86_64ManagedRegister in_reg = min_reg.AsX86_64();
  if (in_reg.IsNoRegister()) {  // TODO(64): && null_allowed
    // Use out_reg as indicator of null.
    in_reg = out_reg;
    // TODO: movzwl
    movl(in_reg.AsCpuRegister(), Address(CpuRegister(RSP), handle_scope_offset));
  }
  CHECK(in_reg.IsCpuRegister());
  CHECK(out_reg.IsCpuRegister());
  VerifyObject(in_reg, null_allowed);
  if (null_allowed) {
    Label null_arg;
    if (!out_reg.Equals(in_reg)) {
      xorl(out_reg.AsCpuRegister(), out_reg.AsCpuRegister());
    }
    testl(in_reg.AsCpuRegister(), in_reg.AsCpuRegister());
    j(kZero, &null_arg);
    leaq(out_reg.AsCpuRegister(), Address(CpuRegister(RSP), handle_scope_offset));
    Bind(&null_arg);
  } else {
    leaq(out_reg.AsCpuRegister(), Address(CpuRegister(RSP), handle_scope_offset));
  }
}

void X86_64Assembler::CreateHandleScopeEntry(FrameOffset out_off,
                                   FrameOffset handle_scope_offset,
                                   ManagedRegister mscratch,
                                   bool null_allowed) {
  X86_64ManagedRegister scratch = mscratch.AsX86_64();
  CHECK(scratch.IsCpuRegister());
  if (null_allowed) {
    Label null_arg;
    movl(scratch.AsCpuRegister(), Address(CpuRegister(RSP), handle_scope_offset));
    testl(scratch.AsCpuRegister(), scratch.AsCpuRegister());
    j(kZero, &null_arg);
    leaq(scratch.AsCpuRegister(), Address(CpuRegister(RSP), handle_scope_offset));
    Bind(&null_arg);
  } else {
    leaq(scratch.AsCpuRegister(), Address(CpuRegister(RSP), handle_scope_offset));
  }
  Store(out_off, scratch, 8);
}

// Given a handle scope entry, load the associated reference.
void X86_64Assembler::LoadReferenceFromHandleScope(ManagedRegister mout_reg,
                                         ManagedRegister min_reg) {
  X86_64ManagedRegister out_reg = mout_reg.AsX86_64();
  X86_64ManagedRegister in_reg = min_reg.AsX86_64();
  CHECK(out_reg.IsCpuRegister());
  CHECK(in_reg.IsCpuRegister());
  Label null_arg;
  if (!out_reg.Equals(in_reg)) {
    xorl(out_reg.AsCpuRegister(), out_reg.AsCpuRegister());
  }
  testl(in_reg.AsCpuRegister(), in_reg.AsCpuRegister());
  j(kZero, &null_arg);
  movq(out_reg.AsCpuRegister(), Address(in_reg.AsCpuRegister(), 0));
  Bind(&null_arg);
}

void X86_64Assembler::VerifyObject(ManagedRegister /*src*/, bool /*could_be_null*/) {
  // TODO: not validating references
}

void X86_64Assembler::VerifyObject(FrameOffset /*src*/, bool /*could_be_null*/) {
  // TODO: not validating references
}

void X86_64Assembler::Call(ManagedRegister mbase, Offset offset, ManagedRegister) {
  X86_64ManagedRegister base = mbase.AsX86_64();
  CHECK(base.IsCpuRegister());
  call(Address(base.AsCpuRegister(), offset.Int32Value()));
  // TODO: place reference map on call
}

void X86_64Assembler::Call(FrameOffset base, Offset offset, ManagedRegister mscratch) {
  CpuRegister scratch = mscratch.AsX86_64().AsCpuRegister();
  movq(scratch, Address(CpuRegister(RSP), base));
  call(Address(scratch, offset));
}

void X86_64Assembler::CallFromThread64(ThreadOffset<8> offset, ManagedRegister /*mscratch*/) {
  gs()->call(Address::Absolute(offset, true));
}

void X86_64Assembler::GetCurrentThread(ManagedRegister tr) {
  gs()->movq(tr.AsX86_64().AsCpuRegister(), Address::Absolute(Thread::SelfOffset<8>(), true));
}

void X86_64Assembler::GetCurrentThread(FrameOffset offset, ManagedRegister mscratch) {
  X86_64ManagedRegister scratch = mscratch.AsX86_64();
  gs()->movq(scratch.AsCpuRegister(), Address::Absolute(Thread::SelfOffset<8>(), true));
  movq(Address(CpuRegister(RSP), offset), scratch.AsCpuRegister());
}

// Slowpath entered when Thread::Current()->_exception is non-null
class X86_64ExceptionSlowPath FINAL : public SlowPath {
 public:
  explicit X86_64ExceptionSlowPath(size_t stack_adjust) : stack_adjust_(stack_adjust) {}
  virtual void Emit(Assembler *sp_asm) OVERRIDE;
 private:
  const size_t stack_adjust_;
};

void X86_64Assembler::ExceptionPoll(ManagedRegister /*scratch*/, size_t stack_adjust) {
  X86_64ExceptionSlowPath* slow = new X86_64ExceptionSlowPath(stack_adjust);
  buffer_.EnqueueSlowPath(slow);
  gs()->cmpl(Address::Absolute(Thread::ExceptionOffset<8>(), true), Immediate(0));
  j(kNotEqual, slow->Entry());
}

void X86_64ExceptionSlowPath::Emit(Assembler *sasm) {
  X86_64Assembler* sp_asm = down_cast<X86_64Assembler*>(sasm);
#define __ sp_asm->
  __ Bind(&entry_);
  // Note: the return value is dead
  if (stack_adjust_ != 0) {  // Fix up the frame.
    __ DecreaseFrameSize(stack_adjust_);
  }
  // Pass exception as argument in RDI
  __ gs()->movq(CpuRegister(RDI), Address::Absolute(Thread::ExceptionOffset<8>(), true));
  __ gs()->call(Address::Absolute(QUICK_ENTRYPOINT_OFFSET(8, pDeliverException), true));
  // this call should never return
  __ int3();
#undef __
}

void X86_64Assembler::AddConstantArea() {
  const std::vector<int32_t>& area = constant_area_.GetBuffer();
  for (size_t i = 0, e = area.size(); i < e; i++) {
    AssemblerBuffer::EnsureCapacity ensured(&buffer_);
    EmitInt32(area[i]);
  }
}

int ConstantArea::AddInt32(int32_t v) {
  for (size_t i = 0, e = buffer_.size(); i < e; i++) {
    if (v == buffer_[i]) {
      return i * elem_size_;
    }
  }

  // Didn't match anything.
  int result = buffer_.size() * elem_size_;
  buffer_.push_back(v);
  return result;
}

int ConstantArea::AddInt64(int64_t v) {
  int32_t v_low = v;
  int32_t v_high = v >> 32;
  if (buffer_.size() > 1) {
    // Ensure we don't pass the end of the buffer.
    for (size_t i = 0, e = buffer_.size() - 1; i < e; i++) {
      if (v_low == buffer_[i] && v_high == buffer_[i + 1]) {
        return i * elem_size_;
      }
    }
  }

  // Didn't match anything.
  int result = buffer_.size() * elem_size_;
  buffer_.push_back(v_low);
  buffer_.push_back(v_high);
  return result;
}

int ConstantArea::AddDouble(double v) {
  // Treat the value as a 64-bit integer value.
  return AddInt64(bit_cast<int64_t, double>(v));
}

int ConstantArea::AddFloat(float v) {
  // Treat the value as a 32-bit integer value.
  return AddInt32(bit_cast<int32_t, float>(v));
}

}  // namespace x86_64
}  // namespace art
