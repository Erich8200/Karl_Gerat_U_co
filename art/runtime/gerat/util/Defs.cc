#include "Defs.h"

namespace gerat
{
    // uint8_t* ClassDataHeader::encode(uint8_t* in)
    // {
    //     in = gerat::EncodeUnsignedLeb128(in, static_fields_size_);
    //     in = gerat::EncodeUnsignedLeb128(in, instance_fields_size_);
    //     in = gerat::EncodeUnsignedLeb128(in, direct_methods_size_);
    //     return gerat::EncodeUnsignedLeb128(in, virtual_methods_size_);
    // }

    // uint8_t* ClassDataField::encode(uint8_t* in)
    // {
    //     in = gerat::EncodeUnsignedLeb128(in, field_idx_delta_);
    //     return gerat::EncodeUnsignedLeb128(in, access_flags_);
    // }

    // uint8_t* ClassDataMethod::encode(uint8_t* in)
    // {
    //     in = gerat::EncodeUnsignedLeb128(in, method_idx_delta_);
    //     in = gerat::EncodeUnsignedLeb128(in, access_flags_);
    //     return gerat::EncodeUnsignedLeb128(in, code_off_);
    // }

    uint8_t* ClassDataItemMem::encode(uint8_t* in)
    {
        in = gerat::EncodeUnsignedLeb128(in, static_fields_size_);
        in = gerat::EncodeUnsignedLeb128(in, instance_fields_size_);
        in = gerat::EncodeUnsignedLeb128(in, direct_methods_size_);
        in = gerat::EncodeUnsignedLeb128(in, virtual_methods_size_);

        for (size_t i = 0; i < fields.size(); i++) {
            in = gerat::EncodeUnsignedLeb128(in, fields[i].field_idx_delta_);
            in = gerat::EncodeUnsignedLeb128(in, fields[i].access_flags_);            
        }

        for (size_t i = 0; i < methods.size(); i++) {
            in = gerat::EncodeUnsignedLeb128(in, methods[i].method_idx_delta_);
            in = gerat::EncodeUnsignedLeb128(in, methods[i].access_flags_);
            in = gerat::EncodeUnsignedLeb128(in, methods[i].code_off_);            
        }
        return in;
    }

} // namespace gerat