/*
 * Copyright (C) 2012 The Android Open Source Project
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

#include "mir_to_lir-inl.h"

#include <functional>

#include "arch/arm/instruction_set_features_arm.h"
#include "base/bit_utils.h"
#include "base/macros.h"
#include "dex/compiler_ir.h"
#include "dex/mir_graph.h"
#include "dex/quick/arm/arm_lir.h"
#include "driver/compiler_driver.h"
#include "driver/compiler_options.h"
#include "entrypoints/quick/quick_entrypoints.h"
#include "mirror/array.h"
#include "mirror/object_array-inl.h"
#include "mirror/object-inl.h"
#include "mirror/object_reference.h"
#include "utils/dex_cache_arrays_layout-inl.h"
#include "verifier/method_verifier.h"

namespace art {

// Shortcuts to repeatedly used long types.
typedef mirror::ObjectArray<mirror::Object> ObjArray;
typedef mirror::ObjectArray<mirror::Class> ClassArray;

/*
 * This source files contains "gen" codegen routines that should
 * be applicable to most targets.  Only mid-level support utilities
 * and "op" calls may be used here.
 */

ALWAYS_INLINE static inline bool ForceSlowFieldPath(CompilationUnit* cu) {
  return (cu->enable_debug & (1 << kDebugSlowFieldPath)) != 0;
}

ALWAYS_INLINE static inline bool ForceSlowStringPath(CompilationUnit* cu) {
  return (cu->enable_debug & (1 << kDebugSlowStringPath)) != 0;
}

ALWAYS_INLINE static inline bool ForceSlowTypePath(CompilationUnit* cu) {
  return (cu->enable_debug & (1 << kDebugSlowTypePath)) != 0;
}

void Mir2Lir::GenIfNullUseHelperImm(RegStorage r_result, QuickEntrypointEnum trampoline, int imm) {
  class CallHelperImmMethodSlowPath : public LIRSlowPath {
   public:
    CallHelperImmMethodSlowPath(Mir2Lir* m2l, LIR* fromfast, LIR* cont,
                                QuickEntrypointEnum trampoline_in, int imm_in,
                                RegStorage r_result_in)
        : LIRSlowPath(m2l, fromfast, cont), trampoline_(trampoline_in),
          imm_(imm_in), r_result_(r_result_in) {
    }

    void Compile() {
      GenerateTargetLabel();
      m2l_->CallRuntimeHelperImm(trampoline_, imm_, true);
      m2l_->OpRegCopy(r_result_,  m2l_->TargetReg(kRet0, kRef));
      m2l_->OpUnconditionalBranch(cont_);
    }

   private:
    QuickEntrypointEnum trampoline_;
    const int imm_;
    const RegStorage r_result_;
  };

  LIR* branch = OpCmpImmBranch(kCondEq, r_result, 0, nullptr);
  LIR* cont = NewLIR0(kPseudoTargetLabel);

  AddSlowPath(new (arena_) CallHelperImmMethodSlowPath(this, branch, cont, trampoline, imm,
                                                       r_result));
}

RegStorage Mir2Lir::GenGetOtherTypeForSgetSput(const MirSFieldLoweringInfo& field_info,
                                               int opt_flags) {
  DCHECK_NE(field_info.StorageIndex(), DexFile::kDexNoIndex);
  // May do runtime call so everything to home locations.
  FlushAllRegs();
  RegStorage r_base = TargetReg(kArg0, kRef);
  LockTemp(r_base);
  if (CanUseOpPcRelDexCacheArrayLoad()) {
    uint32_t offset = dex_cache_arrays_layout_.TypeOffset(field_info.StorageIndex());
    OpPcRelDexCacheArrayLoad(cu_->dex_file, offset, r_base, false);
  } else {
    // Using fixed register to sync with possible call to runtime support.
    RegStorage r_method = LoadCurrMethodWithHint(r_base);
    LoadRefDisp(r_method, ArtMethod::DexCacheResolvedTypesOffset().Int32Value(), r_base,
                kNotVolatile);
    int32_t offset_of_field = ObjArray::OffsetOfElement(field_info.StorageIndex()).Int32Value();
    LoadRefDisp(r_base, offset_of_field, r_base, kNotVolatile);
  }
  // r_base now points at static storage (Class*) or null if the type is not yet resolved.
  LIR* unresolved_branch = nullptr;
  if (!field_info.IsClassInDexCache() && (opt_flags & MIR_CLASS_IS_IN_DEX_CACHE) == 0) {
    // Check if r_base is null.
    unresolved_branch = OpCmpImmBranch(kCondEq, r_base, 0, nullptr);
  }
  LIR* uninit_branch = nullptr;
  if (!field_info.IsClassInitialized() && (opt_flags & MIR_CLASS_IS_INITIALIZED) == 0) {
    // Check if r_base is not yet initialized class.
    RegStorage r_tmp = TargetReg(kArg2, kNotWide);
    LockTemp(r_tmp);
    uninit_branch = OpCmpMemImmBranch(kCondLt, r_tmp, r_base,
                                      mirror::Class::StatusOffset().Int32Value(),
                                      mirror::Class::kStatusInitialized, nullptr, nullptr);
    FreeTemp(r_tmp);
  }
  if (unresolved_branch != nullptr || uninit_branch != nullptr) {
    //
    // Slow path to ensure a class is initialized for sget/sput.
    //
    class StaticFieldSlowPath : public Mir2Lir::LIRSlowPath {
     public:
      // There are up to two branches to the static field slow path, the "unresolved" when the type
      // entry in the dex cache is null, and the "uninit" when the class is not yet initialized.
      // At least one will be non-null here, otherwise we wouldn't generate the slow path.
      StaticFieldSlowPath(Mir2Lir* m2l, LIR* unresolved, LIR* uninit, LIR* cont, int storage_index,
                          RegStorage r_base_in)
          : LIRSlowPath(m2l, unresolved != nullptr ? unresolved : uninit, cont),
            second_branch_(unresolved != nullptr ? uninit : nullptr),
            storage_index_(storage_index), r_base_(r_base_in) {
      }

      void Compile() {
        LIR* target = GenerateTargetLabel();
        if (second_branch_ != nullptr) {
          second_branch_->target = target;
        }
        m2l_->CallRuntimeHelperImm(kQuickInitializeStaticStorage, storage_index_, true);
        // Copy helper's result into r_base, a no-op on all but MIPS.
        m2l_->OpRegCopy(r_base_,  m2l_->TargetReg(kRet0, kRef));

        m2l_->OpUnconditionalBranch(cont_);
      }

     private:
      // Second branch to the slow path, or null if there's only one branch.
      LIR* const second_branch_;

      const int storage_index_;
      const RegStorage r_base_;
    };

    // The slow path is invoked if the r_base is null or the class pointed
    // to by it is not initialized.
    LIR* cont = NewLIR0(kPseudoTargetLabel);
    AddSlowPath(new (arena_) StaticFieldSlowPath(this, unresolved_branch, uninit_branch, cont,
                                                 field_info.StorageIndex(), r_base));
  }
  return r_base;
}

/*
 * Generate a kPseudoBarrier marker to indicate the boundary of special
 * blocks.
 */
void Mir2Lir::GenBarrier() {
  LIR* barrier = NewLIR0(kPseudoBarrier);
  /* Mark all resources as being clobbered */
  DCHECK(!barrier->flags.use_def_invalid);
  barrier->u.m.def_mask = &kEncodeAll;
}

void Mir2Lir::GenDivZeroException() {
  LIR* branch = OpUnconditionalBranch(nullptr);
  AddDivZeroCheckSlowPath(branch);
}

void Mir2Lir::GenDivZeroCheck(ConditionCode c_code) {
  LIR* branch = OpCondBranch(c_code, nullptr);
  AddDivZeroCheckSlowPath(branch);
}

void Mir2Lir::GenDivZeroCheck(RegStorage reg) {
  LIR* branch = OpCmpImmBranch(kCondEq, reg, 0, nullptr);
  AddDivZeroCheckSlowPath(branch);
}

void Mir2Lir::AddDivZeroCheckSlowPath(LIR* branch) {
  class DivZeroCheckSlowPath : public Mir2Lir::LIRSlowPath {
   public:
    DivZeroCheckSlowPath(Mir2Lir* m2l, LIR* branch_in)
        : LIRSlowPath(m2l, branch_in) {
    }

    void Compile() OVERRIDE {
      m2l_->ResetRegPool();
      m2l_->ResetDefTracking();
      GenerateTargetLabel(kPseudoThrowTarget);
      m2l_->CallRuntimeHelper(kQuickThrowDivZero, true);
    }
  };

  AddSlowPath(new (arena_) DivZeroCheckSlowPath(this, branch));
}

void Mir2Lir::GenArrayBoundsCheck(RegStorage index, RegStorage length) {
  class ArrayBoundsCheckSlowPath : public Mir2Lir::LIRSlowPath {
   public:
    ArrayBoundsCheckSlowPath(Mir2Lir* m2l, LIR* branch_in, RegStorage index_in,
                             RegStorage length_in)
        : LIRSlowPath(m2l, branch_in),
          index_(index_in), length_(length_in) {
    }

    void Compile() OVERRIDE {
      m2l_->ResetRegPool();
      m2l_->ResetDefTracking();
      GenerateTargetLabel(kPseudoThrowTarget);
      m2l_->CallRuntimeHelperRegReg(kQuickThrowArrayBounds, index_, length_, true);
    }

   private:
    const RegStorage index_;
    const RegStorage length_;
  };

  LIR* branch = OpCmpBranch(kCondUge, index, length, nullptr);
  AddSlowPath(new (arena_) ArrayBoundsCheckSlowPath(this, branch, index, length));
}

void Mir2Lir::GenArrayBoundsCheck(int index, RegStorage length) {
  class ArrayBoundsCheckSlowPath : public Mir2Lir::LIRSlowPath {
   public:
    ArrayBoundsCheckSlowPath(Mir2Lir* m2l, LIR* branch_in, int index_in, RegStorage length_in)
        : LIRSlowPath(m2l, branch_in),
          index_(index_in), length_(length_in) {
    }

    void Compile() OVERRIDE {
      m2l_->ResetRegPool();
      m2l_->ResetDefTracking();
      GenerateTargetLabel(kPseudoThrowTarget);

      RegStorage arg1_32 = m2l_->TargetReg(kArg1, kNotWide);
      RegStorage arg0_32 = m2l_->TargetReg(kArg0, kNotWide);

      m2l_->OpRegCopy(arg1_32, length_);
      m2l_->LoadConstant(arg0_32, index_);
      m2l_->CallRuntimeHelperRegReg(kQuickThrowArrayBounds, arg0_32, arg1_32, true);
    }

   private:
    const int32_t index_;
    const RegStorage length_;
  };

  LIR* branch = OpCmpImmBranch(kCondLs, length, index, nullptr);
  AddSlowPath(new (arena_) ArrayBoundsCheckSlowPath(this, branch, index, length));
}

LIR* Mir2Lir::GenNullCheck(RegStorage reg) {
  class NullCheckSlowPath : public Mir2Lir::LIRSlowPath {
   public:
    NullCheckSlowPath(Mir2Lir* m2l, LIR* branch)
        : LIRSlowPath(m2l, branch) {
    }

    void Compile() OVERRIDE {
      m2l_->ResetRegPool();
      m2l_->ResetDefTracking();
      GenerateTargetLabel(kPseudoThrowTarget);
      m2l_->CallRuntimeHelper(kQuickThrowNullPointer, true);
    }
  };

  LIR* branch = OpCmpImmBranch(kCondEq, reg, 0, nullptr);
  AddSlowPath(new (arena_) NullCheckSlowPath(this, branch));
  return branch;
}

/* Perform null-check on a register.  */
LIR* Mir2Lir::GenNullCheck(RegStorage m_reg, int opt_flags) {
  if (!cu_->compiler_driver->GetCompilerOptions().GetImplicitNullChecks()) {
    return GenExplicitNullCheck(m_reg, opt_flags);
  }
  // If null check has not been eliminated, reset redundant store tracking.
  if ((opt_flags & MIR_IGNORE_NULL_CHECK) == 0) {
    ResetDefTracking();
  }
  return nullptr;
}

/* Perform an explicit null-check on a register.  */
LIR* Mir2Lir::GenExplicitNullCheck(RegStorage m_reg, int opt_flags) {
  if (!(cu_->disable_opt & (1 << kNullCheckElimination)) && (opt_flags & MIR_IGNORE_NULL_CHECK)) {
    return nullptr;
  }
  return GenNullCheck(m_reg);
}

