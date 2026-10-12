// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// SettingsStore (06 section 18, 02 section 6.9 settings-1; D-055): <config>/settings.json, written atomically
// and coalesced 1 s after a change; unknown members preserved.

#include <esses/base/atomic_file.hpp>
#include <esses/base/utf8.hpp>
#include <esses/qtmodel/settings_store.hpp>

#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QTimer>
#include <system_error>
#include <utility>

namespace esses::qt {

namespace {

constexpr auto kSchemaId = "urn:esses:settings-1";
constexpr int kSchemaVersion = 1;
constexpr int kDefaultSaveDelayMs = 1000;

QJsonValue lookup(const QJsonObject& root, const QStringList& path) {
    QJsonValue v = root;
    for (const QString& part : path) {
        if (!v.isObject()) {
            return QJsonValue(QJsonValue::Undefined);
        }
        v = v.toObject().value(part);
    }
    return v;
}

QJsonObject assign(QJsonObject object, const QStringList& path, qsizetype i, const QJsonValue& value) {
    if (i + 1 == path.size()) {
        if (value.isUndefined()) {
            object.remove(path[i]);
        } else {
            object.insert(path[i], value);
        }
        return object;
    }
    const QJsonObject child = object.value(path[i]).toObject();
    object.insert(path[i], assign(child, path, i + 1, value));
    return object;
}

bool samePath(const QString& a, const QString& b) {
#if defined(_WIN32)
    return a.compare(b, Qt::CaseInsensitive) == 0;
#else
    return a == b;
#endif
}

} // namespace

SettingsStore::SettingsStore(std::filesystem::path file, QObject* parent)
    : QObject(parent), file_(std::move(file)), saveTimer_(new QTimer(this)) {
    saveTimer_->setSingleShot(true);
    saveTimer_->setInterval(kDefaultSaveDelayMs);
    connect(saveTimer_, &QTimer::timeout, this, [this] {
        if (Result<void> r = saveNow(); !r) {
            Q_EMIT saveFailed(QString::fromStdString(r.error().toString()));
        }
    });
}

SettingsStore::~SettingsStore() {
    if (dirty_) {
        static_cast<void>(saveNow());
    }
}

void SettingsStore::setSaveDelay(int ms) {
    saveTimer_->setInterval(ms < 0 ? 0 : ms);
}

Result<void> SettingsStore::load() {
    std::error_code ec;
    if (file_.empty() || !std::filesystem::exists(file_, ec)) {
        root_ = QJsonObject{};
        dirty_ = false;
        return ok(); // defaults
    }
    QFile f(QString::fromStdString(pathToUtf8(file_)));
    if (!f.open(QIODevice::ReadOnly)) {
        ErrorContext ctx; // not ErrorContext{.path = ...}: GCC 13 -Wmissing-field-initializers
        ctx.path = file_;
        return fail(ErrorCode::Io, "cannot read " + pathToUtf8(file_), std::move(ctx));
    }
    QJsonParseError error{};
    const QJsonDocument doc = QJsonDocument::fromJson(f.readAll(), &error);
    if (error.error != QJsonParseError::NoError || !doc.isObject()) {
        ErrorContext ctx;
        ctx.path = file_;
        ctx.offset = static_cast<std::uint64_t>(error.offset);
        return fail(ErrorCode::Format,
                    "settings.json is not a JSON object: " + error.errorString().toStdString(),
                    std::move(ctx));
    }
    const QJsonObject root = doc.object();
    const QJsonValue schema = root.value(QStringLiteral("$schema"));
    if (!schema.isUndefined() && schema.toString() != QLatin1String(kSchemaId)) {
        ErrorContext ctx;
        ctx.path = file_;
        ctx.schemaId = schema.toString().toStdString();
        return fail(ErrorCode::Invalid, "settings.json names another schema", std::move(ctx));
    }
    root_ = root;
    dirty_ = false;
    return ok();
}

Result<void> SettingsStore::saveNow() {
    saveTimer_->stop();
    if (file_.empty()) {
        dirty_ = false;
        return ok();
    }
    QJsonObject root = root_;
    root.insert(QStringLiteral("$schema"), QLatin1String(kSchemaId));
    root.insert(QStringLiteral("schemaVersion"), kSchemaVersion);
    std::error_code ec;
    std::filesystem::create_directories(file_.parent_path(), ec);
    const QByteArray bytes = QJsonDocument(root).toJson(QJsonDocument::Indented);
    if (Result<void> r = AtomicFile::write(
            file_, std::string_view(bytes.constData(), static_cast<std::size_t>(bytes.size())));
        !r) {
        return r;
    }
    root_ = root;
    dirty_ = false;
    return ok();
}

QVariant SettingsStore::value(const QString& key, const QVariant& fallback) const {
    const QJsonValue v = lookup(root_, key.split(QLatin1Char('.')));
    return v.isUndefined() ? fallback : v.toVariant();
}

void SettingsStore::setValue(const QString& key, const QVariant& v) {
    const QStringList path = key.split(QLatin1Char('.'));
    const QJsonValue next = v.isValid() ? QJsonValue::fromVariant(v) : QJsonValue(QJsonValue::Undefined);
    if (lookup(root_, path) == next) {
        return;
    }
    root_ = assign(root_, path, 0, next);
    dirty_ = true;
    saveTimer_->start();
    Q_EMIT changed(key);
}

QStringList SettingsStore::recent(const QString& key) const {
    QStringList out;
    for (const QJsonValue& v : lookup(root_, key.split(QLatin1Char('.'))).toArray()) {
        if (v.isString()) {
            out << v.toString();
        }
    }
    return out;
}

void SettingsStore::addRecent(const QString& key, const QString& path, int cap) {
    QStringList list = recent(key);
    list.removeIf([&](const QString& e) { return samePath(e, path); });
    list.prepend(path);
    while (list.size() > cap) {
        list.removeLast();
    }
    setValue(key, list);
}

QStringList SettingsStore::recentLogs() const {
    return recent(QStringLiteral("mru.logs"));
}

void SettingsStore::addRecentLog(const QString& path) {
    addRecent(QStringLiteral("mru.logs"), path, kRecentLogs);
}

QStringList SettingsStore::recentProjects() const {
    return recent(QStringLiteral("mru.projects"));
}

void SettingsStore::addRecentProject(const QString& path) {
    addRecent(QStringLiteral("mru.projects"), path, kRecentProjects);
}

} // namespace esses::qt
