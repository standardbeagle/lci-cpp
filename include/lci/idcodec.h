#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>

#include <lci/types.h>

namespace lci {

/// Error codes for base-63 encoding/decoding operations.
enum class CodecErrc {
    EmptyString = 1,
    InvalidChar,
    Overflow,
};

/// Returns a descriptive name for a codec error code.
constexpr std::string_view to_string(CodecErrc e) {
    switch (e) {
        case CodecErrc::EmptyString: return "empty encoded string";
        case CodecErrc::InvalidChar: return "invalid character in encoded string";
        case CodecErrc::Overflow: return "decoded value overflow";
    }
    return "unknown codec error";
}

/// Lightweight result type for decode operations (C++20 compatible).
/// Holds either a value or an error code.
template <typename T>
class DecodeResult {
  public:
    DecodeResult(T value) : data_(std::move(value)) {}  // NOLINT(implicit)
    DecodeResult(CodecErrc err) : data_(err) {}          // NOLINT(implicit)

    bool has_value() const { return std::holds_alternative<T>(data_); }
    explicit operator bool() const { return has_value(); }

    const T& value() const { return std::get<T>(data_); }
    T& value() { return std::get<T>(data_); }
    const T& operator*() const { return value(); }

    CodecErrc error() const { return std::get<CodecErrc>(data_); }

