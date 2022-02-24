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

#include "interpreter_common.h"

#include <cmath>

#include "debugger.h"
#include "mirror/array-inl.h"
#include "unstarted_runtime.h"
#include "verifier/method_verifier.h"

namespace art {
namespace interpreter {

void ThrowNullPointerExceptionFromInterpreter() {
  ThrowNullPointerExceptionFromDexPC();
}

template<FindFieldType find_type, Primitive::Type field_type, bool do_access_check>
bool DoFieldGet(Thread* self, ShadowFrame& shadow_frame, const Instruction* inst,
                uint16_t inst_data) {
  const bool is_static = (find_type == StaticObjectRead) || (find_type == StaticPrimitiveRead);
  const uint32_t field_idx = is_static ? inst->VRegB_21c() : inst->VRegC_22c();
  ArtField* f = FindFieldFromCode<find_type, do_access_check>(field_idx, shadow_frame.GetMethod(), self,
                                                              Primitive::ComponentSize(field_type));
  if (UNLIKELY(f == nullptr)) {
    CHECK(self->IsExceptionPending());
    return false;
  }
  Object* obj;
  if (is_static) {
    obj = f->GetDeclaringClass();
  } else {
    obj = shadow_frame.GetVRegReference(inst->VRegB_22c(inst_data));
    if (UNLIKELY(obj == nullptr)) {
      ThrowNullPointerExceptionForFieldAccess(f, true);
      return false;
    }
  }
  f->GetDeclaringClass()->AssertInitializedOrInitializingInThread(self);
  // Report this field access to instrumentation if needed.
  instrumentation::Instrumentation* instrumentation = Runtime::Current()->GetInstrumentation();
  if (UNLIKELY(instrumentation->HasFieldReadListeners())) {
    Object* this_object = f->IsStatic() ? nullptr : obj;
    instrumentation->FieldReadEvent(self, this_object, shadow_frame.GetMethod(),
                                    shadow_frame.GetDexPC(), f);
  }
  uint32_t vregA = is_static ? inst->VRegA_21c(inst_data) : inst->VRegA_22c(inst_data);
  switch (field_type) {
    case Primitive::kPrimBoolean:
      shadow_frame.SetVReg(vregA, f->GetBoolean(obj));
      break;
    case Primitive::kPrimByte:
      shadow_frame.SetVReg(vregA, f->GetByte(obj));
      break;
    case Primitive::kPrimChar:
      shadow_frame.SetVReg(vregA, f->GetChar(obj));
      break;
    case Primitive::kPrimShort:
      shadow_frame.SetVReg(vregA, f->GetShort(obj));
      break;
    case Primitive::kPrimInt:
      shadow_frame.SetVReg(vregA, f->GetInt(obj));
      break;
    case Primitive::kPrimLong:
      shadow_frame.SetVRegLong(vregA, f->GetLong(obj));
      break;
    case Primitive::kPrimNot:
      shadow_frame.SetVRegReference(vregA, f->GetObject(obj));
      break;
    default:
      LOG(FATAL) << "Unreachable: " << field_type;
      UNREACHABLE();
  }
  return true;
}

// Explicitly instantiate all DoFieldGet functions.
#define EXPLICIT_DO_FIELD_GET_TEMPLATE_DECL(_find_type, _field_type, _do_check) \
  template bool DoFieldGet<_find_type, _field_type, _do_check>(Thread* self, \
                                                               ShadowFrame& shadow_frame, \
                                                               const Instruction* inst, \
                                                               uint16_t inst_data)

#define EXPLICIT_DO_FIELD_GET_ALL_TEMPLATE_DECL(_find_type, _field_type)  \
    EXPLICIT_DO_FIELD_GET_TEMPLATE_DECL(_find_type, _field_type, false);  \
    EXPLICIT_DO_FIELD_GET_TEMPLATE_DECL(_find_type, _field_type, true);

// iget-XXX
EXPLICIT_DO_FIELD_GET_ALL_TEMPLATE_DECL(InstancePrimitiveRead, Primitive::kPrimBoolean)
EXPLICIT_DO_FIELD_GET_ALL_TEMPLATE_DECL(InstancePrimitiveRead, Primitive::kPrimByte)
EXPLICIT_DO_FIELD_GET_ALL_TEMPLATE_DECL(InstancePrimitiveRead, Primitive::kPrimChar)
EXPLICIT_DO_FIELD_GET_ALL_TEMPLATE_DECL(InstancePrimitiveRead, Primitive::kPrimShort)
EXPLICIT_DO_FIELD_GET_ALL_TEMPLATE_DECL(InstancePrimitiveRead, Primitive::kPrimInt)
EXPLICIT_DO_FIELD_GET_ALL_TEMPLATE_DECL(InstancePrimitiveRead, Primitive::kPrimLong)
EXPLICIT_DO_FIELD_GET_ALL_TEMPLATE_DECL(InstanceObjectRead, Primitive::kPrimNot)

// sget-XXX
EXPLICIT_DO_FIELD_GET_ALL_TEMPLATE_DECL(StaticPrimitiveRead, Primitive::kPrimBoolean)
EXPLICIT_DO_FIELD_GET_ALL_TEMPLATE_DECL(StaticPrimitiveRead, Primitive::kPrimByte)
EXPLICIT_DO_FIELD_GET_ALL_TEMPLATE_DECL(StaticPrimitiveRead, Primitive::kPrimChar)
EXPLICIT_DO_FIELD_GET_ALL_TEMPLATE_DECL(StaticPrimitiveRead, Primitive::kPrimShort)
EXPLICIT_DO_FIELD_GET_ALL_TEMPLATE_DECL(StaticPrimitiveRead, Primitive::kPrimInt)
EXPLICIT_DO_FIELD_GET_ALL_TEMPLATE_DECL(StaticPrimitiveRead, Primitive::kPrimLong)
EXPLICIT_DO_FIELD_GET_ALL_TEMPLATE_DECL(StaticObjectRead, Primitive::kPrimNot)

#undef EXPLICIT_DO_FIELD_GET_ALL_TEMPLATE_DECL
#undef EXPLICIT_DO_FIELD_GET_TEMPLATE_DECL

// Handles iget-quick, iget-wide-quick and iget-object-quick instructions.
// Returns true on success, otherwise throws an exception and returns false.
template<Primitive::Type field_type>
bool DoIGetQuick(ShadowFrame& shadow_frame, const Instruction* inst, uint16_t inst_data) {
  Object* obj = shadow_frame.GetVRegReference(inst->VRegB_22c(inst_data));
  if (UNLIKELY(obj == nullptr)) {
    // We lost the reference to the field index so we cannot get a more
    // precised exception message.
    ThrowNullPointerExceptionFromDexPC();
    return false;
  }
  MemberOffset field_offset(inst->VRegC_22c());
  // Report this field access to instrumentation if needed. Since we only have the offset of
  // the field from the base of the object, we need to look for it first.
  instrumentation::Instrumentation* instrumentation = Runtime::Current()->GetInstrumentation();
  if (UNLIKELY(instrumentation->HasFieldReadListeners())) {
    ArtField* f = ArtField::FindInstanceFieldWithOffset(obj->GetClass(),
                                                        field_offset.Uint32Value());
    DCHECK(f != nullptr);
    DCHECK(!f->IsStatic());
    instrumentation->FieldReadEvent(Thread::Current(), obj, shadow_frame.GetMethod(),
                                    shadow_frame.GetDexPC(), f);
  }
  // Note: iget-x-quick instructions are only for non-volatile fields.
  const uint32_t vregA = inst->VRegA_22c(inst_data);
  switch (field_type) {
    case Primitive::kPrimInt:
      shadow_frame.SetVReg(vregA, static_cast<int32_t>(obj->GetField32(field_offset)));
      break;
    case Primitive::kPrimBoolean:
      shadow_frame.SetVReg(vregA, static_cast<int32_t>(obj->GetFieldBoolean(field_offset)));
      break;
    case Primitive::kPrimByte:
      shadow_frame.SetVReg(vregA, static_cast<int32_t>(obj->GetFieldByte(field_offset)));
      break;
    case Primitive::kPrimChar:
      shadow_frame.SetVReg(vregA, static_cast<int32_t>(obj->GetFieldChar(field_offset)));
      break;
    case Primitive::kPrimShort:
      shadow_frame.SetVReg(vregA, static_cast<int32_t>(obj->GetFieldShort(field_offset)));
      break;
    case Primitive::kPrimLong:
      shadow_frame.SetVRegLong(vregA, static_cast<int64_t>(obj->GetField64(field_offset)));
      break;
    case Primitive::kPrimNot:
      shadow_frame.SetVRegReference(vregA, obj->GetFieldObject<mirror::Object>(field_offset));
      break;
    default:
      LOG(FATAL) << "Unreachable: " << field_type;
      UNREACHABLE();
  }
  return true;
}

// Explicitly instantiate all DoIGetQuick functions.
#define EXPLICIT_DO_IGET_QUICK_TEMPLATE_DECL(_field_type) \
  template bool DoIGetQuick<_field_type>(ShadowFrame& shadow_frame, const Instruction* inst, \
                                         uint16_t inst_data)

EXPLICIT_DO_IGET_QUICK_TEMPLATE_DECL(Primitive::kPrimInt);      // iget-quick.
EXPLICIT_DO_IGET_QUICK_TEMPLATE_DECL(Primitive::kPrimBoolean);  // iget-boolean-quick.
EXPLICIT_DO_IGET_QUICK_TEMPLATE_DECL(Primitive::kPrimByte);     // iget-byte-quick.
EXPLICIT_DO_IGET_QUICK_TEMPLATE_DECL(Primitive::kPrimChar);     // iget-char-quick.
EXPLICIT_DO_IGET_QUICK_TEMPLATE_DECL(Primitive::kPrimShort);    // iget-short-quick.
EXPLICIT_DO_IGET_QUICK_TEMPLATE_DECL(Primitive::kPrimLong);     // iget-wide-quick.
EXPLICIT_DO_IGET_QUICK_TEMPLATE_DECL(Primitive::kPrimNot);      // iget-object-quick.
#undef EXPLICIT_DO_IGET_QUICK_TEMPLATE_DECL

template<Primitive::Type field_type>
static JValue GetFieldValue(const ShadowFrame& shadow_frame, uint32_t vreg)
    SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
  JValue field_value;
  switch (field_type) {
    case Primitive::kPrimBoolean:
      field_value.SetZ(static_cast<uint8_t>(shadow_frame.GetVReg(vreg)));
      break;
    case Primitive::kPrimByte:
      field_value.SetB(static_cast<int8_t>(shadow_frame.GetVReg(vreg)));
      break;
    case Primitive::kPrimChar:
      field_value.SetC(static_cast<uint16_t>(shadow_frame.GetVReg(vreg)));
      break;
    case Primitive::kPrimShort:
      field_value.SetS(static_cast<int16_t>(shadow_frame.GetVReg(vreg)));
      break;
    case Primitive::kPrimInt:
      field_value.SetI(shadow_frame.GetVReg(vreg));
      break;
    case Primitive::kPrimLong:
      field_value.SetJ(shadow_frame.GetVRegLong(vreg));
      break;
    case Primitive::kPrimNot:
      field_value.SetL(shadow_frame.GetVRegReference(vreg));
      break;
    default:
      LOG(FATAL) << "Unreachable: " << field_type;
      UNREACHABLE();
  }
  return field_value;
}

template<FindFieldType find_type, Primitive::Type field_type, bool do_access_check,
         bool transaction_active>
bool DoFieldPut(Thread* self, const ShadowFrame& shadow_frame, const Instruction* inst,
                uint16_t inst_data) {
  bool do_assignability_check = do_access_check;
  bool is_static = (find_type == StaticObjectWrite) || (find_type == StaticPrimitiveWrite);
  uint32_t field_idx = is_static ? inst->VRegB_21c() : inst->VRegC_22c();
  ArtField* f = FindFieldFromCode<find_type, do_access_check>(field_idx, shadow_frame.GetMethod(), self,
                                                              Primitive::ComponentSize(field_type));
  if (UNLIKELY(f == nullptr)) {
    CHECK(self->IsExceptionPending());
    return false;
  }
  Object* obj;
  if (is_static) {
    obj = f->GetDeclaringClass();
  } else {
    obj = shadow_frame.GetVRegReference(inst->VRegB_22c(inst_data));
    if (UNLIKELY(obj == nullptr)) {
      ThrowNullPointerExceptionForFieldAccess(f, false);
      return false;
    }
  }
  f->GetDeclaringClass()->AssertInitializedOrInitializingInThread(self);
  uint32_t vregA = is_static ? inst->VRegA_21c(inst_data) : inst->VRegA_22c(inst_data);
  // Report this field access to instrumentation if needed. Since we only have the offset of
  // the field from the base of the object, we need to look for it first.
  instrumentation::Instrumentation* instrumentation = Runtime::Current()->GetInstrumentation();
  if (UNLIKELY(instrumentation->HasFieldWriteListeners())) {
    JValue field_value = GetFieldValue<field_type>(shadow_frame, vregA);
    Object* this_object = f->IsStatic() ? nullptr : obj;
    instrumentation->FieldWriteEvent(self, this_object, shadow_frame.GetMethod(),
                                     shadow_frame.GetDexPC(), f, field_value);
  }
  switch (field_type) {
    case Primitive::kPrimBoolean:
      f->SetBoolean<transaction_active>(obj, shadow_frame.GetVReg(vregA));
      break;
    case Primitive::kPrimByte:
      f->SetByte<transaction_active>(obj, shadow_frame.GetVReg(vregA));
      break;
    case Primitive::kPrimChar:
      f->SetChar<transaction_active>(obj, shadow_frame.GetVReg(vregA));
      break;
    case Primitive::kPrimShort:
      f->SetShort<transaction_active>(obj, shadow_frame.GetVReg(vregA));
      break;
    case Primitive::kPrimInt:
      f->SetInt<transaction_active>(obj, shadow_frame.GetVReg(vregA));
      break;
    case Primitive::kPrimLong:
      f->SetLong<transaction_active>(obj, shadow_frame.GetVRegLong(vregA));
      break;
    case Primitive::kPrimNot: {
      Object* reg = shadow_frame.GetVRegReference(vregA);
      if (do_assignability_check && reg != nullptr) {
        // FieldHelper::GetType can resolve classes, use a handle wrapper which will restore the
        // object in the destructor.
        Class* field_class;
        {
          StackHandleScope<2> hs(self);
          HandleWrapper<mirror::Object> h_reg(hs.NewHandleWrapper(&reg));
          HandleWrapper<mirror::Object> h_obj(hs.NewHandleWrapper(&obj));
          field_class = f->GetType<true>();
        }
        if (!reg->VerifierInstanceOf(field_class)) {
          // This should never happen.
          std::string temp1, temp2, temp3;
          self->ThrowNewExceptionF("Ljava/lang/VirtualMachineError;",
                                   "Put '%s' that is not instance of field '%s' in '%s'",
                                   reg->GetClass()->GetDescriptor(&temp1),
                                   field_class->GetDescriptor(&temp2),
                                   f->GetDeclaringClass()->GetDescriptor(&temp3));
          return false;
        }
      }
      f->SetObj<transaction_active>(obj, reg);
      break;
    }
    default:
      LOG(FATAL) << "Unreachable: " << field_type;
      UNREACHABLE();
  }
  return true;
}

// Explicitly instantiate all DoFieldPut functions.
#define EXPLICIT_DO_FIELD_PUT_TEMPLATE_DECL(_find_type, _field_type, _do_check, _transaction_active) \
  template bool DoFieldPut<_find_type, _field_type, _do_check, _transaction_active>(Thread* self, \
      const ShadowFrame& shadow_frame, const Instruction* inst, uint16_t inst_data)

#define EXPLICIT_DO_FIELD_PUT_ALL_TEMPLATE_DECL(_find_type, _field_type)  \
    EXPLICIT_DO_FIELD_PUT_TEMPLATE_DECL(_find_type, _field_type, false, false);  \
    EXPLICIT_DO_FIELD_PUT_TEMPLATE_DECL(_find_type, _field_type, true, false);  \
    EXPLICIT_DO_FIELD_PUT_TEMPLATE_DECL(_find_type, _field_type, false, true);  \
    EXPLICIT_DO_FIELD_PUT_TEMPLATE_DECL(_find_type, _field_type, true, true);

// iput-XXX
EXPLICIT_DO_FIELD_PUT_ALL_TEMPLATE_DECL(InstancePrimitiveWrite, Primitive::kPrimBoolean)
EXPLICIT_DO_FIELD_PUT_ALL_TEMPLATE_DECL(InstancePrimitiveWrite, Primitive::kPrimByte)
EXPLICIT_DO_FIELD_PUT_ALL_TEMPLATE_DECL(InstancePrimitiveWrite, Primitive::kPrimChar)
EXPLICIT_DO_FIELD_PUT_ALL_TEMPLATE_DECL(InstancePrimitiveWrite, Primitive::kPrimShort)
EXPLICIT_DO_FIELD_PUT_ALL_TEMPLATE_DECL(InstancePrimitiveWrite, Primitive::kPrimInt)
EXPLICIT_DO_FIELD_PUT_ALL_TEMPLATE_DECL(InstancePrimitiveWrite, Primitive::kPrimLong)
EXPLICIT_DO_FIELD_PUT_ALL_TEMPLATE_DECL(InstanceObjectWrite, Primitive::kPrimNot)

// sput-XXX
EXPLICIT_DO_FIELD_PUT_ALL_TEMPLATE_DECL(StaticPrimitiveWrite, Primitive::kPrimBoolean)
EXPLICIT_DO_FIELD_PUT_ALL_TEMPLATE_DECL(StaticPrimitiveWrite, Primitive::kPrimByte)
EXPLICIT_DO_FIELD_PUT_ALL_TEMPLATE_DECL(StaticPrimitiveWrite, Primitive::kPrimChar)
EXPLICIT_DO_FIELD_PUT_ALL_TEMPLATE_DECL(StaticPrimitiveWrite, Primitive::kPrimShort)
EXPLICIT_DO_FIELD_PUT_ALL_TEMPLATE_DECL(StaticPrimitiveWrite, Primitive::kPrimInt)
EXPLICIT_DO_FIELD_PUT_ALL_TEMPLATE_DECL(StaticPrimitiveWrite, Primitive::kPrimLong)
EXPLICIT_DO_FIELD_PUT_ALL_TEMPLATE_DECL(StaticObjectWrite, Primitive::kPrimNot)

#undef EXPLICIT_DO_FIELD_PUT_ALL_TEMPLATE_DECL
#undef EXPLICIT_DO_FIELD_PUT_TEMPLATE_DECL

template<Primitive::Type field_type, bool transaction_active>
bool DoIPutQuick(const ShadowFrame& shadow_frame, const Instruction* inst, uint16_t inst_data) {
  Object* obj = shadow_frame.GetVRegReference(inst->VRegB_22c(inst_data));
  if (UNLIKELY(obj == nullptr)) {
    // We lost the reference to the field index so we cannot get a more
    // precised exception message.
    ThrowNullPointerExceptionFromDexPC();
    return false;
  }
  MemberOffset field_offset(inst->VRegC_22c());
  const uint32_t vregA = inst->VRegA_22c(inst_data);
  // Report this field modification to instrumentation if needed. Since we only have the offset of
  // the field from the base of the object, we need to look for it first.
  instrumentation::Instrumentation* instrumentation = Runtime::Current()->GetInstrumentation();
  if (UNLIKELY(instrumentation->HasFieldWriteListeners())) {
    ArtField* f = ArtField::FindInstanceFieldWithOffset(obj->GetClass(),
                                                        field_offset.Uint32Value());
    DCHECK(f != nullptr);
    DCHECK(!f->IsStatic());
    JValue field_value = GetFieldValue<field_type>(shadow_frame, vregA);
    instrumentation->FieldWriteEvent(Thread::Current(), obj, shadow_frame.GetMethod(),
                                     shadow_frame.GetDexPC(), f, field_value);
  }
  // Note: iput-x-quick instructions are only for non-volatile fields.
  switch (field_type) {
    case Primitive::kPrimBoolean:
      obj->SetFieldBoolean<transaction_active>(field_offset, shadow_frame.GetVReg(vregA));
      break;
    case Primitive::kPrimByte:
      obj->SetFieldByte<transaction_active>(field_offset, shadow_frame.GetVReg(vregA));
      break;
    case Primitive::kPrimChar:
      obj->SetFieldChar<transaction_active>(field_offset, shadow_frame.GetVReg(vregA));
      break;
    case Primitive::kPrimShort:
      obj->SetFieldShort<transaction_active>(field_offset, shadow_frame.GetVReg(vregA));
      break;
    case Primitive::kPrimInt:
      obj->SetField32<transaction_active>(field_offset, shadow_frame.GetVReg(vregA));
      break;
    case Primitive::kPrimLong:
      obj->SetField64<transaction_active>(field_offset, shadow_frame.GetVRegLong(vregA));
      break;
    case Primitive::kPrimNot:
      obj->SetFieldObject<transaction_active>(field_offset, shadow_frame.GetVRegReference(vregA));
      break;
    default:
      LOG(FATAL) << "Unreachable: " << field_type;
      UNREACHABLE();
  }
  return true;
}

// Explicitly instantiate all DoIPutQuick functions.
#define EXPLICIT_DO_IPUT_QUICK_TEMPLATE_DECL(_field_type, _transaction_active) \
  template bool DoIPutQuick<_field_type, _transaction_active>(const ShadowFrame& shadow_frame, \
                                                              const Instruction* inst, \
                                                              uint16_t inst_data)

#define EXPLICIT_DO_IPUT_QUICK_ALL_TEMPLATE_DECL(_field_type)   \
  EXPLICIT_DO_IPUT_QUICK_TEMPLATE_DECL(_field_type, false);     \
  EXPLICIT_DO_IPUT_QUICK_TEMPLATE_DECL(_field_type, true);

EXPLICIT_DO_IPUT_QUICK_ALL_TEMPLATE_DECL(Primitive::kPrimInt)      // iput-quick.
EXPLICIT_DO_IPUT_QUICK_ALL_TEMPLATE_DECL(Primitive::kPrimBoolean)  // iput-boolean-quick.
EXPLICIT_DO_IPUT_QUICK_ALL_TEMPLATE_DECL(Primitive::kPrimByte)     // iput-byte-quick.
EXPLICIT_DO_IPUT_QUICK_ALL_TEMPLATE_DECL(Primitive::kPrimChar)     // iput-char-quick.
EXPLICIT_DO_IPUT_QUICK_ALL_TEMPLATE_DECL(Primitive::kPrimShort)    // iput-short-quick.
EXPLICIT_DO_IPUT_QUICK_ALL_TEMPLATE_DECL(Primitive::kPrimLong)     // iput-wide-quick.
EXPLICIT_DO_IPUT_QUICK_ALL_TEMPLATE_DECL(Primitive::kPrimNot)      // iput-object-quick.
#undef EXPLICIT_DO_IPUT_QUICK_ALL_TEMPLATE_DECL
#undef EXPLICIT_DO_IPUT_QUICK_TEMPLATE_DECL

uint32_t FindNextInstructionFollowingException(
    Thread* self, ShadowFrame& shadow_frame, uint32_t dex_pc,
    const instrumentation::Instrumentation* instrumentation) {
  self->VerifyStack();
  StackHandleScope<2> hs(self);
  Handle<mirror::Throwable> exception(hs.NewHandle(self->GetException()));
  if (instrumentation->HasExceptionCaughtListeners()
      && self->IsExceptionThrownByCurrentMethod(exception.Get())) {
    instrumentation->ExceptionCaughtEvent(self, exception.Get());
  }
  bool clear_exception = false;
  uint32_t found_dex_pc = shadow_frame.GetMethod()->FindCatchBlock(
      hs.NewHandle(exception->GetClass()), dex_pc, &clear_exception);
  if (found_dex_pc == DexFile::kDexNoIndex) {
    // Exception is not caught by the current method. We will unwind to the
    // caller. Notify any instrumentation listener.
    instrumentation->MethodUnwindEvent(self, shadow_frame.GetThisObject(),
                                       shadow_frame.GetMethod(), dex_pc);
  } else {
    // Exception is caught in the current method. We will jump to the found_dex_pc.
    if (clear_exception) {
      self->ClearException();
    }
  }
  return found_dex_pc;
}

void UnexpectedOpcode(const Instruction* inst, const ShadowFrame& shadow_frame) {
  LOG(FATAL) << "Unexpected instruction: "
             << inst->DumpString(shadow_frame.GetMethod()->GetDexFile());
  UNREACHABLE();
}

// Assign register 'src_reg' from shadow_frame to register 'dest_reg' into new_shadow_frame.
static inline void AssignRegister(ShadowFrame* new_shadow_frame, const ShadowFrame& shadow_frame,
                                  size_t dest_reg, size_t src_reg)
    SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
  // If both register locations contains the same value, the register probably holds a reference.
  // Uint required, so that sign extension does not make this wrong on 64b systems
  uint32_t src_value = shadow_frame.GetVReg(src_reg);
  mirror::Object* o = shadow_frame.GetVRegReference<kVerifyNone>(src_reg);
  if (src_value == reinterpret_cast<uintptr_t>(o)) {
    new_shadow_frame->SetVRegReference(dest_reg, o);
  } else {
    new_shadow_frame->SetVReg(dest_reg, src_value);
  }
}

