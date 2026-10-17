// SPDX-FileCopyrightText: Libertas-Studio contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "documentmodels.hpp"

#include "lsession/local_session.hpp"

#include <QAbstractItemModel>
#include <QObject>
#include <QSize>
#include <QString>
#include <QStringList>
#include <QTimer>
#include <QVariantList>
#include <QVariantMap>
#include <QtQml/qqmlregistration.h>

#include <filesystem>
#include <functional>
#include <map>
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
    Q_PROPERTY(QAbstractItemModel* nodes READ nodesModel CONSTANT)
    Q_PROPERTY(QAbstractItemModel* sources READ sourcesModel CONSTANT)
    Q_PROPERTY(QAbstractItemModel* buses READ busesModel CONSTANT)
    /// The scene list filtered to the starred ones. A second model rather than a filter proxy
    /// in QML: the favourites panel must not be able to change the scene list's own selection
    /// or ordering by being looked at.
    Q_PROPERTY(QAbstractItemModel* favouriteScenes READ favouriteScenesModel CONSTANT)
    Q_PROPERTY(QAbstractItemModel* decks READ decksModel CONSTANT)
    Q_PROPERTY(QAbstractItemModel* outputs READ outputsModel CONSTANT)
    Q_PROPERTY(QAbstractItemModel* canvases READ canvasesModel CONSTANT)
    /// The source strips: one per source node with audio in the scene on screen. Separate from
    /// `buses` because they are different things, not two views of one -- a strip is a place
    /// audio comes from and a bus is where it goes.
    Q_PROPERTY(QAbstractItemModel* mixerStrips READ mixerStripsModel CONSTANT)
    Q_PROPERTY(QAbstractItemModel* encodes READ encodesModel CONSTANT)

    Q_PROPERTY(QString projectName READ projectName NOTIFY changed)
    Q_PROPERTY(QString projectPath READ projectPath NOTIFY changed)
    Q_PROPERTY(bool dirty READ dirty NOTIFY changed)
    Q_PROPERTY(int revision READ revision NOTIFY changed)

    Q_PROPERTY(bool canUndo READ canUndo NOTIFY changed)
    Q_PROPERTY(bool canRedo READ canRedo NOTIFY changed)
    Q_PROPERTY(QString undoDescription READ undoDescription NOTIFY changed)
    Q_PROPERTY(QString redoDescription READ redoDescription NOTIFY changed)

    Q_PROPERTY(QString selectedScene READ selectedScene NOTIFY selectionChanged)
    Q_PROPERTY(QString selectedNode READ selectedNode NOTIFY selectionChanged)
    /// Everything the properties panel shows about the selected node, or empty when nothing is
    /// selected.
    Q_PROPERTY(QVariantMap nodeProperties READ nodeProperties NOTIFY selectionChanged)

    /// The canvas every canvas-shaped panel in the shell is working on: which scenes the
    /// scenes panel lists, where a new scene lands, which canvas the emergency button
    /// designates on. Selection, not document state -- it is not saved with the project.
    Q_PROPERTY(QString currentCanvas READ currentCanvas NOTIFY canvasChanged)
    Q_PROPERTY(QString currentCanvasName READ currentCanvasName NOTIFY canvasChanged)
    /// The current canvas is the first one. Linking is a secondary canvas's function, so the
    /// scenes panel shows no link control at all while this is true (ADR-0027).
    Q_PROPERTY(bool currentCanvasIsMain READ currentCanvasIsMain NOTIFY canvasChanged)
    /// How many scenes belong to the current canvas. A canvas still holding scenes cannot be
    /// removed, so the panel greys its own button rather than offering a refused press.
    Q_PROPERTY(int currentCanvasSceneCount READ currentCanvasSceneCount NOTIFY changed)

    Q_PROPERTY(QStringList problems READ problems NOTIFY changed)
    Q_PROPERTY(QStringList blendModes READ blendModes CONSTANT)

