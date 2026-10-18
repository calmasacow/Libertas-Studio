// SPDX-FileCopyrightText: Libertas-Studio contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include "lcomp/preview.hpp"

#include "ldoc/json.hpp"

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <format>
#include <limits>
#include <span>
#include <spawn.h>
#include <sys/stat.h>
#include <unistd.h>
#include <utility>

extern char** environ;

namespace lcomp {
namespace {

enum class Kind : std::uint8_t {
    Hello = 1,
    Project = 2,
    Scene = 3,
    Commit = 4,
    Quit = 5,
    Frame = 6,
    Ack = 7,
    Fault = 8,
};

constexpr std::size_t kMaxDocument = 32u * 1024u * 1024u;

void append_u16(std::vector<std::byte>& out, std::uint16_t v) {
    out.push_back(static_cast<std::byte>(v & 0xffu));
    out.push_back(static_cast<std::byte>((v >> 8) & 0xffu));
}

void append_u32(std::vector<std::byte>& out, std::uint32_t v) {
    for (int shift = 0; shift < 32; shift += 8)
        out.push_back(static_cast<std::byte>((v >> shift) & 0xffu));
}

[[nodiscard]] bool take_u16(std::span<const std::byte> in, std::size_t& at, std::uint16_t& v) {
    if (at + 2 > in.size())
        return false;
    v = static_cast<std::uint16_t>(std::to_integer<unsigned>(in[at]) |
                                   (std::to_integer<unsigned>(in[at + 1]) << 8));
    at += 2;
    return true;
}

[[nodiscard]] bool take_u32(std::span<const std::byte> in, std::size_t& at, std::uint32_t& v) {
    if (at + 4 > in.size())
        return false;
    v = 0;
    for (int shift = 0; shift < 32; shift += 8)
        v |= static_cast<std::uint32_t>(std::to_integer<unsigned>(in[at++])) << shift;
    return true;
}

[[nodiscard]] std::expected<std::string, std::string> read_memfd(const lipc::Fd& fd,
                                                                 std::uint32_t claimed) {
    if (!fd.valid())
        return std::unexpected(std::string{"the document arrived without its buffer"});
    if (claimed > kMaxDocument)
        return std::unexpected(std::string{"a document larger than 32MB was refused"});
    struct stat st{};
    if (::fstat(fd.get(), &st) != 0)
        return std::unexpected(std::format("fstat: {}", std::strerror(errno)));
    if (st.st_size < 0 || static_cast<std::uint64_t>(st.st_size) < claimed)
        return std::unexpected(std::string{"the buffer is shorter than the message claimed"});
    if (claimed == 0)
        return std::string{};
    void* view = ::mmap(nullptr, claimed, PROT_READ, MAP_SHARED, fd.get(), 0);
    if (view == MAP_FAILED)
        return std::unexpected(std::format("mmap: {}", std::strerror(errno)));
    std::string text{static_cast<const char*>(view), claimed};
    ::munmap(view, claimed);
    return text;
}

[[nodiscard]] std::expected<lipc::Fd, std::string> memfd_containing(const char* name,
                                                                    std::string_view bytes) {
    const std::size_t n = bytes.empty() ? 1 : bytes.size();
    auto fd = lipc::make_memfd(name, n);
    if (!fd)
        return std::unexpected(fd.error());
    if (bytes.empty())
        return std::move(*fd);
    void* view = ::mmap(nullptr, bytes.size(), PROT_READ | PROT_WRITE, MAP_SHARED, fd->get(), 0);
    if (view == MAP_FAILED)
        return std::unexpected(std::format("mmap: {}", std::strerror(errno)));
    std::memcpy(view, bytes.data(), bytes.size());
    ::munmap(view, bytes.size());
    return std::move(*fd);
}

[[nodiscard]] std::expected<void, std::string> write_memfd(const lipc::Fd& fd,
                                                           std::span<const std::uint8_t> bytes) {
    if (bytes.empty())
        return {};
    void* view = ::mmap(nullptr, bytes.size(), PROT_READ | PROT_WRITE, MAP_SHARED, fd.get(), 0);
    if (view == MAP_FAILED)
        return std::unexpected(std::format("mmap: {}", std::strerror(errno)));
    std::memcpy(view, bytes.data(), bytes.size());
    ::munmap(view, bytes.size());
    return {};
}

[[nodiscard]] const lipc::Fd* only_fd(const lipc::Message& message) {
    return message.fds.size() == 1 ? &message.fds.front() : nullptr;
}

struct Assembly {
    std::optional<ldoc::Project> project;
    std::string error;

