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

#ifndef ART_COMPILER_DEX_BB_OPTIMIZATIONS_H_
#define ART_COMPILER_DEX_BB_OPTIMIZATIONS_H_

#include "base/casts.h"
#include "compiler_ir.h"
#include "dex_flags.h"
#include "pass_me.h"
#include "mir_graph.h"

namespace art {

/**
 * @class String Change
 * @brief Converts calls to String.<init> to StringFactory instead.
 */
class StringChange : public PassME {
 public:
  StringChange() : PassME("StringChange", kNoNodes) {
  }

  void Start(PassDataHolder* data) const {
    DCHECK(data != nullptr);
    CompilationUnit* c_unit = down_cast<PassMEDataHolder*>(data)->c_unit;
    DCHECK(c_unit != nullptr);
    c_unit->mir_graph->StringChange();
  }

  bool Gate(const PassDataHolder* data) const {
    DCHECK(data != nullptr);
    CompilationUnit* c_unit = down_cast<const PassMEDataHolder*>(data)->c_unit;
    DCHECK(c_unit != nullptr);
    return c_unit->mir_graph->HasInvokes();
  }
};

/**
 * @class CacheFieldLoweringInfo
 * @brief Cache the lowering info for fields used by IGET/IPUT/SGET/SPUT insns.
 */
class CacheFieldLoweringInfo : public PassME {
 public:
  CacheFieldLoweringInfo() : PassME("CacheFieldLoweringInfo", kNoNodes) {
  }

  void Start(PassDataHolder* data) const {
    DCHECK(data != nullptr);
    CompilationUnit* c_unit = down_cast<PassMEDataHolder*>(data)->c_unit;
    DCHECK(c_unit != nullptr);
    c_unit->mir_graph->DoCacheFieldLoweringInfo();
  }

  bool Gate(const PassDataHolder* data) const {
    DCHECK(data != nullptr);
    CompilationUnit* c_unit = down_cast<const PassMEDataHolder*>(data)->c_unit;
    DCHECK(c_unit != nullptr);
    return c_unit->mir_graph->HasFieldAccess();
  }
};

/**
 * @class CacheMethodLoweringInfo
 * @brief Cache the lowering info for methods called by INVOKEs.
 */
class CacheMethodLoweringInfo : public PassME {
 public:
  CacheMethodLoweringInfo() : PassME("CacheMethodLoweringInfo", kNoNodes) {
  }

  void Start(PassDataHolder* data) const {
    DCHECK(data != nullptr);
    CompilationUnit* c_unit = down_cast<PassMEDataHolder*>(data)->c_unit;
    DCHECK(c_unit != nullptr);
    c_unit->mir_graph->DoCacheMethodLoweringInfo();
  }

  bool Gate(const PassDataHolder* data) const {
    DCHECK(data != nullptr);
    CompilationUnit* c_unit = down_cast<const PassMEDataHolder*>(data)->c_unit;
    DCHECK(c_unit != nullptr);
    return c_unit->mir_graph->HasInvokes();
  }
};

/**
 * @class SpecialMethodInliner
 * @brief Performs method inlining pass on special kinds of methods.
 * @details Special methods are methods that fall in one of the following categories:
 * empty, instance getter, instance setter, argument return, and constant return.
 */
class SpecialMethodInliner : public PassME {
 public:
  SpecialMethodInliner() : PassME("SpecialMethodInliner") {
  }

  bool Gate(const PassDataHolder* data) const {
    DCHECK(data != nullptr);
    CompilationUnit* c_unit = down_cast<const PassMEDataHolder*>(data)->c_unit;
    DCHECK(c_unit != nullptr);
    return c_unit->mir_graph->InlineSpecialMethodsGate();
  }

  void Start(PassDataHolder* data) const {
    DCHECK(data != nullptr);
    CompilationUnit* c_unit = down_cast<PassMEDataHolder*>(data)->c_unit;
    DCHECK(c_unit != nullptr);
    c_unit->mir_graph->InlineSpecialMethodsStart();
  }

