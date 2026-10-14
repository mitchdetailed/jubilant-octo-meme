// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// ProjectSession (06 sections 1, 4 and 18; 02 sections 6.1-6.6; D-019, D-070): the shipped defaults, opening,
// creating, saving and closing a project folder with every project document. The document setters, the view
// state written without the undo stack and the journal are in src/undo/project_session_m2.cpp.

#include <esses/base/utf8.hpp>
#include <esses/maths/corrected_speed_distance.hpp>
#include <esses/maths/math_module_io.hpp>
#include <esses/project/shipped_data.hpp>
#include <esses/qtmodel/project_session.hpp>
#include <esses/qtmodel/settings_store.hpp>

#include <QTimer>
#include <algorithm>
#include <chrono>
#include <ctime>
#include <fstream>
#include <functional>
#include <iterator>
#include <optional>
#include <set>
#include <string_view>
#include <system_error>
#include <utility>

namespace esses::qt {

namespace {

constexpr std::string_view kProjectFile = "project.json";
constexpr std::string_view kWorkbookSuffix = ".workbook.json";
constexpr std::string_view kMathsSuffix = ".maths.json";
constexpr std::string_view kOverridesPath = "channels/overrides.json";
constexpr std::string_view kAliasesPath = "channels/aliases.json";
constexpr std::string_view kMappingsPath = "channels/mappings.json";
constexpr std::string_view kUnitMapsDir = "channels/unitmaps/";

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

std::string backupStamp() {
    const std::time_t t = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
    std::tm utc{};
#if defined(_WIN32)
    gmtime_s(&utc, &t);
#else
    gmtime_r(&t, &utc);
#endif
    char buf[32];
    const std::size_t n = std::strftime(buf, sizeof buf, "%Y%m%d-%H%M%S", &utc);
    return std::string(buf, n);
}

std::string genericUtf8(const std::filesystem::path& p) {
    std::string s = pathToUtf8(p);
    std::replace(s.begin(), s.end(), '\\', '/');
    return s;
}

std::string workbookPathOf(const std::string& name) {
    return "workbooks/" + name + std::string(kWorkbookSuffix);
}

std::string modulePathOf(const std::string& id) {
    return "maths/" + id + std::string(kMathsSuffix);
}

std::string unitMapPathOf(const std::string& stem) {
    return std::string(kUnitMapsDir) + stem + ".json";
}

// The bytes of a file, nullopt when it does not exist; Io when it exists and cannot be read.
Result<std::optional<std::string>> readBytes(const std::filesystem::path& path) {
    std::error_code ec;
    if (!std::filesystem::is_regular_file(path, ec)) {
        return std::optional<std::string>{};
    }
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        ErrorContext ctx;
        ctx.path = path;
        return fail(ErrorCode::Io, "cannot read " + pathToUtf8(path), std::move(ctx));
    }
    std::string bytes;
    try {
        bytes.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    } catch (const std::ios_base::failure&) {
        // libstdc++'s filebuf throws a failed read (a directory: EISDIR) whatever the exception mask
        in.setstate(std::ios_base::badbit);
    }
    if (in.bad()) {
        ErrorContext ctx;
        ctx.path = path;
        return fail(ErrorCode::Io, "cannot read " + pathToUtf8(path), std::move(ctx));
    }
    return std::optional<std::string>(std::move(bytes));
}

// A file the project no longer names (a removed or renamed workbook, unit map or module): its content is
// copied to backup/<name>.<yyyymmdd-hhmmss>.json (D-070) and the file removed.
Result<void> retireFile(const std::filesystem::path& folder, const std::string& rel) {
    const std::filesystem::path file = folder / pathFromUtf8(rel);
    std::error_code ec;
    if (!std::filesystem::is_regular_file(file, ec)) {
        return ok();
    }
    std::filesystem::create_directories(folder / "backup", ec);
    std::string stem = pathToUtf8(file.filename());
    if (stem.size() > 5 && stem.ends_with(".json")) {
        stem.resize(stem.size() - 5);
    }
    const std::string stamp = backupStamp();
    std::filesystem::path target = folder / "backup" / pathFromUtf8(stem + "." + stamp + ".json");
    for (int n = 2; std::filesystem::exists(target, ec); ++n) {
        target = folder / "backup" / pathFromUtf8(stem + "." + stamp + "-" + std::to_string(n) + ".json");
    }
    std::filesystem::copy_file(file, target, ec);
    if (ec) {
        return fail(ErrorCode::Io, "cannot back up " + pathToUtf8(file) + ": " + ec.message());
    }
    std::filesystem::remove(file, ec);
    if (ec) {
        return fail(ErrorCode::Io, "cannot remove " + pathToUtf8(file) + ": " + ec.message());
    }
    return ok();
}

bool retirable(std::string_view rel) {
    return rel.starts_with("workbooks/") || rel.starts_with("maths/") || rel.starts_with(kUnitMapsDir);
}

} // namespace

