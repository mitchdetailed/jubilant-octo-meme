// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
#include "ld/ldx.hpp"

#include "ld/ld_codes.hpp"

#include <esses/base/atomic_file.hpp>
#include <esses/base/mapped_file.hpp>

#include <fast_float/fast_float.h>
#include <pugixml.hpp>

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <map>
#include <set>
#include <sstream>

namespace esses::formats::ld {
namespace {

using Level = ImportLogEntry::Level;

constexpr std::array<std::string_view, 8> kKnownClasses = {"SOL",     "EOL",   "BCN",     "SPLTBCN",
                                                           "IGRDBCN", "RESET", "STGSTRT", "STGEND"};
constexpr std::string_view kPlaceholder = "esses-opaque";

std::string_view nameOf(const pugi::xml_node& n) {
    return n.name();
}

std::vector<LdxAttribute> attributesOf(const pugi::xml_node& n) {
    std::vector<LdxAttribute> out;
    for (const pugi::xml_attribute& a : n.attributes()) {
        out.push_back({a.name(), a.value()});
    }
    return out;
}

std::string attr(const pugi::xml_node& n, const char* name) {
    return n.attribute(name).value();
}

// End (exclusive) of the element whose '<' is at `start`, honouring quotes, comments, CDATA and processing
// instructions; npos when the text ends first. The text is known to be well-formed (pugixml parsed it).
std::size_t elementEnd(std::string_view s, std::size_t start) {
    int depth = 0;
    std::size_t i = start;
    while (i < s.size()) {
        if (s[i] != '<') {
            ++i;
            continue;
        }
        const auto skipTo = [&](std::string_view open, std::string_view close) -> bool {
            if (s.compare(i, open.size(), open) != 0) {
                return false;
            }
            const std::size_t e = s.find(close, i + open.size());
            i = e == std::string_view::npos ? s.size() : e + close.size();
            return true;
        };
        if (skipTo("<!--", "-->") || skipTo("<![CDATA[", "]]>") || skipTo("<?", "?>")) {
            continue;
        }
        if (s.compare(i, 2, "</") == 0) {
            const std::size_t e = s.find('>', i + 2);
            if (e == std::string_view::npos) {
                return std::string_view::npos;
            }
            --depth;
            i = e + 1;
            if (depth <= 0) {
                return i;
            }
            continue;
        }
        if (s.compare(i, 2, "<!") == 0) {
            const std::size_t e = s.find('>', i + 2);
            i = e == std::string_view::npos ? s.size() : e + 1;
            continue;
        }
        std::size_t j = i + 1;
        char quote = 0;
        for (; j < s.size(); ++j) {
            const char ch = s[j];
            if (quote != 0) {
                if (ch == quote) {
                    quote = 0;
                }
            } else if (ch == '"' || ch == '\'') {
                quote = ch;
            } else if (ch == '>') {
                break;
            }
        }
        if (j >= s.size()) {
            return std::string_view::npos;
        }
        if (s[j - 1] != '/') {
            ++depth;
        }
        i = j + 1;
        if (depth == 0) {
            return i;
        }
    }
    return std::string_view::npos;
}

// The verbatim source text of an element, or its pugixml serialisation when the offsets are not usable
// (an input that pugixml had to transcode).
std::string sourceText(std::string_view src, const pugi::xml_node& n) {
    const std::ptrdiff_t off = n.offset_debug();
    if (off > 0 && static_cast<std::size_t>(off) < src.size() &&
        src[static_cast<std::size_t>(off) - 1] == '<' &&
        src.compare(static_cast<std::size_t>(off), nameOf(n).size(), nameOf(n)) == 0) {
        const std::size_t start = static_cast<std::size_t>(off) - 1;
        const std::size_t end = elementEnd(src, start);
        if (end != std::string_view::npos) {
            return std::string(src.substr(start, end - start));
        }
    }
    std::ostringstream os;
    n.print(os, "", pugi::format_raw, pugi::encoding_utf8);
    return os.str();
}

std::optional<double> parseTime(std::string_view s) {
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) {
        s.remove_prefix(1);
    }
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t')) {
        s.remove_suffix(1);
    }
    if (s.empty()) {
        return std::nullopt;
    }
    double v = 0.0;
    // fast_float, not std::from_chars: Apple Clang 15's libc++ has no floating-point from_chars.
    const auto r = fast_float::from_chars(s.data(), s.data() + s.size(), v);
    if (r.ec != std::errc() || r.ptr != s.data() + s.size() || !std::isfinite(v) || std::abs(v) > 9.0e18) {
        return std::nullopt;
    }
    return v;
}