void Mir2Lir::MarkPossibleNullPointerException(int opt_flags) {
  if (cu_->compiler_driver->GetCompilerOptions().GetImplicitNullChecks()) {
    if (!(cu_->disable_opt & (1 << kNullCheckElimination)) && (opt_flags & MIR_IGNORE_NULL_CHECK)) {
      return;
    }
    // Insert after last instruction.
    MarkSafepointPC(last_lir_insn_);
  }
}

void Mir2Lir::MarkPossibleNullPointerExceptionAfter(int opt_flags, LIR* after) {
  if (cu_->compiler_driver->GetCompilerOptions().GetImplicitNullChecks()) {
    if (!(cu_->disable_opt & (1 << kNullCheckElimination)) && (opt_flags & MIR_IGNORE_NULL_CHECK)) {
      return;
    }
    MarkSafepointPCAfter(after);
  }
}

void Mir2Lir::MarkPossibleStackOverflowException() {
  if (cu_->compiler_driver->GetCompilerOptions().GetImplicitStackOverflowChecks()) {
    MarkSafepointPC(last_lir_insn_);
  }
}

void Mir2Lir::ForceImplicitNullCheck(RegStorage reg, int opt_flags) {
  if (cu_->compiler_driver->GetCompilerOptions().GetImplicitNullChecks()) {
    if (!(cu_->disable_opt & (1 << kNullCheckElimination)) && (opt_flags & MIR_IGNORE_NULL_CHECK)) {
      return;
    }
    // Force an implicit null check by performing a memory operation (load) from the given
    // register with offset 0.  This will cause a signal if the register contains 0 (null).
    RegStorage tmp = AllocTemp();
    // TODO: for Mips, would be best to use rZERO as the bogus register target.
    LIR* load = Load32Disp(reg, 0, tmp);
    FreeTemp(tmp);
    MarkSafepointPC(load);
  }
}

void Mir2Lir::GenCompareAndBranch(Instruction::Code opcode, RegLocation rl_src1,
                                  RegLocation rl_src2, LIR* taken) {
  ConditionCode cond;
  RegisterClass reg_class = (rl_src1.ref || rl_src2.ref) ? kRefReg : kCoreReg;
  switch (opcode) {
    case Instruction::IF_EQ:
      cond = kCondEq;
      break;
    case Instruction::IF_NE:
      cond = kCondNe;
      break;
    case Instruction::IF_LT:
      cond = kCondLt;
      break;
    case Instruction::IF_GE:
      cond = kCondGe;
      break;
    case Instruction::IF_GT:
      cond = kCondGt;
      break;
    case Instruction::IF_LE:
      cond = kCondLe;
      break;
    default:
      cond = static_cast<ConditionCode>(0);
      LOG(FATAL) << "Unexpected opcode " << opcode;
  }

  // Normalize such that if either operand is constant, src2 will be constant
  if (rl_src1.is_const) {
    RegLocation rl_temp = rl_src1;
    rl_src1 = rl_src2;
    rl_src2 = rl_temp;
    cond = FlipComparisonOrder(cond);
  }

  rl_src1 = LoadValue(rl_src1, reg_class);
  // Is this really an immediate comparison?
  if (rl_src2.is_const) {
    // If it's already live in a register or not easily materialized, just keep going
    RegLocation rl_temp = UpdateLoc(rl_src2);
    int32_t constant_value = mir_graph_->ConstantValue(rl_src2);
    if ((rl_temp.location == kLocDalvikFrame) &&
        InexpensiveConstantInt(constant_value, opcode)) {
      // OK - convert this to a compare immediate and branch
      OpCmpImmBranch(cond, rl_src1.reg, mir_graph_->ConstantValue(rl_src2), taken);
      return;
    }

    // It's also commonly more efficient to have a test against zero with Eq/Ne. This is not worse
    // for x86, and allows a cbz/cbnz for Arm and Mips. At the same time, it works around a register
    // mismatch for 64b systems, where a reference is compared against null, as dex bytecode uses
    // the 32b literal 0 for null.
    if (constant_value == 0 && (cond == kCondEq || cond == kCondNe)) {
      // Use the OpCmpImmBranch and ignore the value in the register.
      OpCmpImmBranch(cond, rl_src1.reg, 0, taken);
      return;
    }
  }

  rl_src2 = LoadValue(rl_src2, reg_class);
  OpCmpBranch(cond, rl_src1.reg, rl_src2.reg, taken);
}

void Mir2Lir::GenCompareZeroAndBranch(Instruction::Code opcode, RegLocation rl_src, LIR* taken) {
  ConditionCode cond;
  RegisterClass reg_class = rl_src.ref ? kRefReg : kCoreReg;
  rl_src = LoadValue(rl_src, reg_class);
  switch (opcode) {
    case Instruction::IF_EQZ:
      cond = kCondEq;
      break;
    case Instruction::IF_NEZ:
      cond = kCondNe;
      break;
    case Instruction::IF_LTZ:
      cond = kCondLt;
      break;
    case Instruction::IF_GEZ:
      cond = kCondGe;
      break;
    case Instruction::IF_GTZ:
      cond = kCondGt;
      break;
    case Instruction::IF_LEZ:
      cond = kCondLe;
      break;
    default:
      cond = static_cast<ConditionCode>(0);
      LOG(FATAL) << "Unexpected opcode " << opcode;
  }
  OpCmpImmBranch(cond, rl_src.reg, 0, taken);
}

void Mir2Lir::GenIntToLong(RegLocation rl_dest, RegLocation rl_src) {
  RegLocation rl_result = EvalLoc(rl_dest, kCoreReg, true);
  if (rl_src.location == kLocPhysReg) {
    OpRegCopy(rl_result.reg, rl_src.reg);
  } else {
    LoadValueDirect(rl_src, rl_result.reg.GetLow());
  }
  OpRegRegImm(kOpAsr, rl_result.reg.GetHigh(), rl_result.reg.GetLow(), 31);
  StoreValueWide(rl_dest, rl_result);
}

void Mir2Lir::GenLongToInt(RegLocation rl_dest, RegLocation rl_src) {
  rl_src = UpdateLocWide(rl_src);
  rl_src = NarrowRegLoc(rl_src);
  StoreValue(rl_dest, rl_src);
}

void Mir2Lir::GenIntNarrowing(Instruction::Code opcode, RegLocation rl_dest,
                              RegLocation rl_src) {
  rl_src = LoadValue(rl_src, kCoreReg);
  RegLocation rl_result = EvalLoc(rl_dest, kCoreReg, true);
  OpKind op = kOpInvalid;
  switch (opcode) {
    case Instruction::INT_TO_BYTE:
      op = kOp2Byte;
      break;
    case Instruction::INT_TO_SHORT:
       op = kOp2Short;
       break;
    case Instruction::INT_TO_CHAR:
       op = kOp2Char;
       break;
    default:
      LOG(ERROR) << "Bad int conversion type";
  }
  OpRegReg(op, rl_result.reg, rl_src.reg);
  StoreValue(rl_dest, rl_result);
}

/*
 * Let helper function take care of everything.  Will call
 * Array::AllocFromCode(type_idx, method, count);
 * Note: AllocFromCode will handle checks for errNegativeArraySize.
 */
void Mir2Lir::GenNewArray(uint32_t type_idx, RegLocation rl_dest,
                          RegLocation rl_src) {
  FlushAllRegs();  /* Everything to home location */
  const DexFile* dex_file = cu_->dex_file;
  CompilerDriver* driver = cu_->compiler_driver;
  if (cu_->compiler_driver->CanAccessTypeWithoutChecks(cu_->method_idx, *dex_file, type_idx)) {
    bool is_type_initialized;  // Ignored as an array does not have an initializer.
    bool use_direct_type_ptr;
    uintptr_t direct_type_ptr;
    bool is_finalizable;
    if (kEmbedClassInCode &&
        driver->CanEmbedTypeInCode(*dex_file, type_idx, &is_type_initialized, &use_direct_type_ptr,
                                   &direct_type_ptr, &is_finalizable)) {
      // The fast path.
      if (!use_direct_type_ptr) {
        LoadClassType(*dex_file, type_idx, kArg0);
        CallRuntimeHelperRegRegLocationMethod(kQuickAllocArrayResolved, TargetReg(kArg0, kNotWide),
                                              rl_src, true);
      } else {
        // Use the direct pointer.
        CallRuntimeHelperImmRegLocationMethod(kQuickAllocArrayResolved, direct_type_ptr, rl_src,
                                              true);
      }
    } else {
      // The slow path.
      CallRuntimeHelperImmRegLocationMethod(kQuickAllocArray, type_idx, rl_src, true);
    }
  } else {
    CallRuntimeHelperImmRegLocationMethod(kQuickAllocArrayWithAccessCheck, type_idx, rl_src, true);
  }
  StoreValue(rl_dest, GetReturn(kRefReg));
}

/*
 * Similar to GenNewArray, but with post-allocation initialization.
 * Verifier guarantees we're dealing with an array class.  Current
 * code throws runtime exception "bad Filled array req" for 'D' and 'J'.
 * Current code also throws internal unimp if not 'L', '[' or 'I'.
 */
void Mir2Lir::GenFilledNewArray(CallInfo* info) {
  size_t elems = info->num_arg_words;
  int type_idx = info->index;
  FlushAllRegs();  /* Everything to home location */
  QuickEntrypointEnum target;
  if (cu_->compiler_driver->CanAccessTypeWithoutChecks(cu_->method_idx, *cu_->dex_file,
                                                       type_idx)) {
    target = kQuickCheckAndAllocArray;
  } else {
    target = kQuickCheckAndAllocArrayWithAccessCheck;
  }
  CallRuntimeHelperImmImmMethod(target, type_idx, elems, true);
  FreeTemp(TargetReg(kArg2, kNotWide));
  FreeTemp(TargetReg(kArg1, kNotWide));
  /*
   * NOTE: the implicit target for Instruction::FILLED_NEW_ARRAY is the
   * return region.  Because AllocFromCode placed the new array
   * in kRet0, we'll just lock it into place.  When debugger support is
   * added, it may be necessary to additionally copy all return
   * values to a home location in thread-local storage
   */
  RegStorage ref_reg = TargetReg(kRet0, kRef);
  LockTemp(ref_reg);

  // TODO: use the correct component size, currently all supported types
  // share array alignment with ints (see comment at head of function)
  size_t component_size = sizeof(int32_t);

  if (elems > 5) {
    DCHECK(info->is_range);  // Non-range insn can't encode more than 5 elems.
    /*
     * Bit of ugliness here.  We're going generate a mem copy loop
     * on the register range, but it is possible that some regs
     * in the range have been promoted.  This is unlikely, but
     * before generating the copy, we'll just force a flush
     * of any regs in the source range that have been promoted to
     * home location.
     */
    for (size_t i = 0; i < elems; i++) {
      RegLocation loc = UpdateLoc(info->args[i]);
      if (loc.location == kLocPhysReg) {
        ScopedMemRefType mem_ref_type(this, ResourceMask::kDalvikReg);
        if (loc.ref) {
          StoreRefDisp(TargetPtrReg(kSp), SRegOffset(loc.s_reg_low), loc.reg, kNotVolatile);
        } else {
          Store32Disp(TargetPtrReg(kSp), SRegOffset(loc.s_reg_low), loc.reg);
        }
      }
    }
    /*
     * TUNING note: generated code here could be much improved, but
     * this is an uncommon operation and isn't especially performance
     * critical.
     */
    // This is addressing the stack, which may be out of the 4G area.
    RegStorage r_src = AllocTempRef();
    RegStorage r_dst = AllocTempRef();
    RegStorage r_idx = AllocTempRef();  // Not really a reference, but match src/dst.
    RegStorage r_val;
    switch (cu_->instruction_set) {
      case kThumb2:
      case kArm64:
        r_val = TargetReg(kLr, kNotWide);
        break;
      case kX86:
      case kX86_64:
        FreeTemp(ref_reg);
        r_val = AllocTemp();
        break;
      case kMips:
      case kMips64:
        r_val = AllocTemp();
        break;
      default: LOG(FATAL) << "Unexpected instruction set: " << cu_->instruction_set;
    }
    // Set up source pointer
    RegLocation rl_first = info->args[0];
    OpRegRegImm(kOpAdd, r_src, TargetPtrReg(kSp), SRegOffset(rl_first.s_reg_low));
    // Set up the target pointer
    OpRegRegImm(kOpAdd, r_dst, ref_reg,
                mirror::Array::DataOffset(component_size).Int32Value());
    // Set up the loop counter (known to be > 0)
    LoadConstant(r_idx, static_cast<int>(elems - 1));
    // Generate the copy loop.  Going backwards for convenience
    LIR* loop_head_target = NewLIR0(kPseudoTargetLabel);
    // Copy next element
    {
      ScopedMemRefType mem_ref_type(this, ResourceMask::kDalvikReg);
      LoadBaseIndexed(r_src, r_idx, r_val, 2, k32);
      // NOTE: No dalvik register annotation, local optimizations will be stopped
      // by the loop boundaries.
    }
    StoreBaseIndexed(r_dst, r_idx, r_val, 2, k32);
    FreeTemp(r_val);
    OpDecAndBranch(kCondGe, r_idx, loop_head_target);
    if (cu_->instruction_set == kX86 || cu_->instruction_set == kX86_64) {
      // Restore the target pointer
      OpRegRegImm(kOpAdd, ref_reg, r_dst,
                  -mirror::Array::DataOffset(component_size).Int32Value());
    }
    FreeTemp(r_idx);
    FreeTemp(r_dst);
    FreeTemp(r_src);
  } else {
    DCHECK_LE(elems, 5u);  // Usually but not necessarily non-range.
    // TUNING: interleave
    for (size_t i = 0; i < elems; i++) {
      RegLocation rl_arg;
      if (info->args[i].ref) {
        rl_arg = LoadValue(info->args[i], kRefReg);
        StoreRefDisp(ref_reg,
                    mirror::Array::DataOffset(component_size).Int32Value() + i * 4, rl_arg.reg,
                    kNotVolatile);
      } else {
        rl_arg = LoadValue(info->args[i], kCoreReg);
        Store32Disp(ref_reg,
                    mirror::Array::DataOffset(component_size).Int32Value() + i * 4, rl_arg.reg);
      }
      // If the LoadValue caused a temp to be allocated, free it
      if (IsTemp(rl_arg.reg)) {
        FreeTemp(rl_arg.reg);
      }
    }
  }
  if (elems != 0 && info->args[0].ref) {
    // If there is at least one potentially non-null value, unconditionally mark the GC card.
    for (size_t i = 0; i < elems; i++) {
      if (!mir_graph_->IsConstantNullRef(info->args[i])) {
        UnconditionallyMarkGCCard(ref_reg);
        break;
      }
    }
  }
  if (info->result.location != kLocInvalid) {
    StoreValue(info->result, GetReturn(kRefReg));
  }
}