struct ProjectSession::Loaded {
    project::ProjectFile project;
    project::Profile profile;
    std::vector<project::WorkbookFile> workbooks;
    std::vector<std::string> names;
    project::ChannelOverrides overrides;
    project::Aliases aliases;
    project::Mappings mappings;
    std::map<std::string, formats::UnitMappingProfile, std::less<>> unitMaps;
    std::vector<maths::MathModule> modules;
    std::map<std::string, Baseline, std::less<>> baseline;
};

ProjectSession::ProjectSession(Paths paths, SettingsStore* settings, QObject* parent)
    : QObject(parent), paths_(std::move(paths)), settings_(settings),
      scheme_(std::make_shared<const project::Scheme>()), autosaveTimer_(new QTimer(this)) {
    connect(autosaveTimer_, &QTimer::timeout, this, [this] {
        if (const Result<void> r = autosave(); !r) {
            Q_EMIT logMessage(tr("Autosave failed: %1").arg(QString::fromStdString(r.error().toString())));
        }
    });
    // The interval follows options.general.autosaveMinutes (an options edit restarts it only when it
    // changed, so frequent edits never postpone the next autosave).
    connect(this, &ProjectSession::documentsChanged, this, [this] { restartAutosaveTimer(); });
    // Undoing back to the saved state is clean again (unless the documents came from a journal or a
    // migration, which only a save makes clean).
    connect(&undoStack_, &QUndoStack::cleanChanged, this, [this](bool clean) {
        if (clean && !recovered_) {
            setDirty(false);
        }
    });
}

ProjectSession::~ProjectSession() = default;

void ProjectSession::publishLog(std::vector<std::string> log) {
    log_ = std::move(log);
    for (const std::string& line : log_) {
        Q_EMIT logMessage(QString::fromStdString(line));
    }
}

