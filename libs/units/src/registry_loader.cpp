// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// UnitRegistry::load: units-1 documents (schemas/units-1.schema.json) through nlohmann-json, with the rules
// the schema cannot express (unit_registry.hpp, schema description). nlohmann exceptions never leave this
// file.
#include "spelling.hpp"

#include <esses/units/unit_registry.hpp>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <iterator>
#include <limits>
#include <new>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace esses::units {

namespace {

using Json = nlohmann::json;

constexpr std::size_t kMaxIds = std::numeric_limits<std::uint16_t>::max(); // ids 0 ... 65534

std::string pointerToken(std::string_view key) {
    std::string out;
    for (const char c : key) {
        if (c == '~') {
            out += "~0";
        } else if (c == '/') {
            out += "~1";
        } else {
            out += c;
        }
    }
    return out;
}

std::string member(const std::string& pointer, std::string_view key) {
    return pointer + "/" + pointerToken(key);
}

std::string element(const std::string& pointer, std::size_t index) {
    return pointer + "/" + std::to_string(index);
}

std::string inQuotes(std::string_view s) {
    return "'" + std::string(s) + "'";
}

// "urn:esses:units-1", "esses:units-1", "https://<host>/.../units-1[.schema].json" -> major; nullopt when the
// id does not name a units document.
std::optional<int> unitsSchemaMajor(std::string_view id) {
    std::string_view rest;
    if (id.starts_with("urn:esses:units-")) {
        rest = id.substr(16);
    } else if (id.starts_with("esses:units-")) {
        rest = id.substr(12);
    } else if (id.starts_with("https://")) {
        const std::size_t slash = id.rfind('/');
        if (slash == std::string_view::npos || slash < 8) {
            return std::nullopt;
        }
        std::string_view file = id.substr(slash + 1);
        if (!file.starts_with("units-")) {
            return std::nullopt;
        }
        file.remove_prefix(6);
        if (file.ends_with(".schema.json")) {
            file.remove_suffix(12);
        } else if (file.ends_with(".json")) {
            file.remove_suffix(5);
        } else {
            return std::nullopt;
        }
        rest = file;
    } else {
        return std::nullopt;
    }
    int major = 0;
    const auto [end, ec] = std::from_chars(rest.data(), rest.data() + rest.size(), major);
    if (ec != std::errc() || end != rest.data() + rest.size() || major < 1) {
        return std::nullopt;
    }
    return major;
}

const Json* get(const Json& object, std::string_view key) {
    const auto it = object.find(std::string(key));
    return it == object.end() ? nullptr : &*it;
}

bool finiteNumber(const Json& v, double& out) {
    if (!v.is_number()) {
        return false;
    }
    out = v.get<double>();
    return std::isfinite(out);
}

} // namespace