/*
 * Array data table format:
 *  ushort ident = 0x0300   magic value
 *  ushort width            width of each element in the table
 *  uint   size             number of elements in the table
 *  ubyte  data[size*width] table of data values (may contain a single-byte
 *                          padding at the end)
 *
 * Total size is 4+(width * size + 1)/2 16-bit code units.
 */
void Mir2Lir::GenFillArrayData(MIR* mir, DexOffset table_offset, RegLocation rl_src) {
  if (kIsDebugBuild) {
    const uint16_t* table = mir_graph_->GetTable(mir, table_offset);
    const Instruction::ArrayDataPayload* payload =
        reinterpret_cast<const Instruction::ArrayDataPayload*>(table);
    CHECK_EQ(payload->ident, static_cast<uint16_t>(Instruction::kArrayDataSignature));
  }
  uint32_t table_offset_from_start = mir->offset + static_cast<int32_t>(table_offset);
  CallRuntimeHelperImmRegLocation(kQuickHandleFillArrayData, table_offset_from_start, rl_src, true);
}

void Mir2Lir::GenSput(MIR* mir, RegLocation rl_src, OpSize size) {
  const MirSFieldLoweringInfo& field_info = mir_graph_->GetSFieldLoweringInfo(mir);
  DCHECK_EQ(SPutMemAccessType(mir->dalvikInsn.opcode), field_info.MemAccessType());
  cu_->compiler_driver->ProcessedStaticField(field_info.FastPut(), field_info.IsReferrersClass());
  if (!ForceSlowFieldPath(cu_) && field_info.FastPut()) {
    DCHECK_GE(field_info.FieldOffset().Int32Value(), 0);
    RegStorage r_base;
    if (field_info.IsReferrersClass()) {
      // Fast path, static storage base is this method's class
      r_base = AllocTempRef();
      RegStorage r_method = LoadCurrMethodWithHint(r_base);
      LoadRefDisp(r_method, ArtMethod::DeclaringClassOffset().Int32Value(), r_base,
                  kNotVolatile);
    } else {
      // Medium path, static storage base in a different class which requires checks that the other
      // class is initialized.
      r_base = GenGetOtherTypeForSgetSput(field_info, mir->optimization_flags);
      if (!field_info.IsClassInitialized() &&
          (mir->optimization_flags & MIR_CLASS_IS_INITIALIZED) == 0) {
        // Ensure load of status and store of value don't re-order.
        // TODO: Presumably the actual value store is control-dependent on the status load,
        // and will thus not be reordered in any case, since stores are never speculated.
        // Does later code "know" that the class is now initialized?  If so, we still
        // need the barrier to guard later static loads.
        GenMemBarrier(kLoadAny);
      }
    }
    // rBase now holds static storage base
    RegisterClass reg_class = RegClassForFieldLoadStore(size, field_info.IsVolatile());
    if (IsWide(size)) {
      rl_src = LoadValueWide(rl_src, reg_class);
    } else {
      rl_src = LoadValue(rl_src, reg_class);
    }
    if (IsRef(size)) {
      StoreRefDisp(r_base, field_info.FieldOffset().Int32Value(), rl_src.reg,
                   field_info.IsVolatile() ? kVolatile : kNotVolatile);
    } else {
      StoreBaseDisp(r_base, field_info.FieldOffset().Int32Value(), rl_src.reg, size,
                    field_info.IsVolatile() ? kVolatile : kNotVolatile);
    }
    if (IsRef(size) && !mir_graph_->IsConstantNullRef(rl_src)) {
      MarkGCCard(mir->optimization_flags, rl_src.reg, r_base);
    }
    FreeTemp(r_base);
  } else {
    FlushAllRegs();  // Everything to home locations
    QuickEntrypointEnum target;
    switch (size) {
      case kReference:
        target = kQuickSetObjStatic;
        break;
      case k64:
      case kDouble:
        target = kQuickSet64Static;
        break;
      case k32:
      case kSingle:
        target = kQuickSet32Static;
        break;
      case kSignedHalf:
      case kUnsignedHalf:
        target = kQuickSet16Static;
        break;
      case kSignedByte:
      case kUnsignedByte:
        target = kQuickSet8Static;
        break;
      case kWord:  // Intentional fallthrough.
      default:
        LOG(FATAL) << "Can't determine entrypoint for: " << size;
        target = kQuickSet32Static;
    }
    CallRuntimeHelperImmRegLocation(target, field_info.FieldIndex(), rl_src, true);
  }
}

void Mir2Lir::GenSget(MIR* mir, RegLocation rl_dest, OpSize size, Primitive::Type type) {
  const MirSFieldLoweringInfo& field_info = mir_graph_->GetSFieldLoweringInfo(mir);
  DCHECK_EQ(SGetMemAccessType(mir->dalvikInsn.opcode), field_info.MemAccessType());
  cu_->compiler_driver->ProcessedStaticField(field_info.FastGet(), field_info.IsReferrersClass());

  if (!ForceSlowFieldPath(cu_) && field_info.FastGet()) {
    DCHECK_GE(field_info.FieldOffset().Int32Value(), 0);
    RegStorage r_base;
    if (field_info.IsReferrersClass()) {
      // Fast path, static storage base is this method's class
      r_base = AllocTempRef();
      RegStorage r_method = LoadCurrMethodWithHint(r_base);
      LoadRefDisp(r_method, ArtMethod::DeclaringClassOffset().Int32Value(), r_base,
                  kNotVolatile);
    } else {
      // Medium path, static storage base in a different class which requires checks that the other
      // class is initialized
      r_base = GenGetOtherTypeForSgetSput(field_info, mir->optimization_flags);
      if (!field_info.IsClassInitialized() &&
          (mir->optimization_flags & MIR_CLASS_IS_INITIALIZED) == 0) {
        // Ensure load of status and load of value don't re-order.
        GenMemBarrier(kLoadAny);
      }
    }
    // r_base now holds static storage base
    RegisterClass reg_class = RegClassForFieldLoadStore(size, field_info.IsVolatile());
    RegLocation rl_result = EvalLoc(rl_dest, reg_class, true);

    int field_offset = field_info.FieldOffset().Int32Value();
    if (IsRef(size)) {
      // TODO: DCHECK?
      LoadRefDisp(r_base, field_offset, rl_result.reg, field_info.IsVolatile() ? kVolatile :
          kNotVolatile);
    } else {
      LoadBaseDisp(r_base, field_offset, rl_result.reg, size, field_info.IsVolatile() ?
          kVolatile : kNotVolatile);
    }
    FreeTemp(r_base);

    if (IsWide(size)) {
      StoreValueWide(rl_dest, rl_result);
    } else {
      StoreValue(rl_dest, rl_result);
    }
  } else {
    DCHECK(SizeMatchesTypeForEntrypoint(size, type));
    FlushAllRegs();  // Everything to home locations
    QuickEntrypointEnum target;
    switch (type) {
      case Primitive::kPrimNot:
        target = kQuickGetObjStatic;
        break;
      case Primitive::kPrimLong:
      case Primitive::kPrimDouble:
        target = kQuickGet64Static;
        break;
      case Primitive::kPrimInt:
      case Primitive::kPrimFloat:
        target = kQuickGet32Static;
        break;
      case Primitive::kPrimShort:
        target = kQuickGetShortStatic;
        break;
      case Primitive::kPrimChar:
        target = kQuickGetCharStatic;
        break;
      case Primitive::kPrimByte:
        target = kQuickGetByteStatic;
        break;
      case Primitive::kPrimBoolean:
        target = kQuickGetBooleanStatic;
        break;
      case Primitive::kPrimVoid:  // Intentional fallthrough.
      default:
        LOG(FATAL) << "Can't determine entrypoint for: " << type;
        target = kQuickGet32Static;
    }
    CallRuntimeHelperImm(target, field_info.FieldIndex(), true);

    // FIXME: pGetXXStatic always return an int or int64 regardless of rl_dest.fp.
    if (IsWide(size)) {
      RegLocation rl_result = GetReturnWide(kCoreReg);
      StoreValueWide(rl_dest, rl_result);
    } else {
      RegLocation rl_result = GetReturn(rl_dest.ref ? kRefReg : kCoreReg);
      StoreValue(rl_dest, rl_result);
    }
  }
}

// Generate code for all slow paths.
void Mir2Lir::HandleSlowPaths() {
  // We should check slow_paths_.Size() every time, because a new slow path
  // may be created during slowpath->Compile().
  for (LIRSlowPath* slowpath : slow_paths_) {
    slowpath->Compile();
  }
  slow_paths_.clear();
}

void Mir2Lir::GenIGet(MIR* mir, int opt_flags, OpSize size, Primitive::Type type,
                      RegLocation rl_dest, RegLocation rl_obj) {
  const MirIFieldLoweringInfo& field_info = mir_graph_->GetIFieldLoweringInfo(mir);
  if (kIsDebugBuild) {
    auto mem_access_type = IsInstructionIGetQuickOrIPutQuick(mir->dalvikInsn.opcode) ?
        IGetQuickOrIPutQuickMemAccessType(mir->dalvikInsn.opcode) :
        IGetMemAccessType(mir->dalvikInsn.opcode);
    DCHECK_EQ(mem_access_type, field_info.MemAccessType()) << mir->dalvikInsn.opcode;
  }
  cu_->compiler_driver->ProcessedInstanceField(field_info.FastGet());
  if (!ForceSlowFieldPath(cu_) && field_info.FastGet()) {
    RegisterClass reg_class = RegClassForFieldLoadStore(size, field_info.IsVolatile());
    // A load of the class will lead to an iget with offset 0.
    DCHECK_GE(field_info.FieldOffset().Int32Value(), 0);
    rl_obj = LoadValue(rl_obj, kRefReg);
    GenNullCheck(rl_obj.reg, opt_flags);
    RegLocation rl_result = EvalLoc(rl_dest, reg_class, true);
    int field_offset = field_info.FieldOffset().Int32Value();
    LIR* load_lir;
    if (IsRef(size)) {
      load_lir = LoadRefDisp(rl_obj.reg, field_offset, rl_result.reg, field_info.IsVolatile() ?
          kVolatile : kNotVolatile);
    } else {
      load_lir = LoadBaseDisp(rl_obj.reg, field_offset, rl_result.reg, size,
                              field_info.IsVolatile() ? kVolatile : kNotVolatile);
    }
    MarkPossibleNullPointerExceptionAfter(opt_flags, load_lir);
    if (IsWide(size)) {
      StoreValueWide(rl_dest, rl_result);
    } else {
      StoreValue(rl_dest, rl_result);
    }
  } else {
    DCHECK(SizeMatchesTypeForEntrypoint(size, type));
    QuickEntrypointEnum target;
    switch (type) {
      case Primitive::kPrimNot:
        target = kQuickGetObjInstance;
        break;
      case Primitive::kPrimLong:
      case Primitive::kPrimDouble:
        target = kQuickGet64Instance;
        break;
      case Primitive::kPrimFloat:
      case Primitive::kPrimInt:
        target = kQuickGet32Instance;
        break;
      case Primitive::kPrimShort:
        target = kQuickGetShortInstance;
        break;
      case Primitive::kPrimChar:
        target = kQuickGetCharInstance;
        break;
      case Primitive::kPrimByte:
        target = kQuickGetByteInstance;
        break;
      case Primitive::kPrimBoolean:
        target = kQuickGetBooleanInstance;
        break;
      case Primitive::kPrimVoid:  // Intentional fallthrough.
      default:
        LOG(FATAL) << "Can't determine entrypoint for: " << type;
        target = kQuickGet32Instance;
    }
    // Second argument of pGetXXInstance is always a reference.
    DCHECK_EQ(static_cast<unsigned int>(rl_obj.wide), 0U);
    CallRuntimeHelperImmRegLocation(target, field_info.FieldIndex(), rl_obj, true);

    // FIXME: pGetXXInstance always return an int or int64 regardless of rl_dest.fp.
    if (IsWide(size)) {
      RegLocation rl_result = GetReturnWide(kCoreReg);
      StoreValueWide(rl_dest, rl_result);
    } else {
      RegLocation rl_result = GetReturn(rl_dest.ref ? kRefReg : kCoreReg);
      StoreValue(rl_dest, rl_result);
    }
  }
}

