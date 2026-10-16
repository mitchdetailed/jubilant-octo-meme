# esses_model

Layer 1, Qt-free. The data-source model of 02_DATA_MODEL.md sections 4 and 5 (D-013, D-014, D-015, D-130,
D-132, D-140): channel identity and descriptors, data layers and sources, details, markers and marker edits,
ranges and range groups, the channel resolver, the `sidecar-1` and `markers-1` documents and the whole-outing
range rule that `esses-cli laps` uses in M0.

Public headers live in `include/esses/model/` (umbrella `model.hpp`); the types are in namespace
`esses::model`. Links `esses_base`, `esses_units` and `esses_store`; nlohmann-json is a private dependency of
the document code and no JSON type appears in a public header.

| Header | Contents |
|---|---|
| `ids.hpp` | `SourceId` (= `Hash128`, the quick hash), `ChannelId`, `MarkerId`, `RangeId`, `EnumTableId`, `std::hash` for each, `kNormalLayer`, `canonicalLayerName` |
| `channel.hpp` | `SampleKind` (store's enum, re-exported), `ChannelSource`, `EnumEntry` (with `severity`), `EnumTable`, `SourceEncoding`, `ChannelDescriptor`, `ChannelStatus`, `Channel`; `channelIdLogged`, `channelIdMathsModule`, `channelIdMathsLocal`, `channelIdOther` |
| `detail.hpp` | `Provenance`, `DateTime`, `Quantityd`, `DetailValue`, `Detail`, the standard detail table, `overlayDetails`, `mergeDetails`, unit resolution |
| `marker.hpp` | `MarkerClass` (the eight D-014 classes + `DeviceEvent`), `Marker`, `MarkerEditOp`, `MarkerEdit`, `markerIdLogged`, model order (at equal times EOL before SOL, D-158), `assignLoggedIds`, device events, `mergeMarkers`, `applyMarkerEdits`, `markerSetHash` |
| `range.hpp` | `RangeKind`, `LapClass`, `Range`, `RangeGroup`, `RangeGroupTree` (per-path versions, `inheritVersions`), `RangeRef`, `rangeId` |
| `layer.hpp` | `DataLayer` with its case-folded indexes |
| `sidecar.hpp` | `Annotation`, `VideoLink`, `RangeOverride`, `SidecarLayer`, `SidecarDocument`, `SidecarApplyReport`, `hashText` |
| `source.hpp` | `SourceProvenance` (`Provenance_Source`), `CompanionData`, `DataSource`, `mergeCompanion`, `applySidecar`, `deriveFileDetails`, `newSidecarFor` |
| `resolver.hpp` | `ResolutionStep`, `ResolutionTrace`, `MappingRow/Table`, `AliasCandidate/Row/Table`, `ChannelResolver`, `makeAliasDescriptor`, `applyMappings` |
| `whole_outing.hpp` | `WholeOutingOptions`, `buildWholeOutingGroup`, `rebuildWholeOutingRanges` |
| `markers_document.hpp` | `MarkersDocument` (`markers-1`) |
| `opaque_json.hpp` | `OpaqueJson`, an alias of `esses_base`'s `JsonDoc` (M1); since M2 the model's own headers spell `JsonDoc` and keep including this header for the code that still spells the alias |
| `notes.hpp` | `ModelNote` and its codes |

## Open pipeline order (05 section 2 steps 3 to 6)

1. The importer fills `DataSource::id`, `prov`, `details` (prov `Logged`) and every layer's channels and
   markers (file order; ids from `markerIdLogged` or `assignLoggedIds`; SOL/EOL synthesised with prov
   `Derived` when the file lacks them; device events through `makeDeviceEventMarker`).
2. `mergeCompanion(src, companion)` once, with an empty `CompanionData` when there is no companion: sets
   `baseDetails` and each layer's `baseMarkers` (precedence companion > file).
3. `deriveFileDetails(src)` (the `File` group the whole-outing rule names its outing after).
4. `applySidecar(src, SidecarDocument::load(SidecarDocument::pathFor(log)))`: details and markers with the
   sidecar's edits (precedence sidecar > companion > file), always rebuilt from the base.
5. Phase-1 ranges: `rebuildWholeOutingRanges(src)` in M0 (runs the rule and the sidecar's range overrides and
   carries range-group versions over); `esses_analysis`' rule runner from M1.
6. Copy each `ModelNote` into the source's `ImportLog` under its code.

A published source is `std::shared_ptr<const DataSource>`; an edit produces a new snapshot with
`DataSource::withSidecar(doc)` (version + 1, channels and columns shared, only changed range-group paths bump).

## Note codes

`sidecar.orphan_marker_edit`, `sidecar.orphan_range_override`, `sidecar.range_override_by_name`,
`sidecar.offset_distance_pending`, `sidecar.source_changed`, `sidecar.unknown_layer`,
`sidecar.layer_details_ignored`, `companion.unknown_layer`, `model.duplicate_marker_id`,
`detail.unit_unknown`. Orphaned edits and overrides are reported and kept in the document, never dropped
(D-065).

## Deviations from the design text

- `OpaqueJson` (`localMaths`, `runs`, `stages`, unknown top-level members, `probe`) is spelled where 02 writes
  `JsonDoc`; since M1 it is an alias of `esses_base`'s `JsonDoc`, whose compact canonical text makes equality
  value equality. The one canonical writer (02 section 6.5 rule 1) and the typed JSON readers live in
  `esses_base` (`json_doc.hpp`, `json_nlohmann.hpp`); `src/json_read.hpp` only brings the readers into
  `model::detail`.
- `DataLayer::baseMarkers` and `DataSource::baseDetails` are additions: the merged file + companion state that
  every sidecar re-application starts from, so restore and removal edits need no inverse operation.
- `Quantityd` carries the unit `symbol` as written (D-074) next to the resolved `unit`; a document loaded from
  disk has `unit == kNoUnit` until `resolveUnit` / `resolveDetailUnits` runs with a registry. A sidecar round
  trip is therefore byte-identical without a registry. `MarkerEdit::dMUnit` keeps `offsetDistance`'s unit the
  same way.
- `VideoLink::file` and `SidecarDocument::sourcePathRel` are UTF-8 strings as written, not
  `std::filesystem::path`, so they round-trip byte-identically on every OS.
- `SidecarDocument::apply` returns a `SidecarApplyReport` (notes and self-healing range-override matches,
  applied by `applyHeals`) instead of `void`; `ChannelResolver::Resolution` adds the Alias descriptor and
  `ResolutionTrace` adds `unitLabel`. The resolver takes an optional `UnitRegistry*` for that label.
- `SourceProvenance` is the struct name (CamelCase lint); `Provenance_Source` is an alias.
- Loader leniency: an explicit `null` for an absent optional, upper-case UUIDs and range overrides without
  effect (dropped) are accepted and normalised on save; everything else that `sidecar-1` forbids is rejected
  with `Invalid` and the JSON pointer in `context.detail`. Duplicate offset/ignore edits for one marker and a
  second `offsetDistance` per layer are rejected on load.
- Marker order is (t, class in declaration order, id) exactly as 02 section 4.3 behaviour 8 states; an EOL at
  the same microsecond as a beacon therefore sorts before the beacon.
- Numeric details are not yet exposed as `Detail` constant channels (02 section 4.2 behaviour 3); that needs
  the maths engine's constant-series representation (M1/M2).

## M2 contract (07 §5.1)

Sources and tests are globbed since M2. `sidecar.hpp` adds the edit commands the M2 undo commands wrap:
`SidecarDocument::setAnnotation`, `removeAnnotation`, `setLocalMaths`, `restoreLogged` (`src/sidecar_edits.cpp`,
final; `tests/sidecar_edit_m2_tests.cpp`). `markerEdits` (offset, ignore, offsetDistance; restore is a command,
D-065), `rangeOverrides` keyed `(groupPath, startMarker)`, `localMaths` and `annotations` were already read and
written in M1; no `M2-STUB` is left in this library. The lap rebuild after a marker edit (AC-M2-05) needs the
circuit rules, so its test lives in `esses_analysis` (`tests/marker_edit_rebuild_tests.cpp`); the sidecar's
byte-identical round trip is part of `esses_project`'s AC-M2-07 test (`libs/model/tests/data/rich.sidecar.json`).
`OpaqueJson` (M1) is spelled `JsonDoc` in the model's headers and sources since M2; the alias stays for its
other users. Consumers: `esses_qtmodel` (`undo_commands.hpp`).
