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

#ifndef ART_COMPILER_OPTIMIZING_GVN_H_
#define ART_COMPILER_OPTIMIZING_GVN_H_

#include "nodes.h"
#include "optimization.h"

namespace art {

class SideEffectsAnalysis;

class GVNOptimization : public HOptimization {
 public:
  GVNOptimization(HGraph* graph, const SideEffectsAnalysis& side_effects)
      : HOptimization(graph, true, kGlobalValueNumberingPassName), side_effects_(side_effects) {}

  void Run() OVERRIDE;

  static constexpr const char* kGlobalValueNumberingPassName = "GVN";

 private:
  const SideEffectsAnalysis& side_effects_;

  DISALLOW_COPY_AND_ASSIGN(GVNOptimization);
};

}  // namespace art

#endif  // ART_COMPILER_OPTIMIZING_GVN_H_