// Builds a UnitRegistry document by document (friend of UnitRegistry).
class RegistryBuilder {
public:
    Result<void> addDocument(std::string_view text, bool user, const std::filesystem::path* path) {
        user_ = user;
        path_ = path != nullptr ? std::optional<std::filesystem::path>(*path) : std::nullopt;
        lenient_ = false;
        firstNewQuantity_ = reg_.quantities_.size();
        if (!user) {
            shippedUnitCount_ = 0;
        }

        Json doc;
        try {
            doc = Json::parse(text.data(), text.data() + text.size());
        } catch (const Json::parse_error& e) {
            ErrorContext ctx = context("");
            ctx.offset = e.byte > 0 ? e.byte - 1 : 0;
            return Failure{Error{ErrorCode::Format, std::string("unit table is not valid JSON: ") + e.what(),
                                 std::move(ctx)}};
        }
        if (!doc.is_object()) {
            return invalid("", "a units-1 document is a JSON object");
        }
        if (const Json* schema = get(doc, "$schema")) {
            if (!schema->is_string()) {
                return invalid("/$schema", "$schema is a string");
            }
            const std::string id = schema->get<std::string>();
            const std::optional<int> major = unitsSchemaMajor(id);
            if (!major) {
                return invalid("/$schema", "$schema " + inQuotes(id) + " does not name the units-1 schema");
            }
            if (*major > UnitRegistry::kSchemaMajor) {
                ErrorContext ctx = context("/$schema");
                ctx.schemaId = id;
                return Failure{Error{ErrorCode::Unsupported,
                                     "unit table schema " + inQuotes(id) +
                                         " is newer than this build reads (" +
                                         std::string(UnitRegistry::kSchemaId) + ")",
                                     std::move(ctx)}};
            }
        }
        if (const Json* version = get(doc, "schemaVersion")) {
            if (!version->is_number_integer() ||
                (!version->is_number_unsigned() && version->get<std::int64_t>() < 1) ||
                version->get<std::uint64_t>() == 0) {
                return invalid("/schemaVersion", "schemaVersion is an integer >= 1");
            }
            lenient_ =
                version->get<std::uint64_t>() > static_cast<std::uint64_t>(UnitRegistry::kSchemaVersion);
        }
        // Unknown top-level members are allowed (the schema keeps the top level open).
        const Json* quantities = get(doc, "quantities");
        if (quantities == nullptr) {
            return invalid("", "the member 'quantities' is required");
        }
        if (!quantities->is_array() || quantities->empty()) {
            return invalid("/quantities", "quantities is a non-empty array");
        }
        pendingDeltas_.clear();
        newQuantityPointers_.clear();
        for (std::size_t i = 0; i < quantities->size(); ++i) {
            if (auto r = addQuantity((*quantities)[i], element("/quantities", i)); !r) {
                return r;
            }
        }
        if (auto r = resolveDeltas(); !r) {
            return r;
        }
        if (auto r = checkFamilies(); !r) {
            return r;
        }
        if (!user) {
            shippedUnitCount_ = reg_.units_.size();
        }
        return ok();
    }

    Result<UnitRegistry> finish() {
        if (reg_.units_.empty()) {
            return fail(ErrorCode::Invalid, "the unit table defines no units", context(""));
        }
        return std::move(reg_);
    }

private:
    struct PendingDelta {
        QuantityId quantity;
        std::string name;
        std::string pointer;
    };

    ErrorContext context(std::string pointer) const {
        ErrorContext ctx;
        ctx.path = path_;
        ctx.schemaId = std::string(UnitRegistry::kSchemaId);
        ctx.detail = std::move(pointer);
        return ctx;
    }

    Failure invalid(std::string pointer, std::string message) const {
        std::string where = pointer.empty() ? std::string("document") : pointer;
        std::string text = (user_ ? "user unit table, " : "unit table, ") + where + ": " + std::move(message);
        return Failure{Error{ErrorCode::Invalid, std::move(text), context(std::move(pointer))}};
    }

    Result<void> checkMembers(const Json& object, const std::string& pointer,
                              std::initializer_list<std::string_view> allowed) const {
        if (lenient_) {
            return ok();
        }
        for (const auto& item : object.items()) {
            const std::string& key = item.key();
            if (std::find(allowed.begin(), allowed.end(), key) == allowed.end()) {
                return invalid(member(pointer, key), "unknown member " + inQuotes(key));
            }
        }
        return ok();
    }

    Result<std::string> readName(const Json& object, const std::string& pointer, std::string_view key,
                                 bool required) const {
        const Json* v = get(object, key);
        if (v == nullptr) {
            if (required) {
                return invalid(pointer, "the member " + inQuotes(key) + " is required");
            }
            return std::string();
        }
        if (!v->is_string() || !spelling::isValidName(v->get_ref<const std::string&>())) {
            return invalid(member(pointer, key),
                           std::string(key) +
                               " is a non-empty string without leading or trailing white space");
        }
        return v->get<std::string>();
    }

    Result<Dimension> readDimension(const Json& object, const std::string& pointer) const {
        const Json* v = get(object, "dim");
        if (v == nullptr) {
            return invalid(pointer, "the member 'dim' is required");
        }
        const std::string p = member(pointer, "dim");
        if (!v->is_array() || v->size() != Dimension::kCount) {
            return invalid(p, "dim has seven integer exponents [L, M, T, I, Theta, N, J]");
        }
        Dimension d;
        for (std::size_t k = 0; k < Dimension::kCount; ++k) {
            const Json& e = (*v)[k];
            if (!e.is_number_integer()) {
                return invalid(element(p, k), "dimension exponents are integers");
            }
            const bool tooLarge = e.is_number_unsigned() && e.get<std::uint64_t>() > 127U;
            const std::int64_t value = tooLarge ? 128 : e.get<std::int64_t>();
            if (value < -127 || value > 127) {
                return invalid(element(p, k), "dimension exponents lie in [-127, 127]");
            }
            d.exp[k] = static_cast<std::int8_t>(value);
        }
        return d;
    }

