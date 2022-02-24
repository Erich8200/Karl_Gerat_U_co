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

#ifndef ART_COMPILER_OPTIMIZING_DEAD_CODE_ELIMINATION_H_
#define ART_COMPILER_OPTIMIZING_DEAD_CODE_ELIMINATION_H_

#include "nodes.h"
#include "optimization.h"
#include "optimizing_compiler_stats.h"

namespace art {

/**
 * Optimization pass performing dead code elimination (removal of
 * unused variables/instructions) on the SSA form.
 */
class HDeadCodeElimination : public HOptimization {
 public:
  HDeadCodeElimination(HGraph* graph,
                       OptimizingCompilerStats* stats = nullptr,
                       const char* name = kInitialDeadCodeEliminationPassName)
      : HOptimization(graph, true, name, stats) {}

  void Run() OVERRIDE;

  static constexpr const char* kInitialDeadCodeEliminationPassName = "dead_code_elimination";
  static constexpr const char* kFinalDeadCodeEliminationPassName = "dead_code_elimination_final";

 private:
  void MaybeRecordDeadBlock(HBasicBlock* block);
  void RemoveDeadBlocks();
  void RemoveDeadInstructions();

  DISALLOW_COPY_AND_ASSIGN(HDeadCodeElimination);
};

}  // namespace art

#endif  // ART_COMPILER_OPTIMIZING_DEAD_CODE_ELIMINATION_H_