  private:
    std::variant<T, CodecErrc> data_;
};

// -- Base-63 constants --------------------------------------------------------

/// Number of symbols in the base-63 alphabet.
inline constexpr uint64_t kBase63 = 63;

/// The base-63 alphabet: A-Z (0-25), a-z (26-51), 0-9 (52-61), _ (62).
inline constexpr std::string_view kAlphabet63 =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789_";

// -- Constexpr lookup tables --------------------------------------------------

namespace detail {

/// Builds a reverse lookup table mapping ASCII byte -> base-63 value.
/// Invalid characters map to 255.
consteval std::array<uint8_t, 128> make_char_to_value() {
    std::array<uint8_t, 128> table{};
    for (auto& v : table) v = 255;
    for (uint8_t i = 0; i < 26; ++i) table[static_cast<size_t>('A' + i)] = i;
    for (uint8_t i = 0; i < 26; ++i)
        table[static_cast<size_t>('a' + i)] = static_cast<uint8_t>(26 + i);
    for (uint8_t i = 0; i < 10; ++i)
        table[static_cast<size_t>('0' + i)] = static_cast<uint8_t>(52 + i);
    table[static_cast<size_t>('_')] = 62;
    return table;
}

/// Compile-time reverse lookup: ASCII char -> base-63 digit (255 = invalid).
inline constexpr auto kCharToValue = make_char_to_value();

/// Maximum number of base-63 digits in a uint64_t encoding.
inline constexpr int kMaxBase63Digits = 11;

}  // namespace detail

// -- Base-63 encode/decode ----------------------------------------------------

/// Encodes a uint64 value to a base-63 string.
/// Returns "A" for zero (minimum non-empty encoding).
inline std::string base63_encode(uint64_t value) {
    if (value == 0) return "A";

    std::array<char, detail::kMaxBase63Digits> buf{};
    int pos = detail::kMaxBase63Digits;

    while (value > 0) {
        --pos;
        buf[static_cast<size_t>(pos)] = static_cast<char>(kAlphabet63[value % kBase63]);
        value /= kBase63;
    }

    return std::string(buf.data() + pos,
                       static_cast<size_t>(detail::kMaxBase63Digits - pos));
}

/// Encodes a uint64 value to a base-63 string.
/// Returns empty string for zero (used for composite IDs where 0 means "none").
inline std::string base63_encode_no_zero(uint64_t value) {
    if (value == 0) return {};
    return base63_encode(value);
}

/// Decodes a base-63 string to a uint64 value.
/// Returns an error for empty strings, invalid characters, or overflow.
inline DecodeResult<uint64_t> base63_decode(std::string_view encoded) {
    if (encoded.empty()) return CodecErrc::EmptyString;

    uint64_t value = 0;
    for (char c : encoded) {
        auto uc = static_cast<unsigned char>(c);
        if (uc >= 128) return CodecErrc::InvalidChar;

        uint8_t digit = detail::kCharToValue[uc];
        if (digit == 255) return CodecErrc::InvalidChar;

        if (value > (UINT64_MAX / kBase63)) {
            return CodecErrc::Overflow;
        }
        value = value * kBase63 + digit;
    }

    return value;
}

/// Checks if a string is a valid base-63 encoded value.
inline bool base63_is_valid(std::string_view encoded) {
    if (encoded.empty()) return false;
    for (char c : encoded) {
        auto uc = static_cast<unsigned char>(c);
        if (uc >= 128 || detail::kCharToValue[uc] == 255) return false;
    }
    return true;
}

// -- Symbol ID encode/decode --------------------------------------------------

/// Encodes a SymbolID to a base-63 string.
inline std::string encode_symbol_id(SymbolID id) {
    return base63_encode(id);
}

/// Decodes a base-63 string to a SymbolID.
inline DecodeResult<SymbolID> decode_symbol_id(std::string_view encoded) {
    return base63_decode(encoded);
}

// -- File ID encode/decode ----------------------------------------------------

/// Encodes a FileID to a base-63 string.
inline std::string encode_file_id(FileID id) {
    return base63_encode(id);
}

/// Decodes a base-63 string to a FileID.
/// Returns Overflow if the decoded value exceeds uint32_t range.
inline DecodeResult<FileID> decode_file_id(std::string_view encoded) {
    auto result = base63_decode(encoded);
    if (!result) return result.error();
    if (*result > UINT32_MAX) return CodecErrc::Overflow;
    return static_cast<FileID>(*result);
}

// -- Composite ID packing -----------------------------------------------------

/// Packs two uint32 values into a single uint64.
/// Lower goes into lower 32 bits, upper goes into upper 32 bits.
constexpr uint64_t pack_uint32_pair(uint32_t lower, uint32_t upper) {
    return static_cast<uint64_t>(lower) | (static_cast<uint64_t>(upper) << 32);
}

/// Unpacks a uint64 into two uint32 values: {lower, upper}.
constexpr std::pair<uint32_t, uint32_t> unpack_uint32_pair(uint64_t packed) {
    return {static_cast<uint32_t>(packed & 0xFFFFFFFF),
            static_cast<uint32_t>((packed >> 32) & 0xFFFFFFFF)};
}

/// Encodes a FileID and local symbol ID into a single base-63 string.
inline std::string encode_composite(FileID file_id, uint32_t local_symbol_id) {
    uint64_t combined = pack_uint32_pair(static_cast<uint32_t>(file_id), local_symbol_id);
    return base63_encode_no_zero(combined);
}

/// Decoded composite ID containing file and local symbol identifiers.
struct CompositeID {
    FileID file_id{};
    uint32_t local_symbol_id{};
};

/// Decodes a base-63 string into a FileID and local symbol ID.
inline DecodeResult<CompositeID> decode_composite(std::string_view encoded) {
    if (encoded.empty()) return CodecErrc::EmptyString;

    auto result = base63_decode(encoded);
    if (!result) return result.error();

    auto [lower, upper] = unpack_uint32_pair(*result);
    return CompositeID{static_cast<FileID>(lower), upper};
}

/// Packs a FileID and local symbol ID into a uint64.
constexpr uint64_t pack_composite(FileID file_id, uint32_t local_symbol_id) {
    return pack_uint32_pair(static_cast<uint32_t>(file_id), local_symbol_id);
}

/// Unpacks a uint64 into a CompositeID.
constexpr CompositeID unpack_composite(uint64_t packed) {
    auto [lower, upper] = unpack_uint32_pair(packed);
    return {static_cast<FileID>(lower), upper};
}

}  // namespace lci