void Mir2Lir::GenIPut(MIR* mir, int opt_flags, OpSize size,
                      RegLocation rl_src, RegLocation rl_obj) {
  const MirIFieldLoweringInfo& field_info = mir_graph_->GetIFieldLoweringInfo(mir);
  if (kIsDebugBuild) {
    auto mem_access_type = IsInstructionIGetQuickOrIPutQuick(mir->dalvikInsn.opcode) ?
        IGetQuickOrIPutQuickMemAccessType(mir->dalvikInsn.opcode) :
        IPutMemAccessType(mir->dalvikInsn.opcode);
    DCHECK_EQ(mem_access_type, field_info.MemAccessType());
  }
  cu_->compiler_driver->ProcessedInstanceField(field_info.FastPut());
  if (!ForceSlowFieldPath(cu_) && field_info.FastPut()) {
    RegisterClass reg_class = RegClassForFieldLoadStore(size, field_info.IsVolatile());
    // Dex code never writes to the class field.
    DCHECK_GE(static_cast<uint32_t>(field_info.FieldOffset().Int32Value()),
              sizeof(mirror::HeapReference<mirror::Class>));
    rl_obj = LoadValue(rl_obj, kRefReg);
    if (IsWide(size)) {
      rl_src = LoadValueWide(rl_src, reg_class);
    } else {
      rl_src = LoadValue(rl_src, reg_class);
    }
    GenNullCheck(rl_obj.reg, opt_flags);
    int field_offset = field_info.FieldOffset().Int32Value();
    LIR* null_ck_insn;
    if (IsRef(size)) {
      null_ck_insn = StoreRefDisp(rl_obj.reg, field_offset, rl_src.reg, field_info.IsVolatile() ?
          kVolatile : kNotVolatile);
    } else {
      null_ck_insn = StoreBaseDisp(rl_obj.reg, field_offset, rl_src.reg, size,
                                   field_info.IsVolatile() ? kVolatile : kNotVolatile);
    }
    MarkPossibleNullPointerExceptionAfter(opt_flags, null_ck_insn);
    if (IsRef(size) && !mir_graph_->IsConstantNullRef(rl_src)) {
      MarkGCCard(opt_flags, rl_src.reg, rl_obj.reg);
    }
  } else {
    QuickEntrypointEnum target;
    switch (size) {
      case kReference:
        target = kQuickSetObjInstance;
        break;
      case k64:
      case kDouble:
        target = kQuickSet64Instance;
        break;
      case k32:
      case kSingle:
        target = kQuickSet32Instance;
        break;
      case kSignedHalf:
      case kUnsignedHalf:
        target = kQuickSet16Instance;
        break;
      case kSignedByte:
      case kUnsignedByte:
        target = kQuickSet8Instance;
        break;
      case kWord:  // Intentional fallthrough.
      default:
        LOG(FATAL) << "Can't determine entrypoint for: " << size;
        target = kQuickSet32Instance;
    }
    CallRuntimeHelperImmRegLocationRegLocation(target, field_info.FieldIndex(), rl_obj, rl_src,
                                               true);
  }
}

void Mir2Lir::GenArrayObjPut(int opt_flags, RegLocation rl_array, RegLocation rl_index,
                             RegLocation rl_src) {
  bool needs_range_check = !(opt_flags & MIR_IGNORE_RANGE_CHECK);
  bool needs_null_check = !((cu_->disable_opt & (1 << kNullCheckElimination)) &&
      (opt_flags & MIR_IGNORE_NULL_CHECK));
  QuickEntrypointEnum target = needs_range_check
        ? (needs_null_check ? kQuickAputObjectWithNullAndBoundCheck
                            : kQuickAputObjectWithBoundCheck)
        : kQuickAputObject;
  CallRuntimeHelperRegLocationRegLocationRegLocation(target, rl_array, rl_index, rl_src, true);
}

void Mir2Lir::GenConstClass(uint32_t type_idx, RegLocation rl_dest) {
  RegLocation rl_result;
  if (!cu_->compiler_driver->CanAccessTypeWithoutChecks(cu_->method_idx,
                                                        *cu_->dex_file,
                                                        type_idx)) {
    // Call out to helper which resolves type and verifies access.
    // Resolved type returned in kRet0.
    CallRuntimeHelperImm(kQuickInitializeTypeAndVerifyAccess, type_idx, true);
    rl_result = GetReturn(kRefReg);
  } else {
    rl_result = EvalLoc(rl_dest, kRefReg, true);
    // We don't need access checks, load type from dex cache
    if (CanUseOpPcRelDexCacheArrayLoad()) {
      size_t offset = dex_cache_arrays_layout_.TypeOffset(type_idx);
      OpPcRelDexCacheArrayLoad(cu_->dex_file, offset, rl_result.reg, false);
    } else {
      int32_t dex_cache_offset =
          ArtMethod::DexCacheResolvedTypesOffset().Int32Value();
      RegStorage res_reg = AllocTempRef();
      RegStorage r_method = LoadCurrMethodWithHint(res_reg);
      LoadRefDisp(r_method, dex_cache_offset, res_reg, kNotVolatile);
      int32_t offset_of_type = ClassArray::OffsetOfElement(type_idx).Int32Value();
      LoadRefDisp(res_reg, offset_of_type, rl_result.reg, kNotVolatile);
      FreeTemp(res_reg);
    }
    if (!cu_->compiler_driver->CanAssumeTypeIsPresentInDexCache(*cu_->dex_file,
        type_idx) || ForceSlowTypePath(cu_)) {
      // Slow path, at runtime test if type is null and if so initialize
      FlushAllRegs();
      GenIfNullUseHelperImm(rl_result.reg, kQuickInitializeType, type_idx);
    }
  }
  StoreValue(rl_dest, rl_result);
}

void Mir2Lir::GenConstString(uint32_t string_idx, RegLocation rl_dest) {
  /* NOTE: Most strings should be available at compile time */
  int32_t offset_of_string = mirror::ObjectArray<mirror::String>::OffsetOfElement(string_idx).
                                                                                      Int32Value();
  if (!cu_->compiler_driver->CanAssumeStringIsPresentInDexCache(
      *cu_->dex_file, string_idx) || ForceSlowStringPath(cu_)) {
    // slow path, resolve string if not in dex cache
    FlushAllRegs();
    LockCallTemps();  // Using explicit registers

    // Might call out to helper, which will return resolved string in kRet0
    RegStorage ret0 = TargetReg(kRet0, kRef);
    if (CanUseOpPcRelDexCacheArrayLoad()) {
      size_t offset = dex_cache_arrays_layout_.StringOffset(string_idx);
      OpPcRelDexCacheArrayLoad(cu_->dex_file, offset, ret0, false);
    } else {
      // Method to declaring class.
      RegStorage arg0 = TargetReg(kArg0, kRef);
      RegStorage r_method = LoadCurrMethodWithHint(arg0);
      LoadRefDisp(r_method, ArtMethod::DeclaringClassOffset().Int32Value(), arg0, kNotVolatile);
      // Declaring class to dex cache strings.
      LoadRefDisp(arg0, mirror::Class::DexCacheStringsOffset().Int32Value(), arg0, kNotVolatile);

      LoadRefDisp(arg0, offset_of_string, ret0, kNotVolatile);
    }
    GenIfNullUseHelperImm(ret0, kQuickResolveString, string_idx);

    GenBarrier();
    StoreValue(rl_dest, GetReturn(kRefReg));
  } else {
    RegLocation rl_result = EvalLoc(rl_dest, kRefReg, true);
    if (CanUseOpPcRelDexCacheArrayLoad()) {
      size_t offset = dex_cache_arrays_layout_.StringOffset(string_idx);
      OpPcRelDexCacheArrayLoad(cu_->dex_file, offset, rl_result.reg, false);
    } else {
      RegLocation rl_method = LoadCurrMethod();
      RegStorage res_reg = AllocTempRef();
      LoadRefDisp(rl_method.reg, ArtMethod::DeclaringClassOffset().Int32Value(), res_reg,
                  kNotVolatile);
      LoadRefDisp(res_reg, mirror::Class::DexCacheStringsOffset().Int32Value(), res_reg,
                  kNotVolatile);
      LoadRefDisp(res_reg, offset_of_string, rl_result.reg, kNotVolatile);
      FreeTemp(res_reg);
    }
    StoreValue(rl_dest, rl_result);
  }
}

/*
 * Let helper function take care of everything.  Will
 * call Class::NewInstanceFromCode(type_idx, method);
 */
void Mir2Lir::GenNewInstance(uint32_t type_idx, RegLocation rl_dest) {
  FlushAllRegs();  /* Everything to home location */
  // alloc will always check for resolution, do we also need to verify
  // access because the verifier was unable to?
  const DexFile* dex_file = cu_->dex_file;
  CompilerDriver* driver = cu_->compiler_driver;
  if (driver->CanAccessInstantiableTypeWithoutChecks(cu_->method_idx, *dex_file, type_idx)) {
    bool is_type_initialized;
    bool use_direct_type_ptr;
    uintptr_t direct_type_ptr;
    bool is_finalizable;
    if (kEmbedClassInCode &&
        driver->CanEmbedTypeInCode(*dex_file, type_idx, &is_type_initialized, &use_direct_type_ptr,
                                   &direct_type_ptr, &is_finalizable) &&
                                   !is_finalizable) {
      // The fast path.
      if (!use_direct_type_ptr) {
        LoadClassType(*dex_file, type_idx, kArg0);
        if (!is_type_initialized) {
          CallRuntimeHelperRegMethod(kQuickAllocObjectResolved, TargetReg(kArg0, kRef), true);
        } else {
          CallRuntimeHelperRegMethod(kQuickAllocObjectInitialized, TargetReg(kArg0, kRef), true);
        }
      } else {
        // Use the direct pointer.
        if (!is_type_initialized) {
          CallRuntimeHelperImmMethod(kQuickAllocObjectResolved, direct_type_ptr, true);
        } else {
          CallRuntimeHelperImmMethod(kQuickAllocObjectInitialized, direct_type_ptr, true);
        }
      }
    } else {
      // The slow path.
      CallRuntimeHelperImmMethod(kQuickAllocObject, type_idx, true);
    }
  } else {
    CallRuntimeHelperImmMethod(kQuickAllocObjectWithAccessCheck, type_idx, true);
  }
  StoreValue(rl_dest, GetReturn(kRefReg));
}

void Mir2Lir::GenThrow(RegLocation rl_src) {
  FlushAllRegs();
  CallRuntimeHelperRegLocation(kQuickDeliverException, rl_src, true);
}

