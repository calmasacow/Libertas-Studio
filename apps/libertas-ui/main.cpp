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
#include <QQuickWindow>
#include <QTimer>
#include <QUrl>

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
void insist_on_wayland() {
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
    insist_on_wayland();

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
    parser.addOption(layout_opt);
    parser.addOption(check_opt);
    parser.process(app);

    const bool check = parser.isSet(check_opt);
    if (!check && QGuiApplication::platformName() == QLatin1String("xcb")) {
        return fail("running on XWayland (xcb). Libertas is Wayland-native by design (ADR-0003); "
                    "it will not run as an X11 client. Start it from a Wayland session.");
    }

    const std::filesystem::path layout_path =
        parser.isSet(layout_opt)
            ? std::filesystem::path(parser.value(layout_opt).toStdString())
            : config_dir() / "layout.toml";

    // The singletons are constructed by the QML engine, so everything they need that QML cannot
    // supply is handed over first. `headless` is how --check keeps the window off screen: on a
    // live Wayland session a self-test has no business putting one there.
    libertas::ui::Workspace::configure(layout_path, check);

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
        report(!window->isVisible(), "the window is not shown in --check mode");

        workspace.setViewport(2560, 1400);
        // Start from the default rather than from whatever a previous run saved. The check runs
        // against the real config path by default, and persistence working is precisely why the
        // assertions below would otherwise depend on the last run.
        workspace.resetLayout();
        report(workspace.panelCount() == 6, "the default layout resolves to six panel groups");
        report(workspace.dividerCount() == 5, "and five dividers");

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
        workspace.beginDrag(QStringLiteral("log"));
        workspace.updateDrag(px, py);
        report(workspace.dragging(), "a drag is in flight");
        report(workspace.dropValid(), "the left zone of the program canvas accepts the drop");
        report(workspace.dropDescription().contains(QStringLiteral("left")),
               "and says what it would do");
        report(!workspace.dropPreview().isEmpty(), "with a preview region to highlight");
        report(!workspace.dropTargetRect().isEmpty(), "and a target rect for the wireframe");
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
