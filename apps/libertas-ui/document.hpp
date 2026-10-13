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
#include <QVariantList>
#include <QVariantMap>
#include <QtQml/qqmlregistration.h>

#include <filesystem>
#include <functional>
#include <memory>
#include <string_view>

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
    Q_PROPERTY(QAbstractItemModel* buses READ busesModel CONSTANT)
    /// The scene list filtered to the starred ones. A second model rather than a filter proxy
    /// in QML: the favourites panel must not be able to change the scene list's own selection
    /// or ordering by being looked at.
    Q_PROPERTY(QAbstractItemModel* favouriteScenes READ favouriteScenesModel CONSTANT)
    Q_PROPERTY(QAbstractItemModel* decks READ decksModel CONSTANT)
    Q_PROPERTY(QAbstractItemModel* outputs READ outputsModel CONSTANT)

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
    [[nodiscard]] QAbstractItemModel* busesModel() { return &buses_; }
    [[nodiscard]] QAbstractItemModel* favouriteScenesModel() { return &favourites_; }
    [[nodiscard]] QAbstractItemModel* decksModel() { return &decks_; }
    [[nodiscard]] QAbstractItemModel* outputsModel() { return &outputs_; }

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
    /// Negative moves it towards the top of the list.
    Q_INVOKABLE void moveScene(const QString& id, int delta);
    /// Mark a scene as one to reach for. Favourites are part of the document, not the layout:
    /// they travel with the project and are the same set whichever workspace is on screen.
    Q_INVOKABLE void setSceneFavourite(const QString& id, bool favourite);
    /// Make a scene this canvas's safety net (ADR-0025). Checks suitability first and reports
    /// what is wrong rather than silently accepting a net that will not hold.
    Q_INVOKABLE void setEmergencyScene(const QString& id);
    /// What is wrong with using this scene as a safety net, for the confirmation the designation
    /// flow shows. Empty when it is suitable.
    Q_INVOKABLE [[nodiscard]] QStringList emergencySceneProblems(const QString& id) const;

    // ---- inputs (project-level source instances) --------------------------------------------

    /// The source types this build can hold in a document. None of them render yet; the list is
    /// here because a document that cannot name what it is waiting for is harder to build a
    /// pipeline against than one that can.
    Q_PROPERTY(QVariantList sourceTypes READ sourceTypes CONSTANT)
    [[nodiscard]] QVariantList sourceTypes() const;

    Q_INVOKABLE void addSource(const QString& name, const QString& type);
    Q_INVOKABLE void removeSource(const QString& id);
    Q_INVOKABLE void renameSource(const QString& id, const QString& name);
    /// Bind the selected layer to this source, or to nothing when the id is empty.
    Q_INVOKABLE void bindSelectedLayer(const QString& sourceId);

    // ---- buses (the audio graph) -------------------------------------------------------------
    //
    // Nothing here makes a sound: there is no audio host yet, so the mixer edits routing and
    // levels in the document and says plainly that it is not metering anything. The alternative
    // -- drawing moving meters off a timer -- would make the panel look finished and make it
    // impossible to tell later which half works.

    /// The bus kinds a document can hold, as {id, title, note} for the add menu.
    Q_PROPERTY(QVariantList busKinds READ busKinds CONSTANT)
    [[nodiscard]] QVariantList busKinds() const;

    Q_INVOKABLE void addBus(const QString& name, const QString& kind);
    Q_INVOKABLE void removeBus(const QString& id);
    Q_INVOKABLE void renameBus(const QString& id, const QString& name);
    /// `dragging` true while the fader is under the pointer: the whole drag becomes one undo
    /// step that lands back where it started. The caller must finish with `endGesture()`.
    Q_INVOKABLE void setBusGain(const QString& id, double gainDb, bool dragging = false);
    /// The operator let go. Harmless if no drag was in progress.
    Q_INVOKABLE void endGesture();
    Q_INVOKABLE void setBusPan(const QString& id, double pan);
    Q_INVOKABLE void setBusMute(const QString& id, bool mute);
    Q_INVOKABLE void setBusSolo(const QString& id, bool solo);

    /// Send the selected layer's audio to this bus, or to nothing when the id is empty.
    /// Unrouted is not the same as muted, and the mixer says which it is.
    Q_INVOKABLE void routeSelectedLayer(const QString& busId);
    Q_INVOKABLE void setSelectedLayerGain(double gainDb);
    Q_INVOKABLE void setSelectedLayerMute(bool mute);

    // ---- decks and outputs (the controls panel) ---------------------------------------------
    //
    // Nothing here starts a broadcast: there is no render process yet, and the panel says so.
    // What these edit is the document -- which scene is on program, which is staged on preview,
    // what each output would encode and where it would send it. That is the state the render
    // process will read when it exists, so getting it right now is not a placeholder.

    /// Transitions this build can name, as {id, title}. One of them is a cut, which is the only
    /// one that needs no renderer to be honest about.
    Q_PROPERTY(QVariantList transitions READ transitions CONSTANT)
    [[nodiscard]] QVariantList transitions() const;
    Q_PROPERTY(QVariantList destinationKinds READ destinationKinds CONSTANT)
    [[nodiscard]] QVariantList destinationKinds() const;
    Q_PROPERTY(QVariantList codecs READ codecs CONSTANT)
    [[nodiscard]] QVariantList codecs() const;

    Q_INVOKABLE void setDeckProgram(const QString& deckId, const QString& sceneId);
    Q_INVOKABLE void setDeckPreview(const QString& deckId, const QString& sceneId);
    /// Put what is on preview onto program. The transition itself is the renderer's job; what
    /// this does is move the pointer, which is the part that lives in the document.
    Q_INVOKABLE void takeDeck(const QString& deckId);
    Q_INVOKABLE void setDeckStudioMode(const QString& deckId, bool studio);
    Q_INVOKABLE void setDeckTransition(const QString& deckId, const QString& transition,
                                       int durationMs);

    Q_INVOKABLE void addOutput(const QString& name);
    Q_INVOKABLE void removeOutput(const QString& id);
    Q_INVOKABLE void renameOutput(const QString& id, const QString& name);
    Q_INVOKABLE void setOutputEnabled(const QString& id, bool enabled);
    Q_INVOKABLE void setOutputBitrate(const QString& id, int kbps);
    Q_INVOKABLE void setOutputEncoder(const QString& id, const QString& codec,
                                      const QString& implementation);
    /// The destination's kind and URL. There is deliberately no setter for a credential here:
    /// the project file has no field that can hold one (ADR-0022), only a name for one.
    Q_INVOKABLE void setOutputDestination(const QString& id, const QString& kind,
                                          const QString& url);
    Q_INVOKABLE void setOutputCredentialRef(const QString& id, const QString& ref);

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
    /// How the content fills the node's box, which is a different thing from where the box sits.
    Q_INVOKABLE void setLayerFit(const QString& fit);
    Q_INVOKABLE void setLayerBoxSize(double w, double h);
    Q_INVOKABLE void setLayerCrop(double left, double top, double right, double bottom);
    Q_INVOKABLE void setLayerAlignment(double x, double y);

    Q_PROPERTY(QStringList fitModes READ fitModes CONSTANT)
    [[nodiscard]] QStringList fitModes() const;

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

    /// Read-modify-write helpers. SetBus and SetNodeAudio carry a whole value rather than a
    /// delta, so every edit has to start from the current one; doing it in one place is what
    /// stops a setter that forgot a field from quietly resetting it.
    void edit_bus(const QString& id, const std::function<void(ldoc::Bus&)>& change,
                  std::string_view gesture = {});
    void edit_selected_audio(const std::function<void(ldoc::AudioRouting&)>& change);
    void edit_deck(const QString& id, const std::function<void(ldoc::Deck&)>& change);
    void edit_output(const QString& id, const std::function<void(ldoc::Output&)>& change);

    std::unique_ptr<lsession::Session> session_;
    SceneModel scenes_;
    SceneModel favourites_;
    LayerModel layers_;
    SourceModel sources_;
    BusModel buses_;
    DeckModel decks_;
    OutputModel outputs_;

    QString selected_scene_;
    QString selected_layer_;
    QTimer autosave_;
};

} // namespace libertas::ui
