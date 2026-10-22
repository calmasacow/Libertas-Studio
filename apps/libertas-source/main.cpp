// SPDX-FileCopyrightText: Libertas-Studio contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#include "alpha_video.hpp"
#include "decklink.hpp"

#include <QApplication>
#include <QAudioBuffer>
#include <QAudioBufferOutput>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QFont>
#include <QImageReader>
#include <QJsonArray>
#include <QJsonDocument>
#include <QMediaMetaData>
#include <QMediaPlayer>
#include <QMovie>
#include <QPainter>
#include <QTimer>
#include <QVideoFrame>
#include <QVideoSink>
#include <QWebEngineScript>
#include <QWebEngineScriptCollection>
#include <QWebEngineView>
#include <atomic>
#include <charconv>
#include <cstring>
#include <lcapture/audio.hpp>
#include <lcomp/preview.hpp>
#include <mutex>
#include <thread>
#include <unistd.h>

namespace {
class Content : public QObject {
    ldoc::Source source_;
    QSize size_;
    QImage image_;
    std::unique_ptr<QMediaPlayer> player_;
    std::unique_ptr<AlphaVideo> alpha_;
    QElapsedTimer media_position_clock_;
    qint64 media_position_ = 0;
    bool active_ = true;
    std::unique_ptr<QMovie> movie_;
    std::unique_ptr<QAudioBufferOutput> audio_;
    std::vector<float> audio_pending_;
    std::mutex audio_mu_;
    std::unique_ptr<libertas::capture::AudioSink> browser_sink_;
    std::unique_ptr<libertas::capture::AudioCapture> browser_audio_;
    std::unique_ptr<Decklink> decklink_;
    std::atomic<bool> vendor_frame_pending_{false};
    std::uint64_t generation_ = 0;
    std::unique_ptr<QVideoSink> video_;
    QTimer slides_;
    QTimer web_frames_;
    std::unique_ptr<QWebEngineView> web_;
    QStringList files_;
    qsizetype index_ = 0;
    bool valid_ = false;

    void append_audio(std::span<const float> samples) {
        std::lock_guard lock(audio_mu_);
        audio_pending_.insert(audio_pending_.end(), samples.begin(), samples.end());
        if (audio_pending_.size() > 9600)
            audio_pending_.erase(audio_pending_.begin(), audio_pending_.end() - 9600);
    }

    std::string text(const char* key, const char* fallback = "") const {
        auto it = source_.settings.find(key);
        if (it != source_.settings.end())
            if (const auto* value = std::get_if<std::string>(&it->second))
                return *value;
        return fallback;
    }

    int number(const char* key, int fallback) const {
        auto it = source_.settings.find(key);
        if (it != source_.settings.end())
            if (const auto* value = std::get_if<std::int64_t>(&it->second))
                return static_cast<int>(
                    std::clamp(*value, std::int64_t{-100000}, std::int64_t{100000}));
        return fallback;
    }

    bool flag(const char* key, bool fallback) const {
        auto it = source_.settings.find(key);
        if (it != source_.settings.end())
            if (const auto* value = std::get_if<bool>(&it->second))
                return *value;
        return fallback;
    }

    void publish(const QImage& image) {
        valid_ = !image.isNull();
        if (valid_ && !active_ && flag("restartOnActivate", true)) {
            if (player_) player_->pause();
            if (alpha_) alpha_->pause();
            if (movie_) movie_->setPaused(true);
        }
        if (valid_ &&
            (image.width() > 8192 || image.height() > 8192 ||
             static_cast<std::int64_t>(image.width()) * image.height() * 4 > 128 * 1024 * 1024)) {
            qWarning("source frame exceeds image budget");
            valid_ = false;
        }
        if (valid_ && (image.size() == size_ || source_.type == "source.media" ||
                       source_.type == "source.playlist")) {
            image_ = image.convertToFormat(QImage::Format_RGBA8888);
            dirty = true;
            return;
        }
        image_ = QImage(size_, QImage::Format_RGBA8888);
        image_.fill(Qt::transparent);
        if (valid_) {
            QPainter painter(&image_);
            painter.setRenderHint(QPainter::SmoothPixmapTransform);
            const auto fit = image.size().scaled(size_, Qt::KeepAspectRatio);
            painter.drawImage(QRect(QPoint((size_.width() - fit.width()) / 2,
                                           (size_.height() - fit.height()) / 2),
                                    fit),
                              image);
        }
        dirty = true;
    }