public:
    explicit Document(QObject* parent = nullptr);
    ~Document() override;

    /// Where to open from. Empty means a new, unsaved document. Call before the QML engine
    /// instantiates the singleton.
    static void configure(std::filesystem::path project_dir);
    static Document* create(QQmlEngine*, QJSEngine*);

    /// The current canvas's pair. The `scenes` and `nodes` QML properties and the self-check
    /// go through these; a panel bound to one particular canvas uses scenesFor/nodesFor.
    [[nodiscard]] QAbstractItemModel* scenesModel() { return scenesFor(current_canvas_); }
    [[nodiscard]] QAbstractItemModel* nodesModel() { return nodesFor(current_canvas_); }
    [[nodiscard]] QAbstractItemModel* sourcesModel() { return &sources_; }
    [[nodiscard]] QAbstractItemModel* busesModel() { return &buses_; }
    [[nodiscard]] QAbstractItemModel* favouriteScenesModel() { return &favourites_; }
    [[nodiscard]] QAbstractItemModel* decksModel() { return &decks_; }
    [[nodiscard]] QAbstractItemModel* outputsModel() { return &outputs_; }
    [[nodiscard]] QAbstractItemModel* canvasesModel() { return &canvases_; }
    [[nodiscard]] QAbstractItemModel* mixerStripsModel() { return &mixer_strips_; }

    /// The scene list of one canvas, and the node tree of that canvas's selected scene.
    ///
    /// Each canvas gets its own pair of panels -- "Scenes: Main", "Sources: Vertical" -- so
    /// each needs its own model and its own selected scene. An empty `canvasId` means the
    /// first canvas, which is how the layout's plain "scenes" and "sources" panel ids keep
    /// working: Main is `canvases[0]` and the command layer will not let it be otherwise
    /// (ADR-0027), so it is the one canvas whose panel id can be a constant.
    Q_INVOKABLE [[nodiscard]] QAbstractItemModel* scenesFor(const QString& canvasId);
    Q_INVOKABLE [[nodiscard]] QAbstractItemModel* nodesFor(const QString& canvasId);
    /// Which scene that canvas's panels are showing.
    Q_INVOKABLE [[nodiscard]] QString selectedSceneOn(const QString& canvasId) const;
    /// Select a scene in a particular canvas's list. The canvas the shell is working on
    /// follows: clicking a row in "Scenes: Vertical" is how an operator says "Vertical now".
    Q_INVOKABLE void selectSceneOn(const QString& canvasId, const QString& sceneId);
    /// The name a panel title puts after "Scenes: ". Empty `canvasId` means the first canvas.
    Q_INVOKABLE [[nodiscard]] QString canvasNameOf(const QString& canvasId) const;
    /// That canvas's pixel size, so its Program panel can be drawn the shape it really is.
    /// Empty `canvasId` means the first canvas; an unknown one gives an invalid size.
    Q_INVOKABLE [[nodiscard]] QSize canvasSizeOf(const QString& canvasId) const;
    /// Every canvas id, first one first. Drives the per-canvas panels the workspace keeps.
    Q_INVOKABLE [[nodiscard]] QStringList canvasIds() const;
    /// The same list, by name, in the same order.
    Q_INVOKABLE [[nodiscard]] QStringList canvasNames() const;
    [[nodiscard]] QAbstractItemModel* encodesModel() { return &encodes_; }

    [[nodiscard]] QString projectName() const;
    [[nodiscard]] QString projectPath() const;
    [[nodiscard]] bool dirty() const;
    [[nodiscard]] int revision() const;
    [[nodiscard]] bool canUndo() const;
    [[nodiscard]] bool canRedo() const;
    [[nodiscard]] QString undoDescription() const;
    [[nodiscard]] QString redoDescription() const;
    /// The current canvas's selected scene. Properties, Controls and the emergency
    /// designation all mean this one; a panel bound to a particular canvas asks
    /// selectedSceneOn instead.
    [[nodiscard]] QString selectedScene() const { return selectedSceneOn(current_canvas_); }
    [[nodiscard]] QString selectedNode() const { return selected_node_; }
    [[nodiscard]] QVariantMap nodeProperties() const;
    [[nodiscard]] QStringList problems() const;
    [[nodiscard]] QStringList blendModes() const;
    [[nodiscard]] QString currentCanvas() const { return current_canvas_; }
    [[nodiscard]] QString currentCanvasName() const;
    [[nodiscard]] bool currentCanvasIsMain() const;
    [[nodiscard]] int currentCanvasSceneCount() const;

    // ---- selection (UI state, not document state) -------------------------------------------

    Q_INVOKABLE void selectScene(const QString& id);
    Q_INVOKABLE void selectNode(const QString& id);
    Q_INVOKABLE void selectCanvas(const QString& id);

    // ---- canvases ----------------------------------------------------------------------------

    Q_INVOKABLE void addCanvas(const QString& name);
    Q_INVOKABLE void removeCanvas(const QString& id);
    Q_INVOKABLE void renameCanvas(const QString& id, const QString& name);
    Q_INVOKABLE void setCanvasSize(const QString& id, int width, int height);
    /// The rate as a fraction, because 60000/1001 is a different clock from 60/1 and rounding
    /// it to 59.94 at the model boundary would lose which one the operator meant.
    Q_INVOKABLE void setCanvasFps(const QString& id, int num, int den);

    // ---- scene links -------------------------------------------------------------------------

    /// Every scene on Main, as {id, name, ticked, takenBy}, for the link popup of `sceneId`.
    /// `ticked` is true when that Main scene already brings this scene up; `takenBy` names the
    /// scene it currently brings up on this canvas instead, and is empty when it is free.
    Q_INVOKABLE [[nodiscard]] QVariantList mainScenesForLink(const QString& sceneId) const;
    /// Commit a whole tick-list as one undo step. Links not in the list are removed, links in
    /// it that point elsewhere on this canvas are moved, and the notice names what moved.
    Q_INVOKABLE void commitSceneLinks(const QString& sceneId, const QStringList& mainSceneIds);
    /// How many Main scenes bring `sceneId` up. Drives the chain icon's colour.
    Q_INVOKABLE [[nodiscard]] int sceneLinkCount(const QString& sceneId) const;

    // ---- scenes -----------------------------------------------------------------------------

    Q_INVOKABLE void addScene(const QString& name);
    /// Add a scene to a named canvas. Empty `canvasId` means the first one.
    Q_INVOKABLE void addSceneOn(const QString& canvasId, const QString& name);
    /// Select a node in a particular canvas's tree. The canvas the shell is working on
    /// follows, exactly as it does for a scene.
    Q_INVOKABLE void selectNodeOn(const QString& canvasId, const QString& nodeId);
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

    // ---- sources (project level; a source node binds one) -------------------------------------

    /// The source types this build can hold in a document. None of them render yet; the list is
    /// here because a document that cannot name what it is waiting for is harder to build a
    /// pipeline against than one that can.
    Q_PROPERTY(QVariantList sourceTypes READ sourceTypes CONSTANT)
    [[nodiscard]] QVariantList sourceTypes() const;

    /// Put a new source of this type into the current scene: make the source, make a source
    /// node, bind them. One `Batch`, so it is one undo step -- nobody pressing Ctrl+Z after
    /// adding a camera wants the node back without its source.
    Q_INVOKABLE void addNewSourceToScene(const QString& name, const QString& type,
                                         bool visible = true);
    /// Put a source the project already has into the current scene. Also one step, and
    /// deliberately does not create a second source: one camera, opened once, in six scenes.
    Q_INVOKABLE void addExistingSourceToScene(const QString& sourceId, bool visible = true);

    /// Every source of one type, for the add dialog's right-hand grid. Each entry carries what
    /// the tile needs to draw itself: `id`, `name`, `typeTitle`, `useCount`, `selfContained`,
    /// and `swatch` -- a colour string when the source is one we can honestly preview without a
    /// renderer, empty otherwise, in which case the tile falls back to the type's glyph.
    Q_INVOKABLE [[nodiscard]] QVariantList sourcesOfType(const QString& type) const;
    /// The most recently created sources, newest first, whatever their type. Sources are
    /// appended to the project, so this is the tail reversed -- the real order, not a guess.
    Q_INVOKABLE [[nodiscard]] QVariantList recentSources(int limit) const;

    // ---- a source's own settings, behind the gear in the Sources panel ----------------------
    //
    // A source's settings are document state, not pipeline state: they are what the render
    // process will be told to open when it exists, and they serialise into the project today.
    // So editing them now is real work rather than a mock-up -- the same reasoning that applies
    // to the outputs panel, which configures encoders no encoder reads yet.

    /// What a source of this type is configured by, in the order the dialog should draw it.
    /// Each entry carries `key`, `label`, `kind` and `value` -- the type's default -- plus
    /// `choices` and `suffix` where the kind uses them.
    ///
    /// A `note` entry has no key and no value: it is a line of prose the dialog prints above
    /// the field it is about. They are in the schema rather than looked up separately so that
    /// a note sits where it belongs instead of all of them piling up at the top.
    Q_INVOKABLE [[nodiscard]] QVariantList sourceSettingsSchema(const QString& type) const;
    /// Everything the settings dialog needs: the tile fields plus `settings`, which is the
    /// stored values with the type's defaults showing through wherever a key is absent.
    Q_INVOKABLE [[nodiscard]] QVariantMap sourceDetails(const QString& id) const;
    /// Commit a dialog's worth of edits as one `SetSource`, and so one undo step.
    ///
    /// Values are coerced to the kind the schema declares for each key, which is what stops a
    /// field from changing which alternative of `ParamValue` a setting is stored as merely by
    /// being typed into. Keys the schema does not name are left exactly as they were: a project
    /// written by a later build keeps settings this one cannot draw.
    Q_INVOKABLE void applySourceSettings(const QString& id, const QString& name,
                                         const QVariantMap& values);

    /// Add a source to the *project* only: nothing appears in any scene. Not `addSourceNode`,
    /// which is the other half -- a node in the current scene, bound to nothing -- and not
    /// `addNewSourceToScene`, which is both at once and is what the add dialog uses.
    Q_INVOKABLE void addSource(const QString& name, const QString& type);
    Q_INVOKABLE void removeSource(const QString& id);
    Q_INVOKABLE void renameSource(const QString& id, const QString& name);
    /// Bind the selected source node to this source, or to nothing when the id is empty.
    Q_INVOKABLE void bindSelectedNode(const QString& sourceId);

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

    /// Send the selected source node's audio to this bus, or to nothing when the id is empty.
    /// Unrouted is not the same as muted, and the mixer says which it is.
    Q_INVOKABLE void routeSelectedNode(const QString& busId);
    Q_INVOKABLE void setSelectedNodeGain(double gainDb);
    Q_INVOKABLE void setSelectedNodeMute(bool mute);

    /// The same two edits, keyed by node, for the mixer: it draws many strips and none of them
    /// is the selection, so routing a strip must not require selecting it first.
    Q_INVOKABLE void setNodeGain(const QString& sceneId, const QString& nodeId, double gainDb,
                                 bool dragging = false);
    Q_INVOKABLE void setNodeMute(const QString& sceneId, const QString& nodeId, bool mute);
    Q_INVOKABLE void routeNode(const QString& sceneId, const QString& nodeId,
                               const QString& busId);

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

    /// How many distinct encodes the project asks of the GPU. Not the number of outputs:
    /// destinations sharing one encode cost one (ADR-0029).
    Q_INVOKABLE [[nodiscard]] int encodeCount() const;
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
    /// Point an output at an encode that already exists -- the Aitum case: encode 1080p once,
    /// send it to Twitch and Kick (ADR-0029). The outputs manager window is where this lives.
    Q_INVOKABLE void setOutputEncode(const QString& outputId, const QString& encodeId);
    /// A new destination hung straight off an existing encode, which is the same reuse said
    /// the other way round.
    Q_INVOKABLE void addOutputOnEncode(const QString& encodeId, const QString& name);
    /// A new encode of the current canvas, with no destination on it yet.
    Q_INVOKABLE void addEncode();
    Q_INVOKABLE void renameEncode(const QString& id, const QString& name);
    Q_INVOKABLE void setEncodeRescale(const QString& id, int width, int height);

    // ---- the nodes of the current scene ------------------------------------------------------
    //
    // "Node" rather than "source" in these names because they act on whatever is selected,
    // groups included, and match the `SetNode*` commands underneath. Only `addSourceNode` is
    // about one kind: a source node bound to nothing yet, for `bindSelectedNode` to fill.

    Q_INVOKABLE void addSourceNode(const QString& name);
    Q_INVOKABLE void addGroup(const QString& name);
    Q_INVOKABLE void removeNode(const QString& id);
    Q_INVOKABLE void renameNode(const QString& id, const QString& name);
    Q_INVOKABLE void setNodeVisible(const QString& id, bool visible);
    Q_INVOKABLE void setNodeLocked(const QString& id, bool locked);
    /// Negative moves towards the front of the list, which is towards the top of the stack.
    Q_INVOKABLE void moveNode(const QString& id, int delta);

    // ---- the selected node's properties ------------------------------------------------------

    Q_INVOKABLE void setNodePosition(double x, double y);
    Q_INVOKABLE void setNodeScale(double x, double y);
    Q_INVOKABLE void setNodeRotation(double degrees);
    Q_INVOKABLE void setNodeOpacity(double opacity);
    Q_INVOKABLE void setNodeBlendMode(const QString& mode);
    /// How the content fills the node's box, which is a different thing from where the box sits.
    Q_INVOKABLE void setNodeFit(const QString& fit);
    Q_INVOKABLE void setNodeBoxSize(double w, double h);
    Q_INVOKABLE void setNodeCrop(double left, double top, double right, double bottom);
    Q_INVOKABLE void setNodeAlignment(double x, double y);

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
    /// The shell is now working on a different canvas.
    void canvasChanged();
    /// Something to say in the status bar: a refused command, a failed save, a recovered journal.
    void notice(const QString& text);

