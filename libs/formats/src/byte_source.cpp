// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
#include <esses/base/casefold.hpp>
#include <esses/base/utf8.hpp>
#include <esses/formats/byte_source.hpp>

#include <algorithm>
#include <string>
#include <system_error>
#include <utility>

namespace esses::formats {
namespace {

constexpr std::size_t kQuickWindow = std::size_t{1} << 20; // first and last MiB (as MappedFile's stamp)
constexpr std::size_t kContentBlock = std::size_t{8} << 20;

// Plain routines for MappedFile::guarded(): they own no object with a destructor.
struct HashJob {
    std::span<const std::byte> bytes;
    std::span<const std::byte> header;
    Hash128Stream* stream = nullptr;
};

void quickHashRoutine(void* ctx) {
    auto* job = static_cast<HashJob*>(ctx);
    const std::size_t window = std::min(kQuickWindow, job->bytes.size());
    job->stream->updateU64(static_cast<std::uint64_t>(job->bytes.size()));
    job->stream->update(job->bytes.first(window));
    job->stream->update(job->bytes.last(window));
    if (!job->header.empty()) {
        job->stream->updateU64(static_cast<std::uint64_t>(job->header.size()));
        job->stream->update(job->header);
    }
}

void blockHashRoutine(void* ctx) {
    auto* job = static_cast<HashJob*>(ctx);
    job->stream->update(job->bytes);
}

std::span<const std::byte> clampSpan(std::span<const std::byte> all, std::uint64_t offset,
                                     std::uint64_t len) {
    if (offset >= all.size()) {
        return {};
    }
    const std::uint64_t n = std::min<std::uint64_t>(len, all.size() - offset);
    return all.subspan(static_cast<std::size_t>(offset), static_cast<std::size_t>(n));
}

bool sameName(std::string_view a, std::string_view b) {
#if defined(ESSES_OS_WINDOWS)
    return ciEquals(a, b);
#else
    return a == b;
#endif
}

class FileSource final : public ByteSource {
public:
    explicit FileSource(std::shared_ptr<const MappedFile> file) : file_(std::move(file)) {}

    const std::filesystem::path& path() const override { return file_->path(); }
    std::uint64_t size() const override { return file_->size(); }
    std::span<const std::byte> span(std::uint64_t offset, std::uint64_t len) const override {
        return clampSpan(file_->bytes(), offset, len);
    }
    Hash128 quickHash(std::span<const std::byte> formatHeader) const override {
        if (formatHeader.empty()) {
            return file_->stampAtOpen().quick; // the same algorithm without a header
        }
        Hash128Stream stream;
        HashJob job{file_->bytes(), formatHeader, &stream};
        if (!file_->guarded(&quickHashRoutine, &job)) {
            return quickHashOf({},
                               formatHeader); // the mapping faulted; the open fails on its own read anyway
        }
        return stream.digest();
    }
    Result<Hash128> contentHash(CancelToken cancel) const override {
        Hash128Stream stream;
        const std::span<const std::byte> all = file_->bytes();
        for (std::size_t off = 0; off < all.size(); off += kContentBlock) {
            if (cancel.cancelled()) {
                return fail(ErrorCode::Cancelled, "content hash cancelled");
            }
            HashJob job{all.subspan(off, std::min(kContentBlock, all.size() - off)), {}, &stream};
            if (auto r = file_->guarded(&blockHashRoutine, &job); !r) {
                return Failure{r.error()};
            }
        }
        return stream.digest();
    }
    std::vector<std::filesystem::path> siblings() const override { return listSiblings(file_->path()); }
    std::int64_t mtimeUnixUs() const override { return file_->stampAtOpen().mtimeUnixUs; }
    std::shared_ptr<const MappedFile> mappedFile() const override { return file_; }

private:
    std::shared_ptr<const MappedFile> file_;
};

class MemorySource final : public ByteSource {
public:
    MemorySource(std::shared_ptr<const std::vector<std::byte>> bytes, std::filesystem::path path,
                 std::vector<std::filesystem::path> siblings)
        : bytes_(std::move(bytes)), path_(std::move(path)), siblings_(std::move(siblings)) {}

