// SPDX-FileCopyrightText: Libertas-Studio contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "ldoc/command.hpp"

#include <deque>
#include <expected>
#include <string>

namespace ldoc {

/// A project plus its undo stack.
///
/// Undo is not a second mechanism here. Applying a command returns the command that reverses it,
/// so undoing is `apply()` again with that one -- and undoing *that* returns the original effect,
/// which is what goes on the redo stack. There is one code path, so there is one set of bugs,
/// and anything that can be undone can also be journaled and replayed.
///
/// Compare `llayout::History`, which snapshots: a workspace layout is a few dozen nodes, while a
/// project is scenes full of them, and two hundred snapshots of that is a different proposition.
/// Commands also carry *what the operator did*, which is what the undo menu needs to say.
class DocumentHistory {
public:
    explicit DocumentHistory(Project project, std::size_t limit = 200);

    [[nodiscard]] const Project& project() const { return project_; }

    /// Apply a command and record it. On failure nothing changed and nothing was recorded.
    /// Returns the description, for the undo menu and the status line.
    std::expected<std::string, std::string> run(const Command& command);

    [[nodiscard]] bool can_undo() const { return !past_.empty(); }
    [[nodiscard]] bool can_redo() const { return !future_.empty(); }
    /// "Hide Camera", for an Undo menu item that says what it will undo.
    [[nodiscard]] std::string undo_description() const;
    [[nodiscard]] std::string redo_description() const;

    /// Both return the command that was *actually applied* to the document, which for an undo
    /// is the inverse. A listener -- or, once the document lives in another process, a replica of
    /// it -- can then update precisely instead of re-reading everything.
    std::expected<Command, std::string> undo();
    std::expected<Command, std::string> redo();

    [[nodiscard]] std::size_t undo_depth() const { return past_.size(); }
    [[nodiscard]] std::size_t redo_depth() const { return future_.size(); }

    /// Replace the project and forget the history, for loading a different one.
    void reset(Project project);

    /// Record that the document was written as this generation.
    ///
    /// Not `reset`: saving must not throw away the undo stack. The generation is file metadata
    /// rather than document content -- it is not something the operator did and not something
    /// they can undo -- so it is set here without touching the history.
    void note_saved(std::uint64_t generation);

    /// Apply a command without recording it, for replaying a journal into a freshly loaded
    /// project: those commands already happened, and they are not undo steps for this session.
    std::expected<void, std::string> replay(const Command& command);

private:
    struct Step {
        Command undo;
        std::string description;
    };

    Project project_;
    std::deque<Step> past_;
    std::deque<Step> future_;
    std::size_t limit_;
};

} // namespace ldoc
