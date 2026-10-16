// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT

#include <esses/widgets/shell/layout_file.hpp>

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QSaveFile>
#include <system_error>

namespace esses::widgets {

namespace {

QString key(const char* k) {
    return QString::fromLatin1(k);
}

QString qPath(const std::filesystem::path& p) {
    return QString::fromStdWString(p.wstring());
}

QJsonObject object(const QJsonObject& parent, const QString& member) {
    return parent.value(member).toObject();
}

QJsonObject object(const QJsonObject& parent, const char* member) {
    return object(parent, key(member));
}

} // namespace

LayoutFile::LayoutFile() {
    root_.insert(key("$schema"), key(kSchemaId));
    root_.insert(key("schemaVersion"), 1);
}

bool LayoutFile::load(const std::filesystem::path& file, LayoutFile& out, QString* error) {
    out = LayoutFile();
    QFile f(qPath(file));
    if (!f.exists()) {
        return true;
    }
    if (!f.open(QIODevice::ReadOnly)) {
        if (error != nullptr) {
            *error = QStringLiteral("%1: %2").arg(qPath(file), f.errorString());
        }
        return false;
    }
    QJsonParseError parse{};
    const QJsonDocument doc = QJsonDocument::fromJson(f.readAll(), &parse);
    if (parse.error != QJsonParseError::NoError || !doc.isObject()) {
        if (error != nullptr) {
            *error = QStringLiteral("%1: not a JSON object (%2)").arg(qPath(file), parse.errorString());
        }
        return false;
    }
    const QJsonObject root = doc.object();
    const QString schema = root.value(key("$schema")).toString();
    if (!schema.isEmpty() && schema != key(kSchemaId) && schema != QLatin1String("esses:layout-1")) {
        if (error != nullptr) {
            *error = QStringLiteral("%1: unsupported schema %2").arg(qPath(file), schema);
        }
        return false;
    }
    out.root_ = root;
    out.root_.insert(key("$schema"), key(kSchemaId));
    out.root_.insert(key("schemaVersion"), 1);
    return true;
}

bool LayoutFile::save(const std::filesystem::path& file, QString* error) const {
    std::error_code ec;
    std::filesystem::create_directories(file.parent_path(), ec);
    QSaveFile f(qPath(file));
    if (!f.open(QIODevice::WriteOnly)) {
        if (error != nullptr) {
            *error = QStringLiteral("%1: %2").arg(qPath(file), f.errorString());
        }
        return false;
    }
    f.write(QJsonDocument(root_).toJson(QJsonDocument::Indented));
    if (!f.commit()) {
        if (error != nullptr) {
            *error = QStringLiteral("%1: %2").arg(qPath(file), f.errorString());
        }
        return false;
    }
    return true;
}

QByteArray LayoutFile::perspective(const QString& k) const {
    const QJsonObject p = object(object(root_, "perspectives"), k);
    // The member is "ads" in layout-1 whatever backend wrote it (06 section 3.4).
    return QByteArray::fromBase64(p.value(key("ads")).toString().toLatin1());
}

void LayoutFile::setPerspective(const QString& k, const QByteArray& perspective) {
    QJsonObject all = object(root_, "perspectives");
    QJsonObject p;
    p.insert(key("ads"), QString::fromLatin1(perspective.toBase64()));
    p.insert(key("savedAt"), QDateTime::currentDateTimeUtc().toString(Qt::ISODate));
    all.insert(k, p);
    root_.insert(key("perspectives"), all);
}

void LayoutFile::removePerspective(const QString& k) {
    QJsonObject all = object(root_, "perspectives");
    all.remove(k);
    root_.insert(key("perspectives"), all);
}

QByteArray LayoutFile::windowGeometry() const {
    return QByteArray::fromBase64(object(root_, "window").value(key("geometry")).toString().toLatin1());
}

bool LayoutFile::windowMaximised() const {
    return object(root_, "window").value(key("maximised")).toBool();
}

void LayoutFile::setWindow(const QByteArray& geometry, bool maximised, const QString& screenName,
                           double dpr) {
    QJsonObject w = object(root_, "window");
    w.insert(key("geometry"), QString::fromLatin1(geometry.toBase64()));
    w.insert(key("maximised"), maximised);
    QJsonObject screen;
    screen.insert(key("name"), screenName);
    screen.insert(key("dpr"), dpr);
    w.insert(key("screen"), screen);
    root_.insert(key("window"), w);
}

std::optional<QString> LayoutFile::activeWorkbook() const {
    const QJsonValue v = object(root_, "active").value(key("workbook"));
    return v.isString() ? std::optional<QString>(v.toString()) : std::nullopt;
}

std::optional<QString> LayoutFile::activeWorksheet() const {
    const QJsonValue v = object(root_, "active").value(key("worksheet"));
    return v.isString() ? std::optional<QString>(v.toString()) : std::nullopt;
}

void LayoutFile::setActive(const QString& workbookId, const QString& worksheetName) {
    QJsonObject a;
    a.insert(key("workbook"), workbookId);
    a.insert(key("worksheet"), worksheetName);
    root_.insert(key("active"), a);
}

void LayoutFile::setDockState(const QString& id, const QString& state) {
    setDockOption(id, QStringLiteral("state"), state);
}

QJsonObject LayoutFile::dock(const QString& id) const {
    return object(object(root_, "docks"), id);
}

void LayoutFile::setDockOption(const QString& id, const QString& option, const QJsonValue& value) {
    QJsonObject docks = object(root_, "docks");
    QJsonObject d = docks.value(id).toObject();
    d.insert(option, value);
    docks.insert(id, d);
    root_.insert(key("docks"), docks);
}

QByteArray LayoutFile::dialogGeometry(const QString& k) const {
    return QByteArray::fromBase64(
        object(object(root_, "dialogs"), k).value(key("geometry")).toString().toLatin1());
}

void LayoutFile::setDialogGeometry(const QString& k, const QByteArray& geometry) {
    QJsonObject dialogs = object(root_, "dialogs");
    QJsonObject d = dialogs.value(k).toObject(); // other members (a splitter, 06 section 3.4) are kept
    d.insert(key("geometry"), QString::fromLatin1(geometry.toBase64()));
    dialogs.insert(k, d);
    root_.insert(key("dialogs"), dialogs);
}

void LayoutFile::setVersions(const QString& appVersion, const QString& qtVersion, const QString& dockBackend,
                             const QString& adsVersion) {
    root_.insert(key("appVersion"), appVersion);
    root_.insert(key("qtVersion"), qtVersion);
    root_.insert(key("dockBackend"), dockBackend);
    if (adsVersion.isEmpty()) {
        root_.remove(key("adsVersion"));
    } else {
        root_.insert(key("adsVersion"), adsVersion);
    }
}

QString LayoutFile::appVersion() const {
    return root_.value(key("appVersion")).toString();
}

QString LayoutFile::dockBackend() const {
    return root_.value(key("dockBackend")).toString();
}

QString LayoutFile::adsVersion() const {
    return root_.value(key("adsVersion")).toString();
}

} // namespace esses::widgets
