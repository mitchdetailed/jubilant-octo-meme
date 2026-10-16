// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
#include "fixture.hpp"

#include "cli_common.hpp"
#include "expect_channels.hpp"
#include "inspect.hpp"
#include "process.hpp"
#include "sha256.hpp"
#include "validate.hpp"

#include <esses/base/casefold.hpp>
#include <esses/base/env.hpp>
#include <esses/base/priority.hpp>
#include <esses/formats/byte_source.hpp>
#include <esses/model/markers_document.hpp>
#include <esses/store/column.hpp>
#include <esses/store/series.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <map>
#include <ostream>
#include <set>
#include <sstream>
#include <system_error>

namespace esses::cli {
namespace {

namespace fs = std::filesystem;
using nlohmann::json;

constexpr std::size_t kProblemsPerChannel = 5;

std::string lowerExt(const fs::path& p) {
    std::string e = u8(p.extension());
    for (char& c : e) {
        if (c >= 'A' && c <= 'Z') {
            c = static_cast<char>(c - 'A' + 'a');
        }
    }
    return e;
}

bool pathExists(const fs::path& p) {
    std::error_code ec;
    return fs::exists(p, ec);
}

bool sameFile(const fs::path& a, const fs::path& b) {
    std::error_code ec;
    const bool same = fs::equivalent(a, b, ec);
    return !ec && same;
}

Result<void> copyFile(const fs::path& from, const fs::path& to) {
    std::error_code ec;
    fs::create_directories(to.parent_path(), ec);
    fs::copy_file(from, to, fs::copy_options::overwrite_existing, ec);
    if (ec) {
        ErrorContext c;
        c.path = from;
        return fail(ErrorCode::Io, "cannot copy " + u8(from) + " to " + u8(to) + ": " + ec.message(),
                    std::move(c));
    }
    return ok();
}

json defaultImportOptions(std::string_view format) {
    if (format == "motec.ld") {
        return json{{"ld", json{{"shiftOrder", "afterMul"}}}}; // D-037, stated explicitly in every fixture
    }
    return {};
}

std::string shiftOrderOf(const json& importOptions) {
    if (importOptions.is_object() && importOptions.contains("ld") && importOptions["ld"].is_object() &&
        importOptions["ld"].contains("shiftOrder") && importOptions["ld"]["shiftOrder"].is_string()) {
        return importOptions["ld"]["shiftOrder"].get<std::string>();
    }
    return "afterMul";
}

json toleranceJson() {
    return json{{"F32", "1ulp"}, {"F64", "1ulp"}, {"I32", "exact"}, {"timesUs", "exact"}};
}

Result<std::string> markersText(const model::DataSource& src) {
    return model::MarkersDocument::fromSource(src).toJson();
}

Result<std::string> lapsText(const model::DataSource& src) {
    return canonicalText(lapsJson(src));
}

fs::path uniqueTempDir(std::string_view tag) {
    static std::atomic<unsigned> counter{0};
    const auto now = std::chrono::steady_clock::now().time_since_epoch().count();
    std::error_code ec;
    fs::path base = fs::temp_directory_path(ec);
    if (ec) {
        base = fs::current_path();
    }
    return base /
           p8("esses-" + std::string(tag) + "-" + std::to_string(now) + "-" + std::to_string(++counter));
}

// First differing line of two texts, for a readable mismatch report.
std::string firstDifference(std::string_view expected, std::string_view actual) {
    std::size_t line = 1;
    std::size_t a = 0;
    std::size_t b = 0;
    while (a < expected.size() || b < actual.size()) {
        const std::size_t ea = std::min(expected.find('\n', a), expected.size());
        const std::size_t eb = std::min(actual.find('\n', b), actual.size());
        const std::string_view la =
            a < expected.size() ? expected.substr(a, ea - a) : std::string_view("<end>");
        const std::string_view lb = b < actual.size() ? actual.substr(b, eb - b) : std::string_view("<end>");
        if (la != lb) {
            const auto cut = [](std::string_view s) {
                return std::string(s.substr(0, 160)) + (s.size() > 160 ? "..." : "");
            };
            return "line " + std::to_string(line) + ": expected '" + cut(la) + "', got '" + cut(lb) + "'";
        }
        a = ea + 1;
        b = eb + 1;
        ++line;
    }
    return "identical";
}

std::string numberText(double v) {
    std::ostringstream os;
    os.precision(17);
    os << v;
    return os.str();
}

} // namespace

std::string defaultPython() {
    if (const auto v = getEnv(envName("PYTHON")); v && !v->empty()) {
        return *v;
    }
#if defined(_WIN32)
    return "python";
#else
    return "python3";
#endif
}

fs::path defaultRefDecode() {
    if (const auto v = getEnv(envName("REF_DECODE")); v && !v->empty()) {
        return p8(*v);
    }
    return AppContext::sourceDir() / "tools" / "ld_ref_decode.py";
}

// ---- fixture add
// -------------------------------------------------------------------------------------------

namespace {

struct OracleRun {
    std::string kind;
    std::string ref;
    std::size_t channels = 0;
};

Result<OracleRun> runRefDecode(const FixtureAddOptions& o, const fs::path& input, const std::string& order,
                               const fs::path& channelsDir) {
    const fs::path tool = o.refDecode ? *o.refDecode : defaultRefDecode();
    if (!pathExists(tool)) {
        return fail(ErrorCode::NotFound,
                    "the reference decoder " + u8(tool) + " does not exist (--ref-decode)");
    }
    const fs::path tmp = uniqueTempDir("ref-decode");
    std::error_code ec;
    fs::create_directories(tmp / "dumps", ec);
    ProcessRequest req;
    req.program = o.python.empty() ? defaultPython() : o.python;
    req.args = {"-I",
                u8(tool),
                u8(input),
                "--json",
                "-o",
                u8(tmp / "report.json"),
                "--no-stats",
                "--no-hash",
                "--dump-format",
                "csv",
                "--shift-order",
                order,
                "--dump-dir",
                u8(tmp / "dumps")};
    if (o.channels.empty()) {
        req.args.insert(req.args.end(), {"--dump-channel", "*"});
    } else {
        for (const std::string& c : o.channels) {
            req.args.insert(req.args.end(), {"--dump-channel", c});
        }
    }
    req.stdoutFile = tmp / "stdout.txt";
    req.stderrFile = tmp / "stderr.txt";
    auto rc = runProcess(req);
    const auto cleanup = [&] {
        std::error_code e2;
        fs::remove_all(tmp, e2);
    };
    if (!rc) {
        cleanup();
        return Failure{rc.error()};
    }
    if (*rc != 0) {
        const auto errText = readFile(tmp / "stderr.txt");
        cleanup();
        return fail(ErrorCode::Internal, "the reference decoder exited with " + std::to_string(*rc) + ": " +
                                             (errText ? *errText : std::string()));
    }
    OracleRun run;
    run.kind = "ref-decode";
    std::string version = "?";
    if (auto report = loadJson(tmp / "report.json"); report && report->is_object()) {
        version = report->value("tool_version", std::string("?"));
    }
    run.ref = "tools/ld_ref_decode.py " + version + " (MIT), " + order;
    std::vector<fs::path> metas;
    for (fs::directory_iterator it(tmp / "dumps", ec), end; !ec && it != end; it.increment(ec)) {
        if (it->path().extension() == ".json") {
            metas.push_back(it->path());
        }
    }
    std::sort(metas.begin(), metas.end());
    for (const fs::path& m : metas) {
        auto meta = loadJson(m);
        if (!meta || !meta->is_object()) {
            cleanup();
            return fail(ErrorCode::Format, "unreadable oracle dump description " + u8(m));
        }
        const std::string name = meta->value("name", std::string());
        const auto index = meta->value("index", 0u);
        const json files = meta->value("files", json::object());
        const std::string key = order + ".csv";
        if (!files.contains(key)) {
            cleanup();
            return fail(ErrorCode::Format,
                        std::string("the oracle dump of '").append(name).append("' has no ").append(key));
        }
        auto text = readFile(tmp / "dumps" / p8(files[key].get<std::string>()));
        if (!text) {
            cleanup();
            return Failure{text.error()};
        }
        if (auto w = writeFile(channelsDir / p8(expectedFileName(index, name)), *text); !w) {
            cleanup();
            return Failure{w.error()};
        }
        ++run.channels;
    }
    cleanup();
    return run;
}

Result<OracleRun> copyOracleDir(const FixtureAddOptions& o, const fs::path& channelsDir) {
    OracleRun run;
    run.kind = o.oracleKind.empty() ? "public-project" : o.oracleKind;
    run.ref = o.oracleRef.empty() ? u8(fs::path(o.oracle).filename()) : o.oracleRef;
    std::error_code ec;
    for (fs::directory_iterator it(p8(o.oracle), ec), end; !ec && it != end; it.increment(ec)) {
        if (!parseExpectedFileName(u8(it->path().filename()))) {
            continue;
        }
        if (auto r = copyFile(it->path(), channelsDir / it->path().filename()); !r) {
            return Failure{r.error()};
        }
        ++run.channels;
    }
    return run;
}

} // namespace

int runFixtureAdd(AppContext& ctx, const FixtureAddOptions& o, std::ostream& out, std::ostream& err) {
    const auto bad = [&](const std::string& message) {
        err << "esses-cli fixture add: " << message << "\n";
        return 2;
    };
    auto reg = ctx.registry();
    if (!reg) {
        return bad(reg.error().toString());
    }
    const formats::IImporter* importer = (*reg)->find(o.format);
    if (importer == nullptr) {
        return bad("unknown format '" + o.format + "'");
    }
    if (!pathExists(o.input)) {
        return bad("no such file: " + u8(o.input));
    }
    const bool refDecode = o.oracle == "ref-decode";
    std::error_code ec;
    if (!refDecode && !fs::is_directory(p8(o.oracle), ec)) {
        return bad("--oracle must be ref-decode or a directory of expected channel CSVs");
    }
    // Checked before anything is written, so a refused add leaves no fixture directory behind.
    if (!o.isPrivate && (o.licence.empty() || o.licence == "LicenseRef-unknown")) {
        return bad("a public fixture needs --licence with a licence that permits redistribution (fixture-1 "
                   "refuses LicenseRef-unknown unless the fixture is --private)");
    }
    if (!o.validity.empty() && o.validity != "strict" && o.validity != "finite-only") {
        return bad("--validity must be strict or finite-only");
    }
    // oracle.validity: tools/ld_ref_decode.py does not decode gap blocks, so its CSVs can assert finiteness
    // only; strict needs someone who supplies the expected validity (esses-gen-fixtures passes --validity
    // strict and writes it).
    const std::string validity = !o.validity.empty() ? o.validity : refDecode ? "finite-only" : "strict";
    fs::create_directories(o.out, ec);
    if (ec) {
        return bad("cannot create " + u8(o.out) + ": " + ec.message());
    }

    // The input and its companions, renamed to the input.<ext> stem (companion discovery is by stem).
    const fs::path target = o.out / p8("input" + lowerExt(o.input));
    std::vector<fs::path> companions;
    if (auto ranked = (*reg)->sniff(o.input); ranked) {
        for (const auto& [imp, sr] : *ranked) {
            if (imp == importer) {
                companions = sr.companions;
            }
        }
    }
    if (!sameFile(o.input, target)) {
        for (fs::directory_iterator it(o.out, ec), end; !ec && it != end; it.increment(ec)) {
            if (it->is_regular_file() && it->path().stem() == "input") {
                std::error_code e2;
                fs::remove(it->path(), e2);
            }
        }
        if (auto r = copyFile(o.input, target); !r) {
            return bad(r.error().toString());
        }
    }
    std::vector<fs::path> companionTargets;
    for (const fs::path& c : companions) {
        const fs::path dst = o.out / p8("input" + lowerExt(c));
        if (!sameFile(c, dst)) {
            if (auto r = copyFile(c, dst); !r) {
                return bad(r.error().toString());
            }
        }
        companionTargets.push_back(dst);
    }

    // Import options: stated explicitly (the motec.ld default order included, D-037).
    json importOptions = defaultImportOptions(o.format);
    if (o.optionsJson) {
        auto j = parseJson(*o.optionsJson);
        if (!j) {
            return bad("--options: " + j.error().message);
        }
        importOptions = *j;
    }
    auto opts = makeImportOptionsJson(o.format, importOptions);
    if (!opts) {
        return bad(opts.error().toString());
    }

    // expected.probe.json, .markers.json, .laps.json from the reader (metadata only).
    auto probed = probeFile(ctx, target, *opts);
    if (!probed) {
        return bad(u8(target) + ": " + probed.error().toString());
    }
    auto probeText = probed->probe.canonicalJson();
    if (!probeText) {
        return bad(probeText.error().toString());
    }
    if (auto w = writeFile(o.out / "expected.probe.json", *probeText); !w) {
        return bad(w.error().toString());
    }
    {
        auto opened = openFile(ctx, target, *opts, false, false);
        if (!opened) {
            return bad(u8(target) + ": " + opened.error().toString());
        }
        auto mt = markersText(*opened->source);
        auto lt = lapsText(*opened->source);
        if (!mt || !lt) {
            return bad((mt ? lt.error() : mt.error()).toString());
        }
        if (auto w = writeFile(o.out / "expected.markers.json", *mt); !w) {
            return bad(w.error().toString());
        }
        if (auto w = writeFile(o.out / "expected.laps.json", *lt); !w) {
            return bad(w.error().toString());
        }
    }
    ctx.cache().clear();

    // expected.channels/ from the oracle (D-041).
    const fs::path channelsDir = o.out / "expected.channels";
    fs::remove_all(channelsDir, ec);
    fs::create_directories(channelsDir, ec);
    auto oracle = refDecode ? runRefDecode(o, target, shiftOrderOf(importOptions), channelsDir)
                            : copyOracleDir(o, channelsDir);
    if (!oracle) {
        return bad(oracle.error().toString());
    }

    // fixture.json (fixture-1).
    json fx = json::object();
    fx["$schema"] = "urn:esses:fixture-1";
    fx["schemaVersion"] = 1;
    fx["format"] = o.format;
    fx["name"] = o.name.empty() ? u8(fs::absolute(o.out).lexically_normal().filename()) : o.name;
    if (fx["name"].get<std::string>().empty()) {
        fx["name"] = u8(fs::absolute(o.out).lexically_normal().parent_path().filename());
    }
    auto inputSha = sha256File(target);
    if (!inputSha) {
        return bad(inputSha.error().toString());
    }
    fx["input"] =
        json{{"file", u8(target.filename())}, {"sha256", *inputSha}, {"bytes", fs::file_size(target, ec)}};
    if (!companionTargets.empty()) {
        json cs = json::array();
        for (const fs::path& c : companionTargets) {
            auto sha = sha256File(c);
            if (!sha) {
                return bad(sha.error().toString());
            }
            cs.push_back(json{{"file", u8(c.filename())}, {"sha256", *sha}, {"bytes", fs::file_size(c, ec)}});
        }
        fx["companions"] = std::move(cs);
    }
    fx["source"] = o.source.empty()
                       ? std::string("added by esses-cli fixture add from ") + u8(o.input.filename())
                       : o.source;
    if (!o.provenance.empty()) {
        fx["provenance"] = o.provenance;
    }
    fx["licence"] = o.licence.empty() ? std::string("LicenseRef-unknown") : o.licence;
    if (!o.permission.empty()) {
        fx["permission"] = o.permission;
    }
    fx["private"] = o.isPrivate;
    if (!importOptions.is_null()) {
        fx["importOptions"] = importOptions;
    }
    fx["dialect"] = probed->probe.dialect;
    json oj = json{{"kind", oracle->kind}, {"ref", oracle->ref}, {"tolerance", toleranceJson()}};
    oj["validity"] = validity; // written even when it is the schema default, so the fixture says it
    fx["oracle"] = std::move(oj);
    if (o.channels.empty()) {
        fx["channelsCompared"] = "all";
    } else {
        fx["channelsCompared"] = o.channels;
    }
    if (!o.notes.empty()) {
        fx["notes"] = o.notes;
    }
    auto fxText = canonicalText(fx);
    if (!fxText) {
        return bad(fxText.error().toString());
    }
    if (auto w = writeFile(o.out / "fixture.json", *fxText); !w) {
        return bad(w.error().toString());
    }

    // The documents just written must satisfy their schemas.
    int rc = 0;
    if (auto dir = ctx.schemaDir(); dir) {
        SchemaStore store(*dir);
        for (const char* f :
             {"fixture.json", "expected.probe.json", "expected.markers.json", "expected.laps.json"}) {
            auto doc = loadJson(o.out / f);
            if (!doc) {
                err << "fixture add: " << doc.error().toString() << "\n";
                rc = 1;
                continue;
            }
            auto errs = store.validateDocument(*doc);
            if (!errs) {
                err << "fixture add: " << f << ": " << errs.error().toString() << "\n";
                rc = 1;
                continue;
            }
            for (const SchemaError& e : *errs) {
                err << "fixture add: " << f << " at '" << e.pointer << "': " << e.message << "\n";
                rc = 1;
            }
        }
    } else {
        err << "fixture add: " << dir.error().toString() << " (expectations not schema-checked)\n";
    }
    if (o.json) {
        const json summary = json{{"dir", u8(o.out)},
                                  {"name", fx["name"]},
                                  {"dialect", probed->probe.dialect},
                                  {"channels", oracle->channels},
                                  {"oracle", json{{"kind", oracle->kind}, {"ref", oracle->ref}}},
                                  {"schemaValid", rc == 0}};
        if (auto text = canonicalText(summary); text) {
            out << *text;
        }
    } else if (!o.quiet) {
        out << "fixture add: " << u8(o.out) << ": " << oracle->channels << " channel(s) from " << oracle->kind
            << ", dialect '" << probed->probe.dialect << "'\n";
    }
    return rc;
}

// ---- fixture verify ------------------------------------------------------------------------------------

namespace {

struct Target {
    fs::path dir;
    std::optional<fs::path> input; // from a manifest
};

// fixture diff: explain every difference instead of listing the first few (05 section 9.2).
struct Explain {
    bool on = false;
    std::string channel; // case-folded; "" = every channel
};

// The JSON Pointer paths where two JSON texts differ (nlohmann's JSON Patch from `expected` to `actual`), at
// most `limit` of them: "<op> at '<path>': expected <value>, got <value>".
std::vector<std::string> jsonDifferences(std::string_view expected, std::string_view actual,
                                         std::size_t limit) {
    std::vector<std::string> out;
    const json e = json::parse(expected, nullptr, false);
    const json a = json::parse(actual, nullptr, false);
    if (e.is_discarded() || a.is_discarded()) {
        return out;
    }
    const auto shortText = [](const json& v) {
        std::string t = v.dump();
        return t.size() > 120 ? t.substr(0, 117) + "..." : t;
    };
    const json patch = json::diff(e, a);
    for (const json& op : patch) {
        if (out.size() == limit) {
            out.push_back("... and " + std::to_string(patch.size() - limit) + " more");
            break;
        }
        const std::string path = op.value("path", std::string());
        const std::string kind = op.value("op", std::string());
        const json::json_pointer ptr(path);
        std::string line = kind;
        line += " at '";
        line += path;
        line += "'";
        if (kind == "replace" || kind == "remove") {
            line += ": expected " + (e.contains(ptr) ? shortText(e.at(ptr)) : std::string("<absent>"));
        }
        if (kind == "replace" || kind == "add") {
            line += std::string(kind == "add" ? ": " : ", ") + "got " + shortText(op.value("value", json()));
        }
        out.push_back(std::move(line));
    }
    return out;
}

enum class Status : std::uint8_t { Pass, Fail, Skip };

struct Outcome {
    std::string id;
    fs::path dir;
    Status status = Status::Pass;
    std::string skipReason;
    std::vector<std::string> problems;
    std::vector<std::string> notes;
    std::size_t channels = 0;
    std::uint64_t samples = 0;
    std::uint64_t readerInvalid = 0;
    std::uint64_t maxF32Ulps = 0;
    bool updated = false;
};

void findFixtures(const fs::path& root, std::vector<fs::path>& out) {
    std::error_code ec;
    if (pathExists(root / "fixture.json")) {
        out.push_back(root);
    }
    std::vector<fs::path> subdirs;
    for (fs::directory_iterator it(root, ec), end; !ec && it != end; it.increment(ec)) {
        std::error_code e2;
        const std::string name = u8(it->path().filename());
        if (it->is_directory(e2) && !name.empty() && name.front() != '.' && name != "expected.channels") {
            subdirs.push_back(it->path());
        }
    }
    std::sort(subdirs.begin(), subdirs.end());
    for (const fs::path& d : subdirs) {
        findFixtures(d, out);
    }
}

std::string keyOf(const fs::path& p) {
    std::error_code ec;
    const fs::path c = fs::weakly_canonical(p, ec);
    return u8(ec ? fs::absolute(p).lexically_normal() : c);
}

struct Comparison {
    std::uint64_t compared = 0;
    std::uint64_t readerInvalid = 0;
    std::uint64_t maxF32Ulps = 0;
    std::uint64_t maxF64Ulps = 0;
    std::uint64_t mismatches = 0;
    std::vector<std::string> problems; // the first kProblemsPerChannel, in sample order
};

void addProblem(Comparison& c, std::string text) {
    ++c.mismatches;
    if (c.problems.size() < kProblemsPerChannel) {
        c.problems.push_back(std::move(text));
    }
}

Comparison compareChannel(const ExpectedChannel& e, const model::Channel& ch, const Tolerance& tol,
                          bool strict) {
    Comparison r;
    // M2: timestamped channels (text importers) compare their I64 time column with the oracle's times.
    const auto* us = std::get_if<store::UniformSeries>(&ch.series);
    const auto* ts = std::get_if<store::TimestampedSeries>(&ch.series);
    const store::Column& col = store::seriesValues(ch.series);
    const store::SampleIdx n = col.size();
    const std::uint64_t expectedCount = e.samples.empty() ? 0 : e.samples.back().index + 1;
    if (e.samples.empty()) {
        if (n != 0) {
            addProblem(r, "the oracle decodes no sample, the reader " + std::to_string(n));
        }
        return r;
    }
    if (static_cast<std::uint64_t>(n) != expectedCount) {
        addProblem(r,
                   "sample count: oracle " + std::to_string(expectedCount) + ", reader " + std::to_string(n));
        if (static_cast<std::uint64_t>(n) < expectedCount) {
            return r;
        }
    }
    const store::Dtype dtype = col.dtype();
    std::size_t k = 0;
    while (k < e.samples.size()) {
        std::size_t j = k + 1;
        while (j < e.samples.size() && e.samples[j].index == e.samples[j - 1].index + 1 &&
               e.samples[j].index - e.samples[k].index < (std::uint64_t{1} << 22)) {
            ++j;
        }
        const auto first = static_cast<store::SampleIdx>(e.samples[k].index);
        const auto last = static_cast<store::SampleIdx>(e.samples[j - 1].index);
        auto h = col.require(first, last, Priority::Background, {});
        Result<store::ChunkHandle> th = ts != nullptr
                                            ? ts->timesUs.require(first, last, Priority::Background, {})
                                            : Result<store::ChunkHandle>(store::ChunkHandle{});
        if (!h || !th) {
            addProblem(r, "decode of samples " + std::to_string(first) + ".." + std::to_string(last) +
                              " failed: " + (h ? th.error() : h.error()).toString());
            k = j;
            continue;
        }
        for (std::size_t q = k; q < j; ++q) {
            const ExpectedSample& s = e.samples[q];
            const auto i = static_cast<store::SampleIdx>(s.index);
            const Micros t = us != nullptr ? us->timeAt(i) : th->int64At(i).value_or(-1);
            if (t != s.timeUs) {
                addProblem(r, "sample " + std::to_string(i) + ": time " + std::to_string(t) + " us, oracle " +
                                  std::to_string(s.timeUs) + " us");
            }
            const bool rv = h->validAt(i);
            const double v = h->at(i);
            ++r.compared;
            const bool expFinite = std::isfinite(s.value);
            if (strict) {
                // The oracle's valid column means "finite": an infinity the reader keeps as a value agrees.
                if (rv && !s.valid && std::isinf(s.value) && v == s.value) {
                    continue;
                }
                if (rv != s.valid) {
                    addProblem(r, "sample " + std::to_string(i) + ": validity " + (rv ? "valid" : "invalid") +
                                      ", expected " + (s.valid ? "valid" : "invalid") + " (oracle value " +
                                      numberText(s.value) + ")");
                    continue;
                }
                if (!rv) {
                    continue;
                }
            } else {
                if (!rv) {
                    if (expFinite) {
                        ++r.readerInvalid; // gap block: the oracle cannot judge it
                    }
                    continue;
                }
                if (!expFinite) {
                    const bool same = std::isnan(s.value) ? std::isnan(v) : v == s.value;
                    if (!same) {
                        addProblem(r, "sample " + std::to_string(i) + ": " + numberText(v) + ", oracle " +
                                          numberText(s.value));
                    }
                    continue;
                }
            }
            switch (dtype) {
            case store::Dtype::I32:
            case store::Dtype::I64:
                if (v != s.value) {
                    addProblem(r, "sample " + std::to_string(i) + ": " + numberText(v) + ", oracle " +
                                      numberText(s.value) + " (integer, exact)");
                }
                break;
            case store::Dtype::F32: {
                const std::uint64_t d = ulpDistanceF32(v, s.value);
                r.maxF32Ulps = std::max(r.maxF32Ulps, d);
                if (d > tol.f32Ulps) {
                    addProblem(r, "sample " + std::to_string(i) + ": " + numberText(v) + ", oracle " +
                                      numberText(s.value) + " (" + std::to_string(d) + " F32 ulp)");
                }
                break;
            }
            case store::Dtype::F64: {
                const std::uint64_t d = ulpDistanceF64(v, s.value);
                r.maxF64Ulps = std::max(r.maxF64Ulps, d);
                if (d > tol.f64Ulps) {
                    addProblem(r, "sample " + std::to_string(i) + ": " + numberText(v) + ", oracle " +
                                      numberText(s.value) + " (" + std::to_string(d) + " F64 ulp)");
                }
                break;
            }
            }
        }
        k = j;
    }
    return r;
}

bool readerExcluded(const formats::ProbeResult& probe, std::string_view oracleName) {
    for (const formats::ImportLogEntry& w : probe.warnings) {
        if ((w.code == "ld.zero_rate" || w.code == "ld.unknown_dtype") && w.ctx.channel &&
            namesCompatible(*w.ctx.channel, oracleName)) {
            return true;
        }
    }
    return false;
}

// Compares (or with update rewrites) one expectation file; "" when equal.
void checkText(const fs::path& file, const std::string& actual, bool required, bool update, Outcome& o,
               const Explain& explain = {}) {
    auto expected = readFile(file);
    if (!expected) {
        if (update) {
            if (auto w = writeFile(file, actual); w) {
                o.updated = true;
                o.notes.push_back("wrote " + u8(file.filename()));
            } else {
                o.problems.push_back(w.error().toString());
            }
        } else if (required) {
            o.problems.push_back(u8(file.filename()) + " is missing");
        }
        return;
    }
    if (*expected == actual) {
        return;
    }
    if (update) {
        if (auto w = writeFile(file, actual); w) {
            o.updated = true;
            o.notes.push_back("updated " + u8(file.filename()) + " (" + firstDifference(*expected, actual) +
                              ")");
        } else {
            o.problems.push_back(w.error().toString());
        }
        return;
    }
    const std::vector<std::string> paths =
        explain.on ? jsonDifferences(*expected, actual, 25) : std::vector<std::string>{};
    if (paths.empty()) {
        o.problems.push_back(u8(file.filename()) + " differs, " + firstDifference(*expected, actual));
    }
    for (const std::string& d : paths) {
        o.problems.push_back(u8(file.filename()) + ": " + d);
    }
}

Outcome verifyOne(AppContext& ctx, SchemaStore* schemas, const Target& t, bool update,
                  const Explain& explain = {}) {
    Outcome o;
    o.dir = t.dir;
    o.id = u8(t.dir.filename());
    auto fxLoaded = loadJson(t.dir / "fixture.json");
    if (!fxLoaded || !fxLoaded->is_object()) {
        o.status = Status::Fail;
        o.problems.push_back(fxLoaded ? std::string("fixture.json is not an object")
                                      : fxLoaded.error().toString());
        return o;
    }
    json fx = std::move(fxLoaded).value();
    bool fxChanged = false;
    const std::string format = fx.value("format", std::string());
    o.id = format + "/" + fx.value("name", o.id);
    if (schemas != nullptr) {
        auto errs = schemas->validate(fx, "fixture-1");
        if (!errs) {
            o.problems.push_back("fixture-1: " + errs.error().toString());
        } else {
            for (const SchemaError& e : *errs) {
                o.problems.push_back("fixture.json at '" + e.pointer + "': " + e.message);
            }
        }
    }
    const bool isPrivate = fx.value("private", false);
    const json input = fx.value("input", json::object());
    const std::string inputName = input.value("file", std::string("input.ld"));
    const fs::path inputPath = t.input ? *t.input : t.dir / p8(inputName);
    if (!pathExists(inputPath)) {
        const bool remote = input.contains("remote") && input["remote"].is_object();
        if (isPrivate) {
            o.status = Status::Skip;
            o.skipReason = "private: input absent";
        } else if (remote) {
            o.status = Status::Skip;
            o.skipReason = "remote input not fetched";
        } else {
            o.status = Status::Fail;
            o.problems.push_back(inputName +
                                 " is missing (synthetic inputs are written by esses-gen-fixtures "
                                 "--inputs-only)");
        }
        return o;
    }

    // Input and companion identity.
    std::error_code ec;
    const auto checkFile = [&](const fs::path& p, json& entry, const std::string& label) {
        const auto bytes = fs::file_size(p, ec);
        auto sha = sha256File(p);
        if (!sha) {
            o.problems.push_back(sha.error().toString());
            return;
        }
        const bool sizeOk = !entry.contains("bytes") || entry["bytes"].get<std::uint64_t>() == bytes;
        const bool shaOk = entry.value("sha256", std::string()) == *sha;
        if (sizeOk && shaOk) {
            return;
        }
        if (update) {
            entry["sha256"] = *sha;
            entry["bytes"] = bytes;
            fxChanged = true;
            o.notes.push_back("updated the hash of " + label);
        } else {
            o.problems.push_back(label + ": " +
                                 (sizeOk ? "" : "size " + std::to_string(bytes) + " differs, ") +
                                 (shaOk ? "" : "sha256 " + *sha + " differs from fixture.json"));
        }
    };
    if (fx.contains("input") && fx["input"].is_object()) {
        checkFile(inputPath, fx["input"], inputName);
    }
    if (fx.contains("companions") && fx["companions"].is_array()) {
        for (json& c : fx["companions"]) {
            const std::string cname = c.value("file", std::string());
            fs::path cpath = t.dir / p8(cname);
            if (t.input) {
                const std::vector<fs::path> sib = formats::listSiblings(*t.input);
                const auto found = formats::findCompanion(sib, *t.input, u8(p8(cname).extension()));
                cpath = found ? *found : fs::path();
            }
            if (cpath.empty() || !pathExists(cpath)) {
                o.problems.push_back("companion " + cname + " is missing");
                continue;
            }
            checkFile(cpath, c, cname);
        }
    }

    // Probe.
    const json importOptions = fx.value("importOptions", json());
    auto opts = makeImportOptionsJson(format, importOptions);
    if (!opts) {
        o.status = Status::Fail;
        o.problems.push_back(opts.error().toString());
        return o;
    }
    auto probed = probeFile(ctx, inputPath, *opts);
    if (!probed) {
        o.status = Status::Fail;
        o.problems.push_back("probe: " + probed.error().toString());
        return o;
    }
    auto probeText = probed->probe.canonicalJson();
    if (!probeText) {
        o.status = Status::Fail;
        o.problems.push_back(probeText.error().toString());
        return o;
    }
    checkText(t.dir / "expected.probe.json", *probeText, true, update, o, explain);
    if (!fx.contains("dialect") || fx["dialect"] != probed->probe.dialect) {
        if (update || !fx.contains("dialect")) {
            if (update) {
                fx["dialect"] = probed->probe.dialect;
                fxChanged = true;
                o.notes.push_back("dialect set to '" + probed->probe.dialect + "'");
            }
        } else {
            o.problems.push_back("dialect: fixture.json says '" + fx["dialect"].get<std::string>() +
                                 "', the probe reports '" + probed->probe.dialect + "'");
        }
    }

    // Open: markers, laps, channels.
    auto opened = openFile(ctx, inputPath, *opts, false, false);
    if (!opened) {
        o.status = Status::Fail;
        o.problems.push_back("open: " + opened.error().toString());
        return o;
    }
    const model::DataSource& src = *opened->source;
    if (auto mt = markersText(src); mt) {
        checkText(t.dir / "expected.markers.json", *mt, false, update, o, explain);
    } else {
        o.problems.push_back(mt.error().toString());
    }
    if (auto lt = lapsText(src); lt) {
        checkText(t.dir / "expected.laps.json", *lt, false, update, o, explain);
    } else {
        o.problems.push_back(lt.error().toString());
    }

    const json oracle = fx.value("oracle", json::object());
    auto tol = parseTolerance(oracle.value("tolerance", json::object()));
    if (!tol) {
        o.problems.push_back(tol.error().toString());
        tol = Tolerance{};
    }
    const bool strict = oracle.value("validity", std::string("strict")) != "finite-only";
    auto expected = readExpectedChannels(t.dir);
    if (!expected) {
        o.problems.push_back(expected.error().toString());
    } else if (!src.layers.empty()) {
        const model::DataLayer& layer = src.layers.front();
        std::vector<bool> matched(layer.channels.size(), false);
        std::size_t p = 0;
        for (const ExpectedChannel& e : *expected) {
            std::optional<std::size_t> at;
            for (std::size_t q = p; q < layer.channels.size(); ++q) {
                if (!matched[q] && namesCompatible(layer.channels[q].desc.loggedName, e.name)) {
                    at = q;
                    break;
                }
            }
            if (!at) {
                if (readerExcluded(probed->probe, e.name)) {
                    o.notes.push_back("oracle channel " + std::to_string(e.oracleIndex) + " '" + e.name +
                                      "' is not a channel for the reader (ld.zero_rate / ld.unknown_dtype)");
                } else {
                    o.problems.push_back("oracle channel " + std::to_string(e.oracleIndex) + " '" + e.name +
                                         "' has no matching reader channel");
                }
                continue;
            }
            matched[*at] = true;
            p = *at + 1;
            const model::Channel& ch = layer.channels[*at];
            if (!explain.channel.empty() && caseFold(ch.desc.name) != explain.channel &&
                caseFold(e.name) != explain.channel) {
                continue;
            }
            const Comparison c = compareChannel(e, ch, *tol, strict);
            if (explain.on) {
                // One line per channel: what was compared, how far apart, where it first differs.
                std::string line = "channel '" + ch.desc.name + "' (oracle " + std::to_string(e.oracleIndex) +
                                   " '" + e.name + "'): " + std::to_string(c.compared) + " samples, " +
                                   std::to_string(c.mismatches) + " differ";
                if (c.maxF32Ulps > 0 || c.maxF64Ulps > 0) {
                    line += ", max " + std::to_string(std::max(c.maxF32Ulps, c.maxF64Ulps)) +
                            (c.maxF64Ulps > c.maxF32Ulps ? " F64" : " F32") + " ulp";
                }
                if (!c.problems.empty()) {
                    line += "; first: " + c.problems.front();
                }
                o.notes.push_back(std::move(line));
            }
            ++o.channels;
            o.samples += c.compared;
            o.readerInvalid += c.readerInvalid;
            o.maxF32Ulps = std::max(o.maxF32Ulps, c.maxF32Ulps);
            for (const std::string& s : c.problems) {
                o.problems.push_back("channel '" + ch.desc.name + "': " + s);
            }
            if (c.mismatches > c.problems.size()) {
                o.problems.push_back("channel '" + ch.desc.name + "': ... and " +
                                     std::to_string(c.mismatches - c.problems.size()) + " more");
            }
            ctx.cache().clear();
        }
        const json compared = fx.value("channelsCompared", json("all"));
        if (compared.is_string() && compared.get<std::string>() == "all" && explain.channel.empty()) {
            for (std::size_t q = 0; q < layer.channels.size(); ++q) {
                if (!matched[q]) {
                    o.problems.push_back("reader channel '" + layer.channels[q].desc.name +
                                         "' has no oracle expectation");
                }
            }
        }
    }
    ctx.cache().clear();

    if (fxChanged) {
        auto text = canonicalText(fx);
        if (text) {
            if (auto w = writeFile(t.dir / "fixture.json", *text); w) {
                o.updated = true;
            } else {
                o.problems.push_back(w.error().toString());
            }
        }
    }
    o.status = o.problems.empty() ? Status::Pass : Status::Fail;
    return o;
}

Result<std::vector<Target>> manifestTargets(const fs::path& manifest) {
    auto j = loadJson(manifest);
    if (!j) {
        return Failure{j.error()};
    }
    if (!j->is_object() || !j->contains("fixtures") || !(*j)["fixtures"].is_array()) {
        return fail(ErrorCode::Format, u8(manifest) + R"(: expected {"fixtures": [{"dir", "input"}]})");
    }
    std::vector<Target> out;
    const fs::path base = manifest.parent_path();
    for (const json& e : (*j)["fixtures"]) {
        if (!e.is_object() || !e.contains("dir") || !e["dir"].is_string()) {
            return fail(ErrorCode::Format, u8(manifest) + ": an entry lacks \"dir\"");
        }
        Target t;
        const fs::path dir = p8(e["dir"].get<std::string>());
        t.dir = dir.is_absolute() ? dir : base / dir;
        if (e.contains("input") && e["input"].is_string()) {
            const fs::path in = p8(e["input"].get<std::string>());
            t.input = in.is_absolute() ? in : base / in;
        }
        out.push_back(std::move(t));
    }
    return out;
}

std::string_view statusText(Status s) {
    switch (s) {
    case Status::Pass:
        return "PASS";
    case Status::Fail:
        return "FAIL";
    case Status::Skip:
        break;
    }
    return "SKIP";
}

} // namespace

int runFixtureVerify(AppContext& ctx, const FixtureVerifyOptions& o, std::ostream& out, std::ostream& err) {
    std::vector<Target> targets;
    std::map<std::string, std::size_t> byKey;
    std::size_t absentManifests = 0;
    for (const fs::path& m : o.manifests) {
        if (o.ifPresent && !pathExists(m)) {
            out << "SKIP  " << u8(m) << "  (skipped (private): manifest absent)\n";
            ++absentManifests;
            continue;
        }
        auto ts = manifestTargets(m);
        if (!ts) {
            err << "esses-cli fixture verify: " << ts.error().toString() << "\n";
            return 2;
        }
        for (Target& t : *ts) {
            byKey[keyOf(t.dir)] = targets.size();
            targets.push_back(std::move(t));
        }
    }
    for (const fs::path& root : o.roots) {
        if (!pathExists(root)) {
            err << "esses-cli fixture verify: no such directory " << u8(root) << "\n";
            return 2;
        }
        std::vector<fs::path> dirs;
        findFixtures(root, dirs);
        for (const fs::path& d : dirs) {
            if (byKey.emplace(keyOf(d), targets.size()).second) {
                targets.push_back(Target{d, std::nullopt});
            }
        }
    }
    if (targets.empty()) {
        if (absentManifests > 0 && o.roots.empty()) {
            out << "fixture verify: skipped (private): no manifest present\n";
            return 0;
        }
        err << "esses-cli fixture verify: no fixture.json found\n";
        return 2;
    }
    if (!ctx.registry()) {
        err << "esses-cli fixture verify: " << ctx.registry().error().toString() << "\n";
        return 2;
    }
    std::optional<SchemaStore> schemas;
    if (auto dir = ctx.schemaDir(); dir) {
        schemas.emplace(*dir);
    } else {
        err << "esses-cli fixture verify: " << dir.error().toString()
            << " (fixture.json not schema-checked)\n";
    }

    std::vector<Outcome> results;
    std::size_t passed = 0, failed = 0, skipped = 0, updated = 0;
    std::uint64_t samples = 0, channels = 0;
    for (const Target& t : targets) {
        Outcome r = verifyOne(ctx, schemas ? &*schemas : nullptr, t, o.update);
        switch (r.status) {
        case Status::Pass:
            ++passed;
            break;
        case Status::Fail:
            ++failed;
            break;
        case Status::Skip:
            ++skipped;
            break;
        }
        updated += r.updated ? 1 : 0;
        samples += r.samples;
        channels += r.channels;
        if (!o.json) {
            out << statusText(r.status) << "  " << r.id;
            if (r.status == Status::Skip) {
                out << "  (" << r.skipReason << ")";
            } else {
                out << "  (" << r.channels << " channels, " << r.samples << " samples";
                if (r.readerInvalid > 0) {
                    out << ", " << r.readerInvalid << " gap-invalid not judged by the oracle";
                }
                out << ")";
            }
            out << "\n";
            for (const std::string& p : r.problems) {
                out << "    - " << p << "\n";
            }
            if (o.verbose || o.update) {
                for (const std::string& n : r.notes) {
                    out << "    . " << n << "\n";
                }
            }
        }
        results.push_back(std::move(r));
    }
    if (o.json) {
        json doc = json::object();
        json fixtures = json::array();
        for (const Outcome& r : results) {
            json f = json::object();
            f["id"] = r.id;
            f["dir"] = u8(r.dir);
            f["status"] = std::string(statusText(r.status));
            if (!r.skipReason.empty()) {
                f["skipReason"] = r.skipReason;
            }
            f["channels"] = r.channels;
            f["samples"] = r.samples;
            f["readerInvalid"] = r.readerInvalid;
            f["maxF32Ulps"] = r.maxF32Ulps;
            f["problems"] = r.problems;
            f["notes"] = r.notes;
            fixtures.push_back(std::move(f));
        }
        doc["fixtures"] = std::move(fixtures);
        doc["summary"] = json{{"passed", passed},   {"failed", failed},     {"skipped", skipped},
                              {"updated", updated}, {"channels", channels}, {"samples", samples}};
        if (auto text = canonicalText(doc); text) {
            out << *text;
        }
    } else {
        out << "fixture verify: " << passed << " passed, " << failed << " failed, " << skipped << " skipped";
        if (o.update) {
            out << ", " << updated << " updated";
        }
        out << " (" << channels << " channels, " << samples << " samples compared)\n";
    }
    return failed > 0 ? 1 : 0;
}

int runFixtureDiff(AppContext& ctx, const FixtureDiffOptions& o, std::ostream& out, std::ostream& err) {
    std::vector<Target> targets;
    for (const fs::path& root : o.dirs) {
        if (!pathExists(root)) {
            err << "esses-cli fixture diff: no such directory " << u8(root) << "\n";
            return 2;
        }
        std::vector<fs::path> dirs;
        findFixtures(root, dirs);
        for (const fs::path& d : dirs) {
            targets.push_back(Target{d, std::nullopt});
        }
    }
    if (targets.empty()) {
        err << "esses-cli fixture diff: no fixture.json found\n";
        return 2;
    }
    if (!ctx.registry()) {
        err << "esses-cli fixture diff: " << ctx.registry().error().toString() << "\n";
        return 2;
    }
    std::optional<SchemaStore> schemas;
    if (auto dir = ctx.schemaDir(); dir) {
        schemas.emplace(*dir);
    }
    Explain explain;
    explain.on = true;
    explain.channel = caseFold(o.channel);
    json fixtures = json::array();
    std::size_t differing = 0;
    for (const Target& t : targets) {
        const Outcome r = verifyOne(ctx, schemas ? &*schemas : nullptr, t, false, explain);
        const bool differs = r.status == Status::Fail;
        differing += differs ? 1 : 0;
        if (o.json) {
            fixtures.push_back(json{{"id", r.id},
                                    {"dir", u8(r.dir)},
                                    {"status", std::string(statusText(r.status))},
                                    {"channels", r.notes},
                                    {"differences", r.problems}});
            continue;
        }
        out << (r.status == Status::Skip ? "SKIP" : differs ? "DIFF" : "SAME") << "  " << r.id;
        if (r.status == Status::Skip) {
            out << "  (" << r.skipReason << ")";
        }
        out << "\n";
        for (const std::string& n : r.notes) {
            out << "    " << n << "\n";
        }
        for (const std::string& d : r.problems) {
            out << "    - " << d << "\n";
        }
    }
    if (o.json) {
        if (auto text = canonicalText(json{{"fixtures", std::move(fixtures)}, {"differing", differing}});
            text) {
            out << *text;
        }
    } else {
        out << "fixture diff: " << differing << " of " << targets.size() << " fixtures differ\n";
    }
    return differing > 0 ? 1 : 0;
}

} // namespace esses::cli
