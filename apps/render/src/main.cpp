// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// esses-render (04 section 6.7 item 5; 07 section 4.5 demo; AC-M1-05):
//   esses-render <workbook.json> <log...> --worksheet N -o out.png [--dpr 2] [--component K]
//                [--selection fastest|default] [--size WxH] [--scheme light|dark|<file>] [--font-dir DIR]
//                [--timeout S]
// Start-up as esses but with QGuiApplication on the offscreen platform and no NoBlock thread (this tool may
// wait on its own futures, 04 section 1 item 4). The pipeline is the application's: SourceLoader (with the
// workbook profile's rules as the step-6 range rebuild) -> MathsService (built-in Corr Speed / Corr Dist) and
// DataSelectionModel -> DistanceService -> WorksheetViewModel (the worksheet's domain, zoomed to the Main
// range, W) -> RenderService::prepareInputs -> OffscreenRenderer, which composes each component through a
// LayerCache exactly like its widget (AC-M1-05). Exit codes: 0 written, 1 failure, 2 bad arguments.

#include <esses/analysis/rule_runner.hpp>
#include <esses/base/paths.hpp>
#include <esses/maths/channel_key.hpp>
#include <esses/maths/math_status.hpp>
#include <esses/model/ids.hpp>
#include <esses/plotcore/view_state.hpp>
#include <esses/project/profile.hpp>
#include <esses/project/scheme.hpp>
#include <esses/project/shipped_data.hpp>
#include <esses/project/workbook_file.hpp>
#include <esses/qtmodel/app_runtime.hpp>
#include <esses/qtmodel/data_selection_model.hpp>
#include <esses/qtmodel/distance_service.hpp>
#include <esses/qtmodel/maths_service.hpp>
#include <esses/qtmodel/render_service.hpp>
#include <esses/qtmodel/source_loader.hpp>
#include <esses/qtmodel/worksheet_view_model.hpp>
#include <esses/qtmodel/workspace_view_model.hpp>
#include <esses/qtrender/offscreen_renderer.hpp>
#include <esses/qtrender/qt_text_metrics.hpp>
#include <esses/qtrender/scheme_fonts.hpp>
#include <esses/units/unit_registry.hpp>

#include <QCommandLineParser>
#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFileInfo>
#include <QGuiApplication>
#include <QImage>
#include <QRegularExpression>
#include <QTextStream>
#include <QTimer>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace {

using namespace esses;

constexpr int kExitOk = 0;
constexpr int kExitFailure = 1;
constexpr int kExitUsage = 2;

std::filesystem::path toPath(const QString& s) {
    return std::filesystem::path(s.toStdWString());
}

QString fromStd(const std::string& s) {
    return QString::fromStdString(s);
}

// The value of `--font-dir` (or ESSES_TEST_FONT_DIR), read before the application exists so the font
// database can be pointed at it.
QByteArray fontDirArgument(int argc, char** argv) {
    for (int i = 1; i < argc; ++i) {
        const char* a = argv[i];
        if (std::strcmp(a, "--font-dir") == 0 && i + 1 < argc) {
            return QByteArray(argv[i + 1]);
        }
        if (std::strncmp(a, "--font-dir=", 11) == 0) {
            return QByteArray(a + 11);
        }
    }
    return qgetenv("ESSES_TEST_FONT_DIR");
}

// Runs the event loop until `done` or the deadline; false on timeout.
bool waitFor(const std::function<bool()>& done, const QElapsedTimer& clock, qint64 timeoutMs) {
    while (!done()) {
        if (clock.elapsed() > timeoutMs) {
            return false;
        }
        QEventLoop loop;
        QTimer::singleShot(10, &loop, &QEventLoop::quit);
        loop.exec();
    }
    return true;
}

std::optional<QSize> parseSize(const QString& text) {
    static const QRegularExpression re(QStringLiteral("^(\\d{1,5})[xX](\\d{1,5})$"));
    const QRegularExpressionMatch m = re.match(text);
    if (!m.hasMatch()) {
        return std::nullopt;
    }
    const QSize size(m.captured(1).toInt(), m.captured(2).toInt());
    if (size.width() < 1 || size.height() < 1 || size.width() > 16384 || size.height() > 16384) {
        return std::nullopt;
    }
    return size;
}

