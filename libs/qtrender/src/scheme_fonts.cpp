// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT

#include "font_cache.hpp"

#include <esses/qtrender/scheme_fonts.hpp>

#include <QFontDatabase>
#include <QGuiApplication>
#include <algorithm>
#include <cctype>
#include <mutex>
#include <set>
#include <string>
#include <system_error>
#include <vector>

namespace esses::qt {

namespace {

bool isFontFile(const std::filesystem::path& p) {
    std::string ext = p.extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return ext == ".ttf" || ext == ".otf";
}

} // namespace

SchemeFonts::SchemeFonts() : id_(detail::nextFontSetId()) {}

SchemeFonts::SchemeFonts(const project::Scheme& scheme, std::uint64_t generation)
    : scheme_(scheme), generation_(generation), id_(detail::nextFontSetId()) {}

QFont SchemeFonts::font(plot::TextStyleId style, bool rotated) const {
    return detail::makeFont(scheme_.font(detail::roleForStyle(style, rotated)));
}

QFont SchemeFonts::role(const QString& name) const {
    return detail::makeFont(scheme_.font(name.toStdString()));
}

std::string_view SchemeFonts::roleName(plot::TextStyleId style, bool rotated) noexcept {
    return detail::roleForStyle(style, rotated);
}

int SchemeFonts::registerApplicationFonts(const std::filesystem::path& dir) {
    if (qobject_cast<QGuiApplication*>(QCoreApplication::instance()) == nullptr) {
        return 0;
    }
    std::error_code ec;
    if (!std::filesystem::is_directory(dir, ec)) {
        return 0;
    }
    std::vector<std::filesystem::path> files;
    for (const auto& entry : std::filesystem::directory_iterator(dir, ec)) {
        if (entry.is_regular_file(ec) && isFontFile(entry.path())) {
            files.push_back(entry.path());
        }
    }
    std::sort(files.begin(), files.end());

    // QFontDatabase registers a file again on every call (a new id); remember what this application did.
    static std::mutex mutex;
    static std::set<std::filesystem::path> registered;
    static const QCoreApplication* registeredFor = nullptr;
    const std::scoped_lock lock(mutex);
    if (registeredFor != QCoreApplication::instance()) {
        registered.clear();
        registeredFor = QCoreApplication::instance();
    }
    int count = 0;
    for (const auto& f : files) {
        const std::filesystem::path canonical = std::filesystem::weakly_canonical(f, ec);
        if (registered.count(canonical) != 0) {
            ++count;
            continue;
        }
        if (QFontDatabase::addApplicationFont(QString::fromStdWString(f.wstring())) >= 0) {
            registered.insert(canonical);
            ++count;
        }
    }
    return count;
}

} // namespace esses::qt
