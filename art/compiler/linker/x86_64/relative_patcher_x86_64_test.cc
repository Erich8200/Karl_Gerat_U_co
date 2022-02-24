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
#include "linker/x86_64/relative_patcher_x86_64.h"

namespace art {
namespace linker {

class X86_64RelativePatcherTest : public RelativePatcherTest {
 public:
  X86_64RelativePatcherTest() : RelativePatcherTest(kX86_64, "default") { }

 protected:
  static const uint8_t kCallRawCode[];
  static const ArrayRef<const uint8_t> kCallCode;
  static const uint8_t kDexCacheLoadRawCode[];
  static const ArrayRef<const uint8_t> kDexCacheLoadCode;

  uint32_t GetMethodOffset(uint32_t method_idx) {
    auto result = method_offset_map_.FindMethodOffset(MethodRef(method_idx));
    CHECK(result.first);
    return result.second;
  }
};

const uint8_t X86_64RelativePatcherTest::kCallRawCode[] = {
    0xe8, 0x00, 0x01, 0x00, 0x00
};

const ArrayRef<const uint8_t> X86_64RelativePatcherTest::kCallCode(kCallRawCode);

const uint8_t X86_64RelativePatcherTest::kDexCacheLoadRawCode[] = {
    0x8b, 0x05,  // mov eax, [rip + <offset>]
    0x00, 0x01, 0x00, 0x00
};

const ArrayRef<const uint8_t> X86_64RelativePatcherTest::kDexCacheLoadCode(
    kDexCacheLoadRawCode);

TEST_F(X86_64RelativePatcherTest, CallSelf) {
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

TEST_F(X86_64RelativePatcherTest, CallOther) {
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

TEST_F(X86_64RelativePatcherTest, CallTrampoline) {
  LinkerPatch patches[] = {
      LinkerPatch::RelativeCodePatch(kCallCode.size() - 4u, nullptr, 2u),
  };
  AddCompiledMethod(MethodRef(1u), kCallCode, ArrayRef<const LinkerPatch>(patches));
  Link();

  auto result = method_offset_map_.FindMethodOffset(MethodRef(1u));
  ASSERT_TRUE(result.first);
  uint32_t diff = kTrampolineOffset - (result.second + kCallCode.size());
  static const uint8_t expected_code[] = {
      0xe8,
      static_cast<uint8_t>(diff), static_cast<uint8_t>(diff >> 8),
      static_cast<uint8_t>(diff >> 16), static_cast<uint8_t>(diff >> 24)
  };
  EXPECT_TRUE(CheckLinkedMethod(MethodRef(1u), ArrayRef<const uint8_t>(expected_code)));
}

TEST_F(X86_64RelativePatcherTest, DexCacheReference) {
  dex_cache_arrays_begin_ = 0x12345678;
  constexpr size_t kElementOffset = 0x1234;
  LinkerPatch patches[] = {
      LinkerPatch::DexCacheArrayPatch(kDexCacheLoadCode.size() - 4u, nullptr, 0u, kElementOffset),
  };
  AddCompiledMethod(MethodRef(1u), kDexCacheLoadCode, ArrayRef<const LinkerPatch>(patches));
  Link();

  auto result = method_offset_map_.FindMethodOffset(MethodRef(1u));
  ASSERT_TRUE(result.first);
  uint32_t diff =
      dex_cache_arrays_begin_ + kElementOffset - (result.second + kDexCacheLoadCode.size());
  static const uint8_t expected_code[] = {
      0x8b, 0x05,
      static_cast<uint8_t>(diff), static_cast<uint8_t>(diff >> 8),
      static_cast<uint8_t>(diff >> 16), static_cast<uint8_t>(diff >> 24)
  };
  EXPECT_TRUE(CheckLinkedMethod(MethodRef(1u), ArrayRef<const uint8_t>(expected_code)));
}

}  // namespace linker
}  // namespace art