class LdxParser {
public:
    LdxParser(std::string_view src, ImportLog& log, LdxDocument& doc) : src_(src), log_(log), doc_(doc) {}

    void root(const pugi::xml_node& r) {
        doc_.rootAttributes = attributesOf(r);
        std::uint32_t pos = 0;
        bool layersSeen = false;
        for (const pugi::xml_node& c : r.children()) {
            if (c.type() != pugi::node_element) {
                continue;
            }
            if (nameOf(c) == "Layers" && !layersSeen) {
                layersSeen = true;
                layers(c);
            } else {
                opaque("LDXFile", pos, c, true);
            }
            ++pos;
        }
    }

private:
    void layers(const pugi::xml_node& n) {
        doc_.layersAttributes = attributesOf(n);
        std::uint32_t pos = 0;
        for (const pugi::xml_node& c : n.children()) {
            if (c.type() != pugi::node_element) {
                continue;
            }
            if (nameOf(c) == "Layer" && !doc_.hasLayer) {
                doc_.hasLayer = true;
                layer(c);
            } else if (nameOf(c) == "Details" && !doc_.hasDetails) {
                doc_.hasDetails = true;
                details(c, "LDXFile/Layers/Details");
            } else {
                opaque("LDXFile/Layers", pos, c, true);
            }
            ++pos;
        }
    }

    void layer(const pugi::xml_node& n) {
        doc_.layerAttributes = attributesOf(n);
        std::uint32_t pos = 0;
        for (const pugi::xml_node& c : n.children()) {
            if (c.type() != pugi::node_element) {
                continue;
            }
            if (nameOf(c) == "MarkerBlock" && !doc_.hasMarkerBlock) {
                doc_.hasMarkerBlock = true;
                markerBlock(c);
            } else if (nameOf(c) == "Details" && !doc_.hasDetails) {
                doc_.hasDetails = true;
                doc_.detailsInLayer = true;
                details(c, "LDXFile/Layers/Layer/Details");
            } else {
                opaque("LDXFile/Layers/Layer", pos, c, true);
            }
            ++pos;
        }
    }

    void markerBlock(const pugi::xml_node& n) {
        doc_.markerBlockAttributes = attributesOf(n);
        std::uint32_t pos = 0;
        for (const pugi::xml_node& c : n.children()) {
            if (c.type() != pugi::node_element) {
                continue;
            }
            if (nameOf(c) == "MarkerGroup") {
                group(c);
            } else {
                opaque("LDXFile/Layers/Layer/MarkerBlock", pos, c, true);
            }
            ++pos;
        }
    }

    void group(const pugi::xml_node& n) {
        const auto g = static_cast<std::uint32_t>(doc_.groups.size());
        doc_.groups.push_back({attr(n, "Name"), attr(n, "Index"), attributesOf(n)});
        noteUnknownAttributes(n, {"Name", "Index"});
        const std::string path = "LDXFile/Layers/Layer/MarkerBlock/MarkerGroup#" + std::to_string(g);
        std::uint32_t pos = 0;
        for (const pugi::xml_node& c : n.children()) {
            if (c.type() != pugi::node_element) {
                continue;
            }
            if (nameOf(c) == "Marker") {
                marker(c, g, path, pos);
            } else {
                opaque(path, pos, c, true);
            }
            ++pos;
        }
    }