// For final classes there are no sub-classes to check and so we can answer the instance-of
// question with simple comparisons.
void Mir2Lir::GenInstanceofFinal(bool use_declaring_class, uint32_t type_idx, RegLocation rl_dest,
                                 RegLocation rl_src) {
  // X86 has its own implementation.
  DCHECK(cu_->instruction_set != kX86 && cu_->instruction_set != kX86_64);

  RegLocation object = LoadValue(rl_src, kRefReg);
  RegLocation rl_result = EvalLoc(rl_dest, kCoreReg, true);
  RegStorage result_reg = rl_result.reg;
  if (IsSameReg(result_reg, object.reg)) {
    result_reg = AllocTypedTemp(false, kCoreReg);
    DCHECK(!IsSameReg(result_reg, object.reg));
  }
  LoadConstant(result_reg, 0);     // assume false
  LIR* null_branchover = OpCmpImmBranch(kCondEq, object.reg, 0, nullptr);

  RegStorage check_class = AllocTypedTemp(false, kRefReg);
  RegStorage object_class = AllocTypedTemp(false, kRefReg);

  if (use_declaring_class) {
    RegStorage r_method = LoadCurrMethodWithHint(check_class);
    LoadRefDisp(r_method, ArtMethod::DeclaringClassOffset().Int32Value(), check_class,
                kNotVolatile);
    LoadRefDisp(object.reg,  mirror::Object::ClassOffset().Int32Value(), object_class,
                kNotVolatile);
  } else if (CanUseOpPcRelDexCacheArrayLoad()) {
    size_t offset = dex_cache_arrays_layout_.TypeOffset(type_idx);
    OpPcRelDexCacheArrayLoad(cu_->dex_file, offset, check_class, false);
    LoadRefDisp(object.reg,  mirror::Object::ClassOffset().Int32Value(), object_class,
                kNotVolatile);
  } else {
    RegStorage r_method = LoadCurrMethodWithHint(check_class);
    LoadRefDisp(r_method, ArtMethod::DexCacheResolvedTypesOffset().Int32Value(),
                check_class, kNotVolatile);
    LoadRefDisp(object.reg,  mirror::Object::ClassOffset().Int32Value(), object_class,
                kNotVolatile);
    int32_t offset_of_type = ClassArray::OffsetOfElement(type_idx).Int32Value();
    LoadRefDisp(check_class, offset_of_type, check_class, kNotVolatile);
  }

  // FIXME: what should we be comparing here? compressed or decompressed references?
  if (cu_->instruction_set == kThumb2) {
    OpRegReg(kOpCmp, check_class, object_class);  // Same?
    LIR* it = OpIT(kCondEq, "");   // if-convert the test
    LoadConstant(result_reg, 1);     // .eq case - load true
    OpEndIT(it);
  } else {
    GenSelectConst32(check_class, object_class, kCondEq, 1, 0, result_reg, kCoreReg);
  }
  LIR* target = NewLIR0(kPseudoTargetLabel);
  null_branchover->target = target;
  FreeTemp(object_class);
  FreeTemp(check_class);
  if (IsTemp(result_reg)) {
    OpRegCopy(rl_result.reg, result_reg);
    FreeTemp(result_reg);
  }
  StoreValue(rl_dest, rl_result);
}

void Mir2Lir::GenInstanceofCallingHelper(bool needs_access_check, bool type_known_final,
                                         bool type_known_abstract, bool use_declaring_class,
                                         bool can_assume_type_is_in_dex_cache,
                                         uint32_t type_idx, RegLocation rl_dest,
                                         RegLocation rl_src) {
  FlushAllRegs();
  // May generate a call - use explicit registers
  LockCallTemps();
  RegStorage class_reg = TargetReg(kArg2, kRef);  // kArg2 will hold the Class*
  RegStorage ref_reg = TargetReg(kArg0, kRef);  // kArg0 will hold the ref.
  RegStorage ret_reg = GetReturn(kRefReg).reg;
  if (needs_access_check) {
    // Check we have access to type_idx and if not throw IllegalAccessError,
    // returns Class* in kArg0
    CallRuntimeHelperImmMethod(kQuickInitializeTypeAndVerifyAccess, type_idx, true);
    OpRegCopy(class_reg, ret_reg);  // Align usage with fast path
    LoadValueDirectFixed(rl_src, ref_reg);  // kArg0 <= ref
  } else if (use_declaring_class) {
    RegStorage r_method = LoadCurrMethodWithHint(TargetReg(kArg1, kRef));
    LoadValueDirectFixed(rl_src, ref_reg);  // kArg0 <= ref
    LoadRefDisp(r_method, ArtMethod::DeclaringClassOffset().Int32Value(),
                class_reg, kNotVolatile);
  } else {
    if (can_assume_type_is_in_dex_cache) {
      // Conditionally, as in the other case we will also load it.
      LoadValueDirectFixed(rl_src, ref_reg);  // kArg0 <= ref
    }

    if (CanUseOpPcRelDexCacheArrayLoad()) {
      size_t offset = dex_cache_arrays_layout_.TypeOffset(type_idx);
      OpPcRelDexCacheArrayLoad(cu_->dex_file, offset, class_reg, false);
    } else {
      RegStorage r_method = LoadCurrMethodWithHint(class_reg);
      // Load dex cache entry into class_reg (kArg2)
      LoadRefDisp(r_method, ArtMethod::DexCacheResolvedTypesOffset().Int32Value(),
                  class_reg, kNotVolatile);
      int32_t offset_of_type = ClassArray::OffsetOfElement(type_idx).Int32Value();
      LoadRefDisp(class_reg, offset_of_type, class_reg, kNotVolatile);
    }
    if (!can_assume_type_is_in_dex_cache) {
      GenIfNullUseHelperImm(class_reg, kQuickInitializeType, type_idx);

      // Should load value here.
      LoadValueDirectFixed(rl_src, ref_reg);  // kArg0 <= ref
    }
  }
  /* kArg0 is ref, kArg2 is class. If ref==null, use directly as bool result */
  RegLocation rl_result = GetReturn(kCoreReg);
  if (!IsSameReg(rl_result.reg, ref_reg)) {
    // On MIPS and x86_64 rArg0 != rl_result, place false in result if branch is taken.
    LoadConstant(rl_result.reg, 0);
  }
  LIR* branch1 = OpCmpImmBranch(kCondEq, ref_reg, 0, nullptr);

  /* load object->klass_ */
  RegStorage ref_class_reg = TargetReg(kArg1, kRef);  // kArg1 will hold the Class* of ref.
  DCHECK_EQ(mirror::Object::ClassOffset().Int32Value(), 0);
  LoadRefDisp(ref_reg, mirror::Object::ClassOffset().Int32Value(),
              ref_class_reg, kNotVolatile);
  /* kArg0 is ref, kArg1 is ref->klass_, kArg2 is class */
  LIR* branchover = nullptr;
  if (type_known_final) {
    // rl_result == ref == class.
    GenSelectConst32(ref_class_reg, class_reg, kCondEq, 1, 0, rl_result.reg,
                     kCoreReg);
  } else {
    if (cu_->instruction_set == kThumb2) {
      RegStorage r_tgt = LoadHelper(kQuickInstanceofNonTrivial);
      LIR* it = nullptr;
      if (!type_known_abstract) {
      /* Uses conditional nullification */
        OpRegReg(kOpCmp, ref_class_reg, class_reg);  // Same?
        it = OpIT(kCondEq, "EE");   // if-convert the test
        LoadConstant(rl_result.reg, 1);     // .eq case - load true
      }
      OpRegCopy(ref_reg, class_reg);    // .ne case - arg0 <= class
      OpReg(kOpBlx, r_tgt);    // .ne case: helper(class, ref->class)
      if (it != nullptr) {
        OpEndIT(it);
      }
      FreeTemp(r_tgt);
    } else {
      if (!type_known_abstract) {
        /* Uses branchovers */
        LoadConstant(rl_result.reg, 1);     // assume true
        branchover = OpCmpBranch(kCondEq, TargetReg(kArg1, kRef), TargetReg(kArg2, kRef), nullptr);
      }

      OpRegCopy(TargetReg(kArg0, kRef), class_reg);    // .ne case - arg0 <= class
      CallRuntimeHelper(kQuickInstanceofNonTrivial, false);
    }
  }
  // TODO: only clobber when type isn't final?
  ClobberCallerSave();
  /* branch targets here */
  LIR* target = NewLIR0(kPseudoTargetLabel);
  StoreValue(rl_dest, rl_result);
  branch1->target = target;
  if (branchover != nullptr) {
    branchover->target = target;
  }
}

void Mir2Lir::GenInstanceof(uint32_t type_idx, RegLocation rl_dest, RegLocation rl_src) {
  bool type_known_final, type_known_abstract, use_declaring_class;
  bool needs_access_check = !cu_->compiler_driver->CanAccessTypeWithoutChecks(cu_->method_idx,
                                                                              *cu_->dex_file,
                                                                              type_idx,
                                                                              &type_known_final,
                                                                              &type_known_abstract,
                                                                              &use_declaring_class);
  bool can_assume_type_is_in_dex_cache = !needs_access_check &&
      cu_->compiler_driver->CanAssumeTypeIsPresentInDexCache(*cu_->dex_file, type_idx);

  if ((use_declaring_class || can_assume_type_is_in_dex_cache) && type_known_final) {
    GenInstanceofFinal(use_declaring_class, type_idx, rl_dest, rl_src);
  } else {
    GenInstanceofCallingHelper(needs_access_check, type_known_final, type_known_abstract,
                               use_declaring_class, can_assume_type_is_in_dex_cache,
                               type_idx, rl_dest, rl_src);
  }
}

void Mir2Lir::GenCheckCast(int opt_flags, uint32_t insn_idx, uint32_t type_idx,
                           RegLocation rl_src) {
  if ((opt_flags & MIR_IGNORE_CHECK_CAST) != 0) {
    // Compiler analysis proved that this check-cast would never cause an exception.
    return;
  }
  bool type_known_final, type_known_abstract, use_declaring_class;
  bool needs_access_check = !cu_->compiler_driver->CanAccessTypeWithoutChecks(cu_->method_idx,
                                                                              *cu_->dex_file,
                                                                              type_idx,
                                                                              &type_known_final,
                                                                              &type_known_abstract,
                                                                              &use_declaring_class);
  // Note: currently type_known_final is unused, as optimizing will only improve the performance
  // of the exception throw path.
  DexCompilationUnit* cu = mir_graph_->GetCurrentDexCompilationUnit();
  if (!needs_access_check && cu_->compiler_driver->IsSafeCast(cu, insn_idx)) {
    // Verifier type analysis proved this check cast would never cause an exception.
    return;
  }
  FlushAllRegs();
  // May generate a call - use explicit registers
  LockCallTemps();
  RegStorage class_reg = TargetReg(kArg2, kRef);  // kArg2 will hold the Class*
  if (needs_access_check) {
    // Check we have access to type_idx and if not throw IllegalAccessError,
    // returns Class* in kRet0
    // InitializeTypeAndVerifyAccess(idx, method)
    CallRuntimeHelperImmMethod(kQuickInitializeTypeAndVerifyAccess, type_idx, true);
    OpRegCopy(class_reg, TargetReg(kRet0, kRef));  // Align usage with fast path
  } else if (use_declaring_class) {
    RegStorage method_reg = LoadCurrMethodWithHint(TargetReg(kArg1, kRef));
    LoadRefDisp(method_reg, ArtMethod::DeclaringClassOffset().Int32Value(),
                class_reg, kNotVolatile);
  } else {
    // Load dex cache entry into class_reg (kArg2)
    if (CanUseOpPcRelDexCacheArrayLoad()) {
      size_t offset = dex_cache_arrays_layout_.TypeOffset(type_idx);
      OpPcRelDexCacheArrayLoad(cu_->dex_file, offset, class_reg, false);
    } else {
      RegStorage r_method = LoadCurrMethodWithHint(class_reg);

      LoadRefDisp(r_method, ArtMethod::DexCacheResolvedTypesOffset().Int32Value(),
                  class_reg, kNotVolatile);
      int32_t offset_of_type = ClassArray::OffsetOfElement(type_idx).Int32Value();
      LoadRefDisp(class_reg, offset_of_type, class_reg, kNotVolatile);
    }
    if (!cu_->compiler_driver->CanAssumeTypeIsPresentInDexCache(*cu_->dex_file, type_idx)) {
      // Need to test presence of type in dex cache at runtime
      GenIfNullUseHelperImm(class_reg, kQuickInitializeType, type_idx);
    }
  }
  // At this point, class_reg (kArg2) has class
  LoadValueDirectFixed(rl_src, TargetReg(kArg0, kRef));  // kArg0 <= ref

  // Slow path for the case where the classes are not equal.  In this case we need
  // to call a helper function to do the check.
  class SlowPath : public LIRSlowPath {
   public:
    SlowPath(Mir2Lir* m2l, LIR* fromfast, LIR* cont, bool load)
        : LIRSlowPath(m2l, fromfast, cont), load_(load) {
    }

    void Compile() {
      GenerateTargetLabel();

      if (load_) {
        m2l_->LoadRefDisp(m2l_->TargetReg(kArg0, kRef), mirror::Object::ClassOffset().Int32Value(),
                          m2l_->TargetReg(kArg1, kRef), kNotVolatile);
      }
      m2l_->CallRuntimeHelperRegReg(kQuickCheckCast, m2l_->TargetReg(kArg2, kRef),
                                    m2l_->TargetReg(kArg1, kRef), true);
      m2l_->OpUnconditionalBranch(cont_);
    }

   private:
    const bool load_;
  };

  if (type_known_abstract) {
    // Easier case, run slow path if target is non-null (slow path will load from target)
    LIR* branch = OpCmpImmBranch(kCondNe, TargetReg(kArg0, kRef), 0, nullptr);
    LIR* cont = NewLIR0(kPseudoTargetLabel);
    AddSlowPath(new (arena_) SlowPath(this, branch, cont, true));
  } else {
    // Harder, more common case.  We need to generate a forward branch over the load
    // if the target is null.  If it's non-null we perform the load and branch to the
    // slow path if the classes are not equal.

    /* Null is OK - continue */
    LIR* branch1 = OpCmpImmBranch(kCondEq, TargetReg(kArg0, kRef), 0, nullptr);
    /* load object->klass_ */
    DCHECK_EQ(mirror::Object::ClassOffset().Int32Value(), 0);
    LoadRefDisp(TargetReg(kArg0, kRef), mirror::Object::ClassOffset().Int32Value(),
                TargetReg(kArg1, kRef), kNotVolatile);

    LIR* branch2 = OpCmpBranch(kCondNe, TargetReg(kArg1, kRef), class_reg, nullptr);
    LIR* cont = NewLIR0(kPseudoTargetLabel);

    // Add the slow path that will not perform load since this is already done.
    AddSlowPath(new (arena_) SlowPath(this, branch2, cont, false));

    // Set the null check to branch to the continuation.
    branch1->target = cont;
  }
}

