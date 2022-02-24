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

#include <stdio.h>
#include <stdlib.h>

#include <fstream>
#include <iostream>
#include <map>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

#include "arch/instruction_set_features.h"
#include "art_field-inl.h"
#include "art_method-inl.h"
#include "base/unix_file/fd_file.h"
#include "class_linker.h"
#include "class_linker-inl.h"
#include "dex_file-inl.h"
#include "dex_instruction.h"
#include "disassembler.h"
#include "elf_builder.h"
#include "gc_map.h"
#include "gc/space/image_space.h"
#include "gc/space/large_object_space.h"
#include "gc/space/space-inl.h"
#include "image.h"
#include "indenter.h"
#include "mapping_table.h"
#include "mirror/array-inl.h"
#include "mirror/class-inl.h"
#include "mirror/object-inl.h"
#include "mirror/object_array-inl.h"
#include "oat.h"
#include "oat_file-inl.h"
#include "os.h"
#include "output_stream.h"
#include "safe_map.h"
#include "scoped_thread_state_change.h"
#include "ScopedLocalRef.h"
#include "thread_list.h"
#include "verifier/dex_gc_map.h"
#include "verifier/method_verifier.h"
#include "vmap_table.h"
#include "well_known_classes.h"

#include <sys/stat.h>
#include "cmdline.h"

namespace art {

const char* image_methods_descriptions_[] = {
  "kResolutionMethod",
  "kImtConflictMethod",
  "kImtUnimplementedMethod",
  "kCalleeSaveMethod",
  "kRefsOnlySaveMethod",
  "kRefsAndArgsSaveMethod",
};

const char* image_roots_descriptions_[] = {
  "kDexCaches",
  "kClassRoots",
};

class OatSymbolizer FINAL {
 public:
  class RodataWriter FINAL : public CodeOutput {
   public:
    explicit RodataWriter(const OatFile* oat_file) : oat_file_(oat_file) {}

    bool Write(OutputStream* out) OVERRIDE {
      const size_t rodata_size = oat_file_->GetOatHeader().GetExecutableOffset();
      return out->WriteFully(oat_file_->Begin(), rodata_size);
    }

   private:
    const OatFile* oat_file_;
  };

  class TextWriter FINAL : public CodeOutput {
   public:
    explicit TextWriter(const OatFile* oat_file) : oat_file_(oat_file) {}

    bool Write(OutputStream* out) OVERRIDE {
      const size_t rodata_size = oat_file_->GetOatHeader().GetExecutableOffset();
      const uint8_t* text_begin = oat_file_->Begin() + rodata_size;
      return out->WriteFully(text_begin, oat_file_->End() - text_begin);
    }

   private:
    const OatFile* oat_file_;
  };

  explicit OatSymbolizer(const OatFile* oat_file, const std::string& output_name) :
      oat_file_(oat_file), builder_(nullptr),
      output_name_(output_name.empty() ? "symbolized.oat" : output_name) {
  }

  typedef void (OatSymbolizer::*Callback)(const DexFile::ClassDef&,
                                          uint32_t,
                                          const OatFile::OatMethod&,
                                          const DexFile&,
                                          uint32_t,
                                          const DexFile::CodeItem*,
                                          uint32_t);

  bool Symbolize() {
    Elf32_Word rodata_size = oat_file_->GetOatHeader().GetExecutableOffset();
    uint32_t size = static_cast<uint32_t>(oat_file_->End() - oat_file_->Begin());
    uint32_t text_size = size - rodata_size;
    uint32_t bss_size = oat_file_->BssSize();
    RodataWriter rodata_writer(oat_file_);
    TextWriter text_writer(oat_file_);
    builder_.reset(new ElfBuilder<ElfTypes32>(
        oat_file_->GetOatHeader().GetInstructionSet(),
        rodata_size, &rodata_writer,
        text_size, &text_writer,
        bss_size));

    Walk(&art::OatSymbolizer::RegisterForDedup);

    NormalizeState();

    Walk(&art::OatSymbolizer::AddSymbol);

    File* elf_output = OS::CreateEmptyFile(output_name_.c_str());
    bool result = builder_->Write(elf_output);

    // Ignore I/O errors.
    UNUSED(elf_output->FlushClose());

    return result;
  }

  void Walk(Callback callback) {
    std::vector<const OatFile::OatDexFile*> oat_dex_files = oat_file_->GetOatDexFiles();
    for (size_t i = 0; i < oat_dex_files.size(); i++) {
      const OatFile::OatDexFile* oat_dex_file = oat_dex_files[i];
      CHECK(oat_dex_file != nullptr);
      WalkOatDexFile(oat_dex_file, callback);
    }
  }

  void WalkOatDexFile(const OatFile::OatDexFile* oat_dex_file, Callback callback) {
    std::string error_msg;
    std::unique_ptr<const DexFile> dex_file(oat_dex_file->OpenDexFile(&error_msg));
    if (dex_file.get() == nullptr) {
      return;
    }
    for (size_t class_def_index = 0;
        class_def_index < dex_file->NumClassDefs();
        class_def_index++) {
      const DexFile::ClassDef& class_def = dex_file->GetClassDef(class_def_index);
      const OatFile::OatClass oat_class = oat_dex_file->GetOatClass(class_def_index);
      OatClassType type = oat_class.GetType();
      switch (type) {
        case kOatClassAllCompiled:
        case kOatClassSomeCompiled:
          WalkOatClass(oat_class, *dex_file.get(), class_def, callback);
          break;

        case kOatClassNoneCompiled:
        case kOatClassMax:
          // Ignore.
          break;
      }
    }
  }

  void WalkOatClass(const OatFile::OatClass& oat_class, const DexFile& dex_file,
                    const DexFile::ClassDef& class_def, Callback callback) {
    const uint8_t* class_data = dex_file.GetClassData(class_def);
    if (class_data == nullptr) {  // empty class such as a marker interface?
      return;
    }
    // Note: even if this is an interface or a native class, we still have to walk it, as there
    //       might be a static initializer.
    ClassDataItemIterator it(dex_file, class_data);
    SkipAllFields(&it);
    uint32_t class_method_idx = 0;
    while (it.HasNextDirectMethod()) {
      const OatFile::OatMethod oat_method = oat_class.GetOatMethod(class_method_idx);
      WalkOatMethod(class_def, class_method_idx, oat_method, dex_file, it.GetMemberIndex(),
                    it.GetMethodCodeItem(), it.GetMethodAccessFlags(), callback);
      class_method_idx++;
      it.Next();
    }
    while (it.HasNextVirtualMethod()) {
      const OatFile::OatMethod oat_method = oat_class.GetOatMethod(class_method_idx);
      WalkOatMethod(class_def, class_method_idx, oat_method, dex_file, it.GetMemberIndex(),
                    it.GetMethodCodeItem(), it.GetMethodAccessFlags(), callback);
      class_method_idx++;
      it.Next();
    }
    DCHECK(!it.HasNext());
  }

  void WalkOatMethod(const DexFile::ClassDef& class_def, uint32_t class_method_index,
                     const OatFile::OatMethod& oat_method, const DexFile& dex_file,
                     uint32_t dex_method_idx, const DexFile::CodeItem* code_item,
                     uint32_t method_access_flags, Callback callback) {
    if ((method_access_flags & kAccAbstract) != 0) {
      // Abstract method, no code.
      return;
    }
    if (oat_method.GetCodeOffset() == 0) {
      // No code.
      return;
    }

    (this->*callback)(class_def, class_method_index, oat_method, dex_file, dex_method_idx, code_item,
                      method_access_flags);
  }

  void RegisterForDedup(const DexFile::ClassDef& class_def ATTRIBUTE_UNUSED,
                        uint32_t class_method_index ATTRIBUTE_UNUSED,
                        const OatFile::OatMethod& oat_method,
                        const DexFile& dex_file ATTRIBUTE_UNUSED,
                        uint32_t dex_method_idx ATTRIBUTE_UNUSED,
                        const DexFile::CodeItem* code_item ATTRIBUTE_UNUSED,
                        uint32_t method_access_flags ATTRIBUTE_UNUSED) {
    state_[oat_method.GetCodeOffset()]++;
  }

  void NormalizeState() {
    for (auto& x : state_) {
      if (x.second == 1) {
        state_[x.first] = 0;
      }
    }
  }

  enum class DedupState {  // private
    kNotDeduplicated,
    kDeduplicatedFirst,
    kDeduplicatedOther
  };
  DedupState IsDuplicated(uint32_t offset) {
    if (state_[offset] == 0) {
      return DedupState::kNotDeduplicated;
    }
    if (state_[offset] == 1) {
      return DedupState::kDeduplicatedOther;
    }
    state_[offset] = 1;
    return DedupState::kDeduplicatedFirst;
  }

  void AddSymbol(const DexFile::ClassDef& class_def ATTRIBUTE_UNUSED,
                 uint32_t class_method_index ATTRIBUTE_UNUSED,
                 const OatFile::OatMethod& oat_method,
                 const DexFile& dex_file,
                 uint32_t dex_method_idx,
                 const DexFile::CodeItem* code_item ATTRIBUTE_UNUSED,
                 uint32_t method_access_flags ATTRIBUTE_UNUSED) {
    DedupState dedup = IsDuplicated(oat_method.GetCodeOffset());
    if (dedup != DedupState::kDeduplicatedOther) {
      std::string pretty_name = PrettyMethod(dex_method_idx, dex_file, true);

      if (dedup == DedupState::kDeduplicatedFirst) {
        pretty_name = "[Dedup]" + pretty_name;
      }

      auto* symtab = builder_->GetSymtab();

      symtab->AddSymbol(pretty_name, builder_->GetText(),
          oat_method.GetCodeOffset() - oat_file_->GetOatHeader().GetExecutableOffset(),
          true, oat_method.GetQuickCodeSize(), STB_GLOBAL, STT_FUNC);
    }
  }

 private:
  static void SkipAllFields(ClassDataItemIterator* it) {
    while (it->HasNextStaticField()) {
      it->Next();
    }
    while (it->HasNextInstanceField()) {
      it->Next();
    }
  }

  const OatFile* oat_file_;
  std::unique_ptr<ElfBuilder<ElfTypes32> > builder_;
  std::unordered_map<uint32_t, uint32_t> state_;
  const std::string output_name_;
};

class OatDumperOptions {
 public:
  OatDumperOptions(bool dump_raw_mapping_table,
                   bool dump_raw_gc_map,
                   bool dump_vmap,
                   bool disassemble_code,
                   bool absolute_addresses,
                   const char* class_filter,
                   const char* method_filter,
                   bool list_classes,
                   bool list_methods,
                   const char* export_dex_location,
                   uint32_t addr2instr)
    : dump_raw_mapping_table_(dump_raw_mapping_table),
      dump_raw_gc_map_(dump_raw_gc_map),
      dump_vmap_(dump_vmap),
      disassemble_code_(disassemble_code),
      absolute_addresses_(absolute_addresses),
      class_filter_(class_filter),
      method_filter_(method_filter),
      list_classes_(list_classes),
      list_methods_(list_methods),
      export_dex_location_(export_dex_location),
      addr2instr_(addr2instr),
      class_loader_(nullptr) {}

  const bool dump_raw_mapping_table_;
  const bool dump_raw_gc_map_;
  const bool dump_vmap_;
  const bool disassemble_code_;
  const bool absolute_addresses_;
  const char* const class_filter_;
  const char* const method_filter_;
  const bool list_classes_;
  const bool list_methods_;
  const char* const export_dex_location_;
  uint32_t addr2instr_;
  Handle<mirror::ClassLoader>* class_loader_;
};

class OatDumper {
 public:
  explicit OatDumper(const OatFile& oat_file, const OatDumperOptions& options)
    : oat_file_(oat_file),
      oat_dex_files_(oat_file.GetOatDexFiles()),
      options_(options),
      resolved_addr2instr_(0),
      instruction_set_(oat_file_.GetOatHeader().GetInstructionSet()),
      disassembler_(Disassembler::Create(instruction_set_,
                                         new DisassemblerOptions(options_.absolute_addresses_,
                                                                 oat_file.Begin(),
                                                                 true /* can_read_litals_ */))) {
    CHECK(options_.class_loader_ != nullptr);
    CHECK(options_.class_filter_ != nullptr);
    CHECK(options_.method_filter_ != nullptr);
    AddAllOffsets();
  }

  ~OatDumper() {
    delete disassembler_;
  }

  InstructionSet GetInstructionSet() {
    return instruction_set_;
  }

