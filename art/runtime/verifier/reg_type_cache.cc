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

#include "reg_type_cache-inl.h"

#include "base/casts.h"
#include "class_linker-inl.h"
#include "dex_file-inl.h"
#include "mirror/class-inl.h"
#include "mirror/object-inl.h"
#include "reg_type-inl.h"

namespace art {
namespace verifier {

bool RegTypeCache::primitive_initialized_ = false;
uint16_t RegTypeCache::primitive_count_ = 0;
const PreciseConstType* RegTypeCache::small_precise_constants_[kMaxSmallConstant - kMinSmallConstant + 1];

static bool MatchingPrecisionForClass(const RegType* entry, bool precise)
    SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
  if (entry->IsPreciseReference() == precise) {
    // We were or weren't looking for a precise reference and we found what we need.
    return true;
  } else {
    if (!precise && entry->GetClass()->CannotBeAssignedFromOtherTypes()) {
      // We weren't looking for a precise reference, as we're looking up based on a descriptor, but
      // we found a matching entry based on the descriptor. Return the precise entry in that case.
      return true;
    }
    return false;
  }
}

void RegTypeCache::FillPrimitiveAndSmallConstantTypes() {
  entries_.push_back(UndefinedType::GetInstance());
  entries_.push_back(ConflictType::GetInstance());
  entries_.push_back(BooleanType::GetInstance());
  entries_.push_back(ByteType::GetInstance());
  entries_.push_back(ShortType::GetInstance());
  entries_.push_back(CharType::GetInstance());
  entries_.push_back(IntegerType::GetInstance());
  entries_.push_back(LongLoType::GetInstance());
  entries_.push_back(LongHiType::GetInstance());
  entries_.push_back(FloatType::GetInstance());
  entries_.push_back(DoubleLoType::GetInstance());
  entries_.push_back(DoubleHiType::GetInstance());
  for (int32_t value = kMinSmallConstant; value <= kMaxSmallConstant; ++value) {
    int32_t i = value - kMinSmallConstant;
    DCHECK_EQ(entries_.size(), small_precise_constants_[i]->GetId());
    entries_.push_back(small_precise_constants_[i]);
  }
  DCHECK_EQ(entries_.size(), primitive_count_);
}

const RegType& RegTypeCache::FromDescriptor(mirror::ClassLoader* loader, const char* descriptor,
                                            bool precise) {
  DCHECK(RegTypeCache::primitive_initialized_);
  if (descriptor[1] == '\0') {
    switch (descriptor[0]) {
      case 'Z':
        return Boolean();
      case 'B':
        return Byte();
      case 'S':
        return Short();
      case 'C':
        return Char();
      case 'I':
        return Integer();
      case 'J':
        return LongLo();
      case 'F':
        return Float();
      case 'D':
        return DoubleLo();
      case 'V':  // For void types, conflict types.
      default:
        return Conflict();
    }
  } else if (descriptor[0] == 'L' || descriptor[0] == '[') {
    return From(loader, descriptor, precise);
  } else {
    return Conflict();
  }
}

const RegType& RegTypeCache::RegTypeFromPrimitiveType(Primitive::Type prim_type) const {
  DCHECK(RegTypeCache::primitive_initialized_);
  switch (prim_type) {
    case Primitive::kPrimBoolean:
      return *BooleanType::GetInstance();
    case Primitive::kPrimByte:
      return *ByteType::GetInstance();
    case Primitive::kPrimShort:
      return *ShortType::GetInstance();
    case Primitive::kPrimChar:
      return *CharType::GetInstance();
    case Primitive::kPrimInt:
      return *IntegerType::GetInstance();
    case Primitive::kPrimLong:
      return *LongLoType::GetInstance();
    case Primitive::kPrimFloat:
      return *FloatType::GetInstance();
    case Primitive::kPrimDouble:
      return *DoubleLoType::GetInstance();
    case Primitive::kPrimVoid:
    default:
      return *ConflictType::GetInstance();
  }
}

bool RegTypeCache::MatchDescriptor(size_t idx, const StringPiece& descriptor, bool precise) {
  const RegType* entry = entries_[idx];
  if (descriptor != entry->descriptor_) {
    return false;
  }
  if (entry->HasClass()) {
    return MatchingPrecisionForClass(entry, precise);
  }
  // There is no notion of precise unresolved references, the precise information is just dropped
  // on the floor.
  DCHECK(entry->IsUnresolvedReference());
  return true;
}

mirror::Class* RegTypeCache::ResolveClass(const char* descriptor, mirror::ClassLoader* loader) {
  // Class was not found, must create new type.
  // Try resolving class
  ClassLinker* class_linker = Runtime::Current()->GetClassLinker();
  Thread* self = Thread::Current();
  StackHandleScope<1> hs(self);
  Handle<mirror::ClassLoader> class_loader(hs.NewHandle(loader));
  mirror::Class* klass = nullptr;
  if (can_load_classes_) {
    klass = class_linker->FindClass(self, descriptor, class_loader);
  } else {
    klass = class_linker->LookupClass(self, descriptor, ComputeModifiedUtf8Hash(descriptor),
                                      loader);
    if (klass != nullptr && !klass->IsLoaded()) {
      // We found the class but without it being loaded its not safe for use.
      klass = nullptr;
    }
  }
  return klass;
}

const RegType& RegTypeCache::From(mirror::ClassLoader* loader, const char* descriptor,
                                  bool precise) {
  // Try looking up the class in the cache first. We use a StringPiece to avoid continual strlen
  // operations on the descriptor.
  StringPiece descriptor_sp(descriptor);
  for (size_t i = primitive_count_; i < entries_.size(); i++) {
    if (MatchDescriptor(i, descriptor_sp, precise)) {
      return *(entries_[i]);
    }
  }
  // Class not found in the cache, will create a new type for that.
  // Try resolving class.
  mirror::Class* klass = ResolveClass(descriptor, loader);
  if (klass != nullptr) {
    // Class resolved, first look for the class in the list of entries
    // Class was not found, must create new type.
    // To pass the verification, the type should be imprecise,
    // instantiable or an interface with the precise type set to false.
    DCHECK(!precise || klass->IsInstantiable());
    // Create a precise type if:
    // 1- Class is final and NOT an interface. a precise interface is meaningless !!
    // 2- Precise Flag passed as true.
    RegType* entry;
    // Create an imprecise type if we can't tell for a fact that it is precise.
    if (klass->CannotBeAssignedFromOtherTypes() || precise) {
      DCHECK(!(klass->IsAbstract()) || klass->IsArrayClass());
      DCHECK(!klass->IsInterface());
      entry = new PreciseReferenceType(klass, descriptor_sp.as_string(), entries_.size());
    } else {
      entry = new ReferenceType(klass, descriptor_sp.as_string(), entries_.size());
    }
    AddEntry(entry);
    return *entry;
  } else {  // Class not resolved.
    // We tried loading the class and failed, this might get an exception raised
    // so we want to clear it before we go on.
    if (can_load_classes_) {
      DCHECK(Thread::Current()->IsExceptionPending());
      Thread::Current()->ClearException();
    } else {
      DCHECK(!Thread::Current()->IsExceptionPending());
    }
    if (IsValidDescriptor(descriptor)) {
      RegType* entry = new UnresolvedReferenceType(descriptor_sp.as_string(), entries_.size());
      AddEntry(entry);
      return *entry;
    } else {
      // The descriptor is broken return the unknown type as there's nothing sensible that
      // could be done at runtime
      return Conflict();
    }
  }
}

const RegType& RegTypeCache::FromClass(const char* descriptor, mirror::Class* klass, bool precise) {
  DCHECK(klass != nullptr);
  if (klass->IsPrimitive()) {
    // Note: precise isn't used for primitive classes. A char is assignable to an int. All
    // primitive classes are final.
    return RegTypeFromPrimitiveType(klass->GetPrimitiveType());
  } else {
    // Look for the reference in the list of entries to have.
    for (size_t i = primitive_count_; i < entries_.size(); i++) {
      const RegType* cur_entry = entries_[i];
      if (cur_entry->klass_.Read() == klass && MatchingPrecisionForClass(cur_entry, precise)) {
        return *cur_entry;
      }
    }
    // No reference to the class was found, create new reference.
    RegType* entry;
    if (precise) {
      entry = new PreciseReferenceType(klass, descriptor, entries_.size());
    } else {
      entry = new ReferenceType(klass, descriptor, entries_.size());
    }
    AddEntry(entry);
    return *entry;
  }
}

RegTypeCache::RegTypeCache(bool can_load_classes) : can_load_classes_(can_load_classes) {
  if (kIsDebugBuild) {
    Thread::Current()->AssertThreadSuspensionIsAllowable(gAborting == 0);
  }
  entries_.reserve(64);
  FillPrimitiveAndSmallConstantTypes();
}

RegTypeCache::~RegTypeCache() {
  CHECK_LE(primitive_count_, entries_.size());
  // Delete only the non primitive types.
  if (entries_.size() == kNumPrimitivesAndSmallConstants) {
    // All entries are from the global pool, nothing to delete.
    return;
  }
  std::vector<const RegType*>::iterator non_primitive_begin = entries_.begin();
  std::advance(non_primitive_begin, kNumPrimitivesAndSmallConstants);
  STLDeleteContainerPointers(non_primitive_begin, entries_.end());
}

void RegTypeCache::ShutDown() {
  if (RegTypeCache::primitive_initialized_) {
    UndefinedType::Destroy();
    ConflictType::Destroy();
    BooleanType::Destroy();
    ByteType::Destroy();
    ShortType::Destroy();
    CharType::Destroy();
    IntegerType::Destroy();
    LongLoType::Destroy();
    LongHiType::Destroy();
    FloatType::Destroy();
    DoubleLoType::Destroy();
    DoubleHiType::Destroy();
    for (int32_t value = kMinSmallConstant; value <= kMaxSmallConstant; ++value) {
      const PreciseConstType* type = small_precise_constants_[value - kMinSmallConstant];
      delete type;
      small_precise_constants_[value - kMinSmallConstant] = nullptr;
    }
    RegTypeCache::primitive_initialized_ = false;
    RegTypeCache::primitive_count_ = 0;
  }
}

template <class Type>
const Type* RegTypeCache::CreatePrimitiveTypeInstance(const std::string& descriptor) {
  mirror::Class* klass = nullptr;
  // Try loading the class from linker.
  if (!descriptor.empty()) {
    klass = art::Runtime::Current()->GetClassLinker()->FindSystemClass(Thread::Current(),
                                                                       descriptor.c_str());
    DCHECK(klass != nullptr);
  }
  const Type* entry = Type::CreateInstance(klass, descriptor, RegTypeCache::primitive_count_);
  RegTypeCache::primitive_count_++;
  return entry;
}

void RegTypeCache::CreatePrimitiveAndSmallConstantTypes() {
  CreatePrimitiveTypeInstance<UndefinedType>("");
  CreatePrimitiveTypeInstance<ConflictType>("");
  CreatePrimitiveTypeInstance<BooleanType>("Z");
  CreatePrimitiveTypeInstance<ByteType>("B");
  CreatePrimitiveTypeInstance<ShortType>("S");
  CreatePrimitiveTypeInstance<CharType>("C");
  CreatePrimitiveTypeInstance<IntegerType>("I");
  CreatePrimitiveTypeInstance<LongLoType>("J");
  CreatePrimitiveTypeInstance<LongHiType>("J");
  CreatePrimitiveTypeInstance<FloatType>("F");
  CreatePrimitiveTypeInstance<DoubleLoType>("D");
  CreatePrimitiveTypeInstance<DoubleHiType>("D");
  for (int32_t value = kMinSmallConstant; value <= kMaxSmallConstant; ++value) {
    PreciseConstType* type = new PreciseConstType(value, primitive_count_);
    small_precise_constants_[value - kMinSmallConstant] = type;
    primitive_count_++;
  }
}

const RegType& RegTypeCache::FromUnresolvedMerge(const RegType& left, const RegType& right) {
  BitVector types(1,                                    // Allocate at least a word.
                  true,                                 // Is expandable.
                  Allocator::GetMallocAllocator());     // TODO: Arenas in the verifier.
  const RegType* left_resolved;
  if (left.IsUnresolvedMergedReference()) {
    const UnresolvedMergedType* left_merge = down_cast<const UnresolvedMergedType*>(&left);
    types.Copy(&left_merge->GetUnresolvedTypes());
    left_resolved = &left_merge->GetResolvedPart();
  } else if (left.IsUnresolvedTypes()) {
    types.SetBit(left.GetId());
    left_resolved = &Zero();
  } else {
    left_resolved = &left;
  }

  const RegType* right_resolved;
  if (right.IsUnresolvedMergedReference()) {
    const UnresolvedMergedType* right_merge = down_cast<const UnresolvedMergedType*>(&right);
    types.Union(&right_merge->GetUnresolvedTypes());
    right_resolved = &right_merge->GetResolvedPart();
  } else if (right.IsUnresolvedTypes()) {
    types.SetBit(right.GetId());
    right_resolved = &Zero();
  } else {
    right_resolved = &right;
  }

  // Merge the resolved parts. Left and right might be equal, so use SafeMerge.
  const RegType& resolved_parts_merged = left_resolved->SafeMerge(*right_resolved, this);
  // If we get a conflict here, the merge result is a conflict, not an unresolved merge type.
  if (resolved_parts_merged.IsConflict()) {
    return Conflict();
  }

  // Check if entry already exists.
  for (size_t i = primitive_count_; i < entries_.size(); i++) {
    const RegType* cur_entry = entries_[i];
    if (cur_entry->IsUnresolvedMergedReference()) {
      const UnresolvedMergedType* cmp_type = down_cast<const UnresolvedMergedType*>(cur_entry);
      const RegType& resolved_part = cmp_type->GetResolvedPart();
      const BitVector& unresolved_part = cmp_type->GetUnresolvedTypes();
      // Use SameBitsSet. "types" is expandable to allow merging in the components, but the
      // BitVector in the final RegType will be made non-expandable.
      if (&resolved_part == &resolved_parts_merged &&
              types.SameBitsSet(&unresolved_part)) {
        return *cur_entry;
      }
    }
  }

  // Create entry.
  RegType* entry = new UnresolvedMergedType(resolved_parts_merged,
                                            types,
                                            this,
                                            entries_.size());
  AddEntry(entry);
  return *entry;
}

const RegType& RegTypeCache::FromUnresolvedSuperClass(const RegType& child) {
  // Check if entry already exists.
  for (size_t i = primitive_count_; i < entries_.size(); i++) {
    const RegType* cur_entry = entries_[i];
    if (cur_entry->IsUnresolvedSuperClass()) {
      const UnresolvedSuperClass* tmp_entry =
          down_cast<const UnresolvedSuperClass*>(cur_entry);
      uint16_t unresolved_super_child_id =
          tmp_entry->GetUnresolvedSuperClassChildId();
      if (unresolved_super_child_id == child.GetId()) {
        return *cur_entry;
      }
    }
  }
  RegType* entry = new UnresolvedSuperClass(child.GetId(), this, entries_.size());
  AddEntry(entry);
  return *entry;
}

const UninitializedType& RegTypeCache::Uninitialized(const RegType& type, uint32_t allocation_pc) {
  UninitializedType* entry = nullptr;
  const std::string& descriptor(type.GetDescriptor());
  if (type.IsUnresolvedTypes()) {
    for (size_t i = primitive_count_; i < entries_.size(); i++) {
      const RegType* cur_entry = entries_[i];
      if (cur_entry->IsUnresolvedAndUninitializedReference() &&
          down_cast<const UnresolvedUninitializedRefType*>(cur_entry)->GetAllocationPc()
              == allocation_pc &&
          (cur_entry->GetDescriptor() == descriptor)) {
        return *down_cast<const UnresolvedUninitializedRefType*>(cur_entry);
      }
    }
    entry = new UnresolvedUninitializedRefType(descriptor, allocation_pc, entries_.size());
  } else {
    mirror::Class* klass = type.GetClass();
    for (size_t i = primitive_count_; i < entries_.size(); i++) {
      const RegType* cur_entry = entries_[i];
      if (cur_entry->IsUninitializedReference() &&
          down_cast<const UninitializedReferenceType*>(cur_entry)
              ->GetAllocationPc() == allocation_pc &&
          cur_entry->GetClass() == klass) {
        return *down_cast<const UninitializedReferenceType*>(cur_entry);
      }
    }
    entry = new UninitializedReferenceType(klass, descriptor, allocation_pc, entries_.size());
  }
  AddEntry(entry);
  return *entry;
}

const RegType& RegTypeCache::FromUninitialized(const RegType& uninit_type) {
  RegType* entry;

  if (uninit_type.IsUnresolvedTypes()) {
    const std::string& descriptor(uninit_type.GetDescriptor());
    for (size_t i = primitive_count_; i < entries_.size(); i++) {
      const RegType* cur_entry = entries_[i];
      if (cur_entry->IsUnresolvedReference() &&
          cur_entry->GetDescriptor() == descriptor) {
        return *cur_entry;
      }
    }
    entry = new UnresolvedReferenceType(descriptor, entries_.size());
  } else {
    mirror::Class* klass = uninit_type.GetClass();
    if (uninit_type.IsUninitializedThisReference() && !klass->IsFinal()) {
      // For uninitialized "this reference" look for reference types that are not precise.
      for (size_t i = primitive_count_; i < entries_.size(); i++) {
        const RegType* cur_entry = entries_[i];
        if (cur_entry->IsReference() && cur_entry->GetClass() == klass) {
          return *cur_entry;
        }
      }
      entry = new ReferenceType(klass, "", entries_.size());
    } else if (klass->IsInstantiable()) {
      // We're uninitialized because of allocation, look or create a precise type as allocations
      // may only create objects of that type.
      for (size_t i = primitive_count_; i < entries_.size(); i++) {
        const RegType* cur_entry = entries_[i];
        if (cur_entry->IsPreciseReference() && cur_entry->GetClass() == klass) {
          return *cur_entry;
        }
      }
      entry = new PreciseReferenceType(klass, uninit_type.GetDescriptor(), entries_.size());
    } else {
      return Conflict();
    }
  }
  AddEntry(entry);
  return *entry;
}

const UninitializedType& RegTypeCache::UninitializedThisArgument(const RegType& type) {
  UninitializedType* entry;
  const std::string& descriptor(type.GetDescriptor());
  if (type.IsUnresolvedTypes()) {
    for (size_t i = primitive_count_; i < entries_.size(); i++) {
      const RegType* cur_entry = entries_[i];
      if (cur_entry->IsUnresolvedAndUninitializedThisReference() &&
          cur_entry->GetDescriptor() == descriptor) {
        return *down_cast<const UninitializedType*>(cur_entry);
      }
    }
    entry = new UnresolvedUninitializedThisRefType(descriptor, entries_.size());
  } else {
    mirror::Class* klass = type.GetClass();
    for (size_t i = primitive_count_; i < entries_.size(); i++) {
      const RegType* cur_entry = entries_[i];
      if (cur_entry->IsUninitializedThisReference() && cur_entry->GetClass() == klass) {
        return *down_cast<const UninitializedType*>(cur_entry);
      }
    }
    entry = new UninitializedThisReferenceType(klass, descriptor, entries_.size());
  }
  AddEntry(entry);
  return *entry;
}

const ConstantType& RegTypeCache::FromCat1NonSmallConstant(int32_t value, bool precise) {
  for (size_t i = primitive_count_; i < entries_.size(); i++) {
    const RegType* cur_entry = entries_[i];
    if (cur_entry->klass_.IsNull() && cur_entry->IsConstant() &&
        cur_entry->IsPreciseConstant() == precise &&
        (down_cast<const ConstantType*>(cur_entry))->ConstantValue() == value) {
      return *down_cast<const ConstantType*>(cur_entry);
    }
  }
  ConstantType* entry;
  if (precise) {
    entry = new PreciseConstType(value, entries_.size());
  } else {
    entry = new ImpreciseConstType(value, entries_.size());
  }
  AddEntry(entry);
  return *entry;
}

const ConstantType& RegTypeCache::FromCat2ConstLo(int32_t value, bool precise) {
  for (size_t i = primitive_count_; i < entries_.size(); i++) {
    const RegType* cur_entry = entries_[i];
    if (cur_entry->IsConstantLo() && (cur_entry->IsPrecise() == precise) &&
        (down_cast<const ConstantType*>(cur_entry))->ConstantValueLo() == value) {
      return *down_cast<const ConstantType*>(cur_entry);
    }
  }
  ConstantType* entry;
  if (precise) {
    entry = new PreciseConstLoType(value, entries_.size());
  } else {
    entry = new ImpreciseConstLoType(value, entries_.size());
  }
  AddEntry(entry);
  return *entry;
}

const ConstantType& RegTypeCache::FromCat2ConstHi(int32_t value, bool precise) {
  for (size_t i = primitive_count_; i < entries_.size(); i++) {
    const RegType* cur_entry = entries_[i];
    if (cur_entry->IsConstantHi() && (cur_entry->IsPrecise() == precise) &&
        (down_cast<const ConstantType*>(cur_entry))->ConstantValueHi() == value) {
      return *down_cast<const ConstantType*>(cur_entry);
    }
  }
  ConstantType* entry;
  if (precise) {
    entry = new PreciseConstHiType(value, entries_.size());
  } else {
    entry = new ImpreciseConstHiType(value, entries_.size());
  }
  AddEntry(entry);
  return *entry;
}

const RegType& RegTypeCache::GetComponentType(const RegType& array, mirror::ClassLoader* loader) {
  if (!array.IsArrayTypes()) {
    return Conflict();
  } else if (array.IsUnresolvedTypes()) {
    const std::string& descriptor(array.GetDescriptor());
    const std::string component(descriptor.substr(1, descriptor.size() - 1));
    return FromDescriptor(loader, component.c_str(), false);
  } else {
    mirror::Class* klass = array.GetClass()->GetComponentType();
    std::string temp;
    if (klass->IsErroneous()) {
      // Arrays may have erroneous component types, use unresolved in that case.
      // We assume that the primitive classes are not erroneous, so we know it is a
      // reference type.
      return FromDescriptor(loader, klass->GetDescriptor(&temp), false);
    } else {
      return FromClass(klass->GetDescriptor(&temp), klass,
                       klass->CannotBeAssignedFromOtherTypes());
    }
  }
}

void RegTypeCache::Dump(std::ostream& os) {
  for (size_t i = 0; i < entries_.size(); i++) {
    const RegType* cur_entry = entries_[i];
    if (cur_entry != nullptr) {
      os << i << ": " << cur_entry->Dump() << "\n";
    }
  }
}

void RegTypeCache::VisitStaticRoots(RootVisitor* visitor) {
  // Visit the primitive types, this is required since if there are no active verifiers they wont
  // be in the entries array, and therefore not visited as roots.
  if (primitive_initialized_) {
    RootInfo ri(kRootUnknown);
    UndefinedType::GetInstance()->VisitRoots(visitor, ri);
    ConflictType::GetInstance()->VisitRoots(visitor, ri);
    BooleanType::GetInstance()->VisitRoots(visitor, ri);
    ByteType::GetInstance()->VisitRoots(visitor, ri);
    ShortType::GetInstance()->VisitRoots(visitor, ri);
    CharType::GetInstance()->VisitRoots(visitor, ri);
    IntegerType::GetInstance()->VisitRoots(visitor, ri);
    LongLoType::GetInstance()->VisitRoots(visitor, ri);
    LongHiType::GetInstance()->VisitRoots(visitor, ri);
    FloatType::GetInstance()->VisitRoots(visitor, ri);
    DoubleLoType::GetInstance()->VisitRoots(visitor, ri);
    DoubleHiType::GetInstance()->VisitRoots(visitor, ri);
    for (int32_t value = kMinSmallConstant; value <= kMaxSmallConstant; ++value) {
      small_precise_constants_[value - kMinSmallConstant]->VisitRoots(visitor, ri);
    }
  }
}

void RegTypeCache::VisitRoots(RootVisitor* visitor, const RootInfo& root_info) {
  // Exclude the static roots that are visited by VisitStaticRoots().
  for (size_t i = primitive_count_; i < entries_.size(); ++i) {
    entries_[i]->VisitRoots(visitor, root_info);
  }
}

void RegTypeCache::AddEntry(RegType* new_entry) {
  entries_.push_back(new_entry);
}

}  // namespace verifier
}  // namespace art