void AbortTransactionF(Thread* self, const char* fmt, ...) {
  va_list args;
  va_start(args, fmt);
  AbortTransactionV(self, fmt, args);
  va_end(args);
}

void AbortTransactionV(Thread* self, const char* fmt, va_list args) {
  CHECK(Runtime::Current()->IsActiveTransaction());
  // Constructs abort message.
  std::string abort_msg;
  StringAppendV(&abort_msg, fmt, args);
  // Throws an exception so we can abort the transaction and rollback every change.
  Runtime::Current()->AbortTransactionAndThrowAbortError(self, abort_msg);
}

template<bool is_range, bool do_assignability_check>
bool DoCall(ArtMethod* called_method, Thread* self, ShadowFrame& shadow_frame,
            const Instruction* inst, uint16_t inst_data, JValue* result) {
  bool string_init = false;
  // Replace calls to String.<init> with equivalent StringFactory call.
  if (called_method->GetDeclaringClass()->IsStringClass() && called_method->IsConstructor()) {
    ScopedObjectAccessUnchecked soa(self);
    jmethodID mid = soa.EncodeMethod(called_method);
    called_method = soa.DecodeMethod(WellKnownClasses::StringInitToStringFactoryMethodID(mid));
    string_init = true;
  }

  // Compute method information.
  const DexFile::CodeItem* code_item = called_method->GetCodeItem();
  const uint16_t num_ins = (is_range) ? inst->VRegA_3rc(inst_data) : inst->VRegA_35c(inst_data);
  uint16_t num_regs;
  if (LIKELY(code_item != nullptr)) {
    num_regs = code_item->registers_size_;
    DCHECK_EQ(string_init ? num_ins - 1 : num_ins, code_item->ins_size_);
  } else {
    DCHECK(called_method->IsNative() || called_method->IsProxyMethod());
    num_regs = num_ins;
    if (string_init) {
      // The new StringFactory call is static and has one fewer argument.
      num_regs--;
    }
  }

  // Allocate shadow frame on the stack.
  const char* old_cause = self->StartAssertNoThreadSuspension("DoCall");
  void* memory = alloca(ShadowFrame::ComputeSize(num_regs));
  ShadowFrame* new_shadow_frame(ShadowFrame::Create(num_regs, &shadow_frame, called_method, 0,
                                                    memory));

  // Initialize new shadow frame.
  size_t first_dest_reg = num_regs - num_ins;
  if (do_assignability_check) {
    // Slow path.
    // We might need to do class loading, which incurs a thread state change to kNative. So
    // register the shadow frame as under construction and allow suspension again.
    ScopedStackedShadowFramePusher pusher(
        self, new_shadow_frame, StackedShadowFrameType::kShadowFrameUnderConstruction);
    self->EndAssertNoThreadSuspension(old_cause);

    // We need to do runtime check on reference assignment. We need to load the shorty
    // to get the exact type of each reference argument.
    const DexFile::TypeList* params = new_shadow_frame->GetMethod()->GetParameterTypeList();
    uint32_t shorty_len = 0;
    const char* shorty = new_shadow_frame->GetMethod()->GetShorty(&shorty_len);

    // TODO: find a cleaner way to separate non-range and range information without duplicating
    //       code.
    uint32_t arg[5];  // only used in invoke-XXX.
    uint32_t vregC;   // only used in invoke-XXX-range.
    if (is_range) {
      vregC = inst->VRegC_3rc();
    } else {
      inst->GetVarArgs(arg, inst_data);
    }

    // Handle receiver apart since it's not part of the shorty.
    size_t dest_reg = first_dest_reg;
    size_t arg_offset = 0;
    if (!new_shadow_frame->GetMethod()->IsStatic()) {
      size_t receiver_reg = is_range ? vregC : arg[0];
      new_shadow_frame->SetVRegReference(dest_reg, shadow_frame.GetVRegReference(receiver_reg));
      ++dest_reg;
      ++arg_offset;
    } else if (string_init) {
      // Skip the referrer for the new static StringFactory call.
      ++dest_reg;
      ++arg_offset;
    }
    for (uint32_t shorty_pos = 0; dest_reg < num_regs; ++shorty_pos, ++dest_reg, ++arg_offset) {
      DCHECK_LT(shorty_pos + 1, shorty_len);
      const size_t src_reg = (is_range) ? vregC + arg_offset : arg[arg_offset];
      switch (shorty[shorty_pos + 1]) {
        case 'L': {
          Object* o = shadow_frame.GetVRegReference(src_reg);
          if (do_assignability_check && o != nullptr) {
            Class* arg_type =
                new_shadow_frame->GetMethod()->GetClassFromTypeIndex(
                    params->GetTypeItem(shorty_pos).type_idx_, true);
            if (arg_type == nullptr) {
              CHECK(self->IsExceptionPending());
              return false;
            }
            if (!o->VerifierInstanceOf(arg_type)) {
              // This should never happen.
              std::string temp1, temp2;
              self->ThrowNewExceptionF("Ljava/lang/VirtualMachineError;",
                                       "Invoking %s with bad arg %d, type '%s' not instance of '%s'",
                                       new_shadow_frame->GetMethod()->GetName(), shorty_pos,
                                       o->GetClass()->GetDescriptor(&temp1),
                                       arg_type->GetDescriptor(&temp2));
              return false;
            }
          }
          new_shadow_frame->SetVRegReference(dest_reg, o);
          break;
        }
        case 'J': case 'D': {
          uint64_t wide_value = (static_cast<uint64_t>(shadow_frame.GetVReg(src_reg + 1)) << 32) |
                                static_cast<uint32_t>(shadow_frame.GetVReg(src_reg));
          new_shadow_frame->SetVRegLong(dest_reg, wide_value);
          ++dest_reg;
          ++arg_offset;
          break;
        }
        default:
          new_shadow_frame->SetVReg(dest_reg, shadow_frame.GetVReg(src_reg));
          break;
      }
    }
  } else {
    // Fast path: no extra checks.
    if (is_range) {
      uint16_t first_src_reg = inst->VRegC_3rc();
      if (string_init) {
        // Skip the referrer for the new static StringFactory call.
        ++first_src_reg;
        ++first_dest_reg;
      }
      for (size_t src_reg = first_src_reg, dest_reg = first_dest_reg; dest_reg < num_regs;
          ++dest_reg, ++src_reg) {
        AssignRegister(new_shadow_frame, shadow_frame, dest_reg, src_reg);
      }
    } else {
      DCHECK_LE(num_ins, 5U);
      uint16_t regList = inst->Fetch16(2);
      uint16_t count = num_ins;
      size_t arg_index = 0;
      if (count == 5) {
        AssignRegister(new_shadow_frame, shadow_frame, first_dest_reg + 4U,
                       (inst_data >> 8) & 0x0f);
        --count;
      }
      if (string_init) {
        // Skip the referrer for the new static StringFactory call.
        regList >>= 4;
        ++first_dest_reg;
        --count;
      }
      for (; arg_index < count; ++arg_index, regList >>= 4) {
        AssignRegister(new_shadow_frame, shadow_frame, first_dest_reg + arg_index, regList & 0x0f);
      }
    }
    self->EndAssertNoThreadSuspension(old_cause);
  }

  // Do the call now.
  if (LIKELY(Runtime::Current()->IsStarted())) {
    if (kIsDebugBuild && new_shadow_frame->GetMethod()->GetEntryPointFromInterpreter() == nullptr) {
      LOG(FATAL) << "Attempt to invoke non-executable method: "
          << PrettyMethod(new_shadow_frame->GetMethod());
      UNREACHABLE();
    }
    if (kIsDebugBuild && Runtime::Current()->GetInstrumentation()->IsForcedInterpretOnly() &&
        !new_shadow_frame->GetMethod()->IsNative() &&
        !new_shadow_frame->GetMethod()->IsProxyMethod() &&
        new_shadow_frame->GetMethod()->GetEntryPointFromInterpreter()
            == artInterpreterToCompiledCodeBridge) {
      LOG(FATAL) << "Attempt to call compiled code when -Xint: "
          << PrettyMethod(new_shadow_frame->GetMethod());
      UNREACHABLE();
    }
    // Force the use of interpreter when it is required by the debugger.
    EntryPointFromInterpreter* entry;
    if (UNLIKELY(Dbg::IsForcedInterpreterNeededForCalling(self, new_shadow_frame->GetMethod()))) {
      entry = &art::artInterpreterToInterpreterBridge;
    } else {
      entry = new_shadow_frame->GetMethod()->GetEntryPointFromInterpreter();
    }
    entry(self, code_item, new_shadow_frame, result);
  } else {
    UnstartedRuntime::Invoke(self, code_item, new_shadow_frame, result, first_dest_reg);
  }

  if (string_init && !self->IsExceptionPending()) {
    // Set the new string result of the StringFactory.
    uint32_t vregC = (is_range) ? inst->VRegC_3rc() : inst->VRegC_35c();
    shadow_frame.SetVRegReference(vregC, result->GetL());
    // Overwrite all potential copies of the original result of the new-instance of string with the
    // new result of the StringFactory. Use the verifier to find this set of registers.
    ArtMethod* method = shadow_frame.GetMethod();
    MethodReference method_ref = method->ToMethodReference();
    SafeMap<uint32_t, std::set<uint32_t>> string_init_map;
    SafeMap<uint32_t, std::set<uint32_t>>* string_init_map_ptr;
    MethodRefToStringInitRegMap& method_to_string_init_map = Runtime::Current()->GetStringInitMap();
    auto it = method_to_string_init_map.find(method_ref);
    if (it == method_to_string_init_map.end()) {
      string_init_map = std::move(verifier::MethodVerifier::FindStringInitMap(method));
      method_to_string_init_map.Overwrite(method_ref, string_init_map);
      string_init_map_ptr = &string_init_map;
    } else {
      string_init_map_ptr = &it->second;
    }
    if (string_init_map_ptr->size() != 0) {
      uint32_t dex_pc = shadow_frame.GetDexPC();
      auto map_it = string_init_map_ptr->find(dex_pc);
      if (map_it != string_init_map_ptr->end()) {
        const std::set<uint32_t>& reg_set = map_it->second;
        for (auto set_it = reg_set.begin(); set_it != reg_set.end(); ++set_it) {
          shadow_frame.SetVRegReference(*set_it, result->GetL());
        }
      }
    }
  }

  return !self->IsExceptionPending();
}