  bool Dump(std::ostream& os) {
    bool success = true;
    const OatHeader& oat_header = oat_file_.GetOatHeader();

    os << "MAGIC:\n";
    os << oat_header.GetMagic() << "\n\n";

    os << "CHECKSUM:\n";
    os << StringPrintf("0x%08x\n\n", oat_header.GetChecksum());

    os << "INSTRUCTION SET:\n";
    os << oat_header.GetInstructionSet() << "\n\n";

    {
      std::unique_ptr<const InstructionSetFeatures> features(
          InstructionSetFeatures::FromBitmap(oat_header.GetInstructionSet(),
                                             oat_header.GetInstructionSetFeaturesBitmap()));
      os << "INSTRUCTION SET FEATURES:\n";
      os << features->GetFeatureString() << "\n\n";
    }

    os << "DEX FILE COUNT:\n";
    os << oat_header.GetDexFileCount() << "\n\n";

#define DUMP_OAT_HEADER_OFFSET(label, offset) \
    os << label " OFFSET:\n"; \
    os << StringPrintf("0x%08x", oat_header.offset()); \
    if (oat_header.offset() != 0 && options_.absolute_addresses_) { \
      os << StringPrintf(" (%p)", oat_file_.Begin() + oat_header.offset()); \
    } \
    os << StringPrintf("\n\n");

    DUMP_OAT_HEADER_OFFSET("EXECUTABLE", GetExecutableOffset);
    DUMP_OAT_HEADER_OFFSET("INTERPRETER TO INTERPRETER BRIDGE",
                           GetInterpreterToInterpreterBridgeOffset);
    DUMP_OAT_HEADER_OFFSET("INTERPRETER TO COMPILED CODE BRIDGE",
                           GetInterpreterToCompiledCodeBridgeOffset);
    DUMP_OAT_HEADER_OFFSET("JNI DLSYM LOOKUP",
                           GetJniDlsymLookupOffset);
    DUMP_OAT_HEADER_OFFSET("QUICK GENERIC JNI TRAMPOLINE",
                           GetQuickGenericJniTrampolineOffset);
    DUMP_OAT_HEADER_OFFSET("QUICK IMT CONFLICT TRAMPOLINE",
                           GetQuickImtConflictTrampolineOffset);
    DUMP_OAT_HEADER_OFFSET("QUICK RESOLUTION TRAMPOLINE",
                           GetQuickResolutionTrampolineOffset);
    DUMP_OAT_HEADER_OFFSET("QUICK TO INTERPRETER BRIDGE",
                           GetQuickToInterpreterBridgeOffset);
#undef DUMP_OAT_HEADER_OFFSET

    os << "IMAGE PATCH DELTA:\n";
    os << StringPrintf("%d (0x%08x)\n\n",
                       oat_header.GetImagePatchDelta(),
                       oat_header.GetImagePatchDelta());

    os << "IMAGE FILE LOCATION OAT CHECKSUM:\n";
    os << StringPrintf("0x%08x\n\n", oat_header.GetImageFileLocationOatChecksum());

    os << "IMAGE FILE LOCATION OAT BEGIN:\n";
    os << StringPrintf("0x%08x\n\n", oat_header.GetImageFileLocationOatDataBegin());

    // Print the key-value store.
    {
      os << "KEY VALUE STORE:\n";
      size_t index = 0;
      const char* key;
      const char* value;
      while (oat_header.GetStoreKeyValuePairByIndex(index, &key, &value)) {
        os << key << " = " << value << "\n";
        index++;
      }
      os << "\n";
    }

    if (options_.absolute_addresses_) {
      os << "BEGIN:\n";
      os << reinterpret_cast<const void*>(oat_file_.Begin()) << "\n\n";

      os << "END:\n";
      os << reinterpret_cast<const void*>(oat_file_.End()) << "\n\n";
    }

    os << "SIZE:\n";
    os << oat_file_.Size() << "\n\n";

    os << std::flush;

    // If set, adjust relative address to be searched
    if (options_.addr2instr_ != 0) {
      resolved_addr2instr_ = options_.addr2instr_ + oat_header.GetExecutableOffset();
      os << "SEARCH ADDRESS (executable offset + input):\n";
      os << StringPrintf("0x%08x\n\n", resolved_addr2instr_);
    }

    for (size_t i = 0; i < oat_dex_files_.size(); i++) {
      const OatFile::OatDexFile* oat_dex_file = oat_dex_files_[i];
      CHECK(oat_dex_file != nullptr);

      // If file export selected skip file analysis
      if (options_.export_dex_location_) {
        if (!ExportDexFile(os, *oat_dex_file)) {
          success = false;
        }
      } else {
        if (!DumpOatDexFile(os, *oat_dex_file)) {
          success = false;
        }
      }
    }
    os << std::flush;
    return success;
  }

  size_t ComputeSize(const void* oat_data) {
    if (reinterpret_cast<const uint8_t*>(oat_data) < oat_file_.Begin() ||
        reinterpret_cast<const uint8_t*>(oat_data) > oat_file_.End()) {
      return 0;  // Address not in oat file
    }
    uintptr_t begin_offset = reinterpret_cast<uintptr_t>(oat_data) -
                             reinterpret_cast<uintptr_t>(oat_file_.Begin());
    auto it = offsets_.upper_bound(begin_offset);
    CHECK(it != offsets_.end());
    uintptr_t end_offset = *it;
    return end_offset - begin_offset;
  }

  InstructionSet GetOatInstructionSet() {
    return oat_file_.GetOatHeader().GetInstructionSet();
  }

  const void* GetQuickOatCode(ArtMethod* m) SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
    for (size_t i = 0; i < oat_dex_files_.size(); i++) {
      const OatFile::OatDexFile* oat_dex_file = oat_dex_files_[i];
      CHECK(oat_dex_file != nullptr);
      std::string error_msg;
      std::unique_ptr<const DexFile> dex_file(oat_dex_file->OpenDexFile(&error_msg));
      if (dex_file.get() == nullptr) {
        LOG(WARNING) << "Failed to open dex file '" << oat_dex_file->GetDexFileLocation()
            << "': " << error_msg;
      } else {
        const char* descriptor = m->GetDeclaringClassDescriptor();
        const DexFile::ClassDef* class_def =
            dex_file->FindClassDef(descriptor, ComputeModifiedUtf8Hash(descriptor));
        if (class_def != nullptr) {
          uint16_t class_def_index = dex_file->GetIndexForClassDef(*class_def);
          const OatFile::OatClass oat_class = oat_dex_file->GetOatClass(class_def_index);
          size_t method_index = m->GetMethodIndex();
          return oat_class.GetOatMethod(method_index).GetQuickCode();
        }
      }
    }
    return nullptr;
  }

 private:
  void AddAllOffsets() {
    // We don't know the length of the code for each method, but we need to know where to stop
    // when disassembling. What we do know is that a region of code will be followed by some other
    // region, so if we keep a sorted sequence of the start of each region, we can infer the length
    // of a piece of code by using upper_bound to find the start of the next region.
    for (size_t i = 0; i < oat_dex_files_.size(); i++) {
      const OatFile::OatDexFile* oat_dex_file = oat_dex_files_[i];
      CHECK(oat_dex_file != nullptr);
      std::string error_msg;
      std::unique_ptr<const DexFile> dex_file(oat_dex_file->OpenDexFile(&error_msg));
      if (dex_file.get() == nullptr) {
        LOG(WARNING) << "Failed to open dex file '" << oat_dex_file->GetDexFileLocation()
            << "': " << error_msg;
        continue;
      }
      offsets_.insert(reinterpret_cast<uintptr_t>(&dex_file->GetHeader()));
      for (size_t class_def_index = 0;
           class_def_index < dex_file->NumClassDefs();
           class_def_index++) {
        const DexFile::ClassDef& class_def = dex_file->GetClassDef(class_def_index);
        const OatFile::OatClass oat_class = oat_dex_file->GetOatClass(class_def_index);
        const uint8_t* class_data = dex_file->GetClassData(class_def);
        if (class_data != nullptr) {
          ClassDataItemIterator it(*dex_file, class_data);
          SkipAllFields(it);
          uint32_t class_method_index = 0;
          while (it.HasNextDirectMethod()) {
            AddOffsets(oat_class.GetOatMethod(class_method_index++));
            it.Next();
          }
          while (it.HasNextVirtualMethod()) {
            AddOffsets(oat_class.GetOatMethod(class_method_index++));
            it.Next();
          }
        }
      }
    }

    // If the last thing in the file is code for a method, there won't be an offset for the "next"
    // thing. Instead of having a special case in the upper_bound code, let's just add an entry
    // for the end of the file.
    offsets_.insert(oat_file_.Size());
  }

  static uint32_t AlignCodeOffset(uint32_t maybe_thumb_offset) {
    return maybe_thumb_offset & ~0x1;  // TODO: Make this Thumb2 specific.
  }

  void AddOffsets(const OatFile::OatMethod& oat_method) {
    uint32_t code_offset = oat_method.GetCodeOffset();
    if (oat_file_.GetOatHeader().GetInstructionSet() == kThumb2) {
      code_offset &= ~0x1;
    }
    offsets_.insert(code_offset);
    offsets_.insert(oat_method.GetMappingTableOffset());
    offsets_.insert(oat_method.GetVmapTableOffset());
    offsets_.insert(oat_method.GetGcMapOffset());
  }

  bool DumpOatDexFile(std::ostream& os, const OatFile::OatDexFile& oat_dex_file) {
    bool success = true;
    bool stop_analysis = false;
    os << "OatDexFile:\n";
    os << StringPrintf("location: %s\n", oat_dex_file.GetDexFileLocation().c_str());
    os << StringPrintf("checksum: 0x%08x\n", oat_dex_file.GetDexFileLocationChecksum());

    // Create the verifier early.

    std::string error_msg;
    std::unique_ptr<const DexFile> dex_file(oat_dex_file.OpenDexFile(&error_msg));
    if (dex_file.get() == nullptr) {
      os << "NOT FOUND: " << error_msg << "\n\n";
      os << std::flush;
      return false;
    }
    for (size_t class_def_index = 0;
         class_def_index < dex_file->NumClassDefs();
         class_def_index++) {
      const DexFile::ClassDef& class_def = dex_file->GetClassDef(class_def_index);
      const char* descriptor = dex_file->GetClassDescriptor(class_def);

      // TODO: Support regex
      if (DescriptorToDot(descriptor).find(options_.class_filter_) == std::string::npos) {
        continue;
      }

      uint32_t oat_class_offset = oat_dex_file.GetOatClassOffset(class_def_index);
      const OatFile::OatClass oat_class = oat_dex_file.GetOatClass(class_def_index);
      os << StringPrintf("%zd: %s (offset=0x%08x) (type_idx=%d)",
                         class_def_index, descriptor, oat_class_offset, class_def.class_idx_)
         << " (" << oat_class.GetStatus() << ")"
         << " (" << oat_class.GetType() << ")\n";
      // TODO: include bitmap here if type is kOatClassSomeCompiled?
      Indenter indent_filter(os.rdbuf(), kIndentChar, kIndentBy1Count);
      std::ostream indented_os(&indent_filter);
      if (options_.list_classes_) continue;
      if (!DumpOatClass(indented_os, oat_class, *(dex_file.get()), class_def, &stop_analysis)) {
        success = false;
      }
      if (stop_analysis) {
        os << std::flush;
        return success;
      }
    }

    os << std::flush;
    return success;
  }

  bool ExportDexFile(std::ostream& os, const OatFile::OatDexFile& oat_dex_file) {
    std::string error_msg;
    std::string dex_file_location = oat_dex_file.GetDexFileLocation();

    std::unique_ptr<const DexFile> dex_file(oat_dex_file.OpenDexFile(&error_msg));
    if (dex_file == nullptr) {
      os << "Failed to open dex file '" << dex_file_location << "': " << error_msg;
      return false;
    }
    size_t fsize = oat_dex_file.FileSize();

    // Some quick checks just in case
    if (fsize == 0 || fsize < sizeof(DexFile::Header)) {
      os << "Invalid dex file\n";
      return false;
    }

    // Verify output directory exists
    if (!OS::DirectoryExists(options_.export_dex_location_)) {
      // TODO: Extend OS::DirectoryExists if symlink support is required
      os << options_.export_dex_location_ << " output directory not found or symlink\n";
      return false;
    }

    // Beautify path names
    if (dex_file_location.size() > PATH_MAX || dex_file_location.size() <= 0) {
      return false;
    }

    std::string dex_orig_name;
    size_t dex_orig_pos = dex_file_location.rfind('/');
    if (dex_orig_pos == std::string::npos)
      dex_orig_name = dex_file_location;
    else
      dex_orig_name = dex_file_location.substr(dex_orig_pos + 1);

    // A more elegant approach to efficiently name user installed apps is welcome
    if (dex_orig_name.size() == 8 && !dex_orig_name.compare("base.apk")) {
      dex_file_location.erase(dex_orig_pos, strlen("base.apk") + 1);
      size_t apk_orig_pos = dex_file_location.rfind('/');
      if (apk_orig_pos != std::string::npos) {
        dex_orig_name = dex_file_location.substr(++apk_orig_pos);
      }
    }

    std::string out_dex_path(options_.export_dex_location_);
    if (out_dex_path.back() != '/') {
      out_dex_path.append("/");
    }
    out_dex_path.append(dex_orig_name);
    out_dex_path.append("_export.dex");
    if (out_dex_path.length() > PATH_MAX) {
      return false;
    }

    std::unique_ptr<File> file(OS::CreateEmptyFile(out_dex_path.c_str()));
    if (file.get() == nullptr) {
      os << "Failed to open output dex file " << out_dex_path;
      return false;
    }

    if (!file->WriteFully(dex_file->Begin(), fsize)) {
      os << "Failed to write dex file";
      file->Erase();
      return false;
    }

    if (file->FlushCloseOrErase() != 0) {
      os << "Flush and close failed";
      return false;
    }

    os << StringPrintf("Dex file exported at %s (%zd bytes)\n", out_dex_path.c_str(), fsize);
    os << std::flush;

    return true;
  }

