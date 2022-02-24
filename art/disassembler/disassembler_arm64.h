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

#ifndef ART_DISASSEMBLER_DISASSEMBLER_ARM64_H_
#define ART_DISASSEMBLER_DISASSEMBLER_ARM64_H_

#include "disassembler.h"

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wshadow"
#include "vixl/a64/decoder-a64.h"
#include "vixl/a64/disasm-a64.h"
#pragma GCC diagnostic pop

namespace art {
namespace arm64 {

class CustomDisassembler FINAL : public vixl::Disassembler {
 public:
  explicit CustomDisassembler(DisassemblerOptions* options) :
      vixl::Disassembler(), read_literals_(options->can_read_literals_) {
    if (!options->absolute_addresses_) {
      MapCodeAddress(0, reinterpret_cast<const vixl::Instruction*>(options->base_address_));
    }
  }

  // Use register aliases in the disassembly.
  void AppendRegisterNameToOutput(const vixl::Instruction* instr,
                                  const vixl::CPURegister& reg) OVERRIDE;

  // Improve the disassembly of literal load instructions.
  void VisitLoadLiteral(const vixl::Instruction* instr) OVERRIDE;

  // Improve the disassembly of thread offset.
  void VisitLoadStoreUnsignedOffset(const vixl::Instruction* instr) OVERRIDE;

 private:
  // Indicate if the disassembler should read data loaded from literal pools.
  // This should only be enabled if reading the target of literal loads is safe.
  // Here are possible outputs when the option is on or off:
  // read_literals_ | disassembly
  //           true | 0x72681558: 1c000acb  ldr s11, pc+344 (addr 0x726816b0)
  //          false | 0x72681558: 1c000acb  ldr s11, pc+344 (addr 0x726816b0) (3.40282e+38)
  const bool read_literals_;
};

class DisassemblerArm64 FINAL : public Disassembler {
 public:
  explicit DisassemblerArm64(DisassemblerOptions* options) :
      Disassembler(options), disasm(options) {
    decoder.AppendVisitor(&disasm);
  }

  size_t Dump(std::ostream& os, const uint8_t* begin) OVERRIDE;
  void Dump(std::ostream& os, const uint8_t* begin, const uint8_t* end) OVERRIDE;

 private:
  vixl::Decoder decoder;
  CustomDisassembler disasm;

  DISALLOW_COPY_AND_ASSIGN(DisassemblerArm64);
};

}  // namespace arm64
}  // namespace art

#endif  // ART_DISASSEMBLER_DISASSEMBLER_ARM64_H_