    // Names and quantity aliases share one case-folded namespace.
    Result<void> registerQuantityName(const std::string& name, QuantityId id, const std::string& pointer) {
        const std::string key = spelling::folded(name);
        if (const auto it = reg_.quantityNames_.find(key); it != reg_.quantityNames_.end()) {
            if (it->second == id) {
                return ok();
            }
            return invalid(pointer, "the quantity name " + inQuotes(name) + " is already used by quantity " +
                                        inQuotes(reg_.quantities_[it->second].name));
        }
        reg_.quantityNames_.emplace(key, id);
        return ok();
    }

    Result<void> addQuantity(const Json& q, const std::string& pointer) {
        if (!q.is_object()) {
            return invalid(pointer, "a quantity is a JSON object");
        }
        if (auto r = checkMembers(
                q, pointer,
                {"name", "aliases", "family", "dim", "base", "isDelta", "deltaFamily", "units", "note"});
            !r) {
            return r;
        }
        auto name = readName(q, pointer, "name", true);
        if (!name) {
            return Failure{name.error()};
        }
        auto family = readName(q, pointer, "family", true);
        if (!family) {
            return Failure{family.error()};
        }
        auto dim = readDimension(q, pointer);
        if (!dim) {
            return Failure{dim.error()};
        }
        const Json* base = get(q, "base");
        if (base == nullptr) {
            return invalid(pointer, "the member 'base' is required");
        }
        if (!base->is_string() || !spelling::isValidSymbol(base->get_ref<const std::string&>())) {
            return invalid(member(pointer, "base"), "base is the symbol of one of the quantity's units");
        }
        const std::string baseSymbol = base->get<std::string>();
        bool isDelta = false;
        if (const Json* d = get(q, "isDelta")) {
            if (!d->is_boolean()) {
                return invalid(member(pointer, "isDelta"), "isDelta is a boolean");
            }
            isDelta = d->get<bool>();
        }
        auto deltaFamily = readName(q, pointer, "deltaFamily", false);
        if (!deltaFamily) {
            return Failure{deltaFamily.error()};
        }
        if (isDelta && !deltaFamily->empty()) {
            return invalid(pointer, "a delta quantity has no deltaFamily of its own");
        }
        std::vector<std::string> aliases;
        if (const Json* a = get(q, "aliases")) {
            const std::string p = member(pointer, "aliases");
            if (!a->is_array()) {
                return invalid(p, "aliases is an array of names");
            }
            for (std::size_t k = 0; k < a->size(); ++k) {
                const Json& e = (*a)[k];
                if (!e.is_string() || !spelling::isValidName(e.get_ref<const std::string&>())) {
                    return invalid(element(p, k), "a quantity alias is a non-empty name");
                }
                aliases.push_back(e.get<std::string>());
            }
        }
        const Json* units = get(q, "units");
        if (units == nullptr) {
            return invalid(pointer, "the member 'units' is required");
        }
        if (!units->is_array() || units->empty()) {
            return invalid(member(pointer, "units"), "units is a non-empty array (at least the base unit)");
        }

        // A user document may extend a shipped quantity of the same name; anything else with a known name is
        // a duplicate.
        const auto known = reg_.quantityNames_.find(spelling::folded(*name));
        QuantityId id = 0;
        bool extending = false;
        if (known != reg_.quantityNames_.end()) {
            const QuantityId existing = known->second;
            if (!user_ || existing >= firstNewQuantity_ || reg_.quantities_[existing].name != *name) {
                return invalid(member(pointer, "name"), "the quantity name " + inQuotes(*name) +
                                                            " is already used by quantity " +
                                                            inQuotes(reg_.quantities_[existing].name));
            }
            const Quantity& e = reg_.quantities_[existing];
            const std::string existingDelta =
                e.deltaFamily ? reg_.quantities_[*e.deltaFamily].name : std::string();
            if (e.family != *family || e.dim != *dim || reg_.units_[e.base].symbol != baseSymbol ||
                e.isDelta != isDelta || existingDelta != *deltaFamily) {
                return invalid(pointer,
                               "a user file that extends the shipped quantity " + inQuotes(*name) +
                                   " repeats its family, dim, base, isDelta and deltaFamily unchanged");
            }
            id = existing;
            extending = true;
        } else {
            if (reg_.quantities_.size() >= kMaxIds) {
                return invalid(pointer, "too many quantities");
            }
            id = static_cast<QuantityId>(reg_.quantities_.size());
            const bool firstOfTable = !user_ && id == 0;
            if (firstOfTable && (!dim->isDimensionless() || !baseSymbol.empty())) {
                return invalid(pointer,
                               "the first quantity of the shipped table is the unitless quantity (dim all "
                               "zero, base \"\")");
            }
            Quantity quantity;
            quantity.id = id;
            quantity.name = *name;
            quantity.dim = *dim;
            quantity.family = *family;
            quantity.isDelta = isDelta;
            reg_.quantities_.push_back(std::move(quantity));
            newQuantityPointers_.push_back(pointer);
            reg_.unitsOfQuantity_.emplace_back();
            reg_.absoluteOf_.emplace_back();
            if (auto r = registerQuantityName(*name, id, member(pointer, "name")); !r) {
                return r;
            }
            if (!deltaFamily->empty()) {
                pendingDeltas_.push_back(PendingDelta{id, *deltaFamily, member(pointer, "deltaFamily")});
            }
        }
        for (std::size_t k = 0; k < aliases.size(); ++k) {
            if (auto r = registerQuantityName(aliases[k], id, element(member(pointer, "aliases"), k)); !r) {
                return r;
            }
            reg_.quantities_[id].aliases.push_back(aliases[k]);
        }

        for (std::size_t k = 0; k < units->size(); ++k) {
            if (auto r = addUnit((*units)[k], element(member(pointer, "units"), k), id); !r) {
                return r;
            }
        }

        if (!extending) {
            Quantity& quantity = reg_.quantities_[id];
            const std::vector<UnitId>& own = reg_.unitsOfQuantity_[id];
            const auto baseIt = std::find_if(own.begin(), own.end(),
                                             [&](UnitId u) { return reg_.units_[u].symbol == baseSymbol; });
            if (baseIt == own.end()) {
                return invalid(member(pointer, "base"),
                               "base " + inQuotes(baseSymbol) + " is not one of the quantity's units");
            }
            const UnitRegistry::Affine& a = reg_.affine_[*baseIt];
            if (a.fromA != 1.0 || a.fromB != 0.0) {
                return invalid(member(pointer, "base"),
                               "the base unit " + inQuotes(baseSymbol) + " has scalar 1 and offset 0");
            }
            quantity.base = *baseIt;
            if (id == 0 && !user_ && quantity.base != kNoUnit) {
                return invalid(pointer, "the first unit of the unitless quantity has the empty symbol");
            }
        }
        return ok();
    }

