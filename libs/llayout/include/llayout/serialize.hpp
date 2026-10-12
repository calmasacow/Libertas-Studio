// SPDX-FileCopyrightText: Libertas-Studio contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "llayout/layout.hpp"

#include <expected>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

/// Layout persistence. TOML, because the project already has to read TOML (Omarchy's
/// `colors.toml` for theme following, and effect manifests per ADR-0004), and because a layout
/// file a user can read and hand-edit is a layout file a user can report a bug about.
namespace llayout {

inline constexpr int kLayoutFormatVersion = 1;

[[nodiscard]] std::string to_toml(const Layout& l);

[[nodiscard]] std::expected<Layout, std::string> from_toml(std::string_view text);

/// Atomic, fsynced, versioned (ADR-0015 rule 5).
[[nodiscard]] std::expected<void, std::string> save(const Layout& l,
                                                    const std::filesystem::path& path);

[[nodiscard]] std::expected<Layout, std::string> load(const std::filesystem::path& path);

struct LoadOutcome {
    Layout layout;
    /// Empty when the file loaded. Otherwise what went wrong, for the UI to surface -- a layout
    /// that fails to parse falls back to the default *and says so*, rather than silently
    /// restoring something incoherent.
    std::string complaint;
    /// True when `layout` is the built-in default rather than what was on disk.
    bool used_default = false;
};

/// What the application calls at startup. Never fails.
[[nodiscard]] LoadOutcome load_or_default(const std::filesystem::path& path);

} // namespace llayout