Result<project::WorkbookFile> ProjectSession::loadShippedWorkbook(std::vector<std::string>& log,
                                                                  const project::Profile* profile) const {
    const project::Profile& prof = profile != nullptr ? *profile : profile_;
    if (!prof.defaultWorkbook.empty()) {
        const std::filesystem::path p = paths_.dataDir() / "workbooks" /
                                        pathFromUtf8(prof.defaultWorkbook + std::string(kWorkbookSuffix));
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

// ---- loading
// ------------------------------------------------------------------------------------------------

Result<ProjectSession::Loaded>
ProjectSession::loadDocuments(const std::filesystem::path& folder,
                              const std::map<std::string, std::string, std::less<>>* overlay,
                              std::vector<std::string>& log) const {
    Loaded out;
    // The text of a document: the journal's entry when it has one, else the file. The baseline is always the
    // file (a recovered document differs from it, so it stays dirty until saved).
    struct Text {
        std::optional<std::string> text;
        bool fromJournal = false;
    };
    const auto read = [&](const std::string& rel) -> Text {
        Baseline b;
        Result<std::optional<std::string>> disk = readBytes(folder / pathFromUtf8(rel));
        if (!disk) {
            log.push_back(rel + ": " + disk.error().toString());
            b.failed = true;
            out.baseline.insert_or_assign(rel, b);
            return {};
        }
        if (*disk) {
            b.disk = hash128(**disk);
        }
        out.baseline.insert_or_assign(rel, b);
        if (overlay != nullptr) {
            if (const auto it = overlay->find(rel); it != overlay->end()) {
                return Text{it->second, true};
            }
        }
        return Text{std::move(*disk), false};
    };
    // Records the canonical text of what was read as the document's clean state. A journal entry stays dirty
    // (its text hash is never the file's), and so does a document the reader migrated or completed (02
    // section 6.6 rule 2: the next save rewrites it, with the D-070 backup of the original).
    const auto settle = [&](const std::string& rel, const Text& t, const std::string& canonical,
                            bool migrated = false) {
        Baseline& b = out.baseline[rel];
        b.text = t.fromJournal || migrated ? Hash128{} : hash128(canonical);
    };
    const auto failed = [&](const std::string& rel, const Text& t, const Error& e) {
        log.push_back(rel + " not read: " + e.toString());
        if (!t.fromJournal) {
            out.baseline[rel].failed = true; // never written over (the user's file is kept as it is)
        }
    };

    // project.json (required).
    {
        const std::string rel(kProjectFile);
        const Text t = read(rel);
        if (!t.text) {
            ErrorContext ctx;
            ctx.path = folder / pathFromUtf8(kProjectFile);
            return fail(out.baseline[rel].failed ? ErrorCode::Io : ErrorCode::NotFound,
                        "no project.json in " + pathToUtf8(folder), std::move(ctx));
        }
        Result<project::ProjectFile> pf = project::ProjectFile::parse(*t.text, log);
        if (!pf) {
            Error e = pf.error();
            e.context.path = folder / pathFromUtf8(kProjectFile);
            return Failure{std::move(e)};
        }
        out.project = std::move(pf).value();
        settle(rel, t, out.project.toJson(), out.project.dirty);
    }

    // The project's profile (data/profiles/<id>.profile.json), else the built-in Circuit profile.
    out.profile = profile_;
    if (out.project.profile != profile_.id) {
        const std::filesystem::path p =
            paths_.dataDir() / "profiles" / pathFromUtf8(out.project.profile + ".profile.json");
        if (Result<project::Profile> loaded = project::Profile::load(p, log)) {
            out.profile = std::move(loaded).value();
        } else {
            log.push_back("profile '" + out.project.profile + "' not found; using the Circuit profile");
            out.profile = project::Profile::circuitDefault();
        }
    }

    // Workbooks in project order; an unreadable one is skipped.
    for (const std::string& name : out.project.workbooks) {
        const std::string rel = workbookPathOf(name);
        const Text t = read(rel);
        if (!t.text) {
            if (!out.baseline[rel].failed) {
                out.baseline.erase(rel);
            }
            log.push_back("workbook '" + name + "' skipped: no " + rel);
            continue;
        }
        Result<project::WorkbookFile> wb =
            project::WorkbookFile::parse(*t.text, name + std::string(kWorkbookSuffix), log);
        if (!wb) {
            log.push_back("workbook '" + name + "' skipped: " + wb.error().toString());
            if (!t.fromJournal) {
                out.baseline[rel].failed = true;
            }
            continue;
        }
        settle(rel, t, wb->toJson(), wb->dirty);
        out.workbooks.push_back(std::move(wb).value());
        out.names.push_back(name);
    }
    if (out.workbooks.empty()) {
        // The profile's default workbook (a shipped file, written into the project by the next save).
        Result<project::WorkbookFile> shipped = loadShippedWorkbook(log, &out.profile);
        if (!shipped) {
            return Failure{shipped.error()};
        }
        log.push_back("the project has no readable workbook; the default workbook is shown");
        out.names.push_back(shipped->name);
        out.workbooks.push_back(std::move(shipped).value());
    }

    // Channel documents: an absent file is an empty document (not written until edited).
    const auto channelDoc = [&](std::string_view relView, auto& doc, auto parse) {
        const std::string rel(relView);
        const Text t = read(rel);
        if (t.text) {
            auto parsed = parse(*t.text);
            if (parsed) {
                doc = std::move(parsed).value();
                settle(rel, t, doc.toJson());
                return;
            }
            failed(rel, t, parsed.error());
        }
        Baseline& b = out.baseline[rel];
        if (!b.failed) {
            b.text = hash128(doc.toJson());
        }
    };
    channelDoc(kOverridesPath, out.overrides,
               [&](std::string_view s) { return project::ChannelOverrides::parse(s, log); });
    channelDoc(kAliasesPath, out.aliases,
               [&](std::string_view s) { return project::Aliases::parse(s, log); });
    channelDoc(kMappingsPath, out.mappings,
               [&](std::string_view s) { return project::Mappings::parse(s, log); });

    // Unit maps: every channels/unitmaps/*.json (and the journal's).
    std::set<std::string> unitMapPaths;
    {
        std::error_code ec;
        for (const auto& e : std::filesystem::directory_iterator(folder / "channels" / "unitmaps", ec)) {
            if (e.is_regular_file(ec) && e.path().extension() == ".json") {
                unitMapPaths.insert(unitMapPathOf(pathToUtf8(e.path().stem())));
            }
        }
        if (overlay != nullptr) {
            for (const auto& [rel, text] : *overlay) {
                if (rel.starts_with(kUnitMapsDir) && rel.ends_with(".json")) {
                    unitMapPaths.insert(rel);
                }
            }
        }
    }
    for (const std::string& rel : unitMapPaths) {
        const std::string stem = rel.substr(kUnitMapsDir.size(), rel.size() - kUnitMapsDir.size() - 5);
        const Text t = read(rel);
        if (!t.text) {
            continue;
        }
        Result<formats::UnitMappingProfile> map = formats::UnitMappingProfile::parse(*t.text, log);
        if (!map) {
            failed(rel, t, map.error());
            continue;
        }
        Result<std::string> canonical = map->toJson();
        settle(rel, t, canonical ? *canonical : std::string());
        out.unitMaps.insert_or_assign(stem, std::move(map).value());
    }

    // Maths modules in mathsOrder.
    const std::shared_ptr<const units::UnitRegistry> registry = units();
    for (const std::string& id : out.project.mathsOrder) {
        const std::string rel = modulePathOf(id);
        const Text t = read(rel);
        if (!t.text) {
            if (!out.baseline[rel].failed) {
                out.baseline.erase(rel);
                log.push_back("maths module '" + id + "' skipped: no " + rel);
            }
            continue;
        }
        if (registry == nullptr) {
            failed(rel, t, Error{ErrorCode::NotFound, "no unit table to read maths modules with", {}});
            continue;
        }
        Result<maths::MathModule> module = maths::MathModuleIo::parse(*t.text, *registry, log);
        if (!module) {
            failed(rel, t, module.error());
            continue;
        }
        module->origin = maths::MathModule::Origin::Global;
        Result<std::string> canonical = maths::MathModuleIo::toJson(*module, *registry);
        settle(rel, t, canonical ? *canonical : std::string());
        out.modules.push_back(std::move(module).value());
    }
    return out;
}

void ProjectSession::adopt(Loaded&& loaded) {
    profile_ = std::move(loaded.profile);
    project_ = std::move(loaded.project);
    workbooks_ = std::move(loaded.workbooks);
    workbookNames_ = std::move(loaded.names);
    overrides_ = std::move(loaded.overrides);
    aliases_ = std::move(loaded.aliases);
    mappings_ = std::move(loaded.mappings);
    unitMaps_ = std::move(loaded.unitMaps);
    mathsModules_ = std::move(loaded.modules);
    baseline_ = std::move(loaded.baseline);
    activeWorkbook_ = 0;
}

Result<void> ProjectSession::openProject(const std::filesystem::path& where) {
    std::filesystem::path folder = where;
    std::error_code ec;
    if (std::filesystem::is_regular_file(folder, ec) && folder.filename() == pathFromUtf8(kProjectFile)) {
        folder = folder.parent_path();
    }
    if (!std::filesystem::is_regular_file(folder / pathFromUtf8(kProjectFile), ec)) {
        ErrorContext ctx;
        ctx.path = folder / pathFromUtf8(kProjectFile);
        return fail(ErrorCode::Io, "no project.json in " + pathToUtf8(folder), std::move(ctx));
    }
    if (!defaultsLoaded_) {
        if (Result<void> d = loadDefaults(); !d) {
            return d;
        }
    }
    std::vector<std::string> log;
    Result<Loaded> loaded = loadDocuments(folder, nullptr, log);
    if (!loaded) {
        return Failure{loaded.error()};
    }
    undoStack_.clear();
    adopt(std::move(loaded).value());
    folder_ = folder;
    journal_ = std::make_unique<project::Journal>(folder, uuidRandom());
    lastJournaled_.clear();
    replayCache_.reset();
    recoveryOutstanding_ = false;
    recovered_ = project_.dirty; // a migrated project.json is rewritten by the next save
    undoStack_.setClean();
    setDirty(recovered_);
    if (settings_ != nullptr) {
        settings_->addRecentProject(QString::fromStdString(pathToUtf8(folder)));
    }
    publishLog(std::move(log));
    Q_EMIT projectOpened();
    Q_EMIT documentsChanged();
    restartAutosaveTimer();
    offerRecovery();
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
    // The profile's default maths modules are named in mathsOrder; only the modules with a file are loaded.
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

// The journal's replay applied in memory (02 section 6.8 rule 3; recoverJournal, the recovery prompt).
Result<void> ProjectSession::applyReplay(const project::JournalReplay& replay) {
    if (!folder_) {
        return fail(ErrorCode::Invalid, "no project is open");
    }
    std::map<std::string, std::string, std::less<>> overlay;
    for (const project::JournalEntry& e : replay.entries) {
        overlay.insert_or_assign(e.path, e.document.canonical());
    }
    std::vector<std::string> log = replay.log;
    Result<Loaded> loaded = loadDocuments(*folder_, &overlay, log);
    if (!loaded) {
        return Failure{loaded.error()};
    }
    undoStack_.clear();
    adopt(std::move(loaded).value());
    lastJournaled_.clear();
    replayCache_.reset();
    recoveryOutstanding_ = false;
    recovered_ = true;
    undoStack_.setClean();
    setDirty(true);
    log.push_back("recovered " + std::to_string(replay.entries.size()) +
                  " unsaved document(s) from the journal");
    // Adopt the recovered journal (D-187): this session journals the recovered documents as its own, then the
    // replayed files go, so a later edit (an undo back to the disk's version included) is never overridden
    // by them; until that write the replayed files stay, so a crash in between loses nothing.
    if (Result<void> own = journalDirty(true); !own) {
        log.push_back("the recovered documents could not be journalled: " + own.error().toString());
    } else if (Result<void> r = journal_->clear(project::JournalSessions::Others); !r) {
        log.push_back("the replayed journal files could not be deleted: " + r.error().toString());
    }
    for (const std::string& path : replay.conflicts) {
        log.push_back(path + " changed on disk after the journal was written; the journal's version is used");
    }
    for (const std::string& line : log) {
        log_.push_back(line);
        Q_EMIT logMessage(QString::fromStdString(line));
    }
    Q_EMIT documentsChanged();
    return ok();
}

// ---- saving
// -------------------------------------------------------------------------------------------------

project::ProjectFile ProjectSession::projectForSave() const {
    project::ProjectFile p = project_;
    p.workbooks = workbookNames_;
    p.mathsOrder.clear();
    for (const maths::MathModule& m : mathsModules_) {
        p.mathsOrder.push_back(m.id);
    }
    // A module whose file could not be read stays named (its file is kept as it is).
    for (const std::string& id : project_.mathsOrder) {
        const auto b = baseline_.find(modulePathOf(id));
        if (b != baseline_.end() && b->second.failed &&
            std::find(p.mathsOrder.begin(), p.mathsOrder.end(), id) == p.mathsOrder.end()) {
            p.mathsOrder.push_back(id);
        }
    }
    return p;
}

std::vector<std::pair<std::string, std::string>> ProjectSession::documentTexts() const {
    std::vector<std::pair<std::string, std::string>> out;
    out.emplace_back(std::string(kProjectFile), projectForSave().toJson());
    for (std::size_t i = 0; i < workbooks_.size(); ++i) {
        const std::string name = i < workbookNames_.size() ? workbookNames_[i] : workbooks_[i].name;
        out.emplace_back(workbookPathOf(name), workbooks_[i].toJson());
    }
    out.emplace_back(std::string(kOverridesPath), overrides_.toJson());
    out.emplace_back(std::string(kAliasesPath), aliases_.toJson());
    out.emplace_back(std::string(kMappingsPath), mappings_.toJson());
    for (const auto& [stem, map] : unitMaps_) {
        Result<std::string> text = map.toJson();
        out.emplace_back(unitMapPathOf(stem), text ? std::move(text).value() : std::string());
    }
    const std::shared_ptr<const units::UnitRegistry> registry = units();
    for (const maths::MathModule& m : mathsModules_) {
        Result<std::string> text = registry != nullptr ? maths::MathModuleIo::toJson(m, *registry)
                                                       : Result<std::string>(std::string());
        out.emplace_back(modulePathOf(m.id), text ? std::move(text).value() : std::string());
    }
    return out;
}

void ProjectSession::journalAhead() {
    // Write-ahead (02 section 6.8 rule 2, D-187): the journal first holds everything the save is about to
    // write, so a crash or a failure part-way through leaves the saved content recoverable and no older
    // journal entry of this session can later replay over a file the save wrote.
    if (journal_ == nullptr) {
        return;
    }
    if (Result<void> ahead = journalDirty(true); !ahead) {
        Q_EMIT logMessage(tr("The journal could not be written before saving: %1")
                              .arg(QString::fromStdString(ahead.error().toString())));
        // No stale copy of this session may outlive the files written next.
        static_cast<void>(journal_->clear(project::JournalSessions::Own));
        lastJournaled_.clear();
    }
}

Result<void> ProjectSession::writeIfChanged(const std::string& rel, const std::string& text) {
    const auto b = baseline_.find(rel);
    if (b != baseline_.end() && b->second.failed) {
        Q_EMIT logMessage(tr("%1 was not saved: it could not be read when the project was opened")
                              .arg(QString::fromStdString(rel)));
        return ok();
    }
    if (text.empty()) {
        return fail(ErrorCode::Invalid, rel + " has a value that cannot be written (non-finite number)");
    }
    const Hash128 h = hash128(text);
    const bool unchanged = b != baseline_.end() && b->second.text == h;
    const bool onDisk = b != baseline_.end() && b->second.disk.has_value();
    const bool mustExist = rel == kProjectFile || rel.starts_with("workbooks/") || rel.starts_with("maths/");
    if (unchanged && (onDisk || !mustExist)) {
        return ok();
    }
    const std::filesystem::path target = *folder_ / pathFromUtf8(rel);
    std::error_code ec;
    std::filesystem::create_directories(target.parent_path(), ec);
    // The D-070 copy of the previous version is made first (D-190); a copy that fails is reported, and the
    // document, written all the same, gets its new baseline (it is on disk).
    std::optional<Error> backupError;
    if (Result<void> r = project::saveWithBackup(*folder_, target, text, std::chrono::system_clock::now(), 5,
                                                 &backupError);
        !r) {
        return r;
    }
    if (backupError) {
        Q_EMIT logMessage(
            tr("%1 was saved without its backup copy: %2")
                .arg(QString::fromStdString(rel), QString::fromStdString(backupError->toString())));
    }
    baseline_[rel] = Baseline{h, h, false};
    return ok();
}

Result<void> ProjectSession::retireUnnamed(const std::set<std::string, std::less<>>& current,
                                           const std::function<bool(std::string_view)>& within) {
    Result<void> first = ok();
    // A name that changed only in letter case is the same file on a case-insensitive file system (Windows,
    // macOS): it was just written and is never retired.
    const auto sameFileAsCurrent = [&](const std::string& old) {
        const std::filesystem::path oldPath = *folder_ / pathFromUtf8(old);
        for (const std::string& rel : current) {
            std::error_code ec;
            if (std::filesystem::equivalent(oldPath, *folder_ / pathFromUtf8(rel), ec) && !ec) {
                return true;
            }
        }
        return false;
    };
    for (auto it = baseline_.begin(); it != baseline_.end();) {
        const bool unnamed = current.count(it->first) == 0 && retirable(it->first) && within(it->first) &&
                             it->second.disk && !it->second.failed;
        if (!unnamed) {
            ++it;
            continue;
        }
        if (sameFileAsCurrent(it->first)) {
            it = baseline_.erase(it);
            continue;
        }
        if (Result<void> r = retireFile(*folder_, it->first); !r) {
            if (first) {
                first = std::move(r);
            }
            ++it;
            continue;
        }
        it = baseline_.erase(it);
    }
    return first;
}

bool ProjectSession::hasUnsavedDocuments() const {
    for (const auto& [rel, text] : documentTexts()) {
        const auto b = baseline_.find(rel);
        if (b != baseline_.end() && b->second.failed) {
            continue; // never written: not an edit
        }
        if (text.empty() || b == baseline_.end() || b->second.text != hash128(text)) {
            return true;
        }
    }
    return false;
}

void ProjectSession::markSaved() {
    lastJournaled_.clear();
    recoveryOutstanding_ = hasRecoverableJournal();
    recovered_ = false;
    project_.dirty = false;
    undoStack_.setClean();
    setDirty(false);
}

Result<void> ProjectSession::save() {
    if (!folder_) {
        return fail(ErrorCode::Invalid, "no project is open");
    }
    Result<void> first = ok();
    const auto note = [&first](Result<void> r) {
        if (!r && first) {
            first = std::move(r);
        }
    };
    journalAhead();
    std::vector<std::pair<std::string, std::string>> texts = documentTexts();
    // project.json last, so it never names a workbook or module that was not written (02 section 6.8 rule 2).
    std::rotate(texts.begin(), texts.begin() + 1, texts.end());
    std::set<std::string, std::less<>> current;
    for (const auto& [rel, text] : texts) {
        current.insert(rel);
        if (rel == kProjectFile && !first) {
            continue; // written only when every other document was (02 section 6.8 rule 2; journalled ahead)
        }
        note(writeIfChanged(rel, text));
    }
    if (!first) {
        return first; // the journal stays: the unsaved documents remain recoverable
    }
    // Files of workbooks, unit maps and modules the project no longer names, retired only once every write
    // succeeded: after a failed one, project.json on disk still names them and they stay (as in
    // saveMathsModules).
    if (Result<void> r = retireUnnamed(current, [](std::string_view) { return true; }); !r) {
        return r; // the journal stays
    }
    if (journal_ != nullptr) {
        // Only this session's files: a journal whose recovery was put off (Later) stays for the next open.
        if (Result<void> r = journal_->clear(project::JournalSessions::Own); !r) {
            return r;
        }
    }
    markSaved();
    return ok();
}

Result<void> ProjectSession::saveMathsModules() {
    if (!folder_) {
        return fail(ErrorCode::Invalid, "no project is open");
    }
    journalAhead();
    Result<void> first = ok();
    const auto note = [&first](Result<void> r) {
        if (!r && first) {
            first = std::move(r);
        }
    };
    std::set<std::string, std::less<>> current;
    const std::shared_ptr<const units::UnitRegistry> registry = units();
    for (const maths::MathModule& m : mathsModules_) {
        const std::string rel = modulePathOf(m.id);
        current.insert(rel);
        Result<std::string> text = registry != nullptr ? maths::MathModuleIo::toJson(m, *registry)
                                                       : Result<std::string>(std::string());
        note(writeIfChanged(rel, text ? *text : std::string()));
    }
    // A module whose file could not be read stays named; the files of modules no longer named are retired
    // after project.json stops naming them.
    for (const auto& [rel, b] : baseline_) {
        if (b.failed && rel.starts_with("maths/")) {
            current.insert(rel);
        }
    }
    if (!first) {
        return first; // the journal stays (written ahead)
    }
    // The module order: project.json as it is on disk with this session's mathsOrder, so the session's other
    // project.json edits stay unsaved (D-190, amending D-177 (a)).
    const std::filesystem::path projectPath = *folder_ / pathFromUtf8(kProjectFile);
    std::vector<std::string> log;
    Result<project::ProjectFile> onDisk = project::ProjectFile::load(projectPath, log);
    if (!onDisk) {
        return Failure{onDisk.error()};
    }
    const std::vector<std::string> order = projectForSave().mathsOrder;
    if (onDisk->mathsOrder != order) {
        onDisk->mathsOrder = order;
        const std::string text = onDisk->toJson();
        if (text.empty()) {
            return fail(ErrorCode::Invalid,
                        "project.json has a value that cannot be written (non-finite number)");
        }
        std::optional<Error> backupError;
        if (Result<void> r = project::saveWithBackup(*folder_, projectPath, text,
                                                     std::chrono::system_clock::now(), 5, &backupError);
            !r) {
            return r;
        }
        if (backupError) {
            Q_EMIT logMessage(tr("project.json was saved without its backup copy: %1")
                                  .arg(QString::fromStdString(backupError->toString())));
        }
        // The file holds this text now; the session's own project.json stays dirty if it differs.
        const Hash128 h = hash128(text);
        baseline_[std::string(kProjectFile)] = Baseline{h, h, false};
    }
    if (Result<void> r =
            retireUnnamed(current, [](std::string_view rel) { return rel.starts_with("maths/"); });
        !r) {
        return r;
    }
    if (!hasUnsavedDocuments()) {
        // Nothing else was edited: the session is saved (its journal goes, as after save()).
        if (journal_ != nullptr) {
            if (Result<void> r = journal_->clear(project::JournalSessions::Own); !r) {
                return r;
            }
        }
        markSaved();
        return ok();
    }
    // The other edits stay journalled, now against the files as they are (the modules left the snapshot and
    // project.json has a new base), so a crash replays them without a false conflict.
    if (journal_ != nullptr) {
        if (Result<void> r = journalDirty(true); !r) {
            Q_EMIT logMessage(tr("The journal could not be written after saving the maths modules: %1")
                                  .arg(QString::fromStdString(r.error().toString())));
        }
    }
    return ok();
}

void ProjectSession::closeProject() {
    if (!folder_) {
        return;
    }
    folder_.reset();
    undoStack_.clear();
    autosaveTimer_->stop();
    journal_.reset();
    lastJournaled_.clear();
    replayCache_.reset();
    baseline_.clear();
    recoveryOutstanding_ = false;
    recovered_ = false;
    std::vector<std::string> log;
    if (Result<project::WorkbookFile> wb = loadShippedWorkbook(log)) {
        workbookNames_ = {wb->name};
        workbooks_ = {std::move(wb).value()};
    } else {
        workbooks_.clear();
        workbookNames_.clear();
    }
    project_ = project::ProjectFile::makeDefault("Untitled", profile_.id.empty() ? "circuit" : profile_.id);
    overrides_ = {};
    aliases_ = {};
    mappings_ = {};
    unitMaps_.clear();
    mathsModules_.clear();
    activeWorkbook_ = 0;
    setDirty(false);
    Q_EMIT projectClosed();
    Q_EMIT documentsChanged();
}

// ---- view state and accessors
// -------------------------------------------------------------------------------

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
        markDirty();
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

void ProjectSession::setUnits(std::shared_ptr<const units::UnitRegistry> units) {
    units_ = std::move(units);
}

std::shared_ptr<const units::UnitRegistry> ProjectSession::units() const {
    if (units_ == nullptr) {
        Result<units::UnitRegistry> r = units::UnitRegistry::loadFiles(paths_.dataDir() / "units.json");
        if (r) {
            units_ = std::make_shared<const units::UnitRegistry>(std::move(r).value());
        }
    }
    return units_;
}

model::MappingTable ProjectSession::mappingTable() const {
    const std::shared_ptr<const units::UnitRegistry> registry = units();
    if (registry == nullptr) {
        model::MappingTable t;
        for (const project::Mappings::Row& r : mappings_.rows) {
            model::MappingRow row;
            row.from = r.from;
            row.to = r.to;
            row.unitlessOnly = r.unitlessOnly;
            t.rows.push_back(std::move(row));
        }
        return t;
    }
    std::vector<std::string> log;
    return mappings_.toTable(*registry, log);
}

} // namespace esses::qt