  static void SkipAllFields(ClassDataItemIterator& it) {
    while (it.HasNextStaticField()) {
      it.Next();
    }
    while (it.HasNextInstanceField()) {
      it.Next();
    }
  }

  bool DumpOatClass(std::ostream& os, const OatFile::OatClass& oat_class, const DexFile& dex_file,
                    const DexFile::ClassDef& class_def, bool* stop_analysis) {
    bool success = true;
    bool addr_found = false;
    const uint8_t* class_data = dex_file.GetClassData(class_def);
    if (class_data == nullptr) {  // empty class such as a marker interface?
      os << std::flush;
      return success;
    }
    ClassDataItemIterator it(dex_file, class_data);
    SkipAllFields(it);
    uint32_t class_method_index = 0;
    while (it.HasNextDirectMethod()) {
      if (!DumpOatMethod(os, class_def, class_method_index, oat_class, dex_file,
                         it.GetMemberIndex(), it.GetMethodCodeItem(),
                         it.GetRawMemberAccessFlags(), &addr_found)) {
        success = false;
      }
      if (addr_found) {
        *stop_analysis = true;
        return success;
      }
      class_method_index++;
      it.Next();
    }
    while (it.HasNextVirtualMethod()) {
      if (!DumpOatMethod(os, class_def, class_method_index, oat_class, dex_file,
                         it.GetMemberIndex(), it.GetMethodCodeItem(),
                         it.GetRawMemberAccessFlags(), &addr_found)) {
        success = false;
      }
      if (addr_found) {
        *stop_analysis = true;
        return success;
      }
      class_method_index++;
      it.Next();
    }
    DCHECK(!it.HasNext());
    os << std::flush;
    return success;
  }

  static constexpr uint32_t kPrologueBytes = 16;

  // When this was picked, the largest arm method was 55,256 bytes and arm64 was 50,412 bytes.
  static constexpr uint32_t kMaxCodeSize = 100 * 1000;

  bool DumpOatMethod(std::ostream& os, const DexFile::ClassDef& class_def,
                     uint32_t class_method_index,
                     const OatFile::OatClass& oat_class, const DexFile& dex_file,
                     uint32_t dex_method_idx, const DexFile::CodeItem* code_item,
                     uint32_t method_access_flags, bool* addr_found) {
    bool success = true;

    // TODO: Support regex
    std::string method_name = dex_file.GetMethodName(dex_file.GetMethodId(dex_method_idx));
    if (method_name.find(options_.method_filter_) == std::string::npos) {
      return success;
    }

    std::string pretty_method = PrettyMethod(dex_method_idx, dex_file, true);
    os << StringPrintf("%d: %s (dex_method_idx=%d)\n",
                       class_method_index, pretty_method.c_str(),
                       dex_method_idx);
    if (options_.list_methods_) return success;

    Indenter indent1_filter(os.rdbuf(), kIndentChar, kIndentBy1Count);
    std::unique_ptr<std::ostream> indent1_os(new std::ostream(&indent1_filter));
    Indenter indent2_filter(indent1_os->rdbuf(), kIndentChar, kIndentBy1Count);
    std::unique_ptr<std::ostream> indent2_os(new std::ostream(&indent2_filter));

    uint32_t oat_method_offsets_offset = oat_class.GetOatMethodOffsetsOffset(class_method_index);
    const OatMethodOffsets* oat_method_offsets = oat_class.GetOatMethodOffsets(class_method_index);
    const OatFile::OatMethod oat_method = oat_class.GetOatMethod(class_method_index);
    uint32_t code_offset = oat_method.GetCodeOffset();
    uint32_t code_size = oat_method.GetQuickCodeSize();
    if (resolved_addr2instr_ != 0) {
      if (resolved_addr2instr_ > code_offset + code_size) {
        return success;
      } else {
        *addr_found = true;  // stop analyzing file at next iteration
      }
    }

    {
      *indent1_os << "DEX CODE:\n";
      DumpDexCode(*indent2_os, dex_file, code_item);
    }

    std::unique_ptr<verifier::MethodVerifier> verifier;
    if (Runtime::Current() != nullptr) {
      *indent1_os << "VERIFIER TYPE ANALYSIS:\n";
      verifier.reset(DumpVerifier(*indent2_os, dex_method_idx, &dex_file, class_def, code_item,
                                  method_access_flags));
    }
    {
      *indent1_os << "OatMethodOffsets ";
      if (options_.absolute_addresses_) {
        *indent1_os << StringPrintf("%p ", oat_method_offsets);
      }
      *indent1_os << StringPrintf("(offset=0x%08x)\n", oat_method_offsets_offset);
      if (oat_method_offsets_offset > oat_file_.Size()) {
        *indent1_os << StringPrintf(
            "WARNING: oat method offsets offset 0x%08x is past end of file 0x%08zx.\n",
            oat_method_offsets_offset, oat_file_.Size());
        // If we can't read OatMethodOffsets, the rest of the data is dangerous to read.
        os << std::flush;
        return false;
      }

      *indent2_os << StringPrintf("code_offset: 0x%08x ", code_offset);
      uint32_t aligned_code_begin = AlignCodeOffset(oat_method.GetCodeOffset());
      if (aligned_code_begin > oat_file_.Size()) {
        *indent2_os << StringPrintf("WARNING: "
                                    "code offset 0x%08x is past end of file 0x%08zx.\n",
                                    aligned_code_begin, oat_file_.Size());
        success = false;
      }
      *indent2_os << "\n";

      *indent2_os << "gc_map: ";
      if (options_.absolute_addresses_) {
        *indent2_os << StringPrintf("%p ", oat_method.GetGcMap());
      }
      uint32_t gc_map_offset = oat_method.GetGcMapOffset();
      *indent2_os << StringPrintf("(offset=0x%08x)\n", gc_map_offset);
      if (gc_map_offset > oat_file_.Size()) {
        *indent2_os << StringPrintf("WARNING: "
                                    "gc map table offset 0x%08x is past end of file 0x%08zx.\n",
                                    gc_map_offset, oat_file_.Size());
        success = false;
      } else if (options_.dump_raw_gc_map_) {
        Indenter indent3_filter(indent2_os->rdbuf(), kIndentChar, kIndentBy1Count);
        std::ostream indent3_os(&indent3_filter);
        DumpGcMap(indent3_os, oat_method, code_item);
      }
    }
    {
      *indent1_os << "OatQuickMethodHeader ";
      uint32_t method_header_offset = oat_method.GetOatQuickMethodHeaderOffset();
      const OatQuickMethodHeader* method_header = oat_method.GetOatQuickMethodHeader();

      if (options_.absolute_addresses_) {
        *indent1_os << StringPrintf("%p ", method_header);
      }
      *indent1_os << StringPrintf("(offset=0x%08x)\n", method_header_offset);
      if (method_header_offset > oat_file_.Size()) {
        *indent1_os << StringPrintf(
            "WARNING: oat quick method header offset 0x%08x is past end of file 0x%08zx.\n",
            method_header_offset, oat_file_.Size());
        // If we can't read the OatQuickMethodHeader, the rest of the data is dangerous to read.
        os << std::flush;
        return false;
      }

      *indent2_os << "mapping_table: ";
      if (options_.absolute_addresses_) {
        *indent2_os << StringPrintf("%p ", oat_method.GetMappingTable());
      }
      uint32_t mapping_table_offset = oat_method.GetMappingTableOffset();
      *indent2_os << StringPrintf("(offset=0x%08x)\n", oat_method.GetMappingTableOffset());
      if (mapping_table_offset > oat_file_.Size()) {
        *indent2_os << StringPrintf("WARNING: "
                                    "mapping table offset 0x%08x is past end of file 0x%08zx. "
                                    "mapping table offset was loaded from offset 0x%08x.\n",
                                    mapping_table_offset, oat_file_.Size(),
                                    oat_method.GetMappingTableOffsetOffset());
        success = false;
      } else if (options_.dump_raw_mapping_table_) {
        Indenter indent3_filter(indent2_os->rdbuf(), kIndentChar, kIndentBy1Count);
        std::ostream indent3_os(&indent3_filter);
        DumpMappingTable(indent3_os, oat_method);
      }

      *indent2_os << "vmap_table: ";
      if (options_.absolute_addresses_) {
        *indent2_os << StringPrintf("%p ", oat_method.GetVmapTable());
      }
      uint32_t vmap_table_offset = oat_method.GetVmapTableOffset();
      *indent2_os << StringPrintf("(offset=0x%08x)\n", vmap_table_offset);
      if (vmap_table_offset > oat_file_.Size()) {
        *indent2_os << StringPrintf("WARNING: "
                                    "vmap table offset 0x%08x is past end of file 0x%08zx. "
                                    "vmap table offset was loaded from offset 0x%08x.\n",
                                    vmap_table_offset, oat_file_.Size(),
                                    oat_method.GetVmapTableOffsetOffset());
        success = false;
      } else if (options_.dump_vmap_) {
        DumpVmapData(*indent2_os, oat_method, code_item);
      }
    }
    {
      *indent1_os << "QuickMethodFrameInfo\n";

      *indent2_os << StringPrintf("frame_size_in_bytes: %zd\n", oat_method.GetFrameSizeInBytes());
      *indent2_os << StringPrintf("core_spill_mask: 0x%08x ", oat_method.GetCoreSpillMask());
      DumpSpillMask(*indent2_os, oat_method.GetCoreSpillMask(), false);
      *indent2_os << "\n";
      *indent2_os << StringPrintf("fp_spill_mask: 0x%08x ", oat_method.GetFpSpillMask());
      DumpSpillMask(*indent2_os, oat_method.GetFpSpillMask(), true);
      *indent2_os << "\n";
    }
    {
        // Based on spill masks from QuickMethodFrameInfo so placed
        // after it is dumped, but useful for understanding quick
        // code, so dumped here.
        DumpVregLocations(*indent2_os, oat_method, code_item);
    }
    {
      *indent1_os << "CODE: ";
      uint32_t code_size_offset = oat_method.GetQuickCodeSizeOffset();
      if (code_size_offset > oat_file_.Size()) {
        *indent2_os << StringPrintf("WARNING: "
                                    "code size offset 0x%08x is past end of file 0x%08zx.",
                                    code_size_offset, oat_file_.Size());
        success = false;
      } else {
        const void* code = oat_method.GetQuickCode();
        uint32_t aligned_code_begin = AlignCodeOffset(code_offset);
        uint64_t aligned_code_end = aligned_code_begin + code_size;

        if (options_.absolute_addresses_) {
          *indent1_os << StringPrintf("%p ", code);
        }
        *indent1_os << StringPrintf("(code_offset=0x%08x size_offset=0x%08x size=%u)%s\n",
                                    code_offset,
                                    code_size_offset,
                                    code_size,
                                    code != nullptr ? "..." : "");

        if (aligned_code_begin > oat_file_.Size()) {
          *indent2_os << StringPrintf("WARNING: "
                                      "start of code at 0x%08x is past end of file 0x%08zx.",
                                      aligned_code_begin, oat_file_.Size());
          success = false;
        } else if (aligned_code_end > oat_file_.Size()) {
          *indent2_os << StringPrintf("WARNING: "
                                      "end of code at 0x%08" PRIx64 " is past end of file 0x%08zx. "
                                      "code size is 0x%08x loaded from offset 0x%08x.\n",
                                      aligned_code_end, oat_file_.Size(),
                                      code_size, code_size_offset);
          success = false;
          if (options_.disassemble_code_) {
            if (code_size_offset + kPrologueBytes <= oat_file_.Size()) {
              DumpCode(*indent2_os, verifier.get(), oat_method, code_item, true, kPrologueBytes);
            }
          }
        } else if (code_size > kMaxCodeSize) {
          *indent2_os << StringPrintf("WARNING: "
                                      "code size %d is bigger than max expected threshold of %d. "
                                      "code size is 0x%08x loaded from offset 0x%08x.\n",
                                      code_size, kMaxCodeSize,
                                      code_size, code_size_offset);
          success = false;
          if (options_.disassemble_code_) {
            if (code_size_offset + kPrologueBytes <= oat_file_.Size()) {
              DumpCode(*indent2_os, verifier.get(), oat_method, code_item, true, kPrologueBytes);
            }
          }
        } else if (options_.disassemble_code_) {
          DumpCode(*indent2_os, verifier.get(), oat_method, code_item, !success, 0);
        }
      }
    }
    os << std::flush;
    return success;
  }