private:
    void rebuild();
    void run(const ldoc::Command& command);
    [[nodiscard]] const ldoc::Scene* current_scene() const;
    [[nodiscard]] const ldoc::Scene* scene_of_canvas(const QString& canvasId) const;
    /// `canvasId`, or the first canvas's id when it is empty.
    [[nodiscard]] QString canvas_key(const QString& canvasId) const;
    [[nodiscard]] const ldoc::Node* current_node() const;
    /// The canvas the shell is currently working against. One canvas until Phase 2 gives the UI
    /// a way to choose; taking the first is honest rather than pretending otherwise.
    [[nodiscard]] const ldoc::Canvas* current_canvas() const;

    /// Read-modify-write helpers. SetBus and SetNodeAudio carry a whole value rather than a
    /// delta, so every edit has to start from the current one; doing it in one place is what
    /// stops a setter that forgot a field from quietly resetting it.
    void edit_bus(const QString& id, const std::function<void(ldoc::Bus&)>& change,
                  std::string_view gesture = {});
    void edit_selected_audio(const std::function<void(ldoc::AudioRouting&)>& change);
    /// The node-keyed form the selection one is written in terms of. `gesture` collapses a
    /// fader drag into one undo step, exactly as the bus faders do.
    void edit_node_audio(const QString& sceneId, const QString& nodeId,
                         const std::function<void(ldoc::AudioRouting&)>& change,
                         std::string_view gesture = {});
    /// One entry for the add dialog's grid, built the same way wherever it is listed from.
    [[nodiscard]] QVariantMap source_tile(const ldoc::Source& src) const;

    void edit_deck(const QString& id, const std::function<void(ldoc::Deck&)>& change);
    void edit_output(const QString& id, const std::function<void(ldoc::Output&)>& change);
    /// Edit the encode an output names. Several outputs may name the same one, so this says so
    /// when it happens rather than letting the operator find out mid-stream (ADR-0029).
    void edit_encode_of(const QString& output_id,
                        const std::function<void(ldoc::Encode&)>& change);
    void edit_canvas(const QString& id, const std::function<void(ldoc::Canvas&)>& change);

    std::unique_ptr<lsession::Session> session_;
    /// One per canvas, keyed by id, created on demand and owned by this object. Kept after a
    /// canvas goes rather than deleted: a panel can outlive its canvas by a frame, and an
    /// empty model is a better thing for it to be bound to than a dangling pointer.
    std::map<QString, SceneModel*> scenes_by_canvas_;
    std::map<QString, NodeModel*> nodes_by_canvas_;
    SceneModel favourites_;
    SourceModel sources_;
    BusModel buses_;
    MixerStripModel mixer_strips_;
    DeckModel decks_;
    OutputModel outputs_;
    CanvasModel canvases_;
    EncodeModel encodes_;

    /// Per canvas: each canvas's panels keep their own place in their own scene list.
    std::map<QString, QString> selected_scene_;
    QString selected_node_;
    /// Empty means "the first canvas". Resolved on every read rather than stored as an id, so
    /// that removing the current canvas cannot leave the shell pointing at nothing.
    QString current_canvas_;
    QTimer autosave_;
};

} // namespace libertas::ui