    void marker(const pugi::xml_node& n, std::uint32_t g, const std::string& path, std::uint32_t pos) {
        const std::string cls = attr(n, "ClassName");
        if (!isKnownMarkerClass(cls)) {
            if (classesLogged_.insert(cls).second) {
                ImportLogEntry::Context ctx;
                ctx.detail = cls;
                ctx.channel = cls;
                log_.add(Level::Info, std::string(code::kLdxUnknownMarkerClass),
                         "marker class '" + cls + "' is not interpreted; the element is kept verbatim", ctx);
            }
            opaque(path, pos, n, false);
            return;
        }
        const auto t = parseTime(attr(n, "Time"));
        if (!t) {
            ImportLogEntry::Context ctx;
            ctx.detail = attr(n, "Time");
            log_.add(Level::Warning, std::string(code::kLdxMarkerTimeUnparsable),
                     "marker '" + attr(n, "Name") + "' has no usable Time; the element is kept verbatim",
                     ctx);
            opaque(path, pos, n, false);
            return;
        }
        LdxMarker m;
        m.group = g;
        m.className = cls;
        m.name = attr(n, "Name");
        m.version = attr(n, "Version");
        m.flags = attr(n, "Flags");
        std::uint32_t fv = 0;
        const auto r = std::from_chars(m.flags.data(), m.flags.data() + m.flags.size(), fv);
        m.flagsValue = r.ec == std::errc() ? fv : 0;
        m.timeUs = *t;
        m.tUs = static_cast<Micros>(std::llround(*t));
        m.fileIndex = static_cast<std::uint32_t>(doc_.markers.size());
        m.attributes = attributesOf(n);
        noteUnknownAttributes(n, {"Version", "ClassName", "Name", "Flags", "Time"});
        doc_.markers.push_back(std::move(m));
    }

    void details(const pugi::xml_node& n, const std::string& path) {
        doc_.detailsAttributes = attributesOf(n);
        std::uint32_t pos = 0;
        for (const pugi::xml_node& c : n.children()) {
            if (c.type() != pugi::node_element) {
                continue;
            }
            if (nameOf(c) == "String") {
                doc_.details.push_back({attr(c, "Id"), attr(c, "Value"), attributesOf(c)});
                noteUnknownAttributes(c, {"Id", "Value"});
            } else {
                opaque(path, pos, c, true);
            }
            ++pos;
        }
    }

    void noteUnknownAttributes(const pugi::xml_node& n, std::initializer_list<std::string_view> known) {
        for (const pugi::xml_attribute& a : n.attributes()) {
            const std::string_view an = a.name();
            if (std::find(known.begin(), known.end(), an) == known.end()) {
                ImportLogEntry::Context ctx;
                ctx.detail = std::string(nameOf(n)) + "/@" + std::string(an);
                log_.add(Level::Info, std::string(code::kLdxUnknownAttribute),
                         "attribute '" + std::string(an) + "' on " + std::string(nameOf(n)) +
                             " kept, not interpreted",
                         ctx);
            }
        }
    }

    void opaque(const std::string& parent, std::uint32_t pos, const pugi::xml_node& n, bool logUnknown) {
        if (logUnknown) {
            ImportLogEntry::Context ctx;
            ctx.detail = parent + "/" + std::string(nameOf(n));
            log_.add(Level::Info, std::string(code::kLdxUnknownElement),
                     "element '" + std::string(nameOf(n)) + "' under " + parent + " kept verbatim", ctx);
        }
        doc_.opaque.push_back({parent, pos, sourceText(src_, n)});
    }

    std::string_view src_;
    ImportLog& log_;
    LdxDocument& doc_;
    std::set<std::string> classesLogged_;
};

void assignOrdinals(std::vector<LdxMarker>& markers) {
    std::map<std::tuple<std::string, std::uint32_t, Micros>, std::uint32_t> seen;
    for (LdxMarker& m : markers) {
        m.ordinal = seen[std::make_tuple(m.className, m.group, m.tUs)]++;
    }
}