void Mir2Lir::GenLong3Addr(OpKind first_op, OpKind second_op, RegLocation rl_dest,
                           RegLocation rl_src1, RegLocation rl_src2) {
  RegLocation rl_result;
  if (cu_->instruction_set == kThumb2) {
    /*
     * NOTE:  This is the one place in the code in which we might have
     * as many as six live temporary registers.  There are 5 in the normal
     * set for Arm.  Until we have spill capabilities, temporarily add
     * lr to the temp set.  It is safe to do this locally, but note that
     * lr is used explicitly elsewhere in the code generator and cannot
     * normally be used as a general temp register.
     */
    MarkTemp(TargetReg(kLr, kNotWide));   // Add lr to the temp pool
    FreeTemp(TargetReg(kLr, kNotWide));   // and make it available
  }
  rl_src1 = LoadValueWide(rl_src1, kCoreReg);
  rl_src2 = LoadValueWide(rl_src2, kCoreReg);
  rl_result = EvalLoc(rl_dest, kCoreReg, true);
  // The longs may overlap - use intermediate temp if so
  if ((rl_result.reg.GetLowReg() == rl_src1.reg.GetHighReg()) || (rl_result.reg.GetLowReg() == rl_src2.reg.GetHighReg())) {
    RegStorage t_reg = AllocTemp();
    OpRegRegReg(first_op, t_reg, rl_src1.reg.GetLow(), rl_src2.reg.GetLow());
    OpRegRegReg(second_op, rl_result.reg.GetHigh(), rl_src1.reg.GetHigh(), rl_src2.reg.GetHigh());
    OpRegCopy(rl_result.reg.GetLow(), t_reg);
    FreeTemp(t_reg);
  } else {
    OpRegRegReg(first_op, rl_result.reg.GetLow(), rl_src1.reg.GetLow(), rl_src2.reg.GetLow());
    OpRegRegReg(second_op, rl_result.reg.GetHigh(), rl_src1.reg.GetHigh(), rl_src2.reg.GetHigh());
  }
  /*
   * NOTE: If rl_dest refers to a frame variable in a large frame, the
   * following StoreValueWide might need to allocate a temp register.
   * To further work around the lack of a spill capability, explicitly
   * free any temps from rl_src1 & rl_src2 that aren't still live in rl_result.
   * Remove when spill is functional.
   */
  FreeRegLocTemps(rl_result, rl_src1);
  FreeRegLocTemps(rl_result, rl_src2);
  StoreValueWide(rl_dest, rl_result);
  if (cu_->instruction_set == kThumb2) {
    Clobber(TargetReg(kLr, kNotWide));
    UnmarkTemp(TargetReg(kLr, kNotWide));  // Remove lr from the temp pool
  }
}

void Mir2Lir::GenShiftOpLong(Instruction::Code opcode, RegLocation rl_dest,
                             RegLocation rl_src1, RegLocation rl_shift) {
  QuickEntrypointEnum target;
  switch (opcode) {
    case Instruction::SHL_LONG:
    case Instruction::SHL_LONG_2ADDR:
      target = kQuickShlLong;
      break;
    case Instruction::SHR_LONG:
    case Instruction::SHR_LONG_2ADDR:
      target = kQuickShrLong;
      break;
    case Instruction::USHR_LONG:
    case Instruction::USHR_LONG_2ADDR:
      target = kQuickUshrLong;
      break;
    default:
      LOG(FATAL) << "Unexpected case";
      target = kQuickShlLong;
  }
  FlushAllRegs();   /* Send everything to home location */
  CallRuntimeHelperRegLocationRegLocation(target, rl_src1, rl_shift, false);
  RegLocation rl_result = GetReturnWide(kCoreReg);
  StoreValueWide(rl_dest, rl_result);
}


void Mir2Lir::GenArithOpInt(Instruction::Code opcode, RegLocation rl_dest,
                            RegLocation rl_src1, RegLocation rl_src2, int flags) {
  DCHECK(cu_->instruction_set != kX86 && cu_->instruction_set != kX86_64);
  OpKind op = kOpBkpt;
  bool is_div_rem = false;
  bool check_zero = false;
  bool unary = false;
  RegLocation rl_result;
  bool shift_op = false;
  switch (opcode) {
    case Instruction::NEG_INT:
      op = kOpNeg;
      unary = true;
      break;
    case Instruction::NOT_INT:
      op = kOpMvn;
      unary = true;
      break;
    case Instruction::ADD_INT:
    case Instruction::ADD_INT_2ADDR:
      op = kOpAdd;
      break;
    case Instruction::SUB_INT:
    case Instruction::SUB_INT_2ADDR:
      op = kOpSub;
      break;
    case Instruction::MUL_INT:
    case Instruction::MUL_INT_2ADDR:
      op = kOpMul;
      break;
    case Instruction::DIV_INT:
    case Instruction::DIV_INT_2ADDR:
      check_zero = true;
      op = kOpDiv;
      is_div_rem = true;
      break;
    /* NOTE: returns in kArg1 */
    case Instruction::REM_INT:
    case Instruction::REM_INT_2ADDR:
      check_zero = true;
      op = kOpRem;
      is_div_rem = true;
      break;
    case Instruction::AND_INT:
    case Instruction::AND_INT_2ADDR:
      op = kOpAnd;
      break;
    case Instruction::OR_INT:
    case Instruction::OR_INT_2ADDR:
      op = kOpOr;
      break;
    case Instruction::XOR_INT:
    case Instruction::XOR_INT_2ADDR:
      op = kOpXor;
      break;
    case Instruction::SHL_INT:
    case Instruction::SHL_INT_2ADDR:
      shift_op = true;
      op = kOpLsl;
      break;
    case Instruction::SHR_INT:
    case Instruction::SHR_INT_2ADDR:
      shift_op = true;
      op = kOpAsr;
      break;
    case Instruction::USHR_INT:
    case Instruction::USHR_INT_2ADDR:
      shift_op = true;
      op = kOpLsr;
      break;
    default:
      LOG(FATAL) << "Invalid word arith op: " << opcode;
  }
  if (!is_div_rem) {
    if (unary) {
      rl_src1 = LoadValue(rl_src1, kCoreReg);
      rl_result = EvalLoc(rl_dest, kCoreReg, true);
      OpRegReg(op, rl_result.reg, rl_src1.reg);
    } else {
      if ((shift_op) && (cu_->instruction_set != kArm64)) {
        rl_src2 = LoadValue(rl_src2, kCoreReg);
        RegStorage t_reg = AllocTemp();
        OpRegRegImm(kOpAnd, t_reg, rl_src2.reg, 31);
        rl_src1 = LoadValue(rl_src1, kCoreReg);
        rl_result = EvalLoc(rl_dest, kCoreReg, true);
        OpRegRegReg(op, rl_result.reg, rl_src1.reg, t_reg);
        FreeTemp(t_reg);
      } else {
        rl_src1 = LoadValue(rl_src1, kCoreReg);
        rl_src2 = LoadValue(rl_src2, kCoreReg);
        rl_result = EvalLoc(rl_dest, kCoreReg, true);
        OpRegRegReg(op, rl_result.reg, rl_src1.reg, rl_src2.reg);
      }
    }
    StoreValue(rl_dest, rl_result);
  } else {
    bool done = false;      // Set to true if we happen to find a way to use a real instruction.
    if (cu_->instruction_set == kMips || cu_->instruction_set == kMips64 ||
        cu_->instruction_set == kArm64) {
      rl_src1 = LoadValue(rl_src1, kCoreReg);
      rl_src2 = LoadValue(rl_src2, kCoreReg);
      if (check_zero && (flags & MIR_IGNORE_DIV_ZERO_CHECK) == 0) {
        GenDivZeroCheck(rl_src2.reg);
      }
      rl_result = GenDivRem(rl_dest, rl_src1.reg, rl_src2.reg, op == kOpDiv);
      done = true;
    } else if (cu_->instruction_set == kThumb2) {
      if (cu_->compiler_driver->GetInstructionSetFeatures()->AsArmInstructionSetFeatures()->
              HasDivideInstruction()) {
        // Use ARM SDIV instruction for division.  For remainder we also need to
        // calculate using a MUL and subtract.
        rl_src1 = LoadValue(rl_src1, kCoreReg);
        rl_src2 = LoadValue(rl_src2, kCoreReg);
        if (check_zero && (flags & MIR_IGNORE_DIV_ZERO_CHECK) == 0) {
          GenDivZeroCheck(rl_src2.reg);
        }
        rl_result = GenDivRem(rl_dest, rl_src1.reg, rl_src2.reg, op == kOpDiv);
        done = true;
      }
    }

    // If we haven't already generated the code use the callout function.
    if (!done) {
      FlushAllRegs();   /* Send everything to home location */
      LoadValueDirectFixed(rl_src2, TargetReg(kArg1, kNotWide));
      RegStorage r_tgt = CallHelperSetup(kQuickIdivmod);
      LoadValueDirectFixed(rl_src1, TargetReg(kArg0, kNotWide));
      if (check_zero && (flags & MIR_IGNORE_DIV_ZERO_CHECK) == 0) {
        GenDivZeroCheck(TargetReg(kArg1, kNotWide));
      }
      // NOTE: callout here is not a safepoint.
      CallHelper(r_tgt, kQuickIdivmod, false /* not a safepoint */);
      if (op == kOpDiv)
        rl_result = GetReturn(kCoreReg);
      else
        rl_result = GetReturnAlt();
    }
    StoreValue(rl_dest, rl_result);
  }
}

/*
 * The following are the first-level codegen routines that analyze the format
 * of each bytecode then either dispatch special purpose codegen routines
 * or produce corresponding Thumb instructions directly.
 */

// Returns true if no more than two bits are set in 'x'.
static bool IsPopCountLE2(unsigned int x) {
  x &= x - 1;
  return (x & (x - 1)) == 0;
}