  bool Worker(PassDataHolder* data) const {
    DCHECK(data != nullptr);
    PassMEDataHolder* pass_me_data_holder = down_cast<PassMEDataHolder*>(data);
    CompilationUnit* c_unit = pass_me_data_holder->c_unit;
    DCHECK(c_unit != nullptr);
    BasicBlock* bb = pass_me_data_holder->bb;
    DCHECK(bb != nullptr);
    c_unit->mir_graph->InlineSpecialMethods(bb);
    // No need of repeating, so just return false.
    return false;
  }

  void End(PassDataHolder* data) const {
    DCHECK(data != nullptr);
    CompilationUnit* c_unit = down_cast<PassMEDataHolder*>(data)->c_unit;
    DCHECK(c_unit != nullptr);
    c_unit->mir_graph->InlineSpecialMethodsEnd();
  }
};

/**
 * @class CodeLayout
 * @brief Perform the code layout pass.
 */
class CodeLayout : public PassME {
 public:
  CodeLayout() : PassME("CodeLayout", kAllNodes, kOptimizationBasicBlockChange, "2_post_layout_cfg") {
  }

  void Start(PassDataHolder* data) const {
    DCHECK(data != nullptr);
    CompilationUnit* c_unit = down_cast<PassMEDataHolder*>(data)->c_unit;
    DCHECK(c_unit != nullptr);
    c_unit->mir_graph->VerifyDataflow();
    c_unit->mir_graph->ClearAllVisitedFlags();
  }

  bool Worker(PassDataHolder* data) const;
};

/**
 * @class NullCheckElimination
 * @brief Null check elimination pass.
 */
class NullCheckElimination : public PassME {
 public:
  NullCheckElimination()
    : PassME("NCE", kRepeatingPreOrderDFSTraversal, "3_post_nce_cfg") {
  }

  bool Gate(const PassDataHolder* data) const {
    DCHECK(data != nullptr);
    CompilationUnit* c_unit = down_cast<const PassMEDataHolder*>(data)->c_unit;
    DCHECK(c_unit != nullptr);
    return c_unit->mir_graph->EliminateNullChecksGate();
  }

  bool Worker(PassDataHolder* data) const {
    DCHECK(data != nullptr);
    PassMEDataHolder* pass_me_data_holder = down_cast<PassMEDataHolder*>(data);
    CompilationUnit* c_unit = pass_me_data_holder->c_unit;
    DCHECK(c_unit != nullptr);
    BasicBlock* bb = pass_me_data_holder->bb;
    DCHECK(bb != nullptr);
    return c_unit->mir_graph->EliminateNullChecks(bb);
  }

  void End(PassDataHolder* data) const {
    DCHECK(data != nullptr);
    CompilationUnit* c_unit = down_cast<PassMEDataHolder*>(data)->c_unit;
    DCHECK(c_unit != nullptr);
    c_unit->mir_graph->EliminateNullChecksEnd();
  }
};

class ClassInitCheckElimination : public PassME {
 public:
  ClassInitCheckElimination()
    : PassME("ClInitCheckElimination", kRepeatingPreOrderDFSTraversal) {
  }

  bool Gate(const PassDataHolder* data) const {
    DCHECK(data != nullptr);
    CompilationUnit* c_unit = down_cast<const PassMEDataHolder*>(data)->c_unit;
    DCHECK(c_unit != nullptr);
    return c_unit->mir_graph->EliminateClassInitChecksGate();
  }

  bool Worker(PassDataHolder* data) const {
    DCHECK(data != nullptr);
    PassMEDataHolder* pass_me_data_holder = down_cast<PassMEDataHolder*>(data);
    CompilationUnit* c_unit = pass_me_data_holder->c_unit;
    DCHECK(c_unit != nullptr);
    BasicBlock* bb = pass_me_data_holder->bb;
    DCHECK(bb != nullptr);
    return c_unit->mir_graph->EliminateClassInitChecks(bb);
  }

  void End(PassDataHolder* data) const {
    DCHECK(data != nullptr);
    CompilationUnit* c_unit = down_cast<PassMEDataHolder*>(data)->c_unit;
    DCHECK(c_unit != nullptr);
    c_unit->mir_graph->EliminateClassInitChecksEnd();
  }
};

/**
 * @class GlobalValueNumberingPass
 * @brief Performs the global value numbering pass.
 */
class GlobalValueNumberingPass : public PassME {
 public:
  GlobalValueNumberingPass()
    : PassME("GVN", kLoopRepeatingTopologicalSortTraversal, "4_post_gvn_cfg") {
  }

