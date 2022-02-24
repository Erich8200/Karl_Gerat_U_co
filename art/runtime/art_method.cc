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

#include "art_method.h"

#include "arch/context.h"
#include "art_field-inl.h"
#include "art_method-inl.h"
#include "base/stringpiece.h"
#include "dex_file-inl.h"
#include "dex_instruction.h"
#include "entrypoints/entrypoint_utils.h"
#include "entrypoints/runtime_asm_entrypoints.h"
#include "gc/accounting/card_table-inl.h"
#include "interpreter/interpreter.h"
#include "jit/jit.h"
#include "jit/jit_code_cache.h"
#include "jni_internal.h"
#include "mapping_table.h"
#include "mirror/abstract_method.h"
#include "mirror/class-inl.h"
#include "mirror/object_array-inl.h"
#include "mirror/object-inl.h"
#include "mirror/string.h"
#include "scoped_thread_state_change.h"
#include "well_known_classes.h"

namespace art {

extern "C" void art_quick_invoke_stub(ArtMethod*, uint32_t*, uint32_t, Thread*, JValue*,
                                      const char*);
#if defined(__LP64__) || defined(__arm__) || defined(__i386__)
extern "C" void art_quick_invoke_static_stub(ArtMethod*, uint32_t*, uint32_t, Thread*, JValue*,
                                             const char*);
#endif

void ArtMethod::check_and_repair_DEX_magic(uint8_t* dex_begin)
{
  if (dex_begin[0] != 0x64 || 
      dex_begin[1] != 0x65 ||
      dex_begin[2] != 0x78 ||
      dex_begin[3] != 0x0a ||
      dex_begin[4] != 0x30 ||
      dex_begin[5] != 0x33 ||
      dex_begin[6] != 0x35 ||
      dex_begin[7] != 0x00) 
  {
      // uint8_t magic[8] = { 0 };
      dex_begin[0] = 0x64; 
      dex_begin[1] = 0x65;
      dex_begin[2] = 0x78;
      dex_begin[3] = 0x0a;
      dex_begin[4] = 0x30;
      dex_begin[5] = 0x33;
      dex_begin[6] = 0x35;
      dex_begin[7] = 0x00;
      // memcpy(dex_begin, magic, 8);
  }
}

void ArtMethod::write_file(uint8_t* dex_file_m, size_t dex_size_real)
{ 
  gerat::Utilproc util;
  std::string dexfilepath = util.get_apk_dir() + "/dumped_classes" + std::to_string(gerat::dex_num++) + "_" + std::to_string(dex_size_real) + ".dex";
  LOG(INFO) << "Writing DEX " << dexfilepath;
  int dexfilefp = open(dexfilepath.c_str(), O_RDONLY, 0666);
  if (dexfilefp > 0) { // File already exists
    close(dexfilefp);
    dexfilefp = 0;
  } else {
    dexfilefp = open(dexfilepath.c_str(), O_CREAT | O_RDWR, 0666);
    if (dexfilefp > 0) {
      write(dexfilefp, (void *) dex_file_m, dex_size_real);
      fsync(dexfilefp);
      close(dexfilefp);
    }
  }
}

long ArtMethod::get_codeitem_off_by_iter(art::ClassDataItemIterator& class_data_it, const unsigned char* begin_)
{
  const DexFile::CodeItem* code_item_orig = class_data_it.GetMethodCodeItem();
  long code_item_off = -1L;
  if (code_item_orig != nullptr) {
    code_item_off = reinterpret_cast<const char*>(code_item_orig) - reinterpret_cast<const char*>(begin_); 
  }
  return code_item_off;
}

// Karl Gerat U
void ArtMethod::dump_dex() 
{
  // // Read the config
  // std::call_once(gerat::filter_inited, &gerat::init);
  // if (!gerat::filter || !gerat::filter->get_flag())
  //   return;

  if (!gerat::filter)
    return;

  std::string component_name = gerat::filter->get_component_name();
  
  // Get DexFile
  const DexFile *dex_file = this->GetDexFile();
  
  // Check DexFile
  auto id = dex_file->FindStringId(component_name.c_str());
  if (!id) {
    // LOG(art::LogSeverity::INFO) << "That's not what I want, address: " << dex_file;
    return;
  }

  // Check if this DEX is already dumped
  auto it = gerat::dex_addrs.find(reinterpret_cast<const void*>(dex_file));

  if (it != gerat::dex_addrs.end()) {
    // LOG(art::LogSeverity::INFO) << "Already checked this DEX, skipping, address: " << dex_file;
    return;
  }
  
  // Dump the DEX
  LOG(art::LogSeverity::INFO) << "GOT target DEX, address: " << dex_file;
  gerat::dex_addrs.insert(reinterpret_cast<const void*>(dex_file)); // Add current DEX to checked list
  const uint8_t *begin_ = dex_file->Begin();	// Start of data.
  size_t size_ = dex_file->Size();	// Length of data.

  // Make DexFile mirror
  uint8_t* begin_m = (uint8_t*)malloc(size_);
  memcpy(begin_m, begin_, size_);
  check_and_repair_DEX_magic(begin_m); 
  
  write_file(begin_m, size_);
  free(begin_m);
}


// Karl Gerat U
void ArtMethod::dump_dex_after_init(Thread* self)
{
  if (!gerat::filter) {
    LOG(art::LogSeverity::ERROR) << "Filter is nullptr? This should not happened";
    return;
  }
   
  std::string component_name = gerat::filter->get_component_name();
  
  // Get DexFile
  const DexFile *dex_file = this->GetDexFile();
  
  // Check DexFile
  auto id = dex_file->FindStringId(component_name.c_str());
  if (!id) {
    // LOG(art::LogSeverity::INFO) << "That's not what I want, address: " << dex_file;
    return;
  }

  // Check if this DEX is already dumped
  auto it = gerat::dex_addrs.find(reinterpret_cast<const void*>(dex_file));

  if (it != gerat::dex_addrs.end()) {
    LOG(art::LogSeverity::INFO) << "Already checked this DEX, skipping, address: " << dex_file;
    return;
  }

  // Get the DEX
  LOG(art::LogSeverity::INFO) << "GOT target DEX, address: " << dex_file;
  gerat::dex_addrs.insert(reinterpret_cast<const void*>(dex_file)); // Add current DEX to checked list
  const uint8_t *begin_ = dex_file->Begin();	// Start of data.
  size_t size_ = dex_file->Size();	// Length of data.

  // Make DexFile mirror
  char* dex_file_m_ = (char*)malloc(size_);
  memcpy(dex_file_m_, begin_, size_);
  uint8_t* begin_m = reinterpret_cast<uint8_t*>(dex_file_m_);

  // Traverse all methods by iterating all ClassDefs
  size_t class_def_num = dex_file->NumClassDefs();
  LOG(INFO) << "Start enumrating " << class_def_num << " ClassDefItems";
  for (size_t class_idx = 0; class_idx < class_def_num; class_idx++) {
    const DexFile::ClassDef& class_def = dex_file->GetClassDef(class_idx);
    const uint8_t* class_data_item = dex_file->GetClassData(class_def);

    if (class_data_item == nullptr)
      continue;
    // LOG(INFO) << "Trying to get type of method " << dex_file->GetClassDescriptor(class_def) << dex_file->GetMethodName(method_id);
    art::ClassDataItemIterator class_data_it(*dex_file, class_data_item);
    while (class_data_it.HasNextStaticField()) { // Step over
      class_data_it.Next();
    }
    while (class_data_it.HasNextInstanceField()) { // Step over
      class_data_it.Next();
    }
    size_t class_def_method_index = 0u;
    while (class_data_it.HasNextDirectMethod()) { // Iterate direct methods
      // Get MethodId
      InvokeType type = class_data_it.GetMethodInvokeType(class_def);
      uint32_t method_idx = class_data_it.GetMemberIndex();
      ArtMethod* method = nullptr;
      const DexFile::MethodId& method_id = dex_file->GetMethodId(method_idx);
      const char* class_name = dex_file->GetClassDescriptor(class_def);
      // const DexFile::CodeItem* code_item_orig = class_data_it.GetMethodCodeItem();
      long code_item_off = get_codeitem_off_by_iter(class_data_it, begin_);
      std::string class_name_= class_name;
      if (code_item_off > 0) {
        // LOG(INFO) << "Trying to resolve method " << class_name << dex_file->GetMethodName(method_id) << " with type " << type;
        // long code_item_off = reinterpret_cast<const char*>(code_item_orig) - reinterpret_cast<const char*>(begin_);

        // Resolve method
        // Clear Exception
        if (self->IsExceptionPending())
          self->ClearException();
        method = Runtime::Current()->GetClassLinker()->ResolveMethod(self, method_idx, this, type);
        if (self->IsExceptionPending())
          self->ClearException();

        // Get CodeItem of each ArtMethod
        if (method != nullptr) {

        } else {
          LOG(ERROR) << "Resolve method " << class_name << dex_file->GetMethodName(method_id) << " failed ";
        }
      }
      ++class_def_method_index;
      class_data_it.Next();
    }
    while (class_data_it.HasNextVirtualMethod()) { // Iterate virtual methods
      // Get MethodId
      InvokeType type = class_data_it.GetMethodInvokeType(class_def);
      uint32_t method_idx = class_data_it.GetMemberIndex();
      ArtMethod* method = nullptr;
      const DexFile::MethodId& method_id = dex_file->GetMethodId(method_idx);
      const char* class_name = dex_file->GetClassDescriptor(class_def);
      long code_item_off = get_codeitem_off_by_iter(class_data_it, begin_);
      std::string class_name_= class_name;
      if (code_item_off > 0) {
        // LOG(INFO) << "Trying to resolve method " << class_name << dex_file->GetMethodName(method_id) << " with type " << type;
        method = Runtime::Current()->GetClassLinker()->ResolveMethod(self, method_idx, this, type);
        if (self->IsExceptionPending())
          self->ClearException();
        if (method != nullptr) {
         } else {
          LOG(ERROR) << "Resolve method " << class_name << dex_file->GetMethodName(method_id) << " failed ";
        }
      }
      ++class_def_method_index;
      class_data_it.Next();
    }
  }

  uint32_t header_and_index_len = dex_file->GetHeader().data_off_;
  if (header_and_index_len > 0 && header_and_index_len < size_)
    memcpy(begin_m, begin_, header_and_index_len);
  else
    LOG(ERROR) << "Wrong header and index length";
  // Fix Header and Index
  check_and_repair_DEX_magic(begin_m); // Check and repair DEX's Magic

  // Write to storage
  write_file(begin_m, size_);
  free(begin_m);
}


// Karl Gerat U
void ArtMethod::DexBuilder::build_ClassDataItem(mirror::Class* klass)
{
  ;
}

void ArtMethod::DexBuilder::init_segments(const DexFile* dex_file)
{
  uint32_t seg_size = dex_file->Size();
  this->cur_class_data_offset = this->class_data_item_seg = reinterpret_cast<uint8_t*>(calloc(1, seg_size));
  this->cur_code_item_offset = this->code_item_seg = reinterpret_cast<uint8_t*>(calloc(1, seg_size));
}

uint32_t ArtMethod::DexBuilder::get_dex_max_seg_size(const DexFile* dex_file)
{
  // unsigned int seg_sizes[6] = { 0 };
	// seg_sizes[0] = dex_file->GetHeader().string_ids_size_ * 4U;
	// seg_sizes[1] = dex_file->GetHeader().type_ids_size_ * 4U;
	// seg_sizes[2] = dex_file->GetHeader().proto_ids_size_ * 12U;
	// seg_sizes[3] = dex_file->GetHeader().field_ids_size_ * 8U;
	// seg_sizes[4] = dex_file->GetHeader().method_ids_size_ * 8U;
	// seg_sizes[5] = dex_file->GetHeader().class_defs_size_ * 32U;

  // auto it = std::max_element(seg_sizes, seg_sizes + 5);
  // return *it;
  return dex_file->Size();
}

void ArtMethod::DexBuilder::repair_Header(const DexFile* dex_file, uint8_t* dex_file_m_)
{
  const DexFile::Header& dexHeader = dex_file->GetHeader();
	unsigned int StrIDAddr = 0x70;
	unsigned int TypeIDAddr = StrIDAddr + dexHeader.string_ids_size_ * 4U;
	unsigned int ProtoIDAddr = TypeIDAddr + dexHeader.type_ids_size_ * 4U;
	unsigned int FieldIDAddr = ProtoIDAddr + dexHeader.proto_ids_size_ * 12U;
	unsigned int MethodIDAddr = FieldIDAddr + dexHeader.field_ids_size_ * 8U;
	unsigned int ClassDefAddr = MethodIDAddr + dexHeader.method_ids_size_ * 8U;
	unsigned int DataAddr = ClassDefAddr + dexHeader.class_defs_size_ * 32U;

  DexFile::Header* dexHeader_ = reinterpret_cast<DexFile::Header*>(dex_file_m_);
	if (DataAddr == dexHeader.data_off_) {
    LOG(art::LogSeverity::INFO) << "Caculation seems accurate! Fix it";
		dexHeader_->string_ids_off_ = StrIDAddr;
		dexHeader_->type_ids_off_ = TypeIDAddr;
		dexHeader_->proto_ids_off_ = ProtoIDAddr;
		dexHeader_->field_ids_off_ = FieldIDAddr;
		dexHeader_->method_ids_off_ = MethodIDAddr;
		dexHeader_->class_defs_off_ = ClassDefAddr;
	} else{
    LOG(art::LogSeverity::ERROR) << "Caculation seems INACCURATE! Not fixing...";
	}
  dexHeader_->header_size_ = 0x70;
  dexHeader_->file_size_ = this->dex_size;
}

void ArtMethod::DexBuilder::repair_ClassDefs(const DexFile* dex_file, uint8_t* dex_file_m_)
{
  // gerat::DexHeader* dex_header = reinterpret_cast<gerat::DexHeader*>(dex_file_m_);
  // uint32_t class_def_rela_off = dex_header->class_defs_off_;
  uint32_t class_def_num = dex_file->NumClassDefs();
  
  for (size_t i = 0; i < class_def_num; i++)
  {
    const DexFile::ClassDef& orig_class_def = dex_file->GetClassDef(i);
    uint32_t class_def_rela_off = reinterpret_cast<const uint8_t*>(&orig_class_def) - reinterpret_cast<const uint8_t*>(dex_file->Begin());
    uint16_t class_idx = orig_class_def.class_idx_;
    gerat::ClassDef* class_def = reinterpret_cast<gerat::ClassDef*>(dex_file_m_ + class_def_rela_off);
    auto it = this->class_data_offset_map.find(class_idx);
    if (it != this->class_data_offset_map.end()) {
      class_def->class_data_off_ = this->class_data_addr - this->begin + this->class_data_offset_map[class_idx];
    }
  }
}

uint8_t* ArtMethod::DexBuilder::copy_DEX_data(const DexFile* dex_file, uint8_t* dex_file_m_)
{
  size_t orig_dex_size = dex_file->Size();
  memcpy(dex_file_m_, dex_file->Begin(), orig_dex_size);
  this->begin = dex_file_m_;
  if (orig_dex_size%16)
    return dex_file_m_ + orig_dex_size + (16 - orig_dex_size%16); // 16 Byte alignment for data segment
  else
    return dex_file_m_ + orig_dex_size;
}

uint8_t* ArtMethod::DexBuilder::copy_code_items(uint8_t* target)
{
  // Copy CodeItems
  memcpy(target, this->code_item_seg, cur_code_item_offset-code_item_seg);
  this->code_item_addr = target;
  return target + (cur_code_item_offset-code_item_seg);
}

uint8_t* ArtMethod::DexBuilder::copy_class_data_items(uint8_t* target)
{
  // Copy ClassDataItems
  // Stream write
  uint8_t* seg_start = this->class_data_addr = target;
  uint8_t* new_target = target;
  for (size_t i = 0; i < class_data_items.size(); i++) {
    
    // Repair code_item_off s for all methods
    // uint32_t method_idx_prev = 0;
    for (size_t j = 0; j < class_data_items[i].methods.size(); j++) {
        uint32_t method_idx = class_data_items[i].methods[j].method_idx;  // Be careful when dealing with methid_idx_delta
        // method_idx_prev = method_idx;
        if (this->code_item_offset_map.find(method_idx) == this->code_item_offset_map.end())
          continue;
        uint32_t seg_off = this->code_item_addr - this->begin;
        uint32_t code_item_off = seg_off + this->code_item_offset_map[method_idx];
        class_data_items[i].methods[j].code_off_ = code_item_off;
    }
    
    // Patch failing methods
    if (class_data_items[i].methods.size() == 0) {
      class_data_items[i].direct_methods_size_ = class_data_items[i].virtual_methods_size_ = 0;
    }

    // Bug here
    // target = class_data_items[i].encode(target);
    new_target = class_data_items[i].encode(target);
    // Record cur ClassDataItem relative offset
    uint16_t class_idx = class_data_items[i].class_idx;
    class_data_offset_map[class_idx] = static_cast<uint32_t>(target - seg_start);
    target = new_target;
    // built_class_idx_set.insert(klass->GetDexClassDefIndex());
  }
  return new_target;
}

void ArtMethod::DexBuilder::set_dex_size(size_t size)
{
  this->dex_size = size;
}

void ArtMethod::DexBuilder::repair_Magic(uint8_t* dex_file_m_)
{
  dex_file_m_[0] = 0x64; 
  dex_file_m_[1] = 0x65;
  dex_file_m_[2] = 0x78;
  dex_file_m_[3] = 0x0a;
  dex_file_m_[4] = 0x30;
  dex_file_m_[5] = 0x33;
  dex_file_m_[6] = 0x35;
  dex_file_m_[7] = 0x00;
}

// Karl Gerat U level 3 unpacking
void ArtMethod::build_dex(Thread* self)
{
  if (UNLIKELY(!gerat::filter)) {
    LOG(ERROR) << "Filter is nullptr? This should not happened";
    return;
  }
   
  std::string component_name = gerat::filter->get_component_name();
  
  // Get DexFile
  const DexFile *dex_file = this->GetDexFile();
  
  // Check DexFile
  auto id = dex_file->FindStringId(component_name.c_str());
  if (!id) {
    // LOG(art::LogSeverity::INFO) << "That's not what I want, address: " << dex_file;
    return;
  }

  std::string this_component_name = std::string(this->GetDeclaringClassDescriptor());
  if (component_name.find(this_component_name) == std::string::npos) {
    return;
  }

  LOG(INFO) << "Start point method declaring class name: " << this_component_name;

  // Check if this DEX is already dumped
  auto it1 = gerat::dex_addrs.find(reinterpret_cast<const void*>(dex_file));
  if (it1 != gerat::dex_addrs.end()) {
    // LOG(INFO) << "Already checked this DEX, skipping, address: " << dex_file;
    return;
  }

  // Declare capture
  LOG(INFO) << "GOT target DEX, address: " << dex_file;
  gerat::dex_addrs.insert(reinterpret_cast<const void*>(dex_file)); // Add current DEX to checked list
  gerat::set_started_flag();

  // Init memory
  DexBuilder* dex_builder = new DexBuilder();
  dex_builder->init_segments(dex_file);
  uint8_t* dex_file_m = reinterpret_cast<uint8_t*>(calloc(dex_file->Size(), 2));

  // Get ClassDefNum
  size_t class_def_num = dex_file->NumClassDefs();
  LOG(INFO) << "Start enumrating " << class_def_num << " ClassDefItems";
  
  auto class_linker = Runtime::Current()->GetClassLinker();

  // Iterate classes for methods
  StackHandleScope<1> hs(self);
  Handle<mirror::ClassLoader> class_loader(hs.NewHandle(this->GetClassLoader()));
  for (uint32_t class_idx = 0; class_idx < class_def_num; class_idx++) {
    // auto it = dex_builder->built_class_idx_set.find(class_idx);
    // if (it == dex_builder->built_class_idx_set.end()) {
    // Get ClassDef
    const DexFile::ClassDef& class_def = dex_file->GetClassDef(class_idx);
    
    // Force Initialize class, youpk's approach
    if (self->IsExceptionPending())
      self->ClearException();
    
    std::string class_name = std::string(dex_file->GetClassDescriptor(class_def));
    if (gerat::filter->should_initialize(class_name))
    {
      mirror::Class* klass = class_linker->FindClass(self, dex_file->GetClassDescriptor(class_def), class_loader);
      if (klass != nullptr) {
        // Build ClassDataItem by mirror::Class dynamically
        if (self->IsExceptionPending())
          self->ClearException();

        LOG(INFO) << "Trying to initialize class " << class_name;

        StackHandleScope<1> hs2(self);
        Handle<mirror::Class> h_class(hs2.NewHandle(klass));
        class_linker->EnsureInitialized(self, h_class, true, true);

        // Build ClassDataItem and CodeItem data
        dex_builder->build_ClassDataItem(klass);
      } else {
        // Build ClassDataItem by ClassDefItem statically
        dex_builder->build_ClassDataItem2(self, this, dex_file, class_def);
      }
    } else
    {
      LOG(INFO) << "Bypass class " << class_name;
    }
    
	}

  uint8_t* code_items_addr = dex_builder->copy_DEX_data(dex_file, dex_file_m);
  uint8_t* class_data_items_addr = dex_builder->copy_code_items(code_items_addr);
  uint8_t* end = dex_builder->copy_class_data_items(class_data_items_addr);
  // dex_builder->copy_class_data_items(class_data_items_addr);
	size_t dex_size_real = end - dex_file_m;
	// size_t dex_size_real = 2*dex_file->Size();
  dex_builder->set_dex_size(dex_size_real);

	dex_builder->repair_Header(dex_file, dex_file_m);
	dex_builder->repair_ClassDefs(dex_file, dex_file_m);

  // Fix Header and Index
  dex_builder->repair_Magic(dex_file_m); // Repair DEX's Magic
  // Write to storage
  write_file(dex_file_m, dex_size_real);

  // Cleaning work
  free(dex_file_m);
  dex_file_m = nullptr;
  dex_builder->clear_segments();
  free(dex_file_m);
  delete dex_builder;
}

ArtMethod* ArtMethod::FromReflectedMethod(const ScopedObjectAccessAlreadyRunnable& soa,
                                          jobject jlr_method) {
  auto* abstract_method = soa.Decode<mirror::AbstractMethod*>(jlr_method);
  DCHECK(abstract_method != nullptr);
  return abstract_method->GetArtMethod();
}

mirror::String* ArtMethod::GetNameAsString(Thread* self) {
  CHECK(!IsProxyMethod());
  StackHandleScope<1> hs(self);
  Handle<mirror::DexCache> dex_cache(hs.NewHandle(GetDexCache()));
  auto* dex_file = dex_cache->GetDexFile();
  uint32_t dex_method_idx = GetDexMethodIndex();
  const DexFile::MethodId& method_id = dex_file->GetMethodId(dex_method_idx);
  return Runtime::Current()->GetClassLinker()->ResolveString(*dex_file, method_id.name_idx_,
                                                             dex_cache);
}

InvokeType ArtMethod::GetInvokeType() {
  // TODO: kSuper?
  if (GetDeclaringClass()->IsInterface()) {
    return kInterface;
  } else if (IsStatic()) {
    return kStatic;
  } else if (IsDirect()) {
    return kDirect;
  } else {
    return kVirtual;
  }
}

size_t ArtMethod::NumArgRegisters(const StringPiece& shorty) {
  CHECK_LE(1U, shorty.length());
  uint32_t num_registers = 0;
  for (size_t i = 1; i < shorty.length(); ++i) {
    char ch = shorty[i];
    if (ch == 'D' || ch == 'J') {
      num_registers += 2;
    } else {
      num_registers += 1;
    }
  }
  return num_registers;
}

static bool HasSameNameAndSignature(ArtMethod* method1, ArtMethod* method2)
    SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
  ScopedAssertNoThreadSuspension ants(Thread::Current(), "HasSameNameAndSignature");
  const DexFile* dex_file = method1->GetDexFile();
  const DexFile::MethodId& mid = dex_file->GetMethodId(method1->GetDexMethodIndex());
  if (method1->GetDexCache() == method2->GetDexCache()) {
    const DexFile::MethodId& mid2 = dex_file->GetMethodId(method2->GetDexMethodIndex());
    return mid.name_idx_ == mid2.name_idx_ && mid.proto_idx_ == mid2.proto_idx_;
  }
  const DexFile* dex_file2 = method2->GetDexFile();
  const DexFile::MethodId& mid2 = dex_file2->GetMethodId(method2->GetDexMethodIndex());
  if (!DexFileStringEquals(dex_file, mid.name_idx_, dex_file2, mid2.name_idx_)) {
    return false;  // Name mismatch.
  }
  return dex_file->GetMethodSignature(mid) == dex_file2->GetMethodSignature(mid2);
}