template <bool is_range, bool do_access_check, bool transaction_active>
bool DoFilledNewArray(const Instruction* inst, const ShadowFrame& shadow_frame,
                      Thread* self, JValue* result) {
  DCHECK(inst->Opcode() == Instruction::FILLED_NEW_ARRAY ||
         inst->Opcode() == Instruction::FILLED_NEW_ARRAY_RANGE);
  const int32_t length = is_range ? inst->VRegA_3rc() : inst->VRegA_35c();
  if (!is_range) {
    // Checks FILLED_NEW_ARRAY's length does not exceed 5 arguments.
    CHECK_LE(length, 5);
  }
  if (UNLIKELY(length < 0)) {
    ThrowNegativeArraySizeException(length);
    return false;
  }
  uint16_t type_idx = is_range ? inst->VRegB_3rc() : inst->VRegB_35c();
  Class* array_class = ResolveVerifyAndClinit(type_idx, shadow_frame.GetMethod(),
                                              self, false, do_access_check);
  if (UNLIKELY(array_class == nullptr)) {
    DCHECK(self->IsExceptionPending());
    return false;
  }
  CHECK(array_class->IsArrayClass());
  Class* component_class = array_class->GetComponentType();
  const bool is_primitive_int_component = component_class->IsPrimitiveInt();
  if (UNLIKELY(component_class->IsPrimitive() && !is_primitive_int_component)) {
    if (component_class->IsPrimitiveLong() || component_class->IsPrimitiveDouble()) {
      ThrowRuntimeException("Bad filled array request for type %s",
                            PrettyDescriptor(component_class).c_str());
    } else {
      self->ThrowNewExceptionF("Ljava/lang/InternalError;",
                               "Found type %s; filled-new-array not implemented for anything but 'int'",
                               PrettyDescriptor(component_class).c_str());
    }
    return false;
  }
  Object* new_array = Array::Alloc<true>(self, array_class, length,
                                         array_class->GetComponentSizeShift(),
                                         Runtime::Current()->GetHeap()->GetCurrentAllocator());
  if (UNLIKELY(new_array == nullptr)) {
    self->AssertPendingOOMException();
    return false;
  }
  uint32_t arg[5];  // only used in filled-new-array.
  uint32_t vregC;   // only used in filled-new-array-range.
  if (is_range) {
    vregC = inst->VRegC_3rc();
  } else {
    inst->GetVarArgs(arg);
  }
  for (int32_t i = 0; i < length; ++i) {
    size_t src_reg = is_range ? vregC + i : arg[i];
    if (is_primitive_int_component) {
      new_array->AsIntArray()->SetWithoutChecks<transaction_active>(
          i, shadow_frame.GetVReg(src_reg));
    } else {
      new_array->AsObjectArray<Object>()->SetWithoutChecks<transaction_active>(
          i, shadow_frame.GetVRegReference(src_reg));
    }
  }

  result->SetL(new_array);
  return true;
}

