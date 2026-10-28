// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// The 0.1 memory budget in the GUI (memory_budget.hpp; 07 section 7.1 row 1, AC-M4-02; 01 sections 7.2 and
// 8.3; D-011, D-043). Root of the M4 budget package in esses_qtmodel (docs/milestones/M4.md section 3):
// the setting, the texts of the open-time warning and of the Stats; PerformanceMonitor's sampling and timer
// are the contract's (D-206).

#include <esses/qtmodel/app_runtime.hpp>
#include <esses/qtmodel/memory_budget.hpp>
#include <esses/qtmodel/settings_store.hpp>

#include <QMetaType>
#include <QTimer>
#include <QVariant>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <numeric>

namespace esses::qt {

namespace {

constexpr std::uint64_t kKiB = 1024;
constexpr std::uint64_t kMiB = kKiB * 1024;
constexpr std::uint64_t kGiB = kMiB * 1024;

// A positive byte count from a settings value (a JSON number: an integer or a double); nullopt for anything
// else. The double is range-checked before it becomes an integer.
std::optional<std::uint64_t> positiveBytes(const QVariant& v) {
    switch (v.typeId()) {
    case QMetaType::Short:
    case QMetaType::Int:
    case QMetaType::Long:
    case QMetaType::LongLong: {
        const qlonglong n = v.toLongLong();
        return n > 0 ? std::optional<std::uint64_t>(static_cast<std::uint64_t>(n)) : std::nullopt;
    }
    case QMetaType::UShort:
    case QMetaType::UInt:
    case QMetaType::ULong:
    case QMetaType::ULongLong: {
        const qulonglong n = v.toULongLong();
        return n > 0 ? std::optional<std::uint64_t>(static_cast<std::uint64_t>(n)) : std::nullopt;
    }
    case QMetaType::Float:
    case QMetaType::Double: {
        const double d = v.toDouble();
        if (!std::isfinite(d) || !(d > 0.0)) {
            return std::nullopt;
        }
        constexpr double kTwoTo64 = 18446744073709551616.0;
        if (d >= kTwoTo64) {
            return std::numeric_limits<std::uint64_t>::max();
        }
        return static_cast<std::uint64_t>(d); // in [0, 2^64): a fraction of a byte becomes 0, then the floor
    }
    default:
        return std::nullopt;
    }
}

QString oneDecimal(std::uint64_t bytes, std::uint64_t unit) {
    return QString::number(static_cast<double>(bytes) / static_cast<double>(unit), 'f', 1);
}

// "1,234" (digit groups for the counts of the Stats text, C locale so the tests read the same everywhere).
QString grouped(std::uint64_t n) {
    QString digits = QString::number(n);
    for (qsizetype at = digits.size() - 3; at > 0; at -= 3) {
        digits.insert(at, QLatin1Char(','));
    }
    return digits;
}

std::uint64_t percentOf(std::uint64_t part, std::uint64_t whole) {
    if (whole == 0) {
        return 0;
    }
    const double pct = 100.0 * static_cast<double>(part) / static_cast<double>(whole);
    return pct >= 1e9 ? std::uint64_t{1'000'000'000} : static_cast<std::uint64_t>(pct); // range-checked
}

} // namespace

std::optional<std::size_t> budgetFromSettings(const SettingsStore& settings) {
    const std::optional<std::uint64_t> bytes =
        positiveBytes(settings.value(QString::fromLatin1(kBudgetSettingKey)));
    if (!bytes) {
        return std::nullopt;
    }
    const std::uint64_t capped = std::min<std::uint64_t>(*bytes, std::numeric_limits<std::size_t>::max());
    return std::max(static_cast<std::size_t>(capped), kMinBudgetBytes);
}

QString budgetWarningText(const OpenBudgetCheck& check) {
    return QStringLiteral("needs about %1 of memory once loaded, above the %2 memory budget (%3 in use). "
                          "Continue to open it anyway, or Cancel.")
        .arg(formatBytes(check.estimateBytes), formatBytes(check.budgetBytes),
             formatBytes(check.residentBytes));
}

QString formatBytes(std::uint64_t bytes) {
    if (bytes < kKiB) {
        return bytes == 1 ? QStringLiteral("1 byte") : QStringLiteral("%1 bytes").arg(bytes);
    }
    if (bytes < kMiB - kKiB / 2) { // whole kilobytes up to 1023 KB
        return QStringLiteral("%1 KB").arg((bytes + kKiB / 2) / kKiB);
    }
    if (bytes < 10 * kMiB - kMiB / 20) { // 1.0 MB to 9.9 MB with one decimal
        return oneDecimal(bytes, kMiB) + QStringLiteral(" MB");
    }
    if (bytes < kGiB - kMiB / 2) { // whole megabytes up to 1023 MB
        return QStringLiteral("%1 MB").arg((bytes + kMiB / 2) / kMiB);
    }
    return oneDecimal(bytes, kGiB) + QStringLiteral(" GB");
}

QString performanceLine(const PerformanceSnapshot& snapshot) {
    if (snapshot.budgetBytes == 0) {
        return {}; // no runtime behind it (shut down)
    }
    QString line = QStringLiteral("Resident %1 of %2")
                       .arg(formatBytes(snapshot.cache.residentBytes), formatBytes(snapshot.budgetBytes));
    if (snapshot.cache.residentBytes > snapshot.budgetBytes) {
        line += QStringLiteral(" (over budget)");
    }
    return line;
}

QString performanceDetails(const PerformanceSnapshot& snapshot) {
    if (snapshot.budgetBytes == 0) {
        return {};
    }
    const store::ChunkCache::Stats& c = snapshot.cache;
    const TaskPool::Stats& p = snapshot.pool;
    const std::size_t queued = std::accumulate(p.queued.begin(), p.queued.end(), std::size_t{0});
    return QStringLiteral("Memory: %1 resident of the %2 budget (%3 %), %4 pinned; %5 chunks (%6 pinned); "
                          "%7 hits, %8 misses, %9 evictions. ")
               .arg(formatBytes(c.residentBytes), formatBytes(snapshot.budgetBytes),
                    QString::number(percentOf(c.residentBytes, snapshot.budgetBytes)),
                    formatBytes(c.pinnedBytes), grouped(c.chunks), grouped(c.pinnedChunks), grouped(c.hits),
                    grouped(c.misses), grouped(c.evictions)) +
           QStringLiteral(
               "Tasks: %1 running, %2 queued (%3 interactive, %4 visible, %5 background), %6 parked; "
               "%7 workers.")
               .arg(grouped(p.running), grouped(queued), grouped(p.queued[0]), grouped(p.queued[1]),
                    grouped(p.queued[2]), grouped(p.parked), grouped(p.workers));
}

PerformanceMonitor::PerformanceMonitor(AppRuntime& runtime, QObject* parent)
    : QObject(parent), runtime_(runtime), timer_(new QTimer(this)) {
    timer_->setInterval(intervalMs_);
    connect(timer_, &QTimer::timeout, this, [this] { refresh(); });
}

PerformanceMonitor::~PerformanceMonitor() = default;

void PerformanceMonitor::setInterval(int ms) {
    intervalMs_ = std::max(ms, 100);
    timer_->setInterval(intervalMs_);
}

void PerformanceMonitor::start() {
    timer_->start();
}

void PerformanceMonitor::stop() {
    timer_->stop();
}

bool PerformanceMonitor::running() const {
    return timer_->isActive();
}

PerformanceSnapshot PerformanceMonitor::sample() const {
    PerformanceSnapshot s;
    if (runtime_.isShutDown()) {
        return s; // the pool and the cache are torn down: nothing to read
    }
    s.cache = runtime_.cache().stats();
    s.budgetBytes = runtime_.cache().budgetBytes();
    s.pool = runtime_.pool().stats();
    return s;
}

void PerformanceMonitor::refresh() {
    last_ = sample();
    Q_EMIT updated(last_);
}

} // namespace esses::qt