ArtMethod* ArtMethod::FindOverriddenMethod(size_t pointer_size) {
  if (IsStatic()) {
    return nullptr;
  }
  mirror::Class* declaring_class = GetDeclaringClass();
  mirror::Class* super_class = declaring_class->GetSuperClass();
  uint16_t method_index = GetMethodIndex();
  ArtMethod* result = nullptr;
  // Did this method override a super class method? If so load the result from the super class'
  // vtable
  if (super_class->HasVTable() && method_index < super_class->GetVTableLength()) {
    result = super_class->GetVTableEntry(method_index, pointer_size);
  } else {
    // Method didn't override superclass method so search interfaces
    if (IsProxyMethod()) {
      result = GetDexCacheResolvedMethods()->GetElementPtrSize<ArtMethod*>(
          GetDexMethodIndex(), pointer_size);
      CHECK_EQ(result,
               Runtime::Current()->GetClassLinker()->FindMethodForProxy(GetDeclaringClass(), this));
    } else {
      mirror::IfTable* iftable = GetDeclaringClass()->GetIfTable();
      for (size_t i = 0; i < iftable->Count() && result == nullptr; i++) {
        mirror::Class* interface = iftable->GetInterface(i);
        for (size_t j = 0; j < interface->NumVirtualMethods(); ++j) {
          ArtMethod* interface_method = interface->GetVirtualMethod(j, pointer_size);
          if (HasSameNameAndSignature(
              this, interface_method->GetInterfaceMethodIfProxy(sizeof(void*)))) {
            result = interface_method;
            break;
          }
        }
      }
    }
  }
  DCHECK(result == nullptr || HasSameNameAndSignature(
      GetInterfaceMethodIfProxy(sizeof(void*)), result->GetInterfaceMethodIfProxy(sizeof(void*))));
  return result;
}

