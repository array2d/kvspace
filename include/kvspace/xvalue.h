// kvspace XValue — typed value with TLV encoding.
// Aligned with Go kvspace.XValue (internal/kvspace/xvalue*.go).
#pragma once

#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>

namespace kvspace {

// ── XValue: tagged union over TLV-encoded raw bytes ──────────────────────────
//
// Zero value: kind_=="" (null). Once constructed via factory, logically immutable.
// Raw bytes are owned; copies are deep.
//
// TLV wire format (Encode/Decode):
//   [1B kind_len][N B kind_name][4B raw_len LE][M B raw_value]
//   null → empty bytes

class XValue {
public:
    XValue() : kind_("null") {}

    // ── factories ──────────────────────────────────────────────────────────

    static XValue Null() { return XValue("null", {}); }

    static XValue Int8(int8_t v)   { return XValue("int8",   encodeInt8(v)); }
    static XValue Int16(int16_t v) { return XValue("int16",  encodeInt16(v)); }
    static XValue Int32(int32_t v) { return XValue("int32",  encodeInt32(v)); }
    static XValue Int64(int64_t v) { return XValue("int64",  encodeInt64(v)); }
    static XValue Int(int64_t v)   { return Int64(v); }

    static XValue Uint8(uint8_t v)   { return XValue("uint8",  encodeUint8(v)); }
    static XValue Uint16(uint16_t v) { return XValue("uint16", encodeUint16(v)); }
    static XValue Uint32(uint32_t v) { return XValue("uint32", encodeUint32(v)); }
    static XValue Uint64(uint64_t v) { return XValue("uint64", encodeUint64(v)); }

    static XValue Float32(float v) { return XValue("float32", encodeFloat32(v)); }
    static XValue Float64(double v){ return XValue("float64", encodeFloat64(v)); }
    static XValue Float(double v)  { return Float64(v); }

    static XValue Bool(bool v) {
        uint8_t b = v ? 1 : 0;
        return XValue("bool", {&b, &b + 1});
    }

    static XValue Str(std::string_view v) {
        return XValue("string", std::vector<uint8_t>(v.begin(), v.end()));
    }

    static XValue Bytes(const std::vector<uint8_t>& v) {
        return XValue("bytes", v);
    }

    // Raw constructs an XValue with arbitrary kind and raw bytes (for extensions).
    static XValue Raw(std::string_view kind, const std::vector<uint8_t>& raw) {
        return XValue(std::string(kind), raw);
    }

    // ── accessors ───────────────────────────────────────────────────────────

    bool IsNull() const { return kind_ == "null" || kind_.empty(); }
    const std::string& Kind() const { return kind_; }
    const std::vector<uint8_t>& RawBytes() const { return raw_; }

    int8_t  AsInt8()  const;
    int16_t AsInt16() const;
    int32_t AsInt32() const;
    int64_t AsInt64() const;
    int64_t AsInt()   const { return AsInt64(); }

    uint8_t  AsUint8()  const;
    uint16_t AsUint16() const;
    uint32_t AsUint32() const;
    uint64_t AsUint64() const;

    float  AsFloat32() const;
    double AsFloat64() const;
    double AsFloat()   const { return AsFloat64(); }

    bool AsBool() const;
    std::string AsStr() const;
    std::vector<uint8_t> AsBytes() const;

    // ── TLV codec ───────────────────────────────────────────────────────────

    // Encode to TLV wire format. Null → empty vector.
    std::vector<uint8_t> Encode() const;

    // Decode from TLV wire format. Invalid/empty → Null().
    static XValue Decode(const uint8_t* data, size_t len);
    static XValue Decode(const std::vector<uint8_t>& data) {
        return Decode(data.data(), data.size());
    }

    // Encoded size in bytes (without allocating). Null → 0.
    size_t EncodedSize() const;

    // ── comparison ──────────────────────────────────────────────────────────

    bool operator==(const XValue& o) const {
        return kind_ == o.kind_ && raw_ == o.raw_;
    }
    bool operator!=(const XValue& o) const { return !(*this == o); }

private:
    XValue(std::string kind, std::vector<uint8_t> raw)
        : kind_(std::move(kind)), raw_(std::move(raw)) {}