  void DumpSpillMask(std::ostream& os, uint32_t spill_mask, bool is_float) {
    if (spill_mask == 0) {
      return;
    }
    os << "(";
    for (size_t i = 0; i < 32; i++) {
      if ((spill_mask & (1 << i)) != 0) {
        if (is_float) {
          os << "fr" << i;
        } else {
          os << "r" << i;
        }
        spill_mask ^= 1 << i;  // clear bit
        if (spill_mask != 0) {
          os << ", ";
        } else {
          break;
        }
      }
    }
    os << ")";
  }

  // Display data stored at the the vmap offset of an oat method.
  void DumpVmapData(std::ostream& os,
                    const OatFile::OatMethod& oat_method,
                    const DexFile::CodeItem* code_item) {
    if (oat_method.GetGcMap() == nullptr) {
      // If the native GC map is null, then this method has been
      // compiled with the optimizing compiler. The optimizing
      // compiler currently outputs its stack maps in the vmap table.
      const void* raw_code_info = oat_method.GetVmapTable();
      if (raw_code_info != nullptr) {
        CodeInfo code_info(raw_code_info);
        DCHECK(code_item != nullptr);
        DumpCodeInfo(os, code_info, *code_item);
      }
    } else {
      // Otherwise, display the vmap table.
      const uint8_t* raw_table = oat_method.GetVmapTable();
      if (raw_table != nullptr) {
        VmapTable vmap_table(raw_table);
        DumpVmapTable(os, oat_method, vmap_table);
      }
    }
  }

  // Display a CodeInfo object emitted by the optimizing compiler.
  void DumpCodeInfo(std::ostream& os,
                    const CodeInfo& code_info,
                    const DexFile::CodeItem& code_item) {
    code_info.Dump(os, code_item.registers_size_);
  }

  // Display a vmap table.
  void DumpVmapTable(std::ostream& os,
                     const OatFile::OatMethod& oat_method,
                     const VmapTable& vmap_table) {
    bool first = true;
    bool processing_fp = false;
    uint32_t spill_mask = oat_method.GetCoreSpillMask();
    for (size_t i = 0; i < vmap_table.Size(); i++) {
      uint16_t dex_reg = vmap_table[i];
      uint32_t cpu_reg = vmap_table.ComputeRegister(spill_mask, i,
                                                    processing_fp ? kFloatVReg : kIntVReg);
      os << (first ? "v" : ", v")  << dex_reg;
      if (!processing_fp) {
        os << "/r" << cpu_reg;
      } else {
        os << "/fr" << cpu_reg;
      }
      first = false;
      if (!processing_fp && dex_reg == 0xFFFF) {
        processing_fp = true;
        spill_mask = oat_method.GetFpSpillMask();
      }
    }
    os << "\n";
  }

  void DumpVregLocations(std::ostream& os, const OatFile::OatMethod& oat_method,
                         const DexFile::CodeItem* code_item) {
    if (code_item != nullptr) {
      size_t num_locals_ins = code_item->registers_size_;
      size_t num_ins = code_item->ins_size_;
      size_t num_locals = num_locals_ins - num_ins;
      size_t num_outs = code_item->outs_size_;

      os << "vr_stack_locations:";
      for (size_t reg = 0; reg <= num_locals_ins; reg++) {
        // For readability, delimit the different kinds of VRs.
        if (reg == num_locals_ins) {
          os << "\n\tmethod*:";
        } else if (reg == num_locals && num_ins > 0) {
          os << "\n\tins:";
        } else if (reg == 0 && num_locals > 0) {
          os << "\n\tlocals:";
        }

        uint32_t offset = StackVisitor::GetVRegOffsetFromQuickCode(
            code_item,
            oat_method.GetCoreSpillMask(),
            oat_method.GetFpSpillMask(),
            oat_method.GetFrameSizeInBytes(),
            reg,
            GetInstructionSet());
        os << " v" << reg << "[sp + #" << offset << "]";
      }

      for (size_t out_reg = 0; out_reg < num_outs; out_reg++) {
        if (out_reg == 0) {
          os << "\n\touts:";
        }

        uint32_t offset = StackVisitor::GetOutVROffset(out_reg, GetInstructionSet());
        os << " v" << out_reg << "[sp + #" << offset << "]";
      }

      os << "\n";
    }
  }

  void DescribeVReg(std::ostream& os, const OatFile::OatMethod& oat_method,
                    const DexFile::CodeItem* code_item, size_t reg, VRegKind kind) {
    const uint8_t* raw_table = oat_method.GetVmapTable();
    if (raw_table != nullptr) {
      const VmapTable vmap_table(raw_table);
      uint32_t vmap_offset;
      if (vmap_table.IsInContext(reg, kind, &vmap_offset)) {
        bool is_float = (kind == kFloatVReg) || (kind == kDoubleLoVReg) || (kind == kDoubleHiVReg);
        uint32_t spill_mask = is_float ? oat_method.GetFpSpillMask()
                                       : oat_method.GetCoreSpillMask();
        os << (is_float ? "fr" : "r") << vmap_table.ComputeRegister(spill_mask, vmap_offset, kind);
      } else {
        uint32_t offset = StackVisitor::GetVRegOffsetFromQuickCode(
            code_item,
            oat_method.GetCoreSpillMask(),
            oat_method.GetFpSpillMask(),
            oat_method.GetFrameSizeInBytes(),
            reg,
            GetInstructionSet());
        os << "[sp + #" << offset << "]";
      }
    }
  }

  void DumpGcMapRegisters(std::ostream& os, const OatFile::OatMethod& oat_method,
                          const DexFile::CodeItem* code_item,
                          size_t num_regs, const uint8_t* reg_bitmap) {
    bool first = true;
    for (size_t reg = 0; reg < num_regs; reg++) {
      if (((reg_bitmap[reg / 8] >> (reg % 8)) & 0x01) != 0) {
        if (first) {
          os << "  v" << reg << " (";
          DescribeVReg(os, oat_method, code_item, reg, kReferenceVReg);
          os << ")";
          first = false;
        } else {
          os << ", v" << reg << " (";
          DescribeVReg(os, oat_method, code_item, reg, kReferenceVReg);
          os << ")";
        }
      }
    }
    if (first) {
      os << "No registers in GC map\n";
    } else {
      os << "\n";
    }
  }
  void DumpGcMap(std::ostream& os, const OatFile::OatMethod& oat_method,
                 const DexFile::CodeItem* code_item) {
    const uint8_t* gc_map_raw = oat_method.GetGcMap();
    if (gc_map_raw == nullptr) {
      return;  // No GC map.
    }
    const void* quick_code = oat_method.GetQuickCode();
    NativePcOffsetToReferenceMap map(gc_map_raw);
    for (size_t entry = 0; entry < map.NumEntries(); entry++) {
      const uint8_t* native_pc = reinterpret_cast<const uint8_t*>(quick_code) +
          map.GetNativePcOffset(entry);
      os << StringPrintf("%p", native_pc);
      DumpGcMapRegisters(os, oat_method, code_item, map.RegWidth() * 8, map.GetBitMap(entry));
    }
  }

  void DumpMappingTable(std::ostream& os, const OatFile::OatMethod& oat_method) {
    const void* quick_code = oat_method.GetQuickCode();
    if (quick_code == nullptr) {
      return;
    }
    MappingTable table(oat_method.GetMappingTable());
    if (table.TotalSize() != 0) {
      Indenter indent_filter(os.rdbuf(), kIndentChar, kIndentBy1Count);
      std::ostream indent_os(&indent_filter);
      if (table.PcToDexSize() != 0) {
        typedef MappingTable::PcToDexIterator It;
        os << "suspend point mappings {\n";
        for (It cur = table.PcToDexBegin(), end = table.PcToDexEnd(); cur != end; ++cur) {
          indent_os << StringPrintf("0x%04x -> 0x%04x\n", cur.NativePcOffset(), cur.DexPc());
        }
        os << "}\n";
      }
      if (table.DexToPcSize() != 0) {
        typedef MappingTable::DexToPcIterator It;
        os << "catch entry mappings {\n";
        for (It cur = table.DexToPcBegin(), end = table.DexToPcEnd(); cur != end; ++cur) {
          indent_os << StringPrintf("0x%04x -> 0x%04x\n", cur.NativePcOffset(), cur.DexPc());
        }
        os << "}\n";
      }
    }
  }

  uint32_t DumpMappingAtOffset(std::ostream& os, const OatFile::OatMethod& oat_method,
                               size_t offset, bool suspend_point_mapping) {
    MappingTable table(oat_method.GetMappingTable());
    if (suspend_point_mapping && table.PcToDexSize() > 0) {
      typedef MappingTable::PcToDexIterator It;
      for (It cur = table.PcToDexBegin(), end = table.PcToDexEnd(); cur != end; ++cur) {
        if (offset == cur.NativePcOffset()) {
          os << StringPrintf("suspend point dex PC: 0x%04x\n", cur.DexPc());
          return cur.DexPc();
        }
      }
    } else if (!suspend_point_mapping && table.DexToPcSize() > 0) {
      typedef MappingTable::DexToPcIterator It;
      for (It cur = table.DexToPcBegin(), end = table.DexToPcEnd(); cur != end; ++cur) {
        if (offset == cur.NativePcOffset()) {
          os << StringPrintf("catch entry dex PC: 0x%04x\n", cur.DexPc());
          return cur.DexPc();
        }
      }
    }
    return DexFile::kDexNoIndex;
  }

  void DumpGcMapAtNativePcOffset(std::ostream& os, const OatFile::OatMethod& oat_method,
                                 const DexFile::CodeItem* code_item, size_t native_pc_offset) {
    const uint8_t* gc_map_raw = oat_method.GetGcMap();
    if (gc_map_raw != nullptr) {
      NativePcOffsetToReferenceMap map(gc_map_raw);
      if (map.HasEntry(native_pc_offset)) {
        size_t num_regs = map.RegWidth() * 8;
        const uint8_t* reg_bitmap = map.FindBitMap(native_pc_offset);
        bool first = true;
        for (size_t reg = 0; reg < num_regs; reg++) {
          if (((reg_bitmap[reg / 8] >> (reg % 8)) & 0x01) != 0) {
            if (first) {
              os << "GC map objects:  v" << reg << " (";
              DescribeVReg(os, oat_method, code_item, reg, kReferenceVReg);
              os << ")";
              first = false;
            } else {
              os << ", v" << reg << " (";
              DescribeVReg(os, oat_method, code_item, reg, kReferenceVReg);
              os << ")";
            }
          }
        }
        if (!first) {
          os << "\n";
        }
      }
    }
  }

  void DumpVRegsAtDexPc(std::ostream& os, verifier::MethodVerifier* verifier,
                        const OatFile::OatMethod& oat_method,
                        const DexFile::CodeItem* code_item, uint32_t dex_pc) {
    DCHECK(verifier != nullptr);
    std::vector<int32_t> kinds = verifier->DescribeVRegs(dex_pc);
    bool first = true;
    for (size_t reg = 0; reg < code_item->registers_size_; reg++) {
      VRegKind kind = static_cast<VRegKind>(kinds.at(reg * 2));
      if (kind != kUndefined) {
        if (first) {
          os << "VRegs:  v";
          first = false;
        } else {
          os << ", v";
        }
        os << reg << " (";
        switch (kind) {
          case kImpreciseConstant:
            os << "Imprecise Constant: " << kinds.at((reg * 2) + 1) << ", ";
            DescribeVReg(os, oat_method, code_item, reg, kind);
            break;
          case kConstant:
            os << "Constant: " << kinds.at((reg * 2) + 1);
            break;
          default:
            DescribeVReg(os, oat_method, code_item, reg, kind);
            break;
        }
        os << ")";
      }
    }
    if (!first) {
      os << "\n";
    }
  }


  void DumpDexCode(std::ostream& os, const DexFile& dex_file, const DexFile::CodeItem* code_item) {
    if (code_item != nullptr) {
      size_t i = 0;
      while (i < code_item->insns_size_in_code_units_) {
        const Instruction* instruction = Instruction::At(&code_item->insns_[i]);
        os << StringPrintf("0x%04zx: ", i) << instruction->DumpHexLE(5)
           << StringPrintf("\t| %s\n", instruction->DumpString(&dex_file).c_str());
        i += instruction->SizeInCodeUnits();
      }
    }
  }

