// SPDX-FileCopyrightText: Libertas-Studio contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <atomic>
#include <functional>
#include <lcomp/preview.hpp>
#include <memory>
#include <thread>

// ponytail: one process per active source; pool only after measuring startup or memory limits.
class SourceWorker {
  public:
    using Callback = std::function<void(std::shared_ptr<const lcomp::PreviewFrame>)>;
    SourceWorker(const ldoc::Source& source, Callback callback,
                 std::function<void(std::span<const float>)> audio = {});
    ~SourceWorker();
    void set_active(bool active) { active_.store(active); }

  private:
    std::atomic<bool> active_{true};
    std::jthread thread_;
};