    Result<std::vector<std::string>> readSpellings(const Json& u, const std::string& pointer,
                                                   std::string_view key) {
        std::vector<std::string> out;
        const Json* a = get(u, key);
        if (a == nullptr) {
            return out;
        }
        const std::string p = member(pointer, key);
        if (!a->is_array()) {
            return invalid(p, std::string(key) + " is an array of strings");
        }
        for (std::size_t k = 0; k < a->size(); ++k) {
            const Json& e = (*a)[k];
            if (!e.is_string() || !spelling::isValidSpelling(e.get_ref<const std::string&>())) {
                return invalid(element(p, k), "a spelling is non-empty, without control characters, leading, "
                                              "trailing or repeated spaces");
            }
            std::string s = e.get<std::string>();
            if (std::find(out.begin(), out.end(), s) != out.end()) {
                return invalid(element(p, k), inQuotes(s) + " is listed twice");
            }
            out.push_back(std::move(s));
        }
        return out;
    }

    std::string describe(UnitId id) const {
        const Unit& u = reg_.units_[id];
        return inQuotes(u.symbol) + " (" + reg_.quantities_[u.qty].name + ")";
    }

    Failure collision(const std::string& pointer, std::string_view spelling, std::string_view how,
                      UnitId other, std::string_view self) const {
        std::string message = "the spelling " + inQuotes(spelling) + " of unit " + inQuotes(self) + " " +
                              std::string(how) + " unit " + describe(other);
        if (user_ && other < shippedUnitCount_) {
            message += "; a user file may not reuse a shipped spelling";
        }
        return invalid(pointer, std::move(message));
    }