uint32_t ArtMethod::FindDexMethodIndexInOtherDexFile(const DexFile& other_dexfile,
                                                     uint32_t name_and_signature_idx) {
  const DexFile* dexfile = GetDexFile();
  const uint32_t dex_method_idx = GetDexMethodIndex();
  const DexFile::MethodId& mid = dexfile->GetMethodId(dex_method_idx);
  const DexFile::MethodId& name_and_sig_mid = other_dexfile.GetMethodId(name_and_signature_idx);
  DCHECK_STREQ(dexfile->GetMethodName(mid), other_dexfile.GetMethodName(name_and_sig_mid));
  DCHECK_EQ(dexfile->GetMethodSignature(mid), other_dexfile.GetMethodSignature(name_and_sig_mid));
  if (dexfile == &other_dexfile) {
    return dex_method_idx;
  }
  const char* mid_declaring_class_descriptor = dexfile->StringByTypeIdx(mid.class_idx_);
  const DexFile::StringId* other_descriptor =
      other_dexfile.FindStringId(mid_declaring_class_descriptor);
  if (other_descriptor != nullptr) {
    const DexFile::TypeId* other_type_id =
        other_dexfile.FindTypeId(other_dexfile.GetIndexForStringId(*other_descriptor));
    if (other_type_id != nullptr) {
      const DexFile::MethodId* other_mid = other_dexfile.FindMethodId(
          *other_type_id, other_dexfile.GetStringId(name_and_sig_mid.name_idx_),
          other_dexfile.GetProtoId(name_and_sig_mid.proto_idx_));
      if (other_mid != nullptr) {
        return other_dexfile.GetIndexForMethodId(*other_mid);
      }
    }
  }
  return DexFile::kDexNoIndex;
}

