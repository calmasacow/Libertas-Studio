// SPDX-FileCopyrightText: Libertas-Studio contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ldoc/history.hpp"

#include <algorithm>
#include <format>
#include <utility>

namespace ldoc {

DocumentHistory::DocumentHistory(Project project, std::size_t limit)
    : project_(std::move(project)), limit_(std::max<std::size_t>(limit, 1)) {}

std::expected<std::string, std::string> DocumentHistory::run(const Command& command) {
    auto applied = apply(project_, command);
    if (!applied)
        return std::unexpected(applied.error());

    past_.push_back(Step{std::move(applied->inverse), applied->description});
    // A new action makes the redo tail unreachable: the operator has chosen a different branch.
    future_.clear();
    while (past_.size() > limit_)
        past_.pop_front();
    return past_.back().description;
}

std::expected<void, std::string> DocumentHistory::replay(const Command& command) {
    auto applied = apply(project_, command);
    if (!applied)
        return std::unexpected(applied.error());
    return {};
}

std::string DocumentHistory::undo_description() const {
    return past_.empty() ? std::string{} : past_.back().description;
}

std::string DocumentHistory::redo_description() const {
    return future_.empty() ? std::string{} : future_.front().description;
}

std::expected<void, std::string> DocumentHistory::undo() {
    if (past_.empty())
        return std::unexpected("there is nothing to undo");

    const Step step = past_.back();
    auto applied = apply(project_, step.undo);
    if (!applied) {
        // The stack and the document have disagreed, which should be impossible: every inverse
        // was built from the state it reverses. Say so rather than silently dropping the step
        // and leaving the stack pointing at a document it no longer describes.
        return std::unexpected(
            std::format("'{}' could not be undone: {}", step.description, applied.error()));
    }
    past_.pop_back();
    future_.push_front(Step{std::move(applied->inverse), step.description});
    while (future_.size() > limit_)
        future_.pop_back();
    return {};
}

std::expected<void, std::string> DocumentHistory::redo() {
    if (future_.empty())
        return std::unexpected("there is nothing to redo");

    const Step step = future_.front();
    auto applied = apply(project_, step.undo);
    if (!applied)
        return std::unexpected(
            std::format("'{}' could not be redone: {}", step.description, applied.error()));
    future_.pop_front();
    past_.push_back(Step{std::move(applied->inverse), step.description});
    while (past_.size() > limit_)
        past_.pop_front();
    return {};
}

void DocumentHistory::reset(Project project) {
    project_ = std::move(project);
    past_.clear();
    future_.clear();
}

} // namespace ldoc
