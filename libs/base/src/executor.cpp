// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
#include <esses/base/executor.hpp>

#include <utility>

namespace esses {

void InlineExecutor::run(Priority /*priority*/, TaskKey /*key*/, CancelToken token,
                         std::function<void(CancelToken)> fn) {
    if (!token.cancelled() && fn) {
        fn(std::move(token));
    }
}

} // namespace esses