uint32_t ArtMethod::ToDexPc(const uintptr_t pc, bool abort_on_failure) {
  const void* entry_point = GetQuickOatEntryPoint(sizeof(void*));
  uint32_t sought_offset = pc - reinterpret_cast<uintptr_t>(entry_point);
  if (IsOptimized(sizeof(void*))) {
    CodeInfo code_info = GetOptimizedCodeInfo();
    StackMap stack_map = code_info.GetStackMapForNativePcOffset(sought_offset);
    if (stack_map.IsValid()) {
      return stack_map.GetDexPc(code_info);
    }
  } else {
    MappingTable table(entry_point != nullptr ?
        GetMappingTable(EntryPointToCodePointer(entry_point), sizeof(void*)) : nullptr);
    if (table.TotalSize() == 0) {
      // NOTE: Special methods (see Mir2Lir::GenSpecialCase()) have an empty mapping
      // but they have no suspend checks and, consequently, we never call ToDexPc() for them.
      DCHECK(IsNative() || IsCalleeSaveMethod() || IsProxyMethod()) << PrettyMethod(this);
      return DexFile::kDexNoIndex;   // Special no mapping case
    }
    // Assume the caller wants a pc-to-dex mapping so check here first.
    typedef MappingTable::PcToDexIterator It;
    for (It cur = table.PcToDexBegin(), end = table.PcToDexEnd(); cur != end; ++cur) {
      if (cur.NativePcOffset() == sought_offset) {
        return cur.DexPc();
      }
    }
    // Now check dex-to-pc mappings.
    typedef MappingTable::DexToPcIterator It2;
    for (It2 cur = table.DexToPcBegin(), end = table.DexToPcEnd(); cur != end; ++cur) {
      if (cur.NativePcOffset() == sought_offset) {
        return cur.DexPc();
      }
    }
  }
  if (abort_on_failure) {
      LOG(FATAL) << "Failed to find Dex offset for PC offset " << reinterpret_cast<void*>(sought_offset)
             << "(PC " << reinterpret_cast<void*>(pc) << ", entry_point=" << entry_point
             << " current entry_point=" << GetQuickOatEntryPoint(sizeof(void*))
             << ") in " << PrettyMethod(this);
  }
  return DexFile::kDexNoIndex;
}

