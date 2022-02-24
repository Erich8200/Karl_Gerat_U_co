#include <stdint.h>
#include <vector>
#include "leb128.h"

namespace gerat {

    class ClassDataHeader 
    {
    public:
        uint32_t static_fields_size_;  // the number of static fields
        uint32_t instance_fields_size_;  // the number of instance fields
        uint32_t direct_methods_size_;  // the number of direct methods
        uint32_t virtual_methods_size_;  // the number of virtual methods
        // uint8_t* encode(uint8_t* in);
    };

    // A decoded version of the field of a class_data_item
    class ClassDataField 
    {
    public:
        uint32_t field_idx_delta_;  // delta of index into the field_ids array for FieldId
        uint32_t access_flags_;  // access flags for the field
        // uint8_t* encode(uint8_t* in);
    };

    // A decoded version of the method of a class_data_item
    class ClassDataMethod 
    {
    public:
        uint16_t method_idx; // Added for Karl Gerat U, not existing in original AOSP struct
        uint32_t method_idx_delta_;  // delta of index into the method_ids array for MethodId
        uint32_t access_flags_;
        uint32_t code_off_;
        // uint8_t* encode(uint8_t* in);
    };

    class ClassDataItemMem
    {
    public:
        uint16_t class_idx; // Added for Karl Gerat U, not existing in original AOSP struct
        uint32_t static_fields_size_;  // the number of static fields
        uint32_t instance_fields_size_;  // the number of instance fields
        uint32_t direct_methods_size_;  // the number of direct methods
        uint32_t virtual_methods_size_;  // the number of virtual methods
        std::vector<ClassDataField> fields; // fields
        std::vector<ClassDataMethod> methods; // methods
        uint8_t* encode(uint8_t* in);
    };

    typedef struct DexHeader {
        uint8_t magic_[8];
        uint32_t checksum_;  // See also location_checksum_
        uint8_t signature_[20U];
        uint32_t file_size_;  // size of entire file
        uint32_t header_size_;  // offset to start of next section
        uint32_t endian_tag_;
        uint32_t link_size_;  // unused
        uint32_t link_off_;  // unused
        uint32_t map_off_;  // unused
        uint32_t string_ids_size_;  // number of StringIds
        uint32_t string_ids_off_;  // file offset of StringIds array
        uint32_t type_ids_size_;  // number of TypeIds, we don't support more than 65535
        uint32_t type_ids_off_;  // file offset of TypeIds array
        uint32_t proto_ids_size_;  // number of ProtoIds, we don't support more than 65535
        uint32_t proto_ids_off_;  // file offset of ProtoIds array
        uint32_t field_ids_size_;  // number of FieldIds
        uint32_t field_ids_off_;  // file offset of FieldIds array
        uint32_t method_ids_size_;  // number of MethodIds
        uint32_t method_ids_off_;  // file offset of MethodIds array
        uint32_t class_defs_size_;  // number of ClassDefs
        uint32_t class_defs_off_;  // file offset of ClassDef array
        uint32_t data_size_;  // unused
        uint32_t data_off_;  // unused
    } DexHeader;

    // Raw class_def_item.
    struct ClassDef {
        uint16_t class_idx_;  // index into type_ids_ array for this class
        uint16_t pad1_;  // padding = 0
        uint32_t access_flags_;
        uint16_t superclass_idx_;  // index into type_ids_ array for superclass
        uint16_t pad2_;  // padding = 0
        uint32_t interfaces_off_;  // file offset to TypeList
        uint32_t source_file_idx_;  // index into string_ids_ for source file name
        uint32_t annotations_off_;  // file offset to annotations_directory_item
        uint32_t class_data_off_;  // file offset to class_data_item
        uint32_t static_values_off_;  // file offset to EncodedArray
    };

}