// Element children of `parent` that the writer counts as positions: elements and opaque placeholders.
pugi::xml_node positionChild(const pugi::xml_node& parent, std::uint32_t index) {
    std::uint32_t k = 0;
    for (pugi::xml_node c = parent.first_child(); c; c = c.next_sibling()) {
        const bool counted =
            c.type() == pugi::node_element || (c.type() == pugi::node_pi && nameOf(c) == kPlaceholder);
        if (!counted) {
            continue;
        }
        if (k == index) {
            return c;
        }
        ++k;
    }
    return {};
}

void setAttributes(pugi::xml_node n, const std::vector<LdxAttribute>& attrs) {
    for (const LdxAttribute& a : attrs) {
        n.append_attribute(a.name.c_str()).set_value(a.value.c_str());
    }
}

// Original attribute order with the known values replaced; known attributes missing from the original are
// appended in the default order.
std::vector<LdxAttribute> merged(const std::vector<LdxAttribute>& original,
                                 const std::vector<LdxAttribute>& known) {
    std::vector<LdxAttribute> out;
    for (const LdxAttribute& a : original) {
        const auto k =
            std::find_if(known.begin(), known.end(), [&](const LdxAttribute& x) { return x.name == a.name; });
        out.push_back(k != known.end() ? *k : a);
    }
    for (const LdxAttribute& k : known) {
        if (std::none_of(out.begin(), out.end(), [&](const LdxAttribute& x) { return x.name == k.name; })) {
            out.push_back(k);
        }
    }
    return out;
}

} // namespace

bool isKnownMarkerClass(std::string_view cls) noexcept {
    return std::find(kKnownClasses.begin(), kKnownClasses.end(), cls) != kKnownClasses.end();
}

std::string formatLdxTime(double timeUs) {
    char buf[64];
    const auto r = std::to_chars(buf, buf + sizeof(buf), timeUs, std::chars_format::fixed, 2);
    return r.ec == std::errc() ? std::string(buf, r.ptr) : std::string("0.00");
}

LdxDocument LdxDocument::makeDefault() {
    LdxDocument d;
    d.rootAttributes = {{"Locale", "C"}, {"DefaultLocale", "C"}, {"Version", "1.6"}};
    return d;
}

void LdxDocument::addMarker(std::string_view group, std::string_view className, std::string_view name,
                            double timeUs, std::string_view flags) {
    auto it = std::find_if(groups.begin(), groups.end(), [&](const LdxGroup& g) { return g.name == group; });
    if (it == groups.end()) {
        groups.push_back({std::string(group), std::to_string(groups.size()), {}});
        it = groups.end() - 1;
    }
    LdxMarker m;
    m.group = static_cast<std::uint32_t>(it - groups.begin());
    m.className = className;
    m.name = name;
    m.version = "100";
    m.flags = flags;
    std::uint32_t fv = 0;
    const auto r = std::from_chars(m.flags.data(), m.flags.data() + m.flags.size(), fv);
    m.flagsValue = r.ec == std::errc() ? fv : 0;
    m.timeUs = timeUs;
    m.tUs = static_cast<Micros>(std::llround(timeUs));
    m.fileIndex = static_cast<std::uint32_t>(markers.size());
    markers.push_back(std::move(m));
    hasLayer = true;
    hasMarkerBlock = true;
    normalise();
}

void LdxDocument::setDetail(std::string_view id, std::string_view value) {
    hasDetails = true;
    for (LdxDetail& d : details) {
        if (d.id == id) {
            d.value = value;
            return;
        }
    }
    details.push_back({std::string(id), std::string(value), {}});
}

const LdxDetail* LdxDocument::findDetail(std::string_view id) const noexcept {
    const auto it =
        std::find_if(details.begin(), details.end(), [&](const LdxDetail& d) { return d.id == id; });
    return it == details.end() ? nullptr : &*it;
}