uintptr_t ArtMethod::ToNativeQuickPc(const uint32_t dex_pc, bool abort_on_failure) {
  const void* entry_point = GetQuickOatEntryPoint(sizeof(void*));
  MappingTable table(entry_point != nullptr ?
      GetMappingTable(EntryPointToCodePointer(entry_point), sizeof(void*)) : nullptr);
  if (table.TotalSize() == 0) {
    DCHECK_EQ(dex_pc, 0U);
    return 0;   // Special no mapping/pc == 0 case
  }
  // Assume the caller wants a dex-to-pc mapping so check here first.
  typedef MappingTable::DexToPcIterator It;
  for (It cur = table.DexToPcBegin(), end = table.DexToPcEnd(); cur != end; ++cur) {
    if (cur.DexPc() == dex_pc) {
      return reinterpret_cast<uintptr_t>(entry_point) + cur.NativePcOffset();
    }
  }
  // Now check pc-to-dex mappings.
  typedef MappingTable::PcToDexIterator It2;
  for (It2 cur = table.PcToDexBegin(), end = table.PcToDexEnd(); cur != end; ++cur) {
    if (cur.DexPc() == dex_pc) {
      return reinterpret_cast<uintptr_t>(entry_point) + cur.NativePcOffset();
    }
  }
  if (abort_on_failure) {
    LOG(FATAL) << "Failed to find native offset for dex pc 0x" << std::hex << dex_pc
               << " in " << PrettyMethod(this);
  }
  return UINTPTR_MAX;
}