// Returns true if it added instructions to 'cu' to divide 'rl_src' by 'lit'
// and store the result in 'rl_dest'.
bool Mir2Lir::HandleEasyDivRem(Instruction::Code dalvik_opcode ATTRIBUTE_UNUSED, bool is_div,
                               RegLocation rl_src, RegLocation rl_dest, int lit) {
  if ((lit < 2) || (!IsPowerOfTwo(lit))) {
    return false;
  }
  int k = CTZ(lit);
  if (k >= 30) {
    // Avoid special cases.
    return false;
  }
  rl_src = LoadValue(rl_src, kCoreReg);
  RegLocation rl_result = EvalLoc(rl_dest, kCoreReg, true);
  if (is_div) {
    RegStorage t_reg = AllocTemp();
    if (lit == 2) {
      // Division by 2 is by far the most common division by constant.
      OpRegRegImm(kOpLsr, t_reg, rl_src.reg, 32 - k);
      OpRegRegReg(kOpAdd, t_reg, t_reg, rl_src.reg);
      OpRegRegImm(kOpAsr, rl_result.reg, t_reg, k);
    } else {
      OpRegRegImm(kOpAsr, t_reg, rl_src.reg, 31);
      OpRegRegImm(kOpLsr, t_reg, t_reg, 32 - k);
      OpRegRegReg(kOpAdd, t_reg, t_reg, rl_src.reg);
      OpRegRegImm(kOpAsr, rl_result.reg, t_reg, k);
    }
  } else {
    RegStorage t_reg1 = AllocTemp();
    RegStorage t_reg2 = AllocTemp();
    if (lit == 2) {
      OpRegRegImm(kOpLsr, t_reg1, rl_src.reg, 32 - k);
      OpRegRegReg(kOpAdd, t_reg2, t_reg1, rl_src.reg);
      OpRegRegImm(kOpAnd, t_reg2, t_reg2, lit -1);
      OpRegRegReg(kOpSub, rl_result.reg, t_reg2, t_reg1);
    } else {
      OpRegRegImm(kOpAsr, t_reg1, rl_src.reg, 31);
      OpRegRegImm(kOpLsr, t_reg1, t_reg1, 32 - k);
      OpRegRegReg(kOpAdd, t_reg2, t_reg1, rl_src.reg);
      OpRegRegImm(kOpAnd, t_reg2, t_reg2, lit - 1);
      OpRegRegReg(kOpSub, rl_result.reg, t_reg2, t_reg1);
    }
  }
  StoreValue(rl_dest, rl_result);
  return true;
}

// Returns true if it added instructions to 'cu' to multiply 'rl_src' by 'lit'
// and store the result in 'rl_dest'.
bool Mir2Lir::HandleEasyMultiply(RegLocation rl_src, RegLocation rl_dest, int lit) {
  if (lit < 0) {
    return false;
  }
  if (lit == 0) {
    RegLocation rl_result = EvalLoc(rl_dest, kCoreReg, true);
    LoadConstant(rl_result.reg, 0);
    StoreValue(rl_dest, rl_result);
    return true;
  }
  if (lit == 1) {
    rl_src = LoadValue(rl_src, kCoreReg);
    RegLocation rl_result = EvalLoc(rl_dest, kCoreReg, true);
    OpRegCopy(rl_result.reg, rl_src.reg);
    StoreValue(rl_dest, rl_result);
    return true;
  }
  // There is RegRegRegShift on Arm, so check for more special cases
  if (cu_->instruction_set == kThumb2) {
    return EasyMultiply(rl_src, rl_dest, lit);
  }
  // Can we simplify this multiplication?
  bool power_of_two = false;
  bool pop_count_le2 = false;
  bool power_of_two_minus_one = false;
  if (IsPowerOfTwo(lit)) {
    power_of_two = true;
  } else if (IsPopCountLE2(lit)) {
    pop_count_le2 = true;
  } else if (IsPowerOfTwo(lit + 1)) {
    power_of_two_minus_one = true;
  } else {
    return false;
  }
  rl_src = LoadValue(rl_src, kCoreReg);
  RegLocation rl_result = EvalLoc(rl_dest, kCoreReg, true);
  if (power_of_two) {
    // Shift.
    OpRegRegImm(kOpLsl, rl_result.reg, rl_src.reg, CTZ(lit));
  } else if (pop_count_le2) {
    // Shift and add and shift.
    int first_bit = CTZ(lit);
    int second_bit = CTZ(lit ^ (1 << first_bit));
    GenMultiplyByTwoBitMultiplier(rl_src, rl_result, lit, first_bit, second_bit);
  } else {
    // Reverse subtract: (src << (shift + 1)) - src.
    DCHECK(power_of_two_minus_one);
    // TUNING: rsb dst, src, src lsl#CTZ(lit + 1)
    RegStorage t_reg = AllocTemp();
    OpRegRegImm(kOpLsl, t_reg, rl_src.reg, CTZ(lit + 1));
    OpRegRegReg(kOpSub, rl_result.reg, t_reg, rl_src.reg);
  }
  StoreValue(rl_dest, rl_result);
  return true;
}

// Returns true if it generates instructions.
bool Mir2Lir::HandleEasyFloatingPointDiv(RegLocation rl_dest, RegLocation rl_src1,
                                         RegLocation rl_src2) {
  if (!rl_src2.is_const ||
      ((cu_->instruction_set != kThumb2) && (cu_->instruction_set != kArm64))) {
    return false;
  }

  if (!rl_src2.wide) {
    int32_t divisor = mir_graph_->ConstantValue(rl_src2);
    if (CanDivideByReciprocalMultiplyFloat(divisor)) {
      // Generate multiply by reciprocal instead of div.
      float recip = 1.0f/bit_cast<float, int32_t>(divisor);
      GenMultiplyByConstantFloat(rl_dest, rl_src1, bit_cast<int32_t, float>(recip));
      return true;
    }
  } else {
    int64_t divisor = mir_graph_->ConstantValueWide(rl_src2);
    if (CanDivideByReciprocalMultiplyDouble(divisor)) {
      // Generate multiply by reciprocal instead of div.
      double recip = 1.0/bit_cast<double, int64_t>(divisor);
      GenMultiplyByConstantDouble(rl_dest, rl_src1, bit_cast<int64_t, double>(recip));
      return true;
    }
  }
  return false;
}

void Mir2Lir::GenArithOpIntLit(Instruction::Code opcode, RegLocation rl_dest, RegLocation rl_src,
                               int lit) {
  RegLocation rl_result;
  OpKind op = static_cast<OpKind>(0);    /* Make gcc happy */
  int shift_op = false;
  bool is_div = false;

  switch (opcode) {
    case Instruction::RSUB_INT_LIT8:
    case Instruction::RSUB_INT: {
      rl_src = LoadValue(rl_src, kCoreReg);
      rl_result = EvalLoc(rl_dest, kCoreReg, true);
      if (cu_->instruction_set == kThumb2) {
        OpRegRegImm(kOpRsub, rl_result.reg, rl_src.reg, lit);
      } else {
        OpRegReg(kOpNeg, rl_result.reg, rl_src.reg);
        OpRegImm(kOpAdd, rl_result.reg, lit);
      }
      StoreValue(rl_dest, rl_result);
      return;
    }

    case Instruction::SUB_INT:
    case Instruction::SUB_INT_2ADDR:
      lit = -lit;
      FALLTHROUGH_INTENDED;
    case Instruction::ADD_INT:
    case Instruction::ADD_INT_2ADDR:
    case Instruction::ADD_INT_LIT8:
    case Instruction::ADD_INT_LIT16:
      op = kOpAdd;
      break;
    case Instruction::MUL_INT:
    case Instruction::MUL_INT_2ADDR:
    case Instruction::MUL_INT_LIT8:
    case Instruction::MUL_INT_LIT16: {
      if (HandleEasyMultiply(rl_src, rl_dest, lit)) {
        return;
      }
      op = kOpMul;
      break;
    }
    case Instruction::AND_INT:
    case Instruction::AND_INT_2ADDR:
    case Instruction::AND_INT_LIT8:
    case Instruction::AND_INT_LIT16:
      op = kOpAnd;
      break;
    case Instruction::OR_INT:
    case Instruction::OR_INT_2ADDR:
    case Instruction::OR_INT_LIT8:
    case Instruction::OR_INT_LIT16:
      op = kOpOr;
      break;
    case Instruction::XOR_INT:
    case Instruction::XOR_INT_2ADDR:
    case Instruction::XOR_INT_LIT8:
    case Instruction::XOR_INT_LIT16:
      op = kOpXor;
      break;
    case Instruction::SHL_INT_LIT8:
    case Instruction::SHL_INT:
    case Instruction::SHL_INT_2ADDR:
      lit &= 31;
      shift_op = true;
      op = kOpLsl;
      break;
    case Instruction::SHR_INT_LIT8:
    case Instruction::SHR_INT:
    case Instruction::SHR_INT_2ADDR:
      lit &= 31;
      shift_op = true;
      op = kOpAsr;
      break;
    case Instruction::USHR_INT_LIT8:
    case Instruction::USHR_INT:
    case Instruction::USHR_INT_2ADDR:
      lit &= 31;
      shift_op = true;
      op = kOpLsr;
      break;

    case Instruction::DIV_INT:
    case Instruction::DIV_INT_2ADDR:
    case Instruction::DIV_INT_LIT8:
    case Instruction::DIV_INT_LIT16:
    case Instruction::REM_INT:
    case Instruction::REM_INT_2ADDR:
    case Instruction::REM_INT_LIT8:
    case Instruction::REM_INT_LIT16: {
      if (lit == 0) {
        GenDivZeroException();
        return;
      }
      if ((opcode == Instruction::DIV_INT) ||
          (opcode == Instruction::DIV_INT_2ADDR) ||
          (opcode == Instruction::DIV_INT_LIT8) ||
          (opcode == Instruction::DIV_INT_LIT16)) {
        is_div = true;
      } else {
        is_div = false;
      }
      if (HandleEasyDivRem(opcode, is_div, rl_src, rl_dest, lit)) {
        return;
      }

      bool done = false;
      if (cu_->instruction_set == kMips || cu_->instruction_set == kMips64 ||
          cu_->instruction_set == kArm64) {
        rl_src = LoadValue(rl_src, kCoreReg);
        rl_result = GenDivRemLit(rl_dest, rl_src.reg, lit, is_div);
        done = true;
      } else if (cu_->instruction_set == kX86 || cu_->instruction_set == kX86_64) {
        rl_result = GenDivRemLit(rl_dest, rl_src, lit, is_div);
        done = true;
      } else if (cu_->instruction_set == kThumb2) {
        if (cu_->compiler_driver->GetInstructionSetFeatures()->AsArmInstructionSetFeatures()->
                HasDivideInstruction()) {
          // Use ARM SDIV instruction for division.  For remainder we also need to
          // calculate using a MUL and subtract.
          rl_src = LoadValue(rl_src, kCoreReg);
          rl_result = GenDivRemLit(rl_dest, rl_src.reg, lit, is_div);
          done = true;
        }
      }

      if (!done) {
        FlushAllRegs();   /* Everything to home location. */
        LoadValueDirectFixed(rl_src, TargetReg(kArg0, kNotWide));
        Clobber(TargetReg(kArg0, kNotWide));
        CallRuntimeHelperRegImm(kQuickIdivmod, TargetReg(kArg0, kNotWide), lit, false);
        if (is_div)
          rl_result = GetReturn(kCoreReg);
        else
          rl_result = GetReturnAlt();
      }
      StoreValue(rl_dest, rl_result);
      return;
    }
    default:
      LOG(FATAL) << "Unexpected opcode " << opcode;
  }
  rl_src = LoadValue(rl_src, kCoreReg);
  rl_result = EvalLoc(rl_dest, kCoreReg, true);
  // Avoid shifts by literal 0 - no support in Thumb.  Change to copy.
  if (shift_op && (lit == 0)) {
    OpRegCopy(rl_result.reg, rl_src.reg);
  } else {
    OpRegRegImm(op, rl_result.reg, rl_src.reg, lit);
  }
  StoreValue(rl_dest, rl_result);
}

