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

#ifndef ART_RUNTIME_ARCH_MIPS64_INSTRUCTION_SET_FEATURES_MIPS64_H_
#define ART_RUNTIME_ARCH_MIPS64_INSTRUCTION_SET_FEATURES_MIPS64_H_

#include "arch/instruction_set_features.h"

namespace art {

// Instruction set features relevant to the MIPS64 architecture.
class Mips64InstructionSetFeatures FINAL : public InstructionSetFeatures {
 public:
  // Process a CPU variant string like "r4000" and create InstructionSetFeatures.
  static const Mips64InstructionSetFeatures* FromVariant(const std::string& variant,
                                                        std::string* error_msg);

  // Parse a bitmap and create an InstructionSetFeatures.
  static const Mips64InstructionSetFeatures* FromBitmap(uint32_t bitmap);

  // Turn C pre-processor #defines into the equivalent instruction set features.
  static const Mips64InstructionSetFeatures* FromCppDefines();

  // Process /proc/cpuinfo and use kRuntimeISA to produce InstructionSetFeatures.
  static const Mips64InstructionSetFeatures* FromCpuInfo();

  // Process the auxiliary vector AT_HWCAP entry and use kRuntimeISA to produce
  // InstructionSetFeatures.
  static const Mips64InstructionSetFeatures* FromHwcap();

  // Use assembly tests of the current runtime (ie kRuntimeISA) to determine the
  // InstructionSetFeatures. This works around kernel bugs in AT_HWCAP and /proc/cpuinfo.
  static const Mips64InstructionSetFeatures* FromAssembly();

  bool Equals(const InstructionSetFeatures* other) const OVERRIDE;

  InstructionSet GetInstructionSet() const OVERRIDE {
    return kMips64;
  }

  uint32_t AsBitmap() const OVERRIDE;

  std::string GetFeatureString() const OVERRIDE;

  virtual ~Mips64InstructionSetFeatures() {}

 protected:
  // Parse a vector of the form "fpu32", "mips2" adding these to a new Mips64InstructionSetFeatures.
  virtual const InstructionSetFeatures*
      AddFeaturesFromSplitString(const bool smp, const std::vector<std::string>& features,
                                 std::string* error_msg) const OVERRIDE;

 private:
  explicit Mips64InstructionSetFeatures(bool smp) : InstructionSetFeatures(smp) {
  }

  // Bitmap positions for encoding features as a bitmap.
  enum {
    kSmpBitfield = 1,
  };

  DISALLOW_COPY_AND_ASSIGN(Mips64InstructionSetFeatures);
};

}  // namespace art

#endif  // ART_RUNTIME_ARCH_MIPS64_INSTRUCTION_SET_FEATURES_MIPS64_H_