  verifier::MethodVerifier* DumpVerifier(std::ostream& os, uint32_t dex_method_idx,
                                         const DexFile* dex_file,
                                         const DexFile::ClassDef& class_def,
                                         const DexFile::CodeItem* code_item,
                                         uint32_t method_access_flags) {
    if ((method_access_flags & kAccNative) == 0) {
      ScopedObjectAccess soa(Thread::Current());
      StackHandleScope<1> hs(soa.Self());
      Handle<mirror::DexCache> dex_cache(
          hs.NewHandle(Runtime::Current()->GetClassLinker()->FindDexCache(*dex_file)));
      DCHECK(options_.class_loader_ != nullptr);
      return verifier::MethodVerifier::VerifyMethodAndDump(
          soa.Self(), os, dex_method_idx, dex_file, dex_cache, *options_.class_loader_, &class_def,
          code_item, nullptr, method_access_flags);
    }

    return nullptr;
  }

  void DumpCode(std::ostream& os, verifier::MethodVerifier* verifier,
                const OatFile::OatMethod& oat_method, const DexFile::CodeItem* code_item,
                bool bad_input, size_t code_size) {
    const void* quick_code = oat_method.GetQuickCode();

    if (code_size == 0) {
      code_size = oat_method.GetQuickCodeSize();
    }
    if (code_size == 0 || quick_code == nullptr) {
      os << "NO CODE!\n";
      return;
    } else {
      const uint8_t* quick_native_pc = reinterpret_cast<const uint8_t*>(quick_code);
      size_t offset = 0;
      while (offset < code_size) {
        if (!bad_input) {
          DumpMappingAtOffset(os, oat_method, offset, false);
        }
        offset += disassembler_->Dump(os, quick_native_pc + offset);
        if (!bad_input) {
          uint32_t dex_pc = DumpMappingAtOffset(os, oat_method, offset, true);
          if (dex_pc != DexFile::kDexNoIndex) {
            DumpGcMapAtNativePcOffset(os, oat_method, code_item, offset);
            if (verifier != nullptr) {
              DumpVRegsAtDexPc(os, verifier, oat_method, code_item, dex_pc);
            }
          }
        }
      }
    }
  }

  const OatFile& oat_file_;
  const std::vector<const OatFile::OatDexFile*> oat_dex_files_;
  const OatDumperOptions& options_;
  uint32_t resolved_addr2instr_;
  InstructionSet instruction_set_;
  std::set<uintptr_t> offsets_;
  Disassembler* disassembler_;
};

class ImageDumper {
 public:
  explicit ImageDumper(std::ostream* os, gc::space::ImageSpace& image_space,
                       const ImageHeader& image_header, OatDumperOptions* oat_dumper_options)
      : os_(os),
        image_space_(image_space),
        image_header_(image_header),
        oat_dumper_options_(oat_dumper_options) {}

  bool Dump() SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
    std::ostream& os = *os_;
    os << "MAGIC: " << image_header_.GetMagic() << "\n\n";

    os << "IMAGE BEGIN: " << reinterpret_cast<void*>(image_header_.GetImageBegin()) << "\n\n";

    os << "IMAGE SIZE: " << image_header_.GetImageSize() << "\n\n";

    for (size_t i = 0; i < ImageHeader::kSectionCount; ++i) {
      auto section = static_cast<ImageHeader::ImageSections>(i);
      os << "IMAGE SECTION " << section << ": " << image_header_.GetImageSection(section) << "\n\n";
    }

    os << "OAT CHECKSUM: " << StringPrintf("0x%08x\n\n", image_header_.GetOatChecksum());

    os << "OAT FILE BEGIN:" << reinterpret_cast<void*>(image_header_.GetOatFileBegin()) << "\n\n";

    os << "OAT DATA BEGIN:" << reinterpret_cast<void*>(image_header_.GetOatDataBegin()) << "\n\n";

    os << "OAT DATA END:" << reinterpret_cast<void*>(image_header_.GetOatDataEnd()) << "\n\n";

    os << "OAT FILE END:" << reinterpret_cast<void*>(image_header_.GetOatFileEnd()) << "\n\n";

    os << "PATCH DELTA:" << image_header_.GetPatchDelta() << "\n\n";

    os << "COMPILE PIC: " << (image_header_.CompilePic() ? "yes" : "no") << "\n\n";

    {
      os << "ROOTS: " << reinterpret_cast<void*>(image_header_.GetImageRoots()) << "\n";
      Indenter indent1_filter(os.rdbuf(), kIndentChar, kIndentBy1Count);
      std::ostream indent1_os(&indent1_filter);
      static_assert(arraysize(image_roots_descriptions_) ==
          static_cast<size_t>(ImageHeader::kImageRootsMax), "sizes must match");
      for (int i = 0; i < ImageHeader::kImageRootsMax; i++) {
        ImageHeader::ImageRoot image_root = static_cast<ImageHeader::ImageRoot>(i);
        const char* image_root_description = image_roots_descriptions_[i];
        mirror::Object* image_root_object = image_header_.GetImageRoot(image_root);
        indent1_os << StringPrintf("%s: %p\n", image_root_description, image_root_object);
        if (image_root_object->IsObjectArray()) {
          Indenter indent2_filter(indent1_os.rdbuf(), kIndentChar, kIndentBy1Count);
          std::ostream indent2_os(&indent2_filter);
          mirror::ObjectArray<mirror::Object>* image_root_object_array
              = image_root_object->AsObjectArray<mirror::Object>();
          for (int j = 0; j < image_root_object_array->GetLength(); j++) {
            mirror::Object* value = image_root_object_array->Get(j);
            size_t run = 0;
            for (int32_t k = j + 1; k < image_root_object_array->GetLength(); k++) {
              if (value == image_root_object_array->Get(k)) {
                run++;
              } else {
                break;
              }
            }
            if (run == 0) {
              indent2_os << StringPrintf("%d: ", j);
            } else {
              indent2_os << StringPrintf("%d to %zd: ", j, j + run);
              j = j + run;
            }
            if (value != nullptr) {
              PrettyObjectValue(indent2_os, value->GetClass(), value);
            } else {
              indent2_os << j << ": null\n";
            }
          }
        }
      }

      os << "METHOD ROOTS\n";
      static_assert(arraysize(image_methods_descriptions_) ==
          static_cast<size_t>(ImageHeader::kImageMethodsCount), "sizes must match");
      for (int i = 0; i < ImageHeader::kImageMethodsCount; i++) {
        auto image_root = static_cast<ImageHeader::ImageMethod>(i);
        const char* description = image_methods_descriptions_[i];
        auto* image_method = image_header_.GetImageMethod(image_root);
        indent1_os << StringPrintf("%s: %p\n", description, image_method);
      }
    }
    os << "\n";

    ClassLinker* class_linker = Runtime::Current()->GetClassLinker();
    std::string image_filename = image_space_.GetImageFilename();
    std::string oat_location = ImageHeader::GetOatLocationFromImageLocation(image_filename);
    os << "OAT LOCATION: " << oat_location;
    os << "\n";
    std::string error_msg;
    const OatFile* oat_file = class_linker->FindOpenedOatFileFromOatLocation(oat_location);
    if (oat_file == nullptr) {
      oat_file = OatFile::Open(oat_location, oat_location,
                               nullptr, nullptr, false, nullptr,
                               &error_msg);
      if (oat_file == nullptr) {
        os << "NOT FOUND: " << error_msg << "\n";
        return false;
      }
    }
    os << "\n";

    stats_.oat_file_bytes = oat_file->Size();

    oat_dumper_.reset(new OatDumper(*oat_file, *oat_dumper_options_));

    for (const OatFile::OatDexFile* oat_dex_file : oat_file->GetOatDexFiles()) {
      CHECK(oat_dex_file != nullptr);
      stats_.oat_dex_file_sizes.push_back(std::make_pair(oat_dex_file->GetDexFileLocation(),
                                                         oat_dex_file->FileSize()));
    }

    os << "OBJECTS:\n" << std::flush;

    // Loop through all the image spaces and dump their objects.
    gc::Heap* heap = Runtime::Current()->GetHeap();
    const std::vector<gc::space::ContinuousSpace*>& spaces = heap->GetContinuousSpaces();
    Thread* self = Thread::Current();
    {
      {
        WriterMutexLock mu(self, *Locks::heap_bitmap_lock_);
        heap->FlushAllocStack();
      }
      // Since FlushAllocStack() above resets the (active) allocation
      // stack. Need to revoke the thread-local allocation stacks that
      // point into it.
      {
        self->TransitionFromRunnableToSuspended(kNative);
        ThreadList* thread_list = Runtime::Current()->GetThreadList();
        thread_list->SuspendAll(__FUNCTION__);
        heap->RevokeAllThreadLocalAllocationStacks(self);
        thread_list->ResumeAll();
        self->TransitionFromSuspendedToRunnable();
      }
    }
    {
      std::ostream* saved_os = os_;
      Indenter indent_filter(os.rdbuf(), kIndentChar, kIndentBy1Count);
      std::ostream indent_os(&indent_filter);
      os_ = &indent_os;

      // Mark dex caches.
      dex_cache_arrays_.clear();
      {
        ReaderMutexLock mu(self, *class_linker->DexLock());
        for (size_t i = 0; i < class_linker->GetDexCacheCount(); ++i) {
          auto* dex_cache = class_linker->GetDexCache(i);
          dex_cache_arrays_.insert(dex_cache->GetResolvedFields());
          dex_cache_arrays_.insert(dex_cache->GetResolvedMethods());
        }
      }
      ReaderMutexLock mu(self, *Locks::heap_bitmap_lock_);
      for (const auto& space : spaces) {
        if (space->IsImageSpace()) {
          auto* image_space = space->AsImageSpace();
          // Dump the normal objects before ArtMethods.
          image_space->GetLiveBitmap()->Walk(ImageDumper::Callback, this);
          indent_os << "\n";
          // TODO: Dump fields.
          // Dump methods after.
          const auto& methods_section = image_header_.GetMethodsSection();
          const auto pointer_size =
              InstructionSetPointerSize(oat_dumper_->GetOatInstructionSet());
          const auto method_size = ArtMethod::ObjectSize(pointer_size);
          for (size_t pos = 0; pos < methods_section.Size(); pos += method_size) {
            auto* method = reinterpret_cast<ArtMethod*>(
                image_space->Begin() + pos + methods_section.Offset());
            indent_os << method << " " << " ArtMethod: " << PrettyMethod(method) << "\n";
            DumpMethod(method, this, indent_os);
            indent_os << "\n";
          }
        }
      }
      // Dump the large objects separately.
      heap->GetLargeObjectsSpace()->GetLiveBitmap()->Walk(ImageDumper::Callback, this);
      indent_os << "\n";
      os_ = saved_os;
    }
    os << "STATS:\n" << std::flush;
    std::unique_ptr<File> file(OS::OpenFileForReading(image_filename.c_str()));
    if (file.get() == nullptr) {
      LOG(WARNING) << "Failed to find image in " << image_filename;
    }
    if (file.get() != nullptr) {
      stats_.file_bytes = file->GetLength();
    }
    size_t header_bytes = sizeof(ImageHeader);
    const auto& bitmap_section = image_header_.GetImageSection(ImageHeader::kSectionImageBitmap);
    const auto& field_section = image_header_.GetImageSection(ImageHeader::kSectionArtFields);
    const auto& method_section = image_header_.GetMethodsSection();
    const auto& intern_section = image_header_.GetImageSection(
        ImageHeader::kSectionInternedStrings);
    stats_.header_bytes = header_bytes;
    size_t alignment_bytes = RoundUp(header_bytes, kObjectAlignment) - header_bytes;
    stats_.alignment_bytes += alignment_bytes;
    stats_.alignment_bytes += bitmap_section.Offset() - image_header_.GetImageSize();
    stats_.bitmap_bytes += bitmap_section.Size();
    stats_.art_field_bytes += field_section.Size();
    stats_.art_method_bytes += method_section.Size();
    stats_.interned_strings_bytes += intern_section.Size();
    stats_.Dump(os);
    os << "\n";

    os << std::flush;

