// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
#include <esses/base/utf8.hpp>
#include <esses/model/source.hpp>
#include <esses/model/whole_outing.hpp>

#include <algorithm>
#include <string>
#include <utility>

namespace esses::model {

const DataLayer* DataSource::layer(std::string_view name) const {
    const std::string_view key = canonicalLayerName(name);
    for (const DataLayer& l : layers) {
        if (canonicalLayerName(l.name) == key) {
            return &l;
        }
    }
    return nullptr;
}

DataLayer* DataSource::layer(std::string_view name) {
    const std::string_view key = canonicalLayerName(name);
    for (DataLayer& l : layers) {
        if (canonicalLayerName(l.name) == key) {
            return &l;
        }
    }
    return nullptr;
}

std::shared_ptr<const DataSource> DataSource::withSidecar(SidecarDocument doc, SidecarApplyReport* report,
                                                          const RangeRebuild& rebuild) const {
    auto next = std::make_shared<DataSource>(*this); // channels share their columns (Column is a handle)
    SidecarApplyReport rep = applySidecar(*next, std::move(doc));
    rep.append(rebuild ? rebuild(*next) : rebuildWholeOutingRanges(*next));
    for (std::size_t i = 0; i < next->layers.size() && i < layers.size(); ++i) {
        next->layers[i].ranges.inheritVersions(layers[i].ranges);
    }
    next->version = version + 1;
    if (report != nullptr) {
        *report = std::move(rep);
    }
    return next;
}

std::vector<ModelNote> mergeCompanion(DataSource& src, CompanionData companion) {
    std::vector<ModelNote> notes;
    for (Detail& d : companion.details) {
        d.prov = Provenance::Companion;
    }
    src.baseDetails = overlayDetails(src.details, companion.details);
    src.details = src.baseDetails;

    for (DataLayer& l : src.layers) {
        std::vector<Marker> fromCompanion;
        const std::string_view key = canonicalLayerName(l.name);
        if (auto it = companion.markers.find(key); it != companion.markers.end()) {
            fromCompanion = it->second;
        }
        if (key == kNormalLayer) {
            if (auto it = companion.markers.find(std::string_view{}); it != companion.markers.end()) {
                fromCompanion.insert(fromCompanion.end(), it->second.begin(), it->second.end());
            }
        }
        l.baseMarkers = mergeMarkers(std::move(l.markers), fromCompanion, &notes, key);
        l.markers = l.baseMarkers;
    }
    for (const auto& [name, markers] : companion.markers) {
        if (!markers.empty() && src.layer(name) == nullptr) {
            notes.push_back(ModelNote{ModelNote::Level::Warning,
                                      std::string(kNoteCompanionUnknownLayer),
                                      "the companion has " + std::to_string(markers.size()) +
                                          " markers for layer '" + std::string(canonicalLayerName(name)) +
                                          "', which the log does not have; they are ignored",
                                      std::string(canonicalLayerName(name)),
                                      {}});
        }
    }
    src.companionOpaque = std::move(companion.opaque);
    return notes;
}

SidecarApplyReport applySidecar(DataSource& src, SidecarDocument doc) {
    SidecarApplyReport report;
    report.notes = doc.checkSource(src.id, src.prov.contentHash);

    if (src.baseDetails.empty() && !src.details.empty()) {
        src.baseDetails = src.details; // mergeCompanion was not called: the file's details are the base
    }
    // Derived details (the File group, statistics) are not part of the base; carry them over.
    std::vector<Detail> derived;
    for (const Detail& d : src.details) {
        if (d.prov == Provenance::Derived && findDetail(src.baseDetails, d.id) == nullptr) {
            derived.push_back(d);
        }
    }
    src.details = overlayDetails(src.baseDetails, std::as_const(doc).layer(kNormalLayer).details);
    for (Detail& d : derived) {
        if (findDetail(src.details, d.id) == nullptr) {
            src.details.push_back(std::move(d));
        }
    }

    for (const auto& [name, layerEdits] : doc.layers()) {
        if (layerEdits.empty()) {
            continue;
        }
        if (src.layer(name) == nullptr) {
            report.notes.push_back(ModelNote{ModelNote::Level::Warning,
                                             std::string(kNoteUnknownLayer),
                                             "the sidecar holds edits for layer '" + name +
                                                 "', which the log does not have; they are kept",
                                             name,
                                             {}});
        } else if (name != kNormalLayer && !layerEdits.details.empty()) {
            report.notes.push_back(ModelNote{
                ModelNote::Level::Info,
                std::string(kNoteLayerDetailsIgnored),
                "details of layer '" + name + "' are kept in the sidecar; source details come from 'Normal'",
                name,
                {}});
        }
    }
    for (DataLayer& l : src.layers) {
        report.append(doc.applyMarkers(l));
    }
    src.sidecar = std::move(doc);
    return report;
}

void deriveFileDetails(DataSource& src) {
    std::erase_if(src.details, [](const Detail& d) {
        return d.prov == Provenance::Derived &&
               (d.id == "File Name" || d.id == "File Path" || d.id == "File Size");
    });
    const auto add = [&src](std::string id, DetailValue value) {
        if (Detail* existing = findDetail(src.details, id)) {
            existing->value = std::move(value);
            existing->prov = Provenance::Derived;
            existing->original.reset();
        } else {
            src.details.push_back(Detail{std::move(id), std::move(value), Provenance::Derived, std::nullopt});
        }
    };
    add("File Name", pathToUtf8(src.prov.path.filename()));
    add("File Path", pathToUtf8(src.prov.path));
    Quantityd size;
    size.v = static_cast<double>(src.prov.size);
    size.dps = 0;
    add("File Size", std::move(size));
}

SidecarDocument newSidecarFor(const DataSource& src) {
    SidecarDocument doc;
    doc.sourcePathRel = pathToUtf8(src.prov.path.filename());
    doc.sourceSize = src.prov.size;
    doc.sourceHash = src.id;
    doc.sourceContentHash = src.prov.contentHash;
    doc.importerId = src.prov.importerId;
    doc.importerVersion = src.prov.importerVersion;
    for (const auto& c : src.prov.companions) {
        doc.companions.push_back(pathToUtf8(c.filename()));
    }
    std::sort(doc.companions.begin(), doc.companions.end());
    doc.companions.erase(std::unique(doc.companions.begin(), doc.companions.end()), doc.companions.end());
    doc.edited = DateTime::nowUtc();
    return doc;
}

} // namespace esses::model
