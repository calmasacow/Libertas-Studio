// SPDX-FileCopyrightText: Libertas-Studio contributors
// SPDX-License-Identifier: GPL-3.0-or-later
//
// `libertas probe` — report everything about this machine that affects capture, composite and
// encode. This is the permanent bug-report tool (ADR-0005), so it must be dependency-light,
// never crash, and degrade to "unknown" rather than failing.

#include "probe.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cerrno>
#include <cstring>
#include <format>
#include <string>
#include <string_view>
#include <vector>

#include <fcntl.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <unistd.h>

#include <xf86drm.h>
#include <vulkan/vulkan.h>

extern "C" {
#include <libavcodec/avcodec.h>
#include <pipewire/pipewire.h>
}

#include <gio/gio.h>

namespace libertas {
namespace {

// ---------------------------------------------------------------------------- output helpers

template <typename... Args> void out(std::format_string<Args...> fmt, Args&&... args) {
    const std::string s = std::format(fmt, std::forward<Args>(args)...);
    std::fwrite(s.data(), 1, s.size(), stdout);
    std::fputc('\n', stdout);
}

void section(std::string_view title) {
    out("");
    out("== {} {}", title, std::string(title.size() < 60 ? 60 - title.size() : 0, '='));
}

std::string env_or(const char* name, std::string_view fallback = "(unset)") {
    const char* v = std::getenv(name);
    return (v != nullptr && *v != '\0') ? std::string(v) : std::string(fallback);
}

/// Runs a command and returns its first line of stdout, trimmed. Empty on any failure.
std::string capture_cmd(const char* cmd) {
    std::FILE* p = ::popen(cmd, "r");
    if (p == nullptr)
        return {};
    std::array<char, 512> buf{};
    std::string first;
    if (std::fgets(buf.data(), static_cast<int>(buf.size()), p) != nullptr)
        first = buf.data();
    ::pclose(p);
    while (!first.empty() && (first.back() == '\n' || first.back() == '\r' || first.back() == ' '))
        first.pop_back();
    return first;
}

bool have_exe(const char* name) {
    const std::string cmd = std::format("command -v {} 2>/dev/null", name);
    return !capture_cmd(cmd.c_str()).empty();
}

struct Findings {
    bool on_xwayland = false;
    bool wayland_ok = false;
    int warnings = 0;
};

void warn_line(Findings& f, std::string_view msg) {
    ++f.warnings;
    out("  !! {}", msg);
}

// ---------------------------------------------------------------------------- session / display

void probe_session(Findings& f) {
    section("session");

    const std::string session_type = env_or("XDG_SESSION_TYPE");
    const std::string wl_display = env_or("WAYLAND_DISPLAY");
    const std::string x_display = env_or("DISPLAY");
    const std::string qt_platform = env_or("QT_QPA_PLATFORM");
    const std::string desktop = env_or("XDG_CURRENT_DESKTOP");

    out("  XDG_SESSION_TYPE    {}", session_type);
    out("  XDG_CURRENT_DESKTOP {}", desktop);
    out("  WAYLAND_DISPLAY     {}", wl_display);
    out("  DISPLAY             {}  (XWayland, for other clients)", x_display);
    out("  QT_QPA_PLATFORM     {}", qt_platform);
    out("  QT_QPA_PLATFORMTHEME {}", env_or("QT_QPA_PLATFORMTHEME"));

    f.wayland_ok = (session_type == "wayland") && (wl_display != "(unset)");

    // ADR-0003: we must never *be* an X11 client. A trailing ";xcb" in QT_QPA_PLATFORM lets Qt
    // silently fall back to XWayland, which is how one ends up debugging an X11 app believed
    // to be native. Flag it here so it can never regress unnoticed.
    if (qt_platform.find("xcb") != std::string::npos) {
        warn_line(f, std::format(
            "QT_QPA_PLATFORM contains 'xcb' ({}): Qt may silently fall back to XWayland. "
            "Libertas forces QT_QPA_PLATFORM=wayland with no fallback.", qt_platform));
    }

    if (!f.wayland_ok) {
        f.on_xwayland = true;
        out("  !! not a Wayland session - Libertas is Wayland-native with no X11 backend (ADR-0003)");
    }

    const std::string hypr = env_or("HYPRLAND_INSTANCE_SIGNATURE", "");
    if (!hypr.empty() && have_exe("hyprctl"))
        out("  compositor          Hyprland {}", capture_cmd("hyprctl version -j 2>/dev/null | "
            "sed -n 's/.*\"tag\": *\"\\([^\"]*\\)\".*/\\1/p' | head -1"));

    out("  AQ_DRM_DEVICES      {}", env_or("AQ_DRM_DEVICES"));
}

// ---------------------------------------------------------------------------- DRM devices

struct DrmNode {
    std::string path;      // /dev/dri/renderD128
    std::string driver;    // nvidia-drm, i915, ...
    unsigned dev_major = 0;
    unsigned dev_minor = 0;
    bool is_render = false;
};

std::vector<DrmNode> probe_drm() {
    section("DRM devices");
    std::vector<DrmNode> nodes;

    std::array<drmDevicePtr, 16> devs{};
    const int n = drmGetDevices2(0, devs.data(), static_cast<int>(devs.size()));
    if (n <= 0) {
        out("  (drmGetDevices2 returned {}; no DRM devices visible)", n);
        return nodes;
    }

    for (int i = 0; i < n; ++i) {
        drmDevicePtr d = devs[static_cast<std::size_t>(i)];
        std::string bus = "(non-PCI)";
        if (d->bustype == DRM_BUS_PCI && d->businfo.pci != nullptr) {
            bus = std::format("pci {:04x}:{:02x}:{:02x}.{} vendor {:04x} device {:04x}",
                              d->businfo.pci->domain, d->businfo.pci->bus,
                              d->businfo.pci->dev, d->businfo.pci->func,
                              d->deviceinfo.pci->vendor_id, d->deviceinfo.pci->device_id);
        }

        for (int t = 0; t < DRM_NODE_MAX; ++t) {
            if ((d->available_nodes & (1 << t)) == 0)
                continue;
            const char* path = d->nodes[t];
            if (path == nullptr)
                continue;

            DrmNode node;
            node.path = path;
            node.is_render = (t == DRM_NODE_RENDER);

            struct stat st{};
            if (::stat(path, &st) == 0) {
                node.dev_major = major(st.st_rdev);
                node.dev_minor = minor(st.st_rdev);
            }

            const int fd = ::open(path, O_RDWR | O_CLOEXEC);
            if (fd >= 0) {
                if (drmVersionPtr ver = drmGetVersion(fd); ver != nullptr) {
                    node.driver = (ver->name != nullptr) ? ver->name : "?";
                    drmFreeVersion(ver);
                }
                ::close(fd);
            } else {
                node.driver = std::format("(open failed: {})", std::strerror(errno));
            }

            out("  {:<22} {}:{:<3} driver={:<12} {}", node.path, node.dev_major, node.dev_minor,
                node.driver.empty() ? "?" : node.driver, bus);
            nodes.push_back(std::move(node));
        }
    }
    drmFreeDevices(devs.data(), n);
    return nodes;
}

// ---------------------------------------------------------------------------- Vulkan

bool has_ext(const std::vector<VkExtensionProperties>& exts, const char* name) {
    return std::any_of(exts.begin(), exts.end(), [name](const VkExtensionProperties& e) {
        return std::strcmp(e.extensionName, name) == 0;
    });
}

const char* device_type_name(VkPhysicalDeviceType t) {
    switch (t) {
    case VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU: return "integrated";
    case VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU:   return "discrete";
    case VK_PHYSICAL_DEVICE_TYPE_VIRTUAL_GPU:    return "virtual";
    case VK_PHYSICAL_DEVICE_TYPE_CPU:            return "cpu";
    default:                                     return "other";
    }
}

void probe_vulkan(Findings& f, const std::vector<DrmNode>& drm_nodes) {
    section("Vulkan");

    VkApplicationInfo app{};
    app.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    app.pApplicationName = "libertas-probe";
    app.apiVersion = VK_API_VERSION_1_3;

    VkInstanceCreateInfo ici{};
    ici.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    ici.pApplicationInfo = &app;

    VkInstance inst = VK_NULL_HANDLE;
    if (VkResult r = vkCreateInstance(&ici, nullptr, &inst); r != VK_SUCCESS) {
        warn_line(f, std::format("vkCreateInstance failed ({}) - no Vulkan 1.3 instance",
                                 static_cast<int>(r)));
        return;
    }

    std::uint32_t count = 0;
    vkEnumeratePhysicalDevices(inst, &count, nullptr);
    if (count == 0) {
        warn_line(f, "no Vulkan physical devices");
        vkDestroyInstance(inst, nullptr);
        return;
    }
    std::vector<VkPhysicalDevice> devices(count);
    vkEnumeratePhysicalDevices(inst, &count, devices.data());

    // Extensions that the capture->composite->encode path actually depends on.
    static constexpr std::array<const char*, 7> kWanted{
        VK_EXT_IMAGE_DRM_FORMAT_MODIFIER_EXTENSION_NAME,
        VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME,
        VK_EXT_EXTERNAL_MEMORY_DMA_BUF_EXTENSION_NAME,
        VK_KHR_EXTERNAL_SEMAPHORE_FD_EXTENSION_NAME,
        VK_KHR_TIMELINE_SEMAPHORE_EXTENSION_NAME,
        VK_EXT_PHYSICAL_DEVICE_DRM_EXTENSION_NAME,
        "VK_KHR_video_encode_queue",
    };

    for (std::uint32_t i = 0; i < count; ++i) {
        VkPhysicalDevice dev = devices[i];

        VkPhysicalDeviceDrmPropertiesEXT drm{};
        drm.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRM_PROPERTIES_EXT;
        VkPhysicalDeviceProperties2 props2{};
        props2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
        props2.pNext = &drm;
        vkGetPhysicalDeviceProperties2(dev, &props2);
        const VkPhysicalDeviceProperties& p = props2.properties;

        out("");
        out("  [{}] {}", i, p.deviceName);
        out("      type={} api={}.{}.{} driver=0x{:x} vendor=0x{:04x} device=0x{:04x}",
            device_type_name(p.deviceType),
            VK_API_VERSION_MAJOR(p.apiVersion), VK_API_VERSION_MINOR(p.apiVersion),
            VK_API_VERSION_PATCH(p.apiVersion),
            p.driverVersion, p.vendorID, p.deviceID);

        std::uint32_t ext_count = 0;
        vkEnumerateDeviceExtensionProperties(dev, nullptr, &ext_count, nullptr);
        std::vector<VkExtensionProperties> exts(ext_count);
        vkEnumerateDeviceExtensionProperties(dev, nullptr, &ext_count, exts.data());

        // DRM node identity: this is how we pick the compositing device from a dmabuf's owner
        // instead of from config (see the plan, section 3).
        if (has_ext(exts, VK_EXT_PHYSICAL_DEVICE_DRM_EXTENSION_NAME)) {
            if (drm.hasPrimary != 0u)
                out("      drm primary {}:{}", drm.primaryMajor, drm.primaryMinor);
            if (drm.hasRender != 0u)
                out("      drm render  {}:{}", drm.renderMajor, drm.renderMinor);

            for (const DrmNode& n : drm_nodes) {
                const bool match_primary = drm.hasPrimary != 0u &&
                    n.dev_major == static_cast<unsigned>(drm.primaryMajor) &&
                    n.dev_minor == static_cast<unsigned>(drm.primaryMinor);
                const bool match_render = drm.hasRender != 0u &&
                    n.dev_major == static_cast<unsigned>(drm.renderMajor) &&
                    n.dev_minor == static_cast<unsigned>(drm.renderMinor);
                if (match_primary || match_render)
                    out("      -> {} ({})", n.path, n.driver);
            }
        } else {
            out("      drm        (VK_EXT_physical_device_drm unsupported: cannot match to a "
                "DRM node, so buffer-derived device selection is unavailable here)");
        }

        for (const char* want : kWanted)
            out("      {:<46} {}", want, has_ext(exts, want) ? "yes" : "NO");
    }

    vkDestroyInstance(inst, nullptr);
}

// ---------------------------------------------------------------------------- encoders

void probe_encoders() {
    section("encoders (libavcodec)");
    out("  libavcodec {}.{}.{}", LIBAVCODEC_VERSION_MAJOR, LIBAVCODEC_VERSION_MINOR,
        LIBAVCODEC_VERSION_MICRO);
    out("");

    static constexpr std::array<const char*, 12> kNames{
        "h264_nvenc", "hevc_nvenc", "av1_nvenc",
        "h264_vaapi", "hevc_vaapi", "av1_vaapi",
        "h264_qsv",   "hevc_qsv",   "av1_qsv",
        "libx264",    "libsvtav1",  "aac",
    };
    for (const char* name : kNames) {
        const AVCodec* c = avcodec_find_encoder_by_name(name);
        out("  {:<14} {}", name, (c != nullptr) ? "present" : "-");
    }
    out("");
    out("  note: NVENC concurrent-session capacity is NOT probed yet. Per the plan it must be");
    out("        MEASURED (not hardcoded) before per-destination multistream encode is offered.");
}

// ---------------------------------------------------------------------------- PipeWire + portal

void probe_pipewire() {
    section("PipeWire");
    pw_init(nullptr, nullptr);
    out("  headers {}", pw_get_headers_version());
    out("  library {}", pw_get_library_version());
    pw_deinit();
}

void probe_portal(Findings& f) {
    section("xdg-desktop-portal");

    GError* err = nullptr;
    GDBusConnection* bus = g_bus_get_sync(G_BUS_TYPE_SESSION, nullptr, &err);
    if (bus == nullptr) {
        warn_line(f, std::format("no session bus: {}",
                                 (err != nullptr && err->message != nullptr) ? err->message : "?"));
        if (err != nullptr) g_error_free(err);
        return;
    }

    GVariant* reply = g_dbus_connection_call_sync(
        bus, "org.freedesktop.portal.Desktop", "/org/freedesktop/portal/desktop",
        "org.freedesktop.DBus.Introspectable", "Introspect", nullptr, G_VARIANT_TYPE("(s)"),
        G_DBUS_CALL_FLAGS_NONE, 3000, nullptr, &err);

    if (reply == nullptr) {
        warn_line(f, std::format("portal introspect failed: {}",
                                 (err != nullptr && err->message != nullptr) ? err->message : "?"));
        if (err != nullptr) g_error_free(err);
        g_object_unref(bus);
        return;
    }

    const char* xml = nullptr;
    g_variant_get(reply, "(&s)", &xml);
    const std::string_view doc = (xml != nullptr) ? std::string_view(xml) : std::string_view{};

    static constexpr std::array<const char*, 5> kIfaces{
        "org.freedesktop.portal.ScreenCast",
        "org.freedesktop.portal.GlobalShortcuts",
        "org.freedesktop.portal.Screenshot",
        "org.freedesktop.portal.Inhibit",
        "org.freedesktop.portal.RemoteDesktop",
    };
    for (const char* iface : kIfaces) {
        const bool present = doc.find(iface) != std::string_view::npos;
        out("  {:<42} {}", iface, present ? "yes" : "NO");
        if (!present && (std::strcmp(iface, "org.freedesktop.portal.ScreenCast") == 0 ||
                         std::strcmp(iface, "org.freedesktop.portal.GlobalShortcuts") == 0)) {
            warn_line(f, std::format("{} missing - required (ADR-0003)", iface));
        }
    }

    g_variant_unref(reply);
    g_object_unref(bus);
}

// ---------------------------------------------------------------------------- VAAPI drivers

// On a dual-GPU box, nvidia-vaapi-driver can claim VAAPI for *every* render node, including the
// Intel one, and NVDEC is decode-only. That surfaces as "No usable encoding entrypoint found".
// See ADR-0006. We must therefore pick the driver per device rather than trusting libva's default.
void probe_vaapi(Findings& f, const std::vector<DrmNode>& nodes) {
    section("VAAPI drivers per render node");

    if (!have_exe("vainfo")) {
        out("  vainfo not installed - cannot verify VAAPI driver binding (install libva-utils)");
        return;
    }

    bool any_encode = false;
    for (const DrmNode& n : nodes) {
        if (!n.is_render)
            continue;

        const std::string drv = capture_cmd(std::format(
            "vainfo --display drm --device {} 2>&1 | sed -n 's/.*Driver version: *//p' | head -1",
            n.path).c_str());
        const std::string enc = capture_cmd(std::format(
            "vainfo --display drm --device {} 2>&1 | grep -c 'VAProfileH264High.*VAEntrypointEncSlice'",
            n.path).c_str());
        const bool has_enc = (!enc.empty() && enc != "0");
        any_encode = any_encode || has_enc;

        out("  {:<22} default driver : {}", n.path, drv.empty() ? "(none)" : drv);
        out("  {:<22} H264 encode    : {}", "", has_enc ? "yes" : "NO");

        // If the default binding cannot encode, try the vendor-appropriate driver explicitly.
        if (!has_enc) {
            const char* forced = (n.driver == "i915" || n.driver == "xe") ? "iHD"
                               : (n.driver == "amdgpu") ? "radeonsi" : nullptr;
            if (forced != nullptr) {
                const std::string fdrv = capture_cmd(std::format(
                    "LIBVA_DRIVER_NAME={} vainfo --display drm --device {} 2>&1 "
                    "| sed -n 's/.*Driver version: *//p' | head -1", forced, n.path).c_str());
                const std::string fenc = capture_cmd(std::format(
                    "LIBVA_DRIVER_NAME={} vainfo --display drm --device {} 2>&1 "
                    "| grep -c 'VAProfileH264High.*VAEntrypointEncSlice'", forced, n.path).c_str());
                if (!fenc.empty() && fenc != "0") {
                    any_encode = true;
                    out("  {:<22} with LIBVA_DRIVER_NAME={}: {} -- H264 encode yes", "", forced,
                        fdrv);
                    warn_line(f, std::format(
                        "{}: VAAPI default binding cannot encode, but LIBVA_DRIVER_NAME={} can. "
                        "Libertas must set the driver explicitly per device (ADR-0006).",
                        n.path, forced));
                }
            }
        }
    }

    if (!any_encode)
        out("  note: no VAAPI encode entrypoint found on any node; the VAAPI fallback tier is "
            "unavailable here.");
}

// ---------------------------------------------------------------------------- QSV sessions

// QSV sits on oneVPL over VAAPI, so the nvidia-vaapi-driver hijack (ADR-0006/0007) stops it from
// even creating a session. This is the author's production Twitch encoder, so it is probed directly.
void probe_qsv(Findings& f, const std::vector<DrmNode>& nodes) {
    section("QSV encode sessions");

    if (!have_exe("ffmpeg")) {
        out("  ffmpeg CLI not found - skipping QSV session check");
        return;
    }

    // A one-second 640x360 encode to /dev/null is enough to prove a session can be created.
    const auto qsv_cmd = [](const std::string& node, const char* env_prefix) {
        return std::string(env_prefix) +
               "ffmpeg -hide_banner -loglevel error -y -init_hw_device qsv=hw:" + node +
               " -filter_hw_device hw -f lavfi -i testsrc=size=640x360:rate=30:duration=1"
               " -vf format=nv12,hwupload=extra_hw_frames=16 -c:v h264_qsv -f null - 2>&1 | head -1";
    };

    bool probed_any = false;
    for (const DrmNode& n : nodes) {
        if (!n.is_render || (n.driver != "i915" && n.driver != "xe"))
            continue;
        probed_any = true;

        const std::string plain = capture_cmd(qsv_cmd(n.path, "").c_str());
        if (plain.empty()) {
            out("  {:<22} QSV session (default env)      : OK", n.path);
            continue;
        }
        out("  {:<22} QSV session (default env)      : FAILED", n.path);
        out("  {:<22}   {}", "", plain);

        const std::string forced = capture_cmd(qsv_cmd(n.path, "LIBVA_DRIVER_NAME=iHD ").c_str());
        if (forced.empty()) {
            out("  {:<22} QSV with LIBVA_DRIVER_NAME=iHD : OK", "");
            warn_line(f, std::format(
                "{}: QSV requires LIBVA_DRIVER_NAME=iHD. Libertas sets this per encode worker "
                "process (ADR-0007) rather than relying on the session environment.", n.path));
        } else {
            out("  {:<22} QSV with LIBVA_DRIVER_NAME=iHD : FAILED", "");
            out("  {:<22}   {}", "", forced);
            warn_line(f, std::format("{}: QSV unavailable on this node", n.path));
        }
    }

    if (!probed_any)
        out("  no Intel render node present; QSV not applicable");
}

// ---------------------------------------------------------------------------- Omarchy theme

void probe_theme() {
    section("Omarchy theme");

    if (!have_exe("omarchy")) {
        out("  omarchy not found - the fallback theme provider will be used");
        return;
    }
    // Plan/ADR: /usr/share/omarchy/version is stale (reports 4.0.0.alpha on a 4.0.4-1 install).
    // Note: the subcommand is 'omarchy version'; 'omarchy --version' is not a valid command (rc=127).
    out("  omarchy version     {}", capture_cmd("omarchy version 2>/dev/null"));

    const std::string home = env_or("HOME", "");
    const std::string state = home + "/.local/state/omarchy/current";
    out("  theme.name          {}",
        capture_cmd(std::format("cat {}/theme.name 2>/dev/null", state).c_str()));
    out("  colors.toml mode    {}",
        capture_cmd(std::format("omarchy-theme-color --file {}/theme/colors.toml mode 2>/dev/null",
                                state).c_str()));
    out("  accent              {}", capture_cmd("omarchy-theme-color accent 2>/dev/null"));
    out("  background          {}", capture_cmd("omarchy-theme-color background 2>/dev/null"));
    out("  resolved keys       {}",
        capture_cmd("omarchy-theme-color --all 2>/dev/null | wc -l"));
    out("  background image    {}",
        capture_cmd(std::format("readlink -f {}/background 2>/dev/null", state).c_str()));
    out("");
    out("  hook dir            {}/.config/omarchy/hooks/theme-set.d/", home);
    out("  template dir        {}/.config/omarchy/themed/", home);
}

} // namespace

int probe_main(bool strict) {
    Findings f;

    out("libertas probe - environment report");
    out("(capture/composite/encode relevant facts; attach this to any bug report)");

    probe_session(f);
    const std::vector<DrmNode> drm_nodes = probe_drm();
    probe_vulkan(f, drm_nodes);
    probe_encoders();
    probe_vaapi(f, drm_nodes);
    probe_qsv(f, drm_nodes);
    probe_pipewire();
    probe_portal(f);
    probe_theme();

    section("verdict");
    if (f.on_xwayland) {
        out("  FAIL: not a native Wayland session.");
        out("  Libertas is Wayland-native and ships no X11 backend (ADR-0003). Running under");
        out("  XWayland is refused deliberately rather than silently degrading.");
        return 2;
    }
    out("  Wayland session OK; {} warning(s).", f.warnings);
    if (strict && f.warnings > 0) {
        out("  FAIL: --strict was given and warnings were found.");
        return 1;
    }
    return 0;
}

} // namespace libertas
