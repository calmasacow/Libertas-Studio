// SPDX-FileCopyrightText: Libertas-Studio contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "layoutmodels.hpp"

#include "llayout/history.hpp"
#include "llayout/layout.hpp"

#include <QAbstractItemModel>
#include <QObject>
#include <QPointF>
#include <QtQml/qqmlregistration.h>
#include <QRectF>
#include <QString>
#include <QStringList>
#include <QTimer>
#include <QVariantMap>

#include <filesystem>
#include <optional>

class QJSEngine;
class QQmlEngine;

namespace libertas::ui {

/// The QML-facing face of the layout engine.
///
/// This class holds no layout logic. Every gesture is translated into a call on `llayout` and
/// every answer -- including whether a drop is even legal -- comes back from there, so the
/// behaviour the operator sees is the behaviour the headless tests cover.
///
/// Panels and dividers are exposed as a count plus an indexed accessor, with a `revision` counter
/// that ticks on every change, rather than as a list property. QML's Repeater rebuilds all its
/// delegates when a list model is replaced, which during a divider drag means destroying the item
/// holding the mouse grab sixty times a second. A stable count keeps the delegates alive and lets
/// the revision counter drive their bindings instead.
class Workspace : public QObject {
    Q_OBJECT
    // A QML singleton rather than a context property. Context properties are invisible to
    // qmllint and to the QML compiler, so every binding that touched one was unqualified and
    // unchecked -- a typo in a property name became a silent runtime warning instead of a build
    // failure. In the layer this whole project is betting on, that is the wrong trade.
    QML_ELEMENT
    QML_SINGLETON

    Q_PROPERTY(QAbstractItemModel* panels READ panelsModel CONSTANT)
    Q_PROPERTY(QAbstractItemModel* dividers READ dividersModel CONSTANT)
    Q_PROPERTY(int revision READ revision NOTIFY layoutChanged)
    Q_PROPERTY(int panelCount READ panelCount NOTIFY layoutChanged)
    Q_PROPERTY(int dividerCount READ dividerCount NOTIFY layoutChanged)
    Q_PROPERTY(QStringList closedPanels READ closedPanels NOTIFY layoutChanged)
    Q_PROPERTY(bool canUndo READ canUndo NOTIFY layoutChanged)
    Q_PROPERTY(bool canRedo READ canRedo NOTIFY layoutChanged)
    Q_PROPERTY(QString layoutSource READ layoutSource NOTIFY layoutChanged)

    Q_PROPERTY(bool dragging READ dragging NOTIFY dragChanged)
    Q_PROPERTY(QString dragPanel READ dragPanel NOTIFY dragChanged)
    Q_PROPERTY(QString dragTitle READ dragTitle NOTIFY dragChanged)
    Q_PROPERTY(QPointF dragPos READ dragPos NOTIFY dragChanged)
    /// The region the dragged panel would occupy. Empty when there is no legal drop here.
    Q_PROPERTY(QRectF dropPreview READ dropPreview NOTIFY dragChanged)
    /// The whole panel being hovered, for the five-zone wireframe. Empty for an edge drop.
    Q_PROPERTY(QRectF dropTargetRect READ dropTargetRect NOTIFY dragChanged)
    Q_PROPERTY(QRectF dropCentreRect READ dropCentreRect NOTIFY dragChanged)
    Q_PROPERTY(bool dropValid READ dropValid NOTIFY dragChanged)
    /// Plain words for what releasing here would do, or why it would not work.
    Q_PROPERTY(QString dropDescription READ dropDescription NOTIFY dragChanged)

public:
    explicit Workspace(std::filesystem::path config_path, QObject* parent = nullptr);
    ~Workspace() override;

    /// Configuration the singleton needs at construction, which QML cannot supply. Call before
    /// the engine loads any QML.
    static void configure(std::filesystem::path config_path);
    static Workspace* create(QQmlEngine*, QJSEngine*);