uint32_t ArtMethod::FindCatchBlock(Handle<mirror::Class> exception_type,
                                   uint32_t dex_pc, bool* has_no_move_exception) {
  const DexFile::CodeItem* code_item = GetCodeItem();
  // Set aside the exception while we resolve its type.
  Thread* self = Thread::Current();
  StackHandleScope<1> hs(self);
  Handle<mirror::Throwable> exception(hs.NewHandle(self->GetException()));
  self->ClearException();
  // Default to handler not found.
  uint32_t found_dex_pc = DexFile::kDexNoIndex;
  // Iterate over the catch handlers associated with dex_pc.
  for (CatchHandlerIterator it(*code_item, dex_pc); it.HasNext(); it.Next()) {
    uint16_t iter_type_idx = it.GetHandlerTypeIndex();
    // Catch all case
    if (iter_type_idx == DexFile::kDexNoIndex16) {
      found_dex_pc = it.GetHandlerAddress();
      break;
    }
    // Does this catch exception type apply?
    mirror::Class* iter_exception_type = GetClassFromTypeIndex(iter_type_idx, true);
    if (UNLIKELY(iter_exception_type == nullptr)) {
      // Now have a NoClassDefFoundError as exception. Ignore in case the exception class was
      // removed by a pro-guard like tool.
      // Note: this is not RI behavior. RI would have failed when loading the class.
      self->ClearException();
      // Delete any long jump context as this routine is called during a stack walk which will
      // release its in use context at the end.
      delete self->GetLongJumpContext();
      LOG(WARNING) << "Unresolved exception class when finding catch block: "
        << DescriptorToDot(GetTypeDescriptorFromTypeIdx(iter_type_idx));
    } else if (iter_exception_type->IsAssignableFrom(exception_type.Get())) {
      found_dex_pc = it.GetHandlerAddress();
      break;
    }
  }
  if (found_dex_pc != DexFile::kDexNoIndex) {
    const Instruction* first_catch_instr =
        Instruction::At(&code_item->insns_[found_dex_pc]);
    *has_no_move_exception = (first_catch_instr->Opcode() != Instruction::MOVE_EXCEPTION);
  }
  // Put the exception back.
  if (exception.Get() != nullptr) {
    self->SetException(exception.Get());
  }
  return found_dex_pc;
}

void ArtMethod::AssertPcIsWithinQuickCode(uintptr_t pc) {
  if (IsNative() || IsRuntimeMethod() || IsProxyMethod()) {
    return;
  }
  if (pc == reinterpret_cast<uintptr_t>(GetQuickInstrumentationExitPc())) {
    return;
  }
  const void* code = GetEntryPointFromQuickCompiledCode();
  if (code == GetQuickInstrumentationEntryPoint()) {
    return;
  }
  ClassLinker* class_linker = Runtime::Current()->GetClassLinker();
  if (class_linker->IsQuickToInterpreterBridge(code) ||
      class_linker->IsQuickResolutionStub(code)) {
    return;
  }
  // If we are the JIT then we may have just compiled the method after the
  // IsQuickToInterpreterBridge check.
  jit::Jit* const jit = Runtime::Current()->GetJit();
  if (jit != nullptr &&
      jit->GetCodeCache()->ContainsCodePtr(reinterpret_cast<const void*>(code))) {
    return;
  }
  /*
   * During a stack walk, a return PC may point past-the-end of the code
   * in the case that the last instruction is a call that isn't expected to
   * return.  Thus, we check <= code + GetCodeSize().
   *
   * NOTE: For Thumb both pc and code are offset by 1 indicating the Thumb state.
   */
  CHECK(PcIsWithinQuickCode(reinterpret_cast<uintptr_t>(code), pc))
      << PrettyMethod(this)
      << " pc=" << std::hex << pc
      << " code=" << code
      << " size=" << GetCodeSize(
          EntryPointToCodePointer(reinterpret_cast<const void*>(code)));
}

