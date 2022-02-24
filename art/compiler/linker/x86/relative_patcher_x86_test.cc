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

#include "linker/relative_patcher_test.h"
#include "linker/x86/relative_patcher_x86.h"

namespace art {
namespace linker {

class X86RelativePatcherTest : public RelativePatcherTest {
 public:
  X86RelativePatcherTest() : RelativePatcherTest(kX86, "default") { }

 protected:
  static const uint8_t kCallRawCode[];
  static const ArrayRef<const uint8_t> kCallCode;

  uint32_t GetMethodOffset(uint32_t method_idx) {
    auto result = method_offset_map_.FindMethodOffset(MethodRef(method_idx));
    CHECK(result.first);
    return result.second;
  }
};

const uint8_t X86RelativePatcherTest::kCallRawCode[] = {
    0xe8, 0x00, 0x01, 0x00, 0x00
};

const ArrayRef<const uint8_t> X86RelativePatcherTest::kCallCode(kCallRawCode);

TEST_F(X86RelativePatcherTest, CallSelf) {
  LinkerPatch patches[] = {
      LinkerPatch::RelativeCodePatch(kCallCode.size() - 4u, nullptr, 1u),
  };
  AddCompiledMethod(MethodRef(1u), kCallCode, ArrayRef<const LinkerPatch>(patches));
  Link();

  static const uint8_t expected_code[] = {
      0xe8, 0xfb, 0xff, 0xff, 0xff
  };
  EXPECT_TRUE(CheckLinkedMethod(MethodRef(1u), ArrayRef<const uint8_t>(expected_code)));
}

TEST_F(X86RelativePatcherTest, CallOther) {
  LinkerPatch method1_patches[] = {
      LinkerPatch::RelativeCodePatch(kCallCode.size() - 4u, nullptr, 2u),
  };
  AddCompiledMethod(MethodRef(1u), kCallCode, ArrayRef<const LinkerPatch>(method1_patches));
  LinkerPatch method2_patches[] = {
      LinkerPatch::RelativeCodePatch(kCallCode.size() - 4u, nullptr, 1u),
  };
  AddCompiledMethod(MethodRef(2u), kCallCode, ArrayRef<const LinkerPatch>(method2_patches));
  Link();

  uint32_t method1_offset = GetMethodOffset(1u);
  uint32_t method2_offset = GetMethodOffset(2u);
  uint32_t diff_after = method2_offset - (method1_offset + kCallCode.size() /* PC adjustment */);
  static const uint8_t method1_expected_code[] = {
      0xe8,
      static_cast<uint8_t>(diff_after), static_cast<uint8_t>(diff_after >> 8),
      static_cast<uint8_t>(diff_after >> 16), static_cast<uint8_t>(diff_after >> 24)
  };
  EXPECT_TRUE(CheckLinkedMethod(MethodRef(1u), ArrayRef<const uint8_t>(method1_expected_code)));
  uint32_t diff_before = method1_offset - (method2_offset + kCallCode.size() /* PC adjustment */);
  static const uint8_t method2_expected_code[] = {
      0xe8,
      static_cast<uint8_t>(diff_before), static_cast<uint8_t>(diff_before >> 8),
      static_cast<uint8_t>(diff_before >> 16), static_cast<uint8_t>(diff_before >> 24)
  };
  EXPECT_TRUE(CheckLinkedMethod(MethodRef(2u), ArrayRef<const uint8_t>(method2_expected_code)));
}

TEST_F(X86RelativePatcherTest, CallTrampoline) {
  LinkerPatch patches[] = {
      LinkerPatch::RelativeCodePatch(kCallCode.size() - 4u, nullptr, 2u),
  };
  AddCompiledMethod(MethodRef(1u), kCallCode, ArrayRef<const LinkerPatch>(patches));
  Link();

  auto result = method_offset_map_.FindMethodOffset(MethodRef(1));
  ASSERT_TRUE(result.first);
  uint32_t diff = kTrampolineOffset - (result.second + kCallCode.size());
  static const uint8_t expected_code[] = {
      0xe8,
      static_cast<uint8_t>(diff), static_cast<uint8_t>(diff >> 8),
      static_cast<uint8_t>(diff >> 16), static_cast<uint8_t>(diff >> 24)
  };
  EXPECT_TRUE(CheckLinkedMethod(MethodRef(1u), ArrayRef<const uint8_t>(expected_code)));
}

TEST_F(X86RelativePatcherTest, DexCacheReference) {
  dex_cache_arrays_begin_ = 0x12345678;
  constexpr size_t kElementOffset = 0x1234;
  static const uint8_t raw_code[] = {
      0xe8, 0x00, 0x00, 0x00, 0x00,         // call +0
      0x5b,                                 // pop ebx
      0x8b, 0x83, 0x00, 0x01, 0x00, 0x00,   // mov eax, [ebx + 256 (kDummy32BitValue)]
  };
  constexpr uint32_t anchor_offset = 5u;  // After call +0.
  ArrayRef<const uint8_t> code(raw_code);
  LinkerPatch patches[] = {
      LinkerPatch::DexCacheArrayPatch(code.size() - 4u, nullptr, anchor_offset, kElementOffset),
  };
  AddCompiledMethod(MethodRef(1u), code, ArrayRef<const LinkerPatch>(patches));
  Link();

  auto result = method_offset_map_.FindMethodOffset(MethodRef(1u));
  ASSERT_TRUE(result.first);
  uint32_t diff =
      dex_cache_arrays_begin_ + kElementOffset - (result.second + anchor_offset);
  static const uint8_t expected_code[] = {
      0xe8, 0x00, 0x00, 0x00, 0x00,         // call +0
      0x5b,                                 // pop ebx
      0x8b, 0x83,                           // mov eax, [ebx + diff]
      static_cast<uint8_t>(diff), static_cast<uint8_t>(diff >> 8),
      static_cast<uint8_t>(diff >> 16), static_cast<uint8_t>(diff >> 24)
  };
  EXPECT_TRUE(CheckLinkedMethod(MethodRef(1u), ArrayRef<const uint8_t>(expected_code)));
}

}  // namespace linker
}  // namespace art
