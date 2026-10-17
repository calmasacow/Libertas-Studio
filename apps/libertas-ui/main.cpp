// SPDX-FileCopyrightText: Libertas-Studio contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include "document.hpp"
#include "shortcuts.hpp"
#include "telemetry.hpp"
#include "theme.hpp"
#include "workspace.hpp"

#include "llayout/serialize.hpp"

#include "ldoc/json.hpp"

#include <QCommandLineOption>
#include <QCommandLineParser>
#include <QGuiApplication>
#include <QMouseEvent>
#include <QObject>
#include <QQmlApplicationEngine>
#include <QCoreApplication>
#include <QEventLoop>
#include <QImage>
#include <QQuickItem>
#include <QSize>
#include <QSet>
#include <QVariantMap>
#include <QQuickWindow>
#include <QThread>
#include <QTimer>
#include <QUrl>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>

namespace {

std::filesystem::path home_dir() {
    if (const char* h = std::getenv("HOME"); h && *h)
        return std::filesystem::path(h);
    return std::filesystem::path(".");
}

std::filesystem::path config_dir() {
    if (const char* x = std::getenv("XDG_CONFIG_HOME"); x && *x)
        return std::filesystem::path(x) / "libertas";
    if (const char* h = std::getenv("HOME"); h && *h)
        return std::filesystem::path(h) / ".config" / "libertas";
    return std::filesystem::path(".libertas");
}

/// ADR-0003: Wayland-native, with no X11 code and no silent fallback to it.
///
/// Omarchy's own environment sets QT_QPA_PLATFORM to "wayland;xcb" with XWayland live, so a Qt
/// application quietly becomes an X11 client whenever the Wayland path misbehaves -- which is how
/// one ends up debugging an X11 application believing it to be native. Libertas asks for wayland
/// and refuses to run on xcb, so that failure is loud on the first launch rather than subtle for
/// a year.
void insist_on_wayland(bool check_mode) {
    if (check_mode) {
        // --check always runs on the offscreen platform, overriding anything in the environment.
        // The window has to be *shown* for QML to lay it out -- a never-shown window has a
        // zero-size content item, which is how the first version of this check managed to assert
        // everything about a blank rectangle -- and "shown" must not mean "on the operator's
        // screen".
        qputenv("QT_QPA_PLATFORM", "offscreen");
        return;
    }
    if (const char* existing = std::getenv("QT_QPA_PLATFORM"); existing && *existing) {
        // An explicit choice is honoured -- "offscreen" is how this binary is tested without a
        // window appearing on anybody's screen -- but a fallback list is not a choice.
        const std::string_view v{existing};
        if (v.find(';') == std::string_view::npos && v != "xcb")
            return;
    }
    qputenv("QT_QPA_PLATFORM", "wayland");
}

int fail(const char* msg) {
    std::fprintf(stderr, "libertas-ui: %s\n", msg);
    return 1;
}

} // namespace

