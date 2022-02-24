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

#ifndef ART_COMPILER_COMPILER_H_
#define ART_COMPILER_COMPILER_H_

#include "dex_file.h"
#include "os.h"

namespace art {

class ArtMethod;
class Backend;
struct CompilationUnit;
class CompilerDriver;
class CompiledMethod;
class OatWriter;

class Compiler {
 public:
  enum Kind {
    kQuick,
    kOptimizing
  };

  static Compiler* Create(CompilerDriver* driver, Kind kind);

  virtual void Init() = 0;

  virtual void UnInit() const = 0;

  virtual bool CanCompileMethod(uint32_t method_idx, const DexFile& dex_file, CompilationUnit* cu)
      const = 0;

  virtual CompiledMethod* Compile(const DexFile::CodeItem* code_item,
                                  uint32_t access_flags,
                                  InvokeType invoke_type,
                                  uint16_t class_def_idx,
                                  uint32_t method_idx,
                                  jobject class_loader,
                                  const DexFile& dex_file) const = 0;

  virtual CompiledMethod* JniCompile(uint32_t access_flags,
                                     uint32_t method_idx,
                                     const DexFile& dex_file) const = 0;

  virtual uintptr_t GetEntryPointOf(ArtMethod* method) const
     SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) = 0;

  uint64_t GetMaximumCompilationTimeBeforeWarning() const {
    return maximum_compilation_time_before_warning_;
  }

  virtual void InitCompilationUnit(CompilationUnit& cu) const = 0;

  virtual ~Compiler() {}

  /*
   * @brief Generate and return Dwarf CFI initialization, if supported by the
   * backend.
   * @param driver CompilerDriver for this compile.
   * @returns nullptr if not supported by backend or a vector of bytes for CFI DWARF
   * information.
   * @note This is used for backtrace information in generated code.
   */
  virtual std::vector<uint8_t>* GetCallFrameInformationInitialization(const CompilerDriver& driver)
      const {
    UNUSED(driver);
    return nullptr;
  }

  // Returns whether the method to compile is such a pathological case that
  // it's not worth compiling.
  static bool IsPathologicalCase(const DexFile::CodeItem& code_item,
                                 uint32_t method_idx,
                                 const DexFile& dex_file);

 protected:
  explicit Compiler(CompilerDriver* driver, uint64_t warning) :
      driver_(driver), maximum_compilation_time_before_warning_(warning) {
  }

  CompilerDriver* GetCompilerDriver() const {
    return driver_;
  }

 private:
  CompilerDriver* const driver_;
  const uint64_t maximum_compilation_time_before_warning_;

  DISALLOW_COPY_AND_ASSIGN(Compiler);
};

}  // namespace art

#endif  // ART_COMPILER_COMPILER_H_