    const std::filesystem::path& path() const override { return path_; }
    std::uint64_t size() const override { return bytes_->size(); }
    std::span<const std::byte> span(std::uint64_t offset, std::uint64_t len) const override {
        return clampSpan(*bytes_, offset, len);
    }
    Hash128 quickHash(std::span<const std::byte> formatHeader) const override {
        return quickHashOf(*bytes_, formatHeader);
    }
    Result<Hash128> contentHash(CancelToken cancel) const override {
        if (cancel.cancelled()) {
            return fail(ErrorCode::Cancelled, "content hash cancelled");
        }
        return hash128(std::span<const std::byte>(*bytes_));
    }
    std::vector<std::filesystem::path> siblings() const override { return siblings_; }
    std::shared_ptr<const std::vector<std::byte>> buffer() const override { return bytes_; }

private:
    std::shared_ptr<const std::vector<std::byte>> bytes_;
    std::filesystem::path path_;
    std::vector<std::filesystem::path> siblings_;
};

} // namespace

Result<std::shared_ptr<const ByteSource>> openByteSource(const std::filesystem::path& path) {
    auto mapped = MappedFile::open(path);
    if (!mapped) {
        return Failure{mapped.error()};
    }
    auto file = std::make_shared<const MappedFile>(std::move(mapped).value());
    return std::shared_ptr<const ByteSource>(std::make_shared<FileSource>(std::move(file)));
}

std::shared_ptr<const ByteSource> makeMemorySource(std::vector<std::byte> bytes, std::filesystem::path path,
                                                   std::vector<std::filesystem::path> siblings) {
    return std::make_shared<MemorySource>(std::make_shared<const std::vector<std::byte>>(std::move(bytes)),
                                          std::move(path), std::move(siblings));
}

std::vector<std::filesystem::path> listSiblings(const std::filesystem::path& file) {
    std::vector<std::filesystem::path> out;
    if (file.empty()) {
        return out;
    }
    const std::filesystem::path dir =
        file.has_parent_path() ? file.parent_path() : std::filesystem::path(".");
    const std::string stem = pathToUtf8(file.stem());
    const std::string name = pathToUtf8(file.filename());
    std::error_code ec;
    std::filesystem::directory_iterator it(dir, ec);
    if (ec) {
        return out;
    }
    for (const std::filesystem::directory_iterator end; it != end; it.increment(ec)) {
        if (ec) {
            break;
        }
        const std::filesystem::path& p = it->path();
        std::error_code fec;
        if (!it->is_regular_file(fec)) {
            continue;
        }
        if (sameName(pathToUtf8(p.stem()), stem) && !sameName(pathToUtf8(p.filename()), name)) {
            out.push_back(p);
        }
    }
    std::sort(out.begin(), out.end(), [](const std::filesystem::path& a, const std::filesystem::path& b) {
        return pathToUtf8(a.filename()) < pathToUtf8(b.filename());
    });
    return out;
}

std::optional<std::filesystem::path> findCompanion(std::span<const std::filesystem::path> siblings,
                                                   const std::filesystem::path& file,
                                                   std::string_view extension) {
    const std::string wanted = pathToUtf8(file.stem()) + std::string(extension);
    for (const std::filesystem::path& s : siblings) {
        if (sameName(pathToUtf8(s.filename()), wanted)) {
            return s;
        }
    }
    return std::nullopt;
}

Hash128 quickHashOf(std::span<const std::byte> bytes, std::span<const std::byte> formatHeader) {
    Hash128Stream stream;
    HashJob job{bytes, formatHeader, &stream};
    quickHashRoutine(&job);
    return stream.digest();
}

} // namespace esses::formats