    return oat_dumper_->Dump(os);
  }

 private:
  static void PrettyObjectValue(std::ostream& os, mirror::Class* type, mirror::Object* value)
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
    CHECK(type != nullptr);
    if (value == nullptr) {
      os << StringPrintf("null   %s\n", PrettyDescriptor(type).c_str());
    } else if (type->IsStringClass()) {
      mirror::String* string = value->AsString();
      os << StringPrintf("%p   String: %s\n", string,
                         PrintableString(string->ToModifiedUtf8().c_str()).c_str());
    } else if (type->IsClassClass()) {
      mirror::Class* klass = value->AsClass();
      os << StringPrintf("%p   Class: %s\n", klass, PrettyDescriptor(klass).c_str());
    } else {
      os << StringPrintf("%p   %s\n", value, PrettyDescriptor(type).c_str());
    }
  }

  static void PrintField(std::ostream& os, ArtField* field, mirror::Object* obj)
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
    os << StringPrintf("%s: ", field->GetName());
    switch (field->GetTypeAsPrimitiveType()) {
      case Primitive::kPrimLong:
        os << StringPrintf("%" PRId64 " (0x%" PRIx64 ")\n", field->Get64(obj), field->Get64(obj));
        break;
      case Primitive::kPrimDouble:
        os << StringPrintf("%f (%a)\n", field->GetDouble(obj), field->GetDouble(obj));
        break;
      case Primitive::kPrimFloat:
        os << StringPrintf("%f (%a)\n", field->GetFloat(obj), field->GetFloat(obj));
        break;
      case Primitive::kPrimInt:
        os << StringPrintf("%d (0x%x)\n", field->Get32(obj), field->Get32(obj));
        break;
      case Primitive::kPrimChar:
        os << StringPrintf("%u (0x%x)\n", field->GetChar(obj), field->GetChar(obj));
        break;
      case Primitive::kPrimShort:
        os << StringPrintf("%d (0x%x)\n", field->GetShort(obj), field->GetShort(obj));
        break;
      case Primitive::kPrimBoolean:
        os << StringPrintf("%s (0x%x)\n", field->GetBoolean(obj)? "true" : "false",
            field->GetBoolean(obj));
        break;
      case Primitive::kPrimByte:
        os << StringPrintf("%d (0x%x)\n", field->GetByte(obj), field->GetByte(obj));
        break;
      case Primitive::kPrimNot: {
        // Get the value, don't compute the type unless it is non-null as we don't want
        // to cause class loading.
        mirror::Object* value = field->GetObj(obj);
        if (value == nullptr) {
          os << StringPrintf("null   %s\n", PrettyDescriptor(field->GetTypeDescriptor()).c_str());
        } else {
          // Grab the field type without causing resolution.
          mirror::Class* field_type = field->GetType<false>();
          if (field_type != nullptr) {
            PrettyObjectValue(os, field_type, value);
          } else {
            os << StringPrintf("%p   %s\n", value,
                               PrettyDescriptor(field->GetTypeDescriptor()).c_str());
          }
        }
        break;
      }
      default:
        os << "unexpected field type: " << field->GetTypeDescriptor() << "\n";
        break;
    }
  }

  static void DumpFields(std::ostream& os, mirror::Object* obj, mirror::Class* klass)
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
    mirror::Class* super = klass->GetSuperClass();
    if (super != nullptr) {
      DumpFields(os, obj, super);
    }
    ArtField* fields = klass->GetIFields();
    for (size_t i = 0, count = klass->NumInstanceFields(); i < count; i++) {
      PrintField(os, &fields[i], obj);
    }
  }

  bool InDumpSpace(const mirror::Object* object) {
    return image_space_.Contains(object);
  }

  const void* GetQuickOatCodeBegin(ArtMethod* m)
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
    const void* quick_code = m->GetEntryPointFromQuickCompiledCodePtrSize(
        InstructionSetPointerSize(oat_dumper_->GetOatInstructionSet()));
    if (Runtime::Current()->GetClassLinker()->IsQuickResolutionStub(quick_code)) {
      quick_code = oat_dumper_->GetQuickOatCode(m);
    }
    if (oat_dumper_->GetInstructionSet() == kThumb2) {
      quick_code = reinterpret_cast<void*>(reinterpret_cast<uintptr_t>(quick_code) & ~0x1);
    }
    return quick_code;
  }

  uint32_t GetQuickOatCodeSize(ArtMethod* m)
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
    const uint32_t* oat_code_begin = reinterpret_cast<const uint32_t*>(GetQuickOatCodeBegin(m));
    if (oat_code_begin == nullptr) {
      return 0;
    }
    return oat_code_begin[-1];
  }

  const void* GetQuickOatCodeEnd(ArtMethod* m)
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
    const uint8_t* oat_code_begin = reinterpret_cast<const uint8_t*>(GetQuickOatCodeBegin(m));
    if (oat_code_begin == nullptr) {
      return nullptr;
    }
    return oat_code_begin + GetQuickOatCodeSize(m);
  }

  static void Callback(mirror::Object* obj, void* arg) SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
    DCHECK(obj != nullptr);
    DCHECK(arg != nullptr);
    ImageDumper* state = reinterpret_cast<ImageDumper*>(arg);
    if (!state->InDumpSpace(obj)) {
      return;
    }

    size_t object_bytes = obj->SizeOf();
    size_t alignment_bytes = RoundUp(object_bytes, kObjectAlignment) - object_bytes;
    state->stats_.object_bytes += object_bytes;
    state->stats_.alignment_bytes += alignment_bytes;

    std::ostream& os = *state->os_;
    mirror::Class* obj_class = obj->GetClass();
    if (obj_class->IsArrayClass()) {
      os << StringPrintf("%p: %s length:%d\n", obj, PrettyDescriptor(obj_class).c_str(),
                         obj->AsArray()->GetLength());
    } else if (obj->IsClass()) {
      mirror::Class* klass = obj->AsClass();
      os << StringPrintf("%p: java.lang.Class \"%s\" (", obj, PrettyDescriptor(klass).c_str())
         << klass->GetStatus() << ")\n";
    } else if (obj_class->IsStringClass()) {
      os << StringPrintf("%p: java.lang.String %s\n", obj,
                         PrintableString(obj->AsString()->ToModifiedUtf8().c_str()).c_str());
    } else {
      os << StringPrintf("%p: %s\n", obj, PrettyDescriptor(obj_class).c_str());
    }
    Indenter indent_filter(os.rdbuf(), kIndentChar, kIndentBy1Count);
    std::ostream indent_os(&indent_filter);
    DumpFields(indent_os, obj, obj_class);
    const auto image_pointer_size =
        InstructionSetPointerSize(state->oat_dumper_->GetOatInstructionSet());
    if (obj->IsObjectArray()) {
      auto* obj_array = obj->AsObjectArray<mirror::Object>();
      for (int32_t i = 0, length = obj_array->GetLength(); i < length; i++) {
        mirror::Object* value = obj_array->Get(i);
        size_t run = 0;
        for (int32_t j = i + 1; j < length; j++) {
          if (value == obj_array->Get(j)) {
            run++;
          } else {
            break;
          }
        }
        if (run == 0) {
          indent_os << StringPrintf("%d: ", i);
        } else {
          indent_os << StringPrintf("%d to %zd: ", i, i + run);
          i = i + run;
        }
        mirror::Class* value_class =
            (value == nullptr) ? obj_class->GetComponentType() : value->GetClass();
        PrettyObjectValue(indent_os, value_class, value);
      }
    } else if (obj->IsClass()) {
      mirror::Class* klass = obj->AsClass();
      ArtField* sfields = klass->GetSFields();
      const size_t num_fields = klass->NumStaticFields();
      if (num_fields != 0) {
        indent_os << "STATICS:\n";
        Indenter indent2_filter(indent_os.rdbuf(), kIndentChar, kIndentBy1Count);
        std::ostream indent2_os(&indent2_filter);
        for (size_t i = 0; i < num_fields; i++) {
          PrintField(indent2_os, &sfields[i], sfields[i].GetDeclaringClass());
        }
      }
    } else {
      auto it = state->dex_cache_arrays_.find(obj);
      if (it != state->dex_cache_arrays_.end()) {
        const auto& field_section = state->image_header_.GetImageSection(
            ImageHeader::kSectionArtFields);
        const auto& method_section = state->image_header_.GetMethodsSection();
        auto* arr = down_cast<mirror::PointerArray*>(obj);
        for (int32_t i = 0, length = arr->GetLength(); i < length; i++) {
          void* elem = arr->GetElementPtrSize<void*>(i, image_pointer_size);
          size_t run = 0;
          for (int32_t j = i + 1; j < length &&
              elem == arr->GetElementPtrSize<void*>(j, image_pointer_size); j++, run++) { }
          if (run == 0) {
            indent_os << StringPrintf("%d: ", i);
          } else {
            indent_os << StringPrintf("%d to %zd: ", i, i + run);
            i = i + run;
          }
          auto offset = reinterpret_cast<uint8_t*>(elem) - state->image_space_.Begin();
          std::string msg;
          if (field_section.Contains(offset)) {
            msg = PrettyField(reinterpret_cast<ArtField*>(elem));
          } else if (method_section.Contains(offset)) {
            msg = PrettyMethod(reinterpret_cast<ArtMethod*>(elem));
          } else {
            msg = "Unknown type";
          }
          indent_os << StringPrintf("%p   %s\n", elem, msg.c_str());
        }
      }
    }
    std::string temp;
    state->stats_.Update(obj_class->GetDescriptor(&temp), object_bytes);
  }

  void DumpMethod(ArtMethod* method, ImageDumper* state, std::ostream& indent_os)
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
    DCHECK(method != nullptr);
    const auto image_pointer_size =
        InstructionSetPointerSize(state->oat_dumper_->GetOatInstructionSet());
    if (method->IsNative()) {
      DCHECK(method->GetNativeGcMap(image_pointer_size) == nullptr) << PrettyMethod(method);
      DCHECK(method->GetMappingTable(image_pointer_size) == nullptr) << PrettyMethod(method);
      bool first_occurrence;
      const void* quick_oat_code = state->GetQuickOatCodeBegin(method);
      uint32_t quick_oat_code_size = state->GetQuickOatCodeSize(method);
      state->ComputeOatSize(quick_oat_code, &first_occurrence);
      if (first_occurrence) {
        state->stats_.native_to_managed_code_bytes += quick_oat_code_size;
      }
      if (quick_oat_code != method->GetEntryPointFromQuickCompiledCodePtrSize(image_pointer_size)) {
        indent_os << StringPrintf("OAT CODE: %p\n", quick_oat_code);
      }
    } else if (method->IsAbstract() || method->IsCalleeSaveMethod() ||
      method->IsResolutionMethod() || method->IsImtConflictMethod() ||
      method->IsImtUnimplementedMethod() || method->IsClassInitializer()) {
      DCHECK(method->GetNativeGcMap(image_pointer_size) == nullptr) << PrettyMethod(method);
      DCHECK(method->GetMappingTable(image_pointer_size) == nullptr) << PrettyMethod(method);
    } else {
      const DexFile::CodeItem* code_item = method->GetCodeItem();
      size_t dex_instruction_bytes = code_item->insns_size_in_code_units_ * 2;
      state->stats_.dex_instruction_bytes += dex_instruction_bytes;

      bool first_occurrence;
      size_t gc_map_bytes = state->ComputeOatSize(
          method->GetNativeGcMap(image_pointer_size), &first_occurrence);
      if (first_occurrence) {
        state->stats_.gc_map_bytes += gc_map_bytes;
      }

      size_t pc_mapping_table_bytes = state->ComputeOatSize(
          method->GetMappingTable(image_pointer_size), &first_occurrence);
      if (first_occurrence) {
        state->stats_.pc_mapping_table_bytes += pc_mapping_table_bytes;
      }

      size_t vmap_table_bytes = state->ComputeOatSize(
          method->GetVmapTable(image_pointer_size), &first_occurrence);
      if (first_occurrence) {
        state->stats_.vmap_table_bytes += vmap_table_bytes;
      }

      const void* quick_oat_code_begin = state->GetQuickOatCodeBegin(method);
      const void* quick_oat_code_end = state->GetQuickOatCodeEnd(method);
      uint32_t quick_oat_code_size = state->GetQuickOatCodeSize(method);
      state->ComputeOatSize(quick_oat_code_begin, &first_occurrence);
      if (first_occurrence) {
        state->stats_.managed_code_bytes += quick_oat_code_size;
        if (method->IsConstructor()) {
          if (method->IsStatic()) {
            state->stats_.class_initializer_code_bytes += quick_oat_code_size;
          } else if (dex_instruction_bytes > kLargeConstructorDexBytes) {
            state->stats_.large_initializer_code_bytes += quick_oat_code_size;
          }
        } else if (dex_instruction_bytes > kLargeMethodDexBytes) {
          state->stats_.large_method_code_bytes += quick_oat_code_size;
        }
      }
      state->stats_.managed_code_bytes_ignoring_deduplication += quick_oat_code_size;

      indent_os << StringPrintf("OAT CODE: %p-%p\n", quick_oat_code_begin, quick_oat_code_end);
      indent_os << StringPrintf("SIZE: Dex Instructions=%zd GC=%zd Mapping=%zd\n",
      dex_instruction_bytes, gc_map_bytes, pc_mapping_table_bytes);

      size_t total_size = dex_instruction_bytes + gc_map_bytes + pc_mapping_table_bytes +
          vmap_table_bytes + quick_oat_code_size + ArtMethod::ObjectSize(image_pointer_size);

      double expansion =
      static_cast<double>(quick_oat_code_size) / static_cast<double>(dex_instruction_bytes);
      state->stats_.ComputeOutliers(total_size, expansion, method);
    }
  }

  std::set<const void*> already_seen_;
  // Compute the size of the given data within the oat file and whether this is the first time
  // this data has been requested
  size_t ComputeOatSize(const void* oat_data, bool* first_occurrence) {
    if (already_seen_.count(oat_data) == 0) {
      *first_occurrence = true;
      already_seen_.insert(oat_data);
    } else {
      *first_occurrence = false;
    }
    return oat_dumper_->ComputeSize(oat_data);
  }

 public:
  struct Stats {
    size_t oat_file_bytes;
    size_t file_bytes;

    size_t header_bytes;
    size_t object_bytes;
    size_t art_field_bytes;
    size_t art_method_bytes;
    size_t interned_strings_bytes;
    size_t bitmap_bytes;
    size_t alignment_bytes;

    size_t managed_code_bytes;
    size_t managed_code_bytes_ignoring_deduplication;
    size_t managed_to_native_code_bytes;
    size_t native_to_managed_code_bytes;
    size_t class_initializer_code_bytes;
    size_t large_initializer_code_bytes;
    size_t large_method_code_bytes;

    size_t gc_map_bytes;
    size_t pc_mapping_table_bytes;
    size_t vmap_table_bytes;

    size_t dex_instruction_bytes;

    std::vector<ArtMethod*> method_outlier;
    std::vector<size_t> method_outlier_size;
    std::vector<double> method_outlier_expansion;
    std::vector<std::pair<std::string, size_t>> oat_dex_file_sizes;

    explicit Stats()
        : oat_file_bytes(0),
          file_bytes(0),
          header_bytes(0),
          object_bytes(0),
          art_field_bytes(0),
          art_method_bytes(0),
          interned_strings_bytes(0),
          bitmap_bytes(0),
          alignment_bytes(0),
          managed_code_bytes(0),
          managed_code_bytes_ignoring_deduplication(0),
          managed_to_native_code_bytes(0),
          native_to_managed_code_bytes(0),
          class_initializer_code_bytes(0),
          large_initializer_code_bytes(0),
          large_method_code_bytes(0),
          gc_map_bytes(0),
          pc_mapping_table_bytes(0),
          vmap_table_bytes(0),
          dex_instruction_bytes(0) {}

    struct SizeAndCount {
      SizeAndCount(size_t bytes_in, size_t count_in) : bytes(bytes_in), count(count_in) {}
      size_t bytes;
      size_t count;
    };
    typedef SafeMap<std::string, SizeAndCount> SizeAndCountTable;
    SizeAndCountTable sizes_and_counts;

    void Update(const char* descriptor, size_t object_bytes_in) {
      SizeAndCountTable::iterator it = sizes_and_counts.find(descriptor);
      if (it != sizes_and_counts.end()) {
        it->second.bytes += object_bytes_in;
        it->second.count += 1;
      } else {
        sizes_and_counts.Put(descriptor, SizeAndCount(object_bytes_in, 1));
      }
    }

    double PercentOfOatBytes(size_t size) {
      return (static_cast<double>(size) / static_cast<double>(oat_file_bytes)) * 100;
    }

    double PercentOfFileBytes(size_t size) {
      return (static_cast<double>(size) / static_cast<double>(file_bytes)) * 100;
    }

    double PercentOfObjectBytes(size_t size) {
      return (static_cast<double>(size) / static_cast<double>(object_bytes)) * 100;
    }

    void ComputeOutliers(size_t total_size, double expansion, ArtMethod* method) {
      method_outlier_size.push_back(total_size);
      method_outlier_expansion.push_back(expansion);
      method_outlier.push_back(method);
    }

    void DumpOutliers(std::ostream& os)
        SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
      size_t sum_of_sizes = 0;
      size_t sum_of_sizes_squared = 0;
      size_t sum_of_expansion = 0;
      size_t sum_of_expansion_squared = 0;
      size_t n = method_outlier_size.size();
      for (size_t i = 0; i < n; i++) {
        size_t cur_size = method_outlier_size[i];
        sum_of_sizes += cur_size;
        sum_of_sizes_squared += cur_size * cur_size;
        double cur_expansion = method_outlier_expansion[i];
        sum_of_expansion += cur_expansion;
        sum_of_expansion_squared += cur_expansion * cur_expansion;
      }
      size_t size_mean = sum_of_sizes / n;
      size_t size_variance = (sum_of_sizes_squared - sum_of_sizes * size_mean) / (n - 1);
      double expansion_mean = sum_of_expansion / n;
      double expansion_variance =
          (sum_of_expansion_squared - sum_of_expansion * expansion_mean) / (n - 1);

      // Dump methods whose size is a certain number of standard deviations from the mean
      size_t dumped_values = 0;
      size_t skipped_values = 0;
      for (size_t i = 100; i > 0; i--) {  // i is the current number of standard deviations
        size_t cur_size_variance = i * i * size_variance;
        bool first = true;
        for (size_t j = 0; j < n; j++) {
          size_t cur_size = method_outlier_size[j];
          if (cur_size > size_mean) {
            size_t cur_var = cur_size - size_mean;
            cur_var = cur_var * cur_var;
            if (cur_var > cur_size_variance) {
              if (dumped_values > 20) {
                if (i == 1) {
                  skipped_values++;
                } else {
                  i = 2;  // jump to counting for 1 standard deviation
                  break;
                }
              } else {
                if (first) {
                  os << "\nBig methods (size > " << i << " standard deviations the norm):\n";
                  first = false;
                }
                os << PrettyMethod(method_outlier[j]) << " requires storage of "
                    << PrettySize(cur_size) << "\n";
                method_outlier_size[j] = 0;  // don't consider this method again
                dumped_values++;
              }
            }
          }
        }
      }
      if (skipped_values > 0) {
        os << "... skipped " << skipped_values
           << " methods with size > 1 standard deviation from the norm\n";
      }
      os << std::flush;

      // Dump methods whose expansion is a certain number of standard deviations from the mean
      dumped_values = 0;
      skipped_values = 0;
      for (size_t i = 10; i > 0; i--) {  // i is the current number of standard deviations
        double cur_expansion_variance = i * i * expansion_variance;
        bool first = true;
        for (size_t j = 0; j < n; j++) {
          double cur_expansion = method_outlier_expansion[j];
          if (cur_expansion > expansion_mean) {
            size_t cur_var = cur_expansion - expansion_mean;
            cur_var = cur_var * cur_var;
            if (cur_var > cur_expansion_variance) {
              if (dumped_values > 20) {
                if (i == 1) {
                  skipped_values++;
                } else {
                  i = 2;  // jump to counting for 1 standard deviation
                  break;
                }
              } else {
                if (first) {
                  os << "\nLarge expansion methods (size > " << i
                      << " standard deviations the norm):\n";
                  first = false;
                }
                os << PrettyMethod(method_outlier[j]) << " expanded code by "
                   << cur_expansion << "\n";
                method_outlier_expansion[j] = 0.0;  // don't consider this method again
                dumped_values++;
              }
            }
          }
        }
      }
      if (skipped_values > 0) {
        os << "... skipped " << skipped_values
           << " methods with expansion > 1 standard deviation from the norm\n";
      }
      os << "\n" << std::flush;
    }

    void Dump(std::ostream& os) SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
      {
        os << "art_file_bytes = " << PrettySize(file_bytes) << "\n\n"
           << "art_file_bytes = header_bytes + object_bytes + alignment_bytes\n";
        Indenter indent_filter(os.rdbuf(), kIndentChar, kIndentBy1Count);
        std::ostream indent_os(&indent_filter);
        indent_os << StringPrintf("header_bytes          =  %8zd (%2.0f%% of art file bytes)\n"
                                  "object_bytes          =  %8zd (%2.0f%% of art file bytes)\n"
                                  "art_field_bytes       =  %8zd (%2.0f%% of art file bytes)\n"
                                  "art_method_bytes      =  %8zd (%2.0f%% of art file bytes)\n"
                                  "interned_string_bytes =  %8zd (%2.0f%% of art file bytes)\n"
                                  "bitmap_bytes          =  %8zd (%2.0f%% of art file bytes)\n"
                                  "alignment_bytes       =  %8zd (%2.0f%% of art file bytes)\n\n",
                                  header_bytes, PercentOfFileBytes(header_bytes),
                                  object_bytes, PercentOfFileBytes(object_bytes),
                                  art_field_bytes, PercentOfFileBytes(art_field_bytes),
                                  art_method_bytes, PercentOfFileBytes(art_method_bytes),
                                  interned_strings_bytes,
                                  PercentOfFileBytes(interned_strings_bytes),
                                  bitmap_bytes, PercentOfFileBytes(bitmap_bytes),
                                  alignment_bytes, PercentOfFileBytes(alignment_bytes))
            << std::flush;
        CHECK_EQ(file_bytes, header_bytes + object_bytes + art_field_bytes + art_method_bytes +
                 interned_strings_bytes + bitmap_bytes + alignment_bytes);
      }

      os << "object_bytes breakdown:\n";
      size_t object_bytes_total = 0;
      for (const auto& sizes_and_count : sizes_and_counts) {
        const std::string& descriptor(sizes_and_count.first);
        double average = static_cast<double>(sizes_and_count.second.bytes) /
            static_cast<double>(sizes_and_count.second.count);
        double percent = PercentOfObjectBytes(sizes_and_count.second.bytes);
        os << StringPrintf("%32s %8zd bytes %6zd instances "
                           "(%4.0f bytes/instance) %2.0f%% of object_bytes\n",
                           descriptor.c_str(), sizes_and_count.second.bytes,
                           sizes_and_count.second.count, average, percent);
        object_bytes_total += sizes_and_count.second.bytes;
      }
      os << "\n" << std::flush;
      CHECK_EQ(object_bytes, object_bytes_total);

      os << StringPrintf("oat_file_bytes               = %8zd\n"
                         "managed_code_bytes           = %8zd (%2.0f%% of oat file bytes)\n"
                         "managed_to_native_code_bytes = %8zd (%2.0f%% of oat file bytes)\n"
                         "native_to_managed_code_bytes = %8zd (%2.0f%% of oat file bytes)\n\n"
                         "class_initializer_code_bytes = %8zd (%2.0f%% of oat file bytes)\n"
                         "large_initializer_code_bytes = %8zd (%2.0f%% of oat file bytes)\n"
                         "large_method_code_bytes      = %8zd (%2.0f%% of oat file bytes)\n\n",
                         oat_file_bytes,
                         managed_code_bytes,
                         PercentOfOatBytes(managed_code_bytes),
                         managed_to_native_code_bytes,
                         PercentOfOatBytes(managed_to_native_code_bytes),
                         native_to_managed_code_bytes,
                         PercentOfOatBytes(native_to_managed_code_bytes),
                         class_initializer_code_bytes,
                         PercentOfOatBytes(class_initializer_code_bytes),
                         large_initializer_code_bytes,
                         PercentOfOatBytes(large_initializer_code_bytes),
                         large_method_code_bytes,
                         PercentOfOatBytes(large_method_code_bytes))
            << "DexFile sizes:\n";
      for (const std::pair<std::string, size_t>& oat_dex_file_size : oat_dex_file_sizes) {
        os << StringPrintf("%s = %zd (%2.0f%% of oat file bytes)\n",
                           oat_dex_file_size.first.c_str(), oat_dex_file_size.second,
                           PercentOfOatBytes(oat_dex_file_size.second));
      }

      os << "\n" << StringPrintf("gc_map_bytes           = %7zd (%2.0f%% of oat file bytes)\n"
                                 "pc_mapping_table_bytes = %7zd (%2.0f%% of oat file bytes)\n"
                                 "vmap_table_bytes       = %7zd (%2.0f%% of oat file bytes)\n\n",
                                 gc_map_bytes, PercentOfOatBytes(gc_map_bytes),
                                 pc_mapping_table_bytes, PercentOfOatBytes(pc_mapping_table_bytes),
                                 vmap_table_bytes, PercentOfOatBytes(vmap_table_bytes))
         << std::flush;

      os << StringPrintf("dex_instruction_bytes = %zd\n", dex_instruction_bytes)
         << StringPrintf("managed_code_bytes expansion = %.2f (ignoring deduplication %.2f)\n\n",
                         static_cast<double>(managed_code_bytes) /
                             static_cast<double>(dex_instruction_bytes),
                         static_cast<double>(managed_code_bytes_ignoring_deduplication) /
                             static_cast<double>(dex_instruction_bytes))
         << std::flush;

      DumpOutliers(os);
    }
  } stats_;

 private:
  enum {
    // Number of bytes for a constructor to be considered large. Based on the 1000 basic block
    // threshold, we assume 2 bytes per instruction and 2 instructions per block.
    kLargeConstructorDexBytes = 4000,
    // Number of bytes for a method to be considered large. Based on the 4000 basic block
    // threshold, we assume 2 bytes per instruction and 2 instructions per block.
    kLargeMethodDexBytes = 16000
  };
  std::ostream* os_;
  gc::space::ImageSpace& image_space_;
  const ImageHeader& image_header_;
  std::unique_ptr<OatDumper> oat_dumper_;
  OatDumperOptions* oat_dumper_options_;
  std::set<mirror::Object*> dex_cache_arrays_;

  DISALLOW_COPY_AND_ASSIGN(ImageDumper);
};

