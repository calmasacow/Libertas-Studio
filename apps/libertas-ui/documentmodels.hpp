// SPDX-FileCopyrightText: Libertas-Studio contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "rowmodel.hpp"

#include <QString>

#include <vector>

namespace libertas::ui {

struct SceneRow {
    QString id;
    QString name;
    int layerCount = 0;
    /// On air on at least one deck.
    bool isProgram = false;
    bool isPreview = false;
    /// The safety net for at least one canvas (ADR-0025). Marked in the list, because losing it
    /// unnoticed is that ADR's own failure arriving by another route.
    bool isEmergency = false;
    /// Marked by the operator; the favourites panel is this filtered.
    bool isFavourite = false;

    friend bool operator==(const SceneRow&, const SceneRow&) = default;
};

class SceneModel : public RowModelBase {
    Q_OBJECT

public:
    enum Role {
        IdRole = Qt::UserRole + 1,
        NameRole,
        LayerCountRole,
        IsProgramRole,
        IsPreviewRole,
        IsEmergencyRole,
        IsFavouriteRole,
    };

    using RowModelBase::RowModelBase;

    [[nodiscard]] int rowCount(const QModelIndex& parent = {}) const override;
    [[nodiscard]] QVariant data(const QModelIndex& index, int role) const override;
    [[nodiscard]] QHash<int, QByteArray> roleNames() const override;

    void set_rows(std::vector<SceneRow> rows) { sync(rows_, std::move(rows)); }
    [[nodiscard]] const std::vector<SceneRow>& rows() const { return rows_; }

private:
    std::vector<SceneRow> rows_;
};

struct LayerRow {
    QString id;
    QString name;
    QString kind;
    /// What the layer is bound to, for the second line of the row. Empty when nothing is.
    QString sourceName;
    /// Depth in the scene tree; a layers panel is a flattened tree with indentation.
    int depth = 0;
    bool visible = true;
    bool locked = false;
    bool isGroup = false;
    int effectCount = 0;

    friend bool operator==(const LayerRow&, const LayerRow&) = default;
};

class LayerModel : public RowModelBase {
    Q_OBJECT

public:
    enum Role {
        IdRole = Qt::UserRole + 1,
        NameRole,
        KindRole,
        SourceNameRole,
        DepthRole,
        VisibleRole,
        LockedRole,
        IsGroupRole,
        EffectCountRole,
    };

    using RowModelBase::RowModelBase;

    [[nodiscard]] int rowCount(const QModelIndex& parent = {}) const override;
    [[nodiscard]] QVariant data(const QModelIndex& index, int role) const override;
    [[nodiscard]] QHash<int, QByteArray> roleNames() const override;

    void set_rows(std::vector<LayerRow> rows) { sync(rows_, std::move(rows)); }
    [[nodiscard]] const std::vector<LayerRow>& rows() const { return rows_; }

private:
    std::vector<LayerRow> rows_;
};

struct SourceRow {
    QString id;
    QString name;
    QString type;
    /// How many layers across the whole project bind to it. Sources are project-level, so this
    /// is the number that says whether removing one is safe.
    int useCount = 0;
    /// Safe to put in an emergency scene (ADR-0025).
    bool selfContained = false;

    friend bool operator==(const SourceRow&, const SourceRow&) = default;
};

class SourceModel : public RowModelBase {
    Q_OBJECT

public:
    enum Role {
        IdRole = Qt::UserRole + 1,
        NameRole,
        TypeRole,
        UseCountRole,
        SelfContainedRole,
    };

    using RowModelBase::RowModelBase;

    [[nodiscard]] int rowCount(const QModelIndex& parent = {}) const override;
    [[nodiscard]] QVariant data(const QModelIndex& index, int role) const override;
    [[nodiscard]] QHash<int, QByteArray> roleNames() const override;

    void set_rows(std::vector<SourceRow> rows) { sync(rows_, std::move(rows)); }
    [[nodiscard]] const std::vector<SourceRow>& rows() const { return rows_; }

private:
    std::vector<SourceRow> rows_;
};

struct BusRow {
    QString id;
    QString name;
    QString kind;
    double gainDb = 0.0;
    double pan = 0.0;
    bool mute = false;
    bool solo = false;
    int channels = 2;
    /// How many layers, across every scene, route here. The mixer shows it because a bus with
    /// nothing feeding it looks identical to a working one until the show starts.
    int feedCount = 0;
    int sendCount = 0;
    /// This bus is where the selected layer's audio goes, so the strip can say so.
    bool carriesSelection = false;

