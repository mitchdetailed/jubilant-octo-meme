// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// Shared helpers of the esses_project tests: repository paths, file I/O, temporary directories and schema
// validation of what Esses writes (the vendored json-schema-validator, as esses-cli validate).
#pragma once

#include <catch2/catch_test_macros.hpp>
#include <nlohmann/json-schema.hpp>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <string_view>
#include <vector>

namespace esses::project::test {

inline std::filesystem::path repoRoot() {
    return std::filesystem::path(ESSES_TEST_REPO_ROOT);
}

inline std::filesystem::path dataFile(std::string_view relative) {
    return repoRoot() / "data" / std::filesystem::path(relative);
}

inline std::string readAll(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    REQUIRE(in.good());
    return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

inline void writeAll(const std::filesystem::path& path, std::string_view text) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    REQUIRE(out.good());
    out.write(text.data(), static_cast<std::streamsize>(text.size()));
}

// The *.json files of a directory, sorted.
inline std::vector<std::filesystem::path> jsonFiles(const std::filesystem::path& dir) {
    std::vector<std::filesystem::path> out;
    for (const auto& e : std::filesystem::directory_iterator(dir)) {
        if (e.is_regular_file() && e.path().extension() == ".json") {
            out.push_back(e.path());
        }
    }
    std::sort(out.begin(), out.end());
    return out;
}

// A fresh, empty directory under the system temp directory, removed on destruction.
class TempDir {
public:
    explicit TempDir(const std::string& tag) {
        static std::atomic<int> counter{0};
        path_ = std::filesystem::temp_directory_path() /
                ("esses-project-tests-" + tag + "-" +
                 std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + "-" +
                 std::to_string(++counter));
        std::filesystem::create_directories(path_);
    }
    ~TempDir() {
        std::error_code ec;
        std::filesystem::remove_all(path_, ec);
    }
    TempDir(const TempDir&) = delete;
    TempDir& operator=(const TempDir&) = delete;
    const std::filesystem::path& path() const { return path_; }

private:
    std::filesystem::path path_;
};

class CollectErrors : public nlohmann::json_schema::error_handler {
public:
    void error(const nlohmann::json::json_pointer& ptr, const nlohmann::json&,
               const std::string& message) override {
        errors.push_back(ptr.to_string() + ": " + message);
    }
    std::vector<std::string> errors;
};

// The errors of validating `text` against schemas/<schemaId>.schema.json (empty when valid).
inline std::vector<std::string> schemaErrors(std::string_view text, const std::string& schemaId) {
    const nlohmann::json schema =
        nlohmann::json::parse(readAll(repoRoot() / "schemas" / (schemaId + ".schema.json")));
    nlohmann::json_schema::json_validator validator;
    validator.set_root_schema(schema);
    CollectErrors collect;
    validator.validate(nlohmann::json::parse(text), collect);
    return collect.errors;
}

} // namespace esses::project::test