    std::string kind_;
    std::vector<uint8_t> raw_;

    // ── low-level encode helpers ────────────────────────────────────────────

    static std::vector<uint8_t> encodeInt8(int8_t v)   { return {static_cast<uint8_t>(v)}; }
    static std::vector<uint8_t> encodeInt16(int16_t v) { return leBytes(v); }
    static std::vector<uint8_t> encodeInt32(int32_t v) { return leBytes(v); }
    static std::vector<uint8_t> encodeInt64(int64_t v) { return leBytes(v); }
    static std::vector<uint8_t> encodeUint8(uint8_t v)   { return {v}; }
    static std::vector<uint8_t> encodeUint16(uint16_t v) { return leBytes(v); }
    static std::vector<uint8_t> encodeUint32(uint32_t v) { return leBytes(v); }
    static std::vector<uint8_t> encodeUint64(uint64_t v) { return leBytes(v); }

    static std::vector<uint8_t> encodeFloat32(float v) {
        uint32_t bits; std::memcpy(&bits, &v, 4);
        return leBytes(bits);
    }
    static std::vector<uint8_t> encodeFloat64(double v) {
        uint64_t bits; std::memcpy(&bits, &v, 8);
        return leBytes(bits);
    }

    template<typename T>
    static std::vector<uint8_t> leBytes(T v) {
        std::vector<uint8_t> b(sizeof(T));
        for (size_t i = 0; i < sizeof(T); i++)
            b[i] = static_cast<uint8_t>(v >> (i * 8));
        return b;
    }

    template<typename T>
    static T fromLE(const uint8_t* data) {
        T v = 0;
        for (size_t i = 0; i < sizeof(T); i++)
            v |= static_cast<T>(data[i]) << (i * 8);
        return v;
    }
};

// ── inline accessor impls ───────────────────────────────────────────────────

inline int8_t XValue::AsInt8() const {
    if (kind_ != "int8" || raw_.size() < 1) return 0;
    return static_cast<int8_t>(raw_[0]);
}
inline int16_t XValue::AsInt16() const {
    if (kind_ != "int16" || raw_.size() < 2) return 0;
    return static_cast<int16_t>(fromLE<uint16_t>(raw_.data()));
}
inline int32_t XValue::AsInt32() const {
    if (kind_ != "int32" || raw_.size() < 4) return 0;
    return static_cast<int32_t>(fromLE<uint32_t>(raw_.data()));
}
inline int64_t XValue::AsInt64() const {
    if (kind_ != "int64" || raw_.size() < 8) return 0;
    return static_cast<int64_t>(fromLE<uint64_t>(raw_.data()));
}

inline uint8_t XValue::AsUint8() const {
    if (kind_ != "uint8" || raw_.empty()) return 0;
    return raw_[0];
}
inline uint16_t XValue::AsUint16() const {
    if (kind_ != "uint16" || raw_.size() < 2) return 0;
    return fromLE<uint16_t>(raw_.data());
}
inline uint32_t XValue::AsUint32() const {
    if (kind_ != "uint32" || raw_.size() < 4) return 0;
    return fromLE<uint32_t>(raw_.data());
}
inline uint64_t XValue::AsUint64() const {
    if (kind_ != "uint64" || raw_.size() < 8) return 0;
    return fromLE<uint64_t>(raw_.data());
}

inline float XValue::AsFloat32() const {
    if (kind_ != "float32" || raw_.size() < 4) return 0;
    uint32_t bits = fromLE<uint32_t>(raw_.data());
    float v; std::memcpy(&v, &bits, 4); return v;
}
inline double XValue::AsFloat64() const {
    if (kind_ != "float64" || raw_.size() < 8) return 0;
    uint64_t bits = fromLE<uint64_t>(raw_.data());
    double v; std::memcpy(&v, &bits, 8); return v;
}

inline bool XValue::AsBool() const {
    if (kind_ != "bool" || raw_.empty()) return false;
    return raw_[0] != 0;
}
inline std::string XValue::AsStr() const {
    if (kind_ != "string") return "";
    return std::string(raw_.begin(), raw_.end());
}
inline std::vector<uint8_t> XValue::AsBytes() const {
    if (kind_ != "bytes") return {};
    return raw_;
}

} // namespace kvspace
