#include <gtest/gtest.h>

#include <lci/core/file_service.h>
#include <lci/core/line_scanner.h>

#include <string>

namespace lci {
namespace {

// ---------------------------------------------------------------------------
// LineScanner - basic operations
// ---------------------------------------------------------------------------
TEST(LineScannerTest, EmptyContent) {
    LineScanner scanner("");
    EXPECT_EQ(scanner.line_count(), 0);
    EXPECT_TRUE(scanner.line(0).empty());
    EXPECT_TRUE(scanner.offsets().empty());
}

TEST(LineScannerTest, SingleLineNoNewline) {
    LineScanner scanner("hello");
    EXPECT_EQ(scanner.line_count(), 1);
    EXPECT_EQ(scanner.line(0), "hello");
    EXPECT_EQ(scanner.line_offset(0), 0u);
}

TEST(LineScannerTest, SingleLineWithNewline) {
    LineScanner scanner("hello\n");
    EXPECT_EQ(scanner.line_count(), 1);
    EXPECT_EQ(scanner.line(0), "hello");
}

TEST(LineScannerTest, MultipleLines) {
    LineScanner scanner("line1\nline2\nline3");
    EXPECT_EQ(scanner.line_count(), 3);
    EXPECT_EQ(scanner.line(0), "line1");
    EXPECT_EQ(scanner.line(1), "line2");
    EXPECT_EQ(scanner.line(2), "line3");
    EXPECT_EQ(scanner.line_offset(0), 0u);
    EXPECT_EQ(scanner.line_offset(1), 6u);
    EXPECT_EQ(scanner.line_offset(2), 12u);
}

TEST(LineScannerTest, MultipleLinesTrailingNewline) {
    LineScanner scanner("line1\nline2\nline3\n");
    EXPECT_EQ(scanner.line_count(), 3);
    EXPECT_EQ(scanner.line(0), "line1");
    EXPECT_EQ(scanner.line(1), "line2");
    EXPECT_EQ(scanner.line(2), "line3");
}

// ---------------------------------------------------------------------------
// LineScanner - CRLF handling
// ---------------------------------------------------------------------------
TEST(LineScannerTest, CRLFNormalization) {
    LineScanner scanner("line1\r\nline2\r\nline3");
    EXPECT_EQ(scanner.line_count(), 3);
    EXPECT_EQ(scanner.line(0), "line1");
    EXPECT_EQ(scanner.line(1), "line2");
    EXPECT_EQ(scanner.line(2), "line3");
}

TEST(LineScannerTest, CRLFOffsets) {
    LineScanner scanner("line1\r\nline2\r\nline3");
    ASSERT_EQ(scanner.offsets().size(), 3u);
    EXPECT_EQ(scanner.offsets()[0], 0u);
    EXPECT_EQ(scanner.offsets()[1], 7u);
    EXPECT_EQ(scanner.offsets()[2], 14u);
}

TEST(LineScannerTest, MixedLineEndings) {
    LineScanner scanner("line1\nline2\r\nline3\n");
    EXPECT_EQ(scanner.line_count(), 3);
    EXPECT_EQ(scanner.line(0), "line1");
    EXPECT_EQ(scanner.line(1), "line2");
    EXPECT_EQ(scanner.line(2), "line3");
}

TEST(LineScannerTest, CRLFTrailingNewline) {
    LineScanner scanner("line1\r\nline2\r\n");
    EXPECT_EQ(scanner.line_count(), 2);
    EXPECT_EQ(scanner.line(0), "line1");
    EXPECT_EQ(scanner.line(1), "line2");
}

// ---------------------------------------------------------------------------
// LineScanner - edge cases
// ---------------------------------------------------------------------------
TEST(LineScannerTest, EmptyLinesBetween) {
    LineScanner scanner("a\n\nb");
    EXPECT_EQ(scanner.line_count(), 3);
    EXPECT_EQ(scanner.line(0), "a");
    EXPECT_EQ(scanner.line(1), "");
    EXPECT_EQ(scanner.line(2), "b");
}

TEST(LineScannerTest, AllEmptyLines) {
    LineScanner scanner("\n\n\n");
    EXPECT_EQ(scanner.line_count(), 3);
    EXPECT_EQ(scanner.line(0), "");
    EXPECT_EQ(scanner.line(1), "");
    EXPECT_EQ(scanner.line(2), "");
}

TEST(LineScannerTest, SingleNewline) {
    LineScanner scanner("\n");
    EXPECT_EQ(scanner.line_count(), 1);
    EXPECT_EQ(scanner.line(0), "");
}

TEST(LineScannerTest, OutOfBoundsAccess) {
    LineScanner scanner("hello\nworld");
    EXPECT_TRUE(scanner.line(-1).empty());
    EXPECT_TRUE(scanner.line(2).empty());
    EXPECT_TRUE(scanner.line(100).empty());
    EXPECT_EQ(scanner.line_offset(-1), 0u);
    EXPECT_EQ(scanner.line_offset(100), 0u);
}

// ---------------------------------------------------------------------------
// LineScanner - O(1) random access
// ---------------------------------------------------------------------------
TEST(LineScannerTest, RandomAccessLargeFile) {
    std::string content;
    constexpr int kLineCount = 10000;
    for (int i = 0; i < kLineCount; ++i) {
        content += "line " + std::to_string(i) + "\n";
    }

    LineScanner scanner(content);
    EXPECT_EQ(scanner.line_count(), kLineCount);

    // Access lines out of order to verify O(1) random access.
    EXPECT_EQ(scanner.line(0), "line 0");
    EXPECT_EQ(scanner.line(9999), "line 9999");
    EXPECT_EQ(scanner.line(5000), "line 5000");
    EXPECT_EQ(scanner.line(1), "line 1");
}

// ---------------------------------------------------------------------------
// LineScanner - line_at_offset (binary search)
// ---------------------------------------------------------------------------
TEST(LineScannerTest, LineAtOffset) {
    LineScanner scanner("abc\ndef\nghi");
    // "abc" starts at 0, "def" at 4, "ghi" at 8
    EXPECT_EQ(scanner.line_at_offset(0), 1);
    EXPECT_EQ(scanner.line_at_offset(2), 1);
    EXPECT_EQ(scanner.line_at_offset(4), 2);
    EXPECT_EQ(scanner.line_at_offset(8), 3);
}

TEST(LineScannerTest, LineAtOffsetEmpty) {
    LineScanner scanner("");
    EXPECT_EQ(scanner.line_at_offset(0), 0);
}

// ---------------------------------------------------------------------------
// count_lines free function
// ---------------------------------------------------------------------------
TEST(CountLinesTest, EmptyContent) {
    EXPECT_EQ(count_lines(""), 0);
}

TEST(CountLinesTest, SingleLine) {
    EXPECT_EQ(count_lines("hello"), 1);
}

TEST(CountLinesTest, SingleLineWithNewline) {
    EXPECT_EQ(count_lines("hello\n"), 1);
}

TEST(CountLinesTest, MultipleLines) {
    EXPECT_EQ(count_lines("a\nb\nc"), 3);
}

TEST(CountLinesTest, MultipleLinesTrailingNewline) {
    EXPECT_EQ(count_lines("a\nb\nc\n"), 3);
}

TEST(CountLinesTest, JustNewline) {
    EXPECT_EQ(count_lines("\n"), 1);
}

TEST(CountLinesTest, MultipleNewlines) {
    EXPECT_EQ(count_lines("\n\n\n"), 3);
}

// ---------------------------------------------------------------------------
// LineScanner offsets match compute_line_offsets
// ---------------------------------------------------------------------------
TEST(LineScannerTest, OffsetsMatchComputeLineOffsets) {
    std::string_view content = "line1\nline2\nline3";
    LineScanner scanner(content);
    auto expected = compute_line_offsets(content);

    ASSERT_EQ(scanner.offsets().size(), expected.size());
    for (size_t i = 0; i < expected.size(); ++i) {
        EXPECT_EQ(scanner.offsets()[i], expected[i]);
    }
}

TEST(LineScannerTest, CRLFOffsetsMatchComputeLineOffsets) {
    std::string_view content = "line1\r\nline2\r\nline3";
    LineScanner scanner(content);
    auto expected = compute_line_offsets(content);

    ASSERT_EQ(scanner.offsets().size(), expected.size());
    for (size_t i = 0; i < expected.size(); ++i) {
        EXPECT_EQ(scanner.offsets()[i], expected[i]);
    }
}

// ---------------------------------------------------------------------------
// FileService - basic operations
// ---------------------------------------------------------------------------
TEST(FileServiceTest, DefaultConstruction) {
    FileService svc;
    EXPECT_EQ(svc.max_file_size_bytes(), FileService::kDefaultMaxFileSize);
}

TEST(FileServiceTest, LoadAndGetContent) {
    FileService svc;
    auto result = svc.load_file("test.cpp", "hello world");
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(svc.get_content(result.value()), "hello world");
}

TEST(FileServiceTest, MaxFileSizeEnforcement) {
    FileService svc(nullptr, 100);
    std::string large_content(200, 'x');

    auto result = svc.load_file("big.cpp", large_content);
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().type, ErrorType::FileTooLarge);
}

TEST(FileServiceTest, ExactMaxFileSizeAllowed) {
    FileService svc(nullptr, 100);
    std::string exact_content(100, 'x');

    auto result = svc.load_file("exact.cpp", exact_content);
    ASSERT_TRUE(result.has_value());
}

// ---------------------------------------------------------------------------
// FileService - line content access
// ---------------------------------------------------------------------------
TEST(FileServiceTest, GetLineContent) {
    FileService svc;
    auto result = svc.load_file("test.cpp", "line1\nline2\nline3");
    ASSERT_TRUE(result.has_value());
    FileID id = result.value();

    EXPECT_EQ(svc.get_line_content(id, 0), "line1");
    EXPECT_EQ(svc.get_line_content(id, 1), "line2");
    EXPECT_EQ(svc.get_line_content(id, 2), "line3");
}

TEST(FileServiceTest, GetLineContentCRLF) {
    FileService svc;
    auto result = svc.load_file("test.cpp", "line1\r\nline2\r\nline3");
    ASSERT_TRUE(result.has_value());
    FileID id = result.value();

    EXPECT_EQ(svc.get_line_content(id, 0), "line1");
    EXPECT_EQ(svc.get_line_content(id, 1), "line2");
    EXPECT_EQ(svc.get_line_content(id, 2), "line3");
}

TEST(FileServiceTest, GetLineContentOutOfBounds) {
    FileService svc;
    auto result = svc.load_file("test.cpp", "hello");
    ASSERT_TRUE(result.has_value());
    FileID id = result.value();

    EXPECT_TRUE(svc.get_line_content(id, -1).empty());
    EXPECT_TRUE(svc.get_line_content(id, 1).empty());
}

TEST(FileServiceTest, GetLineContentEmptyFile) {
    FileService svc;
    auto result = svc.load_file("empty.cpp", "");
    ASSERT_TRUE(result.has_value());
    FileID id = result.value();

    EXPECT_EQ(svc.get_line_count(id), 0);
    EXPECT_TRUE(svc.get_line_content(id, 0).empty());
}

TEST(FileServiceTest, GetLineContentSingleLine) {
    FileService svc;
    auto result = svc.load_file("single.cpp", "only line");
    ASSERT_TRUE(result.has_value());
    FileID id = result.value();

    EXPECT_EQ(svc.get_line_count(id), 1);
    EXPECT_EQ(svc.get_line_content(id, 0), "only line");
}

TEST(FileServiceTest, GetLineCount) {
    FileService svc;
    auto result = svc.load_file("test.cpp", "a\nb\nc");
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(svc.get_line_count(result.value()), 3);
}

TEST(FileServiceTest, GetLineCountNonExistentFile) {
    FileService svc;
    EXPECT_EQ(svc.get_line_count(999), 0);
}

TEST(FileServiceTest, GetContentNonExistentFile) {
    FileService svc;
    EXPECT_TRUE(svc.get_content(999).empty());
}

// ---------------------------------------------------------------------------
// FileService - shared store
// ---------------------------------------------------------------------------
TEST(FileServiceTest, SharedContentStore) {
    auto store = std::make_shared<FileContentStore>();
    FileService svc(store, 1024);

    auto result = svc.load_file("test.cpp", "content");
    ASSERT_TRUE(result.has_value());

    // Verify the store is shared.
    EXPECT_EQ(store->get_file_count(), 1);
    EXPECT_EQ(store->get_content(result.value()), "content");
}

}  // namespace
}  // namespace lci