    Result<void> addUnit(const Json& u, const std::string& pointer, QuantityId qty) {
        if (!u.is_object()) {
            return invalid(pointer, "a unit is a JSON object");
        }
        if (auto r = checkMembers(u, pointer,
                                  {"symbol", "display", "scalar", "toBase", "offset", "aliases",
                                   "caseSensitive", "compat", "note"});
            !r) {
            return r;
        }
        const Json* sym = get(u, "symbol");
        if (sym == nullptr) {
            return invalid(pointer, "the member 'symbol' is required");
        }
        if (!sym->is_string() || !spelling::isValidSymbol(sym->get_ref<const std::string&>())) {
            return invalid(
                member(pointer, "symbol"),
                "a symbol is printable ASCII with single inner spaces and no leading or trailing space");
        }
        Unit unit;
        unit.symbol = sym->get<std::string>();
        if (unit.symbol.empty() && !(reg_.units_.empty() && !user_)) {
            return invalid(member(pointer, "symbol"),
                           "the empty symbol is reserved for the unitless unit, the first unit of the table");
        }
        if (const Json* d = get(u, "display")) {
            if (!d->is_string() || !spelling::isValidSpelling(d->get_ref<const std::string&>())) {
                return invalid(member(pointer, "display"), "display is a non-empty spelling");
            }
            unit.display = d->get<std::string>();
        }
        const Json* scalar = get(u, "scalar");
        const Json* toBase = get(u, "toBase");
        if (scalar != nullptr && toBase != nullptr) {
            return invalid(pointer, "scalar and toBase are mutually exclusive");
        }
        UnitRegistry::Affine affine;
        double offset = 0.0;
        if (const Json* o = get(u, "offset")) {
            if (!finiteNumber(*o, offset)) {
                return invalid(member(pointer, "offset"), "offset is a finite number");
            }
        }
        if (scalar != nullptr) {
            double s = 0.0;
            if (!finiteNumber(*scalar, s) || s == 0.0) {
                return invalid(member(pointer, "scalar"), "scalar is a finite, non-zero number");
            }
            affine.fromA = s;
            affine.toA = 1.0 / s;
        } else if (toBase != nullptr) {
            double t = 0.0;
            if (!finiteNumber(*toBase, t) || t == 0.0) {
                return invalid(member(pointer, "toBase"), "toBase is a finite, non-zero number");
            }
            affine.toA = t;
            affine.fromA = 1.0 / t;
        }
        if (!std::isfinite(affine.fromA) || !std::isfinite(affine.toA) || affine.fromA == 0.0 ||
            affine.toA == 0.0) {
            return invalid(pointer, "the conversion factor has no finite inverse");
        }
        affine.fromB = offset;
        affine.toB = -offset * affine.toA;
        unit.scalar = affine.fromA;
        unit.offset = offset;

        auto aliases = readSpellings(u, pointer, "aliases");
        if (!aliases) {
            return Failure{aliases.error()};
        }
        for (std::size_t k = 0; k < aliases->size(); ++k) {
            if ((*aliases)[k] == unit.symbol) {
                return invalid(element(member(pointer, "aliases"), k), "an alias repeats the symbol");
            }
        }
        unit.aliases = std::move(*aliases);
        auto exactOnly = readSpellings(u, pointer, "caseSensitive");
        if (!exactOnly) {
            return Failure{exactOnly.error()};
        }
        for (std::size_t k = 0; k < exactOnly->size(); ++k) {
            const std::string& s = (*exactOnly)[k];
            if (s != unit.symbol &&
                std::find(unit.aliases.begin(), unit.aliases.end(), s) == unit.aliases.end()) {
                return invalid(element(member(pointer, "caseSensitive"), k),
                               inQuotes(s) + " is neither the symbol nor an alias of this unit");
            }
        }
        unit.caseSensitive = std::move(*exactOnly);
        if (const Json* c = get(u, "compat")) {
            if (!c->is_string() || !spelling::isValidTag(c->get_ref<const std::string&>())) {
                return invalid(member(pointer, "compat"), "compat is a non-empty lower-case token");
            }
            unit.compat = c->get<std::string>();
        }

        if (reg_.units_.size() >= kMaxIds) {
            return invalid(pointer, "too many units");
        }
        const auto id = static_cast<UnitId>(reg_.units_.size());
        unit.id = id;
        unit.qty = qty;

        // Spellings: symbol, aliases, then the display form when it is not already one of them.
        std::vector<std::pair<std::string, std::string>> spellings; // (spelling, pointer)
        spellings.emplace_back(unit.symbol, member(pointer, "symbol"));
        for (std::size_t k = 0; k < unit.aliases.size(); ++k) {
            spellings.emplace_back(unit.aliases[k], element(member(pointer, "aliases"), k));
        }
        if (!unit.display.empty() && unit.display != unit.symbol &&
            std::find(unit.aliases.begin(), unit.aliases.end(), unit.display) == unit.aliases.end()) {
            spellings.emplace_back(unit.display, member(pointer, "display"));
        }

        UnitRegistry::UnitKeys keys;
        for (const auto& [s, p] : spellings) {
            const bool exactOnlySpelling = std::find(unit.caseSensitive.begin(), unit.caseSensitive.end(),
                                                     s) != unit.caseSensitive.end();
            const std::string f = spelling::folded(s);
            const std::string n = spelling::normalised(s);
            if (const auto it = reg_.exact_.find(s); it != reg_.exact_.end() && it->second != id) {
                return collision(p, s, "is already a spelling of", it->second, unit.symbol);
            }
            if (!exactOnlySpelling) {
                if (const auto it = reg_.folded_.find(f); it != reg_.folded_.end() && it->second != id) {
                    return collision(p, s, "equals, after case folding, a spelling of", it->second,
                                     unit.symbol);
                }
                if (const auto it = reg_.normalised_.find(n);
                    it != reg_.normalised_.end() && it->second != id) {
                    return collision(p, s, "equals, after the spelling normaliser, a spelling of", it->second,
                                     unit.symbol);
                }
            }
            // Within one quantity every spelling folds (scoped lookups), so case variants must differ there
            // too.
            for (const UnitId other : reg_.unitsOfQuantity_[qty]) {
                const UnitRegistry::UnitKeys& otherKeys = reg_.keys_[other];
                if (std::find(otherKeys.folded.begin(), otherKeys.folded.end(), f) !=
                        otherKeys.folded.end() ||
                    std::find(otherKeys.normalised.begin(), otherKeys.normalised.end(), n) !=
                        otherKeys.normalised.end()) {
                    return collision(p, s,
                                     "equals, after case folding or the normaliser, a spelling of the "
                                     "same quantity's",
                                     other, unit.symbol);
                }
            }
            keys.exact.push_back(s);
            keys.folded.push_back(f);
            keys.normalised.push_back(n);
        }
        for (std::size_t k = 0; k < spellings.size(); ++k) {
            const std::string& s = spellings[k].first;
            reg_.exact_.emplace(s, id);
            const bool exactOnlySpelling = std::find(unit.caseSensitive.begin(), unit.caseSensitive.end(),
                                                     s) != unit.caseSensitive.end();
            if (!exactOnlySpelling) {
                reg_.folded_.emplace(keys.folded[k], id);
                reg_.normalised_.emplace(keys.normalised[k], id);
            }
        }
        reg_.units_.push_back(std::move(unit));
        reg_.affine_.push_back(affine);
        reg_.keys_.push_back(std::move(keys));
        reg_.unitsOfQuantity_[qty].push_back(id);
        return ok();
    }