    void show_slide() {
        if (files_.empty()) {
            publish({});
            return;
        }
        QImageReader reader(files_[index_]);
        const QSize original = reader.size();
        if (!original.isValid() || original.width() > 8192 || original.height() > 8192) {
            publish({});
            return;
        }
        reader.setAutoTransform(true);
        publish(reader.read());
    }

    void media_finished() {
        if (index_ + 1 < files_.size()) {
            ++index_;
            play_file();
        } else if (flag("loop", true)) {
            index_ = 0;
            play_file();
        } else if (flag("clearOnEnd", true))
            publish({});
    }

    qint64 media_position() {
        const auto position = player_->position();
        if (!player_->isPlaying()) {
            media_position_clock_.invalidate();
            return position;
        }
        // Qt publishes position in coarse steps. Interpolate between them so a
        // 30/60 Hz alpha stream does not arrive in 50 ms bursts.
        if (!media_position_clock_.isValid() || position != media_position_) {
            media_position_ = position;
            media_position_clock_.start();
        }
        return position + static_cast<qint64>(static_cast<double>(media_position_clock_.elapsed()) *
                                              player_->playbackRate());
    }

    void play_file() try {
        media_position_clock_.invalidate();
        alpha_.reset();
        movie_.reset();
        if (player_)
            player_->stop();
        if (files_.empty()) {
            publish({});
            return;
        }
        const auto path = files_[index_];
        const auto suffix = QFileInfo(path).suffix().toLower();
        const auto generation = generation_;
        const auto finished = [this, generation] {
            QTimer::singleShot(0, this, [this, generation] {
                if (generation == generation_)
                    media_finished();
            });
        };
        if (suffix == "webp") {
            QImageReader reader(path);
            if (!reader.supportsAnimation() || reader.imageCount() == 1) {
                publish(reader.read());
                return;
            }
            movie_ = std::make_unique<QMovie>(path);
            if (!movie_->isValid()) {
                qWarning("media source: cannot decode WebP");
                publish({});
                return;
            }
            movie_->setSpeed(std::clamp(number("speedPercent", 100), 25, 400));
            connect(movie_.get(), &QMovie::frameChanged, this,
                    [this, first = true, finished](int frame) mutable {
                        if (!first && frame == 0 && !flag("loop", true)) {
                            movie_->stop();
                            finished();
                            return;
                        }
                        first = false;
                        publish(movie_->currentImage());
                    });
            connect(movie_.get(), &QMovie::finished, this, finished);
            connect(movie_.get(), &QMovie::error, this, [this](auto) {
                qWarning("media source: %s", qPrintable(movie_->lastErrorString()));
                movie_->stop();
                publish({});
            });
            movie_->start();
            return;
        }
        if (suffix == "webm") {
            auto alpha = std::make_unique<AlphaVideo>();
            if (alpha->open(path))
                alpha_ = std::move(alpha);
        }
        const double speed = std::clamp(number("speedPercent", 100), 25, 400) / 100.0;
        if (alpha_ && !alpha_->has_audio()) {
            // Silent alpha needs a clock, not a second media pipeline/device probe.
            media_position_clock_.start();
            alpha_->start(
                speed,
                [this, speed] {
                    return static_cast<qint64>(
                        static_cast<double>(media_position_clock_.elapsed()) * speed);
                },
                [this](const QImage& image) { publish(image); }, finished);
            return;
        }
        ensure_player();
        player_->setPlaybackRate(speed);
        player_->setVideoSink(alpha_ ? nullptr : video_.get());
        player_->setSource(QUrl::fromLocalFile(path));
        player_->play();
        if (alpha_)
            alpha_->start(
                player_->playbackRate(), [this] { return media_position(); },
                [this](const QImage& image) { publish(image); }, finished);
    } catch (const std::exception& error) {
        qWarning("media source: %s", error.what());
        alpha_.reset();
        movie_.reset();
        publish({});
    }