    friend bool operator==(const BusRow&, const BusRow&) = default;
};

class BusModel : public RowModelBase {
    Q_OBJECT

public:
    enum Role {
        IdRole = Qt::UserRole + 1,
        NameRole,
        KindRole,
        GainDbRole,
        PanRole,
        MuteRole,
        SoloRole,
        ChannelsRole,
        FeedCountRole,
        SendCountRole,
        CarriesSelectionRole,
    };

    using RowModelBase::RowModelBase;

    [[nodiscard]] int rowCount(const QModelIndex& parent = {}) const override;
    [[nodiscard]] QVariant data(const QModelIndex& index, int role) const override;
    [[nodiscard]] QHash<int, QByteArray> roleNames() const override;

    void set_rows(std::vector<BusRow> rows) { sync(rows_, std::move(rows)); }
    [[nodiscard]] const std::vector<BusRow>& rows() const { return rows_; }

private:
    std::vector<BusRow> rows_;
};

struct DeckRow {
    QString id;
    QString name;
    QString canvasName;
    /// Names rather than ids, because what the controls panel shows is what is on air.
    QString programScene;
    QString previewScene;
    QString programSceneId;
    QString previewSceneId;
    bool studioMode = false;
    QString transition;
    int transitionMs = 300;
    /// The canvas has a safety net designated (ADR-0025). A deck whose canvas has none is a
    /// pre-flight question, and this is where an operator would see it.
    bool hasEmergencyScene = false;

    friend bool operator==(const DeckRow&, const DeckRow&) = default;
};

class DeckModel : public RowModelBase {
    Q_OBJECT

public:
    enum Role {
        IdRole = Qt::UserRole + 1,
        NameRole,
        CanvasNameRole,
        ProgramSceneRole,
        PreviewSceneRole,
        ProgramSceneIdRole,
        PreviewSceneIdRole,
        StudioModeRole,
        TransitionRole,
        TransitionMsRole,
        HasEmergencySceneRole,
    };

    using RowModelBase::RowModelBase;

    [[nodiscard]] int rowCount(const QModelIndex& parent = {}) const override;
    [[nodiscard]] QVariant data(const QModelIndex& index, int role) const override;
    [[nodiscard]] QHash<int, QByteArray> roleNames() const override;

    void set_rows(std::vector<DeckRow> rows) { sync(rows_, std::move(rows)); }
    [[nodiscard]] const std::vector<DeckRow>& rows() const { return rows_; }

private:
    std::vector<DeckRow> rows_;
};

struct OutputRow {
    QString id;
    QString name;
    QString canvasName;
    QString codec;
    QString implementation;
    int bitrateKbps = 0;
    QString destinationKind;
    QString url;
    /// The *name* of a credential. There is no field anywhere in this program that holds the
    /// credential itself (ADR-0022), and this row is one of the places that would be tempting.
    QString credentialRef;
    bool enabled = false;
    /// Enabled and pointed at nothing, which validate() calls an error.
    bool misconfigured = false;

    friend bool operator==(const OutputRow&, const OutputRow&) = default;
};

class OutputModel : public RowModelBase {
    Q_OBJECT

public:
    enum Role {
        IdRole = Qt::UserRole + 1,
        NameRole,
        CanvasNameRole,
        CodecRole,
        ImplementationRole,
        BitrateRole,
        DestinationKindRole,
        UrlRole,
        CredentialRefRole,
        EnabledRole,
        MisconfiguredRole,
    };

    using RowModelBase::RowModelBase;

    [[nodiscard]] int rowCount(const QModelIndex& parent = {}) const override;
    [[nodiscard]] QVariant data(const QModelIndex& index, int role) const override;
    [[nodiscard]] QHash<int, QByteArray> roleNames() const override;

    void set_rows(std::vector<OutputRow> rows) { sync(rows_, std::move(rows)); }
    [[nodiscard]] const std::vector<OutputRow>& rows() const { return rows_; }

private:
    std::vector<OutputRow> rows_;
};

} // namespace libertas::ui