  bool Gate(const PassDataHolder* data) const OVERRIDE {
    DCHECK(data != nullptr);
    CompilationUnit* c_unit = down_cast<const PassMEDataHolder*>(data)->c_unit;
    DCHECK(c_unit != nullptr);
    return c_unit->mir_graph->ApplyGlobalValueNumberingGate();
  }

  bool Worker(PassDataHolder* data) const {
    DCHECK(data != nullptr);
    PassMEDataHolder* pass_me_data_holder = down_cast<PassMEDataHolder*>(data);
    CompilationUnit* c_unit = pass_me_data_holder->c_unit;
    DCHECK(c_unit != nullptr);
    BasicBlock* bb = pass_me_data_holder->bb;
    DCHECK(bb != nullptr);
    return c_unit->mir_graph->ApplyGlobalValueNumbering(bb);
  }

  void End(PassDataHolder* data) const OVERRIDE {
    DCHECK(data != nullptr);
    CompilationUnit* c_unit = down_cast<PassMEDataHolder*>(data)->c_unit;
    DCHECK(c_unit != nullptr);
    c_unit->mir_graph->ApplyGlobalValueNumberingEnd();
  }
};

/**
 * @class DeadCodeEliminationPass
 * @brief Performs the GVN-based dead code elimination pass.
 */
class DeadCodeEliminationPass : public PassME {
 public:
  DeadCodeEliminationPass() : PassME("DCE", kPreOrderDFSTraversal, "4_post_dce_cfg") {
  }

  bool Gate(const PassDataHolder* data) const OVERRIDE {
    DCHECK(data != nullptr);
    CompilationUnit* c_unit = down_cast<const PassMEDataHolder*>(data)->c_unit;
    DCHECK(c_unit != nullptr);
    return c_unit->mir_graph->EliminateDeadCodeGate();
  }

  bool Worker(PassDataHolder* data) const {
    DCHECK(data != nullptr);
    PassMEDataHolder* pass_me_data_holder = down_cast<PassMEDataHolder*>(data);
    CompilationUnit* c_unit = pass_me_data_holder->c_unit;
    DCHECK(c_unit != nullptr);
    BasicBlock* bb = pass_me_data_holder->bb;
    DCHECK(bb != nullptr);
    return c_unit->mir_graph->EliminateDeadCode(bb);
  }

  void End(PassDataHolder* data) const OVERRIDE {
    DCHECK(data != nullptr);
    CompilationUnit* c_unit = down_cast<PassMEDataHolder*>(data)->c_unit;
    DCHECK(c_unit != nullptr);
    c_unit->mir_graph->EliminateDeadCodeEnd();
  }
};

/**
 * @class GlobalValueNumberingCleanupPass
 * @brief Performs the cleanup after global value numbering pass and the dependent
 *        dead code elimination pass that needs the GVN data.
 */
class GlobalValueNumberingCleanupPass : public PassME {
 public:
  GlobalValueNumberingCleanupPass()
    : PassME("GVNCleanup", kNoNodes, "") {
  }

  void Start(PassDataHolder* data) const OVERRIDE {
    DCHECK(data != nullptr);
    CompilationUnit* c_unit = down_cast<const PassMEDataHolder*>(data)->c_unit;
    DCHECK(c_unit != nullptr);
    return c_unit->mir_graph->GlobalValueNumberingCleanup();
  }
};

/**
 * @class BBCombine
 * @brief Perform the basic block combination pass.
 */
class BBCombine : public PassME {
 public:
  BBCombine() : PassME("BBCombine", kPreOrderDFSTraversal, "5_post_bbcombine_cfg") {
  }

  bool Gate(const PassDataHolder* data) const {
    DCHECK(data != nullptr);
    CompilationUnit* c_unit = down_cast<const PassMEDataHolder*>(data)->c_unit;
    DCHECK(c_unit != nullptr);
    return c_unit->mir_graph->HasTryCatchBlocks() ||
        ((c_unit->disable_opt & (1 << kSuppressExceptionEdges)) != 0);
  }

