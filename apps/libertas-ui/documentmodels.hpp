// SPDX-FileCopyrightText: Libertas-Studio contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QAbstractListModel>
#include <QString>

#include <vector>

namespace libertas::ui {

/// Rows for the document panels.
///
/// Same shape as the layout models, and for the same reason: reset only when the row count
/// changes, and otherwise emit dataChanged for the rows that actually moved, so a delegate
/// holding a text cursor or a hover state survives its neighbour being renamed.
///
/// Rows hold plain strings rather than ULIDs, because QML has no ULID type and because a model
/// that cannot be built without a document is a model that cannot be tested without one.
///
/// The shared part is a member template on a *non-template* base. moc takes the first base
/// class's staticMetaObject, so a templated base would leave it referring to one that does not
/// exist.
class RowModelBase : public QAbstractListModel {
    Q_OBJECT

public:
    using QAbstractListModel::QAbstractListModel;

protected:
    template <typename Row> void sync(std::vector<Row>& rows, std::vector<Row> next) {
        if (rows.size() != next.size()) {
            beginResetModel();
            rows = std::move(next);
            endResetModel();
            return;
        }
        for (std::size_t i = 0; i < rows.size(); ++i) {
            if (rows[i] == next[i])
                continue;
            rows[i] = next[i];
            const QModelIndex idx = index(static_cast<int>(i), 0);
            emit dataChanged(idx, idx);
        }
    }

    template <typename Row>
    [[nodiscard]] static const Row* at(const std::vector<Row>& rows, const QModelIndex& idx) {
        if (!idx.isValid() || idx.row() < 0 ||
            static_cast<std::size_t>(idx.row()) >= rows.size())
            return nullptr;
        return &rows[static_cast<std::size_t>(idx.row())];
    }
};

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

} // namespace libertas::ui