    Result<void> resolveDeltas() {
        for (const PendingDelta& pd : pendingDeltas_) {
            const auto it = reg_.quantityNames_.find(spelling::folded(pd.name));
            if (it == reg_.quantityNames_.end()) {
                return invalid(pd.pointer, "deltaFamily " + inQuotes(pd.name) + " names no quantity");
            }
            Quantity& q = reg_.quantities_[pd.quantity];
            const Quantity& target = reg_.quantities_[it->second];
            if (!target.isDelta) {
                return invalid(pd.pointer,
                               "deltaFamily " + inQuotes(pd.name) + " names a quantity without isDelta");
            }
            if (target.dim != q.dim) {
                return invalid(pd.pointer, "deltaFamily " + inQuotes(pd.name) + " has a different dimension");
            }
            q.deltaFamily = target.id;
            if (!reg_.absoluteOf_[target.id]) {
                reg_.absoluteOf_[target.id] = q.id;
            }
        }
        return ok();
    }

    Result<void> checkFamilies() const {
        for (std::size_t i = firstNewQuantity_; i < reg_.quantities_.size(); ++i) {
            const Quantity& q = reg_.quantities_[i];
            for (std::size_t j = 0; j < i; ++j) {
                const Quantity& other = reg_.quantities_[j];
                if (other.family == q.family && (other.dim != q.dim || other.isDelta != q.isDelta)) {
                    return invalid(member(newQuantityPointers_[i - firstNewQuantity_], "family"),
                                   "quantities sharing the family " + inQuotes(q.family) +
                                       " share dimension and isDelta (" + inQuotes(other.name) + " differs)");
                }
            }
        }
        return ok();
    }