// TODO fix thread analysis: should be SHARED_LOCKS_REQUIRED(Locks::mutator_lock_).
template<typename T>
static void RecordArrayElementsInTransactionImpl(mirror::PrimitiveArray<T>* array, int32_t count)
    NO_THREAD_SAFETY_ANALYSIS {
  Runtime* runtime = Runtime::Current();
  for (int32_t i = 0; i < count; ++i) {
    runtime->RecordWriteArray(array, i, array->GetWithoutChecks(i));
  }
}

void RecordArrayElementsInTransaction(mirror::Array* array, int32_t count)
    SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
  DCHECK(Runtime::Current()->IsActiveTransaction());
  DCHECK(array != nullptr);
  DCHECK_LE(count, array->GetLength());
  Primitive::Type primitive_component_type = array->GetClass()->GetComponentType()->GetPrimitiveType();
  switch (primitive_component_type) {
    case Primitive::kPrimBoolean:
      RecordArrayElementsInTransactionImpl(array->AsBooleanArray(), count);
      break;
    case Primitive::kPrimByte:
      RecordArrayElementsInTransactionImpl(array->AsByteArray(), count);
      break;
    case Primitive::kPrimChar:
      RecordArrayElementsInTransactionImpl(array->AsCharArray(), count);
      break;
    case Primitive::kPrimShort:
      RecordArrayElementsInTransactionImpl(array->AsShortArray(), count);
      break;
    case Primitive::kPrimInt:
      RecordArrayElementsInTransactionImpl(array->AsIntArray(), count);
      break;
    case Primitive::kPrimFloat:
      RecordArrayElementsInTransactionImpl(array->AsFloatArray(), count);
      break;
    case Primitive::kPrimLong:
      RecordArrayElementsInTransactionImpl(array->AsLongArray(), count);
      break;
    case Primitive::kPrimDouble:
      RecordArrayElementsInTransactionImpl(array->AsDoubleArray(), count);
      break;
    default:
      LOG(FATAL) << "Unsupported primitive type " << primitive_component_type
                 << " in fill-array-data";
      break;
  }
}

