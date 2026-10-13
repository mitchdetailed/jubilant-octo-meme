// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// Private: one channel of a component resolved in one slot's source (RenderService::prepareInputs and
// ValuesModel share it): logged channels through the source's ChannelResolver, maths outputs (Corr Speed,
// Corr Dist) through MathsService (never blocking), the display converter from the requested unit.
#pragma once

#include <esses/base/priority.hpp>
#include <esses/model/source.hpp>
#include <esses/plotcore/component_build.hpp>
#include <esses/qtmodel/render_service.hpp>

#include <string_view>

namespace esses::qt {

class MathsService;

namespace detail {

plot::ResolvedChannel resolveChannel(MathsService& maths, const model::DataSource& source,
                                     std::string_view layer, const ChannelRequest& request,
                                     Priority priority);

} // namespace detail
} // namespace esses::qt