bool ArtMethod::IsEntrypointInterpreter() {
  ClassLinker* class_linker = Runtime::Current()->GetClassLinker();
  const void* oat_quick_code = class_linker->GetOatMethodQuickCodeFor(this);
  return oat_quick_code == nullptr || oat_quick_code != GetEntryPointFromQuickCompiledCode();
}

const void* ArtMethod::GetQuickOatEntryPoint(size_t pointer_size) {
  if (IsAbstract() || IsRuntimeMethod() || IsProxyMethod()) {
    return nullptr;
  }
  Runtime* runtime = Runtime::Current();
  ClassLinker* class_linker = runtime->GetClassLinker();
  const void* code = runtime->GetInstrumentation()->GetQuickCodeFor(this, pointer_size);
  // On failure, instead of null we get the quick-generic-jni-trampoline for native method
  // indicating the generic JNI, or the quick-to-interpreter-bridge (but not the trampoline)
  // for non-native methods.
  if (class_linker->IsQuickToInterpreterBridge(code) ||
      class_linker->IsQuickGenericJniStub(code)) {
    return nullptr;
  }
  return code;
}

#ifndef NDEBUG
uintptr_t ArtMethod::NativeQuickPcOffset(const uintptr_t pc, const void* quick_entry_point) {
  CHECK_NE(quick_entry_point, GetQuickToInterpreterBridge());
  CHECK_EQ(quick_entry_point,
           Runtime::Current()->GetInstrumentation()->GetQuickCodeFor(this, sizeof(void*)));
  return pc - reinterpret_cast<uintptr_t>(quick_entry_point);
}
#endif

void ArtMethod::Invoke(Thread* self, uint32_t* args, uint32_t args_size, JValue* result,
                       const char* shorty) {
  if (UNLIKELY(__builtin_frame_address(0) < self->GetStackEnd())) {
    ThrowStackOverflowError(self);
    return;
  }

  // Karl Gerat U
  // Sieg Heil

  // Read the config
  std::call_once(gerat::filter_inited, &gerat::init);
  if (!gerat::started) // Unpacking process not started yet, prevent recursive unpacking
  {
    if (gerat::filter && gerat::filter->get_flag()) {
      if (gerat::filter->get_force_init_flag()) {
        // std::shared_ptr<std::thread> t1(new std::thread(std::bind(&ArtMethod::dump_dex_after_init, this, self)));
        // t1->detach();
        std::call_once(gerat::unpack_mode_file_checked, std::bind(&gerat::KarlGeratUFilter::read_list_files, gerat::filter));
        if (gerat::filter->get_rebuild_flag())
          build_dex(self); // level3
        else
          dump_dex_after_init(self); // level2
      } else {
        // std::shared_ptr<std::thread> t(new std::thread(std::bind(&ArtMethod::dump_dex, this)));
        // t->detach();
        dump_dex(); // level1
      }
    }
  }
  

  if (kIsDebugBuild) {
    self->AssertThreadSuspensionIsAllowable();
    CHECK_EQ(kRunnable, self->GetState());
    CHECK_STREQ(GetInterfaceMethodIfProxy(sizeof(void*))->GetShorty(), shorty);
  }

  // Push a transition back into managed code onto the linked list in thread.
  ManagedStack fragment;
  self->PushManagedStackFragment(&fragment);

  Runtime* runtime = Runtime::Current();
  // Call the invoke stub, passing everything as arguments.
  // If the runtime is not yet started or it is required by the debugger, then perform the
  // Invocation by the interpreter.
  if (UNLIKELY(!runtime->IsStarted() || Dbg::IsForcedInterpreterNeededForCalling(self, this))) {
    if (IsStatic()) {
      art::interpreter::EnterInterpreterFromInvoke(self, this, nullptr, args, result);
    } else {
      mirror::Object* receiver =
          reinterpret_cast<StackReference<mirror::Object>*>(&args[0])->AsMirrorPtr();
      art::interpreter::EnterInterpreterFromInvoke(self, this, receiver, args + 1, result);
    }
  } else {
    DCHECK_EQ(runtime->GetClassLinker()->GetImagePointerSize(), sizeof(void*));

    constexpr bool kLogInvocationStartAndReturn = false;
    bool have_quick_code = GetEntryPointFromQuickCompiledCode() != nullptr;
    if (LIKELY(have_quick_code)) {
      if (kLogInvocationStartAndReturn) {
        LOG(INFO) << StringPrintf(
            "Invoking '%s' quick code=%p static=%d", PrettyMethod(this).c_str(),
            GetEntryPointFromQuickCompiledCode(), static_cast<int>(IsStatic() ? 1 : 0));
      }

      // Ensure that we won't be accidentally calling quick compiled code when -Xint.
      if (kIsDebugBuild && runtime->GetInstrumentation()->IsForcedInterpretOnly()) {
        DCHECK(!runtime->UseJit());
        CHECK(IsEntrypointInterpreter())
            << "Don't call compiled code when -Xint " << PrettyMethod(this);
      }

#if defined(__LP64__) || defined(__arm__) || defined(__i386__)
      if (!IsStatic()) {
        (*art_quick_invoke_stub)(this, args, args_size, self, result, shorty);
      } else {
        (*art_quick_invoke_static_stub)(this, args, args_size, self, result, shorty);
      }
#else
      (*art_quick_invoke_stub)(this, args, args_size, self, result, shorty);
#endif
      if (UNLIKELY(self->GetException() == Thread::GetDeoptimizationException())) {
        // Unusual case where we were running generated code and an
        // exception was thrown to force the activations to be removed from the
        // stack. Continue execution in the interpreter.
        self->ClearException();
        ShadowFrame* shadow_frame =
            self->PopStackedShadowFrame(StackedShadowFrameType::kDeoptimizationShadowFrame);
        result->SetJ(self->PopDeoptimizationReturnValue().GetJ());
        self->SetTopOfStack(nullptr);
        self->SetTopOfShadowStack(shadow_frame);
        interpreter::EnterInterpreterFromDeoptimize(self, shadow_frame, result);
      }
      if (kLogInvocationStartAndReturn) {
        LOG(INFO) << StringPrintf("Returned '%s' quick code=%p", PrettyMethod(this).c_str(),
                                  GetEntryPointFromQuickCompiledCode());
      }
    } else {
      LOG(INFO) << "Not invoking '" << PrettyMethod(this) << "' code=null";
      if (result != nullptr) {
        result->SetJ(0);
      }
    }
  }

  // Pop transition.
  self->PopManagedStackFragment(fragment);
}