void Mir2Lir::GenArithOpLong(Instruction::Code opcode, RegLocation rl_dest,
                             RegLocation rl_src1, RegLocation rl_src2, int flags) {
  RegLocation rl_result;
  OpKind first_op = kOpBkpt;
  OpKind second_op = kOpBkpt;
  bool call_out = false;
  bool check_zero = false;
  int ret_reg = TargetReg(kRet0, kNotWide).GetReg();
  QuickEntrypointEnum target;

  switch (opcode) {
    case Instruction::NOT_LONG:
      rl_src2 = LoadValueWide(rl_src2, kCoreReg);
      rl_result = EvalLoc(rl_dest, kCoreReg, true);
      // Check for destructive overlap
      if (rl_result.reg.GetLowReg() == rl_src2.reg.GetHighReg()) {
        RegStorage t_reg = AllocTemp();
        OpRegCopy(t_reg, rl_src2.reg.GetHigh());
        OpRegReg(kOpMvn, rl_result.reg.GetLow(), rl_src2.reg.GetLow());
        OpRegReg(kOpMvn, rl_result.reg.GetHigh(), t_reg);
        FreeTemp(t_reg);
      } else {
        OpRegReg(kOpMvn, rl_result.reg.GetLow(), rl_src2.reg.GetLow());
        OpRegReg(kOpMvn, rl_result.reg.GetHigh(), rl_src2.reg.GetHigh());
      }
      StoreValueWide(rl_dest, rl_result);
      return;
    case Instruction::ADD_LONG:
    case Instruction::ADD_LONG_2ADDR:
      first_op = kOpAdd;
      second_op = kOpAdc;
      break;
    case Instruction::SUB_LONG:
    case Instruction::SUB_LONG_2ADDR:
      first_op = kOpSub;
      second_op = kOpSbc;
      break;
    case Instruction::MUL_LONG:
    case Instruction::MUL_LONG_2ADDR:
      call_out = true;
      ret_reg = TargetReg(kRet0, kNotWide).GetReg();
      target = kQuickLmul;
      break;
    case Instruction::DIV_LONG:
    case Instruction::DIV_LONG_2ADDR:
      call_out = true;
      check_zero = true;
      ret_reg = TargetReg(kRet0, kNotWide).GetReg();
      target = kQuickLdiv;
      break;
    case Instruction::REM_LONG:
    case Instruction::REM_LONG_2ADDR:
      call_out = true;
      check_zero = true;
      target = kQuickLmod;
      /* NOTE - for Arm, result is in kArg2/kArg3 instead of kRet0/kRet1 */
      ret_reg = (cu_->instruction_set == kThumb2) ? TargetReg(kArg2, kNotWide).GetReg() :
          TargetReg(kRet0, kNotWide).GetReg();
      break;
    case Instruction::AND_LONG_2ADDR:
    case Instruction::AND_LONG:
      first_op = kOpAnd;
      second_op = kOpAnd;
      break;
    case Instruction::OR_LONG:
    case Instruction::OR_LONG_2ADDR:
      first_op = kOpOr;
      second_op = kOpOr;
      break;
    case Instruction::XOR_LONG:
    case Instruction::XOR_LONG_2ADDR:
      first_op = kOpXor;
      second_op = kOpXor;
      break;
    default:
      LOG(FATAL) << "Invalid long arith op";
  }
  if (!call_out) {
    GenLong3Addr(first_op, second_op, rl_dest, rl_src1, rl_src2);
  } else {
    FlushAllRegs();   /* Send everything to home location */
    if (check_zero) {
      RegStorage r_tmp1 = TargetReg(kArg0, kWide);
      RegStorage r_tmp2 = TargetReg(kArg2, kWide);
      LoadValueDirectWideFixed(rl_src2, r_tmp2);
      RegStorage r_tgt = CallHelperSetup(target);
      if ((flags & MIR_IGNORE_DIV_ZERO_CHECK) == 0) {
        GenDivZeroCheckWide(r_tmp2);
      }
      LoadValueDirectWideFixed(rl_src1, r_tmp1);
      // NOTE: callout here is not a safepoint
      CallHelper(r_tgt, target, false /* not safepoint */);
    } else {
      CallRuntimeHelperRegLocationRegLocation(target, rl_src1, rl_src2, false);
    }
    // Adjust return regs in to handle case of rem returning kArg2/kArg3
    if (ret_reg == TargetReg(kRet0, kNotWide).GetReg())
      rl_result = GetReturnWide(kCoreReg);
    else
      rl_result = GetReturnWideAlt();
    StoreValueWide(rl_dest, rl_result);
  }
}

void Mir2Lir::GenConst(RegLocation rl_dest, int value) {
  RegLocation rl_result = EvalLoc(rl_dest, kAnyReg, true);
  LoadConstantNoClobber(rl_result.reg, value);
  StoreValue(rl_dest, rl_result);
}

void Mir2Lir::GenConversionCall(QuickEntrypointEnum trampoline, RegLocation rl_dest,
                                RegLocation rl_src, RegisterClass return_reg_class) {
  /*
   * Don't optimize the register usage since it calls out to support
   * functions
   */

  FlushAllRegs();   /* Send everything to home location */
  CallRuntimeHelperRegLocation(trampoline, rl_src, false);
  if (rl_dest.wide) {
    RegLocation rl_result = GetReturnWide(return_reg_class);
    StoreValueWide(rl_dest, rl_result);
  } else {
    RegLocation rl_result = GetReturn(return_reg_class);
    StoreValue(rl_dest, rl_result);
  }
}

class Mir2Lir::SuspendCheckSlowPath : public Mir2Lir::LIRSlowPath {
 public:
  SuspendCheckSlowPath(Mir2Lir* m2l, LIR* branch, LIR* cont)
      : LIRSlowPath(m2l, branch, cont) {
  }

  void Compile() OVERRIDE {
    m2l_->ResetRegPool();
    m2l_->ResetDefTracking();
    GenerateTargetLabel(kPseudoSuspendTarget);
    m2l_->CallRuntimeHelper(kQuickTestSuspend, true);
    if (cont_ != nullptr) {
      m2l_->OpUnconditionalBranch(cont_);
    }
  }
};

/* Check if we need to check for pending suspend request */
void Mir2Lir::GenSuspendTest(int opt_flags) {
  if (NO_SUSPEND || (opt_flags & MIR_IGNORE_SUSPEND_CHECK) != 0) {
    return;
  }
  if (!cu_->compiler_driver->GetCompilerOptions().GetImplicitSuspendChecks()) {
    FlushAllRegs();
    LIR* branch = OpTestSuspend(nullptr);
    LIR* cont = NewLIR0(kPseudoTargetLabel);
    AddSlowPath(new (arena_) SuspendCheckSlowPath(this, branch, cont));
  } else {
    FlushAllRegs();     // TODO: needed?
    LIR* inst = CheckSuspendUsingLoad();
    MarkSafepointPC(inst);
  }
}

/* Check if we need to check for pending suspend request */
void Mir2Lir::GenSuspendTestAndBranch(int opt_flags, LIR* target) {
  if (NO_SUSPEND || (opt_flags & MIR_IGNORE_SUSPEND_CHECK) != 0) {
    OpUnconditionalBranch(target);
    return;
  }
  if (!cu_->compiler_driver->GetCompilerOptions().GetImplicitSuspendChecks()) {
    OpTestSuspend(target);
    FlushAllRegs();
    LIR* branch = OpUnconditionalBranch(nullptr);
    AddSlowPath(new (arena_) SuspendCheckSlowPath(this, branch, target));
  } else {
    // For the implicit suspend check, just perform the trigger
    // load and branch to the target.
    FlushAllRegs();
    LIR* inst = CheckSuspendUsingLoad();
    MarkSafepointPC(inst);
    OpUnconditionalBranch(target);
  }
}

/* Call out to helper assembly routine that will null check obj and then lock it. */
void Mir2Lir::GenMonitorEnter(int opt_flags, RegLocation rl_src) {
  UNUSED(opt_flags);  // TODO: avoid null check with specialized non-null helper.
  FlushAllRegs();
  CallRuntimeHelperRegLocation(kQuickLockObject, rl_src, true);
}

/* Call out to helper assembly routine that will null check obj and then unlock it. */
void Mir2Lir::GenMonitorExit(int opt_flags, RegLocation rl_src) {
  UNUSED(opt_flags);  // TODO: avoid null check with specialized non-null helper.
  FlushAllRegs();
  CallRuntimeHelperRegLocation(kQuickUnlockObject, rl_src, true);
}

/* Generic code for generating a wide constant into a VR. */
void Mir2Lir::GenConstWide(RegLocation rl_dest, int64_t value) {
  RegLocation rl_result = EvalLoc(rl_dest, kAnyReg, true);
  LoadConstantWide(rl_result.reg, value);
  StoreValueWide(rl_dest, rl_result);
}

void Mir2Lir::GenSmallPackedSwitch(MIR* mir, DexOffset table_offset, RegLocation rl_src) {
  BasicBlock* bb = mir_graph_->GetBasicBlock(mir->bb);
  DCHECK(bb != nullptr);
  ArenaVector<SuccessorBlockInfo*>::const_iterator succ_bb_iter = bb->successor_blocks.cbegin();
  const uint16_t* table = mir_graph_->GetTable(mir, table_offset);
  const uint16_t entries = table[1];
  // Chained cmp-and-branch.
  const int32_t* as_int32 = reinterpret_cast<const int32_t*>(&table[2]);
  int32_t starting_key = as_int32[0];
  rl_src = LoadValue(rl_src, kCoreReg);
  int i = 0;
  for (; i < entries; ++i, ++succ_bb_iter) {
    if (!InexpensiveConstantInt(starting_key + i, Instruction::Code::IF_EQ)) {
      // Switch to using a temp and add.
      break;
    }
    SuccessorBlockInfo* successor_block_info = *succ_bb_iter;
    DCHECK(successor_block_info != nullptr);
    int case_block_id = successor_block_info->block;
    DCHECK_EQ(starting_key + i, successor_block_info->key);
    OpCmpImmBranch(kCondEq, rl_src.reg, starting_key + i, &block_label_list_[case_block_id]);
  }
  if (i < entries) {
    // The rest do not seem to be inexpensive. Try to allocate a temp and use add.
    RegStorage key_temp = AllocTypedTemp(false, kCoreReg, false);
    if (key_temp.Valid()) {
      LoadConstantNoClobber(key_temp, starting_key + i);
      for (; i < entries - 1; ++i, ++succ_bb_iter) {
        SuccessorBlockInfo* successor_block_info = *succ_bb_iter;
        DCHECK(successor_block_info != nullptr);
        int case_block_id = successor_block_info->block;
        DCHECK_EQ(starting_key + i, successor_block_info->key);
        OpCmpBranch(kCondEq, rl_src.reg, key_temp, &block_label_list_[case_block_id]);
        OpRegImm(kOpAdd, key_temp, 1);  // Increment key.
      }
      SuccessorBlockInfo* successor_block_info = *succ_bb_iter;
      DCHECK(successor_block_info != nullptr);
      int case_block_id = successor_block_info->block;
      DCHECK_EQ(starting_key + i, successor_block_info->key);
      OpCmpBranch(kCondEq, rl_src.reg, key_temp, &block_label_list_[case_block_id]);
    } else {
      // No free temp, just finish the old loop.
      for (; i < entries; ++i, ++succ_bb_iter) {
        SuccessorBlockInfo* successor_block_info = *succ_bb_iter;
        DCHECK(successor_block_info != nullptr);
        int case_block_id = successor_block_info->block;
        DCHECK_EQ(starting_key + i, successor_block_info->key);
        OpCmpImmBranch(kCondEq, rl_src.reg, starting_key + i, &block_label_list_[case_block_id]);
      }
    }
  }
}

void Mir2Lir::GenPackedSwitch(MIR* mir, DexOffset table_offset, RegLocation rl_src) {
  const uint16_t* table = mir_graph_->GetTable(mir, table_offset);
  if (cu_->verbose) {
    DumpPackedSwitchTable(table);
  }

  const uint16_t entries = table[1];
  if (entries <= kSmallSwitchThreshold) {
    GenSmallPackedSwitch(mir, table_offset, rl_src);
  } else {
    // Use the backend-specific implementation.
    GenLargePackedSwitch(mir, table_offset, rl_src);
  }
}

void Mir2Lir::GenSmallSparseSwitch(MIR* mir, DexOffset table_offset, RegLocation rl_src) {
  BasicBlock* bb = mir_graph_->GetBasicBlock(mir->bb);
  DCHECK(bb != nullptr);
  const uint16_t* table = mir_graph_->GetTable(mir, table_offset);
  const uint16_t entries = table[1];
  // Chained cmp-and-branch.
  rl_src = LoadValue(rl_src, kCoreReg);
  int i = 0;
  for (SuccessorBlockInfo* successor_block_info : bb->successor_blocks) {
    int case_block_id = successor_block_info->block;
    int key = successor_block_info->key;
    OpCmpImmBranch(kCondEq, rl_src.reg, key, &block_label_list_[case_block_id]);
    i++;
  }
  DCHECK_EQ(i, entries);
}

void Mir2Lir::GenSparseSwitch(MIR* mir, DexOffset table_offset, RegLocation rl_src) {
  const uint16_t* table = mir_graph_->GetTable(mir, table_offset);
  if (cu_->verbose) {
    DumpSparseSwitchTable(table);
  }

  const uint16_t entries = table[1];
  if (entries <= kSmallSwitchThreshold) {
    GenSmallSparseSwitch(mir, table_offset, rl_src);
  } else {
    // Use the backend-specific implementation.
    GenLargeSparseSwitch(mir, table_offset, rl_src);
  }
}

bool Mir2Lir::SizeMatchesTypeForEntrypoint(OpSize size, Primitive::Type type) {
  switch (size) {
    case kReference:
      return type == Primitive::kPrimNot;
    case k64:
    case kDouble:
      return type == Primitive::kPrimLong || type == Primitive::kPrimDouble;
    case k32:
    case kSingle:
      return type == Primitive::kPrimInt || type == Primitive::kPrimFloat;
    case kSignedHalf:
      return type == Primitive::kPrimShort;
    case kUnsignedHalf:
      return type == Primitive::kPrimChar;
    case kSignedByte:
      return type == Primitive::kPrimByte;
    case kUnsignedByte:
      return type == Primitive::kPrimBoolean;
    case kWord:  // Intentional fallthrough.
    default:
      return false;  // There are no sane types with this op size.
  }
}

}  // namespace art
