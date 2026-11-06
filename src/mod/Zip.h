#pragma once

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace addon::zip {

struct Entry {
    std::string name; // normalized, uses '/'
    std::uint16_t flags{};
    std::uint16_t method{};
    std::uint32_t crc{};
    std::uint64_t compressedSize{};
    std::uint64_t size{};
    std::uint64_t localOffset{};

    [[nodiscard]] bool isDirectory() const { return !name.empty() && name.back() == '/'; }
};

// Rejects absolute paths, "..", drive letters and NUL bytes (zip-slip protection).
bool isSafeRelativePath(std::string_view path);

// Minimal ZIP reader (stored + deflate) on top of zlib. No zip64, no encryption.
class Reader {
  public:
    bool open(const std::filesystem::path &path, std::string &error);
    [[nodiscard]] const std::vector<Entry> &entries() const { return mEntries; }

    bool readToMemory(const Entry &entry, std::string &out, std::uint64_t maxSize,
                      std::string &error);
    bool extractToFile(const Entry &entry, const std::filesystem::path &dest, std::string &error);

  private:
    using Sink = std::function<bool(const unsigned char *, std::size_t)>;
    bool extract(const Entry &entry, const Sink &sink, std::string &error);

    std::ifstream mFile;
    std::vector<Entry> mEntries;
};

} // namespace addon::zip