    UnitRegistry reg_;
    bool user_ = false;
    bool lenient_ = false;
    std::optional<std::filesystem::path> path_;
    std::size_t firstNewQuantity_ = 0;
    std::size_t shippedUnitCount_ = 0;
    std::vector<PendingDelta> pendingDeltas_;
    std::vector<std::string> newQuantityPointers_; // JSON pointer of each quantity this document adds
};

namespace {

Result<std::string> readText(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        ErrorContext ctx;
        ctx.path = path;
        return fail(ErrorCode::Io, "cannot open the unit table", std::move(ctx));
    }
    std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    if (in.bad()) {
        ErrorContext ctx;
        ctx.path = path;
        return fail(ErrorCode::Io, "cannot read the unit table", std::move(ctx));
    }
    return text;
}

Result<UnitRegistry> build(std::string_view unitsJson, const std::filesystem::path* unitsPath,
                           std::string_view userJson, const std::filesystem::path* userPath) {
    try {
        RegistryBuilder builder;
        if (auto r = builder.addDocument(unitsJson, false, unitsPath); !r) {
            return Failure{r.error()};
        }
        if (!userJson.empty()) {
            if (auto r = builder.addDocument(userJson, true, userPath); !r) {
                return Failure{r.error()};
            }
        }
        return builder.finish();
    } catch (const std::bad_alloc&) {
        return fail(ErrorCode::OutOfMemory, "out of memory while loading the unit table");
    } catch (const std::exception& e) {
        return fail(ErrorCode::Internal,
                    std::string("unexpected error while loading the unit table: ") + e.what());
    }
}

} // namespace

Result<UnitRegistry> UnitRegistry::load(std::string_view unitsJson, std::string_view userJson) {
    return build(unitsJson, nullptr, userJson, nullptr);
}

Result<UnitRegistry> UnitRegistry::loadFiles(const std::filesystem::path& unitsFile,
                                             const std::filesystem::path& userFile) {
    auto units = readText(unitsFile);
    if (!units) {
        return Failure{units.error()};
    }
    std::string user;
    if (!userFile.empty()) {
        std::error_code ec;
        if (std::filesystem::exists(userFile, ec)) {
            auto text = readText(userFile);
            if (!text) {
                return Failure{text.error()};
            }
            user = std::move(*text);
        }
    }
    return build(*units, &unitsFile, user, user.empty() ? nullptr : &userFile);
}

} // namespace esses::units
