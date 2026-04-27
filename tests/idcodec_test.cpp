#include <gtest/gtest.h>

#include <lci/idcodec.h>

namespace lci {
namespace {

// ---------------------------------------------------------------------------
// Base-63 encode: zero
// ---------------------------------------------------------------------------
TEST(Base63EncodeTest, ZeroEncodesToA) {
    EXPECT_EQ(base63_encode(0), "A");
}

// ---------------------------------------------------------------------------
// Base-63 encode: single digits (0-62)
// ---------------------------------------------------------------------------
TEST(Base63EncodeTest, SingleDigits) {
    EXPECT_EQ(base63_encode(0), "A");
    EXPECT_EQ(base63_encode(1), "B");
    EXPECT_EQ(base63_encode(25), "Z");
    EXPECT_EQ(base63_encode(26), "a");
    EXPECT_EQ(base63_encode(51), "z");
    EXPECT_EQ(base63_encode(52), "0");
    EXPECT_EQ(base63_encode(61), "9");
    EXPECT_EQ(base63_encode(62), "_");
}

// ---------------------------------------------------------------------------
// Base-63 encode: multi-digit values
// ---------------------------------------------------------------------------
TEST(Base63EncodeTest, MultiDigit) {
    EXPECT_EQ(base63_encode(63), "BA");
    EXPECT_EQ(base63_encode(64), "BB");
    EXPECT_EQ(base63_encode(125), "B_");
    EXPECT_EQ(base63_encode(126), "CA");
    EXPECT_EQ(base63_encode(3969), "BAA");
    EXPECT_EQ(base63_encode(5130), "BSb");
}

// ---------------------------------------------------------------------------
// Base-63 decode: single digits
// ---------------------------------------------------------------------------
TEST(Base63DecodeTest, SingleDigits) {
    EXPECT_EQ(*base63_decode("A"), 0u);
    EXPECT_EQ(*base63_decode("B"), 1u);
    EXPECT_EQ(*base63_decode("Z"), 25u);
    EXPECT_EQ(*base63_decode("a"), 26u);
    EXPECT_EQ(*base63_decode("z"), 51u);
    EXPECT_EQ(*base63_decode("0"), 52u);
    EXPECT_EQ(*base63_decode("9"), 61u);
    EXPECT_EQ(*base63_decode("_"), 62u);
}

// ---------------------------------------------------------------------------
// Base-63 decode: error cases
// ---------------------------------------------------------------------------
TEST(Base63DecodeTest, EmptyStringReturnsError) {
    auto result = base63_decode("");
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error(), CodecErrc::EmptyString);
}

TEST(Base63DecodeTest, InvalidCharReturnsError) {
    auto result = base63_decode("!");
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error(), CodecErrc::InvalidChar);
}

TEST(Base63DecodeTest, SpaceIsInvalid) {
    auto result = base63_decode("AB CD");
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error(), CodecErrc::InvalidChar);
}

// ---------------------------------------------------------------------------
// Base-63 round-trip
// ---------------------------------------------------------------------------
TEST(Base63RoundTripTest, SmallValues) {
    for (uint64_t v = 0; v <= 1000; ++v) {
        auto encoded = base63_encode(v);
        auto decoded = base63_decode(encoded);
        ASSERT_TRUE(decoded.has_value()) << "Failed to decode " << encoded;
        EXPECT_EQ(*decoded, v) << "Round-trip failed for " << v;
    }
}

TEST(Base63RoundTripTest, BoundaryValues) {
    uint64_t values[] = {
        0, 1, 62, 63, 64, 100, 1000, 10000, 100000, 1000000,
        5130,
        UINT32_MAX,
        UINT64_MAX,
    };
    for (uint64_t v : values) {
        auto encoded = base63_encode(v);
        auto decoded = base63_decode(encoded);
        ASSERT_TRUE(decoded.has_value()) << "Failed to decode " << encoded;
        EXPECT_EQ(*decoded, v) << "Round-trip failed for " << v;
    }
}

// ---------------------------------------------------------------------------
// Base-63 encode_no_zero
// ---------------------------------------------------------------------------
TEST(Base63EncodeNoZeroTest, ZeroReturnsEmpty) {
    EXPECT_EQ(base63_encode_no_zero(0), "");
}

TEST(Base63EncodeNoZeroTest, NonZeroEncodesNormally) {
    EXPECT_EQ(base63_encode_no_zero(1), "B");
    EXPECT_EQ(base63_encode_no_zero(63), "BA");
}

// ---------------------------------------------------------------------------
// Base-63 is_valid
// ---------------------------------------------------------------------------
TEST(Base63IsValidTest, ValidStrings) {
    EXPECT_TRUE(base63_is_valid("A"));
    EXPECT_TRUE(base63_is_valid("ABC"));
    EXPECT_TRUE(base63_is_valid("abcXYZ_09"));
}

TEST(Base63IsValidTest, InvalidStrings) {
    EXPECT_FALSE(base63_is_valid(""));
    EXPECT_FALSE(base63_is_valid("!"));
    EXPECT_FALSE(base63_is_valid("AB CD"));
}