    void ensure_player() {
        if (player_)
            return;
        // Qt caches this policy per process; settings changes restart the worker.
        if (!flag("hardwareDecode", true))
            qputenv("QT_FFMPEG_DECODING_HW_DEVICE_TYPES", QByteArray{});
        // QMediaPlayer probes hardware decoders; text and still-image workers need none.
        player_ = std::make_unique<QMediaPlayer>();
        video_ = std::make_unique<QVideoSink>();
        QAudioFormat format;
        format.setSampleRate(48000);
        format.setChannelCount(2);
        format.setSampleFormat(QAudioFormat::Float);
        audio_ = std::make_unique<QAudioBufferOutput>(format);
        player_->setAudioBufferOutput(audio_.get());
        connect(player_.get(), &QMediaPlayer::tracksChanged, this, [this] {
            player_->setActiveVideoTrack(alpha_ || player_->videoTracks().isEmpty() ? -1 : 0);
        });
        connect(
            audio_.get(), &QAudioBufferOutput::audioBufferReceived, this,
            [this](const QAudioBuffer& buffer) {
                if (!buffer.isValid() || buffer.format().sampleFormat() != QAudioFormat::Float ||
                    buffer.format().channelCount() != 2 || buffer.format().sampleRate() != 48000)
                    return;
                const auto* samples = buffer.constData<float>();
                const auto count = static_cast<std::size_t>(buffer.sampleCount());
                append_audio({samples, count});
            });
        player_->setVideoSink(video_.get());
        connect(video_.get(), &QVideoSink::videoFrameChanged, this, [this](const QVideoFrame& frame) {
            if (frame.isValid())
                publish(frame.toImage());
        });
        connect(player_.get(), &QMediaPlayer::errorOccurred, this,
                [this](auto, const QString& error) {
                    qWarning("media source: %s", qPrintable(error));
                    publish({});
                });
        connect(player_.get(), &QMediaPlayer::mediaStatusChanged, this, [this](auto status) {
            if (status == QMediaPlayer::EndOfMedia && !alpha_ && !movie_)
                media_finished();
        });
    }

  public:
    std::atomic<bool> dirty{true};

    Content() {
        connect(&slides_, &QTimer::timeout, this, [this] {
            if (files_.empty())
                return;
            if (index_ + 1 < files_.size())
                ++index_;
            else if (flag("loop", true))
                index_ = 0;
            else {
                slides_.stop();
                return;
            }
            show_slide();
        });
    }

    ~Content() override {
        decklink_.reset();
        web_.reset();
        browser_audio_.reset();
        browser_sink_.reset();
    }

    void activate(bool active) {
        if (active_ == active) return;
        active_ = active;
        if (!flag("restartOnActivate", true)) return;
        if (active) {
            media_position_clock_.start();
            if (player_) player_->play();
            if (alpha_) alpha_->resume();
            if (movie_) movie_->setPaused(false);
        } else {
            media_position_clock_.start();
            if (player_) { player_->pause(); player_->setPosition(0); }
            if (alpha_) alpha_->rewind();
            if (movie_) { movie_->jumpToFrame(0); movie_->setPaused(true); }
        }
    }

