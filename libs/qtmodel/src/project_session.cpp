// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// ProjectSession (06 sections 1, 4 and 18; 02 sections 6.1-6.2; D-019, D-070). M1: no undo yet.

#include <esses/base/utf8.hpp>
#include <esses/maths/corrected_speed_distance.hpp>
#include <esses/project/shipped_data.hpp>
#include <esses/qtmodel/project_session.hpp>
#include <esses/qtmodel/settings_store.hpp>

#include <algorithm>
#include <chrono>
#include <ctime>
#include <system_error>
#include <utility>

namespace esses::qt {

namespace {

constexpr std::string_view kProjectFile = "project.json";
constexpr std::string_view kWorkbookSuffix = ".workbook.json";

std::string nowRfc3339() {
    const std::time_t t = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
    std::tm utc{};
#if defined(_WIN32)
    gmtime_s(&utc, &t);
#else
    gmtime_r(&t, &utc);
#endif
    char buf[32];
    const std::size_t n = std::strftime(buf, sizeof buf, "%Y-%m-%dT%H:%M:%SZ", &utc);
    return std::string(buf, n);
}

std::string genericUtf8(const std::filesystem::path& p) {
    std::string s = pathToUtf8(p);
    std::replace(s.begin(), s.end(), '\\', '/');
    return s;
}

} // namespace

ProjectSession::ProjectSession(Paths paths, SettingsStore* settings, QObject* parent)
    : QObject(parent), paths_(std::move(paths)), settings_(settings),
      scheme_(std::make_shared<const project::Scheme>()) {}

ProjectSession::~ProjectSession() = default;

void ProjectSession::publishLog(std::vector<std::string> log) {
    log_ = std::move(log);
    for (const std::string& line : log_) {
        Q_EMIT logMessage(QString::fromStdString(line));
    }
}

Result<project::WorkbookFile> ProjectSession::loadShippedWorkbook(std::vector<std::string>& log) const {
    if (!profile_.defaultWorkbook.empty()) {
        const std::filesystem::path p = paths_.dataDir() / "workbooks" /
                                        pathFromUtf8(profile_.defaultWorkbook + std::string(kWorkbookSuffix));
        std::error_code ec;
        if (std::filesystem::exists(p, ec)) {
            return project::WorkbookFile::load(p, log);
        }
    }
    return project::WorkbookFile::load(project::shippedFile(paths_, project::kCircuitDefaultWorkbook), log);
}

Result<void> ProjectSession::loadDefaults() {
    std::vector<std::string> log;
    Result<project::Profile> profile =
        project::Profile::load(project::shippedFile(paths_, project::kCircuitProfile), log);
    if (!profile) {
        return Failure{profile.error()};
    }
    Result<project::Scheme> light =
        project::Scheme::load(project::shippedFile(paths_, project::kLightScheme), log);
    if (!light) {
        return Failure{light.error()};
    }
    Result<project::Scheme> dark =
        project::Scheme::load(project::shippedFile(paths_, project::kDarkScheme), log);
    if (!dark) {
        return Failure{dark.error()};
    }
    Result<project::Keymap> keymap =
        project::Keymap::load(project::shippedFile(paths_, project::kDefaultKeymap), log);
    if (!keymap) {
        return Failure{keymap.error()};
    }
    project::Keymap merged = std::move(keymap).value();
    // The user's keymap (<config>/keymap.json, inherits "default") on top of the shipped one (06
    // section 14.3).
    const std::filesystem::path userKeymap = paths_.configDir() / "keymap.json";
    std::error_code ec;
    if (!paths_.configDir().empty() && std::filesystem::exists(userKeymap, ec)) {
        if (Result<project::Keymap> user = project::Keymap::load(userKeymap, log)) {
            merged = project::Keymap::merged(merged, *user);
        } else {
            log.push_back("keymap.json ignored: " + user.error().toString());
        }
    }
    profile_ = std::move(profile).value();
    Result<project::WorkbookFile> workbook = loadShippedWorkbook(log);
    if (!workbook) {
        return Failure{workbook.error()};
    }
    light_ = std::make_shared<const project::Scheme>(std::move(light).value());
    dark_ = std::make_shared<const project::Scheme>(std::move(dark).value());
    keymap_ = std::move(merged);
    const QString active =
        settings_ != nullptr ? settings_->value(QStringLiteral("scheme.active")).toString() : QString();
    scheme_ = active == QString::fromStdString(dark_->name) ? dark_ : light_;
    defaultsLoaded_ = true;
    if (!folder_) {
        project_ =
            project::ProjectFile::makeDefault("Untitled", profile_.id.empty() ? "circuit" : profile_.id);
        workbookNames_ = {workbook->name};
        workbooks_ = {std::move(workbook).value()};
        activeWorkbook_ = 0;
    }
    publishLog(std::move(log));
    Q_EMIT schemeChanged();
    Q_EMIT documentsChanged();
    return ok();
}

Result<void> ProjectSession::openProject(const std::filesystem::path& where) {
    std::filesystem::path folder = where;
    std::error_code ec;
    if (std::filesystem::is_regular_file(folder, ec) && folder.filename() == pathFromUtf8(kProjectFile)) {
        folder = folder.parent_path();
    }
    std::vector<std::string> log;
    Result<project::ProjectFile> pf = project::ProjectFile::load(folder / pathFromUtf8(kProjectFile), log);
    if (!pf) {
        return Failure{pf.error()};
    }
    if (!defaultsLoaded_) {
        if (Result<void> d = loadDefaults(); !d) {
            return d;
        }
    }
    // The project's profile (data/profiles/<id>.profile.json), else the built-in Circuit profile.
    project::Profile profile = profile_;
    if (pf->profile != profile_.id) {
        const std::filesystem::path p =
            paths_.dataDir() / "profiles" / pathFromUtf8(pf->profile + ".profile.json");
        if (Result<project::Profile> loaded = project::Profile::load(p, log)) {
            profile = std::move(loaded).value();
        } else {
            log.push_back("profile '" + pf->profile + "' not found; using the Circuit profile");
            profile = project::Profile::circuitDefault();
        }
    }
    std::vector<project::WorkbookFile> workbooks;
    std::vector<std::string> names;
    for (const std::string& name : pf->workbooks) {
        const std::filesystem::path p =
            folder / "workbooks" / pathFromUtf8(name + std::string(kWorkbookSuffix));
        Result<project::WorkbookFile> wb = project::WorkbookFile::load(p, log);
        if (!wb) {
            log.push_back("workbook '" + name + "' skipped: " + wb.error().toString());
            continue;
        }
        workbooks.push_back(std::move(wb).value());
        names.push_back(name);
    }
    profile_ = std::move(profile);
    if (workbooks.empty()) {
        Result<project::WorkbookFile> shipped = loadShippedWorkbook(log);
        if (!shipped) {
            return Failure{shipped.error()};
        }
        log.push_back("the project has no readable workbook; the default workbook is shown");
        names.push_back(shipped->name);
        workbooks.push_back(std::move(shipped).value());
    }
    project_ = std::move(pf).value();
    workbooks_ = std::move(workbooks);
    workbookNames_ = std::move(names);
    activeWorkbook_ = 0;
    folder_ = folder;
    if (settings_ != nullptr) {
        settings_->addRecentProject(QString::fromStdString(pathToUtf8(folder)));
    }
    publishLog(std::move(log));
    Q_EMIT projectOpened();
    Q_EMIT documentsChanged();
    return ok();
}

Result<void> ProjectSession::newProject(const std::filesystem::path& folder, const std::string& name) {
    std::error_code ec;
    if (std::filesystem::exists(folder / pathFromUtf8(kProjectFile), ec)) {
        return fail(ErrorCode::Invalid, "a project already exists in " + pathToUtf8(folder));
    }
    if (!defaultsLoaded_) {
        if (Result<void> d = loadDefaults(); !d) {
            return d;
        }
    }
    for (const char* sub : {"workbooks", "channels", "maths", "trackmaps", "backup", ".local"}) {
        std::filesystem::create_directories(folder / sub, ec);
        if (ec) {
            return fail(ErrorCode::Io, "cannot create " + pathToUtf8(folder / sub) + ": " + ec.message());
        }
    }
    std::vector<std::string> log;
    Result<project::WorkbookFile> workbook = loadShippedWorkbook(log);
    if (!workbook) {
        return Failure{workbook.error()};
    }
    const std::string projectName = name.empty() ? pathToUtf8(folder.filename()) : name;
    project::ProjectFile pf =
        project::ProjectFile::makeDefault(projectName, profile_.id.empty() ? "circuit" : profile_.id);
    const std::string wbName = workbook->name.empty() ? std::string("Workbook") : workbook->name;
    if (Result<void> r =
            workbook->save(folder / "workbooks" / pathFromUtf8(wbName + std::string(kWorkbookSuffix)));
        !r) {
        return r;
    }
    pf.workbooks = {wbName};
    if (Result<void> r = pf.save(folder / pathFromUtf8(kProjectFile)); !r) {
        return r;
    }
    return openProject(folder);
}

Result<void> ProjectSession::save() {
    if (!folder_) {
        return fail(ErrorCode::Invalid, "no project is open");
    }
    Result<void> first = ok();
    // Workbooks first, so project.json never names a workbook that was not written.
    for (std::size_t i = 0; i < workbooks_.size(); ++i) {
        const std::string name = i < workbookNames_.size() ? workbookNames_[i] : workbooks_[i].name;
        const std::filesystem::path p =
            *folder_ / "workbooks" / pathFromUtf8(name + std::string(kWorkbookSuffix));
        const std::string text = workbooks_[i].toJson();
        Result<void> r = text.empty()
                             ? fail(ErrorCode::Invalid, "workbook '" + name + "' has a non-finite number")
                             : project::saveWithBackup(*folder_, p, text);
        if (!r && first) {
            first = r;
        }
    }
    project_.workbooks = workbookNames_;
    if (Result<void> r = project_.save(*folder_ / pathFromUtf8(kProjectFile)); !r && first) {
        first = r;
    }
    return first;
}

void ProjectSession::closeProject() {
    if (!folder_) {
        return;
    }
    folder_.reset();
    std::vector<std::string> log;
    if (Result<project::WorkbookFile> wb = loadShippedWorkbook(log)) {
        workbookNames_ = {wb->name};
        workbooks_ = {std::move(wb).value()};
    } else {
        workbooks_.clear();
        workbookNames_.clear();
    }
    project_ = project::ProjectFile::makeDefault("Untitled", profile_.id.empty() ? "circuit" : profile_.id);
    activeWorkbook_ = 0;
    Q_EMIT projectClosed();
    Q_EMIT documentsChanged();
}

void ProjectSession::setActiveWorkbook(int index) {
    if (index < 0 || static_cast<std::size_t>(index) >= workbooks_.size() || index == activeWorkbook_) {
        return;
    }
    activeWorkbook_ = index;
    Q_EMIT documentsChanged();
}

void ProjectSession::replaceWorkbook(int index, project::WorkbookFile workbook) {
    if (index >= 0 && static_cast<std::size_t>(index) < workbooks_.size()) {
        workbooks_[static_cast<std::size_t>(index)] = std::move(workbook);
        Q_EMIT documentsChanged();
    }
}

void ProjectSession::setScheme(std::shared_ptr<const project::Scheme> scheme) {
    if (scheme == nullptr || scheme == scheme_) {
        return;
    }
    scheme_ = std::move(scheme);
    if (settings_ != nullptr) {
        settings_->setValue(QStringLiteral("scheme.active"), QString::fromStdString(scheme_->name));
    }
    Q_EMIT schemeChanged();
}

std::filesystem::path ProjectSession::workbookPath(int index) const {
    if (index < 0 || static_cast<std::size_t>(index) >= workbooks_.size()) {
        return {};
    }
    if (!folder_) {
        return project::shippedFile(paths_, project::kCircuitDefaultWorkbook);
    }
    const auto i = static_cast<std::size_t>(index);
    const std::string name = i < workbookNames_.size() ? workbookNames_[i] : workbooks_[i].name;
    return *folder_ / "workbooks" / pathFromUtf8(name + std::string(kWorkbookSuffix));
}

void ProjectSession::setLastSelection(project::SelectionKeys keys) {
    project_.lastSelection = std::move(keys);
}

void ProjectSession::addRecentSource(const model::DataSource& source) {
    project::RecentSource r;
    std::filesystem::path path = source.prov.path;
    if (folder_) {
        std::error_code ec;
        const std::filesystem::path rel = std::filesystem::relative(path, *folder_, ec);
        if (!ec && !rel.empty()) {
            path = rel;
        }
    }
    r.path = genericUtf8(path);
    r.importer = source.prov.importerId;
    r.hash = "xxh3:" + source.id.hex();
    r.openedAt = nowRfc3339();
    auto& list = project_.recentSources;
    list.erase(std::remove_if(list.begin(), list.end(),
                              [&](const project::RecentSource& e) { return e.hash == r.hash; }),
               list.end());
    list.insert(list.begin(), std::move(r));
    if (settings_ != nullptr) {
        settings_->addRecentLog(QString::fromStdString(pathToUtf8(source.prov.path)));
    }
}

analysis::RuleRunner ProjectSession::ruleRunner(std::vector<std::string>* skipped) const {
    if (!profile_.rules.empty()) {
        Result<analysis::RuleRunner> r = analysis::RuleRunner::fromRuleIds(
            paths_.dataDir() / pathFromUtf8(project::kRulesDir), profile_.rules, skipped);
        if (r) {
            return std::move(r).value();
        }
    }
    return analysis::RuleRunner::circuitDefault();
}

model::OpaqueJson ProjectSession::correctedSpeedSettings() const {
    const auto it = project_.builtinMaths.find(maths::CorrectedSpeedDistanceModule::kId);
    return it == project_.builtinMaths.end() ? model::OpaqueJson{} : it->second.settings;
}

bool ProjectSession::correctedSpeedEnabled() const {
    const auto it = project_.builtinMaths.find(maths::CorrectedSpeedDistanceModule::kId);
    return it == project_.builtinMaths.end() || it->second.enabled;
}

} // namespace esses::qt
