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

#ifndef ART_COMPILER_DEX_DEX_FLAGS_H_
#define ART_COMPILER_DEX_DEX_FLAGS_H_

namespace art {

// Suppress optimization if corresponding bit set.
enum OptControlVector {
  kLoadStoreElimination = 0,
  kLoadHoisting,
  kSuppressLoads,
  kNullCheckElimination,
  kClassInitCheckElimination,
  kGlobalValueNumbering,
  kGvnDeadCodeElimination,
  kLocalValueNumbering,
  kPromoteRegs,
  kTrackLiveTemps,
  kSafeOptimizations,
  kBBOpt,
  kSuspendCheckElimination,
  kMatch,
  kPromoteCompilerTemps,
  kBranchFusing,
  kSuppressExceptionEdges,
  kSuppressMethodInlining,
};

// Force code generation paths for testing.
enum DebugControlVector {
  kDebugVerbose,
  kDebugDumpCFG,
  kDebugSlowFieldPath,
  kDebugSlowInvokePath,
  kDebugSlowStringPath,
  kDebugSlowTypePath,
  kDebugSlowestFieldPath,
  kDebugSlowestStringPath,
  kDebugExerciseResolveMethod,
  kDebugVerifyDataflow,
  kDebugShowMemoryUsage,
  kDebugShowNops,
  kDebugCountOpcodes,
  kDebugDumpCheckStats,
  kDebugShowSummaryMemoryUsage,
  kDebugShowFilterStats,
  kDebugTimings,
  kDebugCodegenDump
};

}  // namespace art

#endif  // ART_COMPILER_DEX_DEX_FLAGS_H_
