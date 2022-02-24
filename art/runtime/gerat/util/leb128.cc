#include "leb128.h"

namespace gerat
{
    uint8_t* EncodeUnsignedLeb128(uint8_t* dest, uint32_t value) 
    {
        uint8_t out = value & 0x7f;
        value >>= 7;
        while (value != 0) {
            *dest++ = out | 0x80;
            out = value & 0x7f;
            value >>= 7;
        }
        *dest++ = out;
        return dest;
    }
} // namespace gerat
