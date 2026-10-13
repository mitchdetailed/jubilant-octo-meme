// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT

#include <esses/base/utf8.hpp>
#include <esses/project/rgba.hpp>
#include <esses/qtmodel/project_session.hpp>
#include <esses/qtmodel/settings_store.hpp>
#include <esses/widgets/editors_options/scheme_selector.hpp>

#include <QColor>
#include <QGuiApplication>
#include <QStyleHints>
#include <algorithm>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

namespace esses::widgets {

namespace {

constexpr std::string_view kSchemeSuffix = ".scheme.json";

QString qPath(const std::filesystem::path& p) {
    return QString::fromStdWString(p.wstring());
}

// A file name stem for a scheme name: letters, digits, space, '-', '_' and '.' kept, anything else '_'.
std::string fileStem(const QString& name) {
    QString stem;
    for (const QChar c : name) {
        stem += c.isLetterOrNumber() || c == QLatin1Char(' ') || c == QLatin1Char('-') ||
                        c == QLatin1Char('_') || c == QLatin1Char('.')
                    ? c
                    : QLatin1Char('_');
    }
    stem = stem.trimmed();
    return stem.isEmpty() ? std::string("scheme") : stem.toStdString();
}

// <dir>/<stem>.scheme.json, numbered ("<stem> 2", ...) while the file exists.
std::filesystem::path freeFile(const std::filesystem::path& dir, const std::string& stem) {
    std::error_code ec;
    std::filesystem::path p = dir / pathFromUtf8(stem + std::string(kSchemeSuffix));
    for (int n = 2; std::filesystem::exists(p, ec); ++n) {
        p = dir / pathFromUtf8(stem + " " + std::to_string(n) + std::string(kSchemeSuffix));
    }
    return p;
}

bool endsWith(const std::string& s, std::string_view suffix) {
    return s.size() >= suffix.size() && s.compare(s.size() - suffix.size(), suffix.size(), suffix) == 0;
}

} // namespace

SchemeSelector::SchemeSelector(qt::ProjectSession& session, qt::SettingsStore* settings, QWidget* parent)
    : QComboBox(parent), session_(session), settings_(settings) {
    setObjectName(QStringLiteral("schemeSelector"));
    setAccessibleName(tr("Colour scheme"));
    setToolTip(tr("Colour scheme"));
    setSizeAdjustPolicy(QComboBox::AdjustToContents);
    setFocusPolicy(Qt::ClickFocus); // a toolbar combo never takes the keyboard focus from the worksheet
    connect(this, &QComboBox::activated, this, [this](int index) {
        if (index >= 0 && index < count()) {
            selectScheme(itemText(index));
        }
    });
    connect(&session_, &qt::ProjectSession::schemeChanged, this, [this] { showActive(); });
    refresh();
}

SchemeSelector::~SchemeSelector() = default;

std::filesystem::path SchemeSelector::userSchemeDir() const {
    return session_.paths().configDir() / "schemes";
}

void SchemeSelector::refresh() {
    entries_.clear();
    for (const bool dark : {false, true}) {
        if (std::shared_ptr<const project::Scheme> s = session_.shippedScheme(dark)) {
            entries_.push_back(Entry{QString::fromStdString(s->name), std::move(s), {}});
        }
    }
    const std::shared_ptr<const project::Scheme> light = session_.shippedScheme(false);
    const std::shared_ptr<const project::Scheme> darkScheme = session_.shippedScheme(true);
    std::vector<std::filesystem::path> files;
    std::error_code ec;
    const std::filesystem::path dir = userSchemeDir();
    if (!dir.empty() && std::filesystem::is_directory(dir, ec)) {
        for (const auto& e : std::filesystem::directory_iterator(dir, ec)) {
            if (e.is_regular_file(ec) && endsWith(pathToUtf8(e.path().filename()), kSchemeSuffix)) {
                files.push_back(e.path());
            }
        }
    }
    std::sort(files.begin(), files.end());
    std::vector<Entry> user;
    for (const std::filesystem::path& file : files) {
        const QString where = qPath(file.filename());
        std::vector<std::string> log;
        Result<project::Scheme> loaded = project::Scheme::load(file, log);
        if (!loaded) {
            Q_EMIT logMessage(tr("Scheme file %1 skipped: %2")
                                  .arg(where, QString::fromStdString(loaded.error().toString())));
            continue;
        }
        project::Scheme s = std::move(loaded).value();
        const QString name = QString::fromStdString(s.name);
        const auto taken = [&name](const Entry& e) { return e.name.compare(name, Qt::CaseInsensitive) == 0; };
        if (name.trimmed().isEmpty() || std::any_of(entries_.begin(), entries_.end(), taken) ||
            std::any_of(user.begin(), user.end(), taken)) {
            Q_EMIT logMessage(tr("Scheme file %1 skipped: the scheme name \"%2\" is empty or already listed")
                                  .arg(where, name));
            continue;
        }
        // 06 section 16 rule 8: what the file lacks comes from the shipped scheme of its background.
        const std::shared_ptr<const project::Scheme>& shipped = s.background == "dark" ? darkScheme : light;
        if (shipped != nullptr) {
            for (const std::string& line : s.fillMissingFrom(*shipped)) {
                log.push_back(line);
            }
        }
        for (const std::string& line : log) {
            Q_EMIT logMessage(where + QStringLiteral(": ") + QString::fromStdString(line));
        }
        user.push_back(Entry{name, std::make_shared<const project::Scheme>(std::move(s)), file});
    }
    std::sort(user.begin(), user.end(), [](const Entry& a, const Entry& b) {
        return QString::compare(a.name, b.name, Qt::CaseInsensitive) < 0;
    });
    for (Entry& e : user) {
        entries_.push_back(std::move(e));
    }
    {
        const QSignalBlocker block(this);
        clear();
        for (const Entry& e : entries_) {
            addItem(e.name);
            setItemData(count() - 1,
                        e.file.empty() ? tr("Shipped scheme (read-only)")
                                       : tr("User scheme: %1").arg(qPath(e.file)),
                        Qt::ToolTipRole);
        }
    }
    showActive();
}

QStringList SchemeSelector::schemeNames() const {
    QStringList names;
    for (int i = 0; i < count(); ++i) {
        names.push_back(itemText(i));
    }
    return names;
}

std::shared_ptr<const project::Scheme> SchemeSelector::scheme(const QString& name) const {
    for (const Entry& e : entries_) {
        if (e.name == name) {
            return e.scheme;
        }
    }
    return nullptr;
}

void SchemeSelector::showActive() {
    const std::shared_ptr<const project::Scheme> active = session_.scheme();
    const int index = active == nullptr ? -1 : findText(QString::fromStdString(active->name));
    const QSignalBlocker block(this);
    setCurrentIndex(index);
}

bool SchemeSelector::apply(const QString& name) {
    const std::shared_ptr<const project::Scheme> s = scheme(name);
    if (s == nullptr) {
        return false;
    }
    if (session_.scheme() != s) {
        session_.setScheme(s); // records scheme.active and emits schemeChanged()
    }
    showActive();
    return true;
}

bool SchemeSelector::selectScheme(const QString& name) {
    if (!apply(name)) {
        showActive();
        return false;
    }
    if (settings_ != nullptr && followsSystem()) {
        settings_->setValue(QString::fromLatin1(kFollowOsKey), false); // an explicit choice wins over the OS
    }
    return true;
}

void SchemeSelector::restoreActive() {
    if (settings_ != nullptr) {
        const QString active = settings_->value(QString::fromLatin1(kActiveKey)).toString();
        const std::shared_ptr<const project::Scheme> current = session_.scheme();
        if (!active.isEmpty() && (current == nullptr || QString::fromStdString(current->name) != active)) {
            apply(active);
        }
    }
    applySystemScheme();
}

bool SchemeSelector::followsSystem() const {
    return settings_ != nullptr && settings_->value(QString::fromLatin1(kFollowOsKey), false).toBool();
}

void SchemeSelector::setFollowSystem(bool on) {
    if (settings_ == nullptr || followsSystem() == on) {
        return;
    }
    settings_->setValue(QString::fromLatin1(kFollowOsKey), on);
    applySystemScheme();
}

bool SchemeSelector::applySystemScheme() {
    if (!followsSystem()) {
        return false;
    }
    const Qt::ColorScheme os = QGuiApplication::styleHints() == nullptr
                                   ? Qt::ColorScheme::Unknown
                                   : QGuiApplication::styleHints()->colorScheme();
    if (os == Qt::ColorScheme::Unknown) {
        return false;
    }
    const std::shared_ptr<const project::Scheme> target = session_.shippedScheme(os == Qt::ColorScheme::Dark);
    if (target == nullptr || target == session_.scheme()) {
        return false;
    }
    session_.setScheme(target);
    return true;
}

Result<std::filesystem::path> SchemeSelector::duplicateActive() {
    const std::shared_ptr<const project::Scheme> active = session_.scheme();
    if (active == nullptr) {
        return fail(ErrorCode::Invalid, "no scheme is active");
    }
    const QString base = QString::fromStdString(active->name) + QStringLiteral(" copy");
    QString name = base;
    for (int n = 2; scheme(name) != nullptr; ++n) {
        name = base + QLatin1Char(' ') + QString::number(n);
    }
    const std::filesystem::path dir = userSchemeDir();
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    if (ec) {
        ErrorContext ctx;
        ctx.path = dir;
        return fail(ErrorCode::Io, "the user scheme folder cannot be created: " + ec.message(),
                    std::move(ctx));
    }
    project::Scheme copy = *active;
    copy.name = name.toStdString();
    const std::filesystem::path file = freeFile(dir, fileStem(name));
    if (Result<void> r = copy.save(file); !r) {
        return Failure{r.error()};
    }
    refresh();
    selectScheme(name);
    return file;
}

Result<QString> SchemeSelector::importScheme(const std::filesystem::path& file) {
    std::vector<std::string> log;
    Result<project::Scheme> loaded = project::Scheme::load(file, log);
    if (!loaded) {
        return Failure{loaded.error()};
    }
    const QString name = QString::fromStdString(loaded->name);
    if (name.trimmed().isEmpty()) {
        return fail(ErrorCode::Invalid, "the scheme has no name");
    }
    for (const Entry& e : entries_) {
        if (e.name.compare(name, Qt::CaseInsensitive) == 0) {
            return fail(ErrorCode::Invalid,
                        "a scheme named \"" + name.toStdString() + "\" is already listed");
        }
    }
    const std::filesystem::path dir = userSchemeDir();
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    if (ec) {
        ErrorContext ctx;
        ctx.path = dir;
        return fail(ErrorCode::Io, "the user scheme folder cannot be created: " + ec.message(),
                    std::move(ctx));
    }
    if (Result<void> r = loaded->save(freeFile(dir, fileStem(name))); !r) {
        return Failure{r.error()};
    }
    refresh();
    return name;
}

Result<void> SchemeSelector::exportActive(const std::filesystem::path& file) const {
    const std::shared_ptr<const project::Scheme> active = session_.scheme();
    if (active == nullptr) {
        return fail(ErrorCode::Invalid, "no scheme is active");
    }
    return active->save(file);
}

QPalette SchemeSelector::palette(const project::Scheme& scheme) {
    const auto colour = [&scheme](std::string_view role) -> std::optional<QColor> {
        const auto it = scheme.roles.find(role);
        if (it == scheme.roles.end()) {
            return std::nullopt;
        }
        const project::Rgba c = it->second;
        return QColor(project::red(c), project::green(c), project::blue(c), project::alpha(c));
    };
    const bool dark = scheme.background == "dark";
    const QColor window =
        colour("windowBackground").value_or(dark ? QColor(0x30, 0x30, 0x30) : QColor(0xf0, 0xf0, 0xf0));
    // The bevel shades (Light, Midlight, Dark, Mid, Shadow) and contrasting texts derived from the window.
    QPalette p(window, window);
    const auto set = [&p](QPalette::ColorRole role, const std::optional<QColor>& c) {
        if (c) {
            for (const QPalette::ColorGroup g : {QPalette::Active, QPalette::Inactive, QPalette::Disabled}) {
                p.setColor(g, role, *c);
            }
        }
    };
    const std::optional<QColor> base = colour("dataBackground");
    set(QPalette::Base, base);
    if (base) {
        set(QPalette::AlternateBase, dark ? base->lighter(115) : base->darker(104));
    }
    const std::optional<QColor> text = colour("dataForeground");
    set(QPalette::WindowText, text);
    set(QPalette::Text, text);
    set(QPalette::ButtonText, text);
    set(QPalette::BrightText, text);
    set(QPalette::Highlight, colour("dataHighlightBackground"));
    set(QPalette::HighlightedText, colour("dataHighlightForeground"));
    const std::optional<QColor> muted = colour("dataForeground2");
    set(QPalette::PlaceholderText, muted);
    if (muted) {
        for (const QPalette::ColorRole role : {QPalette::WindowText, QPalette::Text, QPalette::ButtonText}) {
            p.setColor(QPalette::Disabled, role, *muted);
        }
    }
    return p;
}

} // namespace esses::widgets