// Counts the number of references in the parameter list of the corresponding method.
// Note: Thus does _not_ include "this" for non-static methods.
static uint32_t GetNumberOfReferenceArgsWithoutReceiver(ArtMethod* method)
    SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
  uint32_t shorty_len;
  const char* shorty = method->GetShorty(&shorty_len);
  uint32_t refs = 0;
  for (uint32_t i = 1; i < shorty_len ; ++i) {
    if (shorty[i] == 'L') {
      refs++;
    }
  }
  return refs;
}

QuickMethodFrameInfo ArtMethod::GetQuickFrameInfo() {
  Runtime* runtime = Runtime::Current();

  if (UNLIKELY(IsAbstract())) {
    return runtime->GetCalleeSaveMethodFrameInfo(Runtime::kRefsAndArgs);
  }

  // This goes before IsProxyMethod since runtime methods have a null declaring class.
  if (UNLIKELY(IsRuntimeMethod())) {
    return runtime->GetRuntimeMethodFrameInfo(this);
  }

  // For Proxy method we add special handling for the direct method case  (there is only one
  // direct method - constructor). Direct method is cloned from original
  // java.lang.reflect.Proxy class together with code and as a result it is executed as usual
  // quick compiled method without any stubs. So the frame info should be returned as it is a
  // quick method not a stub. However, if instrumentation stubs are installed, the
  // instrumentation->GetQuickCodeFor() returns the artQuickProxyInvokeHandler instead of an
  // oat code pointer, thus we have to add a special case here.
  if (UNLIKELY(IsProxyMethod())) {
    if (IsDirect()) {
      CHECK(IsConstructor());
      return GetQuickFrameInfo(EntryPointToCodePointer(GetEntryPointFromQuickCompiledCode()));
    } else {
      return runtime->GetCalleeSaveMethodFrameInfo(Runtime::kRefsAndArgs);
    }
  }

  const void* entry_point = runtime->GetInstrumentation()->GetQuickCodeFor(this, sizeof(void*));
  ClassLinker* class_linker = runtime->GetClassLinker();
  // On failure, instead of null we get the quick-generic-jni-trampoline for native method
  // indicating the generic JNI, or the quick-to-interpreter-bridge (but not the trampoline)
  // for non-native methods. And we really shouldn't see a failure for non-native methods here.
  DCHECK(!class_linker->IsQuickToInterpreterBridge(entry_point));

  if (class_linker->IsQuickGenericJniStub(entry_point)) {
    // Generic JNI frame.
    DCHECK(IsNative());
    uint32_t handle_refs = GetNumberOfReferenceArgsWithoutReceiver(this) + 1;
    size_t scope_size = HandleScope::SizeOf(handle_refs);
    QuickMethodFrameInfo callee_info = runtime->GetCalleeSaveMethodFrameInfo(Runtime::kRefsAndArgs);

    // Callee saves + handle scope + method ref + alignment
    // Note: -sizeof(void*) since callee-save frame stores a whole method pointer.
    size_t frame_size = RoundUp(callee_info.FrameSizeInBytes() - sizeof(void*) +
                                sizeof(ArtMethod*) + scope_size, kStackAlignment);
    return QuickMethodFrameInfo(frame_size, callee_info.CoreSpillMask(), callee_info.FpSpillMask());
  }

  const void* code_pointer = EntryPointToCodePointer(entry_point);
  return GetQuickFrameInfo(code_pointer);
}

void ArtMethod::RegisterNative(const void* native_method, bool is_fast) {
  CHECK(IsNative()) << PrettyMethod(this);
  CHECK(!IsFastNative()) << PrettyMethod(this);
  CHECK(native_method != nullptr) << PrettyMethod(this);
  if (is_fast) {
    SetAccessFlags(GetAccessFlags() | kAccFastNative);
  }
  SetEntryPointFromJni(native_method);
}

void ArtMethod::UnregisterNative() {
  CHECK(IsNative() && !IsFastNative()) << PrettyMethod(this);
  // restore stub to lookup native pointer via dlsym
  RegisterNative(GetJniDlsymLookupStub(), false);
}

bool ArtMethod::EqualParameters(Handle<mirror::ObjectArray<mirror::Class>> params) {
  auto* dex_cache = GetDexCache();
  auto* dex_file = dex_cache->GetDexFile();
  const auto& method_id = dex_file->GetMethodId(GetDexMethodIndex());
  const auto& proto_id = dex_file->GetMethodPrototype(method_id);
  const DexFile::TypeList* proto_params = dex_file->GetProtoParameters(proto_id);
  auto count = proto_params != nullptr ? proto_params->Size() : 0u;
  auto param_len = params.Get() != nullptr ? params->GetLength() : 0u;
  if (param_len != count) {
    return false;
  }
  auto* cl = Runtime::Current()->GetClassLinker();
  for (size_t i = 0; i < count; ++i) {
    auto type_idx = proto_params->GetTypeItem(i).type_idx_;
    auto* type = cl->ResolveType(type_idx, this);
    if (type == nullptr) {
      Thread::Current()->AssertPendingException();
      return false;
    }
    if (type != params->GetWithoutChecks(i)) {
      return false;
    }
  }
  return true;
}

}  // namespace art