void LdxDocument::normalise() {
    std::stable_sort(markers.begin(), markers.end(), [](const LdxMarker& a, const LdxMarker& b) {
        return a.tUs < b.tUs || (a.tUs == b.tUs && a.fileIndex < b.fileIndex);
    });
    for (std::size_t i = 0; i < markers.size(); ++i) {
        markers[i].fileIndex = static_cast<std::uint32_t>(i);
    }
    assignOrdinals(markers);
}

Result<LdxDocument> parseLdx(std::span<const std::byte> bytes, ImportLog& log,
                             std::optional<Micros> durationUs) {
    const std::string_view src(reinterpret_cast<const char*>(bytes.data()), bytes.size());
    pugi::xml_document xml;
    const pugi::xml_parse_result pr = xml.load_buffer(
        bytes.data(), bytes.size(), pugi::parse_default | pugi::parse_declaration, pugi::encoding_auto);
    const auto parseError = [&](std::string message, std::uint64_t offset) -> Result<LdxDocument> {
        ImportLogEntry::Context ctx;
        ctx.offset = offset;
        log.add(Level::Warning, std::string(code::kLdxParseError), message, ctx);
        ErrorContext ectx;
        ectx.offset = offset;
        ectx.detail = std::string(code::kLdxParseError);
        return fail(ErrorCode::Format, std::move(message), std::move(ectx));
    };
    if (!pr) {
        return parseError(std::string(".ldx is not well-formed XML: ") + pr.description(),
                          static_cast<std::uint64_t>(std::max<std::ptrdiff_t>(0, pr.offset)));
    }
    const pugi::xml_node root = xml.document_element();
    if (nameOf(root) != "LDXFile") {
        return parseError(".ldx root element is '" + std::string(nameOf(root)) + "', not LDXFile", 0);
    }
    LdxDocument doc;
    LdxParser parser(src, log, doc);
    parser.root(root);

    // Within each group, file order should be time order (groups are written one after the other).
    bool unsorted = false;
    std::map<std::uint32_t, Micros> lastInGroup;
    for (const LdxMarker& m : doc.markers) {
        const auto [it, inserted] = lastInGroup.try_emplace(m.group, m.tUs);
        if (!inserted) {
            unsorted = unsorted || m.tUs < it->second;
            it->second = m.tUs;
        }
    }
    if (unsorted) {
        log.add(Level::Info, std::string(code::kLdxMarkerUnsorted),
                "markers of a group are not in ascending Time order");
    }
    doc.normalise();
    if (durationUs) {
        const auto beyond = std::count_if(doc.markers.begin(), doc.markers.end(),
                                          [&](const LdxMarker& m) { return m.tUs > *durationUs; });
        if (beyond > 0) {
            ImportLogEntry::Context ctx;
            ctx.repeats = static_cast<std::uint32_t>(beyond);
            log.add(Level::Info, std::string(code::kLdxMarkerBeyondEnd),
                    std::to_string(beyond) + " marker(s) after the end of the longest channel", ctx);
        }
    }
    return doc;
}

Result<LdxDocument> readLdxFile(const std::filesystem::path& path, ImportLog& log,
                                std::optional<Micros> durationUs) {
    auto mf = MappedFile::open(path);
    if (!mf) {
        return Failure{mf.error()};
    }
    return parseLdx(mf->bytes(), log, durationUs);
}

