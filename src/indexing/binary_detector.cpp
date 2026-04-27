#include <lci/indexing/binary_detector.h>

#include <algorithm>
#include <cstddef>
#include <string>

namespace lci {

BinaryDetector::BinaryDetector() {
    // Font files
    binary_extensions_[".woff"] = true;
    binary_extensions_[".woff2"] = true;
    binary_extensions_[".ttf"] = true;
    binary_extensions_[".otf"] = true;
    binary_extensions_[".eot"] = true;

    // Image files
    binary_extensions_[".png"] = true;
    binary_extensions_[".jpg"] = true;
    binary_extensions_[".jpeg"] = true;
    binary_extensions_[".gif"] = true;
    binary_extensions_[".bmp"] = true;
    binary_extensions_[".ico"] = true;
    binary_extensions_[".webp"] = true;
    binary_extensions_[".tiff"] = true;
    binary_extensions_[".tif"] = true;

    // Archive files
    binary_extensions_[".zip"] = true;
    binary_extensions_[".tar"] = true;
    binary_extensions_[".gz"] = true;
    binary_extensions_[".bz2"] = true;
    binary_extensions_[".xz"] = true;
    binary_extensions_[".7z"] = true;
    binary_extensions_[".rar"] = true;
    binary_extensions_[".jar"] = true;
    binary_extensions_[".war"] = true;
    binary_extensions_[".ear"] = true;

    // Binary executables
    binary_extensions_[".exe"] = true;
    binary_extensions_[".dll"] = true;
    binary_extensions_[".so"] = true;
    binary_extensions_[".dylib"] = true;
    binary_extensions_[".a"] = true;
    binary_extensions_[".o"] = true;
    binary_extensions_[".obj"] = true;
    binary_extensions_[".bin"] = true;

    // Media files
    binary_extensions_[".mp3"] = true;
    binary_extensions_[".mp4"] = true;
    binary_extensions_[".avi"] = true;
    binary_extensions_[".mov"] = true;
    binary_extensions_[".wmv"] = true;
    binary_extensions_[".flv"] = true;
    binary_extensions_[".wav"] = true;
    binary_extensions_[".flac"] = true;
    binary_extensions_[".ogg"] = true;

    // Document files (binary formats)
    binary_extensions_[".pdf"] = true;
    binary_extensions_[".doc"] = true;
    binary_extensions_[".docx"] = true;
    binary_extensions_[".xls"] = true;
    binary_extensions_[".xlsx"] = true;
    binary_extensions_[".ppt"] = true;
    binary_extensions_[".pptx"] = true;

    // Database files
    binary_extensions_[".db"] = true;
    binary_extensions_[".sqlite"] = true;
    binary_extensions_[".sqlite3"] = true;

    // Compiled bytecode
    binary_extensions_[".pyc"] = true;
    binary_extensions_[".pyo"] = true;
    binary_extensions_[".class"] = true;
    binary_extensions_[".pickle"] = true;
    binary_extensions_[".pkl"] = true;
}

bool BinaryDetector::is_binary_by_extension(std::string_view path) const {
    auto dot = path.rfind('.');
    if (dot == std::string_view::npos) return false;

    // Handle .min.js / .min.css (text, not binary)
    if (path.size() > 7) {
        if (path.substr(path.size() - 7) == ".min.js") return false;
        if (path.size() > 8 && path.substr(path.size() - 8) == ".min.css")
            return false;
    }

    std::string ext(path.substr(dot));
    std::transform(ext.begin(), ext.end(), ext.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

    auto it = binary_extensions_.find(ext);
    return it != binary_extensions_.end() && it->second;
}

bool BinaryDetector::is_binary_by_magic_number(std::string_view content) const {
    if (content.empty()) return false;

    auto len = std::min(content.size(), size_t{512});
    auto sample = content.substr(0, len);
    auto data = reinterpret_cast<const uint8_t*>(sample.data());

    // Magic number signatures
    if (len >= 2 && data[0] == 0x1F && data[1] == 0x8B) return true;  // gzip
    if (len >= 4 && data[0] == 0x50 && data[1] == 0x4B &&
        (data[2] == 0x03 && data[3] == 0x04 ||
         data[2] == 0x05 && data[3] == 0x06)) return true;  // ZIP
    if (len >= 4 && data[0] == 0x89 && data[1] == 0x50 &&
        data[2] == 0x4E && data[3] == 0x47) return true;  // PNG
    if (len >= 3 && data[0] == 0xFF && data[1] == 0xD8 &&
        data[2] == 0xFF) return true;  // JPEG
    if (len >= 4 && data[0] == 0x47 && data[1] == 0x49 &&
        data[2] == 0x46 && data[3] == 0x38) return true;  // GIF
    if (len >= 4 && data[0] == 0x25 && data[1] == 0x50 &&
        data[2] == 0x44 && data[3] == 0x46) return true;  // PDF
    if (len >= 4 && data[0] == 0x7F && data[1] == 0x45 &&
        data[2] == 0x4C && data[3] == 0x46) return true;  // ELF
    if (len >= 2 && data[0] == 0x4D && data[1] == 0x5A) return true;  // MZ (DOS/PE)
    if (len >= 4 && data[0] == 0xCA && data[1] == 0xFE &&
        data[2] == 0xBA && data[3] == 0xBE) return true;  // Mach-O
    if (len >= 4 && data[0] == 0x77 && data[1] == 0x4F &&
        data[2] == 0x46 && (data[3] == 0x46 || data[3] == 0x32))
        return true;  // WOFF/WOFF2

    // Heuristic: null bytes and non-printable ratio
    int null_count = 0;
    int non_printable = 0;
    for (size_t i = 0; i < len; ++i) {
        if (data[i] == 0) ++null_count;
        if (data[i] < 0x20 && data[i] != 0x09 && data[i] != 0x0A &&
            data[i] != 0x0D)
            ++non_printable;
    }

    auto sample_len = static_cast<int>(len);
    if (null_count > sample_len / 100) return true;
    if (non_printable > sample_len * 30 / 100) return true;

    return false;
}

bool BinaryDetector::is_binary(std::string_view path,
                               std::string_view content) const {
    if (is_binary_by_extension(path)) return true;
    if (!content.empty()) return is_binary_by_magic_number(content);
    return false;
}

}  // namespace lci
