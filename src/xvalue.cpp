// kvspace XValue TLV codec implementation.
#include "kvspace/xvalue.h"
#include <algorithm>

namespace kvspace {

// isValidKind: kind name must be [a-zA-Z0-9_], len 1–127.
static bool isValidKind(std::string_view s) {
    if (s.empty() || s.size() > 127) return false;
    for (char c : s) {
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
              (c >= '0' && c <= '9') || c == '_'))
            return false;
    }
    return true;
}

std::vector<uint8_t> XValue::Encode() const {
    if (IsNull()) return {};
    size_t kl = kind_.size(), rl = raw_.size();
    std::vector<uint8_t> buf(1 + kl + 4 + rl);
    buf[0] = static_cast<uint8_t>(kl);
    std::memcpy(buf.data() + 1, kind_.data(), kl);
    // raw_len LE
    buf[1 + kl + 0] = static_cast<uint8_t>(rl);
    buf[1 + kl + 1] = static_cast<uint8_t>(rl >> 8);
    buf[1 + kl + 2] = static_cast<uint8_t>(rl >> 16);
    buf[1 + kl + 3] = static_cast<uint8_t>(rl >> 24);
    if (rl > 0) std::memcpy(buf.data() + 1 + kl + 4, raw_.data(), rl);
    return buf;
}

XValue XValue::Decode(const uint8_t* data, size_t len) {
    if (len == 0) return XValue();
    size_t kindLen = static_cast<size_t>(data[0]);
    if (len < 1 + kindLen + 4) return XValue();
    std::string kind(reinterpret_cast<const char*>(data + 1), kindLen);
    if (!isValidKind(kind)) return XValue();

    const uint8_t* rawLenPtr = data + 1 + kindLen;
    size_t rawLen = static_cast<size_t>(rawLenPtr[0]) |
                    (static_cast<size_t>(rawLenPtr[1]) << 8) |
                    (static_cast<size_t>(rawLenPtr[2]) << 16) |
                    (static_cast<size_t>(rawLenPtr[3]) << 24);
    size_t start = 1 + kindLen + 4;
    if (len < start + rawLen) return XValue();

    std::vector<uint8_t> raw(data + start, data + start + rawLen);
    return XValue(kind, raw);
}

size_t XValue::EncodedSize() const {
    if (IsNull()) return 0;
    return 1 + kind_.size() + 4 + raw_.size();
}

} // namespace kvspace
