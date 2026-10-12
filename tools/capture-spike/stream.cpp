// SPDX-FileCopyrightText: Libertas-Studio contributors
// SPDX-License-Identifier: GPL-3.0-or-later
//
// PipeWire consumer for a portal ScreenCast node, with explicit DRM format-modifier negotiation.
//
// The important difference from OBS's approach: we advertise the modifier list as a MANDATORY
// enumerated choice, so the server either agrees on one of our modifiers or renegotiates - rather
// than accepting a format and failing at import time. See docs/adr/0007 and the plan's section 3.

#include "stream.hpp"

#include <lcore/log.hpp>

#include <array>
#include <cstdio>
#include <cstring>

#include <pipewire/pipewire.h>
#include <spa/param/video/format-utils.h>
#include <spa/pod/builder.h>
#include <spa/debug/format.h>
#include <spa/utils/result.h>

#include <drm_fourcc.h>

namespace libertas::spike {

std::string fourcc_str(std::uint32_t f) {
    if (f == 0)
        return "(none)";
    char b[5] = {static_cast<char>(f & 0xff), static_cast<char>((f >> 8) & 0xff),
                 static_cast<char>((f >> 16) & 0xff), static_cast<char>((f >> 24) & 0xff), '\0'};
    for (char& c : b)
        if (c != '\0' && (c < 32 || c > 126))
            c = '?';
    return std::string(b);
}

std::string modifier_str(std::uint64_t m) {
    if (m == kModifierInvalid)
        return "INVALID (implicit/driver-chosen)";
    if (m == kModifierLinear)
        return "LINEAR";
    const std::uint8_t vendor = static_cast<std::uint8_t>(m >> 56);
    const char* name = "unknown";
    switch (vendor) {
    case 0x01: name = "INTEL"; break;
    case 0x02: name = "AMD"; break;
    case 0x03: name = "NVIDIA"; break;
    case 0x04: name = "SAMSUNG"; break;
    case 0x08: name = "ARM"; break;
    case 0x0a: name = "AMLOGIC"; break;
    default: break;
    }
    return std::format("{}(0x{:x})", name, m);
}

namespace {

// SPA's SPA_RECTANGLE/SPA_FRACTION are C compound literals; C++ cannot take their address, so
// these are named constants instead.
const spa_rectangle kSizeDefault = {1920, 1080};
const spa_rectangle kSizeMin = {1, 1};
const spa_rectangle kSizeMax = {8192, 8192};
const spa_fraction kFpsDefault = {0, 1};  // 0/1 = "source-driven"; a nonzero default fails to
                                          // intersect a source whose framerate is fixed at 0/1.
const spa_fraction kFpsMin = {0, 1};
const spa_fraction kFpsMax = {360, 1};

/// SPA video format -> DRM fourcc. Only the formats a compositor realistically offers.
std::uint32_t spa_to_drm(std::uint32_t spa) {
    switch (spa) {
    case SPA_VIDEO_FORMAT_BGRA: return DRM_FORMAT_ARGB8888;
    case SPA_VIDEO_FORMAT_BGRx: return DRM_FORMAT_XRGB8888;
    case SPA_VIDEO_FORMAT_RGBA: return DRM_FORMAT_ABGR8888;
    case SPA_VIDEO_FORMAT_RGBx: return DRM_FORMAT_XBGR8888;
    case SPA_VIDEO_FORMAT_ARGB: return DRM_FORMAT_BGRA8888;
    case SPA_VIDEO_FORMAT_xRGB: return DRM_FORMAT_BGRX8888;
    case SPA_VIDEO_FORMAT_ABGR: return DRM_FORMAT_RGBA8888;
    case SPA_VIDEO_FORMAT_xBGR: return DRM_FORMAT_RGBX8888;
    default: return 0;
    }
}

const char* spa_format_name(std::uint32_t f) {
    switch (f) {
    case SPA_VIDEO_FORMAT_BGRA: return "BGRA";
    case SPA_VIDEO_FORMAT_BGRx: return "BGRx";
    case SPA_VIDEO_FORMAT_RGBA: return "RGBA";
    case SPA_VIDEO_FORMAT_RGBx: return "RGBx";
    case SPA_VIDEO_FORMAT_ARGB: return "ARGB";
    case SPA_VIDEO_FORMAT_xRGB: return "xRGB";
    case SPA_VIDEO_FORMAT_ABGR: return "ABGR";
    case SPA_VIDEO_FORMAT_xBGR: return "xBGR";
    default: return "?";
    }
}

struct Runner {
    const StreamConfig* cfg = nullptr;
    NegotiatedFormat* fmt = nullptr;
    std::string error;

