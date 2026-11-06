#include "mod/Zip.h"

#include <algorithm>
#include <zlib.h>

namespace addon::zip {

namespace {

std::uint16_t rd16(const unsigned char *p) {
    return static_cast<std::uint16_t>(p[0] | (p[1] << 8));
}

std::uint32_t rd32(const unsigned char *p) {
    return static_cast<std::uint32_t>(p[0]) | (static_cast<std::uint32_t>(p[1]) << 8) |
           (static_cast<std::uint32_t>(p[2]) << 16) | (static_cast<std::uint32_t>(p[3]) << 24);
}

} // namespace

bool isSafeRelativePath(std::string_view path) {
    if (path.empty() || path.front() == '/')
        return false;
    if (path.find('\0') != std::string_view::npos || path.find(':') != std::string_view::npos)
        return false;

    std::size_t start = 0;
    while (start <= path.size()) {
        std::size_t end = path.find('/', start);
        if (end == std::string_view::npos)
            end = path.size();
        if (path.substr(start, end - start) == "..")
            return false;
        start = end + 1;
    }
    return true;
}

bool Reader::open(const std::filesystem::path &path, std::string &error) {
    mEntries.clear();
    mFile.close();
    mFile.clear();
    mFile.open(path, std::ios::binary);
    if (!mFile) {
        error = "no se pudo abrir el archivo";
        return false;
    }

    mFile.seekg(0, std::ios::end);
    const std::streamoff fileSize = mFile.tellg();
    if (fileSize < 22) {
        error = "no es un archivo zip";
        return false;
    }

    const std::streamoff tailSize = std::min<std::streamoff>(fileSize, 22 + 65535);
    std::vector<unsigned char> tail(static_cast<std::size_t>(tailSize));
    mFile.seekg(fileSize - tailSize);
    mFile.read(reinterpret_cast<char *>(tail.data()), tailSize);
    if (!mFile) {
        error = "error leyendo el zip";
        return false;
    }

    std::streamoff eocd = -1;
    for (std::streamoff i = tailSize - 22; i >= 0; --i) {
        if (rd32(&tail[static_cast<std::size_t>(i)]) == 0x06054b50u) {
            eocd = i;
            break;
        }
    }
    if (eocd < 0) {
        error = "no es un archivo zip (sin directorio central)";
        return false;
    }

    const unsigned char *e = &tail[static_cast<std::size_t>(eocd)];
    const std::uint32_t total = rd16(e + 10);
    const std::uint32_t cdSize = rd32(e + 12);
    const std::uint32_t cdOffset = rd32(e + 16);
    if (total == 0xFFFFu || cdSize == 0xFFFFFFFFu || cdOffset == 0xFFFFFFFFu) {
        error = "zip64 no soportado";
        return false;
    }
    if (static_cast<std::uint64_t>(cdOffset) + cdSize > static_cast<std::uint64_t>(fileSize)) {
        error = "directorio central corrupto";
        return false;
    }

    std::vector<unsigned char> cd(cdSize);
    mFile.clear();
    mFile.seekg(cdOffset);
    mFile.read(reinterpret_cast<char *>(cd.data()), cdSize);
    if (!mFile) {
        error = "error leyendo el directorio central";
        return false;
    }

    std::size_t pos = 0;
    for (std::uint32_t i = 0; i < total; ++i) {
        if (pos + 46 > cd.size() || rd32(&cd[pos]) != 0x02014b50u) {
            error = "directorio central corrupto";
            return false;
        }
        const unsigned char *p = &cd[pos];
        const std::uint16_t nameLen = rd16(p + 28);
        const std::uint16_t extraLen = rd16(p + 30);
        const std::uint16_t commentLen = rd16(p + 32);
        if (pos + 46 + nameLen + extraLen + commentLen > cd.size()) {
            error = "directorio central corrupto";
            return false;
        }

        Entry entry;
        entry.flags = rd16(p + 8);
        entry.method = rd16(p + 10);
        entry.crc = rd32(p + 16);
        entry.compressedSize = rd32(p + 20);
        entry.size = rd32(p + 24);
        entry.localOffset = rd32(p + 42);
        entry.name.assign(reinterpret_cast<const char *>(p + 46), nameLen);
        std::replace(entry.name.begin(), entry.name.end(), '\\', '/');

        if (entry.compressedSize == 0xFFFFFFFFu || entry.size == 0xFFFFFFFFu ||
            entry.localOffset == 0xFFFFFFFFu) {
            error = "zip64 no soportado";
            return false;
        }

        mEntries.push_back(std::move(entry));
        pos += 46u + nameLen + extraLen + commentLen;
    }
    return true;
}

bool Reader::extract(const Entry &entry, const Sink &sink, std::string &error) {
    if (entry.flags & 1u) {
        error = "entrada cifrada no soportada";
        return false;
    }
    if (entry.method != 0 && entry.method != 8) {
        error = "metodo de compresion no soportado";
        return false;
    }

    unsigned char header[30];
    mFile.clear();
    mFile.seekg(static_cast<std::streamoff>(entry.localOffset));
    mFile.read(reinterpret_cast<char *>(header), sizeof(header));
    if (!mFile || rd32(header) != 0x04034b50u) {
        error = "cabecera local corrupta";
        return false;
    }
    const std::uint64_t dataStart = entry.localOffset + 30u + rd16(header + 26) + rd16(header + 28);
    mFile.seekg(static_cast<std::streamoff>(dataStart));

    constexpr std::size_t kChunk = 64 * 1024;
    std::vector<unsigned char> in(kChunk);
    std::vector<unsigned char> out(kChunk);
    std::uint64_t remaining = entry.compressedSize;
    std::uint64_t produced = 0;
    uLong crc = crc32(0L, Z_NULL, 0);

    auto readChunk = [&](std::size_t &got) {
        got = static_cast<std::size_t>(std::min<std::uint64_t>(remaining, in.size()));
        mFile.read(reinterpret_cast<char *>(in.data()), static_cast<std::streamsize>(got));
        if (static_cast<std::size_t>(mFile.gcount()) != got)
            return false;
        remaining -= got;
        return true;
    };

    if (entry.method == 0) {
        while (remaining > 0) {
            std::size_t got = 0;
            if (!readChunk(got)) {
                error = "zip truncado";
                return false;
            }
            crc = crc32(crc, in.data(), static_cast<uInt>(got));
            produced += got;
            if (produced > entry.size || !sink(in.data(), got)) {
                error = "error escribiendo o tamano inconsistente";
                return false;
            }
        }
    } else {
        z_stream zs{};
        if (inflateInit2(&zs, -MAX_WBITS) != Z_OK) {
            error = "no se pudo iniciar zlib";
            return false;
        }
        int ret = Z_OK;
        while (ret != Z_STREAM_END) {
            if (zs.avail_in == 0) {
                if (remaining == 0)
                    break;
                std::size_t got = 0;
                if (!readChunk(got)) {
                    inflateEnd(&zs);
                    error = "zip truncado";
                    return false;
                }
                zs.next_in = in.data();
                zs.avail_in = static_cast<uInt>(got);
            }
            do {
                zs.next_out = out.data();
                zs.avail_out = static_cast<uInt>(out.size());
                ret = inflate(&zs, Z_NO_FLUSH);
                if (ret != Z_OK && ret != Z_STREAM_END && ret != Z_BUF_ERROR) {
                    inflateEnd(&zs);
                    error = "datos comprimidos corruptos";
                    return false;
                }
                const std::size_t have = out.size() - zs.avail_out;
                if (have > 0) {
                    crc = crc32(crc, out.data(), static_cast<uInt>(have));
                    produced += have;
                    if (produced > entry.size || !sink(out.data(), have)) {
                        inflateEnd(&zs);
                        error = "error escribiendo o tamano inconsistente";
                        return false;
                    }
                }
            } while (zs.avail_out == 0 && ret != Z_STREAM_END);
        }
        inflateEnd(&zs);
        if (ret != Z_STREAM_END) {
            error = "datos comprimidos incompletos";
            return false;
        }
    }

    if (produced != entry.size || static_cast<std::uint32_t>(crc) != entry.crc) {
        error = "CRC invalido";
        return false;
    }
    return true;
}

bool Reader::readToMemory(const Entry &entry, std::string &out, std::uint64_t maxSize,
                          std::string &error) {
    if (entry.size > maxSize) {
        error = "entrada demasiado grande";
        return false;
    }
    out.clear();
    out.reserve(static_cast<std::size_t>(entry.size));
    return extract(
        entry,
        [&](const unsigned char *data, std::size_t len) {
            out.append(reinterpret_cast<const char *>(data), len);
            return true;
        },
        error);
}

bool Reader::extractToFile(const Entry &entry, const std::filesystem::path &dest,
                           std::string &error) {
    std::error_code ec;
    std::filesystem::create_directories(dest.parent_path(), ec);
    if (ec) {
        error = "no se pudo crear la carpeta: " + ec.message();
        return false;
    }
    std::ofstream out(dest, std::ios::binary | std::ios::trunc);
    if (!out) {
        error = "no se pudo escribir " + dest.string();
        return false;
    }
    const bool ok = extract(
        entry,
        [&](const unsigned char *data, std::size_t len) {
            out.write(reinterpret_cast<const char *>(data), static_cast<std::streamsize>(len));
            return static_cast<bool>(out);
        },
        error);
    out.close();
    if (!ok) {
        std::filesystem::remove(dest, ec);
        return false;
    }
    return true;
}

} // namespace addon::zip