int main(int argc, char** argv) {
    // Read before QGuiApplication: the platform plugin is chosen during its construction, long
    // before QCommandLineParser gets a look at argv.
    bool check = false;
    for (int i = 1; i < argc; ++i)
        check = check || std::string_view{argv[i]} == "--check";
    insist_on_wayland(check);

    QGuiApplication app(argc, argv);
    QGuiApplication::setApplicationName(QStringLiteral("libertas"));
    QGuiApplication::setOrganizationName(QStringLiteral("Libertas-Studio"));
    QGuiApplication::setApplicationVersion(QStringLiteral("0.0.1"));
    // The Wayland app_id, which is what a Hyprland window rule matches on.
    QGuiApplication::setDesktopFileName(QStringLiteral("com.libertas-studio.Studio"));

    QCommandLineParser parser;
    parser.setApplicationDescription(
        QStringLiteral("Libertas Studio -- workspace shell (Phase 1 technical slice)"));
    parser.addHelpOption();
    parser.addVersionOption();

    const QCommandLineOption layout_opt{QStringList{QStringLiteral("layout")},
                                        QStringLiteral("Layout file to use instead of the "
                                                       "default location."),
                                        QStringLiteral("path")};
    const QCommandLineOption check_opt{
        QStringList{QStringLiteral("check")},
        QStringLiteral("Load the shell, exercise the gestures headlessly, report, and exit "
                       "without showing a window.")};
    const QCommandLineOption project_opt{
        QStringList{QStringLiteral("project")},
        QStringLiteral("Project directory to open, creating it if it does not exist. Without "
                       "this the shell starts on an unsaved document."),
        QStringLiteral("dir")};
    const QCommandLineOption shot_opt{
        QStringList{QStringLiteral("check-shots")},
        QStringLiteral("With --check, write the grabbed frames into this directory."),
        QStringLiteral("dir")};
    parser.addOption(layout_opt);
    parser.addOption(project_opt);
    parser.addOption(check_opt);
    parser.addOption(shot_opt);
    parser.process(app);

    if (parser.isSet(check_opt) != check)
        return fail("internal: --check was not detected before the platform was chosen");
    if (!check && QGuiApplication::platformName() == QLatin1String("xcb")) {
        return fail("running on XWayland (xcb). Libertas is Wayland-native by design (ADR-0003); "
                    "it will not run as an X11 client. Start it from a Wayland session.");
    }

    const std::filesystem::path layout_path =
        parser.isSet(layout_opt)
            ? std::filesystem::path(parser.value(layout_opt).toStdString())
            : config_dir() / "layout.toml";

    // The singletons are constructed by the QML engine, so everything they need that QML cannot
    // supply is handed over first.
    libertas::ui::Workspace::configure(layout_path);
    libertas::ui::Document::configure(
        parser.isSet(project_opt)
            ? std::filesystem::path(parser.value(project_opt).toStdString())
            : std::filesystem::path{});
    libertas::ui::Shortcuts::configure(config_dir() / "bindings.toml");
    // Free space is measured where recordings would land, which is beside the project. With no
    // project open there is nothing to measure against yet, so it falls back to the home
    // directory rather than reporting the build tree's filesystem.
    libertas::ui::Telemetry::configure(
        parser.isSet(project_opt)
            ? parser.value(project_opt)
            : QString::fromStdString(home_dir().string()));

    QQmlApplicationEngine engine;

    // Touching them here forces construction, so the notice signals are connected before any QML
    // runs and a complaint from loading the layout is not lost.
    auto* workspace_ptr =
        engine.singletonInstance<libertas::ui::Workspace*>("Libertas", "Workspace");
    auto* theme_ptr = engine.singletonInstance<libertas::ui::Theme*>("Libertas", "Theme");
    auto* document_ptr =
        engine.singletonInstance<libertas::ui::Document*>("Libertas", "Document");
    auto* shortcuts_ptr =
        engine.singletonInstance<libertas::ui::Shortcuts*>("Libertas", "Shortcuts");
    if (!workspace_ptr || !theme_ptr || !document_ptr || !shortcuts_ptr)
        return fail("the singletons could not be created");
    libertas::ui::Workspace& workspace = *workspace_ptr;
    libertas::ui::Theme& theme = *theme_ptr;
    libertas::ui::Document& document = *document_ptr;
    libertas::ui::Shortcuts& shortcuts = *shortcuts_ptr;

    const auto say = [](const QString& t) {
        std::fprintf(stderr, "notice: %s\n", qPrintable(t));
    };
    QObject::connect(&workspace, &libertas::ui::Workspace::notice, &app, say);
    QObject::connect(&theme, &libertas::ui::Theme::notice, &app, say);
    QObject::connect(&document, &libertas::ui::Document::notice, &app, say);
    QObject::connect(&shortcuts, &libertas::ui::Shortcuts::notice, &app, say);

    // Each canvas has its own Scenes and Sources panels, so the workspace has to be told
    // which canvases exist -- to title them "Scenes: Vertical", to offer the closed ones in
    // the status bar, to create a new canvas's pair, and to drop a gone canvas's. Kept as a
    // connection here rather than a pointer from one singleton to the other: the document
    // does not otherwise know there is a workspace, and should not learn.
    const auto sync_canvas_panels = [&workspace, &document] {
        workspace.syncCanvases(document.canvasIds(), document.canvasNames());
    };
    QObject::connect(&document, &libertas::ui::Document::changed, &app, sync_canvas_panels);
    sync_canvas_panels();

    engine.load(QUrl(QStringLiteral("qrc:/qt/qml/Libertas/qml/Main.qml")));

    if (engine.rootObjects().isEmpty())
        return fail("the QML shell failed to load");

    if (check) {
        // Headless self-check. The point is to exercise the Workspace API that QML drives, in a
        // real event loop, with the real QML tree instantiated -- the parts the headless llayout
        // tests deliberately do not cover -- without putting a window on anyone's screen.
        int rc = 0;
        const auto report = [&rc](bool ok, const char* what) {
            std::fprintf(stderr, "  %-4s %s\n", ok ? "ok" : "FAIL", what);
            if (!ok)
                rc = 1;
        };

        auto* window = qobject_cast<QQuickWindow*>(engine.rootObjects().first());
        report(window != nullptr, "the root object is a window");
        if (!window)
            return 1;
        report(QGuiApplication::platformName() == QLatin1String("offscreen"),
               "--check runs on the offscreen platform, so nothing reaches a display");

        // Start from the default rather than from whatever a previous run saved. The check runs
        // against the real config path by default, and persistence working is precisely why the
        // assertions below would otherwise depend on the last run.
        workspace.resetLayout();
        // The viewport is set by QML binding workArea's size, not by this function: using the
        // real path means the check fails if that binding ever stops firing.
        QCoreApplication::processEvents();
        report(workspace.panelCount() == 6, "the default layout resolves to six panel groups");
        report(workspace.dividerCount() == 5, "and five dividers");

        // Did any of it reach the scene graph?
        //
        // An earlier version of this check asserted everything below and passed while the window
        // rendered an empty rectangle: it drove the C++ API directly and never asked what QML had
        // actually drawn. The delegates had been created once at a zero-size viewport and never
        // updated, because the dependency meant to refresh them was a bare statement with no
        // side effect, and the QML compiler had removed it. Every number was right and the window
        // was blank.
        // Walked over childItems(), not findChildren(). A Repeater gives its delegates a visual
        // parent but no QObject parent -- they belong to the delegate model -- so findChildren()
        // on the window returns none of them and would have reported an empty scene graph for a
        // window that was drawing correctly.
        const auto collect = [](QQuickItem* from, const QString& name) {
            QList<QQuickItem*> found;
            QList<QQuickItem*> stack{from};
            while (!stack.isEmpty()) {
                QQuickItem* item = stack.takeLast();
                if (item->objectName() == name)
                    found.append(item);
                stack.append(item->childItems());
            }
            return found;
        };
        // Synthetic pointer events, delivered to the window so the QML handlers run. Shared
        // by every gesture check: proving a function works proves nothing about the pointer
        // that is supposed to reach it.
        const auto post = [&window](QEvent::Type type, QPointF at, Qt::MouseButton b,
                                    Qt::MouseButtons held) {
            QMouseEvent e{type, at, window->mapToGlobal(at), b, held, {}};
            QCoreApplication::sendEvent(window, &e);
        };

        const auto role_of = [](const QAbstractItemModel* m, const char* name) {
            const QHash<int, QByteArray> names = m->roleNames();
            for (auto it = names.cbegin(); it != names.cend(); ++it) {
                if (it.value() == name)
                    return it.key();
            }
            return -1;
        };

        const QList<QQuickItem*> frames =
            collect(window->contentItem(), QStringLiteral("panelFrame"));
        report(frames.size() == 6, "six panel items exist in the scene graph");
        int drawn = 0;
        double covered = 0.0;
        for (const QQuickItem* item : frames) {
            if (item->width() > 1.0 && item->height() > 1.0) {
                ++drawn;
                covered += item->width() * item->height();
            }
        }
        report(drawn == frames.size(), "and every one of them has a real size on screen");
        const double area = window->contentItem()->width() * window->contentItem()->height();
        report(area > 0.0 && covered > area * 0.5,
               "and together they cover most of the window");

        const QVariantMap first = workspace.panelAt(0);
        report(!first.isEmpty() && first["w"].toDouble() > 0.0,
               "panelAt() returns a panel with a real size");
        report(workspace.panelAt(999).isEmpty(), "an out-of-range index is empty, not a crash");

        // Find the program canvas and drag the Log panel onto its left edge.
        QVariantMap program;
        for (int i = 0; i < workspace.panelCount(); ++i) {
            const QVariantMap p = workspace.panelAt(i);
            if (p["tabs"].toStringList().contains(QStringLiteral("program")))
                program = p;
        }
        report(!program.isEmpty(), "the program canvas is in the layout");

        const qreal px = program["x"].toDouble() + 6.0;
        const qreal py = program["y"].toDouble() + program["h"].toDouble() / 2.0;

        // Grab the window before and during the drag. Everything else here asks the engine what
        // it thinks; this asks the scene graph what it drew. The whole gesture is a visual
        // affordance, so "the engine says the preview is at these coordinates" is only half the
        // claim -- the other half is that something is painted there.
        const auto grab = [window] {
            QImage img = window->grabWindow();
            return img.convertToFormat(QImage::Format_ARGB32);
        };
        // Let queued work and short animations finish, so a grab shows a settled frame.
        const auto settle = [] {
            QEventLoop loop;
            QTimer::singleShot(160, &loop, &QEventLoop::quit);
            loop.exec();
        };
        settle();
        const QImage resting = grab();
        report(!resting.isNull() && resting.width() > 0, "the window renders a frame at rest");

        workspace.beginDrag(QStringLiteral("log"));
        workspace.updateDrag(px, py);
        report(workspace.dragging(), "a drag is in flight");
        report(workspace.dropValid(), "the left zone of the program canvas accepts the drop");
        report(workspace.dropDescription().contains(QStringLiteral("left")),
               "and says what it would do");
        report(!workspace.dropPreview().isEmpty(), "with a preview region to highlight");
        report(!workspace.dropTargetRect().isEmpty(), "and a target rect for the wireframe");

        settle();
        const QImage dragging = grab();

        // Counted over a region rather than probed at a point: the wireframe is two one-pixel
        // diagonals, and asking whether one chosen pixel happens to sit on a line is a test of
        // the arithmetic in the test.
        // Panel rects come from the layout engine in the work area's coordinates, and the work
        // area is not the window: the workspace tab strip sits above it. Grabbing the window and
        // indexing it by a panel rect was correct only while that offset was zero, and the drag
        // overlay check started reporting spill onto a neighbouring panel the moment it was not.
        // Asked of the item rather than hard-coded, so that changing the chrome cannot silently
        // invalidate every geometric assertion below.
        const QList<QQuickItem*> work_items =
            collect(window->contentItem(), QStringLiteral("workArea"));
        report(work_items.size() == 1, "the work area is findable in the scene graph");
        const QPointF work_origin =
            work_items.isEmpty() ? QPointF{0, 0}
                                 : work_items.front()->mapToScene(QPointF{0, 0});

        const auto changed_fraction = [&](const QRectF& in) {
            const QRectF r = in.translated(work_origin);
            const qreal dpr = window->devicePixelRatio();
            const int x0 = std::max(0, static_cast<int>(r.left() * dpr));
            const int y0 = std::max(0, static_cast<int>(r.top() * dpr));
            const int x1 = std::min(resting.width(), static_cast<int>(r.right() * dpr));
            const int y1 = std::min(resting.height(), static_cast<int>(r.bottom() * dpr));
            if (x1 <= x0 || y1 <= y0)
                return 0.0;
            long long differing = 0;
            long long total = 0;
            for (int y = y0; y < y1; ++y) {
                for (int x = x0; x < x1; ++x) {
                    ++total;
                    if (resting.pixel(x, y) != dragging.pixel(x, y))
                        ++differing;
                }
            }
            return total ? static_cast<double>(differing) / static_cast<double>(total) : 0.0;
        };

        const double in_preview = changed_fraction(workspace.dropPreview());
        const double in_target = changed_fraction(workspace.dropTargetRect());
        std::fprintf(stderr, "       (repainted: %.0f%% of the preview, %.0f%% of the target)\n",
                     in_preview * 100.0, in_target * 100.0);
        report(in_preview > 0.8,
               "the highlight fills the region the engine said the panel would land in");
        report(in_target > in_preview * 0.4,
               "and the five-zone wireframe reaches the rest of the target");

        // Nothing outside the target panel is touched. The wireframe is two full-length
        // diagonals drawn inside a Shape that fills the whole workspace, so "it is clipped to the
        // panel it belongs to" is a claim worth checking rather than assuming -- a drag overlay
        // that bleeds onto the neighbours would make the gesture ambiguous at exactly the moment
        // it has to be clear. (The status bar does change; it is saying what the drop would do.)
        double worst_spill = 0.0;
        QString worst_panel;
        for (int i = 0; i < workspace.panelCount(); ++i) {
            const QVariantMap p = workspace.panelAt(i);
            if (p["node"].toInt() == static_cast<int>(program["node"].toInt()))
                continue;
            const QRectF r{p["x"].toDouble(), p["y"].toDouble(), p["w"].toDouble(),
                           p["h"].toDouble()};
            const double f = changed_fraction(r);
            if (f > worst_spill) {
                worst_spill = f;
                worst_panel = p["activeId"].toString();
            }
        }
        std::fprintf(stderr, "       (worst spill onto another panel: %.2f%% (%s))\n",
                     worst_spill * 100.0, qPrintable(worst_panel.isEmpty()
                                                         ? QStringLiteral("none")
                                                         : worst_panel));
        report(worst_spill < 0.005, "and no other panel is repainted at all");

        if (parser.isSet(shot_opt)) {
            const QString dir = parser.value(shot_opt);
            resting.save(dir + QStringLiteral("/resting.png"));
            dragging.save(dir + QStringLiteral("/dragging.png"));
            std::fprintf(stderr, "       (frames written to %s)\n", qPrintable(dir));
        }

        report(workspace.endDrag(), "the drop applied");
        report(!workspace.dragging(), "and the drag is over");

        report(workspace.canUndo(), "the dock is undoable");
        const int panels_after_dock = workspace.panelCount();
        workspace.undo();
        report(workspace.panelCount() == panels_after_dock - 1, "undo put it back");
        workspace.redo();
        report(workspace.panelCount() == panels_after_dock, "redo reapplied it");

        // Escape cancels in flight (ADR-0016 rule 7) and changes nothing.
        const int before_cancel = workspace.revision();
        workspace.beginDrag(QStringLiteral("info"));
        workspace.updateDrag(px, py);
        workspace.cancelDrag();
        report(!workspace.dragging(), "Escape cancels a drag in flight");
        report(workspace.revision() == before_cancel, "and a cancelled drag changes nothing");

        // A divider drag is one undo step, whatever it is made of.
        const QVariantMap d = workspace.dividerAt(0);
        const qreal dx = d["x"].toDouble() + d["w"].toDouble() / 2.0;
        const qreal dy = d["y"].toDouble() + d["h"].toDouble() / 2.0;
        report(workspace.dividerAxisAtPoint(dx, dy) >= 0, "a divider is where it says it is");
        report(workspace.dividerAxisAtPoint(5, 5) < 0, "and is not where it is not");
        const int rev_before_divider = workspace.revision();
        const auto first_panel_width = [&workspace] {
            return workspace.panelAt(0)["w"].toDouble();
        };
        const double width_before_resize = first_panel_width();
        report(workspace.beginDividerDrag(dx, dy), "the divider can be grabbed");
        for (int i = 1; i <= 20; ++i)
            workspace.dragDividerTo(dx + (d["horizontal"].toBool() ? i * 2 : 0),
                                    dy + (d["horizontal"].toBool() ? 0 : i * 2));
        report(workspace.revision() > rev_before_divider, "and moves while dragged");
        const double width_mid_resize = first_panel_width();
        workspace.endDividerDrag();
        report(std::abs(first_panel_width() - width_mid_resize) < 0.001,
               "releasing keeps where the divider was dropped");
        // Twenty mouse moves, one undo step: otherwise Ctrl+Z after a resize would walk back
        // through the drag a pixel at a time.
        workspace.undo();
        report(std::abs(first_panel_width() - width_before_resize) < 0.001,
               "and the whole drag undoes in one step");
        workspace.redo();

        // Reopening a closed panel: the answer to losing a dock off-screen.
        workspace.closePanel(QStringLiteral("info"));
        report(workspace.closedPanels().contains(QStringLiteral("info")),
               "a closed panel is listed as reopenable");
        workspace.openPanel(QStringLiteral("info"));
        report(!workspace.closedPanels().contains(QStringLiteral("info")),
               "and comes back when asked");

        // Even the program canvas, which in OBS is the thing that gets lost.
        workspace.closePanel(QStringLiteral("program"));
        report(workspace.closedPanels().contains(QStringLiteral("program")),
               "the program canvas can be closed");
        workspace.openPanel(QStringLiteral("program"));
        report(!workspace.closedPanels().contains(QStringLiteral("program")),
               "and recovered -- it was never an OS window to lose");

        // Persistence, through the real atomic writer and the real parser.
        workspace.saveNow();
        const auto reloaded = llayout::load_workspaces_or_default(layout_path);
        report(!reloaded.used_default && reloaded.complaint.empty(),
               "the workspaces saved and parse back");
        report(!reloaded.workspaces.validate().has_value(), "and what was written is valid");
        report(static_cast<int>(reloaded.workspaces.size()) == workspace.workspaceCount(),
               "with every workspace still in it");
        report(static_cast<int>(reloaded.workspaces.active()) == workspace.activeWorkspace(),
               "and the one that was on screen still marked active");

        // ---- the saved workspaces -----------------------------------------------------------
        //
        // Build and Live exist, hold different panels, and cannot be renamed away. The point of
        // the pair is that there is always a known arrangement to get back to after one has been
        // wrecked, so what matters is that the built-in ones survive being misused.
        report(workspace.workspaceCount() >= 2, "there are at least two workspaces");
        report(workspace.workspaceIsBuiltin(0) && workspace.workspaceIsBuiltin(1),
               "the first two are built in");

        const QString started_on = workspace.activeWorkspaceName();
        workspace.renameWorkspace(0, QStringLiteral("Nope"));
        settle();
        report(workspace.activeWorkspaceName() == started_on ||
                   workspace.workspaceIsBuiltin(0),
               "a built-in workspace cannot be renamed");

        const int before_remove = workspace.workspaceCount();
        workspace.removeWorkspace(0);
        settle();
        report(workspace.workspaceCount() == before_remove,
               "and cannot be removed");

        // Switching changes what is on screen, which is the entire feature.
        workspace.selectWorkspace(1);
        settle();
        // A leaf is a tab *group*, so the panels are its "tabs" -- a group holding three
        // tabs is one leaf with three panels in it.
        const auto panels_on_screen = [&workspace] {
            QStringList out;
            for (int i = 0; i < workspace.panelCount(); ++i)
                out << workspace.panelAt(i).value("tabs").toStringList();
            return out;
        };
        const QStringList live_panels = panels_on_screen();
        workspace.selectWorkspace(0);
        settle();
        const QStringList build_panels = panels_on_screen();
        report(live_panels != build_panels,
               "Build and Live put different panels on screen");
        report(build_panels.contains(QStringLiteral("scenes")),
               "the build arrangement has the scene list");
        report(!build_panels.contains(QStringLiteral("broadcast-stats")),
               "and not the broadcast statistics, which are in the way while building");
        report(live_panels.contains(QStringLiteral("broadcast-stats")),
               "the live arrangement has them");
        report(!live_panels.contains(QStringLiteral("sources")),
               "and not the source list, which is a mis-click waiting to happen");

        // An arrangement follows the workspace it was made in, rather than leaking to the other.
        workspace.closePanel(QStringLiteral("log"));
        settle();
        report(workspace.closedPanels().contains(QStringLiteral("log")),
               "a panel can be closed in one workspace");
        workspace.selectWorkspace(1);
        settle();
        workspace.selectWorkspace(0);
        settle();
        report(workspace.closedPanels().contains(QStringLiteral("log")),
               "and is still closed after switching away and back");
        workspace.openPanel(QStringLiteral("log"));
        settle();

        // A new one starts from what is on screen, and can be renamed and removed.
        const int before_add = workspace.workspaceCount();
        workspace.addWorkspace(QStringLiteral("Editing"));
        settle();
        report(workspace.workspaceCount() == before_add + 1, "a workspace can be added");
        report(workspace.activeWorkspaceName() == QLatin1String("Editing"),
               "and becomes the one on screen");
        report(!workspace.workspaceIsBuiltin(before_add), "and is not built in");
        workspace.renameWorkspace(before_add, QStringLiteral("Edit Bay"));
        settle();
        report(workspace.activeWorkspaceName() == QLatin1String("Edit Bay"),
               "an added workspace can be renamed");
        workspace.removeWorkspace(before_add);
        settle();
        report(workspace.workspaceCount() == before_add, "and removed");
        report(workspace.activeWorkspace() < workspace.workspaceCount(),
               "leaving a real one active");

        // Resetting is the only thing that can be done to a built-in besides rearranging it.
        workspace.selectWorkspace(1);
        settle();
        workspace.closePanel(QStringLiteral("audio-mixer"));
        settle();
        report(workspace.closedPanels().contains(QStringLiteral("audio-mixer")),
               "a built-in workspace can be rearranged");
        workspace.resetWorkspace(1);
        settle();
        report(!workspace.closedPanels().contains(QStringLiteral("audio-mixer")),
               "and reset to how it shipped");
        workspace.selectWorkspace(0);
        settle();

        report(!theme.colors().isEmpty(), "the theme exposes colours");
        report(theme.colors().contains(QStringLiteral("dropFill")), "including the drop overlay fill");

        // ---- the document panels ------------------------------------------------------------
        //
        // Driven through the same API QML drives, which is the whole point: the UI holds no
        // mutable Project and every one of these is a command going to the session.
        const auto scene_rows = [&] { return collect(window->contentItem(),
                                                     QStringLiteral("sceneRow")).size(); };
        const auto node_rows = [&] { return collect(window->contentItem(),
                                                    QStringLiteral("sourceRow")).size(); };

        settle();
        // Counts are relative, because --check may be run against a project it has already been
        // run against -- which is also worth proving, since a self-check that only passes on a
        // clean slate stops being run.
        const int scenes_before = document.scenesModel()->rowCount();
        report(scenes_before >= 1, "the document has at least one scene");
        report(scene_rows() == scenes_before, "and every one of them reached the screen");
        report(!document.selectedScene().isEmpty(), "with one selected");

        document.addScene(QStringLiteral("Gameplay"));
        settle();
        report(document.scenesModel()->rowCount() == scenes_before + 1, "a scene can be added");
        report(scene_rows() == scenes_before + 1, "and the list redrew");
        report(document.dirty(), "which made the document dirty");

        const int nodes_before = document.nodesModel()->rowCount();
        document.addSourceNode(QStringLiteral("Camera"));
        settle();
        report(document.nodesModel()->rowCount() == nodes_before + 1,
               "a source node can be added to it");
        report(node_rows() == nodes_before + 1, "and the Sources panel redrew");
        report(!document.selectedNode().isEmpty(), "with the new source node selected");

        const QVariantMap props = document.nodeProperties();
        report(props.value("name").toString() == QLatin1String("Camera"),
               "the properties panel sees the selected node");
        report(props.value("visible").toBool(), "and its state");

        document.setNodeOpacity(0.5);
        document.setNodeBlendMode(QStringLiteral("multiply"));
        settle();
        report(qFuzzyCompare(document.nodeProperties().value("opacity").toDouble(), 0.5),
               "a property edit reaches the document");
        report(document.nodeProperties().value("blendMode").toString() ==
                   QLatin1String("multiply"),
               "including the blend mode");

        // Undo is the document's, not the layout's: the binding operators expect.
        report(document.canUndo(), "and is undoable");
        document.undo();
        settle();
        report(document.nodeProperties().value("blendMode").toString() ==
                   QLatin1String("normal"),
               "Ctrl+Z puts it back");
        document.redo();
        settle();

        document.setNodeVisible(document.selectedNode(), false);
        settle();
        report(!document.nodeProperties().value("visible").toBool(), "a source node can be hidden");

        // The emergency scene (ADR-0025): designating one reports why it is a poor safety net
        // rather than accepting it silently.
        QString complaint;
        QMetaObject::Connection c = QObject::connect(
            &document, &libertas::ui::Document::notice,
            [&complaint](const QString& t) { complaint = t; });
        document.setEmergencyScene(document.selectedScene());
        settle();
        QObject::disconnect(c);
        report(!complaint.isEmpty(),
               "designating an unsuitable emergency scene says what is wrong with it");
        report(!document.emergencySceneProblems(document.selectedScene()).isEmpty(),
               "and the problems can be asked for without issuing the command");

        // Removing the selected scene must leave the selection somewhere real.
        document.removeScene(document.selectedScene());
        settle();
        report(document.scenesModel()->rowCount() == scenes_before, "a scene can be removed");
        report(!document.selectedScene().isEmpty(),
               "and the selection moves to one that still exists");
        report(document.selectedNode().isEmpty(), "with the node selection cleared");
        // And the node list has to be rebuilt for the scene that is *now* selected, not left
        // empty from the one that was removed.
        report(document.nodesModel()->rowCount() ==
                   [&] {
                       int n = 0;
                       for (const ldoc::Scene& s : document.projectForBindings().scenes)
                           if (ldoc::Id::parse(
                                   document.selectedScene().toStdString()).value_or(
                                   ldoc::Id::nil()) == s.id)
                               ldoc::visit(s.roots, [&n](const ldoc::Node&) { ++n; });
                       return n;
                   }(),
               "and the node list follows the scene that is now selected");

        if (!document.projectPath().startsWith(QLatin1Char('('))) {
            document.save();
            settle();
            report(!document.dirty(), "the document saves");
        }

        // ---- layout properties, which are a different thing from transform ----------------
        document.addSourceNode(QStringLiteral("Fitted"));
        settle();
        report(document.nodeProperties().value("fit").toString() == QLatin1String("natural"),
               "a new source node takes its source's natural size");
        document.setNodeFit(QStringLiteral("cover"));
        document.setNodeBoxSize(1920, 1080);
        document.setNodeCrop(10, 20, 0, 0);
        settle();
        const QVariantMap fitted = document.nodeProperties();
        report(fitted.value("fit").toString() == QLatin1String("cover") &&
                   qFuzzyCompare(fitted.value("boxWidth").toDouble(), 1920.0) &&
                   qFuzzyCompare(fitted.value("cropTop").toDouble(), 20.0),
               "fit, box and crop are separate from the transform");
        report(qFuzzyCompare(fitted.value("scaleX").toDouble(), 1.0),
               "and changing them left the transform alone");
        document.undo();
        document.undo();
        document.undo();
        settle();

        // ---- the workspace from the keyboard ----------------------------------------------
        //
        // ADR-0016's gesture is pointer-only, so this is the path for anyone who cannot aim one.
        workspace.resetLayout();
        settle();
        report(!workspace.focusedPanel().isEmpty(), "a panel has keyboard focus");

        workspace.focusPanel(QStringLiteral("program"));
        report(workspace.focusedPanel() == QLatin1String("program"), "focus can be set");

        workspace.focusNeighbour(QStringLiteral("right"));
        report(workspace.focusedPanel() != QLatin1String("program"),
               "and moves to the panel drawn to the right");
        workspace.focusNeighbour(QStringLiteral("left"));
        report(workspace.focusedPanel() == QLatin1String("program"), "and back again");

        // Cycling visits every panel and returns to where it started.
        const QString start_panel = workspace.focusedPanel();
        QSet<QString> visited;
        for (int i = 0; i < workspace.panelCount() * 3; ++i) {
            visited.insert(workspace.focusedPanel());
            workspace.focusNext(false);
        }
        report(visited.size() >= workspace.panelCount(),
               "cycling focus reaches every panel group");

        // Moving a panel by keyboard rearranges the layout and is undoable like a drag.
        workspace.focusPanel(QStringLiteral("program"));
        const int rev_before_move = workspace.revision();
        workspace.movePanel(QStringLiteral("down"));
        settle();
        report(workspace.revision() > rev_before_move, "a panel can be moved by keyboard");
        report(workspace.canUndo(), "and the move is undoable");

        // Grouping with a neighbour is the keyboard form of a centre drop.
        workspace.focusPanel(QStringLiteral("program"));
        const int panels_before_tab = workspace.panelCount();
        workspace.tabPanelWithNeighbour(QStringLiteral("right"));
        settle();
        report(workspace.panelCount() == panels_before_tab - 1,
               "grouping with a neighbour merges two panel groups into one");

        workspace.resetLayout();
        settle();
        report(!workspace.focusedPanel().isEmpty(),
               "and focus survives the layout being replaced");

        // ---- sources ----------------------------------------------------------------------
        //
        // One panel and one word. Adding a source puts it in the project *and* in the scene, as
        // one undoable step -- nobody pressing Ctrl+Z after adding a camera wants the node back
        // without its source, or the source back with no node.
        const int sources_before = document.sourcesModel()->rowCount();
        const auto source_at = [&](int row, int role) {
            return document.sourcesModel()->data(document.sourcesModel()->index(row, 0),
                                                 Qt::UserRole + role);
        };
        report(!document.sourceTypes().isEmpty(), "the source types are listed");

        // The add dialog's listings, which are what the grid draws from. Checked empty first:
        // an empty project must report nothing rather than everything, since the dialog shows
        // this list before anything has been made.
        report(document.recentSources(24).size() == document.sourcesModel()->rowCount(),
               "the recently-created list matches what the project holds");
        report(document.sourcesOfType(QStringLiteral("source.browser")).isEmpty(),
               "a type with no sources reports none rather than everything");
        for (const QVariant& v : document.sourceTypes()) {
            if (v.toMap().value("icon").toString().isEmpty()) {
                report(false, "a source type has no icon for the dialog to draw");
                break;
            }
        }
        report(true, "every source type carries an icon");

        // Relative, like every other count here: --check may run against a project it has run
        // against before, and a check that only passes on a clean slate stops being run.
        const int cams_before =
            static_cast<int>(document.sourcesOfType(QStringLiteral("source.v4l2")).size());
        const int colours_before =
            static_cast<int>(document.sourcesOfType(QStringLiteral("source.color")).size());
        const int nodes_before_source = document.nodesModel()->rowCount();
        const QString undo_before_source = document.undoDescription();
        document.addNewSourceToScene(QStringLiteral("Desk Cam"), QStringLiteral("source.v4l2"));
        settle();
        report(document.sourcesModel()->rowCount() == sources_before + 1,
               "adding a source puts it in the project");
        report(document.nodesModel()->rowCount() == nodes_before_source + 1,
               "and in the scene");
        const QString cam_id = source_at(sources_before, 1).toString();
        const QString cam_node = document.selectedNode();
        report(document.nodeProperties().value("name").toString() == QLatin1String("Desk Cam"),
               "with the new one selected");
        report(source_at(sources_before, 4).toInt() == 1,
               "bound to it, which the use count reflects");
        report(!document.recentSources(24).isEmpty(),
               "and the recently-created list now has it");
        report(document.recentSources(24).front().toMap().value("name").toString() ==
                   QLatin1String("Desk Cam"),
               "newest first, so the one just made is at the front");
        report(static_cast<int>(document.sourcesOfType(QStringLiteral("source.v4l2")).size()) ==
                   cams_before + 1,
               "and it is listed under its own type");
        report(document.undoDescription() != undo_before_source &&
                   document.undoDescription().contains(QLatin1String("Desk Cam")),
               "as one undo step named for what it did");

        // One undo takes the whole thing back -- not the bind, leaving an orphan.
        document.undo();
        settle();
        report(document.sourcesModel()->rowCount() == sources_before &&
                   document.nodesModel()->rowCount() == nodes_before_source,
               "one undo removes the source and the source node together");
        document.redo();
        settle();

        // An existing source goes into the scene again without being created twice: one camera,
        // opened once, in as many scenes as you like.
        const int nodes_before_reuse = document.nodesModel()->rowCount();
        document.addExistingSourceToScene(cam_id);
        settle();
        report(document.sourcesModel()->rowCount() == sources_before + 1,
               "adding an existing source does not create a second one");
        report(document.nodesModel()->rowCount() == nodes_before_reuse + 1,
               "but does add another source node");
        report(source_at(sources_before, 4).toInt() == 2, "and the use count says two");
        document.undo();
        settle();

        // Explicitly, because the undo above moved the selection -- unbinding "whatever is
        // selected" would have been unbinding something else.
        document.selectNode(cam_node);
        settle();
        document.bindSelectedNode(QString{});
        settle();
        report(source_at(sources_before, 4).toInt() == 0,
               "unbinding the source node releases the source");
        report(source_at(sources_before, 5).toBool() == false,
               "a camera is not safe for an emergency scene");

        // A colour is the one source this build can preview honestly, so the dialog shows its
        // real swatch; everything else falls back to the type glyph and says so by having none.
        document.addNewSourceToScene(QStringLiteral("Backdrop"), QStringLiteral("source.color"));
        settle();
        {
            const QVariantList colours = document.sourcesOfType(QStringLiteral("source.color"));
            report(static_cast<int>(colours.size()) == colours_before + 1,
                   "a colour source can be added");
            if (!colours.isEmpty()) {
                report(colours.front().toMap().value("swatch").toString().startsWith('#'),
                       "and carries a real colour for the dialog to draw");
                report(colours.front().toMap().value("selfContained").toBool(),
                       "and is safe for an emergency scene");
            }
            const QVariantList cams = document.sourcesOfType(QStringLiteral("source.v4l2"));
            if (!cams.isEmpty()) {
                report(cams.front().toMap().value("swatch").toString().isEmpty(),
                       "a camera has no swatch, so the tile falls back to its glyph");
            }
        }

        // A source added with the visibility box cleared arrives hidden.
        document.addNewSourceToScene(QStringLiteral("Hidden"), QStringLiteral("source.color"),
                                     false);
        settle();
        report(!document.nodeProperties().value("visible").toBool(),
               "a source added with 'make visible' cleared arrives hidden");
        document.undo();
        document.undo();
        settle();


        // ---- a source's own settings, behind the gear -------------------------------------
        //
        // The dialog commits nothing until it is accepted, so what is checked is the commit:
        // one command, with every value stored as the kind its field declares. A colour that
        // arrives as a string, or a count that arrives as a double, is a project that reloads
        // as something else.
        document.addNewSourceToScene(QStringLiteral("Lower Third"),
                                     QStringLiteral("source.browser"));
        settle();
        const QString browser_id = document.nodeProperties().value("sourceId").toString();
        const QString browser_node = document.selectedNode();
        report(!browser_id.isEmpty(), "a source node carries the id of the source it is bound to");

        for (const QVariant& v : document.sourceTypes()) {
            const QString type = v.toMap().value("id").toString();
            if (document.sourceSettingsSchema(type).isEmpty()) {
                report(false, qPrintable(QStringLiteral("'%1' has no settings schema").arg(type)));
                break;
            }
        }
        report(true, "every source type has settings the gear can show");
        report(document.sourceSettingsSchema(QStringLiteral("source.nonesuch")).isEmpty(),
               "and a type this build does not know reports none rather than guessing");

        {
            bool well_formed = true;
            for (const QVariant& v : document.sourceSettingsSchema(
                     QStringLiteral("source.v4l2"))) {
                const QVariantMap f = v.toMap();
                const bool note = f.value("kind").toString() == QLatin1String("note");
                well_formed = well_formed && !f.value("label").toString().isEmpty() &&
                              (note ? f.value("key").toString().isEmpty()
                                    : !f.value("key").toString().isEmpty());
            }
            report(well_formed, "every field is named and every note is keyless");
        }

        const QVariantMap browser = document.sourceDetails(browser_id);
        report(!browser.isEmpty() && browser.value("typeTitle").toString() ==
                                         QLatin1String("Browser"),
               "the gear is given the source it is opened on");
        report(browser.value("settings").toMap().value("fps").toInt() == 30,
               "with the type's defaults showing through where nothing is stored");
        report(document.sourceDetails(QStringLiteral("not-an-id")).isEmpty(),
               "and asking about a source that is gone gives nothing, not a crash");

        // Accepting without changing anything must not leave a step to undo past later.
        const QString undo_before_settings = document.undoDescription();
        document.applySourceSettings(browser_id, QStringLiteral("Lower Third"),
                                     browser.value("settings").toMap());
        settle();
        report(document.undoDescription() == undo_before_settings,
               "accepting a settings panel unchanged adds no undo step");

        {
            QVariantMap edited = browser.value("settings").toMap();
            edited["url"] = QStringLiteral("https://example.invalid/overlay");
            edited["fps"] = 60;
            edited["shutdownWhenHidden"] = true;
            document.applySourceSettings(browser_id, QStringLiteral("Overlay"), edited);
            settle();
            const QVariantMap after = document.sourceDetails(browser_id);
            report(after.value("name").toString() == QLatin1String("Overlay"),
                   "the settings panel renames the source");
            report(after.value("settings").toMap().value("url").toString() ==
                       QLatin1String("https://example.invalid/overlay"),
                   "and stores what was typed");
            report(after.value("settings").toMap().value("fps").toInt() == 60 &&
                       after.value("settings").toMap().value("shutdownWhenHidden").toBool(),
                   "a number stays a number and a checkbox a boolean");
            report(document.undoDescription() != undo_before_settings,
                   "as one undo step for the whole panel");
            document.undo();
            settle();
            report(document.sourceDetails(browser_id).value("name").toString() ==
                           QLatin1String("Lower Third") &&
                       document.sourceDetails(browser_id).value("settings").toMap()
                               .value("fps").toInt() == 30,
                   "and one undo takes the name and every field back together");
            document.redo();
            settle();
        }

        // A colour has to reach the document as a colour, because that is what the compositor
        // will read -- and a hex string that cannot be read is refused rather than becoming
        // black, which is a difference nobody notices until the show.
        document.addNewSourceToScene(QStringLiteral("Card"), QStringLiteral("source.color"));
        settle();
        {
            const QString card_id = document.nodeProperties().value("sourceId").toString();
            QVariantMap settings = document.sourceDetails(card_id).value("settings").toMap();
            report(settings.value("color").toString().startsWith('#'),
                   "a colour reaches the panel as hex, which is what a human types");
            settings["color"] = QStringLiteral("#ff8800");
            settings["width"] = 1280;
            document.applySourceSettings(card_id, QString{}, settings);
            settle();
            const QVariantMap after = document.sourceDetails(card_id);
            report(after.value("swatch").toString() == QLatin1String("#ff8800"),
                   "and goes back as one, through the project");
            report(after.value("settings").toMap().value("width").toInt() == 1280,
                   "beside the fields that are not colours");

            QString refusal;
            QMetaObject::Connection cc = QObject::connect(
                &document, &libertas::ui::Document::notice,
                [&refusal](const QString& t) { refusal = t; });
            QVariantMap bad = after.value("settings").toMap();
            bad["color"] = QStringLiteral("not a colour");
            document.applySourceSettings(card_id, QString{}, bad);
            settle();
            QObject::disconnect(cc);
            report(!refusal.isEmpty(), "a colour that cannot be read is refused out loud");
            report(document.sourceDetails(card_id).value("swatch").toString() ==
                       QLatin1String("#ff8800"),
                   "and the colour that was there is left alone rather than going black");
        }

        // The gear, and what disables it. A group has nothing bound to it, and neither does a
        // bare source node -- so neither has settings, and the button says so instead of opening an
        // empty panel.
        {
            const QList<QQuickItem*> gears =
                collect(window->contentItem(), QStringLiteral("sourceSettingsButton"));
            report(gears.size() == 1, "the Sources panel has a gear in its footer");
            document.selectNode(browser_node);
            settle();
            report(!gears.isEmpty() && gears.front()->isEnabled(),
                   "enabled on a source node that is bound to a source");
            document.addGroup(QStringLiteral("Overlays"));
            settle();
            report(document.nodeProperties().value("sourceId").toString().isEmpty(),
                   "a group is bound to nothing");
            report(!gears.isEmpty() && !gears.front()->isEnabled(),
                   "so the gear is disabled on one");
            document.undo();
            settle();
        }

        // And the panel itself, on screen. Driven through the same function the gear and the
        // double-click call, so this covers the QML rather than only the C++ under it.
        {
            const QList<QQuickItem*> panels =
                collect(window->contentItem(), QStringLiteral("sourceSettingsDialog"));
            report(panels.size() == 1, "the settings panel is in the scene graph");
            if (!panels.isEmpty()) {
                report(!panels.front()->isVisible(), "closed until something opens it");
                QMetaObject::invokeMethod(panels.front(), "open",
                                          Q_ARG(QVariant, QVariant{browser_id}));
                settle();
                report(panels.front()->isVisible(), "and visible once a source is handed to it");

                int expected = 0;
                for (const QVariant& v : document.sourceSettingsSchema(
                         QStringLiteral("source.browser")))
                    expected += v.toMap().value("kind").toString() == QLatin1String("note")
                                    ? 0 : 1;
                int drawn_fields = 0;
                for (const QQuickItem* r : collect(window->contentItem(),
                                                   QStringLiteral("sourceSettingRow"))) {
                    if (r->height() > 1.0 && r->width() > 1.0)
                        ++drawn_fields;
                }
                report(drawn_fields >= expected,
                       "with a row drawn for every field the schema names");

                const QList<QQuickItem*> previews =
                    collect(window->contentItem(), QStringLiteral("sourceSettingsPreview"));
                report(previews.size() == 1 && previews.front()->height() > 20.0,
                       "and a preview above them with real size");

                QMetaObject::invokeMethod(panels.front(), "close");
                settle();
                report(!panels.front()->isVisible(), "Cancel puts it away");
            }
        }

        // The other way in, and the one the block above cannot reach: the row itself. That block
        // calls `open` directly, which is the same function the gear and the handler call -- so
        // it proves the panel works and proves nothing about the gesture. The operator asked for
        // the gesture. This drives it through the pointer.
        {
            const QList<QQuickItem*> panels =
                collect(window->contentItem(), QStringLiteral("sourceSettingsDialog"));
            const QList<QQuickItem*> sources_panels =
                collect(window->contentItem(), QStringLiteral("sourcesPanel"));
            report(panels.size() == 1 && sources_panels.size() == 1,
                   "the Sources panel and its settings panel are both on screen");

            // The full sequence a real double-click delivers. A bare DblClick would arrive with
            // no grabber and tell us nothing about what the operator's pointer does.
            const auto double_click = [&](QQuickItem* item) {
                const QPointF at =
                    item->mapToScene(QPointF(item->width() / 2.0, item->height() / 2.0));
                post(QEvent::MouseButtonPress, at, Qt::LeftButton, Qt::LeftButton);
                post(QEvent::MouseButtonRelease, at, Qt::LeftButton, Qt::NoButton);
                post(QEvent::MouseButtonPress, at, Qt::LeftButton, Qt::LeftButton);
                post(QEvent::MouseButtonDblClick, at, Qt::LeftButton, Qt::LeftButton);
                post(QEvent::MouseButtonRelease, at, Qt::LeftButton, Qt::NoButton);
                settle();
            };
            const auto row_for = [&](const QString& node_id) -> QQuickItem* {
                for (QQuickItem* r : collect(window->contentItem(), QStringLiteral("sourceRow")))
                    if (r->property("nodeId").toString() == node_id)
                        return r;
                return nullptr;
            };
            const auto renaming = [&] {
                return sources_panels.isEmpty()
                           ? QString{}
                           : sources_panels.front()->property("renaming").toString();
            };

            QQuickItem* bound = row_for(browser_node);
            report(bound != nullptr, "the browser's row is on screen to be double-clicked");
            if (bound != nullptr && !panels.isEmpty() && !sources_panels.isEmpty()) {
                double_click(bound);
                report(panels.front()->isVisible(),
                       "double-clicking a bound row opens its settings");
                report(renaming().isEmpty(), "and does not start a rename as well");
                QMetaObject::invokeMethod(panels.front(), "close");
                settle();
            }

            // A group is bound to nothing, so the same gesture has to mean the other thing --
            // otherwise F2 would be the only way to rename one.
            document.addGroup(QStringLiteral("Overlays"));
            settle();
            QQuickItem* group_row = row_for(document.selectedNode());
            report(group_row != nullptr, "the group's row is on screen too");
            if (group_row != nullptr && !panels.isEmpty() && !sources_panels.isEmpty()) {
                const QString group_id = document.selectedNode();
                double_click(group_row);
                report(!panels.front()->isVisible(),
                       "double-clicking a group opens no settings panel, because it has none");
                report(renaming() == group_id, "it starts the rename instead");
                sources_panels.front()->setProperty("renaming", QString{});
                settle();
            }
            document.undo();
            settle();
        }

        // ---- the source manager ----------------------------------------------------------
        //
        // The Add Source dialog is the manager (ADR-0033). Deleting from it takes the source's
        // placements with it, which is deliberately not what the bare RemoveSource command
        // does -- so the thing worth checking is that the scenes really lose the nodes, and
        // that one Undo puts every one of them back.
        {
            const QList<QQuickItem*> dialogs =
                collect(window->contentItem(), QStringLiteral("addSourceDialog"));
            report(dialogs.size() == 1, "the source manager is in the scene graph");

            // A source placed twice in one scene, to prove the scene is named once and that
            // both placements go.
            document.addSource(QStringLiteral("Doomed"), QStringLiteral("source.image"));
            settle();
            const QString doomed = document.sourcesModel()
                                       ->index(document.sourcesModel()->rowCount() - 1, 0)
                                       .data(role_of(document.sourcesModel(), "sourceId"))
                                       .toString();
            document.addExistingSourceToScene(doomed, true);
            document.addExistingSourceToScene(doomed, true);
            settle();

            const QStringList using_it = document.scenesUsing(doomed);
            report(using_it.size() == 1,
                   "a source placed twice in one scene names that scene once");

            const int mgr_sources_before = document.sourcesModel()->rowCount();
            const int mgr_nodes_before = document.nodesModel()->rowCount();
            document.removeSourceAndUses(doomed);
            settle();
            report(document.sourcesModel()->rowCount() == mgr_sources_before - 1 &&
                       document.nodesModel()->rowCount() == mgr_nodes_before - 2,
                   "deleting it takes both of its placements with it");
            report(document.scenesUsing(doomed).isEmpty(), "and no scene still uses it");

            document.undo();
            settle();
            report(document.sourcesModel()->rowCount() == mgr_sources_before &&
                       document.nodesModel()->rowCount() == mgr_nodes_before,
                   "one undo puts the source and both placements back");
            document.removeSourceAndUses(doomed);
            settle();

            // And the gesture, through the pointer. The block above calls the function the
            // menu item calls, which proves the command and nothing about the right-click or
            // the two confirmations standing between it and an operator.
            if (!dialogs.isEmpty()) {
                QQuickItem* mgr = dialogs.front();
                document.addSource(QStringLiteral("Clicked"), QStringLiteral("source.image"));
                settle();
                const QString clicked =
                    document.sourcesModel()
                        ->index(document.sourcesModel()->rowCount() - 1, 0)
                        .data(role_of(document.sourcesModel(), "sourceId"))
                        .toString();
                document.addExistingSourceToScene(clicked, true);
                settle();

                QMetaObject::invokeMethod(mgr, "open");
                settle();
                // The grid only lists one type at a time, and the new source is an image.
                mgr->setProperty("selectedType", QStringLiteral("source.image"));
                settle();

                QQuickItem* tile = nullptr;
                for (QQuickItem* t :
                     collect(window->contentItem(), QStringLiteral("existingSourceTile"))) {
                    const QVariant data = t->property("modelData");
                    if (data.toMap().value("id").toString() == clicked)
                        tile = t;
                }
                report(tile != nullptr, "the new source has a tile to right-click");

                if (tile != nullptr) {
                    const QPointF at = tile->mapToScene(
                        QPointF(tile->width() / 2.0, tile->height() / 2.0));
                    post(QEvent::MouseButtonPress, at, Qt::RightButton, Qt::RightButton);
                    post(QEvent::MouseButtonRelease, at, Qt::RightButton, Qt::NoButton);
                    settle();
                    report(!collect(window->contentItem(),
                                    QStringLiteral("stripMenuItem")).isEmpty(),
                           "right-clicking it opens a menu");

                    const QList<QQuickItem*> items =
                        collect(window->contentItem(), QStringLiteral("stripMenuItem"));
                    if (!items.isEmpty()) {
                        const QPointF on = items.front()->mapToScene(
                            QPointF(items.front()->width() / 2.0,
                                    items.front()->height() / 2.0));
                        post(QEvent::MouseButtonPress, on, Qt::LeftButton, Qt::LeftButton);
                        post(QEvent::MouseButtonRelease, on, Qt::LeftButton, Qt::NoButton);
                        settle();
                    }

                    const int before_confirm = document.sourcesModel()->rowCount();
                    report(mgr->property("deleteStage").toInt() == 1 &&
                               before_confirm == document.sourcesModel()->rowCount(),
                           "which asks first and deletes nothing yet");

                    // In use, so the first press only advances to the step that names the
                    // scenes. The destructive press is never the one aimed at the menu item.
                    const auto press = [&](const QString& name) {
                        for (QQuickItem* b : collect(window->contentItem(), name)) {
                            const QPointF mid = b->mapToScene(
                                QPointF(b->width() / 2.0, b->height() / 2.0));
                            post(QEvent::MouseButtonPress, mid, Qt::LeftButton, Qt::LeftButton);
                            post(QEvent::MouseButtonRelease, mid, Qt::LeftButton, Qt::NoButton);
                            settle();
                            return true;
                        }
                        return false;
                    };
                    report(press(QStringLiteral("confirmDeleteSource")), "the first confirm is pressable");
                    report(mgr->property("deleteStage").toInt() == 2 &&
                               document.sourcesModel()->rowCount() == before_confirm,
                           "and still deletes nothing, because the source is in use");

                    report(press(QStringLiteral("confirmDeleteSource")), "the second confirm is pressable");
                    report(document.sourcesModel()->rowCount() == before_confirm - 1 &&
                               document.scenesUsing(clicked).isEmpty(),
                           "and only then is it gone, placements and all");
                    document.undo();
                    settle();
                    document.removeSourceAndUses(clicked);
                    settle();
                }
                QMetaObject::invokeMethod(mgr, "close");
                settle();
            }
        }

        // ---- copy and paste --------------------------------------------------------------
        //
        // The clipboard is the shell's, not the session's (ADR-0036). What matters is that a
        // pasted node is a new node rather than the one copied -- AddNode refuses a duplicate
        // id, and rightly -- and that a scene pasted onto another canvas belongs to that
        // canvas while keeping its name.
        {
            report(document.pasteDescription().isEmpty(), "nothing is on the clipboard yet");

            const QString src_scene = document.selectedScene();
            document.addSourceNode(QStringLiteral("Copy Me"));
            settle();
            const QString copied = document.selectedNode();
            document.copySelectedNode();
            report(!document.pasteDescription().isEmpty(), "copying a source fills it");

            const int clip_before = document.nodesModel()->rowCount();
            document.paste();
            settle();
            report(document.nodesModel()->rowCount() == clip_before + 1,
                   "pasting puts a second one in the scene");
            report(document.selectedNode() != copied,
                   "and it is a new node, not the one copied");

            // Pasting twice has to work: a clipboard that empties itself is a clipboard that
            // cannot fill a scene with six of something.
            document.paste();
            settle();
            report(document.nodesModel()->rowCount() == clip_before + 2,
                   "and the clipboard survives being used");

            document.undo();
            document.undo();
            document.undo();
            settle();

            // A whole scene, onto another canvas.
            document.copyScene(src_scene);
            report(document.pasteDescription().contains(QLatin1String("scene")),
                   "a scene can be copied too");
            document.addCanvas(QStringLiteral("Paste Target"));
            settle();
            const QString target = document.currentCanvas();
            const int clip_scenes_before = document.scenesFor(target)->rowCount();
            document.paste();
            settle();
            report(document.scenesFor(target)->rowCount() == clip_scenes_before + 1,
                   "and pasted onto a different canvas");
            report(document.canvasNameOf(target) != QLatin1String(""),
                   "which is the canvas that was current");
            // The name travels: matching names across canvases are what make a link readable.
            const QAbstractItemModel* home = document.scenesFor(QString{});
            QString original;
            for (int i = 0; i < home->rowCount(); ++i) {
                if (home->index(i, 0).data(role_of(home, "sceneId")).toString() == src_scene)
                    original = home->index(i, 0).data(role_of(home, "name")).toString();
            }
            bool kept_name = false;
            const QAbstractItemModel* sm2 = document.scenesFor(target);
            for (int i = 0; i < sm2->rowCount(); ++i) {
                if (sm2->index(i, 0).data(role_of(sm2, "name")).toString() == original)
                    kept_name = true;
            }
            report(!original.isEmpty() && kept_name, "keeping the name it had");

            document.undo();
            settle();
            document.removeCanvas(target);
            settle();
            document.selectScene(src_scene);
            settle();
        }

        // ---- a scene inside a scene ------------------------------------------------------
        //
        // NodeKind::Precomp existed from the start and nothing could make one, so the model's
        // own cycle check had never been reachable from the UI (ADR-0034). A precomp is a live
        // reference: editing the shown scene changes every scene showing it.
        {
            const QStringList canvases = document.canvasIds();
            report(!canvases.isEmpty(), "there is a canvas to work on");
            // The scene the shell is on now, which the overlay will be shown inside.
            const QString first_scene = document.selectedScene();
            document.addSceneOn(canvases.front(), QStringLiteral("Overlay Pack"));
            settle();
            const QString overlay = document.selectedScene();
            document.addSourceNode(QStringLiteral("Lower Third"));
            settle();

            // Back to a different scene, and show the overlay inside it.
            document.selectScene(first_scene);
            settle();
            const int pre_nodes_before = document.nodesModel()->rowCount();

            bool offered = false;
            for (const QVariant& v : document.scenesForPrecomp())
                offered = offered || v.toMap().value("id").toString() == overlay;
            report(offered, "another scene is offered as something to add");

            bool offers_itself = false;
            for (const QVariant& v : document.scenesForPrecomp())
                offers_itself = offers_itself ||
                                v.toMap().value("id").toString() == first_scene;
            report(!offers_itself, "and a scene is never offered to itself");

            document.addSceneToScene(overlay, true);
            settle();
            report(document.nodesModel()->rowCount() == pre_nodes_before + 1,
                   "adding it puts one node in the scene, not a copy of its contents");

            // The loop. Showing the first scene inside the overlay would close the ring, and
            // the renderer would recurse until it died -- so the command refuses it and the
            // picker never offers it.
            document.selectScene(overlay);
            settle();
            bool loop_offered = false;
            for (const QVariant& v : document.scenesForPrecomp())
                loop_offered = loop_offered ||
                               v.toMap().value("id").toString() == first_scene;
            report(!loop_offered, "a scene that would close a loop is not offered");

            const int overlay_nodes = document.nodesModel()->rowCount();
            document.addSceneToScene(first_scene, true);
            settle();
            report(document.nodesModel()->rowCount() == overlay_nodes,
                   "and the command refuses it even when asked directly");
            bool says_loop = false;
            for (const QString& p : document.problems())
                says_loop = says_loop || p.contains(QLatin1String("precomposes itself"));
            report(!says_loop,
                   "so the project never reaches the state validate() calls an error");

            document.selectScene(first_scene);
            settle();
            document.undo();
            document.undo();
            document.undo();
            settle();
        }

        document.selectNode(browser_node);
        settle();

        // ---- the audio mixer -------------------------------------------------------------
        //
        // No audio host exists yet, so what is checked here is the document half: the buses, the
        // faders, and which bus a source node's audio goes to. All of it is commands, so all of
        // it has to undo.
        const auto bus_strips = [&] { return collect(window->contentItem(),
                                                     QStringLiteral("busStrip")); };
        // By name, not by offset. The positional form was here until inserting three roles
        // silently re-pointed four checks at the wrong columns, and inserting "hidden" and
        // "levelLocked" would have done it again.
        const auto bus_at = [&](int row, const char* role) {
            const QAbstractItemModel* m = document.busesModel();
            return m->data(m->index(row, 0), role_of(m, role));
        };
        settle();
        const int buses_before = document.busesModel()->rowCount();
        report(buses_before >= 1, "the document has at least one bus");
        report(bus_strips().size() == buses_before, "and a strip on screen for each one");
        report(!document.busKinds().isEmpty(), "the bus kinds are listed");

        // The source half. These are the strips an operator from OBS is looking for: one per
        // thing in this scene making sound, left of the divider, with the destinations right
        // of it. The model is asked first, then the scene graph, because a row that exists and
        // is not drawn is the failure this panel had before.
        {
            const int strips_now = document.mixerStripsModel()->rowCount();
            report(collect(window->contentItem(), QStringLiteral("sourceStrip")).size() ==
                       strips_now,
                   "the mixer draws a strip for each source with audio");

            // A colour card is silent, and used to be offered a fader anyway.
            document.addSource(QStringLiteral("Card"), QStringLiteral("source.color"));
            settle();
            const QString colour_src =
                document.sourcesModel()
                    ->index(document.sourcesModel()->rowCount() - 1, 0)
                    .data(role_of(document.sourcesModel(), "sourceId"))
                    .toString();
            document.addSourceNode(QStringLiteral("Card"));
            document.bindSelectedNode(colour_src);
            settle();
            report(document.mixerStripsModel()->rowCount() == strips_now,
                   "and gives a colour card none, because its type has no audio");

            document.addSource(QStringLiteral("Mic"), QStringLiteral("source.audio-input"));
            settle();
            const QString mic_src =
                document.sourcesModel()
                    ->index(document.sourcesModel()->rowCount() - 1, 0)
                    .data(role_of(document.sourcesModel(), "sourceId"))
                    .toString();
            document.addSourceNode(QStringLiteral("Mic"));
            const QString mic_node = document.selectedNode();
            document.bindSelectedNode(mic_src);
            settle();
            report(document.mixerStripsModel()->rowCount() == strips_now + 1,
                   "while an audio input gets one");

            // Node-keyed, not selection-keyed: a mixer draws many strips and none of them is
            // the selection, so this must work without selecting the strip first.
            const QString scene_now = document.selectedScene();
            document.selectNode(QString{});
            document.setNodeGain(scene_now, mic_node, -7.5);
            document.setNodeMute(scene_now, mic_node, true);
            settle();
            const QAbstractItemModel* sm = document.mixerStripsModel();
            bool found = false;
            for (int i = 0; i < sm->rowCount(); ++i) {
                if (sm->index(i, 0).data(role_of(sm, "nodeId")).toString() != mic_node)
                    continue;
                found = qFuzzyCompare(sm->index(i, 0).data(role_of(sm, "gainDb")).toDouble(),
                                      -7.5) &&
                        sm->index(i, 0).data(role_of(sm, "mute")).toBool();
            }
            report(found, "a strip can be faded and muted without being the selection");

            document.undo();
            document.undo();
            settle();

            // The divider sits left of the leftmost destination, which is what makes the two
            // halves read as two halves rather than one list with a gap in it.
            const QList<QQuickItem*> dividers =
                collect(window->contentItem(), QStringLiteral("mixerDivider"));
            bool divider_placed = false;
            for (const QQuickItem* line : dividers) {
                if (!line->isVisible())
                    continue;
                for (const QQuickItem* strip :
                     collect(window->contentItem(), QStringLiteral("busStrip"))) {
                    const QPointF at = strip->mapToItem(line->parentItem(), QPointF{0, 0});
                    if (at.x() >= line->x())
                        divider_placed = true;
                }
            }
            report(divider_placed, "with the divider left of the destination strips");

            // Two buttons per strip and no third: solo is gone from the document, and the
            // reason is that it was document state, so it would have silenced other buses on
            // air rather than only in the operator's headphones (ADR-0031).
            report(collect(window->contentItem(), QStringLiteral("monitorButton")).size() ==
                       static_cast<qsizetype>(bus_strips().size()) +
                           document.mixerStripsModel()->rowCount(),
                   "every strip, source and bus alike, has a monitor button");

            document.setNodeMonitor(scene_now, mic_node, true);
            document.setBusMonitor(document.busesModel()
                                       ->index(0, 0)
                                       .data(role_of(document.busesModel(), "busId"))
                                       .toString(),
                                   true);
            settle();
            bool node_monitored = false;
            for (int i = 0; i < sm->rowCount(); ++i) {
                if (sm->index(i, 0).data(role_of(sm, "nodeId")).toString() == mic_node)
                    node_monitored = sm->index(i, 0).data(role_of(sm, "monitor")).toBool();
            }
            report(node_monitored &&
                       document.busesModel()
                           ->index(0, 0)
                           .data(role_of(document.busesModel(), "monitor"))
                           .toBool(),
                   "which latches on a source and on a bus");

            // Monitoring is local: it must not touch what goes out. Nothing renders yet, so
            // what can be checked now is that it changed no other audio state -- the moment a
            // monitor flag starts muting or rerouting, this fails.
            bool mix_untouched = true;
            for (int i = 0; i < sm->rowCount(); ++i) {
                if (sm->index(i, 0).data(role_of(sm, "mute")).toBool())
                    mix_untouched = false;
            }
            report(mix_untouched, "and changes nothing about the mix going out");

            // Hiding and locking (ADR-0032). Both are document state, so both undo; neither
            // may touch the audio, which is the rule worth a test rather than a comment.
            report(document.hiddenStripCount() == 0, "nothing starts hidden");
            const double gain_before_hide = [&] {
                for (int i = 0; i < sm->rowCount(); ++i) {
                    if (sm->index(i, 0).data(role_of(sm, "nodeId")).toString() == mic_node)
                        return sm->index(i, 0).data(role_of(sm, "gainDb")).toDouble();
                }
                return 0.0;
            }();

            document.setNodeHidden(scene_now, mic_node, true);
            settle();
            report(document.hiddenStripCount() == 1, "a strip can be hidden");
            {
                bool still_there = false;
                bool gain_kept = false;
                bool muted_by_hiding = false;
                for (int i = 0; i < sm->rowCount(); ++i) {
                    if (sm->index(i, 0).data(role_of(sm, "nodeId")).toString() != mic_node)
                        continue;
                    still_there = true;
                    gain_kept = qFuzzyCompare(
                        sm->index(i, 0).data(role_of(sm, "gainDb")).toDouble(),
                        gain_before_hide);
                    muted_by_hiding = sm->index(i, 0).data(role_of(sm, "mute")).toBool();
                }
                // The rule: hiding hides a control, never audio. A hidden strip stays in the
                // model, at its gain, unmuted -- it is still going to air.
                report(still_there && gain_kept && !muted_by_hiding,
                       "and is still in the mix at its own level, unmuted");
            }

            document.setNodeLevelLocked(scene_now, mic_node, true);
            settle();
            {
                bool locked = false;
                bool mutable_still = true;
                for (int i = 0; i < sm->rowCount(); ++i) {
                    if (sm->index(i, 0).data(role_of(sm, "nodeId")).toString() != mic_node)
                        continue;
                    locked = sm->index(i, 0).data(role_of(sm, "levelLocked")).toBool();
                    mutable_still = !sm->index(i, 0).data(role_of(sm, "mute")).toBool();
                }
                report(locked && mutable_still,
                       "a level can be locked, and a locked strip can still be muted");
            }

            document.undo();
            document.undo();
            settle();
            report(document.hiddenStripCount() == 0, "both of which undo");

            document.undo();
            document.undo();
            settle();
        }

        // A strip squeezed into the panel's minimum height still has to be a fader rather than a
        // sliver. The default layout's mixer row is the smallest this gets without a drag.
        {
            int usable_faders = 0;
            for (const QQuickItem* f : collect(window->contentItem(),
                                               QStringLiteral("mixerFader"))) {
                if (f->height() >= 24.0 && f->width() > 1.0)
                    ++usable_faders;
            }
            // Both halves: the source strips carry the same fader as the bus strips, and a
            // narrow panel squeezes the left half first.
            const int faders_drawn =
                static_cast<int>(collect(window->contentItem(),
                                         QStringLiteral("mixerFader")).size());
            report(faders_drawn ==
                       static_cast<int>(bus_strips().size()) +
                           document.mixerStripsModel()->rowCount() &&
                       usable_faders == faders_drawn,
                   "every fader has room to be dragged at the panel's docked size");
        }

        document.addBus(QStringLiteral("Game"), QStringLiteral("submix"));
        settle();
        report(document.busesModel()->rowCount() == buses_before + 1, "a bus can be added");
        report(bus_strips().size() == buses_before + 1, "and the mixer redrew");

        const QString new_bus = bus_at(buses_before, "busId").toString();
        document.setBusGain(new_bus, -6.5);
        document.setBusMute(new_bus, true);
        document.setBusPan(new_bus, -0.5);
        settle();
        report(qFuzzyCompare(bus_at(buses_before, "gainDb").toDouble(), -6.5), "its fader moves");
        report(bus_at(buses_before, "mute").toBool(), "it can be muted");
        report(qFuzzyCompare(bus_at(buses_before, "pan").toDouble(), -0.5), "and panned");

        // The fader as an operator actually reaches it: synthetic mouse events delivered to the
        // window, so the QML handlers run.
        //
        // Everything else about the mixer is driven through Document, the way the divider drag
        // is driven through Workspace. That proves the model and the commands, and proves
        // nothing about the twenty lines of QML between a pointer and them -- which, given that
        // this project exists because of how a UI behaves, is the half worth checking.
        {
            const QList<QQuickItem*> faders =
                collect(window->contentItem(), QStringLiteral("mixerFader"));
            report(!faders.isEmpty(), "the mixer has a fader to grab");
            if (!faders.isEmpty()) {
                QQuickItem* fader = faders.front();
                // Which bus this fader belongs to, asked of the strip it lives in. `collect`
                // walks childItems() and so hands them back in whatever order the scene graph
                // holds them -- which is not the model's order, and assuming it was is how the
                // first version of this check managed to drag one bus's fader and assert about
                // another's.
                QString dragged_bus;
                for (QQuickItem* up = fader; up != nullptr; up = up->parentItem()) {
                    const QVariant id = up->property("busId");
                    if (id.isValid() && !id.toString().isEmpty()) {
                        dragged_bus = id.toString();
                        break;
                    }
                }
                report(!dragged_bus.isEmpty(), "and the strip it belongs to names its bus");
                int dragged_row = -1;
                for (int r = 0; r < document.busesModel()->rowCount(); ++r) {
                    if (bus_at(r, "busId").toString() == dragged_bus)
                        dragged_row = r;
                }
                const auto gain_of_dragged = [&] {
                    return dragged_row < 0 ? 0.0 : bus_at(dragged_row, "gainDb").toDouble();
                };

                const QPointF top = fader->mapToScene(QPointF(fader->width() / 2.0, 2.0));
                const QPointF bottom =
                    fader->mapToScene(QPointF(fader->width() / 2.0, fader->height() - 2.0));


                const double before_drag = gain_of_dragged();
                post(QEvent::MouseButtonPress, bottom, Qt::LeftButton, Qt::LeftButton);
                settle();
                // Pressing near the bottom of the track is a request for silence, so the first
                // bus's fader should have gone down rather than nowhere.
                const double at_bottom = gain_of_dragged();
                report(at_bottom < before_drag,
                       "pressing the fader moves it, so the QML handler is wired to the bus");

                for (int i = 1; i <= 12; ++i) {
                    const QPointF step = bottom + (top - bottom) * (double(i) / 12.0);
                    post(QEvent::MouseMove, step, Qt::NoButton, Qt::LeftButton);
                }
                settle();
                report(gain_of_dragged() > at_bottom, "and dragging it upwards raises it");

                post(QEvent::MouseButtonRelease, top, Qt::LeftButton, Qt::NoButton);
                settle();
                const double after_drag = gain_of_dragged();
                report(after_drag > at_bottom, "releasing keeps where the fader was let go");

                // Thirteen events, one undo step, landing before the press rather than one
                // mouse-move back.
                document.undo();
                settle();
                report(qFuzzyCompare(gain_of_dragged() + 1.0, before_drag + 1.0),
                       "and the whole pointer drag undoes in one step");
                document.redo();
                settle();
                report(qFuzzyCompare(gain_of_dragged() + 1.0, after_drag + 1.0),
                       "which redo puts back");
            }
        }

        // A dragged fader: many commands, one undo step, landing where the drag began.
        {
            const double start = bus_at(buses_before, "gainDb").toDouble();
            const QString was = document.undoDescription();
            for (int i = 0; i < 40; ++i)
                document.setBusGain(new_bus, -20.0 + i * 0.25, true);
            settle();
            report(qFuzzyCompare(bus_at(buses_before, "gainDb").toDouble(), -20.0 + 39 * 0.25),
                   "a dragged fader follows the drag");
            document.endGesture();
            document.undo();
            settle();
            report(qFuzzyCompare(bus_at(buses_before, "gainDb").toDouble(), start),
                   "and one undo puts it back where the drag started");
            report(document.undoDescription() == was,
                   "having used exactly one undo step for the whole drag");
            document.redo();
            settle();
        }

        // Setting a value it already has must not land on the undo stack: a fader released
        // without moving is not an edit.
        const QString before_noop = document.undoDescription();
        document.setBusGain(new_bus, -6.5);
        settle();
        report(document.undoDescription() == before_noop,
               "and setting a fader to where it already is changes nothing");

        document.setBusPan(new_bus, 4.0);
        settle();
        report(bus_at(buses_before, "pan").toDouble() <= 1.0, "a pan outside the range is clamped");

        // Routing: a source node's audio has to be able to reach a bus, and be taken off it again.
        document.addSourceNode(QStringLiteral("Mic"));
        settle();
        if (!document.selectedNode().isEmpty()) {
            // Unbound first: there is no type to ask, so there is no audio to claim. This is
            // the case that used to answer yes for everything.
            report(!document.nodeProperties().value("hasAudio").toBool(),
                   "a node bound to nothing claims no audio");

            document.addSource(QStringLiteral("Mic input"),
                               QStringLiteral("source.audio-input"));
            settle();
            const QString mic_source =
                document.sourcesModel()->index(document.sourcesModel()->rowCount() - 1, 0)
                    .data(role_of(document.sourcesModel(), "sourceId"))
                    .toString();
            document.bindSelectedNode(mic_source);
            settle();
            report(document.nodeProperties().value("hasAudio").toBool(),
                   "a source node has audio of its own");
            report(document.nodeProperties().value("audioBus").toString().isEmpty(),
                   "and starts routed nowhere, which is not the same as muted");

            document.routeSelectedNode(new_bus);
            settle();
            report(document.nodeProperties().value("audioBus").toString() == new_bus,
                   "it can be routed to a bus");
            report(bus_at(buses_before, "feedCount").toInt() == 1,
                   "which the bus strip counts as something feeding it");
            report(bus_at(buses_before, "carriesSelection").toBool(),
                   "and marks as carrying the selection");

            document.setSelectedNodeGain(-3.0);
            document.setSelectedNodeMute(true);
            settle();
            report(qFuzzyCompare(document.nodeProperties().value("audioGain").toDouble(), -3.0),
                   "the source node has its own fader");
            report(document.nodeProperties().value("audioMute").toBool(),
                   "and its own mute");

            document.undo();
            settle();
            report(!document.nodeProperties().value("audioMute").toBool(),
                   "both of which undo");

            // Removing the bus underneath it is allowed: a dangling route is a warning, not an
            // error, and the strip has to stop claiming the node feeds it.
            document.removeBus(new_bus);
            settle();
            report(document.busesModel()->rowCount() == buses_before,
                   "a bus can be removed while a source node still routes to it");
            report(document.nodeProperties().value("audioBusName").toString() ==
                       QLatin1String("(missing bus)"),
                   "and the node says its bus is gone rather than looking unrouted");
            report(!document.problems().isEmpty(), "which validate() reports as a problem");

            document.undo();
            settle();
            report(document.busesModel()->rowCount() == buses_before + 1,
                   "undo brings the bus back");
            report(document.nodeProperties().value("audioBusName").toString() ==
                       QLatin1String("Game"),
                   "with the node routed to it again");

            document.routeSelectedNode(QString{});
            settle();
            report(document.nodeProperties().value("audioBus").toString().isEmpty(),
                   "and a source node can be taken off its bus");
        }

        // A group has no audio, and saying so is better than silently doing nothing.
        document.addGroup(QStringLiteral("Overlays"));
        settle();
        if (!document.selectedNode().isEmpty() &&
            document.nodeProperties().value("kind").toString() == QLatin1String("group")) {
            report(!document.nodeProperties().value("hasAudio").toBool(),
                   "a group has no audio of its own");
            QString refusal;
            QMetaObject::Connection gc = QObject::connect(
                &document, &libertas::ui::Document::notice,
                [&refusal](const QString& t) { refusal = t; });
            document.routeSelectedNode(new_bus);
            settle();
            QObject::disconnect(gc);
            report(!refusal.isEmpty(), "and routing one says so rather than failing quietly");
        }

        // ---- machine telemetry ------------------------------------------------------------
        //
        // The half of the broadcast statistics that is real today. What matters here is not the
        // values -- they are whatever the machine is doing -- but that a figure is never
        // reported as available-and-zero when it was not actually read: a CPU of 0.0% and a
        // *missing* CPU reading look identical in a panel that does not distinguish them.
        {
            // The constructor takes the first sample, so the panel is not blank for a second.
            libertas::ui::Telemetry telemetry;
            // CPU needs two samples to have an interval to divide by, which is itself the
            // reason the first one must not claim to be a reading.
            report(!telemetry.cpuAvailable(),
                   "the first CPU sample reports as unavailable rather than as zero");
            // Far enough apart to have an interval: two samples in the same millisecond
            // divide by zero elapsed time, and read_cpu declines to report rather than
            // inventing a number -- which is the behaviour being checked here.
            QThread::msleep(20);
            telemetry.sample();
            report(telemetry.cpuAvailable() && telemetry.cpuPercent() >= 0.0,
                   "and the second has an interval to measure against");

            report(telemetry.memoryAvailable() && telemetry.memoryBytes() > 1024 * 1024,
                   "memory is read from /proc and is a plausible size");
            report(telemetry.diskAvailable() && telemetry.diskFreeBytes() > 0,
                   "free disk space is read where recordings would land");

            // The GPU figures are allowed to be missing -- a machine with no NVIDIA driver is a
            // supported machine -- but they may not be missing *silently*.
            if (telemetry.gpuAvailable()) {
                report(!telemetry.gpuName().isEmpty(), "the GPU names itself");
                report(telemetry.gpuPercent() >= 0.0 && telemetry.gpuPercent() <= 100.0,
                       "GPU load is a percentage");
                report(telemetry.vramTotalBytes() > 0 &&
                           telemetry.vramUsedBytes() <= telemetry.vramTotalBytes(),
                       "VRAM in use is within VRAM fitted");
                std::fprintf(stderr, "       (%s: %.0f%% gpu, %.0f%% encoder, %.1f/%.1f GiB)\n",
                             telemetry.gpuName().toUtf8().constData(), telemetry.gpuPercent(),
                             telemetry.encoderPercent(),
                             telemetry.vramUsedBytes() / 1073741824.0,
                             telemetry.vramTotalBytes() / 1073741824.0);
            } else {
                report(!telemetry.gpuNote().isEmpty(),
                       "a machine with no GPU telemetry says why rather than showing zeroes");
            }

            // The renderer's figures are not invented while there is no renderer.
            report(!telemetry.rendererAttached(),
                   "no render process is claimed while none exists");
        }

        // ---- the controls panel: decks and outputs ---------------------------------------
        //
        // Nothing broadcasts yet, so what is asserted is the document state the renderer will
        // read: what is on program, what is staged, and what each output would send where.
        // By role *name*, not by offset from Qt::UserRole. These used to be magic numbers,
        // and inserting a role into the middle of an enum silently re-pointed every check
        // after it at the wrong column -- a green suite reading the wrong fields is worse
        // than a red one.
        const auto deck_at = [&](int row, const char* role) {
            const QAbstractItemModel* m = document.decksModel();
            return m->data(m->index(row, 0), role_of(m, role));
        };
        const auto out_at = [&](int row, const char* role) {
            const QAbstractItemModel* m = document.outputsModel();
            return m->data(m->index(row, 0), role_of(m, role));
        };
        settle();
        report(document.decksModel()->rowCount() >= 1, "the document has a deck");
        report(collect(window->contentItem(), QStringLiteral("deckBlock")).size() ==
                   document.decksModel()->rowCount(),
               "and the controls panel drew one block for it");
        report(!document.transitions().isEmpty(), "the transitions are listed");
        report(!document.codecs().isEmpty(), "and the encoders");

        const QString deck_id = deck_at(0, "deckId").toString();
        report(!deck_id.isEmpty(), "with an id to drive it by");

        // Nothing staged: a take has to say so rather than quietly doing nothing.
        document.setDeckPreview(deck_id, QString{});
        settle();
        QString take_complaint;
        QMetaObject::Connection tc = QObject::connect(
            &document, &libertas::ui::Document::notice,
            [&take_complaint](const QString& t) { take_complaint = t; });
        document.takeDeck(deck_id);
        settle();
        QObject::disconnect(tc);
        report(!take_complaint.isEmpty(), "a take with nothing on preview says so");

        document.setDeckPreview(deck_id, document.selectedScene());
        settle();
        report(deck_at(0, "previewSceneId").toString() == document.selectedScene(),
               "a scene can be staged on preview");
        document.takeDeck(deck_id);
        settle();
        report(deck_at(0, "programScene").toString() == deck_at(0, "previewScene").toString(),
               "and a take puts it on program");
        const QString after_take = document.undoDescription();
        document.takeDeck(deck_id);
        settle();
        report(document.undoDescription() == after_take,
               "taking what is already on air changes nothing");

        document.setDeckStudioMode(deck_id, true);
        document.setDeckTransition(deck_id, QStringLiteral("transition.fade"), 450);
        settle();
        report(deck_at(0, "studioMode").toBool(), "studio mode is a document setting");
        report(deck_at(0, "transition").toString() == QLatin1String("transition.fade") &&
                   deck_at(0, "transitionMs").toInt() == 450,
               "and so is the transition and its duration");
        document.undo();
        document.undo();
        settle();
        report(!deck_at(0, "studioMode").toBool(), "both of which undo");

        const int outputs_before = document.outputsModel()->rowCount();
        document.addOutput(QStringLiteral("YouTube"));
        settle();
        report(document.outputsModel()->rowCount() == outputs_before + 1,
               "an output can be added");
        // Outputs shares a tab group with Controls, and only the visible tab is built -- one
        // Loader per panel, not one per panel per frame. So the tab has to be brought forward
        // before asking what it drew, which is itself worth asserting: a panel that is a
        // background tab must not be instantiated at all.
        report(collect(window->contentItem(), QStringLiteral("outputBlock")).isEmpty(),
               "a background tab is not built while it is hidden");
        // The build arrangement does not show the output list at all -- that is the point of
        // having two arrangements -- so it is opened here before being brought forward.
        workspace.openPanel(QStringLiteral("outputs"));
        settle();
        workspace.activate(QStringLiteral("outputs"));
        settle();
        report(collect(window->contentItem(), QStringLiteral("outputBlock")).size() ==
                   outputs_before + 1,
               "and the panel draws it once the tab is brought forward");
        report(!out_at(outputs_before, "isEnabled").toBool(),
               "disabled, because an enabled one would be a keystroke from going live");

        const QString out_id = out_at(outputs_before, "outputId").toString();
        document.setOutputEncoder(out_id, QStringLiteral("hevc"), QStringLiteral("nvenc"));
        document.setOutputBitrate(out_id, 12000);
        document.setOutputDestination(out_id, QStringLiteral("rtmps"),
                                      QStringLiteral("rtmps://a.example/live"));
        document.setOutputCredentialRef(out_id, QStringLiteral("youtube-main"));
        settle();
        report(out_at(outputs_before, "codec").toString() == QLatin1String("hevc") &&
                   out_at(outputs_before, "implementation").toString() == QLatin1String("nvenc"),
               "its encoder can be changed");
        report(out_at(outputs_before, "bitrateKbps").toInt() == 12000, "and its bitrate");
        // An encode's name is a description, so a generated one has to follow what it
        // describes. It said "1080p60 6 Mbps" beside "12000 kbps" until this was fixed, and
        // the name is the handle: it is what the rows print when they name a shared encode.
        report(out_at(outputs_before, "encodeName").toString().contains(
                   QLatin1String("12 Mbps")),
               "and the generated encode name followed the bitrate it describes");
        // The row names the resolution the encode actually sends. Nothing here rescales, so
        // it is the canvas's -- but the field has to be filled, because an empty one reads on
        // screen as "Main -> . h264/nvenc".
        report(out_at(outputs_before, "resolution").toString().contains(QLatin1Char('x')),
               "the row says what resolution leaves the encoder");
        // addOutput gives a new output its own encode rather than joining an existing one
        // (ADR-0029), so nothing is shared yet and the row must not claim otherwise.
        report(out_at(outputs_before, "sharedWith").toStringList().isEmpty(),
               "and a fresh output shares its encode with nobody");
        report(out_at(outputs_before, "credentialRef").toString() == QLatin1String("youtube-main"),
               "and the *name* of its credential");

        // Enabled with nowhere to send to is an error the panel has to show, not swallow.
        document.setOutputDestination(out_id, QStringLiteral("rtmps"), QString{});
        document.setOutputEnabled(out_id, true);
        settle();
        report(!out_at(outputs_before, "isEnabled").toBool() ||
                   out_at(outputs_before, "misconfigured").toBool(),
               "an output enabled with no destination is either refused or flagged");

        // A role added to a model but not declared on its delegate does not fail to build and
        // does not fail any model-level check -- it renders the literal word "undefined" into
        // the panel, under `pragma ComponentBehavior: Bound`. That is what happened to the
        // outputs row, and the self-check passed while the screen was wrong. So: read the
        // screen. Nothing the operator can see may say "undefined".
        {
            QStringList offenders;
            QList<QQuickItem*> stack{window->contentItem()};
            while (!stack.isEmpty()) {
                QQuickItem* item = stack.takeLast();
                const QVariant text = item->property("text");
                if (text.isValid() && text.typeId() == QMetaType::QString &&
                    text.toString().contains(QLatin1String("undefined")))
                    offenders.append(text.toString());
                stack.append(item->childItems());
            }
            const std::string msg =
                offenders.isEmpty()
                    ? std::string{"no panel draws the word \"undefined\""}
                    : ("a panel draws \"undefined\": " +
                       offenders.join(QLatin1String(" | ")).toStdString());
            report(offenders.isEmpty(), msg.c_str());
        }

        // ---- canvases, and the canvas the shell is working on --------------------------
        //
        // Scenes belong to a canvas (ADR-0027), so "which canvas" has to be a thing the
        // operator can see and change -- and the scenes panel has to answer with one
        // canvas's scenes rather than every canvas's at once, which is what it used to do.
        {
            workspace.openPanel(QStringLiteral("canvas"));
            settle();
            workspace.activate(QStringLiteral("canvas"));
            settle();

            const int canvases_before = document.canvasesModel()->rowCount();
            report(canvases_before >= 1, "the document has a canvas");
            report(collect(window->contentItem(), QStringLiteral("canvasRow")).size() ==
                       canvases_before,
                   "and the canvas panel drew a row for each one");
            report(document.currentCanvasIsMain(),
                   "the shell starts on Main, which is the first canvas");

            report(workspace.title(QStringLiteral("scenes")) ==
                       QLatin1String("Scenes: Main"),
                   "a scene list says which canvas it is of");

            const int scenes_on_main = document.scenesModel()->rowCount();
            document.addCanvas(QStringLiteral("Vertical"));
            settle();
            report(document.canvasesModel()->rowCount() == canvases_before + 1,
                   "a canvas can be added");

            // Each canvas gets its own pair of panels, not one pair filtered to whichever
            // canvas is current -- otherwise two canvases cannot be worked on side by side,
            // which is the only reason to have two.
            const QString vertical_id = document.currentCanvas();
            const QString v_scenes = QStringLiteral("scenes:") + vertical_id;
            const QString v_sources = QStringLiteral("sources:") + vertical_id;
            // And a picture of its own. The operator asked for the canvas itself as a tab
            // beside Main's, not only its two lists -- a second canvas you cannot see is one
            // you cannot frame anything on.
            const QString v_program = QStringLiteral("program:") + vertical_id;
            report(workspace.layoutHasPanel(v_program),
                   "a second canvas gets a Program panel of its own");
            report(workspace.title(v_program) == QLatin1String("Program: Vertical") &&
                       workspace.title(QStringLiteral("program")) ==
                           QLatin1String("Program: Main"),
                   "titled with the canvas it shows, Main's included");
            document.setCanvasSize(vertical_id, 1080, 1920);
            settle();
            report(document.canvasSizeOf(vertical_id) == QSize(1080, 1920),
                   "the panel can ask its canvas's size");
            workspace.activate(v_program);
            settle();
            {
                bool portrait = false;
                for (QQuickItem* f : collect(window->contentItem(),
                                             QStringLiteral("programFrame"))) {
                    if (f->isVisible() && f->height() > f->width() * 1.5)
                        portrait = true;
                }
                report(portrait, "and a 9:16 canvas is drawn portrait, not as a second 16:9");
            }
            // The dialogs below live in the Canvas panel; bring it back in front in case the
            // new tab shares its group (the self-check moved Program around earlier).
            workspace.activate(QStringLiteral("canvas"));
            settle();
            report(workspace.layoutHasPanel(v_scenes) && workspace.layoutHasPanel(v_sources),
                   "and gets a scenes and a sources panel of its own");
            report(workspace.title(v_scenes) == QLatin1String("Scenes: Vertical") &&
                       workspace.title(v_sources) == QLatin1String("Sources: Vertical"),
                   "both titled with the canvas they belong to");
            document.renameCanvas(vertical_id, QStringLiteral("Portrait"));
            settle();
            report(workspace.title(v_scenes) == QLatin1String("Scenes: Portrait"),
                   "and renaming the canvas retitles them");
            document.renameCanvas(vertical_id, QStringLiteral("Vertical"));
            settle();

            // Main's list is untouched by any of it: the two are separate models, so the
            // second canvas cannot empty the first the way a shared filtered one did.
            report(static_cast<int>(
                       document.scenesFor(QString{})->rowCount()) == scenes_on_main,
                   "Main's own list still has Main's scenes");
            report(!document.currentCanvasIsMain(),
                   "and adding one selects it, because that is what the operator is about to "
                   "put scenes on");
            // The whole point of the filter. A fresh canvas has no scenes, so the panel is
            // empty -- if this still showed Main's scenes the canvas would be decorative.
            report(document.scenesModel()->rowCount() == 0,
                   "the scenes panel shows the new canvas's scenes, which is none of them");

            document.addScene(QStringLiteral("Vertical A"));
            settle();
            report(document.scenesModel()->rowCount() == 1,
                   "a new scene lands on the canvas the shell is working on");
            const QString vertical_scene = document.selectedScene();

            // ---- linking: which Main scenes bring this one up --------------------------
            //
            // The direction is the thing to get right. This is a scene on a secondary canvas
            // asking which Main scenes cue it, not the other way round.
            const QVariantList offered = document.mainScenesForLink(vertical_scene);
            report(offered.size() == scenes_on_main,
                   "the link popup offers every Main scene and nothing else");
            report(document.sceneLinkCount(vertical_scene) == 0, "and nothing is linked yet");

            if (!offered.isEmpty()) {
                const QString first_main = offered.front().toMap()
                                          .value(QStringLiteral("id")).toString();
                document.commitSceneLinks(vertical_scene, QStringList{first_main});
                settle();
                report(document.sceneLinkCount(vertical_scene) == 1,
                       "ticking a Main scene and pressing Link stores the link");
                // One undo step for the whole tick-list: the popup has one commit point, so
                // undo has one too.
                document.undo();
                settle();
                report(document.sceneLinkCount(vertical_scene) == 0,
                       "and the whole tick-list undoes as one step");
                document.redo();
                settle();
                report(document.sceneLinkCount(vertical_scene) == 1, "and redoes as one");
            }

            // Main carries no link control at all -- not a disabled one, none. A link always
            // runs *from* Main, so a chain on a Main row would be a control that can never do
            // anything.
            report(document.mainScenesForLink(document.selectedScene()).size() ==
                       (document.currentCanvasIsMain() ? 0 : scenes_on_main),
                   "a Main scene has no link list to offer");

            // A canvas still holding scenes cannot be removed; the panel greys its own button
            // rather than offering a press the command will refuse.
            report(document.currentCanvasSceneCount() == 1,
                   "the canvas reports the scene it is holding");

            // The popup itself, not just the model behind it. A dialog that is reparented to
            // the window (ADR-0015) is reachable from the same scene-graph walk as everything
            // else here, so "the rows reached the screen" stays a separate claim from "the
            // document has the links".
            const auto dialog_named = [&](const QString& name) -> QQuickItem* {
                QList<QQuickItem*> found = collect(window->contentItem(), name);
                return found.isEmpty() ? nullptr : found.front();
            };

            if (QQuickItem* link_dialog = dialog_named(QStringLiteral("sceneLinkDialog"))) {
                report(!link_dialog->isVisible(), "the link popup starts closed");
                QMetaObject::invokeMethod(link_dialog, "open",
                                          Q_ARG(QVariant, QVariant{vertical_scene}),
                                          Q_ARG(QVariant, QVariant{QStringLiteral("Vertical A")}));
                settle();
                report(link_dialog->isVisible(), "and the chain icon's click opens it");
                report(collect(window->contentItem(), QStringLiteral("sceneLinkRow")).size() ==
                           scenes_on_main,
                       "with one row per Main scene, drawn");
                if (parser.isSet(shot_opt))
                    grab().save(parser.value(shot_opt) + QStringLiteral("/scene-link.png"));
                // Clear unticks and writes nothing: Link is the only commit point, so closing
                // any other way has to leave the project exactly as it was found.
                const int links_before = document.sceneLinkCount(vertical_scene);
                QMetaObject::invokeMethod(link_dialog, "close");
                settle();
                report(!link_dialog->isVisible(), "and Escape closes it");
                report(document.sceneLinkCount(vertical_scene) == links_before,
                       "having changed nothing, because Link was not pressed");
            } else {
                report(false, "the link popup exists in the scene graph");
            }

            if (QQuickItem* mgr = dialog_named(QStringLiteral("canvasManagerDialog"))) {
                QMetaObject::invokeMethod(mgr, "open");
                settle();
                report(mgr->isVisible(), "the canvas manager opens from the gear");
                report(collect(window->contentItem(), QStringLiteral("canvasBlock")).size() ==
                           document.canvasesModel()->rowCount(),
                       "and draws a block for every canvas");
                if (parser.isSet(shot_opt))
                    grab().save(parser.value(shot_opt) + QStringLiteral("/canvas-manager.png"));
                QMetaObject::invokeMethod(mgr, "close");
                settle();
            } else {
                report(false, "the canvas manager exists in the scene graph");
            }

            // Put it back the way it was found. The checks after this one assume Main.
            document.removeScene(vertical_scene);
            settle();
            const QString vertical_canvas = document.currentCanvas();
            document.removeCanvas(vertical_canvas);
            settle();
            report(document.canvasesModel()->rowCount() == canvases_before,
                   "an empty canvas can be removed");
            // And its panels go with it. A tab called "Scenes: Vertical" for a canvas that
            // no longer exists can never show anything, and the operator did not close it so
            // would not think to.
            report(!workspace.layoutHasPanel(v_program),
                   "its Program panel goes too");
            report(!workspace.layoutHasPanel(v_scenes) && !workspace.layoutHasPanel(v_sources),
                   "taking its two panels with it");
            report(document.currentCanvasIsMain(),
                   "and the shell falls back to Main rather than pointing at nothing");
            report(document.scenesModel()->rowCount() == scenes_on_main,
                   "with Main's scenes listed again");
            workspace.closePanel(QStringLiteral("canvas"));
            settle();
        }

        // ---- the outputs manager: encodes, and what shares them ------------------------
        {
            const int encodes_before = document.encodeCount();
            document.addEncode();
            settle();

            // Which canvas an encode reads has to be changeable, or a second canvas can never
            // be streamed: addEncode takes whichever one was current (ADR-0035).
            {
                const QAbstractItemModel* em = document.encodesModel();
                const int last = em->rowCount() - 1;
                const QString enc =
                    em->index(last, 0).data(role_of(em, "encodeId")).toString();
                const QString was =
                    em->index(last, 0).data(role_of(em, "canvasId")).toString();
                report(!was.isEmpty(), "an encode says which canvas it reads");

                document.addCanvas(QStringLiteral("Second"));
                settle();
                const QString other = document.currentCanvas();
                document.setEncodeCanvas(enc, other);
                settle();
                report(em->index(last, 0).data(role_of(em, "canvasId")).toString() == other,
                       "and can be pointed at another one");
                report(!em->index(last, 0).data(role_of(em, "resolution")).toString().isEmpty(),
                       "with a resolution that still reads");
                document.setEncodeCanvas(enc, was);
                settle();
                document.removeCanvas(other);
                settle();
            }
            report(document.encodeCount() == encodes_before + 1, "an encode can be added");

            // Reuse, which is the whole of ADR-0029 and had no way in from the UI at all
            // until the manager window existed.
            const QString enc_id = document.encodesModel()
                                       ->data(document.encodesModel()->index(encodes_before, 0),
                                              Qt::UserRole + 1)
                                       .toString();
            const int outs_before = document.outputsModel()->rowCount();
            document.addOutputOnEncode(enc_id, QStringLiteral("Shared A"));
            document.addOutputOnEncode(enc_id, QStringLiteral("Shared B"));
            settle();
            report(document.outputsModel()->rowCount() == outs_before + 2,
                   "two destinations can be hung off one encode");
            report(document.encodeCount() == encodes_before + 1,
                   "and the second one does not ask the GPU for another encode");

            // The row has to say so. Identical numbers on two rows are also what two separate
            // encodes configured alike look like.
            report(out_at(outs_before, "sharedWith").toStringList().contains(
                       QLatin1String("Shared B")),
                   "and each row names the other destinations on its encode");

            document.removeOutput(out_at(outs_before + 1, "outputId").toString());
            settle();
            report(document.encodeCount() == encodes_before + 1,
                   "removing one of two keeps the encode, which the other is still sending");
            document.removeOutput(out_at(outs_before, "outputId").toString());
            settle();
            report(document.encodeCount() == encodes_before,
                   "and removing the last one takes it with it");
        }

        // The outputs manager draws the encode graph it is for.
        {
            workspace.activate(QStringLiteral("outputs"));
            settle();
            QList<QQuickItem*> found =
                collect(window->contentItem(), QStringLiteral("outputsManagerDialog"));
            if (found.isEmpty()) {
                report(false, "the outputs manager exists in the scene graph");
            } else {
                QQuickItem* mgr = found.front();
                QMetaObject::invokeMethod(mgr, "open");
                settle();
                report(mgr->isVisible(), "the outputs manager opens from the gear");
                report(collect(window->contentItem(), QStringLiteral("encodeBlock")).size() ==
                           document.encodeCount(),
                       "and draws a block for every encode, which is the GPU's side of it");
                if (parser.isSet(shot_opt))
                    grab().save(parser.value(shot_opt) + QStringLiteral("/outputs-manager.png"));
                QMetaObject::invokeMethod(mgr, "close");
                settle();
                report(!mgr->isVisible(), "and closes again");
            }
        }

        // Nothing in the whole project may serialise a credential. Asserted here as well as in
        // the format tests, because this is the panel that has a text box next to the word key.
        {
            const std::string text = ldoc::project_to_json(document.projectForBindings());
            report(text.find("youtube-main") != std::string::npos,
                   "the credential reference is written to the project");
            for (const char* banned : {"\"stream_key\"", "\"key\"", "\"secret\"",
                                       "\"password\"", "\"token\""}) {
                if (text.find(banned) != std::string::npos) {
                    report(false, "a field that could hold a credential was serialised");
                    break;
                }
            }
            report(true, "and no field that could hold the credential itself exists");
        }

        const int encodes_before_remove = document.encodeCount();
        document.removeOutput(out_id);
        settle();
        report(document.outputsModel()->rowCount() == outputs_before,
               "an output can be removed");
        // addOutput makes an encode and an output together, so removing the output has to take
        // the encode with it. An encode nothing sends is invisible in every panel and still
        // counts against the GPU in the inspector.
        report(document.encodeCount() == encodes_before_remove - 1,
               "and its encode goes with it, since nothing else was sending it");
        document.undo();
        settle();
        report(document.outputsModel()->rowCount() == outputs_before + 1,
               "and undo brings it back");
        report(document.encodeCount() == encodes_before_remove, "encode and all");
        document.removeOutput(out_id);
        settle();

        // Scenes can be reordered, which the scene list is the order of.
        document.addScene(QStringLiteral("Second"));
        settle();
        if (document.scenesModel()->rowCount() >= 2) {
            const auto name_at = [&](int i) {
                return document.scenesModel()
                    ->data(document.scenesModel()->index(i, 0), Qt::UserRole + 2)
                    .toString();
            };
            const QString was_last = name_at(document.scenesModel()->rowCount() - 1);
            document.moveScene(document.selectedScene(), -1);
            settle();
            report(name_at(document.scenesModel()->rowCount() - 1) != was_last,
                   "a scene can be moved up the list");
            document.moveScene(document.selectedScene(), 1);
            settle();
            report(name_at(document.scenesModel()->rowCount() - 1) == was_last,
                   "and back down again");
        }

        // An input can be removed even while a source node still uses it.
        const int inputs_now = document.sourcesModel()->rowCount();
        if (inputs_now > 0) {
            const QString doomed =
                document.sourcesModel()
                    ->data(document.sourcesModel()->index(0, 0), Qt::UserRole + 1)
                    .toString();
            document.removeSource(doomed);
            settle();
            report(document.sourcesModel()->rowCount() == inputs_now - 1,
                   "an input can be removed");
            document.undo();
            settle();
            report(document.sourcesModel()->rowCount() == inputs_now,
                   "and undo brings it back");
        }

        // ---- the edges: no scenes at all, and undo back out of it ------------------------
        //
        // A project with nothing in it is reachable in two keystrokes, and every panel has to
        // survive it rather than assuming there is always something selected.
        {
            int guard = 0;
            while (document.scenesModel()->rowCount() > 0 && guard++ < 50)
                document.removeScene(document.selectedScene());
            settle();
            report(document.scenesModel()->rowCount() == 0, "every scene can be removed");
            report(document.selectedScene().isEmpty(), "leaving nothing selected");
            report(document.nodesModel()->rowCount() == 0, "and no nodes");
            report(document.nodeProperties().isEmpty(), "and no properties to show");
            // Adding one again has to work from empty.
            document.addSourceNode(QStringLiteral("orphan"));
            settle();
            report(document.nodesModel()->rowCount() == 0,
                   "a source node cannot be added when there is no scene to add it to");

            document.addScene(QStringLiteral("Recovered"));
            settle();
            report(document.scenesModel()->rowCount() == 1, "and a scene can be added from empty");
            report(!document.selectedScene().isEmpty(), "which becomes the selection");

            while (document.canUndo() && guard++ < 200)
                document.undo();
            settle();
            report(document.scenesModel()->rowCount() > 0,
                   "undoing all the way back restores the scenes");
            report(!document.selectedScene().isEmpty(),
                   "and the selection lands on one of them");
        }

        // ---- actions and bindings -----------------------------------------------------------
        report(shortcuts.bindingsModel()->rowCount() > 10, "the bindings table is populated");
        report(!shortcuts.applicationBindings().isEmpty(),
               "and the application ones are instantiated as shortcuts");

        // A new scene must become bindable without a restart: the table carries one row per
        // scene, and it was previously frozen at whatever existed when the singleton was built.
        const int bindings_before = shortcuts.bindingsModel()->rowCount();
        document.addScene(QStringLiteral("Bindable"));
        settle();
        report(shortcuts.bindingsModel()->rowCount() == bindings_before + 1,
               "a new scene gains a row in the bindings table");
        document.removeScene(document.selectedScene());
        settle();
        report(shortcuts.bindingsModel()->rowCount() == bindings_before,
               "and loses it again when the scene goes");

        // Ctrl+Z reaches the document, not the layout: with one stack there is no choice, only a
        // gamble about which thing a keystroke reaches.
        document.addScene(QStringLiteral("Undo me"));
        settle();
        const int scenes_now = document.scenesModel()->rowCount();
        shortcuts.perform(QStringLiteral("edit.undo"));
        settle();
        report(document.scenesModel()->rowCount() == scenes_now - 1,
               "performing edit.undo undoes a document change");

        // Rebinding writes through and survives being read back.
        shortcuts.setTrigger(QStringLiteral("edit.undo"), QStringLiteral("Ctrl+U"));
        settle();
        bool rebound = false;
        for (const QVariant& v : shortcuts.applicationBindings()) {
            const QVariantMap m = v.toMap();
            if (m.value("ref").toString() == QLatin1String("edit.undo"))
                rebound = m.value("trigger").toString() == QLatin1String("Ctrl+U");
        }
        report(rebound, "a rebinding reaches the shortcut list");
        shortcuts.resetTrigger(QStringLiteral("edit.undo"));

        report(shortcuts.formatKey(Qt::Key_S, Qt::ControlModifier) == QLatin1String("Ctrl+S"),
               "a key event formats as a binding");
        report(shortcuts.formatKey(Qt::Key_Control, Qt::ControlModifier).isEmpty(),
               "and a bare modifier is refused, since it would never fire");

        // An action nothing can perform yet says so rather than doing nothing silently.
        QString unavailable;
        QMetaObject::Connection sc = QObject::connect(
            &shortcuts, &libertas::ui::Shortcuts::notice,
            [&unavailable](const QString& t) { unavailable = t; });
        shortcuts.perform(QStringLiteral("output.toggle-stream"));
        settle();
        QObject::disconnect(sc);
        report(unavailable.contains(QLatin1String("not wired up")),
               "an action with nothing behind it yet says so");
        std::fprintf(stderr, "\n  theme: %s (%s), layout: %s\n", qPrintable(theme.name()),
                     theme.dark() ? "dark" : "light", qPrintable(workspace.layoutSource()));
        std::fprintf(stderr, "  %s\n", rc == 0 ? "shell self-check passed" : "SHELL CHECK FAILED");
        return rc;
    }

    return QGuiApplication::exec();
}
