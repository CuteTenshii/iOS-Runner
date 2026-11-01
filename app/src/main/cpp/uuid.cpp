#include "uuid.h"

#include <cstddef>
#include <cstdint>
#include <cstdlib>

std::string uuid4() {
    constexpr char kHexDigits[] = "0123456789ABCDEF";
    uint8_t bytes[16];
    arc4random_buf(bytes, sizeof bytes);
    bytes[6] = (bytes[6] & 0x0F) | 0x40;
    bytes[8] = (bytes[8] & 0x3F) | 0x80;
    std::string text;
    text.reserve(36);
    for (size_t i = 0; i < sizeof bytes; ++i) {
        if (i == 4 || i == 6 || i == 8 || i == 10) text += '-';
        text += kHexDigits[bytes[i] >> 4];
        text += kHexDigits[bytes[i] & 0x0F];
    }
    return text;
}