    void reset() {
        project.reset();
        error.clear();
    }
};

[[nodiscard]] std::expected<void, std::string> send_fault(lipc::Channel& channel,
                                                          std::string_view text) {
    std::vector<std::byte> payload;
    payload.push_back(static_cast<std::byte>(Kind::Fault));
    const std::size_t take = std::min(text.size(), std::size_t{1024});
    payload.reserve(1 + take);
    for (std::size_t i = 0; i < take; ++i)
        payload.push_back(static_cast<std::byte>(text[i]));
    return channel.send(payload);
}

[[nodiscard]] std::expected<void, std::string> send_ack(lipc::Channel& channel) {
    const std::byte payload[] = {static_cast<std::byte>(Kind::Ack)};
    return channel.send(payload);
}

struct Slot {
    lipc::Fd fd;
    std::int32_t width = 0;
    std::int32_t height = 0;
};

[[nodiscard]] std::expected<void, std::string>
send_frame(lipc::Channel& channel, const ldoc::Id& canvas, const Picture& picture,
           std::vector<std::pair<ldoc::Id, Slot>>& slots) {
    const std::string id = canvas.to_string();
    if (id.size() != lc::kUlidLength)
        return std::unexpected(std::string{"a canvas id was not 26 characters"});

    const bool have_pixels = picture.stride > 0 && !picture.pixels.empty() &&
                             picture.pixels.size() == static_cast<std::size_t>(picture.stride) *
                                                          static_cast<std::size_t>(picture.height);

    bool fresh = false;
    Slot* slot = nullptr;
    if (have_pixels) {
        for (auto& [key, existing] : slots) {
            if (key == canvas) {
                slot = &existing;
                break;
            }
        }
        if (slot == nullptr || slot->width != picture.width || slot->height != picture.height) {
            const std::string name = "preview-" + id;
            auto created = lipc::make_memfd(name.c_str(), picture.pixels.size());
            if (!created)
                return std::unexpected(created.error());
            if (slot == nullptr) {
                slots.push_back({canvas, Slot{}});
                slot = &slots.back().second;
            }
            slot->fd = std::move(*created);
            slot->width = picture.width;
            slot->height = picture.height;
            fresh = true;
        }
        if (auto wrote = write_memfd(slot->fd, picture.pixels); !wrote)
            return std::unexpected(wrote.error());
    }

    std::vector<std::byte> payload;
    payload.reserve(1 + lc::kUlidLength + 4 + 4 + 4 + 1 + 4);
    payload.push_back(static_cast<std::byte>(Kind::Frame));
    for (char c : id)
        payload.push_back(static_cast<std::byte>(c));
    const auto wire_w =
        picture.width > 0 ? static_cast<std::uint32_t>(picture.width) : std::uint32_t{0};
    const auto wire_h =
        picture.height > 0 ? static_cast<std::uint32_t>(picture.height) : std::uint32_t{0};
    append_u32(payload, wire_w);
    append_u32(payload, wire_h);
    append_u32(payload, have_pixels ? picture.stride : 0u);
    payload.push_back(static_cast<std::byte>(picture.status));
    append_u32(payload, static_cast<std::uint32_t>(picture.gaps.size()));

    if (fresh) {
        const int raw = slot->fd.get();
        return channel.send(payload, std::span<const int>(&raw, 1));
    }
    return channel.send(payload);
}

} // namespace

bool send_would_block(const std::string& error) {
    return error.find("temporarily unavailable") != std::string::npos ||
           error.find("would block") != std::string::npos;
}

PreviewLink::PreviewLink(lipc::Channel channel) : channel_(std::move(channel)) {}

PreviewLink::~PreviewLink() = default;

std::expected<void, std::string> PreviewLink::take_hello(int timeout_ms) {
    auto event = receive(timeout_ms);
    if (!event)
        return std::unexpected(event.error());
    if (!event->has_value())
        return std::unexpected(std::string{"the renderer did not say hello"});
    if ((*event)->kind == PreviewEvent::Kind::PeerGone)
        return std::unexpected(std::string{"the renderer exited before saying hello"});
    if ((*event)->kind == PreviewEvent::Kind::Fault)
        return std::unexpected((*event)->fault);
    if ((*event)->kind != PreviewEvent::Kind::Hello)
        return std::unexpected(
            std::string{"the renderer spoke first with something other than hello"});
    return {};
}

std::expected<void, std::string> PreviewLink::send_project(const ldoc::Project& project) {
    // Serialise once. The length in the message has to be the length of the bytes in the
    // memfd, and a second call would be a second document the two could disagree about.
    const std::string project_bytes = ldoc::project_to_json(project);
    if (project_bytes.size() > std::numeric_limits<std::uint32_t>::max())
        return std::unexpected(std::string{"the project file does not fit in a length"});
    auto project_fd = memfd_containing("doc-project", project_bytes);
    if (!project_fd)
        return std::unexpected(project_fd.error());

    struct SceneBuffer {
        std::string bytes;
        lipc::Fd fd;
    };

    std::vector<SceneBuffer> scenes;
    scenes.reserve(project.scenes.size());
    for (const ldoc::Scene& scene : project.scenes) {
        SceneBuffer buffer;
        buffer.bytes = ldoc::scene_to_json(scene);
        if (buffer.bytes.size() > std::numeric_limits<std::uint32_t>::max())
            return std::unexpected(std::string{"a scene file does not fit in a length"});
        auto fd = memfd_containing("doc-scene", buffer.bytes);
        if (!fd)
            return std::unexpected(fd.error());
        buffer.fd = std::move(*fd);
        scenes.push_back(std::move(buffer));
    }

    const auto send_doc = [this](Kind kind, std::uint32_t length, const lipc::Fd& fd) {
        std::vector<std::byte> payload;
        payload.push_back(static_cast<std::byte>(kind));
        append_u32(payload, length);
        const int raw = fd.get();
        return channel_.send(payload, std::span<const int>(&raw, 1));
    };

    if (auto sent =
            send_doc(Kind::Project, static_cast<std::uint32_t>(project_bytes.size()), *project_fd);
        !sent)
        return sent;
    for (const SceneBuffer& scene : scenes) {
        if (auto sent =
                send_doc(Kind::Scene, static_cast<std::uint32_t>(scene.bytes.size()), scene.fd);
            !sent)
            return sent;
    }
    const std::byte commit[] = {static_cast<std::byte>(Kind::Commit)};
    return channel_.send(commit);
}

void PreviewLink::quit() {
    if (!channel_.valid())
        return;
    const std::byte payload[] = {static_cast<std::byte>(Kind::Quit)};
    (void)channel_.send(payload);
}

std::expected<std::optional<PreviewEvent>, std::string> PreviewLink::poll() {
    auto message = channel_.receive();
    if (!message) {
        if (message.error() == lipc::Empty::WouldBlock)
            return std::optional<PreviewEvent>{};
        PreviewEvent gone;
        gone.kind = PreviewEvent::Kind::PeerGone;
        return gone;
    }
    return decode(std::move(*message));
}

std::expected<std::optional<PreviewEvent>, std::string> PreviewLink::receive(int timeout_ms) {
    auto ready = channel_.wait(timeout_ms);
    if (!ready)
        return std::unexpected(ready.error());
    if (!*ready)
        return std::optional<PreviewEvent>{};
    return poll();
}

std::expected<PreviewEvent, std::string> PreviewLink::decode(lipc::Message message) {
    if (message.payload.empty())
        return std::unexpected(std::string{"the renderer sent an empty message"});
    const auto kind = static_cast<Kind>(std::to_integer<unsigned>(message.payload[0]));
    const std::span<const std::byte> payload = message.payload;

    if (kind == Kind::Ack) {
        PreviewEvent event;
        event.kind = PreviewEvent::Kind::Ack;
        return event;
    }
    if (kind == Kind::Fault) {
        PreviewEvent event;
        event.kind = PreviewEvent::Kind::Fault;
        event.fault.assign(reinterpret_cast<const char*>(payload.data() + 1), payload.size() - 1);
        return event;
    }
    if (kind == Kind::Hello) {
        std::size_t at = 1;
        std::uint16_t version = 0;
        if (!take_u16(payload, at, version))
            return std::unexpected(std::string{"the hello was cut short"});
        if (version != kPreviewProtocol)
            return std::unexpected(
                std::format("the renderer speaks preview protocol {}, and this build speaks {}",
                            version, kPreviewProtocol));
        PreviewEvent event;
        event.kind = PreviewEvent::Kind::Hello;
        return event;
    }
    if (kind != Kind::Frame)
        return std::unexpected(std::string{"the renderer sent a message this build does not know"});

    std::size_t at = 1;
    if (at + lc::kUlidLength > payload.size())
        return std::unexpected(std::string{"a frame arrived without its canvas"});
    const std::string id{reinterpret_cast<const char*>(payload.data() + at), lc::kUlidLength};
    at += lc::kUlidLength;
    const auto canvas = ldoc::Id::parse(id);
    if (!canvas)
        return std::unexpected(std::string{"a frame named a canvas that is not an id"});

    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::uint32_t stride = 0;
    if (!take_u32(payload, at, width) || !take_u32(payload, at, height) ||
        !take_u32(payload, at, stride))
        return std::unexpected(std::string{"a frame's size was cut short"});
    if (at >= payload.size())
        return std::unexpected(std::string{"a frame arrived without its status"});
    const auto status_byte = std::to_integer<unsigned>(payload[at++]);
    if (status_byte > static_cast<unsigned>(PictureStatus::Unsupported))
        return std::unexpected(
            std::string{"a frame arrived with a status this build does not know"});
    std::uint32_t gaps = 0;
    if (!take_u32(payload, at, gaps))
        return std::unexpected(std::string{"a frame's gap count was cut short"});

    if (width > static_cast<std::uint32_t>(std::numeric_limits<std::int32_t>::max()) ||
        height > static_cast<std::uint32_t>(std::numeric_limits<std::int32_t>::max()))
        return std::unexpected(std::string{"a frame's dimension does not fit"});

    PreviewEvent event;
    event.kind = PreviewEvent::Kind::Frame;
    event.frame.canvas = *canvas;
    event.frame.width = static_cast<std::int32_t>(width);
    event.frame.height = static_cast<std::int32_t>(height);
    event.frame.stride = stride;
    event.frame.status = static_cast<PictureStatus>(status_byte);
    event.frame.gaps = gaps;

    if (stride == 0 || width == 0 || height == 0)
        return event;

    if (width > static_cast<std::uint32_t>(kMaxPreviewEdge) ||
        height > static_cast<std::uint32_t>(kMaxPreviewEdge))
        return std::unexpected(std::string{"a frame is larger than this preview will map"});
    if (stride < width * 4u || height > (std::numeric_limits<std::size_t>::max() / stride))
        return std::unexpected(std::string{"a frame's stride does not fit its width"});
    const std::size_t need = static_cast<std::size_t>(stride) * static_cast<std::size_t>(height);

    Mapping* mapping = nullptr;
    for (auto& [key, existing] : mappings_) {
        if (key == id) {
            mapping = &existing;
            break;
        }
    }
    if (!message.fds.empty()) {
        if (message.fds.size() != 1)
            return std::unexpected(std::string{"a frame carried more than one buffer"});
        Mapping fresh;
        fresh.fd = std::move(message.fds.front());
        struct stat st{};
        if (::fstat(fresh.fd.get(), &st) != 0)
            return std::unexpected(std::format("fstat: {}", std::strerror(errno)));
        if (st.st_size < 0 || static_cast<std::uint64_t>(st.st_size) < need)
            return std::unexpected(std::string{"the frame buffer is shorter than the frame"});
        fresh.view = ::mmap(nullptr, need, PROT_READ, MAP_SHARED, fresh.fd.get(), 0);
        if (fresh.view == MAP_FAILED)
            return std::unexpected(std::format("mmap: {}", std::strerror(errno)));
        fresh.bytes = need;
        if (mapping == nullptr) {
            mappings_.push_back({id, std::move(fresh)});
            mapping = &mappings_.back().second;
        } else {
            *mapping = std::move(fresh);
        }
    }
    if (mapping == nullptr || mapping->view == nullptr || mapping->bytes < need)
        return std::unexpected(std::string{"a frame arrived before its buffer"});

    event.frame.pixels.resize(need);
    std::memcpy(event.frame.pixels.data(), mapping->view, need);
    return event;
}

std::expected<void, std::string> serve_preview(lipc::Channel channel) {
    std::vector<std::byte> hello;
    hello.push_back(static_cast<std::byte>(Kind::Hello));
    append_u16(hello, kPreviewProtocol);
    if (auto sent = channel.send(hello); !sent)
        return sent;

    Assembly assembly;
    std::vector<std::pair<ldoc::Id, Slot>> slots;

    for (;;) {
        auto ready = channel.wait(-1);
        if (!ready)
            return std::unexpected(ready.error());
        for (;;) {
            auto message = channel.receive();
            if (!message) {
                if (message.error() == lipc::Empty::PeerGone)
                    return {};
                break;
            }

            const std::span<const std::byte> payload = message->payload;
            if (payload.empty())
                continue;
            const auto kind = static_cast<Kind>(std::to_integer<unsigned>(payload[0]));
            if (kind == Kind::Quit)
                return {};

            if (kind == Kind::Project || kind == Kind::Scene) {
                std::size_t at = 1;
                std::uint32_t length = 0;
                if (!take_u32(payload, at, length)) {
                    assembly.error = "a document message was cut short";
                    continue;
                }
                const lipc::Fd* fd = only_fd(*message);
                if (fd == nullptr) {
                    assembly.error = "a document message arrived without its buffer";
                    continue;
                }
                auto text = read_memfd(*fd, length);
                if (!text) {
                    assembly.error = text.error();
                    continue;
                }
                if (kind == Kind::Project) {
                    assembly.reset();
                    auto parsed = ldoc::project_from_json(*text);
                    if (!parsed)
                        assembly.error = parsed.error();
                    else
                        assembly.project = std::move(parsed->project);
                } else if (assembly.project && assembly.error.empty()) {
                    auto parsed = ldoc::scene_from_json(*text);
                    if (!parsed)
                        assembly.error = parsed.error();
                    else
                        assembly.project->scenes.push_back(std::move(parsed->scene));
                }
                continue;
            }

            if (kind != Kind::Commit)
                continue;

            if (!assembly.project || !assembly.error.empty()) {
                const std::string why =
                    assembly.error.empty() ? "a commit arrived before its project" : assembly.error;
                if (auto sent = send_fault(channel, why); !sent)
                    return sent;
                if (auto sent = send_ack(channel); !sent)
                    return sent;
                continue;
            }

            for (const ldoc::Canvas& canvas : assembly.project->canvases) {
                const Picture picture = composite_canvas(*assembly.project, canvas.id);
                if (auto sent = send_frame(channel, canvas.id, picture, slots); !sent)
                    return sent;
            }
            if (auto sent = send_ack(channel); !sent)
                return sent;
        }
    }
}

std::expected<pid_t, std::string> spawn_renderer(const std::filesystem::path& exe,
                                                 lipc::Fd child_end) {
    if (!child_end.valid())
        return std::unexpected(std::string{"there is no channel to hand the renderer"});
    if (exe.empty())
        return std::unexpected(std::string{"there is no renderer to start"});

    posix_spawn_file_actions_t actions;
    if (const int rc = ::posix_spawn_file_actions_init(&actions); rc != 0)
        return std::unexpected(std::format("posix_spawn_file_actions_init: {}", std::strerror(rc)));

    struct Actions {
        posix_spawn_file_actions_t* value;

        ~Actions() { ::posix_spawn_file_actions_destroy(value); }
    } guard{&actions};

    if (const int rc = ::posix_spawn_file_actions_adddup2(&actions, child_end.get(), 3); rc != 0)
        return std::unexpected(std::format("dup2: {}", std::strerror(rc)));
    if (const int rc = ::posix_spawn_file_actions_addclosefrom_np(&actions, 4); rc != 0)
        return std::unexpected(std::format("closefrom: {}", std::strerror(rc)));

    std::string path = exe.string();
    std::vector<std::string> owned;
    for (char** entry = environ; entry != nullptr && *entry != nullptr; ++entry) {
        const std::string_view item{*entry};
        if (item.starts_with("LIBERTAS_CHANNEL_FD="))
            continue;
        owned.emplace_back(*entry);
    }
    owned.emplace_back("LIBERTAS_CHANNEL_FD=3");
    std::vector<char*> envp;
    envp.reserve(owned.size() + 1);
    for (std::string& item : owned)
        envp.push_back(item.data());
    envp.push_back(nullptr);
    char* argv[] = {path.data(), nullptr};

    pid_t pid = 0;
    const int rc = ::posix_spawn(&pid, path.c_str(), &actions, nullptr, argv, envp.data());
    if (rc != 0)
        return std::unexpected(std::format("could not start the renderer: {}", std::strerror(rc)));
    return pid;
}

} // namespace lcomp