static int DumpImage(Runtime* runtime, const char* image_location, OatDumperOptions* options,
                     std::ostream* os) {
  // Dumping the image, no explicit class loader.
  NullHandle<mirror::ClassLoader> null_class_loader;
  options->class_loader_ = &null_class_loader;

  ScopedObjectAccess soa(Thread::Current());
  gc::Heap* heap = runtime->GetHeap();
  gc::space::ImageSpace* image_space = heap->GetImageSpace();
  CHECK(image_space != nullptr);
  const ImageHeader& image_header = image_space->GetImageHeader();
  if (!image_header.IsValid()) {
    fprintf(stderr, "Invalid image header %s\n", image_location);
    return EXIT_FAILURE;
  }

  ImageDumper image_dumper(os, *image_space, image_header, options);

  bool success = image_dumper.Dump();
  return (success) ? EXIT_SUCCESS : EXIT_FAILURE;
}

static int DumpOatWithRuntime(Runtime* runtime, OatFile* oat_file, OatDumperOptions* options,
                              std::ostream* os) {
  CHECK(runtime != nullptr && oat_file != nullptr && options != nullptr);

  Thread* self = Thread::Current();
  CHECK(self != nullptr);
  // Need well-known-classes.
  WellKnownClasses::Init(self->GetJniEnv());

  // Need to register dex files to get a working dex cache.
  ScopedObjectAccess soa(self);
  ClassLinker* class_linker = runtime->GetClassLinker();
  class_linker->RegisterOatFile(oat_file);
  std::vector<std::unique_ptr<const DexFile>> dex_files;
  for (const OatFile::OatDexFile* odf : oat_file->GetOatDexFiles()) {
    std::string error_msg;
    std::unique_ptr<const DexFile> dex_file = odf->OpenDexFile(&error_msg);
    CHECK(dex_file != nullptr) << error_msg;
    class_linker->RegisterDexFile(*dex_file);
    dex_files.push_back(std::move(dex_file));
  }

  // Need a class loader.
  // Fake that we're a compiler.
  std::vector<const DexFile*> class_path;
  for (auto& dex_file : dex_files) {
    class_path.push_back(dex_file.get());
  }
  jobject class_loader = class_linker->CreatePathClassLoader(self, class_path);

  // Use the class loader while dumping.
  StackHandleScope<1> scope(self);
  Handle<mirror::ClassLoader> loader_handle = scope.NewHandle(
      soa.Decode<mirror::ClassLoader*>(class_loader));
  options->class_loader_ = &loader_handle;

  OatDumper oat_dumper(*oat_file, *options);
  bool success = oat_dumper.Dump(*os);
  return (success) ? EXIT_SUCCESS : EXIT_FAILURE;
}

