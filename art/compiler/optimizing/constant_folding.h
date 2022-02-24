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

#ifndef ART_COMPILER_OPTIMIZING_CONSTANT_FOLDING_H_
#define ART_COMPILER_OPTIMIZING_CONSTANT_FOLDING_H_

#include "nodes.h"
#include "optimization.h"

namespace art {

/**
 * Optimization pass performing a simple constant-expression
 * evaluation on the SSA form.
 *
 * This class is named art::HConstantFolding to avoid name
 * clashes with the art::ConstantPropagation class defined in
 * compiler/dex/post_opt_passes.h.
 */
class HConstantFolding : public HOptimization {
 public:
  explicit HConstantFolding(HGraph* graph, const char* name = kConstantFoldingPassName)
      : HOptimization(graph, true, name) {}

  void Run() OVERRIDE;

  static constexpr const char* kConstantFoldingPassName = "constant_folding";

 private:
  DISALLOW_COPY_AND_ASSIGN(HConstantFolding);
};

}  // namespace art

#endif  // ART_COMPILER_OPTIMIZING_CONSTANT_FOLDING_H_