// ---------------------------------------------------------------------------
// Symbol ID encode/decode
// ---------------------------------------------------------------------------
TEST(SymbolIDTest, EncodeDecodeRoundTrip) {
    SymbolID ids[] = {0, 1, 42, 12345, UINT32_MAX, UINT64_MAX};
    for (SymbolID id : ids) {
        auto encoded = encode_symbol_id(id);
        auto decoded = decode_symbol_id(encoded);
        ASSERT_TRUE(decoded.has_value());
        EXPECT_EQ(*decoded, id);
    }
}

// ---------------------------------------------------------------------------
// File ID encode/decode
// ---------------------------------------------------------------------------
TEST(FileIDTest, EncodeDecodeRoundTrip) {
    FileID ids[] = {0, 1, 42, 12345, UINT32_MAX};
    for (FileID id : ids) {
        auto encoded = encode_file_id(id);
        auto decoded = decode_file_id(encoded);
        ASSERT_TRUE(decoded.has_value());
        EXPECT_EQ(*decoded, id);
    }
}

TEST(FileIDTest, OverflowReturnsError) {
    auto encoded = base63_encode(static_cast<uint64_t>(UINT32_MAX) + 1);
    auto decoded = decode_file_id(encoded);
    ASSERT_FALSE(decoded.has_value());
    EXPECT_EQ(decoded.error(), CodecErrc::Overflow);
}

// ---------------------------------------------------------------------------
// Pack/Unpack uint32 pair
// ---------------------------------------------------------------------------
TEST(PackUint32PairTest, RoundTrips) {
    struct Case { uint32_t lower; uint32_t upper; };
    Case cases[] = {
        {0, 0},
        {1, 0},
        {0, 1},
        {123, 456},
        {UINT32_MAX, UINT32_MAX},
    };
    for (auto [lower, upper] : cases) {
        auto packed = pack_uint32_pair(lower, upper);
        auto [got_lower, got_upper] = unpack_uint32_pair(packed);
        EXPECT_EQ(got_lower, lower);
        EXPECT_EQ(got_upper, upper);
    }
}

// ---------------------------------------------------------------------------
// Composite ID encode/decode
// ---------------------------------------------------------------------------
TEST(CompositeIDTest, EncodeDecodeRoundTrip) {
    struct Case { FileID fid; uint32_t local; };
    Case cases[] = {
        {1, 1},
        {100, 200},
        {UINT32_MAX, UINT32_MAX},
    };
    for (auto [fid, local] : cases) {
        auto encoded = encode_composite(fid, local);
        auto decoded = decode_composite(encoded);
        ASSERT_TRUE(decoded.has_value()) << "Failed for fid=" << fid << " local=" << local;
        EXPECT_EQ(decoded.value().file_id, fid);
        EXPECT_EQ(decoded.value().local_symbol_id, local);
    }
}

TEST(CompositeIDTest, ZeroPackReturnsEmpty) {
    auto encoded = encode_composite(0, 0);
    EXPECT_EQ(encoded, "");
}

TEST(CompositeIDTest, EmptyStringReturnsError) {
    auto result = decode_composite("");
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error(), CodecErrc::EmptyString);
}

// ---------------------------------------------------------------------------
// Pack/Unpack composite
// ---------------------------------------------------------------------------
TEST(PackCompositeTest, RoundTrips) {
    auto packed = pack_composite(42, 99);
    auto comp = unpack_composite(packed);
    EXPECT_EQ(comp.file_id, 42u);
    EXPECT_EQ(comp.local_symbol_id, 99u);
}

// ---------------------------------------------------------------------------
// Constexpr lookup table correctness
// ---------------------------------------------------------------------------
TEST(LookupTableTest, AllAlphabetCharsMapCorrectly) {
    for (uint8_t i = 0; i < kAlphabet63.size(); ++i) {
        auto c = static_cast<unsigned char>(kAlphabet63[i]);
        EXPECT_EQ(detail::kCharToValue[c], i)
            << "Mismatch at index " << static_cast<int>(i) << " char '" << kAlphabet63[i] << "'";
    }
}

TEST(LookupTableTest, NonAlphabetCharsAre255) {
    EXPECT_EQ(detail::kCharToValue[static_cast<size_t>(' ')], 255);
    EXPECT_EQ(detail::kCharToValue[static_cast<size_t>('!')], 255);
    EXPECT_EQ(detail::kCharToValue[static_cast<size_t>('@')], 255);
    EXPECT_EQ(detail::kCharToValue[static_cast<size_t>('\0')], 255);
}

// ---------------------------------------------------------------------------
// CodecErrc to_string
// ---------------------------------------------------------------------------
TEST(CodecErrcTest, ToStringCoversAllVariants) {
    EXPECT_EQ(to_string(CodecErrc::EmptyString), "empty encoded string");
    EXPECT_EQ(to_string(CodecErrc::InvalidChar), "invalid character in encoded string");
    EXPECT_EQ(to_string(CodecErrc::Overflow), "decoded value overflow");
}

}  // namespace
}  // namespace lci
