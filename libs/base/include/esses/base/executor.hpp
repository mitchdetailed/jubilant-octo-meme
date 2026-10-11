// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// Executor (01 section 5.1, D-049): the abstract submit used by maths and plot code so tests can run work
// inline. TaskPool is the only production implementation; InlineExecutor is the test-only one.
#pragma once

#include <esses/base/cancel.hpp>
#include <esses/base/config.hpp>
#include <esses/base/priority.hpp>

#include <functional>

namespace esses {

class Executor {
public:
    Executor() = default;
    Executor(const Executor&) = delete;
    Executor& operator=(const Executor&) = delete;
    virtual ~Executor() = default;
    virtual void run(Priority priority, TaskKey key, CancelToken token,
                     std::function<void(CancelToken)> fn) = 0;
};

// Runs every task immediately on the calling thread (unless its token is already cancelled). Test-only.
class InlineExecutor final : public Executor {
public:
    void run(Priority priority, TaskKey key, CancelToken token, std::function<void(CancelToken)> fn) override;
};

} // namespace esses
