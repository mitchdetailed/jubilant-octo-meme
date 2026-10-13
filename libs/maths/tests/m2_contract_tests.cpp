// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// The parts of the M2 maths contract that are final before the implementations land (07 section 5.1): the
// intrinsic catalogue's names and classes (03 section 5), the FunctionRegistry container (03 section 10.1),
// the data-less Scope (03 section 11 item 12), ArenaView and the maths-1 value types. The M2-STUB functions
// are tested by their owners.

#include <esses/maths/function_registry.hpp>
#include <esses/maths/intrinsics.hpp>
#include <esses/maths/kernel.hpp>
#include <esses/maths/math_module.hpp>
#include <esses/maths/scope.hpp>
#include <esses/units/unit_registry.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstddef>
#include <filesystem>
#include <memory>
#include <set>
#include <string>
#include <utility>

using namespace esses;
using namespace esses::maths;

namespace {

class FakeFunction final : public IMathFunction {
public:
    explicit FakeFunction(std::string name) { manifest_.name = std::move(name); }
    const FunctionManifest& manifest() const override { return manifest_; }
    Result<Value> evaluate(EvalContext& /*ctx*/, std::span<const Value> /*args*/) override {
        return Value{InvalidValue{}};
    }

private:
    FunctionManifest manifest_;
};

} // namespace

TEST_CASE("intrinsic catalogue: unique lower-case names and the classes of 03 section 5", "[unit]") {
    std::set<std::string> names;
    for (std::size_t i = 0; i < static_cast<std::size_t>(IntrinsicId::Count); ++i) {
        const auto id = static_cast<IntrinsicId>(i);
        const std::string name(intrinsicName(id));
        REQUIRE_FALSE(name.empty());
        for (const char c : name) {
            CHECK(((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_'));
        }
        CHECK(names.insert(name).second);
    }
    CHECK(intrinsicName(IntrinsicId::Count).empty());
    CHECK(intrinsicName(IntrinsicId::RoundDown) == "round_down");
    CHECK(intrinsicName(IntrinsicId::FilterChebyBs) == "filter_cheby_bs");
    CHECK(intrinsicClass(IntrinsicId::Choose) == KernelClass::Elementwise);
    CHECK(intrinsicClass(IntrinsicId::Derivative) == KernelClass::Windowed);
    CHECK(intrinsicClass(IntrinsicId::FilterLp) == KernelClass::IIR);
    CHECK(intrinsicClass(IntrinsicId::PreviousSample) == KernelClass::Stateful);
    CHECK(intrinsicClass(IntrinsicId::RangeChange) == KernelClass::Range);
    CHECK(intrinsicClass(IntrinsicId::VarianceTime) == KernelClass::Variance);
}

TEST_CASE("FunctionRegistry: case-folded lookup and external collisions", "[unit]") {
    FunctionRegistry reg;
    FunctionManifest abs;
    abs.name = "abs";
    abs.arities = {1};
    reg.addIntrinsic(abs, IntrinsicId::Abs);
    REQUIRE(reg.find("abs") != nullptr);
    CHECK(reg.find("abs")->arities == std::vector<std::uint8_t>{1});
    CHECK(reg.intrinsicId("abs") == IntrinsicId::Abs);
    CHECK(reg.find("nope") == nullptr);
    CHECK_FALSE(reg.intrinsicId("nope").has_value());

    const auto collision = reg.addExternal(std::make_shared<FakeFunction>("ABS"));
    REQUIRE_FALSE(collision.ok());
    CHECK(collision.error().context.detail == "E-FN-COLLISION");
    CHECK_FALSE(reg.addExternal(nullptr).ok());

    REQUIRE(reg.addExternal(std::make_shared<FakeFunction>("mylib.rolling_rms")).ok());
    CHECK(reg.external("mylib.rolling_rms") != nullptr);
    CHECK_FALSE(reg.intrinsicId("mylib.rolling_rms").has_value());
    REQUIRE(reg.all().size() == 2);
    CHECK(reg.all().front()->name == "abs"); // name order
}

TEST_CASE("Scope: the data-less scope resolves nothing", "[unit]") {
    const FunctionRegistry functions;
    const auto loaded =
        units::UnitRegistry::loadFiles(std::filesystem::path(ESSES_TEST_REPO_ROOT) / "data" / "units.json");
    REQUIRE(loaded.ok());
    const units::UnitRegistry& units = *loaded;
    const Scope scope(functions, units);
    CHECK_FALSE(scope.lookup("engine rpm", ChannelKey{}).has_value());
    CHECK(&scope.functions() == &functions);
    CHECK(&scope.units() == &units);
}

TEST_CASE("ArenaView: aligned bump allocation and reset", "[unit]") {
    std::array<std::byte, 64> storage{};
    ArenaView arena{storage, 0};
    void* a = arena.allocate(10, 8);
    void* b = arena.allocate(8, 16);
    REQUIRE(a != nullptr);
    REQUIRE(b != nullptr);
    CHECK(static_cast<std::byte*>(b) - static_cast<std::byte*>(a) == 16);
    CHECK(arena.allocate(64, 1) == nullptr); // does not fit any more
    arena.reset();
    CHECK(arena.allocate(64, 1) == storage.data());
}

TEST_CASE("maths-1 value types: defaults of 03 section 4.1", "[unit]") {
    const MathItem item;
    CHECK(item.kind == ItemKind::Expression);
    CHECK(item.rate.automatic);
    CHECK(item.interp == store::Interp::Linear);
    CHECK(item.moduleVersion == 1);
    CHECK(item.scale == 1.0);
    CHECK(item.table.outOfRange == TableSpec::OutOfRange::Clamp);
    const MathModule module;
    CHECK(module.precision == "single");
    CHECK(module.origin == MathModule::Origin::Global);
    CHECK(MathModule::kTransientId == "~transient");
    MathModule copy = module;
    copy.items.push_back(item);
    CHECK_FALSE(copy == module);
}
