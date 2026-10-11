// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
#include <esses/model/notes.hpp>

#include <algorithm>

namespace esses::model {

std::string_view modelNoteLevelName(ModelNote::Level level) noexcept {
    switch (level) {
    case ModelNote::Level::Info:
        return "Info";
    case ModelNote::Level::Warning:
        return "Warning";
    case ModelNote::Level::Error:
        return "Error";
    }
    return "Info";
}

bool containsNote(const std::vector<ModelNote>& notes, std::string_view code) noexcept {
    return std::any_of(notes.begin(), notes.end(), [code](const ModelNote& n) { return n.code == code; });
}

std::size_t countNotes(const std::vector<ModelNote>& notes, std::string_view code) noexcept {
    return static_cast<std::size_t>(
        std::count_if(notes.begin(), notes.end(), [code](const ModelNote& n) { return n.code == code; }));
}

} // namespace esses::model