static int DumpOatWithoutRuntime(OatFile* oat_file, OatDumperOptions* options, std::ostream* os) {
  CHECK(oat_file != nullptr && options != nullptr);
  // No image = no class loader.
  NullHandle<mirror::ClassLoader> null_class_loader;
  options->class_loader_ = &null_class_loader;

  OatDumper oat_dumper(*oat_file, *options);
  bool success = oat_dumper.Dump(*os);
  return (success) ? EXIT_SUCCESS : EXIT_FAILURE;
}

static int DumpOat(Runtime* runtime, const char* oat_filename, OatDumperOptions* options,
                   std::ostream* os) {
  std::string error_msg;
  OatFile* oat_file = OatFile::Open(oat_filename, oat_filename, nullptr, nullptr, false,
                                    nullptr, &error_msg);
  if (oat_file == nullptr) {
    fprintf(stderr, "Failed to open oat file from '%s': %s\n", oat_filename, error_msg.c_str());
    return EXIT_FAILURE;
  }

  if (runtime != nullptr) {
    return DumpOatWithRuntime(runtime, oat_file, options, os);
  } else {
    return DumpOatWithoutRuntime(oat_file, options, os);
  }
}

static int SymbolizeOat(const char* oat_filename, std::string& output_name) {
  std::string error_msg;
  OatFile* oat_file = OatFile::Open(oat_filename, oat_filename, nullptr, nullptr, false,
                                    nullptr, &error_msg);
  if (oat_file == nullptr) {
    fprintf(stderr, "Failed to open oat file from '%s': %s\n", oat_filename, error_msg.c_str());
    return EXIT_FAILURE;
  }

  OatSymbolizer oat_symbolizer(oat_file, output_name);
  if (!oat_symbolizer.Symbolize()) {
    fprintf(stderr, "Failed to symbolize\n");
    return EXIT_FAILURE;
  }

  return EXIT_SUCCESS;
}

struct OatdumpArgs : public CmdlineArgs {
 protected:
  using Base = CmdlineArgs;

  virtual ParseStatus ParseCustom(const StringPiece& option,
                                  std::string* error_msg) OVERRIDE {
    {
      ParseStatus base_parse = Base::ParseCustom(option, error_msg);
      if (base_parse != kParseUnknownArgument) {
        return base_parse;
      }
    }

    if (option.starts_with("--oat-file=")) {
      oat_filename_ = option.substr(strlen("--oat-file=")).data();
    } else if (option.starts_with("--image=")) {
      image_location_ = option.substr(strlen("--image=")).data();
    } else if (option =="--dump:raw_mapping_table") {
      dump_raw_mapping_table_ = true;
    } else if (option == "--dump:raw_gc_map") {
      dump_raw_gc_map_ = true;
    } else if (option == "--no-dump:vmap") {
      dump_vmap_ = false;
    } else if (option == "--no-disassemble") {
      disassemble_code_ = false;
    } else if (option.starts_with("--symbolize=")) {
      oat_filename_ = option.substr(strlen("--symbolize=")).data();
      symbolize_ = true;
    } else if (option.starts_with("--class-filter=")) {
      class_filter_ = option.substr(strlen("--class-filter=")).data();
    } else if (option.starts_with("--method-filter=")) {
      method_filter_ = option.substr(strlen("--method-filter=")).data();
    } else if (option.starts_with("--list-classes")) {
      list_classes_ = true;
    } else if (option.starts_with("--list-methods")) {
      list_methods_ = true;
    } else if (option.starts_with("--export-dex-to=")) {
      export_dex_location_ = option.substr(strlen("--export-dex-to=")).data();
    } else if (option.starts_with("--addr2instr=")) {
      if (!ParseUint(option.substr(strlen("--addr2instr=")).data(), &addr2instr_)) {
        *error_msg = "Address conversion failed";
        return kParseError;
      }
    } else {
      return kParseUnknownArgument;
    }

    return kParseOk;
  }

  virtual ParseStatus ParseChecks(std::string* error_msg) OVERRIDE {
    // Infer boot image location from the image location if possible.
    if (boot_image_location_ == nullptr) {
      boot_image_location_ = image_location_;
    }

    // Perform the parent checks.
    ParseStatus parent_checks = Base::ParseChecks(error_msg);
    if (parent_checks != kParseOk) {
      return parent_checks;
    }

    // Perform our own checks.
    if (image_location_ == nullptr && oat_filename_ == nullptr) {
      *error_msg = "Either --image or --oat-file must be specified";
      return kParseError;
    } else if (image_location_ != nullptr && oat_filename_ != nullptr) {
      *error_msg = "Either --image or --oat-file must be specified but not both";
      return kParseError;
    }

    return kParseOk;
  }

  virtual std::string GetUsage() const {
    std::string usage;

    usage +=
        "Usage: oatdump [options] ...\n"
        "    Example: oatdump --image=$ANDROID_PRODUCT_OUT/system/framework/boot.art\n"
        "    Example: adb shell oatdump --image=/system/framework/boot.art\n"
        "\n"
        // Either oat-file or image is required.
        "  --oat-file=<file.oat>: specifies an input oat filename.\n"
        "      Example: --oat-file=/system/framework/boot.oat\n"
        "\n"
        "  --image=<file.art>: specifies an input image location.\n"
        "      Example: --image=/system/framework/boot.art\n"
        "\n";

    usage += Base::GetUsage();

    usage +=  // Optional.
        "  --dump:raw_mapping_table enables dumping of the mapping table.\n"
        "      Example: --dump:raw_mapping_table\n"
        "\n"
        "  --dump:raw_gc_map enables dumping of the GC map.\n"
        "      Example: --dump:raw_gc_map\n"
        "\n"
        "  --no-dump:vmap may be used to disable vmap dumping.\n"
        "      Example: --no-dump:vmap\n"
        "\n"
        "  --no-disassemble may be used to disable disassembly.\n"
        "      Example: --no-disassemble\n"
        "\n"
        "  --list-classes may be used to list target file classes (can be used with filters).\n"
        "      Example: --list-classes\n"
        "      Example: --list-classes --class-filter=com.example.foo\n"
        "\n"
        "  --list-methods may be used to list target file methods (can be used with filters).\n"
        "      Example: --list-methods\n"
        "      Example: --list-methods --class-filter=com.example --method-filter=foo\n"
        "\n"
        "  --symbolize=<file.oat>: output a copy of file.oat with elf symbols included.\n"
        "      Example: --symbolize=/system/framework/boot.oat\n"
        "\n"
        "  --class-filter=<class name>: only dumps classes that contain the filter.\n"
        "      Example: --class-filter=com.example.foo\n"
        "\n"
        "  --method-filter=<method name>: only dumps methods that contain the filter.\n"
        "      Example: --method-filter=foo\n"
        "\n"
        "  --export-dex-to=<directory>: may be used to export oat embedded dex files.\n"
        "      Example: --export-dex-to=/data/local/tmp\n"
        "\n"
        "  --addr2instr=<address>: output matching method disassembled code from relative\n"
        "                          address (e.g. PC from crash dump)\n"
        "      Example: --addr2instr=0x00001a3b\n"
        "\n";

    return usage;
  }

 public:
  const char* oat_filename_ = nullptr;
  const char* class_filter_ = "";
  const char* method_filter_ = "";
  const char* image_location_ = nullptr;
  std::string elf_filename_prefix_;
  bool dump_raw_mapping_table_ = false;
  bool dump_raw_gc_map_ = false;
  bool dump_vmap_ = true;
  bool disassemble_code_ = true;
  bool symbolize_ = false;
  bool list_classes_ = false;
  bool list_methods_ = false;
  uint32_t addr2instr_ = 0;
  const char* export_dex_location_ = nullptr;
};

struct OatdumpMain : public CmdlineMain<OatdumpArgs> {
  virtual bool NeedsRuntime() OVERRIDE {
    CHECK(args_ != nullptr);

    // If we are only doing the oat file, disable absolute_addresses. Keep them for image dumping.
    bool absolute_addresses = (args_->oat_filename_ == nullptr);

    oat_dumper_options_ = std::unique_ptr<OatDumperOptions>(new OatDumperOptions(
        args_->dump_raw_mapping_table_,
        args_->dump_raw_gc_map_,
        args_->dump_vmap_,
        args_->disassemble_code_,
        absolute_addresses,
        args_->class_filter_,
        args_->method_filter_,
        args_->list_classes_,
        args_->list_methods_,
        args_->export_dex_location_,
        args_->addr2instr_));

    return (args_->boot_image_location_ != nullptr || args_->image_location_ != nullptr) &&
          !args_->symbolize_;
  }

  virtual bool ExecuteWithoutRuntime() OVERRIDE {
    CHECK(args_ != nullptr);
    CHECK(args_->oat_filename_ != nullptr);

    MemMap::Init();

    if (args_->symbolize_) {
      return SymbolizeOat(args_->oat_filename_, args_->output_name_) == EXIT_SUCCESS;
    } else {
      return DumpOat(nullptr,
                     args_->oat_filename_,
                     oat_dumper_options_.get(),
                     args_->os_) == EXIT_SUCCESS;
    }
  }

  virtual bool ExecuteWithRuntime(Runtime* runtime) {
    CHECK(args_ != nullptr);

    if (args_->oat_filename_ != nullptr) {
      return DumpOat(runtime,
                     args_->oat_filename_,
                     oat_dumper_options_.get(),
                     args_->os_) == EXIT_SUCCESS;
    }

    return DumpImage(runtime, args_->image_location_, oat_dumper_options_.get(), args_->os_)
      == EXIT_SUCCESS;
  }

  std::unique_ptr<OatDumperOptions> oat_dumper_options_;
};

}  // namespace art

int main(int argc, char** argv) {
  art::OatdumpMain main;
  return main.Main(argc, argv);
}