// Explicit DoCall template function declarations.
#define EXPLICIT_DO_CALL_TEMPLATE_DECL(_is_range, _do_assignability_check)                      \
  template SHARED_LOCKS_REQUIRED(Locks::mutator_lock_)                                          \
  bool DoCall<_is_range, _do_assignability_check>(ArtMethod* method, Thread* self,              \
                                                  ShadowFrame& shadow_frame,                    \
                                                  const Instruction* inst, uint16_t inst_data,  \
                                                  JValue* result)
EXPLICIT_DO_CALL_TEMPLATE_DECL(false, false);
EXPLICIT_DO_CALL_TEMPLATE_DECL(false, true);
EXPLICIT_DO_CALL_TEMPLATE_DECL(true, false);
EXPLICIT_DO_CALL_TEMPLATE_DECL(true, true);
#undef EXPLICIT_DO_CALL_TEMPLATE_DECL

// Explicit DoFilledNewArray template function declarations.
#define EXPLICIT_DO_FILLED_NEW_ARRAY_TEMPLATE_DECL(_is_range_, _check, _transaction_active)       \
  template SHARED_LOCKS_REQUIRED(Locks::mutator_lock_)                                            \
  bool DoFilledNewArray<_is_range_, _check, _transaction_active>(const Instruction* inst,         \
                                                                 const ShadowFrame& shadow_frame, \
                                                                 Thread* self, JValue* result)
#define EXPLICIT_DO_FILLED_NEW_ARRAY_ALL_TEMPLATE_DECL(_transaction_active)       \
  EXPLICIT_DO_FILLED_NEW_ARRAY_TEMPLATE_DECL(false, false, _transaction_active);  \
  EXPLICIT_DO_FILLED_NEW_ARRAY_TEMPLATE_DECL(false, true, _transaction_active);   \
  EXPLICIT_DO_FILLED_NEW_ARRAY_TEMPLATE_DECL(true, false, _transaction_active);   \
  EXPLICIT_DO_FILLED_NEW_ARRAY_TEMPLATE_DECL(true, true, _transaction_active)
EXPLICIT_DO_FILLED_NEW_ARRAY_ALL_TEMPLATE_DECL(false);
EXPLICIT_DO_FILLED_NEW_ARRAY_ALL_TEMPLATE_DECL(true);
#undef EXPLICIT_DO_FILLED_NEW_ARRAY_ALL_TEMPLATE_DECL
#undef EXPLICIT_DO_FILLED_NEW_ARRAY_TEMPLATE_DECL

}  // namespace interpreter
}  // namespace art