    void configure(ldoc::Source source) {
        bool active = true;
        if (auto it = source.settings.find("worker.active"); it != source.settings.end()) {
            if (const auto* value = std::get_if<bool>(&it->second)) active = *value;
            source.settings.erase(it);
        }
        if (source_.id == source.id && source_.type == source.type &&
            source_.settings == source.settings) {
            activate(active);
            return;
        }
        active_ = active;
        slides_.stop();
        alpha_.reset();
        movie_.reset();
        decklink_.reset();
        ++generation_;
        web_frames_.stop();
        web_.reset();
        browser_audio_.reset();
        browser_sink_.reset();
        if (player_)
            player_->stop();
        {
            std::lock_guard lock(audio_mu_);
            audio_pending_.clear();
        }
        source_ = source;
        size_ = QSize(number("width", 1920), number("height", 1080));
        if (size_.width() < 1 || size_.height() < 1 || size_.width() > 4096 ||
            size_.height() > 4096)
            throw std::runtime_error("source dimensions must be between 1 and 4096");
        index_ = 0;
        publish({});
        if (source.type == "source.text") {
            QImage image(size_, QImage::Format_RGBA8888);
            image.fill(Qt::transparent);
            QPainter painter(&image);
            QFont font(QString::fromStdString(text("font", "Sans")));
            font.setPixelSize(std::clamp(number("size", 48), 1, 1024));
            painter.setFont(font);
            QColor colour(Qt::white);
            auto it = source.settings.find("color");
            if (it != source.settings.end())
                if (auto rgba = std::get_if<ldoc::Rgba>(&it->second)) {
                    auto bytes = rgba->to_srgb8();
                    colour = QColor(bytes[0], bytes[1], bytes[2], bytes[3]);
                }
            painter.setPen(colour);
            const auto align = text("alignment", "Left");
            const int flags = Qt::TextWordWrap | Qt::AlignTop |
                              (align == "Right"    ? Qt::AlignRight
                               : align == "Centre" ? Qt::AlignHCenter
                                                   : Qt::AlignLeft);
            painter.drawText(image.rect(), flags, QString::fromStdString(text("text")));
            painter.end();
            publish(image);
        } else if (source.type == "source.blackmagic") {
            std::string error;
            const auto generation = generation_;
            decklink_ = Decklink::open(
                number("deviceIndex", 0), text("mode", "HD 1080p60"),
                [this, generation](QImage image) {
                    if (vendor_frame_pending_.exchange(true))
                        return;
                    QMetaObject::invokeMethod(
                        this,
                        [this, generation, image = std::move(image)] {
                            if (generation == generation_)
                                publish(image);
                            vendor_frame_pending_ = false;
                        },
                        Qt::QueuedConnection);
                },
                [this](std::span<const float> pcm) { append_audio(pcm); }, error);
            if (!decklink_)
                throw std::runtime_error(error);
        } else if (source.type == "source.browser") {
            const QUrl url(QString::fromStdString(text("url")));
            if (!url.isValid() ||
                (url.scheme() != "https" && url.scheme() != "http" && url.scheme() != "file"))
                throw std::runtime_error("browser URL must use http, https or file");
            const std::string sink = "libertas_browser_" + std::to_string(getpid());
            std::string audio_error;
            browser_sink_ = libertas::capture::AudioSink::open(sink, audio_error);
            if (browser_sink_) {
                qputenv("PULSE_SINK", QByteArray::fromStdString(sink));
                browser_audio_ = libertas::capture::AudioCapture::open(
                    sink + ".monitor", false, [this](auto pcm) { append_audio(pcm); }, audio_error);
            }
            if (!browser_audio_)
                qWarning("browser audio: %s", audio_error.c_str());
            web_ = std::make_unique<QWebEngineView>();
            web_->setAttribute(Qt::WA_DontShowOnScreen);
            web_->resize(size_);
            web_->page()->setBackgroundColor(flag("transparentBackground", true) ? Qt::transparent
                                                                                 : Qt::black);
            web_->page()->setAudioMuted(!browser_audio_);
            QWebEngineScript css;
            css.setName("source stylesheet");
            css.setInjectionPoint(QWebEngineScript::DocumentReady);
            const QByteArray quoted = QJsonDocument(QJsonArray{QString::fromStdString(text("css"))})
                                          .toJson(QJsonDocument::Compact);
            css.setSourceCode(QStringLiteral("{const "
                                             "s=document.createElement('style');s.textContent=%1[0]"
                                             ";document.head.appendChild(s);}")
                                  .arg(QString::fromUtf8(quoted)));
            web_->page()->scripts().insert(css);
            connect(web_.get(), &QWebEngineView::loadFinished, this, [this](bool ok) {
                if (ok)
                    web_frames_.start(1000 / std::clamp(number("fps", 30), 1, 60));
                else {
                    web_frames_.stop();
                    publish({});
                }
            });
            connect(&web_frames_, &QTimer::timeout, web_.get(),
                    [this] { publish(web_->grab().toImage()); });
            web_->show();
            web_->load(url);
        } else if (source.type == "source.slideshow") {
            files_ = QString::fromStdString(text("files")).split('\n', Qt::SkipEmptyParts);
            show_slide();
            slides_.start(std::clamp(number("intervalMs", 5000), 100, 100000));
        } else if (source.type == "source.media" || source.type == "source.playlist") {
            files_ = source.type == "source.media"
                         ? QStringList{QString::fromStdString(text("localFile"))}
                         : QString::fromStdString(text("files")).split('\n', Qt::SkipEmptyParts);
            play_file();
        } else
            throw std::runtime_error("unsupported source worker type");
    }