std::string writeLdx(const LdxDocument& doc) {
    pugi::xml_document x;
    pugi::xml_node decl = x.append_child(pugi::node_declaration);
    decl.append_attribute("version").set_value("1.0");
    pugi::xml_node root = x.append_child("LDXFile");
    setAttributes(root, doc.rootAttributes.empty() ? LdxDocument::makeDefault().rootAttributes
                                                   : doc.rootAttributes);

    std::map<std::string, pugi::xml_node> containers;
    containers["LDXFile"] = root;
    pugi::xml_node layers = root.append_child("Layers");
    setAttributes(layers, doc.layersAttributes);
    containers["LDXFile/Layers"] = layers;

    const bool needLayer = doc.hasLayer || doc.hasMarkerBlock || !doc.markers.empty() || doc.detailsInLayer;
    pugi::xml_node layer;
    if (needLayer) {
        layer = layers.append_child("Layer");
        setAttributes(layer, doc.layerAttributes);
        containers["LDXFile/Layers/Layer"] = layer;
        const bool needBlock = doc.hasMarkerBlock || !doc.markers.empty() || !doc.groups.empty();
        if (needBlock) {
            pugi::xml_node block = layer.append_child("MarkerBlock");
            setAttributes(block, doc.markerBlockAttributes);
            containers["LDXFile/Layers/Layer/MarkerBlock"] = block;
            for (std::size_t g = 0; g < doc.groups.size(); ++g) {
                const LdxGroup& grp = doc.groups[g];
                pugi::xml_node gn = block.append_child("MarkerGroup");
                setAttributes(gn, merged(grp.attributes, {{"Name", grp.name}, {"Index", grp.index}}));
                containers["LDXFile/Layers/Layer/MarkerBlock/MarkerGroup#" + std::to_string(g)] = gn;
                for (const LdxMarker& m : doc.markers) {
                    if (m.group != g) {
                        continue;
                    }
                    pugi::xml_node mn = gn.append_child("Marker");
                    setAttributes(mn, merged(m.attributes, {{"Version", m.version},
                                                            {"ClassName", m.className},
                                                            {"Name", m.name},
                                                            {"Flags", m.flags},
                                                            {"Time", formatLdxTime(m.timeUs)}}));
                }
            }
        }
    }
    const bool needDetails = doc.hasDetails || !doc.details.empty();
    if (needDetails) {
        pugi::xml_node parent = doc.detailsInLayer && layer ? layer : layers;
        pugi::xml_node dn = parent.append_child("Details");
        setAttributes(dn, doc.detailsAttributes);
        containers[doc.detailsInLayer && layer ? "LDXFile/Layers/Layer/Details" : "LDXFile/Layers/Details"] =
            dn;
        for (const LdxDetail& d : doc.details) {
            pugi::xml_node s = dn.append_child("String");
            setAttributes(s, merged(d.attributes, {{"Id", d.id}, {"Value", d.value}}));
        }
    }

    // Opaque elements: placeholders at their recorded positions, replaced by the verbatim text below.
    std::vector<std::size_t> order(doc.opaque.size());
    for (std::size_t i = 0; i < order.size(); ++i) {
        order[i] = i;
    }
    std::stable_sort(order.begin(), order.end(), [&](std::size_t a, std::size_t b) {
        return doc.opaque[a].position < doc.opaque[b].position;
    });
    for (const std::size_t i : order) {
        const LdxOpaque& o = doc.opaque[i];
        const auto it = containers.find(o.parentPath);
        pugi::xml_node parent = it != containers.end() ? it->second : root;
        const pugi::xml_node before = positionChild(parent, o.position);
        pugi::xml_node pi =
            before ? parent.insert_child_before(pugi::node_pi, before) : parent.append_child(pugi::node_pi);
        pi.set_name(std::string(kPlaceholder).c_str());
        pi.set_value(std::to_string(i).c_str());
    }

    std::ostringstream os;
    x.save(os, " ", pugi::format_indent, pugi::encoding_utf8);
    std::string text = os.str();
    for (std::size_t i = 0; i < doc.opaque.size(); ++i) {
        const std::string token = "<?" + std::string(kPlaceholder) + " " + std::to_string(i) + "?>";
        const std::size_t at = text.find(token);
        if (at != std::string::npos) {
            text.replace(at, token.size(), doc.opaque[i].xml);
        }
    }
    return text;
}

Result<void> writeLdxFile(const std::filesystem::path& path, const LdxDocument& doc) {
    return AtomicFile::write(path, std::string_view(writeLdx(doc)));
}

} // namespace esses::formats::ld
