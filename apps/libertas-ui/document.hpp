// SPDX-FileCopyrightText: Libertas-Studio contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "documentmodels.hpp"

#include "lsession/local_session.hpp"

#include <QAbstractItemModel>
#include <QObject>
#include <QString>
#include <QStringList>
#include <QTimer>
#include <QVariantMap>
#include <QtQml/qqmlregistration.h>

#include <filesystem>
#include <memory>

class QJSEngine;
class QQmlEngine;

namespace libertas::ui {

/// The QML-facing face of the document, and the only route the UI has to it.
///
/// Everything here turns a gesture into an `ldoc::Command` and hands it to the session. Nothing
/// in this class, and nothing in QML, holds a mutable `Project` -- ADR-0024's first rule, and the
/// reason the document can move to another process later without the UI noticing.
///
/// Models are rebuilt from the session's change notification rather than from the return value of
/// the call that caused it. That is ADR-0024's second rule, and it is what makes a remote session
/// a drop-in: the notification is the only thing that is guaranteed to arrive after the document
/// has actually changed.
class Document : public QObject {
    Q_OBJECT
    QML_ELEMENT
    QML_SINGLETON

    Q_PROPERTY(QAbstractItemModel* scenes READ scenesModel CONSTANT)
    Q_PROPERTY(QAbstractItemModel* layers READ layersModel CONSTANT)
    Q_PROPERTY(QAbstractItemModel* sources READ sourcesModel CONSTANT)

    Q_PROPERTY(QString projectName READ projectName NOTIFY changed)
    Q_PROPERTY(QString projectPath READ projectPath NOTIFY changed)
    Q_PROPERTY(bool dirty READ dirty NOTIFY changed)
    Q_PROPERTY(int revision READ revision NOTIFY changed)

    Q_PROPERTY(bool canUndo READ canUndo NOTIFY changed)
    Q_PROPERTY(bool canRedo READ canRedo NOTIFY changed)
    Q_PROPERTY(QString undoDescription READ undoDescription NOTIFY changed)
    Q_PROPERTY(QString redoDescription READ redoDescription NOTIFY changed)

    Q_PROPERTY(QString selectedScene READ selectedScene NOTIFY selectionChanged)
    Q_PROPERTY(QString selectedLayer READ selectedLayer NOTIFY selectionChanged)
    /// Everything the properties panel shows about the selected layer, or empty when nothing is
    /// selected.
    Q_PROPERTY(QVariantMap layerProperties READ layerProperties NOTIFY selectionChanged)

    Q_PROPERTY(QStringList problems READ problems NOTIFY changed)
    Q_PROPERTY(QStringList blendModes READ blendModes CONSTANT)

public:
    explicit Document(QObject* parent = nullptr);
    ~Document() override;

    /// Where to open from. Empty means a new, unsaved document. Call before the QML engine
    /// instantiates the singleton.
    static void configure(std::filesystem::path project_dir);
    static Document* create(QQmlEngine*, QJSEngine*);

    [[nodiscard]] QAbstractItemModel* scenesModel() { return &scenes_; }
    [[nodiscard]] QAbstractItemModel* layersModel() { return &layers_; }
    [[nodiscard]] QAbstractItemModel* sourcesModel() { return &sources_; }

    [[nodiscard]] QString projectName() const;
    [[nodiscard]] QString projectPath() const;
    [[nodiscard]] bool dirty() const;
    [[nodiscard]] int revision() const;
    [[nodiscard]] bool canUndo() const;
    [[nodiscard]] bool canRedo() const;
    [[nodiscard]] QString undoDescription() const;
    [[nodiscard]] QString redoDescription() const;
    [[nodiscard]] QString selectedScene() const { return selected_scene_; }
    [[nodiscard]] QString selectedLayer() const { return selected_layer_; }
    [[nodiscard]] QVariantMap layerProperties() const;
    [[nodiscard]] QStringList problems() const;
    [[nodiscard]] QStringList blendModes() const;

    // ---- selection (UI state, not document state) -------------------------------------------

    Q_INVOKABLE void selectScene(const QString& id);
    Q_INVOKABLE void selectLayer(const QString& id);

    // ---- scenes -----------------------------------------------------------------------------

    Q_INVOKABLE void addScene(const QString& name);
    Q_INVOKABLE void removeScene(const QString& id);
    Q_INVOKABLE void renameScene(const QString& id, const QString& name);
    Q_INVOKABLE void duplicateScene(const QString& id);
    /// Make a scene this canvas's safety net (ADR-0025). Checks suitability first and reports
    /// what is wrong rather than silently accepting a net that will not hold.
    Q_INVOKABLE void setEmergencyScene(const QString& id);
    /// What is wrong with using this scene as a safety net, for the confirmation the designation
    /// flow shows. Empty when it is suitable.
    Q_INVOKABLE [[nodiscard]] QStringList emergencySceneProblems(const QString& id) const;

    // ---- layers -----------------------------------------------------------------------------

    Q_INVOKABLE void addLayer(const QString& name);
    Q_INVOKABLE void addGroup(const QString& name);
    Q_INVOKABLE void removeLayer(const QString& id);
    Q_INVOKABLE void renameLayer(const QString& id, const QString& name);
    Q_INVOKABLE void setLayerVisible(const QString& id, bool visible);
    Q_INVOKABLE void setLayerLocked(const QString& id, bool locked);
    /// Negative moves towards the front of the list, which is towards the top of the stack.
    Q_INVOKABLE void moveLayer(const QString& id, int delta);

    // ---- the selected layer's properties -----------------------------------------------------

    Q_INVOKABLE void setLayerPosition(double x, double y);
    Q_INVOKABLE void setLayerScale(double x, double y);
    Q_INVOKABLE void setLayerRotation(double degrees);
    Q_INVOKABLE void setLayerOpacity(double opacity);
    Q_INVOKABLE void setLayerBlendMode(const QString& mode);

    // ---- history and persistence -------------------------------------------------------------

    Q_INVOKABLE void undo();
    Q_INVOKABLE void redo();
    Q_INVOKABLE void save();

    // ---- for other C++ bridges, deliberately not Q_INVOKABLE -------------------------------
    //
    // Reachable from C++ so that Shortcuts can resolve an action against the real project and
    // hand back the command it produced; not reachable from QML, which must keep going through
    // the typed invokables above. A const reference either way: nothing outside the session
    // holds a mutable Project.

    [[nodiscard]] const ldoc::Project& projectForBindings() const;
    void runCommand(const ldoc::Command& command);

signals:
    void changed();
    void selectionChanged();
    /// Something to say in the status bar: a refused command, a failed save, a recovered journal.
    void notice(const QString& text);

private:
    void rebuild();
    void run(const ldoc::Command& command);
    [[nodiscard]] const ldoc::Scene* current_scene() const;
    [[nodiscard]] const ldoc::Node* current_layer() const;
    /// The canvas the shell is currently working against. One canvas until Phase 2 gives the UI
    /// a way to choose; taking the first is honest rather than pretending otherwise.
    [[nodiscard]] const ldoc::Canvas* current_canvas() const;

    std::unique_ptr<lsession::Session> session_;
    SceneModel scenes_;
    LayerModel layers_;
    SourceModel sources_;

    QString selected_scene_;
    QString selected_layer_;
    QTimer autosave_;
};

} // namespace libertas::ui
