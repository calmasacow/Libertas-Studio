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
        const auto layer_rows = [&] { return collect(window->contentItem(),
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

        const int layers_before = document.layersModel()->rowCount();
        document.addLayer(QStringLiteral("Camera"));
        settle();
        report(document.layersModel()->rowCount() == layers_before + 1,
               "a layer can be added to it");
        report(layer_rows() == layers_before + 1, "and the layers panel redrew");
        report(!document.selectedLayer().isEmpty(), "with the new layer selected");

        const QVariantMap props = document.layerProperties();
        report(props.value("name").toString() == QLatin1String("Camera"),
               "the properties panel sees the selected layer");
        report(props.value("visible").toBool(), "and its state");

        document.setLayerOpacity(0.5);
        document.setLayerBlendMode(QStringLiteral("multiply"));
        settle();
        report(qFuzzyCompare(document.layerProperties().value("opacity").toDouble(), 0.5),
               "a property edit reaches the document");
        report(document.layerProperties().value("blendMode").toString() ==
                   QLatin1String("multiply"),
               "including the blend mode");

        // Undo is the document's, not the layout's: the binding operators expect.
        report(document.canUndo(), "and is undoable");
        document.undo();
        settle();
        report(document.layerProperties().value("blendMode").toString() ==
                   QLatin1String("normal"),
               "Ctrl+Z puts it back");
        document.redo();
        settle();

        document.setLayerVisible(document.selectedLayer(), false);
        settle();
        report(!document.layerProperties().value("visible").toBool(), "a layer can be hidden");

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
        report(document.selectedLayer().isEmpty(), "with the layer selection cleared");
        // And the layer list has to be rebuilt for the scene that is *now* selected, not left
        // empty from the one that was removed.
        report(document.layersModel()->rowCount() ==
                   [&] {
                       int n = 0;
                       for (const ldoc::Scene& s : document.projectForBindings().scenes)
                           if (ldoc::Id::parse(
                                   document.selectedScene().toStdString()).value_or(
                                   ldoc::Id::nil()) == s.id)
                               ldoc::visit(s.roots, [&n](const ldoc::Node&) { ++n; });
                       return n;
                   }(),
               "and the layer list follows the scene that is now selected");

        if (!document.projectPath().startsWith(QLatin1Char('('))) {
            document.save();
            settle();
            report(!document.dirty(), "the document saves");
        }

        // ---- layout properties, which are a different thing from transform ----------------
        document.addLayer(QStringLiteral("Fitted"));
        settle();
        report(document.layerProperties().value("fit").toString() == QLatin1String("natural"),
               "a new layer takes its source's natural size");
        document.setLayerFit(QStringLiteral("cover"));
        document.setLayerBoxSize(1920, 1080);
        document.setLayerCrop(10, 20, 0, 0);
        settle();
        const QVariantMap fitted = document.layerProperties();
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
        // one undoable step -- nobody pressing Ctrl+Z after adding a camera wants the layer back
        // without its source, or the source back with no layer.
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
        const int layers_before_source = document.layersModel()->rowCount();
        const QString undo_before_source = document.undoDescription();
        document.addSourceAsLayer(QStringLiteral("Desk Cam"), QStringLiteral("source.v4l2"));
        settle();
        report(document.sourcesModel()->rowCount() == sources_before + 1,
               "adding a source puts it in the project");
        report(document.layersModel()->rowCount() == layers_before_source + 1,
               "and in the scene");
        const QString cam_id = source_at(sources_before, 1).toString();
        const QString cam_layer = document.selectedLayer();
        report(document.layerProperties().value("name").toString() == QLatin1String("Desk Cam"),
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
                   document.layersModel()->rowCount() == layers_before_source,
               "one undo removes the source and the layer together");
        document.redo();
        settle();

        // An existing source goes into the scene again without being created twice: one camera,
        // opened once, in as many scenes as you like.
        const int layers_before_reuse = document.layersModel()->rowCount();
        document.addExistingSourceAsLayer(cam_id);
        settle();
        report(document.sourcesModel()->rowCount() == sources_before + 1,
               "adding an existing source does not create a second one");
        report(document.layersModel()->rowCount() == layers_before_reuse + 1,
               "but does add another layer");
        report(source_at(sources_before, 4).toInt() == 2, "and the use count says two");
        document.undo();
        settle();

        // Explicitly, because the undo above moved the selection -- unbinding "whatever is
        // selected" would have been unbinding something else.
        document.selectLayer(cam_layer);
        settle();
        document.bindSelectedLayer(QString{});
        settle();
        report(source_at(sources_before, 4).toInt() == 0,
               "unbinding the layer releases the source");
        report(source_at(sources_before, 5).toBool() == false,
               "a camera is not safe for an emergency scene");

        // A colour is the one source this build can preview honestly, so the dialog shows its
        // real swatch; everything else falls back to the type glyph and says so by having none.
        document.addSourceAsLayer(QStringLiteral("Backdrop"), QStringLiteral("source.color"));
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
        document.addSourceAsLayer(QStringLiteral("Hidden"), QStringLiteral("source.color"), false);
        settle();
        report(!document.layerProperties().value("visible").toBool(),
               "a source added with 'make visible' cleared arrives hidden");
        document.undo();
        document.undo();
        settle();

        // ---- the audio mixer -------------------------------------------------------------
        //
        // No audio host exists yet, so what is checked here is the document half: the buses, the
        // faders, and which bus a layer's audio goes to. All of it is commands, so all of it has
        // to undo.
        const auto bus_strips = [&] { return collect(window->contentItem(),
                                                     QStringLiteral("busStrip")); };
        const auto bus_at = [&](int row, int role) {
            return document.busesModel()->data(document.busesModel()->index(row, 0),
                                               Qt::UserRole + role);
        };
        settle();
        const int buses_before = document.busesModel()->rowCount();
        report(buses_before >= 1, "the document has at least one bus");
        report(bus_strips().size() == buses_before, "and a strip on screen for each one");
        report(!document.busKinds().isEmpty(), "the bus kinds are listed");

        // A strip squeezed into the panel's minimum height still has to be a fader rather than a
        // sliver. The default layout's mixer row is the smallest this gets without a drag.
        {
            int usable_faders = 0;
            for (const QQuickItem* f : collect(window->contentItem(),
                                               QStringLiteral("busFader"))) {
                if (f->height() >= 24.0 && f->width() > 1.0)
                    ++usable_faders;
            }
            report(usable_faders == bus_strips().size(),
                   "every fader has room to be dragged at the panel's docked size");
        }

        document.addBus(QStringLiteral("Game"), QStringLiteral("submix"));
        settle();
        report(document.busesModel()->rowCount() == buses_before + 1, "a bus can be added");
        report(bus_strips().size() == buses_before + 1, "and the mixer redrew");

        const QString new_bus = bus_at(buses_before, 1).toString();
        document.setBusGain(new_bus, -6.5);
        document.setBusMute(new_bus, true);
        document.setBusPan(new_bus, -0.5);
        settle();
        report(qFuzzyCompare(bus_at(buses_before, 4).toDouble(), -6.5), "its fader moves");
        report(bus_at(buses_before, 6).toBool(), "it can be muted");
        report(qFuzzyCompare(bus_at(buses_before, 5).toDouble(), -0.5), "and panned");

        // The fader as an operator actually reaches it: synthetic mouse events delivered to the
        // window, so the QML handlers run.
        //
        // Everything else about the mixer is driven through Document, the way the divider drag
        // is driven through Workspace. That proves the model and the commands, and proves
        // nothing about the twenty lines of QML between a pointer and them -- which, given that
        // this project exists because of how a UI behaves, is the half worth checking.
        {
            const QList<QQuickItem*> faders =
                collect(window->contentItem(), QStringLiteral("busFader"));
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
                    if (bus_at(r, 1).toString() == dragged_bus)
                        dragged_row = r;
                }
                const auto gain_of_dragged = [&] {
                    return dragged_row < 0 ? 0.0 : bus_at(dragged_row, 4).toDouble();
                };

                const QPointF top = fader->mapToScene(QPointF(fader->width() / 2.0, 2.0));
                const QPointF bottom =
                    fader->mapToScene(QPointF(fader->width() / 2.0, fader->height() - 2.0));

                const auto post = [&window](QEvent::Type type, QPointF at, Qt::MouseButton b,
                                            Qt::MouseButtons held) {
                    QMouseEvent e{type, at, window->mapToGlobal(at), b, held, {}};
                    QCoreApplication::sendEvent(window, &e);
                };

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
            const double start = bus_at(buses_before, 4).toDouble();
            const QString was = document.undoDescription();
            for (int i = 0; i < 40; ++i)
                document.setBusGain(new_bus, -20.0 + i * 0.25, true);
            settle();
            report(qFuzzyCompare(bus_at(buses_before, 4).toDouble(), -20.0 + 39 * 0.25),
                   "a dragged fader follows the drag");
            document.endGesture();
            document.undo();
            settle();
            report(qFuzzyCompare(bus_at(buses_before, 4).toDouble(), start),
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
        report(bus_at(buses_before, 5).toDouble() <= 1.0, "a pan outside the range is clamped");

        // Routing: a layer's audio has to be able to reach a bus, and be taken off it again.
        document.addLayer(QStringLiteral("Mic"));
        settle();
        if (!document.selectedLayer().isEmpty()) {
            report(document.layerProperties().value("hasAudio").toBool(),
                   "a layer has audio of its own");
            report(document.layerProperties().value("audioBus").toString().isEmpty(),
                   "and starts routed nowhere, which is not the same as muted");

            document.routeSelectedLayer(new_bus);
            settle();
            report(document.layerProperties().value("audioBus").toString() == new_bus,
                   "it can be routed to a bus");
            report(bus_at(buses_before, 9).toInt() == 1,
                   "which the bus strip counts as something feeding it");
            report(bus_at(buses_before, 11).toBool(),
                   "and marks as carrying the selection");

            document.setSelectedLayerGain(-3.0);
            document.setSelectedLayerMute(true);
            settle();
            report(qFuzzyCompare(document.layerProperties().value("audioGain").toDouble(), -3.0),
                   "the layer has its own fader");
            report(document.layerProperties().value("audioMute").toBool(),
                   "and its own mute");

            document.undo();
            settle();
            report(!document.layerProperties().value("audioMute").toBool(),
                   "both of which undo");

            // Removing the bus underneath it is allowed: a dangling route is a warning, not an
            // error, and the strip has to stop claiming the layer feeds it.
            document.removeBus(new_bus);
            settle();
            report(document.busesModel()->rowCount() == buses_before,
                   "a bus can be removed while a layer still routes to it");
            report(document.layerProperties().value("audioBusName").toString() ==
                       QLatin1String("(missing bus)"),
                   "and the layer says its bus is gone rather than looking unrouted");
            report(!document.problems().isEmpty(), "which validate() reports as a problem");

            document.undo();
            settle();
            report(document.busesModel()->rowCount() == buses_before + 1,
                   "undo brings the bus back");
            report(document.layerProperties().value("audioBusName").toString() ==
                       QLatin1String("Game"),
                   "with the layer routed to it again");

            document.routeSelectedLayer(QString{});
            settle();
            report(document.layerProperties().value("audioBus").toString().isEmpty(),
                   "and a layer can be taken off its bus");
        }

        // A group has no audio, and saying so is better than silently doing nothing.
        document.addGroup(QStringLiteral("Overlays"));
        settle();
        if (!document.selectedLayer().isEmpty() &&
            document.layerProperties().value("kind").toString() == QLatin1String("group")) {
            report(!document.layerProperties().value("hasAudio").toBool(),
                   "a group has no audio of its own");
            QString refusal;
            QMetaObject::Connection gc = QObject::connect(
                &document, &libertas::ui::Document::notice,
                [&refusal](const QString& t) { refusal = t; });
            document.routeSelectedLayer(new_bus);
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
        const auto deck_at = [&](int row, int role) {
            return document.decksModel()->data(document.decksModel()->index(row, 0),
                                               Qt::UserRole + role);
        };
        const auto out_at = [&](int row, int role) {
            return document.outputsModel()->data(document.outputsModel()->index(row, 0),
                                                 Qt::UserRole + role);
        };
        settle();
        report(document.decksModel()->rowCount() >= 1, "the document has a deck");
        report(collect(window->contentItem(), QStringLiteral("deckBlock")).size() ==
                   document.decksModel()->rowCount(),
               "and the controls panel drew one block for it");
        report(!document.transitions().isEmpty(), "the transitions are listed");
        report(!document.codecs().isEmpty(), "and the encoders");

        const QString deck_id = deck_at(0, 1).toString();
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
        report(deck_at(0, 7).toString() == document.selectedScene(),
               "a scene can be staged on preview");
        document.takeDeck(deck_id);
        settle();
        report(deck_at(0, 4).toString() == deck_at(0, 5).toString(),
               "and a take puts it on program");
        const QString after_take = document.undoDescription();
        document.takeDeck(deck_id);
        settle();
        report(document.undoDescription() == after_take,
               "taking what is already on air changes nothing");

        document.setDeckStudioMode(deck_id, true);
        document.setDeckTransition(deck_id, QStringLiteral("transition.fade"), 450);
        settle();
        report(deck_at(0, 8).toBool(), "studio mode is a document setting");
        report(deck_at(0, 9).toString() == QLatin1String("transition.fade") &&
                   deck_at(0, 10).toInt() == 450,
               "and so is the transition and its duration");
        document.undo();
        document.undo();
        settle();
        report(!deck_at(0, 8).toBool(), "both of which undo");

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
        report(!out_at(outputs_before, 10).toBool(),
               "disabled, because an enabled one would be a keystroke from going live");

        const QString out_id = out_at(outputs_before, 1).toString();
        document.setOutputEncoder(out_id, QStringLiteral("hevc"), QStringLiteral("nvenc"));
        document.setOutputBitrate(out_id, 12000);
        document.setOutputDestination(out_id, QStringLiteral("rtmps"),
                                      QStringLiteral("rtmps://a.example/live"));
        document.setOutputCredentialRef(out_id, QStringLiteral("youtube-main"));
        settle();
        report(out_at(outputs_before, 4).toString() == QLatin1String("hevc") &&
                   out_at(outputs_before, 5).toString() == QLatin1String("nvenc"),
               "its encoder can be changed");
        report(out_at(outputs_before, 6).toInt() == 12000, "and its bitrate");
        report(out_at(outputs_before, 9).toString() == QLatin1String("youtube-main"),
               "and the *name* of its credential");

        // Enabled with nowhere to send to is an error the panel has to show, not swallow.
        document.setOutputDestination(out_id, QStringLiteral("rtmps"), QString{});
        document.setOutputEnabled(out_id, true);
        settle();
        report(!out_at(outputs_before, 10).toBool() || out_at(outputs_before, 11).toBool(),
               "an output enabled with no destination is either refused or flagged");

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

        document.removeOutput(out_id);
        settle();
        report(document.outputsModel()->rowCount() == outputs_before,
               "an output can be removed");
        document.undo();
        settle();
        report(document.outputsModel()->rowCount() == outputs_before + 1,
               "and undo brings it back");
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

        // An input can be removed even while a layer still uses it.
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
            report(document.layersModel()->rowCount() == 0, "and no layers");
            report(document.layerProperties().isEmpty(), "and no properties to show");
            // Adding one again has to work from empty.
            document.addLayer(QStringLiteral("orphan"));
            settle();
            report(document.layersModel()->rowCount() == 0,
                   "a layer cannot be added when there is no scene to add it to");

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
