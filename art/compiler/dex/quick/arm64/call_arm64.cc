/*
 * Copyright (C) 2011 The Android Open Source Project
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

/* This file contains codegen for the Thumb2 ISA. */

#include "codegen_arm64.h"

#include "arm64_lir.h"
#include "art_method.h"
#include "base/logging.h"
#include "dex/mir_graph.h"
#include "dex/quick/dex_file_to_method_inliner_map.h"
#include "dex/quick/mir_to_lir-inl.h"
#include "driver/compiler_driver.h"
#include "driver/compiler_options.h"
#include "gc/accounting/card_table.h"
#include "entrypoints/quick/quick_entrypoints.h"
#include "mirror/object_array-inl.h"
#include "utils/dex_cache_arrays_layout-inl.h"

namespace art {

/*
 * The sparse table in the literal pool is an array of <key,displacement>
 * pairs.  For each set, we'll load them as a pair using ldp.
 * The test loop will look something like:
 *
 *   adr   r_base, <table>
 *   ldr   r_val, [rA64_SP, v_reg_off]
 *   mov   r_idx, #table_size
 * loop:
 *   cbz   r_idx, quit
 *   ldp   r_key, r_disp, [r_base], #8
 *   sub   r_idx, #1
 *   cmp   r_val, r_key
 *   b.ne  loop
 *   adr   r_base, #0        ; This is the instruction from which we compute displacements
 *   add   r_base, r_disp
 *   br    r_base
 * quit:
 */
void Arm64Mir2Lir::GenLargeSparseSwitch(MIR* mir, uint32_t table_offset, RegLocation rl_src) {
  const uint16_t* table = mir_graph_->GetTable(mir, table_offset);
  // Add the table to the list - we'll process it later
  SwitchTable *tab_rec =
      static_cast<SwitchTable*>(arena_->Alloc(sizeof(SwitchTable), kArenaAllocData));
  tab_rec->switch_mir = mir;
  tab_rec->table = table;
  tab_rec->vaddr = current_dalvik_offset_;
  uint32_t size = table[1];
  switch_tables_.push_back(tab_rec);

  // Get the switch value
  rl_src = LoadValue(rl_src, kCoreReg);
  RegStorage r_base = AllocTempWide();
  // Allocate key and disp temps.
  RegStorage r_key = AllocTemp();
  RegStorage r_disp = AllocTemp();
  // Materialize a pointer to the switch table
  NewLIR3(kA64Adr2xd, r_base.GetReg(), 0, WrapPointer(tab_rec));
  // Set up r_idx
  RegStorage r_idx = AllocTemp();
  LoadConstant(r_idx, size);

  // Entry of loop.
  LIR* loop_entry = NewLIR0(kPseudoTargetLabel);
  LIR* branch_out = NewLIR2(kA64Cbz2rt, r_idx.GetReg(), 0);

  // Load next key/disp.
  NewLIR4(kA64LdpPost4rrXD, r_key.GetReg(), r_disp.GetReg(), r_base.GetReg(), 2);
  OpRegRegImm(kOpSub, r_idx, r_idx, 1);

  // Go to next case, if key does not match.
  OpRegReg(kOpCmp, r_key, rl_src.reg);
  OpCondBranch(kCondNe, loop_entry);

  // Key does match: branch to case label.
  LIR* switch_label = NewLIR3(kA64Adr2xd, r_base.GetReg(), 0, -1);
  tab_rec->anchor = switch_label;

  // Add displacement to base branch address and go!
  OpRegRegRegExtend(kOpAdd, r_base, r_base, As64BitReg(r_disp), kA64Sxtw, 0U);
  NewLIR1(kA64Br1x, r_base.GetReg());

  // Loop exit label.
  LIR* loop_exit = NewLIR0(kPseudoTargetLabel);
  branch_out->target = loop_exit;
}


void Arm64Mir2Lir::GenLargePackedSwitch(MIR* mir, uint32_t table_offset, RegLocation rl_src) {
  const uint16_t* table = mir_graph_->GetTable(mir, table_offset);
  // Add the table to the list - we'll process it later
  SwitchTable *tab_rec =
      static_cast<SwitchTable*>(arena_->Alloc(sizeof(SwitchTable),  kArenaAllocData));
  tab_rec->switch_mir = mir;
  tab_rec->table = table;
  tab_rec->vaddr = current_dalvik_offset_;
  uint32_t size = table[1];
  switch_tables_.push_back(tab_rec);

  // Get the switch value
  rl_src = LoadValue(rl_src, kCoreReg);
  RegStorage table_base = AllocTempWide();
  // Materialize a pointer to the switch table
  NewLIR3(kA64Adr2xd, table_base.GetReg(), 0, WrapPointer(tab_rec));
  int low_key = s4FromSwitchData(&table[2]);
  RegStorage key_reg;
  // Remove the bias, if necessary
  if (low_key == 0) {
    key_reg = rl_src.reg;
  } else {
    key_reg = AllocTemp();
    OpRegRegImm(kOpSub, key_reg, rl_src.reg, low_key);
  }
  // Bounds check - if < 0 or >= size continue following switch
  OpRegImm(kOpCmp, key_reg, size - 1);
  LIR* branch_over = OpCondBranch(kCondHi, nullptr);

  // Load the displacement from the switch table
  RegStorage disp_reg = AllocTemp();
  LoadBaseIndexed(table_base, As64BitReg(key_reg), disp_reg, 2, k32);

  // Get base branch address.
  RegStorage branch_reg = AllocTempWide();
  LIR* switch_label = NewLIR3(kA64Adr2xd, branch_reg.GetReg(), 0, -1);
  tab_rec->anchor = switch_label;

  // Add displacement to base branch address and go!
  OpRegRegRegExtend(kOpAdd, branch_reg, branch_reg, As64BitReg(disp_reg), kA64Sxtw, 0U);
  NewLIR1(kA64Br1x, branch_reg.GetReg());

  // branch_over target here
  LIR* target = NewLIR0(kPseudoTargetLabel);
  branch_over->target = target;
}

/*
 * Handle unlocked -> thin locked transition inline or else call out to quick entrypoint. For more
 * details see monitor.cc.
 */
void Arm64Mir2Lir::GenMonitorEnter(int opt_flags, RegLocation rl_src) {
  // x0/w0 = object
  // w1    = thin lock thread id
  // x2    = address of lock word
  // w3    = lock word / store failure
  // TUNING: How much performance we get when we inline this?
  // Since we've already flush all register.
  FlushAllRegs();
  LoadValueDirectFixed(rl_src, rs_x0);  // = TargetReg(kArg0, kRef)
  LockCallTemps();  // Prepare for explicit register usage
  LIR* null_check_branch = nullptr;
  if ((opt_flags & MIR_IGNORE_NULL_CHECK) && !(cu_->disable_opt & (1 << kNullCheckElimination))) {
    null_check_branch = nullptr;  // No null check.
  } else {
    // If the null-check fails its handled by the slow-path to reduce exception related meta-data.
    if (!cu_->compiler_driver->GetCompilerOptions().GetImplicitNullChecks()) {
      null_check_branch = OpCmpImmBranch(kCondEq, rs_x0, 0, nullptr);
    }
  }
  Load32Disp(rs_xSELF, Thread::ThinLockIdOffset<8>().Int32Value(), rs_w1);
  OpRegRegImm(kOpAdd, rs_x2, rs_x0, mirror::Object::MonitorOffset().Int32Value());
  NewLIR2(kA64Ldxr2rX, rw3, rx2);
  MarkPossibleNullPointerException(opt_flags);
  // Zero out the read barrier bits.
  OpRegRegImm(kOpAnd, rs_w2, rs_w3, LockWord::kReadBarrierStateMaskShiftedToggled);
  LIR* not_unlocked_branch = OpCmpImmBranch(kCondNe, rs_w2, 0, nullptr);
  // w3 is zero except for the rb bits here. Copy the read barrier bits into w1.
  OpRegRegReg(kOpOr, rs_w1, rs_w1, rs_w3);
  OpRegRegImm(kOpAdd, rs_x2, rs_x0, mirror::Object::MonitorOffset().Int32Value());
  NewLIR3(kA64Stxr3wrX, rw3, rw1, rx2);
  LIR* lock_success_branch = OpCmpImmBranch(kCondEq, rs_w3, 0, nullptr);

  LIR* slow_path_target = NewLIR0(kPseudoTargetLabel);
  not_unlocked_branch->target = slow_path_target;
  if (null_check_branch != nullptr) {
    null_check_branch->target = slow_path_target;
  }
  // TODO: move to a slow path.
  // Go expensive route - artLockObjectFromCode(obj);
  LoadWordDisp(rs_xSELF, QUICK_ENTRYPOINT_OFFSET(8, pLockObject).Int32Value(), rs_xLR);
  ClobberCallerSave();
  LIR* call_inst = OpReg(kOpBlx, rs_xLR);
  MarkSafepointPC(call_inst);

  LIR* success_target = NewLIR0(kPseudoTargetLabel);
  lock_success_branch->target = success_target;
  GenMemBarrier(kLoadAny);
}

/*
 * Handle thin locked -> unlocked transition inline or else call out to quick entrypoint. For more
 * details see monitor.cc. Note the code below doesn't use ldxr/stxr as the code holds the lock
 * and can only give away ownership if its suspended.
 */
void Arm64Mir2Lir::GenMonitorExit(int opt_flags, RegLocation rl_src) {
  // x0/w0 = object
  // w1    = thin lock thread id
  // w2    = lock word
  // TUNING: How much performance we get when we inline this?
  // Since we've already flush all register.
  FlushAllRegs();
  LoadValueDirectFixed(rl_src, rs_x0);  // Get obj
  LockCallTemps();  // Prepare for explicit register usage
  LIR* null_check_branch = nullptr;
  if ((opt_flags & MIR_IGNORE_NULL_CHECK) && !(cu_->disable_opt & (1 << kNullCheckElimination))) {
    null_check_branch = nullptr;  // No null check.
  } else {
    // If the null-check fails its handled by the slow-path to reduce exception related meta-data.
    if (!cu_->compiler_driver->GetCompilerOptions().GetImplicitNullChecks()) {
      null_check_branch = OpCmpImmBranch(kCondEq, rs_x0, 0, nullptr);
    }
  }
  Load32Disp(rs_xSELF, Thread::ThinLockIdOffset<8>().Int32Value(), rs_w1);
  if (!kUseReadBarrier) {
    Load32Disp(rs_x0, mirror::Object::MonitorOffset().Int32Value(), rs_w2);
  } else {
    OpRegRegImm(kOpAdd, rs_x3, rs_x0, mirror::Object::MonitorOffset().Int32Value());
    NewLIR2(kA64Ldxr2rX, rw2, rx3);
  }
  MarkPossibleNullPointerException(opt_flags);
  // Zero out the read barrier bits.
  OpRegRegImm(kOpAnd, rs_w3, rs_w2, LockWord::kReadBarrierStateMaskShiftedToggled);
  // Zero out except the read barrier bits.
  OpRegRegImm(kOpAnd, rs_w2, rs_w2, LockWord::kReadBarrierStateMaskShifted);
  LIR* slow_unlock_branch = OpCmpBranch(kCondNe, rs_w3, rs_w1, nullptr);
  GenMemBarrier(kAnyStore);
  LIR* unlock_success_branch;
  if (!kUseReadBarrier) {
    Store32Disp(rs_x0, mirror::Object::MonitorOffset().Int32Value(), rs_w2);
    unlock_success_branch = OpUnconditionalBranch(nullptr);
  } else {
    OpRegRegImm(kOpAdd, rs_x3, rs_x0, mirror::Object::MonitorOffset().Int32Value());
    NewLIR3(kA64Stxr3wrX, rw1, rw2, rx3);
    unlock_success_branch = OpCmpImmBranch(kCondEq, rs_w1, 0, nullptr);
  }
  LIR* slow_path_target = NewLIR0(kPseudoTargetLabel);
  slow_unlock_branch->target = slow_path_target;
  if (null_check_branch != nullptr) {
    null_check_branch->target = slow_path_target;
  }
  // TODO: move to a slow path.
  // Go expensive route - artUnlockObjectFromCode(obj);
  LoadWordDisp(rs_xSELF, QUICK_ENTRYPOINT_OFFSET(8, pUnlockObject).Int32Value(), rs_xLR);
  ClobberCallerSave();
  LIR* call_inst = OpReg(kOpBlx, rs_xLR);
  MarkSafepointPC(call_inst);

  LIR* success_target = NewLIR0(kPseudoTargetLabel);
  unlock_success_branch->target = success_target;
}

void Arm64Mir2Lir::GenMoveException(RegLocation rl_dest) {
  int ex_offset = Thread::ExceptionOffset<8>().Int32Value();
  RegLocation rl_result = EvalLoc(rl_dest, kRefReg, true);
  LoadRefDisp(rs_xSELF, ex_offset, rl_result.reg, kNotVolatile);
  StoreRefDisp(rs_xSELF, ex_offset, rs_xzr, kNotVolatile);
  StoreValue(rl_dest, rl_result);
}

void Arm64Mir2Lir::UnconditionallyMarkGCCard(RegStorage tgt_addr_reg) {
  RegStorage reg_card_base = AllocTempWide();
  RegStorage reg_card_no = AllocTempWide();  // Needs to be wide as addr is ref=64b
  LoadWordDisp(rs_xSELF, Thread::CardTableOffset<8>().Int32Value(), reg_card_base);
  OpRegRegImm(kOpLsr, reg_card_no, tgt_addr_reg, gc::accounting::CardTable::kCardShift);
  // TODO(Arm64): generate "strb wB, [xB, wC, uxtw]" rather than "strb wB, [xB, xC]"?
  StoreBaseIndexed(reg_card_base, reg_card_no, As32BitReg(reg_card_base),
                   0, kUnsignedByte);
  FreeTemp(reg_card_base);
  FreeTemp(reg_card_no);
}

static dwarf::Reg DwarfCoreReg(int num) {
  return dwarf::Reg::Arm64Core(num);
}

void Arm64Mir2Lir::GenEntrySequence(RegLocation* ArgLocs, RegLocation rl_method) {
  DCHECK_EQ(cfi_.GetCurrentCFAOffset(), 0);  // empty stack.

  /*
   * On entry, x0 to x7 are live.  Let the register allocation
   * mechanism know so it doesn't try to use any of them when
   * expanding the frame or flushing.
   * Reserve x8 & x9 for temporaries.
   */
  LockTemp(rs_x0);
  LockTemp(rs_x1);
  LockTemp(rs_x2);
  LockTemp(rs_x3);
  LockTemp(rs_x4);
  LockTemp(rs_x5);
  LockTemp(rs_x6);
  LockTemp(rs_x7);
  LockTemp(rs_xIP0);
  LockTemp(rs_xIP1);

  /* TUNING:
   * Use AllocTemp() and reuse LR if possible to give us the freedom on adjusting the number
   * of temp registers.
   */

  /*
   * We can safely skip the stack overflow check if we're
   * a leaf *and* our frame size < fudge factor.
   */
  bool skip_overflow_check = mir_graph_->MethodIsLeaf() &&
    !FrameNeedsStackCheck(frame_size_, kArm64);

  const size_t kStackOverflowReservedUsableBytes = GetStackOverflowReservedBytes(kArm64);
  const bool large_frame = static_cast<size_t>(frame_size_) > kStackOverflowReservedUsableBytes;
  bool generate_explicit_stack_overflow_check = large_frame ||
    !cu_->compiler_driver->GetCompilerOptions().GetImplicitStackOverflowChecks();
  const int spill_count = num_core_spills_ + num_fp_spills_;
  const int spill_size = (spill_count * kArm64PointerSize + 15) & ~0xf;  // SP 16 byte alignment.
  const int frame_size_without_spills = frame_size_ - spill_size;

  if (!skip_overflow_check) {
    if (generate_explicit_stack_overflow_check) {
      // Load stack limit
      LoadWordDisp(rs_xSELF, Thread::StackEndOffset<8>().Int32Value(), rs_xIP1);
    } else {
      // Implicit stack overflow check.
      // Generate a load from [sp, #-framesize].  If this is in the stack
      // redzone we will get a segmentation fault.

      // TODO: If the frame size is small enough, is it possible to make this a pre-indexed load,
      //       so that we can avoid the following "sub sp" when spilling?
      OpRegRegImm(kOpSub, rs_x8, rs_sp, GetStackOverflowReservedBytes(kArm64));
      Load32Disp(rs_x8, 0, rs_wzr);
      MarkPossibleStackOverflowException();
    }
  }

  int spilled_already = 0;
  if (spill_size > 0) {
    spilled_already = SpillRegs(rs_sp, core_spill_mask_, fp_spill_mask_, frame_size_);
    DCHECK(spill_size == spilled_already || frame_size_ == spilled_already);
  }

  if (spilled_already != frame_size_) {
    OpRegImm(kOpSub, rs_sp, frame_size_without_spills);
    cfi_.AdjustCFAOffset(frame_size_without_spills);
  }

  if (!skip_overflow_check) {
    if (generate_explicit_stack_overflow_check) {
      class StackOverflowSlowPath: public LIRSlowPath {
      public:
        StackOverflowSlowPath(Mir2Lir* m2l, LIR* branch, size_t sp_displace)
            : LIRSlowPath(m2l, branch),
              sp_displace_(sp_displace) {
        }
        void Compile() OVERRIDE {
          m2l_->ResetRegPool();
          m2l_->ResetDefTracking();
          GenerateTargetLabel(kPseudoThrowTarget);
          // Unwinds stack.
          m2l_->OpRegImm(kOpAdd, rs_sp, sp_displace_);
          m2l_->cfi().AdjustCFAOffset(-sp_displace_);
          m2l_->ClobberCallerSave();
          ThreadOffset<8> func_offset = QUICK_ENTRYPOINT_OFFSET(8, pThrowStackOverflow);
          m2l_->LockTemp(rs_xIP0);
          m2l_->LoadWordDisp(rs_xSELF, func_offset.Int32Value(), rs_xIP0);
          m2l_->NewLIR1(kA64Br1x, rs_xIP0.GetReg());
          m2l_->FreeTemp(rs_xIP0);
          m2l_->cfi().AdjustCFAOffset(sp_displace_);
        }

      private:
        const size_t sp_displace_;
      };

      LIR* branch = OpCmpBranch(kCondUlt, rs_sp, rs_xIP1, nullptr);
      AddSlowPath(new(arena_)StackOverflowSlowPath(this, branch, frame_size_));
    }
  }

  FlushIns(ArgLocs, rl_method);

  FreeTemp(rs_x0);
  FreeTemp(rs_x1);
  FreeTemp(rs_x2);
  FreeTemp(rs_x3);
  FreeTemp(rs_x4);
  FreeTemp(rs_x5);
  FreeTemp(rs_x6);
  FreeTemp(rs_x7);
  FreeTemp(rs_xIP0);
  FreeTemp(rs_xIP1);
}

void Arm64Mir2Lir::GenExitSequence() {
  cfi_.RememberState();
  /*
   * In the exit path, r0/r1 are live - make sure they aren't
   * allocated by the register utilities as temps.
   */
  LockTemp(rs_x0);
  LockTemp(rs_x1);
  UnspillRegs(rs_sp, core_spill_mask_, fp_spill_mask_, frame_size_);

  // Finally return.
  NewLIR0(kA64Ret);
  // The CFI should be restored for any code that follows the exit block.
  cfi_.RestoreState();
  cfi_.DefCFAOffset(frame_size_);
}

void Arm64Mir2Lir::GenSpecialExitSequence() {
  NewLIR0(kA64Ret);
}

void Arm64Mir2Lir::GenSpecialEntryForSuspend() {
  // Keep 16-byte stack alignment - push x0, i.e. ArtMethod*, lr.
  core_spill_mask_ = (1u << rs_xLR.GetRegNum());
  num_core_spills_ = 1u;
  fp_spill_mask_ = 0u;
  num_fp_spills_ = 0u;
  frame_size_ = 16u;
  core_vmap_table_.clear();
  fp_vmap_table_.clear();
  NewLIR4(WIDE(kA64StpPre4rrXD), rs_x0.GetReg(), rs_xLR.GetReg(), rs_sp.GetReg(), -frame_size_ / 8);
  cfi_.AdjustCFAOffset(frame_size_);
  // Do not generate CFI for scratch register x0.
  cfi_.RelOffset(DwarfCoreReg(rxLR), 8);
}

void Arm64Mir2Lir::GenSpecialExitForSuspend() {
  // Pop the frame. (ArtMethod* no longer needed but restore it anyway.)
  NewLIR4(WIDE(kA64LdpPost4rrXD), rs_x0.GetReg(), rs_xLR.GetReg(), rs_sp.GetReg(), frame_size_ / 8);
  cfi_.AdjustCFAOffset(-frame_size_);
  cfi_.Restore(DwarfCoreReg(rxLR));
}

static bool Arm64UseRelativeCall(CompilationUnit* cu, const MethodReference& target_method) {
  // Emit relative calls anywhere in the image or within a dex file otherwise.
  return cu->compiler_driver->IsImage() || cu->dex_file == target_method.dex_file;
}

/*
 * Bit of a hack here - in the absence of a real scheduling pass,
 * emit the next instruction in static & direct invoke sequences.
 */
int Arm64Mir2Lir::Arm64NextSDCallInsn(CompilationUnit* cu, CallInfo* info,
                                      int state, const MethodReference& target_method,
                                      uint32_t unused_idx ATTRIBUTE_UNUSED,
                                      uintptr_t direct_code, uintptr_t direct_method,
                                      InvokeType type) {
  Arm64Mir2Lir* cg = static_cast<Arm64Mir2Lir*>(cu->cg.get());
  if (info->string_init_offset != 0) {
    RegStorage arg0_ref = cg->TargetReg(kArg0, kRef);
    switch (state) {
    case 0: {  // Grab target method* from thread pointer
      cg->LoadWordDisp(rs_xSELF, info->string_init_offset, arg0_ref);
      break;
    }
    case 1:  // Grab the code from the method*
      if (direct_code == 0) {
        // kInvokeTgt := arg0_ref->entrypoint
        cg->LoadWordDisp(arg0_ref,
                         ArtMethod::EntryPointFromQuickCompiledCodeOffset(
                             kArm64PointerSize).Int32Value(), cg->TargetPtrReg(kInvokeTgt));
      }
      break;
    default:
      return -1;
    }
  } else if (direct_code != 0 && direct_method != 0) {
    switch (state) {
    case 0:  // Get the current Method* [sets kArg0]
      if (direct_code != static_cast<uintptr_t>(-1)) {
        cg->LoadConstantWide(cg->TargetPtrReg(kInvokeTgt), direct_code);
      } else if (Arm64UseRelativeCall(cu, target_method)) {
        // Defer to linker patch.
      } else {
        cg->LoadCodeAddress(target_method, type, kInvokeTgt);
      }
      if (direct_method != static_cast<uintptr_t>(-1)) {
        cg->LoadConstantWide(cg->TargetReg(kArg0, kRef), direct_method);
      } else {
        cg->LoadMethodAddress(target_method, type, kArg0);
      }
      break;
    default:
      return -1;
    }
  } else {
    bool use_pc_rel = cg->CanUseOpPcRelDexCacheArrayLoad();
    RegStorage arg0_ref = cg->TargetPtrReg(kArg0);
    switch (state) {
    case 0:  // Get the current Method* [sets kArg0]
      // TUNING: we can save a reg copy if Method* has been promoted.
      if (!use_pc_rel) {
        cg->LoadCurrMethodDirect(arg0_ref);
        break;
      }
      ++state;
      FALLTHROUGH_INTENDED;
    case 1:  // Get method->dex_cache_resolved_methods_
      if (!use_pc_rel) {
        cg->LoadRefDisp(arg0_ref,
                        ArtMethod::DexCacheResolvedMethodsOffset().Int32Value(),
                        arg0_ref,
                        kNotVolatile);
      }
      // Set up direct code if known.
      if (direct_code != 0) {
        if (direct_code != static_cast<uintptr_t>(-1)) {
          cg->LoadConstantWide(cg->TargetPtrReg(kInvokeTgt), direct_code);
        } else if (Arm64UseRelativeCall(cu, target_method)) {
          // Defer to linker patch.
        } else {
          CHECK_LT(target_method.dex_method_index, target_method.dex_file->NumMethodIds());
          cg->LoadCodeAddress(target_method, type, kInvokeTgt);
        }
      }
      if (!use_pc_rel || direct_code != 0) {
        break;
      }
      ++state;
      FALLTHROUGH_INTENDED;
    case 2:  // Grab target method*
      CHECK_EQ(cu->dex_file, target_method.dex_file);
      if (!use_pc_rel) {
        cg->LoadWordDisp(arg0_ref,
                         mirror::Array::DataOffset(kArm64PointerSize).Uint32Value() +
                         target_method.dex_method_index * kArm64PointerSize, arg0_ref);
      } else {
        size_t offset = cg->dex_cache_arrays_layout_.MethodOffset(target_method.dex_method_index);
        cg->OpPcRelDexCacheArrayLoad(cu->dex_file, offset, arg0_ref, true);
      }
      break;
    case 3:  // Grab the code from the method*
      if (direct_code == 0) {
        // kInvokeTgt := arg0_ref->entrypoint
        cg->LoadWordDisp(arg0_ref,
                         ArtMethod::EntryPointFromQuickCompiledCodeOffset(
                             kArm64PointerSize).Int32Value(), cg->TargetPtrReg(kInvokeTgt));
      }
      break;
    default:
      return -1;
    }
  }
  return state + 1;
}

NextCallInsn Arm64Mir2Lir::GetNextSDCallInsn() {
  return Arm64NextSDCallInsn;
}

LIR* Arm64Mir2Lir::CallWithLinkerFixup(const MethodReference& target_method, InvokeType type) {
  // For ARM64, just generate a relative BL instruction that will be filled in at 'link time'.
  // If the target turns out to be too far, the linker will generate a thunk for dispatch.
  int target_method_idx = target_method.dex_method_index;
  const DexFile* target_dex_file = target_method.dex_file;

  // Generate the call instruction and save index, dex_file, and type.
  // NOTE: Method deduplication takes linker patches into account, so we can just pass 0
  // as a placeholder for the offset.
  LIR* call = RawLIR(current_dalvik_offset_, kA64Bl1t, 0,
                     target_method_idx, WrapPointer(target_dex_file), type);
  AppendLIR(call);
  call_method_insns_.push_back(call);
  return call;
}

LIR* Arm64Mir2Lir::GenCallInsn(const MirMethodLoweringInfo& method_info) {
  LIR* call_insn;
  if (method_info.FastPath() && Arm64UseRelativeCall(cu_, method_info.GetTargetMethod()) &&
      (method_info.GetSharpType() == kDirect || method_info.GetSharpType() == kStatic) &&
      method_info.DirectCode() == static_cast<uintptr_t>(-1)) {
    call_insn = CallWithLinkerFixup(method_info.GetTargetMethod(), method_info.GetSharpType());
  } else {
    call_insn = OpReg(kOpBlx, TargetPtrReg(kInvokeTgt));
  }
  return call_insn;
}

}  // namespace art