analysis::RuleRunner rulesFor(const Paths& paths, const std::string& profileName, QTextStream& err) {
    std::vector<std::string> log;
    const std::filesystem::path profilePath =
        paths.dataDir() / "profiles" / std::filesystem::path(profileName + ".profile.json");
    auto profile = project::Profile::load(profilePath, log);
    if (profile) {
        std::vector<std::string> skipped;
        auto runner = analysis::RuleRunner::fromRuleIds(project::shippedFile(paths, project::kRulesDir),
                                                        profile->rules, &skipped);
        if (runner) {
            for (const std::string& s : skipped) {
                err << "esses-render: rule " << fromStd(s) << " is not implemented; skipped\n";
            }
            return std::move(runner).value();
        }
        err << "esses-render: " << fromStd(runner.error().message) << "; using the built-in circuit rules\n";
    } else {
        err << "esses-render: profile " << fromStd(profileName) << ": " << fromStd(profile.error().message)
            << "; using the built-in circuit rules\n";
    }
    return analysis::RuleRunner::circuitDefault();
}

} // namespace

int main(int argc, char** argv) {
    if (qEnvironmentVariableIsEmpty("QT_QPA_PLATFORM")) {
        qputenv("QT_QPA_PLATFORM", "offscreen");
    }
    // The offscreen platform's FreeType font database (Windows) sees only QT_QPA_FONTDIR and application
    // fonts: point it at --font-dir (deterministic goldens, 04 section 6.7 item 2) or at the system fonts.
    const QByteArray fontDir = fontDirArgument(argc, argv);
    if (qEnvironmentVariableIsEmpty("QT_QPA_FONTDIR")) {
        if (!fontDir.isEmpty()) {
            qputenv("QT_QPA_FONTDIR", fontDir);
        }
#ifdef Q_OS_WIN
        else if (!qEnvironmentVariableIsEmpty("WINDIR")) {
            qputenv("QT_QPA_FONTDIR", qgetenv("WINDIR") + "\\Fonts");
        }
#endif
    }

    QGuiApplication app(argc, argv);
    QGuiApplication::setApplicationName(QStringLiteral("esses-render"));
    QTextStream err(stderr);
    QTextStream out(stdout);

    QCommandLineParser parser;
    parser.setApplicationDescription(QStringLiteral("Render a worksheet of a workbook to a PNG image."));
    parser.addHelpOption();
    parser.addPositionalArgument(QStringLiteral("workbook"),
                                 QStringLiteral("The workbook (*.workbook.json)."));
    parser.addPositionalArgument(QStringLiteral("logs"), QStringLiteral("One or more log files."),
                                 QStringLiteral("<log...>"));
    const QCommandLineOption worksheetOption(QStringLiteral("worksheet"), QStringLiteral("Worksheet index."),
                                             QStringLiteral("N"), QStringLiteral("0"));
    const QCommandLineOption outputOption(QStringList{QStringLiteral("o"), QStringLiteral("output")},
                                          QStringLiteral("Output PNG file."), QStringLiteral("file"));
    const QCommandLineOption dprOption(QStringLiteral("dpr"),
                                       QStringLiteral("Device pixel ratio (0.5 to 8)."),
                                       QStringLiteral("ratio"), QStringLiteral("1"));
    const QCommandLineOption componentOption(
        QStringLiteral("component"),
        QStringLiteral("Render only component K of the worksheet, at its size in the worksheet."),
        QStringLiteral("K"));
    const QCommandLineOption selectionOption(
        QStringLiteral("selection"),
        QStringLiteral("Main range: fastest (the fastest trusted lap, CTRL+F; default) or default (the "
                       "selection a newly opened log gets)."),
        QStringLiteral("rule"), QStringLiteral("fastest"));
    const QCommandLineOption sizeOption(
        QStringLiteral("size"), QStringLiteral("Worksheet size in logical pixels (default 1600x900)."),
        QStringLiteral("WxH"), QStringLiteral("1600x900"));
    const QCommandLineOption schemeOption(QStringLiteral("scheme"),
                                          QStringLiteral("Colour scheme: light (default), dark or a "
                                                         "*.scheme.json file."),
                                          QStringLiteral("scheme"), QStringLiteral("light"));
    const QCommandLineOption fontDirOption(
        QStringLiteral("font-dir"),
        QStringLiteral("Register the fonts of DIR and use only them (default: ESSES_TEST_FONT_DIR)."),
        QStringLiteral("DIR"));
    const QCommandLineOption timeoutOption(QStringLiteral("timeout"),
                                           QStringLiteral("Seconds to wait for the logs and maths (default "
                                                          "120)."),
                                           QStringLiteral("S"), QStringLiteral("120"));
    parser.addOptions({worksheetOption, outputOption, dprOption, componentOption, selectionOption, sizeOption,
                       schemeOption, fontDirOption, timeoutOption});
    parser.process(app);

    const QStringList positional = parser.positionalArguments();
    if (positional.size() < 2 || !parser.isSet(outputOption)) {
        err << "esses-render: a workbook, at least one log and -o <file> are required\n";
        return kExitUsage;
    }
    bool ok = false;
    const int worksheet = parser.value(worksheetOption).toInt(&ok);
    if (!ok || worksheet < 0) {
        err << "esses-render: invalid --worksheet " << parser.value(worksheetOption) << '\n';
        return kExitUsage;
    }
    const double dpr = parser.value(dprOption).toDouble(&ok);
    if (!ok || !std::isfinite(dpr) || dpr < 0.5 || dpr > 8.0) {
        err << "esses-render: invalid --dpr " << parser.value(dprOption) << " (0.5 to 8)\n";
        return kExitUsage;
    }
    std::optional<int> component;
    if (parser.isSet(componentOption)) {
        const int k = parser.value(componentOption).toInt(&ok);
        if (!ok || k < 0) {
            err << "esses-render: invalid --component " << parser.value(componentOption) << '\n';
            return kExitUsage;
        }
        component = k;
    }
    const QString selectionRule = parser.value(selectionOption);
    if (selectionRule != QLatin1String("fastest") && selectionRule != QLatin1String("default")) {
        err << "esses-render: invalid --selection " << selectionRule << " (fastest or default)\n";
        return kExitUsage;
    }
    const std::optional<QSize> logicalSize = parseSize(parser.value(sizeOption));
    if (!logicalSize) {
        err << "esses-render: invalid --size " << parser.value(sizeOption) << " (WxH)\n";
        return kExitUsage;
    }
    const int timeoutS = parser.value(timeoutOption).toInt(&ok);
    if (!ok || timeoutS < 1) {
        err << "esses-render: invalid --timeout " << parser.value(timeoutOption) << '\n';
        return kExitUsage;
    }
    const QString fontDirValue =
        parser.isSet(fontDirOption) ? parser.value(fontDirOption) : QString::fromLocal8Bit(fontDir);
    if (!fontDirValue.isEmpty() && qt::SchemeFonts::registerApplicationFonts(toPath(fontDirValue)) == 0) {
        err << "esses-render: no .ttf/.otf font in " << fontDirValue << '\n';
        return kExitUsage;
    }

    auto paths = Paths::resolve();
    if (!paths) {
        err << "esses-render: " << fromStd(paths.error().message) << '\n';
        return kExitFailure;
    }
    auto unitsLoaded = units::UnitRegistry::loadFiles(paths->dataDir() / "units.json");
    if (!unitsLoaded) {
        err << "esses-render: units: " << fromStd(unitsLoaded.error().message) << '\n';
        return kExitFailure;
    }
    const auto units = std::make_shared<const units::UnitRegistry>(std::move(unitsLoaded).value());

    std::vector<std::string> log;
    auto workbookLoaded = project::WorkbookFile::load(toPath(positional.front()), log);
    if (!workbookLoaded) {
        err << "esses-render: " << positional.front() << ": " << fromStd(workbookLoaded.error().message)
            << '\n';
        return kExitFailure;
    }
    const project::WorkbookFile workbook = std::move(workbookLoaded).value();
    if (static_cast<std::size_t>(worksheet) >= workbook.worksheets.size()) {
        err << "esses-render: --worksheet " << worksheet << " out of range (the workbook has "
            << workbook.worksheets.size() << ")\n";
        return kExitUsage;
    }
    const project::WorksheetDef& sheet = workbook.worksheets[static_cast<std::size_t>(worksheet)];
    if (component && static_cast<std::size_t>(*component) >= sheet.components.size()) {
        err << "esses-render: --component " << *component << " out of range (the worksheet has "
            << sheet.components.size() << ")\n";
        return kExitUsage;
    }

    const QString schemeValue = parser.value(schemeOption);
    std::filesystem::path schemePath;
    if (schemeValue == QLatin1String("light")) {
        schemePath = project::shippedFile(*paths, project::kLightScheme);
    } else if (schemeValue == QLatin1String("dark")) {
        schemePath = project::shippedFile(*paths, project::kDarkScheme);
    } else {
        schemePath = toPath(schemeValue);
    }
    auto schemeLoaded = project::Scheme::load(schemePath, log);
    if (!schemeLoaded) {
        err << "esses-render: scheme " << schemeValue << ": " << fromStd(schemeLoaded.error().message)
            << '\n';
        return kExitFailure;
    }
    const auto scheme = std::make_shared<const project::Scheme>(std::move(schemeLoaded).value());
    const auto fonts = std::make_shared<const qt::SchemeFonts>(*scheme, 1);
    const auto metrics = qt::QtTextMetrics::create(*scheme, 1); // after the fonts are registered (04 5.4)

    qt::AppRuntime rt; // no NoBlock thread: this tool waits on its own futures (04 section 1 item 4)
    int rc = kExitOk;
    {
        qt::MathsService maths(rt.pool(), rt.dispatcher(), units);
        qt::DistanceService distance(maths, rt.dispatcher());
        qt::DataSelectionModel selection(nullptr);
        qt::WorkspaceViewModel workspace;
        qt::SourceLoader loader(rt.pool(), rt.dispatcher(), units);
        loader.setRuleRunner(rulesFor(*paths, workbook.profile, err));
        qt::RenderService render(rt.pool(), rt.dispatcher(), selection, maths, distance);

        int pendingOpens = 0;
        QStringList failures;
        std::vector<std::shared_ptr<const model::DataSource>> sources;
        QObject::connect(&loader, &qt::SourceLoader::sourcePublished,
                         [&](std::shared_ptr<const model::DataSource> source) {
                             maths.addSource(source);
                             selection.sourcePublished(source);
                             distance.prefetch(source->id);
                             sources.push_back(std::move(source));
                             --pendingOpens;
                         });
        QObject::connect(&loader, &qt::SourceLoader::openFailed,
                         [&](const QString& path, const QString& msg) {
                             failures << path + QStringLiteral(": ") + msg;
                             --pendingOpens;
                         });
        QObject::connect(&selection, &qt::DataSelectionModel::hint,
                         [&](const QString& text) { err << "esses-render: " << text << '\n'; });

        QElapsedTimer clock;
        clock.start();
        const qint64 timeoutMs = static_cast<qint64>(timeoutS) * 1000;
        for (auto it = positional.begin() + 1; it != positional.end(); ++it) {
            ++pendingOpens;
            loader.open(toPath(*it));
        }
        if (!waitFor([&] { return pendingOpens <= 0; }, clock, timeoutMs)) {
            err << "esses-render: timed out opening the logs\n";
            rc = kExitFailure;
        }
        for (const QString& f : failures) {
            err << "esses-render: " << f << '\n';
        }
        if (rc == kExitOk && sources.empty()) {
            err << "esses-render: no log could be opened\n";
            rc = kExitFailure;
        }

        if (rc == kExitOk) {
            if (selectionRule == QLatin1String("fastest")) {
                selection.selectFastest();
            }
            // Corr Speed / Corr Dist of every source (D-016, AC-M1-10), then the distance indexes of the
            // visible ranges; a source without the inputs ends in Error/NoDistance and renders its banner.
            std::vector<maths::ChannelKey> pendingMaths;
            for (const auto& s : sources) {
                for (const char* name : {"Corr Speed", "Corr Dist"}) {
                    if (maths.isMathsChannel(s->id, model::kNormalLayer, name)) {
                        pendingMaths.push_back(maths::ChannelKey::make(s->id, model::kNormalLayer, name));
                    }
                }
            }
            const auto mathsDone = [&] {
                for (const maths::ChannelKey& key : pendingMaths) {
                    const maths::MathState st = maths.status(key).state;
                    if (st == maths::MathState::Error || st == maths::MathState::Disabled) {
                        continue;
                    }
                    if (!maths.require(key, Priority::Interactive)) {
                        return false;
                    }
                }
                return true;
            };
            const auto distanceDone = [&] {
                for (const plot::Slot& slot : selection.snapshot().visibleSlots()) {
                    if (distance.chain(slot.range, Priority::Interactive) != nullptr) {
                        continue;
                    }
                    const qt::DistanceState st = distance.state(slot.range.src);
                    if (st != qt::DistanceState::Ready && st != qt::DistanceState::NoDistance) {
                        return false;
                    }
                }
                return true;
            };
            if (!waitFor([&] { return mathsDone() && distanceDone(); }, clock, timeoutMs)) {
                err << "esses-render: timed out waiting for Corr Speed / Corr Dist; rendering what is "
                       "ready\n";
            }
        }

        if (rc == kExitOk) {
            qt::WorksheetViewModel view(sheet, workspace, selection, distance);
            view.onSelectionChanged(selection.snapshot());
            view.setSize(static_cast<int>(std::lround(logicalSize->width() * dpr)));
            view.zoomToMainRange(); // W: the Main range, as the application shows a newly selected lap
            const plot::ViewState viewState = view.state();
            const plot::SelectionSnapshot snapshot = selection.snapshot();
            const qt::OffscreenRenderer::InputsProvider provider = [&](const project::ComponentSpec& spec,
                                                                       const plot::LayoutContext& layout) {
                return render.prepareInputs(spec, viewState, snapshot, layout, scheme, metrics);
            };

            qt::OffscreenRenderer renderer(fonts, &rt.pool());
            QImage image;
            if (component) {
                const project::ComponentSpec& spec = sheet.components[static_cast<std::size_t>(*component)];
                const int w = logicalSize->width();
                const int h = logicalSize->height();
                const QSize size(
                    static_cast<int>(std::lround(spec.rect.r * w) - std::lround(spec.rect.l * w)),
                    static_cast<int>(std::lround(spec.rect.b * h) - std::lround(spec.rect.t * h)));
                if (size.width() > 0 && size.height() > 0) {
                    const QSize device(static_cast<int>(std::lround(size.width() * dpr)),
                                       static_cast<int>(std::lround(size.height() * dpr)));
                    image = renderer.renderComponent(
                        provider(spec, plot::LayoutContext{device.width(), device.height(),
                                                           static_cast<float>(dpr)}),
                        size, dpr);
                }
            } else {
                image = renderer.render(workbook, worksheet, provider, *logicalSize, dpr);
            }
            const QString output = parser.value(outputOption);
            if (image.isNull()) {
                err << "esses-render: nothing to render (empty component or worksheet size)\n";
                rc = kExitFailure;
            } else if (!image.save(output, "PNG")) {
                err << "esses-render: cannot write " << output << '\n';
                rc = kExitFailure;
            } else {
                out << "esses-render: wrote "
                    << QDir::toNativeSeparators(QFileInfo(output).absoluteFilePath()) << " (" << image.width()
                    << 'x' << image.height() << " px, worksheet " << worksheet << ", dpr " << dpr << ")\n";
            }
        }
    }
    rt.shutdown();
    return rc;
}
