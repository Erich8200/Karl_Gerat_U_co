/*
 * Copyright (C) 2015 The Android Open Source Project
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

#include "linker/relative_patcher.h"

#include "linker/arm/relative_patcher_thumb2.h"
#include "linker/arm64/relative_patcher_arm64.h"
#include "linker/x86/relative_patcher_x86.h"
#include "linker/x86_64/relative_patcher_x86_64.h"
#include "output_stream.h"

namespace art {
namespace linker {

std::unique_ptr<RelativePatcher> RelativePatcher::Create(
    InstructionSet instruction_set, const InstructionSetFeatures* features,
    RelativePatcherTargetProvider* provider) {
  class RelativePatcherNone FINAL : public RelativePatcher {
   public:
    RelativePatcherNone() { }

    uint32_t ReserveSpace(uint32_t offset,
                          const CompiledMethod* compiled_method ATTRIBUTE_UNUSED,
                          MethodReference method_ref ATTRIBUTE_UNUSED) OVERRIDE {
      return offset;  // No space reserved; no patches expected.
    }

    uint32_t ReserveSpaceEnd(uint32_t offset) OVERRIDE {
      return offset;  // No space reserved; no patches expected.
    }

    uint32_t WriteThunks(OutputStream* out ATTRIBUTE_UNUSED, uint32_t offset) OVERRIDE {
      return offset;  // No thunks added; no patches expected.
    }

    void PatchCall(std::vector<uint8_t>* code ATTRIBUTE_UNUSED,
                   uint32_t literal_offset ATTRIBUTE_UNUSED,
                   uint32_t patch_offset ATTRIBUTE_UNUSED,
                   uint32_t target_offset ATTRIBUTE_UNUSED) OVERRIDE {
      LOG(FATAL) << "Unexpected relative call patch.";
    }

    virtual void PatchDexCacheReference(std::vector<uint8_t>* code ATTRIBUTE_UNUSED,
                                        const LinkerPatch& patch ATTRIBUTE_UNUSED,
                                        uint32_t patch_offset ATTRIBUTE_UNUSED,
                                        uint32_t target_offset ATTRIBUTE_UNUSED) {
      LOG(FATAL) << "Unexpected relative dex cache array patch.";
    }

   private:
    DISALLOW_COPY_AND_ASSIGN(RelativePatcherNone);
  };

  switch (instruction_set) {
    case kX86:
      return std::unique_ptr<RelativePatcher>(new X86RelativePatcher());
    case kX86_64:
      return std::unique_ptr<RelativePatcher>(new X86_64RelativePatcher());
    case kArm:
      // Fall through: we generate Thumb2 code for "arm".
    case kThumb2:
      return std::unique_ptr<RelativePatcher>(new Thumb2RelativePatcher(provider));
    case kArm64:
      return std::unique_ptr<RelativePatcher>(
          new Arm64RelativePatcher(provider, features->AsArm64InstructionSetFeatures()));
    default:
      return std::unique_ptr<RelativePatcher>(new RelativePatcherNone);
  }
}

bool RelativePatcher::WriteCodeAlignment(OutputStream* out, uint32_t aligned_code_delta) {
  static const uint8_t kPadding[] = {
      0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u
  };
  DCHECK_LE(aligned_code_delta, sizeof(kPadding));
  if (UNLIKELY(!out->WriteFully(kPadding, aligned_code_delta))) {
    return false;
  }
  size_code_alignment_ += aligned_code_delta;
  return true;
}

bool RelativePatcher::WriteRelCallThunk(OutputStream* out, const ArrayRef<const uint8_t>& thunk) {
  if (UNLIKELY(!out->WriteFully(thunk.data(), thunk.size()))) {
    return false;
  }
  size_relative_call_thunks_ += thunk.size();
  return true;
}

bool RelativePatcher::WriteMiscThunk(OutputStream* out, const ArrayRef<const uint8_t>& thunk) {
  if (UNLIKELY(!out->WriteFully(thunk.data(), thunk.size()))) {
    return false;
  }
  size_misc_thunks_ += thunk.size();
  return true;
}

}  // namespace linker
}  // namespace art