  bool Worker(PassDataHolder* data) const;
};

/**
 * @class ConstantPropagation
 * @brief Perform a constant propagation pass.
 */
class ConstantPropagation : public PassME {
 public:
  ConstantPropagation() : PassME("ConstantPropagation") {
  }

  void Start(PassDataHolder* data) const {
    DCHECK(data != nullptr);
    CompilationUnit* c_unit = down_cast<PassMEDataHolder*>(data)->c_unit;
    DCHECK(c_unit != nullptr);
    c_unit->mir_graph->InitializeConstantPropagation();
  }

  bool Worker(PassDataHolder* data) const {
    DCHECK(data != nullptr);
    CompilationUnit* c_unit = down_cast<PassMEDataHolder*>(data)->c_unit;
    DCHECK(c_unit != nullptr);
    BasicBlock* bb = down_cast<PassMEDataHolder*>(data)->bb;
    DCHECK(bb != nullptr);
    c_unit->mir_graph->DoConstantPropagation(bb);
    // No need of repeating, so just return false.
    return false;
  }
};

/**
 * @class MethodUseCount
 * @brief Count the register uses of the method
 */
class MethodUseCount : public PassME {
 public:
  MethodUseCount() : PassME("UseCount") {
  }

  bool Worker(PassDataHolder* data) const;

  bool Gate(const PassDataHolder* data) const;
};

/**
 * @class BasicBlock Optimizations
 * @brief Any simple BasicBlock optimization can be put here.
 */
class BBOptimizations : public PassME {
 public:
  BBOptimizations()
      : PassME("BBOptimizations", kNoNodes, kOptimizationBasicBlockChange, "5_post_bbo_cfg") {
  }

  bool Gate(const PassDataHolder* data) const {
    DCHECK(data != nullptr);
    CompilationUnit* c_unit = down_cast<const PassMEDataHolder*>(data)->c_unit;
    DCHECK(c_unit != nullptr);
    return ((c_unit->disable_opt & (1 << kBBOpt)) == 0);
  }

  void Start(PassDataHolder* data) const {
    DCHECK(data != nullptr);
    CompilationUnit* c_unit = down_cast<PassMEDataHolder*>(data)->c_unit;
    DCHECK(c_unit != nullptr);
    c_unit->mir_graph->BasicBlockOptimizationStart();

    /*
     * This pass has a different ordering depending on the suppress exception,
     * so do the pass here for now:
     *   - Later, the Start should just change the ordering and we can move the extended
     *     creation into the pass driver's main job with a new iterator
     */
    c_unit->mir_graph->BasicBlockOptimization();
  }

  void End(PassDataHolder* data) const {
    DCHECK(data != nullptr);
    CompilationUnit* c_unit = down_cast<const PassMEDataHolder*>(data)->c_unit;
    DCHECK(c_unit != nullptr);
    c_unit->mir_graph->BasicBlockOptimizationEnd();
    down_cast<PassMEDataHolder*>(data)->dirty = !c_unit->mir_graph->DfsOrdersUpToDate();
  }
};

/**
 * @class SuspendCheckElimination
 * @brief Any simple BasicBlock optimization can be put here.
 */
class SuspendCheckElimination : public PassME {
 public:
  SuspendCheckElimination()
    : PassME("SuspendCheckElimination", kTopologicalSortTraversal, "6_post_sce_cfg") {
  }

  bool Gate(const PassDataHolder* data) const {
    DCHECK(data != nullptr);
    CompilationUnit* c_unit = down_cast<const PassMEDataHolder*>(data)->c_unit;
    DCHECK(c_unit != nullptr);
    return c_unit->mir_graph->EliminateSuspendChecksGate();
  }

  bool Worker(PassDataHolder* data) const {
    DCHECK(data != nullptr);
    PassMEDataHolder* pass_me_data_holder = down_cast<PassMEDataHolder*>(data);
    CompilationUnit* c_unit = pass_me_data_holder->c_unit;
    DCHECK(c_unit != nullptr);
    BasicBlock* bb = pass_me_data_holder->bb;
    DCHECK(bb != nullptr);
    return c_unit->mir_graph->EliminateSuspendChecks(bb);
  }
};

}  // namespace art

#endif  // ART_COMPILER_DEX_BB_OPTIMIZATIONS_H_