    lcomp::AudioPacket take_audio() {
        std::lock_guard lock(audio_mu_);
        auto samples = std::move(audio_pending_);
        audio_pending_.clear();
        return {source_.id, std::move(samples)};
    }

    lcomp::Picture picture() const {
        lcomp::Picture out;
        out.width = image_.width();
        out.height = image_.height();
        out.stride = static_cast<std::uint32_t>(out.width) * 4;
        out.status = valid_ ? lcomp::PictureStatus::Complete : lcomp::PictureStatus::Partial;
        auto lease = std::make_shared<QImage>(image_);
        out.borrowed = lease->constBits();
        out.owner = std::move(lease);
        return out;
    }
};
} // namespace

int main(int argc, char** argv) {
    const char* spec = std::getenv("LIBERTAS_CHANNEL_FD");
    int fd = -1;
    if (!spec)
        return 2;
    auto parsed = std::from_chars(spec, spec + std::strlen(spec), fd);
    if (parsed.ec != std::errc{} || *parsed.ptr || fd < 0)
        return 2;
    // ponytail: Qt 6.12 image conversion uses a separate eight-thread GUI pool
    // per worker. Media sources already have concurrent decoder threads.
    qputenv("QT_NO_GUI_THREADPOOL", "1");
    qputenv("QT_QPA_PLATFORM", "offscreen");
    qputenv("QTWEBENGINE_CHROMIUM_FLAGS",
            qgetenv("QTWEBENGINE_CHROMIUM_FLAGS") +
                " --disable-gpu --autoplay-policy=no-user-gesture-required");
    QApplication app(argc, argv);
    QImageReader::setAllocationLimit(128);
    Content content;
    std::thread transport([&] {
        try {
            auto result = lcomp::serve_preview(
                lipc::Channel{lipc::Fd{fd}}, {}, {}, [&] { return content.dirty.exchange(false); },
                [&](const ldoc::Project& project, const ldoc::Id&, std::uint32_t) {
                    lcomp::Picture frame;
                    std::string error;
                    QMetaObject::invokeMethod(
                        &content,
                        [&] {
                            try {
                                if (project.sources.size() != 1)
                                    throw std::runtime_error("worker requires one source");
                                content.configure(project.sources.front());
                                frame = content.picture();
                            } catch (const std::exception& e) {
                                error = e.what();
                            }
                        },
                        Qt::BlockingQueuedConnection);
                    if (!error.empty())
                        throw std::runtime_error(error);
                    return frame;
                },
                {},
                [&] {
                    lcomp::AudioPacket packet;
                    QMetaObject::invokeMethod(
                        &content, [&] { packet = content.take_audio(); },
                        Qt::BlockingQueuedConnection);
                    return packet;
                });
            if (!result)
                qWarning("source worker: %s", result.error().c_str());
        } catch (const std::exception& e) {
            qWarning("source worker: %s", e.what());
        }
        QMetaObject::invokeMethod(&app, &QCoreApplication::quit, Qt::QueuedConnection);
    });
    app.exec();
    transport.join();
}
