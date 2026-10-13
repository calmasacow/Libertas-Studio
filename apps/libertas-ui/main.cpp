// SPDX-FileCopyrightText: Libertas-Studio contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include "document.hpp"
#include "shortcuts.hpp"
#include "theme.hpp"
#include "workspace.hpp"

#include "llayout/serialize.hpp"

#include "ldoc/json.hpp"

#include <QCommandLineOption>
#include <QCommandLineParser>
#include <QGuiApplication>
#include <QObject>
#include <QQmlApplicationEngine>
#include <QCoreApplication>
#include <QEventLoop>
#include <QImage>
#include <QQuickItem>
#include <QSet>
#include <QVariantMap>
#include <QQuickWindow>
#include <QTimer>
#include <QUrl>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>

namespace {

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
        const auto changed_fraction = [&](const QRectF& r) {
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
        workspace.beginDrag(QStringLiteral("stats"));
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
        workspace.closePanel(QStringLiteral("stats"));
        report(workspace.closedPanels().contains(QStringLiteral("stats")),
               "a closed panel is listed as reopenable");
        workspace.openPanel(QStringLiteral("stats"));
        report(!workspace.closedPanels().contains(QStringLiteral("stats")),
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
        const auto reloaded = llayout::load(layout_path);
        report(reloaded.has_value(), "the layout saved and parses back");
        if (reloaded)
            report(!reloaded->validate().has_value(), "and what was written is valid");

        report(!theme.colors().isEmpty(), "the theme exposes colours");
        report(theme.colors().contains(QStringLiteral("dropFill")), "including the drop overlay fill");

        // ---- the document panels ------------------------------------------------------------
        //
        // Driven through the same API QML drives, which is the whole point: the UI holds no
        // mutable Project and every one of these is a command going to the session.
        const auto scene_rows = [&] { return collect(window->contentItem(),
                                                     QStringLiteral("sceneRow")).size(); };
        const auto layer_rows = [&] { return collect(window->contentItem(),
                                                     QStringLiteral("layerRow")).size(); };

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

        // ---- inputs ---------------------------------------------------------------------
        const int inputs_before = document.sourcesModel()->rowCount();
        report(!document.sourceTypes().isEmpty(), "the source types are listed");
        document.addSource(QStringLiteral("Desk Cam"), QStringLiteral("source.v4l2"));
        settle();
        report(document.sourcesModel()->rowCount() == inputs_before + 1, "an input can be added");

        // Binding needs a layer; the one added above was removed with its scene, so make another.
        document.addLayer(QStringLiteral("Cam Layer"));
        settle();
        const QString input_id =
            document.sourcesModel()
                ->data(document.sourcesModel()->index(inputs_before, 0),
                       Qt::UserRole + 1)
                .toString();
        document.bindSelectedLayer(input_id);
        settle();
        report(document.sourcesModel()
                   ->data(document.sourcesModel()->index(inputs_before, 0),
                          Qt::UserRole + 4)
                   .toInt() == 1,
               "and bound to a layer, which the use count reflects");

        document.bindSelectedLayer(QString{});
        settle();
        report(document.sourcesModel()
                   ->data(document.sourcesModel()->index(inputs_before, 0),
                          Qt::UserRole + 4)
                   .toInt() == 0,
               "unbinding it again releases the input");

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
        report(collect(window->contentItem(), QStringLiteral("outputBlock")).size() ==
                   outputs_before + 1,
               "and the panel drew it");
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
