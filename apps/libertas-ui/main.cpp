// SPDX-FileCopyrightText: Libertas-Studio contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include "theme.hpp"
#include "workspace.hpp"

#include "llayout/serialize.hpp"

#include <QCommandLineOption>
#include <QCommandLineParser>
#include <QGuiApplication>
#include <QObject>
#include <QQmlApplicationEngine>
#include <QCoreApplication>
#include <QEventLoop>
#include <QImage>
#include <QQuickItem>
#include <QCoreApplication>
#include <QEventLoop>
#include <QImage>
#include <QQuickItem>
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
    const QCommandLineOption shot_opt{
        QStringList{QStringLiteral("check-shots")},
        QStringLiteral("With --check, write the grabbed frames into this directory."),
        QStringLiteral("dir")};
    parser.addOption(layout_opt);
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

    QQmlApplicationEngine engine;

    // Touching them here forces construction, so the notice signals are connected before any QML
    // runs and a complaint from loading the layout is not lost.
    auto* workspace_ptr =
        engine.singletonInstance<libertas::ui::Workspace*>("Libertas", "Workspace");
    auto* theme_ptr = engine.singletonInstance<libertas::ui::Theme*>("Libertas", "Theme");
    if (!workspace_ptr || !theme_ptr)
        return fail("the workspace and theme singletons could not be created");
    libertas::ui::Workspace& workspace = *workspace_ptr;
    libertas::ui::Theme& theme = *theme_ptr;

    const auto say = [](const QString& t) {
        std::fprintf(stderr, "notice: %s\n", qPrintable(t));
    };
    QObject::connect(&workspace, &libertas::ui::Workspace::notice, &app, say);
    QObject::connect(&theme, &libertas::ui::Theme::notice, &app, say);

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
        std::fprintf(stderr, "\n  theme: %s (%s), layout: %s\n", qPrintable(theme.name()),
                     theme.dark() ? "dark" : "light", qPrintable(workspace.layoutSource()));
        std::fprintf(stderr, "  %s\n", rc == 0 ? "shell self-check passed" : "SHELL CHECK FAILED");
        return rc;
    }

    return QGuiApplication::exec();
}
