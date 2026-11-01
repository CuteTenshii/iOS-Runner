#include "base64.h"

#include <cstdint>
#include <cstring>

namespace {

constexpr char kAlphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

bool decode(const std::string& text, bool strict, std::string& out) {
    out.clear();
    uint32_t bits = 0;
    int count = 0;
    for (const char c : text) {
        if (strict && c == '=') break;
        const char* found = c != '\0' ? strchr(kAlphabet, c) : nullptr;
        if (found == nullptr) {
            if (strict) return false;
            continue;
        }
        bits = (bits << 6) | static_cast<uint32_t>(found - kAlphabet);
        count += 6;
        if (count >= 8) {
            count -= 8;
            out += static_cast<char>((bits >> count) & 0xff);
        }
    }
    return true;
}

}  // namespace

std::string base64_encode(const std::string& bytes) {
    std::string out;
    uint32_t bits = 0;
    int count = 0;
    for (const char c : bytes) {
        bits = (bits << 8) | static_cast<uint8_t>(c);
        count += 8;
        while (count >= 6) {
            count -= 6;
            out += kAlphabet[(bits >> count) & 63];
        }
    }
    if (count > 0) out += kAlphabet[(bits << (6 - count)) & 63];
    while (out.size() % 4 != 0) out += '=';
    return out;
}

std::string base64_decode(const std::string& text) {
    std::string out;
    decode(text, false, out);
    return out;
}

bool base64_decode_strict(const std::string& text, std::string& out) { return decode(text, true, out); }