    [[nodiscard]] QAbstractItemModel* panelsModel() { return &panels_; }
    [[nodiscard]] QAbstractItemModel* dividersModel() { return &dividers_; }
    [[nodiscard]] int revision() const { return revision_; }
    [[nodiscard]] int panelCount() const;
    [[nodiscard]] int dividerCount() const;
    [[nodiscard]] QStringList closedPanels() const;
    [[nodiscard]] bool canUndo() const { return history_.can_undo(); }
    [[nodiscard]] bool canRedo() const { return history_.can_redo(); }
    [[nodiscard]] QString layoutSource() const { return layout_source_; }

    [[nodiscard]] bool dragging() const { return drag_.has_value(); }
    [[nodiscard]] QString dragPanel() const;
    [[nodiscard]] QString dragTitle() const;
    [[nodiscard]] QPointF dragPos() const;
    [[nodiscard]] QRectF dropPreview() const;
    [[nodiscard]] QRectF dropTargetRect() const;
    [[nodiscard]] QRectF dropCentreRect() const;
    [[nodiscard]] bool dropValid() const;
    [[nodiscard]] QString dropDescription() const;

    /// {node, x, y, w, h, tabs, titles, active}
    Q_INVOKABLE [[nodiscard]] QVariantMap panelAt(int index) const;
    /// {split, index, horizontal, x, y, w, h}
    Q_INVOKABLE [[nodiscard]] QVariantMap dividerAt(int index) const;

    Q_INVOKABLE void setViewport(qreal w, qreal h);

    // --- panel drag (ADR-0016 rules 1, 2, 3, 4, 7) ---
    Q_INVOKABLE void beginDrag(const QString& panel);
    Q_INVOKABLE void updateDrag(qreal x, qreal y);
    /// Returns true if the drop changed the layout.
    Q_INVOKABLE bool endDrag();
    Q_INVOKABLE void cancelDrag();

    // --- dividers ---
    /// -1 when there is no divider under the point, else 0 for a vertical divider (drag it
    /// sideways) and 1 for a horizontal one.
    Q_INVOKABLE [[nodiscard]] int dividerAxisAtPoint(qreal x, qreal y) const;
    /// Index into dividerAt(), or -1. For the hover highlight, which cannot use a
    /// MouseArea of its own without stealing the grab the resize drag needs.
    Q_INVOKABLE [[nodiscard]] int dividerIndexAtPoint(qreal x, qreal y) const;
    Q_INVOKABLE bool beginDividerDrag(qreal x, qreal y);
    Q_INVOKABLE void dragDividerTo(qreal x, qreal y);
    Q_INVOKABLE void endDividerDrag();

    // --- panels ---
    Q_INVOKABLE void activate(const QString& panel);
    Q_INVOKABLE void closePanel(const QString& panel);
    Q_INVOKABLE void openPanel(const QString& panel);
    Q_INVOKABLE [[nodiscard]] QString title(const QString& panel) const;

    // --- history and persistence ---
    Q_INVOKABLE void undo();
    Q_INVOKABLE void redo();
    Q_INVOKABLE void resetLayout();
    Q_INVOKABLE void saveNow();

signals:
    void layoutChanged();
    void dragChanged();
    /// Something the status line should say: a refused drop, a failed save, a fallback on load.
    void notice(const QString& text);

private:
    struct Drag {
        QString panel;
        QPointF pos;
        std::optional<llayout::DropTarget> target;
        QString refusal;
    };

    [[nodiscard]] const llayout::Layout& layout() const;
    void touch();
    /// Rebuild the QML-facing rows from the current layout and viewport.
    void refresh_models();
    void scheduleSave();
    void recompute_drag_target();

    PanelModel panels_;
    DividerModel dividers_;

    llayout::History history_;
    /// Held only while a divider is being dragged, so the whole drag is one undo step.
    std::optional<llayout::Layout> working_;

    llayout::Viewport vp_{};
    llayout::Constraints constraints_ = llayout::Constraints::studio_defaults();

    std::optional<Drag> drag_;
    struct DividerDrag {
        llayout::NodeId split = llayout::kInvalidNode;
        std::size_t index = 0;
        llayout::Axis axis = llayout::Axis::Horizontal;
        QPointF last;
        llayout::Layout before;
    };
    std::optional<DividerDrag> divider_drag_;

    std::filesystem::path config_path_;
    QString layout_source_;
    QTimer save_timer_;
    int revision_ = 0;
};

} // namespace libertas::ui