    pw_main_loop* loop = nullptr;
    pw_context* context = nullptr;
    pw_core* core = nullptr;
    pw_stream* stream = nullptr;
    spa_hook stream_listener{};

    std::uint32_t frames = 0;
    bool format_logged = false;
    bool fixated = false;
    std::array<std::uint8_t, 2048> fixate_buf{};

    void stop() {
        if (loop != nullptr)
            pw_main_loop_quit(loop);
    }
};

/// Builds one EnumFormat POD advertising our modifier list as a mandatory, unfixated choice.
/// The modifier prop needs flags that add_object cannot express, so this one is built by hand -
/// but on the same skeleton as the shm variant above.
spa_pod* build_format_with_modifiers(spa_pod_builder* b, std::uint32_t spa_format,
                                     const std::vector<std::uint64_t>& mods) {
    spa_pod_frame obj;
    spa_pod_builder_push_object(b, &obj, SPA_TYPE_OBJECT_Format, SPA_PARAM_EnumFormat);

    spa_pod_builder_add(b, SPA_FORMAT_mediaType, SPA_POD_Id(SPA_MEDIA_TYPE_video),
                        SPA_FORMAT_mediaSubtype, SPA_POD_Id(SPA_MEDIA_SUBTYPE_raw),
                        SPA_FORMAT_VIDEO_format, SPA_POD_Id(spa_format), 0);

    // modifier: MANDATORY (we require dmabuf) + DONT_FIXATE (server replies with the
    // intersection as a choice, which we then fixate in on_param_changed).
    spa_pod_builder_prop(b, SPA_FORMAT_VIDEO_modifier,
                         SPA_POD_PROP_FLAG_MANDATORY | SPA_POD_PROP_FLAG_DONT_FIXATE);
    spa_pod_frame choice;
    spa_pod_builder_push_choice(b, &choice, SPA_CHOICE_Enum, 0);
    spa_pod_builder_long(b, static_cast<int64_t>(mods.front())); // default
    for (std::uint64_t m : mods)
        spa_pod_builder_long(b, static_cast<int64_t>(m));
    spa_pod_builder_pop(b, &choice);

    spa_pod_builder_add(b, SPA_FORMAT_VIDEO_size,
                        SPA_POD_CHOICE_RANGE_Rectangle(&kSizeDefault, &kSizeMin, &kSizeMax),
                        SPA_FORMAT_VIDEO_framerate,
                        SPA_POD_CHOICE_RANGE_Fraction(&kFpsDefault, &kFpsMin, &kFpsMax), 0);

    return static_cast<spa_pod*>(spa_pod_builder_pop(b, &obj));
}

/// Builds an EnumFormat POD with no modifier property: the shm/MemPtr fallback.
/// Uses the canonical single-call spa_pod_builder_add_object form from PipeWire's own examples,
/// with the pixel format as a CHOICE_ENUM rather than one param per format.
spa_pod* build_format_shm(spa_pod_builder* b) {
    return static_cast<spa_pod*>(spa_pod_builder_add_object(
        b, SPA_TYPE_OBJECT_Format, SPA_PARAM_EnumFormat,
        SPA_FORMAT_mediaType, SPA_POD_Id(SPA_MEDIA_TYPE_video),
        SPA_FORMAT_mediaSubtype, SPA_POD_Id(SPA_MEDIA_SUBTYPE_raw),
        SPA_FORMAT_VIDEO_format,
        SPA_POD_CHOICE_ENUM_Id(5, SPA_VIDEO_FORMAT_BGRA, SPA_VIDEO_FORMAT_BGRA,
                               SPA_VIDEO_FORMAT_BGRx, SPA_VIDEO_FORMAT_RGBA,
                               SPA_VIDEO_FORMAT_RGBx),
        SPA_FORMAT_VIDEO_size,
        SPA_POD_CHOICE_RANGE_Rectangle(&kSizeDefault, &kSizeMin, &kSizeMax),
        SPA_FORMAT_VIDEO_framerate,
        SPA_POD_CHOICE_RANGE_Fraction(&kFpsDefault, &kFpsMin, &kFpsMax)));
}

/// Builds an EnumFormat POD with the modifier pinned to a single value. Required by the dmabuf
/// negotiation protocol: the server answers our DONT_FIXATE choice with the intersection, and we
/// must then come back with exactly one modifier before buffers can be allocated.
spa_pod* build_format_fixated(spa_pod_builder* b, std::uint32_t spa_format, std::uint64_t modifier,
                              spa_rectangle size, spa_fraction fps) {
    spa_pod_frame f;
    spa_pod_builder_push_object(b, &f, SPA_TYPE_OBJECT_Format, SPA_PARAM_EnumFormat);
    spa_pod_builder_add(b, SPA_FORMAT_mediaType, SPA_POD_Id(SPA_MEDIA_TYPE_video), 0);
    spa_pod_builder_add(b, SPA_FORMAT_mediaSubtype, SPA_POD_Id(SPA_MEDIA_SUBTYPE_raw), 0);
    spa_pod_builder_add(b, SPA_FORMAT_VIDEO_format, SPA_POD_Id(spa_format), 0);
    spa_pod_builder_prop(b, SPA_FORMAT_VIDEO_modifier, SPA_POD_PROP_FLAG_MANDATORY);
    spa_pod_builder_long(b, static_cast<int64_t>(modifier));
    spa_pod_builder_add(b, SPA_FORMAT_VIDEO_size, SPA_POD_Rectangle(&size), 0);
    spa_pod_builder_add(b, SPA_FORMAT_VIDEO_framerate, SPA_POD_Fraction(&fps), 0);
    return static_cast<spa_pod*>(spa_pod_builder_pop(b, &f));
}

/// Mirrors a source's format exactly: all properties fixed, nothing negotiable. Diagnostic used to
/// bisect which negotiable property a compositor rejects.
spa_pod* build_format_mirror(spa_pod_builder* b, std::uint32_t spa_format, spa_rectangle size,
                             bool with_modifier, std::uint64_t modifier) {
    spa_pod_frame obj;
    spa_pod_builder_push_object(b, &obj, SPA_TYPE_OBJECT_Format, SPA_PARAM_EnumFormat);
    spa_pod_builder_add(b, SPA_FORMAT_mediaType, SPA_POD_Id(SPA_MEDIA_TYPE_video),
                        SPA_FORMAT_mediaSubtype, SPA_POD_Id(SPA_MEDIA_SUBTYPE_raw),
                        SPA_FORMAT_VIDEO_format, SPA_POD_Id(spa_format), 0);
    if (with_modifier) {
        spa_pod_builder_prop(b, SPA_FORMAT_VIDEO_modifier, SPA_POD_PROP_FLAG_MANDATORY);
        spa_pod_builder_long(b, static_cast<int64_t>(modifier));
    }
    const spa_fraction fps{0, 1};
    spa_pod_builder_add(b, SPA_FORMAT_VIDEO_size, SPA_POD_Rectangle(&size),
                        SPA_FORMAT_VIDEO_framerate, SPA_POD_Fraction(&fps), 0);
    return static_cast<spa_pod*>(spa_pod_builder_pop(b, &obj));
}

void on_state_changed(void* data, pw_stream_state old, pw_stream_state state, const char* err) {
    auto* r = static_cast<Runner*>(data);
    lc::info("stream state {} -> {}{}{}", pw_stream_state_as_string(old),
             pw_stream_state_as_string(state), (err != nullptr) ? ": " : "",
             (err != nullptr) ? err : "");
    if (state == PW_STREAM_STATE_ERROR) {
        r->error = (err != nullptr) ? err : "stream error";
        r->stop();
    }
}

void on_param_changed(void* data, std::uint32_t id, const spa_pod* param) {
    auto* r = static_cast<Runner*>(data);
    if (param == nullptr || id != SPA_PARAM_Format)
        return;

    spa_video_info info{};
    if (spa_format_parse(param, &info.media_type, &info.media_subtype) < 0)
        return;
    if (info.media_type != SPA_MEDIA_TYPE_video || info.media_subtype != SPA_MEDIA_SUBTYPE_raw)
        return;
    if (spa_format_video_raw_parse(param, &info.info.raw) < 0) {
        r->error = "failed to parse negotiated video format";
        r->stop();
        return;
    }

    const spa_pod_prop* mod_prop =
        spa_pod_find_prop(param, nullptr, SPA_FORMAT_VIDEO_modifier);
    const bool have_mod = (mod_prop != nullptr);

    r->fmt->width = info.info.raw.size.width;
    r->fmt->height = info.info.raw.size.height;
    r->fmt->spa_format = info.info.raw.format;
    r->fmt->drm_format = spa_to_drm(info.info.raw.format);
    r->fmt->modifier = have_mod ? info.info.raw.modifier : kModifierInvalid;
    r->fmt->have_modifier = have_mod;
    r->fmt->fps_num = static_cast<std::int32_t>(info.info.raw.framerate.num);
    r->fmt->fps_den = static_cast<std::int32_t>(info.info.raw.framerate.denom);

    if (!r->format_logged) {
        r->format_logged = true;
        lc::info("negotiated {}x{} {} (drm {}) modifier {} fps {}/{}", r->fmt->width,
                 r->fmt->height, spa_format_name(r->fmt->spa_format),
                 fourcc_str(r->fmt->drm_format),
                 have_mod ? modifier_str(r->fmt->modifier) : std::string("none (shm path)"),
                 r->fmt->fps_num, r->fmt->fps_den);
    }

    // The dmabuf negotiation dance: if the server left the modifier unfixated, we must answer with
    // exactly one modifier before it can allocate buffers. Skipping this is what produces
    // "no more input formats".
    if (have_mod && !r->fixated &&
        (mod_prop->flags & SPA_POD_PROP_FLAG_DONT_FIXATE) != 0) {
        r->fixated = true;
        const spa_rectangle size = {r->fmt->width, r->fmt->height};
        const spa_fraction fps = {static_cast<std::uint32_t>(r->fmt->fps_num),
                                  static_cast<std::uint32_t>(r->fmt->fps_den == 0
                                                                 ? 1
                                                                 : r->fmt->fps_den)};
        spa_pod_builder fb = SPA_POD_BUILDER_INIT(
            r->fixate_buf.data(), static_cast<std::uint32_t>(r->fixate_buf.size()));
        const spa_pod* fixed[1];
        fixed[0] = build_format_fixated(&fb, r->fmt->spa_format, r->fmt->modifier, size, fps);
        lc::info("fixating modifier {} for {}", modifier_str(r->fmt->modifier),
                 spa_format_name(r->fmt->spa_format));
        if (pw_stream_update_params(r->stream, fixed, 1) < 0) {
            r->error = "pw_stream_update_params (fixate) failed";
            r->stop();
        }
        return;
    }

    // Tell the server what buffers we can take. Allowing DmaBuf is what gets us zero-copy;
    // MemFd/MemPtr remain permitted so a compositor that cannot do dmabuf still works.
    std::uint32_t data_types = (1u << SPA_DATA_MemPtr) | (1u << SPA_DATA_MemFd);
    if (have_mod)
        data_types |= (1u << SPA_DATA_DmaBuf);

    std::array<std::uint8_t, 1024> buf{};
    spa_pod_builder b = SPA_POD_BUILDER_INIT(buf.data(), static_cast<std::uint32_t>(buf.size()));
    const spa_pod* params[1];
    params[0] = static_cast<const spa_pod*>(spa_pod_builder_add_object(
        &b, SPA_TYPE_OBJECT_ParamBuffers, SPA_PARAM_Buffers,
        SPA_PARAM_BUFFERS_buffers, SPA_POD_CHOICE_RANGE_Int(8, 2, 16),
        SPA_PARAM_BUFFERS_dataType, SPA_POD_CHOICE_FLAGS_Int(data_types)));

    if (pw_stream_update_params(r->stream, params, 1) < 0) {
        r->error = "pw_stream_update_params failed";
        r->stop();
    }
}

void on_process(void* data) {
    auto* r = static_cast<Runner*>(data);

    pw_buffer* b = pw_stream_dequeue_buffer(r->stream);
    if (b == nullptr)
        return;

    spa_buffer* sb = b->buffer;
    CapturedFrame frame;
    frame.sequence = r->frames;
    frame.width = r->fmt->width;
    frame.height = r->fmt->height;
    frame.drm_format = r->fmt->drm_format;
    frame.modifier = r->fmt->modifier;

    if (sb->n_datas > 0)
        frame.is_dmabuf = (sb->datas[0].type == SPA_DATA_DmaBuf);

    for (std::uint32_t i = 0; i < sb->n_datas; ++i) {
        const spa_data& d = sb->datas[i];
        DmabufPlane p;
        p.fd = (d.type == SPA_DATA_DmaBuf) ? static_cast<int>(d.fd) : -1;
        p.offset = d.chunk != nullptr ? d.chunk->offset : 0;
        p.stride = d.chunk != nullptr ? static_cast<std::uint32_t>(d.chunk->stride) : 0;
        p.size = static_cast<std::int64_t>(d.maxsize);
        frame.planes.push_back(p);
    }

    ++r->frames;
    if (r->cfg->on_frame)
        r->cfg->on_frame(frame);

    pw_stream_queue_buffer(r->stream, b);

    if (r->frames >= r->cfg->max_frames)
        r->stop();
}

constexpr pw_stream_events kStreamEvents = {
    .version = PW_VERSION_STREAM_EVENTS,
    .destroy = nullptr,
    .state_changed = on_state_changed,
    .control_info = nullptr,
    .io_changed = nullptr,
    .param_changed = on_param_changed,
    .add_buffer = nullptr,
    .remove_buffer = nullptr,
    .process = on_process,
    .drained = nullptr,
    .command = nullptr,
    .trigger_done = nullptr,
};

} // namespace

// ---------------------------------------------------------------------------- format dump

namespace {

struct Dumper {
    pw_main_loop* loop = nullptr;
    pw_core* core = nullptr;
    pw_registry* registry = nullptr;
    spa_hook registry_listener{};
    spa_hook core_listener{};
    spa_hook node_listener{};
    pw_node* node = nullptr;
    std::uint32_t want_node = 0;
    int sync_seq = 0;
    std::uint32_t params_seen = 0;
};

void on_node_param(void* data, int, std::uint32_t id, std::uint32_t index, std::uint32_t,
                   const spa_pod* param) {
    auto* d = static_cast<Dumper*>(data);
    if (id != SPA_PARAM_EnumFormat || param == nullptr)
        return;
    ++d->params_seen;
    std::printf("  --- EnumFormat[%u] ---\n", index);
    std::fflush(stdout);
    spa_debug_format(4, nullptr, param);
}

constexpr pw_node_events kNodeEvents = {
    .version = PW_VERSION_NODE_EVENTS,
    .info = nullptr,
    .param = on_node_param,
};

void on_registry_global(void* data, std::uint32_t id, std::uint32_t, const char* type,
                        std::uint32_t version, const spa_dict*) {
    auto* d = static_cast<Dumper*>(data);
    if (id != d->want_node || type == nullptr || std::strcmp(type, PW_TYPE_INTERFACE_Node) != 0)
        return;

    d->node = static_cast<pw_node*>(
        pw_registry_bind(d->registry, id, PW_TYPE_INTERFACE_Node, version, 0));
    if (d->node == nullptr)
        return;
    pw_node_add_listener(d->node, &d->node_listener, &kNodeEvents, d);
    pw_node_enum_params(d->node, 0, SPA_PARAM_EnumFormat, 0, UINT32_MAX, nullptr);
    d->sync_seq = pw_core_sync(d->core, PW_ID_CORE, 0);
}

constexpr pw_registry_events kRegistryEvents = {
    .version = PW_VERSION_REGISTRY_EVENTS,
    .global = on_registry_global,
    .global_remove = nullptr,
};

void on_core_done(void* data, std::uint32_t id, int seq) {
    auto* d = static_cast<Dumper*>(data);
    if (id == PW_ID_CORE && d->sync_seq != 0 && seq >= d->sync_seq)
        pw_main_loop_quit(d->loop);
}

constexpr pw_core_events kCoreEvents = {
    .version = PW_VERSION_CORE_EVENTS,
    .info = nullptr,
    .done = on_core_done,
    .ping = nullptr,
    .error = nullptr,
    .remove_id = nullptr,
    .bound_id = nullptr,
    .add_mem = nullptr,
    .remove_mem = nullptr,
    .bound_props = nullptr,
};

} // namespace

bool dump_node_formats(int pipewire_fd, std::uint32_t node_id, std::string& error) {
    Dumper d;
    d.want_node = node_id;

    pw_init(nullptr, nullptr);
    d.loop = pw_main_loop_new(nullptr);
    pw_context* ctx = pw_context_new(pw_main_loop_get_loop(d.loop), nullptr, 0);
    if (ctx == nullptr) {
        error = "pw_context_new failed";
        return false;
    }
    d.core = pw_context_connect_fd(ctx, pipewire_fd, nullptr, 0);
    if (d.core == nullptr) {
        error = std::format("pw_context_connect_fd failed: {}", std::strerror(errno));
        return false;
    }
    pw_core_add_listener(d.core, &d.core_listener, &kCoreEvents, &d);
    d.registry = pw_core_get_registry(d.core, PW_VERSION_REGISTRY, 0);
    pw_registry_add_listener(d.registry, &d.registry_listener, &kRegistryEvents, &d);

    // Safety net: if the node never appears, the first sync still fires and we quit.
    d.sync_seq = pw_core_sync(d.core, PW_ID_CORE, 0);

    std::printf("\n=== formats offered by node %u ===\n", node_id);
    pw_main_loop_run(d.loop);
    std::printf("  (%u EnumFormat params)\n\n", d.params_seen);

    pw_core_disconnect(d.core);
    pw_context_destroy(ctx);
    pw_main_loop_destroy(d.loop);
    pw_deinit();
    return d.params_seen > 0;
}

bool run_stream(const StreamConfig& cfg, NegotiatedFormat& out_format, std::string& error) {
    Runner r;
    r.cfg = &cfg;
    r.fmt = &out_format;

    pw_init(nullptr, nullptr);

    r.loop = pw_main_loop_new(nullptr);
    if (r.loop == nullptr) {
        error = "pw_main_loop_new failed";
        return false;
    }
    r.context = pw_context_new(pw_main_loop_get_loop(r.loop), nullptr, 0);
    if (r.context == nullptr) {
        error = "pw_context_new failed";
        return false;
    }
    // Takes ownership of the portal's fd.
    r.core = pw_context_connect_fd(r.context, cfg.pipewire_fd, nullptr, 0);
    if (r.core == nullptr) {
        error = std::format("pw_context_connect_fd failed: {}", std::strerror(errno));
        return false;
    }

    pw_properties* props =
        pw_properties_new(PW_KEY_MEDIA_TYPE, "Video", PW_KEY_MEDIA_CATEGORY, "Capture",
                          PW_KEY_MEDIA_ROLE, "Screen", PW_KEY_APP_NAME, "libertas-capture-spike",
                          nullptr);
    r.stream = pw_stream_new(r.core, "libertas-capture", props);
    if (r.stream == nullptr) {
        error = "pw_stream_new failed";
        return false;
    }
    pw_stream_add_listener(r.stream, &r.stream_listener, &kStreamEvents, &r);

    // Advertise, in preference order: each format with our modifier list, then the shm fallback.
    static constexpr std::array<std::uint32_t, 4> kFormats{
        SPA_VIDEO_FORMAT_BGRA, SPA_VIDEO_FORMAT_BGRx, SPA_VIDEO_FORMAT_RGBA,
        SPA_VIDEO_FORMAT_RGBx};

    std::array<std::uint8_t, 8192> pod_buf{};
    spa_pod_builder b =
        SPA_POD_BUILDER_INIT(pod_buf.data(), static_cast<std::uint32_t>(pod_buf.size()));
    std::vector<const spa_pod*> params;

    if (cfg.mirror_width > 0 && cfg.mirror_height > 0) {
        const spa_rectangle sz{cfg.mirror_width, cfg.mirror_height};
        if (!cfg.modifiers.empty())
            params.push_back(build_format_mirror(&b, SPA_VIDEO_FORMAT_BGRA, sz, true,
                                                 cfg.modifiers.front()));
        else
            params.push_back(build_format_mirror(&b, SPA_VIDEO_FORMAT_BGRA, sz, false, 0));
    } else {
        if (!cfg.modifiers.empty())
            for (std::uint32_t f : kFormats)
                params.push_back(build_format_with_modifiers(&b, f, cfg.modifiers));
        params.push_back(build_format_shm(&b));
    }

    lc::info("connecting to node {} advertising {} format params ({} modifiers)", cfg.node_id,
             params.size(), cfg.modifiers.size());

    const int res = pw_stream_connect(
        r.stream, SPA_DIRECTION_INPUT, cfg.node_id,
        static_cast<pw_stream_flags>(PW_STREAM_FLAG_AUTOCONNECT),
        params.data(), static_cast<std::uint32_t>(params.size()));
    if (res < 0) {
        error = std::format("pw_stream_connect failed: {}", spa_strerror(res));
        return false;
    }

    pw_main_loop_run(r.loop);

    pw_stream_destroy(r.stream);
    pw_core_disconnect(r.core);
    pw_context_destroy(r.context);
    pw_main_loop_destroy(r.loop);
    pw_deinit();

    if (!r.error.empty()) {
        error = r.error;
        return false;
    }
    if (r.frames == 0) {
        error = "no frames received";
        return false;
    }
    return true;
}

} // namespace libertas::spike
