// SPDX-FileCopyrightText: Libertas-Studio contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QObject>
#include <QtQml/qqmlregistration.h>
#include <filesystem>

class QJSEngine;
class QQmlEngine;

namespace libertas::ui {

/// Machine-wide settings from Edit > Preferences, kept in preferences.json beside the other
/// configuration. They belong to this computer, not to a collection.
class Preferences : public QObject {
    Q_OBJECT
    QML_ELEMENT
    QML_SINGLETON

    /// GPU memory the compositor may use, in MiB. 0 means automatic: half the GPU memory the
    /// driver reports free at renderer start.
    Q_PROPERTY(int gpuBudgetMib READ gpuBudgetMib NOTIFY changed)

  public:
    /// Where preferences live. Call before the QML engine instantiates the singleton; this
    /// also exports them to the environment the render process inherits.
    static void configure(std::filesystem::path path);
    static Preferences* create(QQmlEngine*, QJSEngine*);

    [[nodiscard]] int gpuBudgetMib() const;
    /// Saves, then restarts the render process so it takes effect. False when out of range
    /// (0, or 256 MiB to 65536 MiB) or the file cannot be written.
    Q_INVOKABLE bool setGpuBudgetMib(int mib);

  signals:
    void changed();
    /// Asks the preview to restart the render process.
    void restartRenderer();

  private:
    using QObject::QObject;
};

} // namespace libertas::ui
