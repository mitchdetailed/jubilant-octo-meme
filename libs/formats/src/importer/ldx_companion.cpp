// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// The .ldx companion between the core's LdxDocument and esses_model (05 section 4 rules 3 to 5, section 5
// item 7; D-015).
//
// companionOpaque encoding. DataSource::companionOpaque holds XML strings; each string this file writes
// starts with a processing instruction <?esses-ldx ...?> that says where the XML after it belongs, so the
// export writer can put every element back where it stood:
//   * an unknown element, verbatim (byte-identical):
//       <?esses-ldx parent="LDXFile/Layers" position="2"?><Foo .../>
//   * a known structural element that was present, with its attributes:
//       <?esses-ldx element="LDXFile/Layers/Layer"?><Layer a="b"/>
//   * a marker group with its attributes:
//       <?esses-ldx group="0"?><MarkerGroup Name="Beacons" Index="N-1"/>
//   * every companion marker's original attributes (Time keeps its fraction below the microsecond):
//       <?esses-ldx marker="<marker id>"?><Marker ... />
//   * every companion detail, in file order, with its original attributes:
//       <?esses-ldx detail="Total Laps"?><String Id=".." Value=".." x=".."/>
// A consumer that only wants the unknown elements takes the strings with a parent= instruction and drops the
// instruction. A string without the instruction is written at the end of LDXFile.
#include "importer/ld_support.hpp"

#include <esses/base/assert.hpp>

#include <fast_float/fast_float.h>
#include <pugixml.hpp>

#include <algorithm>
#include <charconv>
#include <map>
#include <sstream>
#include <string_view>
#include <utility>

