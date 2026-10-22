// SPDX-FileCopyrightText: Libertas-Studio contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#include "document.hpp"

#include <QDateTime>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <cmath>
#include <lcomp/countdown.hpp>
#include <set>

namespace libertas::ui {
QVariantList Document::countdownTextSources() const {
    QVariantList out;
    for (const auto& source : session_->project().sources)
        if (source.type == "source.text")
            out.push_back(QVariantMap{{"id", QString::fromStdString(source.id.to_string())},
                                      {"name", QString::fromStdString(source.name)}});
    return out;
}

QVariantList Document::countdownScenes() const {
    QVariantList out;
    for (const auto& scene : session_->project().scenes)
        out.push_back(QVariantMap{{"id", QString::fromStdString(scene.id.to_string())},
                                  {"name", QString::fromStdString(scene.name)}});
    return out;
}

QVariantList Document::countdowns() const {
    QVariantList out;
    for (const auto& source : session_->project().sources) {
        if (source.type != "source.text" ||
            !lcomp::timer_flag(source.settings, "countdown.enabled"))
            continue;
        QVariantMap row{{"id", QString::fromStdString(source.id.to_string())},
                        {"name", QString::fromStdString(source.name)}};
        for (const auto& [key, value] : source.settings) {
            if (!key.starts_with("countdown."))
                continue;
            std::visit(
                [&](const auto& item) {
                    using T = std::decay_t<decltype(item)>;
                    if constexpr (std::is_same_v<T, std::string>)
                        row[QString::fromStdString(key.substr(10))] = QString::fromStdString(item);
                    else if constexpr (std::is_same_v<T, bool> || std::is_same_v<T, std::int64_t>)
                        row[QString::fromStdString(key.substr(10))] = QVariant::fromValue(item);
                },
                value);
        }
        out.push_back(row);
    }
    return out;
}

QString Document::addCountdown(const QString& sourceId) {
    if (sourceId.isEmpty()) {
        addNewSourceToScene(QStringLiteral("Countdown"), QStringLiteral("source.text"), true);
        const auto* node = current_node();
        const auto* data = node ? std::get_if<ldoc::SourceData>(&node->data) : nullptr;
        if (!data || !data->source)
            return {};
        return addCountdown(QString::fromStdString(data->source->to_string()));
    }
    const auto id = ldoc::Id::parse(sourceId.toStdString());
    const auto* source = id ? session_->project().source(*id) : nullptr;
    if (!source || source->type != "source.text")
        return {};
    auto copy = *source;
    copy.settings["countdown.enabled"] = true;
    copy.settings.try_emplace("countdown.durationMs", std::int64_t{900000});
    copy.settings.try_emplace("countdown.format", std::string("IN: %time%"));
    run(ldoc::SetSource{std::move(copy)});
    return sourceId;
}

void Document::configureCountdown(const QString& sourceId, const QVariantMap& settings) {
    const auto id = ldoc::Id::parse(sourceId.toStdString());
    const auto* source = id ? session_->project().source(*id) : nullptr;
    if (!source || source->type != "source.text")
        return;
    auto copy = *source;
    for (auto it = settings.begin(); it != settings.end(); ++it) {
        const auto key = it.key().toStdString();
        const auto full = "countdown." + key;
        if (key == "durationMs" || key == "targetAtMs") {
            bool ok = false;
            const double value = it.value().toDouble(&ok);
            const double max = key == "durationMs" ? 366.0 * 86400000 : 4102444800000.0;
            if (!ok || !std::isfinite(value) || value < 0 || value > max) {
                emit notice(QStringLiteral("Countdown time is outside its supported range."));
                return;
            }
            copy.settings[full] = static_cast<std::int64_t>(value);
        } else if (key == "mode" || key == "format" || key == "endMessage" || key == "endScene") {
            const auto value = it.value().toString();
            if (value.size() > 1024 || (key == "mode" && value != "remaining" &&
                                        value != "elapsed" && value != "target")) {
                emit notice(QStringLiteral("Invalid countdown setting."));
                return;
            }
            if (key == "endScene" && !value.isEmpty()) {
                const auto scene = ldoc::Id::parse(value.toStdString());
                if (!scene || !session_->project().scene(*scene)) {
                    emit notice(QStringLiteral("Countdown end scene no longer exists."));
                    return;
                }
            }
            copy.settings[full] = value.toStdString();
        } else if (key == "showDays" || key == "showHours" || key == "showMinutes" ||
                   key == "showSeconds" || key == "leadingZero" || key == "showEndMessage") {
            copy.settings[full] = it.value().toBool();
        } else {
            emit notice(QStringLiteral("Unknown countdown setting: %1").arg(it.key()));
            return;
        }
    }
    run(ldoc::SetSource{std::move(copy)});
}

QVariantMap Document::countdownStatus(const QString& sourceId) const {
    const auto id = ldoc::Id::parse(sourceId.toStdString());
    const auto* source = id ? session_->project().source(*id) : nullptr;
    if (!source)
        return {};
    const auto value = lcomp::countdown(source->settings, QDateTime::currentMSecsSinceEpoch());
    return {{"text", QString::fromStdString(value.text)},
            {"running", lcomp::timer_flag(source->settings, "countdown.running")},
            {"ended", value.ended}};
}

void Document::controlCountdown(const QString& sourceId, const QString& action) {
    const auto id = ldoc::Id::parse(sourceId.toStdString());
    const auto* source = id ? session_->project().source(*id) : nullptr;
    if (!source || !lcomp::timer_flag(source->settings, "countdown.enabled"))
        return;
    if (action != "start" && action != "pause" && action != "reset" && action != "finish")
        return;
    auto copy = *source;
    const auto now = QDateTime::currentMSecsSinceEpoch();
    const auto current = lcomp::countdown(copy.settings, now);
    if (action == "start" && lcomp::timer_flag(copy.settings, "countdown.running"))
        return;
    copy.settings["countdown.elapsedMs"] = action == "reset" ? std::int64_t{0} : current.elapsed;
    copy.settings["countdown.targetRemainingMs"] =
        action == "reset"
            ? std::max<std::int64_t>(
                  0, lcomp::timer_number(copy.settings, "countdown.targetAtMs", now) - now)
            : current.remaining;
    copy.settings["countdown.startedAtMs"] = static_cast<std::int64_t>(now);
    copy.settings["countdown.running"] = action == "start";
    const auto target = lcomp::timer_text(copy.settings, "countdown.endScene");
    run(ldoc::SetSource{std::move(copy)});
    if (action == "finish") {
        if (!target.empty())
            activateScene(QString::fromStdString(target));
    }
}

void Document::removeCountdown(const QString& sourceId) {
    const auto id = ldoc::Id::parse(sourceId.toStdString());
    const auto* source = id ? session_->project().source(*id) : nullptr;
    if (!source)
        return;
    auto copy = *source;
    copy.settings["countdown.enabled"] = false;
    copy.settings["countdown.running"] = false;
    run(ldoc::SetSource{std::move(copy)});
}

bool Document::importCountdowns(const QString& path) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly) || file.size() > 1024 * 1024) {
        emit notice(QStringLiteral("Cannot read countdown configuration."));
        return false;
    }
    QJsonParseError error;
    const auto data = QJsonDocument::fromJson(file.readAll(), &error);
    if (error.error != QJsonParseError::NoError || !data.isObject() ||
        !data.object()["timer_widgets"].isArray()) {
        emit notice(QStringLiteral("Invalid OBS countdown configuration."));
        return false;
    }
    if (data.object()["timer_widgets"].toArray().size() > 128) {
        emit notice(QStringLiteral("Countdown configuration has too many timers."));
        return false;
    }
    std::vector<ldoc::Command> commands;
    QStringList warnings;
    std::set<ldoc::Id> bound;
    for (const auto& entry : data.object()["timer_widgets"].toArray()) {
        if (!entry.isObject()) {
            warnings << "Timer entry must be an object";
            continue;
        }
        const auto item = entry.toObject();
        bool valid = item["selectedSource"].isString();
        for (const char* field : {"periodDays", "periodHours", "periodMinutes", "periodSeconds",
                                  "selectedCountdownType"})
            if (item.contains(field) && (!item[field].isDouble() || item[field].toDouble() < 0))
                valid = false;
        for (const char* field :
             {"shouldCountUp", "showDays", "showHours", "showMinutes", "showSeconds",
              "showLeadingZero", "showEndMessage", "showEndScene", "useFormattedOutput"})
            if (item.contains(field) && !item[field].isBool())
                valid = false;
        for (const char* field : {"endMessage", "outputStringFormat", "selectedScene", "dateTime"})
            if (item.contains(field) &&
                (!item[field].isString() || item[field].toString().size() > 1024))
                valid = false;
        if (!valid || item["selectedCountdownType"].toInt() > 1) {
            warnings << "Invalid or unsupported timer configuration";
            continue;
        }
        const auto name = item["selectedSource"].toString();
        const ldoc::Source* found = nullptr;
        int matches = 0;
        for (const auto& source : session_->project().sources)
            if (source.type == "source.text" && QString::fromStdString(source.name) == name) {
                found = &source;
                ++matches;
            }
        if (matches != 1 || !found || !bound.insert(found->id).second) {
            warnings << QStringLiteral("Missing, ambiguous or repeated text source: %1").arg(name);
            continue;
        }
        auto copy = *found;
        auto& settings = copy.settings;
        const double seconds =
            item["periodDays"].toDouble() * 86400 + item["periodHours"].toDouble() * 3600 +
            item["periodMinutes"].toDouble() * 60 + item["periodSeconds"].toDouble();
        if (!std::isfinite(seconds) || seconds < 0 || seconds > 366.0 * 86400) {
            warnings << QStringLiteral("Invalid duration: %1").arg(name);
            continue;
        }
        const bool target = item["selectedCountdownType"].toInt() == 1;
        settings["countdown.obsOriginal"] =
            QJsonDocument(item).toJson(QJsonDocument::Compact).toStdString();
        settings["countdown.enabled"] = true;
        settings["countdown.running"] = false;
        settings["countdown.elapsedMs"] = std::int64_t{0};
        settings["countdown.durationMs"] = static_cast<std::int64_t>(seconds * 1000);
        settings["countdown.mode"] = std::string(target                           ? "target"
                                                 : item["shouldCountUp"].toBool() ? "elapsed"
                                                                                  : "remaining");
        settings["countdown.format"] = item["useFormattedOutput"].toBool()
                                           ? item["outputStringFormat"].toString().toStdString()
                                           : std::string("%time%");
        settings["countdown.endMessage"] = item["endMessage"].toString().toStdString();
        for (const auto& [native, obs] : {std::pair{"showDays", "showDays"},
                                          {"showHours", "showHours"},
                                          {"showMinutes", "showMinutes"},
                                          {"showSeconds", "showSeconds"},
                                          {"leadingZero", "showLeadingZero"},
                                          {"showEndMessage", "showEndMessage"}})
            settings[std::string("countdown.") + native] = item[obs].toBool();
        if (target) {
            const auto time = QDateTime::fromString(item["dateTime"].toString(), Qt::ISODate);
            if (!time.isValid()) {
                warnings << "Invalid target time: " + name;
                continue;
            }
            settings["countdown.targetAtMs"] = static_cast<std::int64_t>(time.toMSecsSinceEpoch());
        }
        settings.erase("countdown.endScene");
        if (item["showEndScene"].toBool()) {
            const auto sceneName = item["selectedScene"].toString();
            int count = 0;
            for (const auto& scene : session_->project().scenes)
                if (QString::fromStdString(scene.name) == sceneName) {
                    settings["countdown.endScene"] = scene.id.to_string();
                    ++count;
                }
            if (count != 1) {
                warnings << "Missing/ambiguous end scene: " + sceneName;
                settings.erase("countdown.endScene");
            }
        }
        if (item["useTextColour"].toBool() || !item["colourRules"].toArray().isEmpty() ||
            item["startOnStreamStart"].toBool() || item["resetTimerOnStreamStart"].toBool() ||
            item["smoothenPeriodTimer"].toBool())
            warnings << "Color rules, stream triggers and timer smoothing need manual review: " +
                            name;
        commands.push_back(ldoc::SetSource{std::move(copy)});
    }
    if (!commands.empty())
        run(ldoc::make_batch(std::move(commands), "Import OBS countdown timers"));
    emit notice(QStringLiteral(
                    "Countdown import finished. Timers start paused. Hotkeys are not imported.%1")
                    .arg(warnings.isEmpty() ? QString{} : "\n" + warnings.join('\n')));
    return warnings.isEmpty();
}
} // namespace libertas::ui
