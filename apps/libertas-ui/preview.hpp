// SPDX-FileCopyrightText: Libertas-Studio contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "ldoc/document.hpp"

#include <QObject>
#include <QString>
#include <QVariantMap>
#include <QTimer>
#include <QtQml/qqmlregistration.h>
#include <filesystem>
#include <map>
#include <memory>

class QJSEngine;
class QQmlEngine;

namespace libertas::ui {

class ProgramImageProvider;

/// The program picture, as published by `libertas-render`.
///
/// This process does not composite. It sends the document when the session says it changed and
/// keeps the latest frame of each canvas for the panel to draw. A remote session will do the
/// same send; the renderer does not know which parent it has (ADR-0040).
class Preview : public QObject {
    Q_OBJECT
    QML_ELEMENT
    QML_SINGLETON

    /// Bumped when a commit's frames have been copied. The image URL carries it so Qt reloads.
    Q_PROPERTY(int generation READ generation NOTIFY changed)
    Q_PROPERTY(bool rendererAttached READ rendererAttached NOTIFY changed)

  public:
    /// Not default-constructible. Qt's singleton registration prefers a default constructor
    /// over `create()`, and `create()` is the only one of the two that is handed the engine
    /// the image provider has to be installed on.
    explicit Preview(QObject* parent);
    ~Preview() override;

    /// The renderer to spawn. Call before the QML engine instantiates the singleton.
    static void configure(std::filesystem::path renderer);
    static Preview* create(QQmlEngine* engine, QJSEngine*);

    [[nodiscard]] int generation() const;
    [[nodiscard]] bool rendererAttached() const;
    Q_INVOKABLE [[nodiscard]] QVariantMap renderStats(const QString& canvasId) const;
    Q_INVOKABLE void resetStats();
    Q_INVOKABLE [[nodiscard]] QVariantMap audioLevel(const QString& nodeId) const;

    /// Empty until a frame has arrived for this canvas, which is when the panel keeps its own
    /// "no pipeline attached" wording. An empty id is the first canvas (ADR-0027).
    Q_INVOKABLE [[nodiscard]] QString caption(const QString& canvasId) const;
    Q_INVOKABLE [[nodiscard]] bool hasFrame(const QString& canvasId) const;
    /// The id the image URL is keyed by. Empty until a document has named a canvas.
    Q_INVOKABLE [[nodiscard]] QString canvasKey(const QString& canvasId) const;
    Q_INVOKABLE void requestSize(QObject* owner, int edge);

    /// The latest document. Called from the session's change notification, never from the
    /// return of the command that caused it (ADR-0024). A snapshot already in flight is not
    /// sent twice: the newest one goes when the renderer acknowledges.
    void publish(const ldoc::Project& project);

    /// Keeps the picture moving while something on it is live.
    ///
    /// The preview is otherwise published only when the document changes (ADR-0024), which is
    /// right for a scene made of colours and stills: nothing moves until someone moves it. A
    /// screen source breaks that assumption -- the picture changes because the desktop changed,
    /// not because the project did -- so a canvas holding one republishes on a timer.
    ///
    /// The rate is the canvas's own `fps`, not the monitor's and not a constant. A 30fps show
    /// previewed at 60 would be showing something the programme will never be, and the canvas
    /// already carries the exact rational to use (ADR-0013 on why it is never a double).
    void setLive(bool live, ldoc::Fps rate);

    friend class ProgramImageProvider;

  signals:
    void changed();
    void notice(const QString& text);

  private:
    struct Held;
    struct State;
    struct Link;

    void start();
    void tick();
    void try_send();
    void update_size();
    void on_readable();
    void on_writable();
    void lose(const QString& why);
    void reap(bool patient);
    void report(const QString& text);
    [[nodiscard]] QString resolve(const QString& canvasId) const;

    std::filesystem::path renderer_;
    std::shared_ptr<State> state_;
    QString first_canvas_;
    std::unique_ptr<Link> link_;
    QTimer* live_ = nullptr;
    ldoc::Project latest_;
    std::map<QObject*, int> preview_sizes_;
    std::uint32_t preview_edge_ = 0;
    struct RenderBaseline { std::uint64_t ns = 0, frames = 0, missed = 0; };
    std::map<QString, RenderBaseline> render_baselines_;
    std::map<QString, std::uint64_t> missed_live_ticks_;
    std::map<QString, QVariantMap> audio_levels_;
};

} // namespace libertas::ui