namespace esses::formats::ldimpl {
namespace {

using ld::LdxAttribute;
using ld::LdxDocument;

constexpr std::string_view kPi = "<?esses-ldx ";

std::string escape(std::string_view s) {
    std::string out;
    out.reserve(s.size());
    for (const char c : s) {
        switch (c) {
        case '&':
            out += "&amp;";
            break;
        case '<':
            out += "&lt;";
            break;
        case '>':
            out += "&gt;";
            break;
        case '"':
            out += "&quot;";
            break;
        case '\'':
            out += "&apos;";
            break;
        case '\n':
            out += "&#10;";
            break;
        case '\r':
            out += "&#13;";
            break;
        case '\t':
            out += "&#9;";
            break;
        default:
            out += c;
            break;
        }
    }
    return out;
}

std::string unescape(std::string_view s) {
    static const std::pair<std::string_view, char> kEntities[] = {
        {"&amp;", '&'},   {"&lt;", '<'},   {"&gt;", '>'},   {"&quot;", '"'},
        {"&apos;", '\''}, {"&#10;", '\n'}, {"&#13;", '\r'}, {"&#9;", '\t'}};
    std::string out;
    for (std::size_t i = 0; i < s.size();) {
        bool matched = false;
        if (s[i] == '&') {
            for (const auto& [entity, ch] : kEntities) {
                if (s.compare(i, entity.size(), entity) == 0) {
                    out += ch;
                    i += entity.size();
                    matched = true;
                    break;
                }
            }
        }
        if (!matched) {
            out += s[i++];
        }
    }
    return out;
}

std::string instruction(std::string_view key, std::string_view value,
                        std::optional<std::pair<std::string_view, std::string>> second = std::nullopt) {
    std::string s(kPi);
    s += key;
    s += "=\"";
    s += escape(value);
    s += '"';
    if (second) {
        s += ' ';
        s += second->first;
        s += "=\"";
        s += escape(second->second);
        s += '"';
    }
    s += "?>";
    return s;
}

std::string emptyElement(std::string_view name, const std::vector<LdxAttribute>& attrs) {
    std::string s = "<" + std::string(name);
    for (const LdxAttribute& a : attrs) {
        s += ' ';
        s += a.name;
        s += "=\"";
        s += escape(a.value);
        s += '"';
    }
    s += "/>";
    return s;
}

struct Entry {
    std::map<std::string, std::string, std::less<>> keys;
    std::string xml;
};

// Splits "<?esses-ldx k="v" ...?>rest"; nullopt when the string does not start with the instruction.
std::optional<Entry> parseEntry(std::string_view s) {
    if (!s.starts_with(kPi)) {
        return std::nullopt;
    }
    const std::size_t end = s.find("?>");
    if (end == std::string_view::npos) {
        return std::nullopt;
    }
    Entry e;
    std::string_view body = s.substr(kPi.size(), end - kPi.size());
    while (!body.empty()) {
        while (!body.empty() && body.front() == ' ') {
            body.remove_prefix(1);
        }
        const std::size_t eq = body.find("=\"");
        if (eq == std::string_view::npos) {
            break;
        }
        const std::size_t close = body.find('"', eq + 2);
        if (close == std::string_view::npos) {
            break;
        }
        e.keys.emplace(std::string(body.substr(0, eq)), unescape(body.substr(eq + 2, close - eq - 2)));
        body.remove_prefix(close + 1);
    }
    e.xml = std::string(s.substr(end + 2));
    return e;
}

std::vector<LdxAttribute> attributesOfXml(const std::string& xml) {
    std::vector<LdxAttribute> out;
    pugi::xml_document doc;
    if (!doc.load_string(xml.c_str())) {
        return out;
    }
    for (const pugi::xml_attribute& a : doc.document_element().attributes()) {
        out.push_back({a.name(), a.value()});
    }
    return out;
}

std::optional<std::string> attrValue(const std::vector<LdxAttribute>& attrs, std::string_view name) {
    for (const LdxAttribute& a : attrs) {
        if (a.name == name) {
            return a.value;
        }
    }
    return std::nullopt;
}

std::string_view detailsPath(bool inLayer) {
    return inLayer ? "LDXFile/Layers/Layer/Details" : "LDXFile/Layers/Details";
}

std::optional<double> parseDouble(std::string_view s) {
    double v = 0.0;
    // fast_float, not std::from_chars: Apple Clang 15's libc++ has no floating-point from_chars.
    const auto r = fast_float::from_chars(s.data(), s.data() + s.size(), v);
    if (r.ec != std::errc() || r.ptr != s.data() + s.size()) {
        return std::nullopt;
    }
    return v;
}

} // namespace

model::CompanionData companionFromLdx(const LdxDocument& doc) {
    model::CompanionData c;
    for (const ld::LdxDetail& d : doc.details) {
        c.details.push_back(model::Detail{d.id, d.value, model::Provenance::Companion, std::nullopt});
        // Always kept: the entries give the export the companion's detail order and attributes.
        c.opaque.push_back(instruction("detail", d.id) + emptyElement("String", d.attributes));
    }
    std::vector<model::Marker>& markers = c.markers[""];
    for (const ld::LdxMarker& m : doc.markers) {
        const auto cls = model::parseMarkerClass(m.className);
        ESSES_ASSERT_MSG(cls.has_value(), "LdxDocument holds only known marker classes");
        if (!cls) {
            continue;
        }
        const std::string group = m.group < doc.groups.size() ? doc.groups[m.group].name : std::string();
        model::Marker mk = model::makeLoggedMarker(*cls, m.tUs, group, m.name, {}, m.flagsValue, m.ordinal,
                                                   model::Provenance::Companion);
        // Always kept: the original spelling of Time (a fraction below the microsecond), Flags and Version
        // and any unknown attribute survive an export.
        c.opaque.push_back(instruction("marker", mk.id.str()) + emptyElement("Marker", m.attributes));
        markers.push_back(std::move(mk));
    }
    // The structure that was there, so the export writes it back (an empty Layer or MarkerBlock included).
    c.opaque.push_back(instruction("element", "LDXFile") + emptyElement("LDXFile", doc.rootAttributes));
    c.opaque.push_back(instruction("element", "LDXFile/Layers") +
                       emptyElement("Layers", doc.layersAttributes));
    if (doc.hasLayer) {
        c.opaque.push_back(instruction("element", "LDXFile/Layers/Layer") +
                           emptyElement("Layer", doc.layerAttributes));
    }
    if (doc.hasMarkerBlock) {
        c.opaque.push_back(instruction("element", "LDXFile/Layers/Layer/MarkerBlock") +
                           emptyElement("MarkerBlock", doc.markerBlockAttributes));
    }
    if (doc.hasDetails) {
        c.opaque.push_back(instruction("element", detailsPath(doc.detailsInLayer)) +
                           emptyElement("Details", doc.detailsAttributes));
    }
    for (std::size_t g = 0; g < doc.groups.size(); ++g) {
        c.opaque.push_back(instruction("group", std::to_string(g)) +
                           emptyElement("MarkerGroup", doc.groups[g].attributes));
    }
    for (const ld::LdxOpaque& o : doc.opaque) {
        c.opaque.push_back(
            instruction("parent", o.parentPath,
                        std::make_pair(std::string_view("position"), std::to_string(o.position))) +
            o.xml);
    }
    if (markers.empty()) {
        c.markers.clear();
    }
    return c;
}

LdxDocument ldxFromSource(const model::DataSource& source, const model::DataLayer& layer, bool withMarkers,
                          bool withDetails) {
    LdxDocument doc = LdxDocument::makeDefault();
    std::map<std::string, std::vector<LdxAttribute>, std::less<>> markerAttrs;
    std::map<std::string, std::vector<LdxAttribute>, std::less<>> detailAttrs;
    std::vector<std::string> detailOrder;
    std::map<std::uint32_t, std::vector<LdxAttribute>> groups;
    for (const std::string& s : source.companionOpaque) {
        const auto e = parseEntry(s);
        if (!e) {
            doc.opaque.push_back({"LDXFile", 0xFFFFFFFFu, s}); // foreign string: at the end of the root
            continue;
        }
        if (const auto it = e->keys.find("parent"); it != e->keys.end()) {
            std::uint32_t pos = 0xFFFFFFFFu;
            if (const auto p = e->keys.find("position"); p != e->keys.end()) {
                std::from_chars(p->second.data(), p->second.data() + p->second.size(), pos);
            }
            doc.opaque.push_back({it->second, pos, e->xml});
        } else if (const auto el = e->keys.find("element"); el != e->keys.end()) {
            const std::vector<LdxAttribute> attrs = attributesOfXml(e->xml);
            const std::string& path = el->second;
            if (path == "LDXFile") {
                doc.rootAttributes = attrs;
            } else if (path == "LDXFile/Layers") {
                doc.layersAttributes = attrs;
            } else if (path == "LDXFile/Layers/Layer") {
                doc.hasLayer = true;
                doc.layerAttributes = attrs;
            } else if (path == "LDXFile/Layers/Layer/MarkerBlock") {
                doc.hasMarkerBlock = true;
                doc.markerBlockAttributes = attrs;
            } else if (path == detailsPath(true) || path == detailsPath(false)) {
                doc.hasDetails = withDetails;
                doc.detailsInLayer = path == detailsPath(true);
                doc.detailsAttributes = attrs;
            }
        } else if (const auto g = e->keys.find("group"); g != e->keys.end()) {
            std::uint32_t index = 0;
            std::from_chars(g->second.data(), g->second.data() + g->second.size(), index);
            groups[index] = attributesOfXml(e->xml);
        } else if (const auto m = e->keys.find("marker"); m != e->keys.end()) {
            markerAttrs[m->second] = attributesOfXml(e->xml);
        } else if (const auto d = e->keys.find("detail"); d != e->keys.end()) {
            if (detailAttrs.emplace(d->second, attributesOfXml(e->xml)).second) {
                detailOrder.push_back(d->second);
            }
        }
    }
    for (auto& [index, attrs] : groups) {
        (void)index;
        doc.groups.push_back({attrValue(attrs, "Name").value_or(std::string()),
                              attrValue(attrs, "Index").value_or(std::to_string(doc.groups.size())), attrs});
    }

    if (withMarkers) {
        for (const model::Marker& m : layer.markers) {
            if (m.cls == model::MarkerClass::DeviceEvent ||
                (m.prov != model::Provenance::Companion && m.prov != model::Provenance::User)) {
                continue; // device events live in the .ld; logged and derived markers are not .ldx facts
            }
            model::MarkerClass cls = m.cls;
            if (!m.enabled && (cls == model::MarkerClass::BCN || cls == model::MarkerClass::SPLTBCN)) {
                cls = model::MarkerClass::IGRDBCN;
            }
            auto git = std::find_if(doc.groups.begin(), doc.groups.end(),
                                    [&](const ld::LdxGroup& g) { return g.name == m.group; });
            if (git == doc.groups.end()) {
                doc.groups.push_back({m.group, std::to_string(doc.groups.size()), {}});
                git = doc.groups.end() - 1;
            }
            ld::LdxMarker x;
            x.group = static_cast<std::uint32_t>(git - doc.groups.begin());
            x.className = std::string(model::markerClassName(cls));
            x.name = m.name;
            x.version = "100";
            x.flags = std::to_string(m.flags);
            x.flagsValue = m.flags;
            x.timeUs = static_cast<double>(m.t);
            x.tUs = m.t;
            x.fileIndex = static_cast<std::uint32_t>(doc.markers.size());
            if (const auto it = markerAttrs.find(m.id.str()); it != markerAttrs.end()) {
                x.attributes = it->second;
                x.version = attrValue(it->second, "Version").value_or(x.version);
                // Keep the original spelling of Flags and Time while they still say the same value.
                if (const auto f = attrValue(it->second, "Flags")) {
                    std::uint32_t v = 0;
                    const auto r = std::from_chars(f->data(), f->data() + f->size(), v);
                    if (r.ec == std::errc() && r.ptr == f->data() + f->size() && v == m.flags) {
                        x.flags = *f;
                    }
                }
                if (const auto t = attrValue(it->second, "Time"); t && !m.originalT) {
                    if (const auto v = parseDouble(*t); v && std::llround(*v) == m.t) {
                        x.timeUs = *v;
                    }
                }
            }
            doc.markers.push_back(std::move(x));
        }
        if (!doc.markers.empty()) {
            doc.hasLayer = true;
            doc.hasMarkerBlock = true;
        }
        doc.normalise();
    }
    if (withDetails) {
        // The companion's details in its order, then the other Companion and User details in source order.
        std::vector<const model::Detail*> ordered;
        for (const std::string& id : detailOrder) {
            const model::Detail* d = model::findDetail(source.details, id);
            if (d != nullptr && std::find(ordered.begin(), ordered.end(), d) == ordered.end()) {
                ordered.push_back(d);
            }
        }
        for (const model::Detail& d : source.details) {
            if (std::find(ordered.begin(), ordered.end(), &d) == ordered.end()) {
                ordered.push_back(&d);
            }
        }
        for (const model::Detail* d : ordered) {
            if (d->prov != model::Provenance::Companion && d->prov != model::Provenance::User) {
                continue;
            }
            ld::LdxDetail x;
            x.id = d->id;
            x.value = model::detailValueText(d->value);
            if (const auto it = detailAttrs.find(d->id); it != detailAttrs.end()) {
                x.attributes = it->second;
            }
            doc.details.push_back(std::move(x));
        }
        doc.hasDetails = doc.hasDetails || !doc.details.empty();
    }
    return doc;
}

bool ldxWorthWriting(const LdxDocument& doc, const model::DataSource& source) {
    return !doc.markers.empty() || !doc.details.empty() || !doc.opaque.empty() ||
           !source.companionOpaque.empty();
}

} // namespace esses::formats::ldimpl
