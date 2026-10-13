// SPDX-FileCopyrightText: Libertas-Studio contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include "shortcuts.hpp"

#include "document.hpp"
#include "workspace.hpp"

#include "lcore/atomic_file.hpp"

#include <QKeySequence>
#include <QQmlEngine>
#include <QVariantMap>

namespace libertas::ui {
namespace {

std::filesystem::path g_bindings_file;

QString qstr(const std::string& s) { return QString::fromStdString(s); }

std::string to_std(const QString& s) {
    const QByteArray b = s.toUtf8();
    return std::string{b.constData(), static_cast<std::size_t>(b.size())};
}

} // namespace

int BindingModel::rowCount(const QModelIndex& parent) const {
    return parent.isValid() ? 0 : static_cast<int>(rows_.size());
}

QVariant BindingModel::data(const QModelIndex& index, int role) const {
    const BindingRow* r = at(rows_, index);
    if (!r)
        return {};
    switch (role) {
    case RefRole:       return r->ref;
    case TitleRole:     return r->title;
    case CategoryRole:  return r->category;
    case ScopeRole:     return r->scope;
    case TriggerRole:   return r->trigger;
    case IsDefaultRole: return r->isDefault;
    case AvailableRole: return r->available;
    case ConflictRole:  return r->conflict;
    default:            return {};
    }
}

QHash<int, QByteArray> BindingModel::roleNames() const {
    return {
        {RefRole, "ref"},           {TitleRole, "title"},
        {CategoryRole, "category"}, {ScopeRole, "scope"},
        {TriggerRole, "trigger"},   {IsDefaultRole, "isDefault"},
        {AvailableRole, "available"}, {ConflictRole, "conflict"},
    };
}

void Shortcuts::configure(std::filesystem::path bindings_file) {
    g_bindings_file = std::move(bindings_file);
}

Shortcuts* Shortcuts::create(QQmlEngine* engine, QJSEngine*) {
    auto* s = new Shortcuts;
    if (engine) {
        // Asking the engine for them creates them if they do not exist yet, so the order these
        // singletons are first touched in does not matter.
        s->document_ = engine->singletonInstance<Document*>("Libertas", "Document");
        s->workspace_ = engine->singletonInstance<Workspace*>("Libertas", "Workspace");
    }
    s->rebuild();
    return s;
}

Shortcuts::Shortcuts(QObject* parent) : QObject(parent), file_(g_bindings_file) {
    if (!file_.empty()) {
        if (const auto text = lc::read_file(file_)) {
            auto loaded = lsession::Bindings::from_toml(*text);
            if (loaded)
                bindings_ = std::move(*loaded);
            else
                QMetaObject::invokeMethod(
                    this,
                    [this, why = qstr(loaded.error())] {
                        emit notice(QStringLiteral("Key bindings could not be read (%1); the "
                                                   "defaults are in use.")
                                        .arg(why));
                    },
                    Qt::QueuedConnection);
        }
    }
}

Shortcuts::~Shortcuts() = default;

void Shortcuts::rebuild() {
    std::vector<BindingRow> rows;
    const ldoc::Project empty;
    const ldoc::Project& project = document_ ? document_->projectForBindings() : empty;

    for (const lsession::ActionRef& ref : lsession::available_actions(project)) {
        const lsession::ActionDef* def = lsession::find_action(ref.action);
        if (!def)
            continue;
        BindingRow row;
        row.ref = qstr(ref.to_string());
        row.title = qstr(lsession::describe(ref, project));
        row.category = qstr(def->category);
        const auto scope = lsession::to_string(def->scope);
        row.scope = QString::fromUtf8(scope.data(), static_cast<qsizetype>(scope.size()));
        row.trigger = qstr(bindings_.trigger_for(ref));
        row.isDefault = bindings_.is_default(ref);
        const auto resolved = lsession::resolve(ref, project);
        row.available = resolved.outcome != lsession::Outcome::NotYetAvailable &&
                        resolved.outcome != lsession::Outcome::Unknown;
        if (!row.trigger.isEmpty()) {
            if (const auto clash = bindings_.conflict(ref, to_std(row.trigger), project))
                row.conflict = qstr(lsession::describe(*clash, project));
        }
        rows.push_back(std::move(row));
    }
    bindings_model_.set_rows(std::move(rows));
    emit changed();
}

QVariantList Shortcuts::applicationBindings() const {
    QVariantList out;
    for (const BindingRow& r : bindings_model_.rows()) {
        if (r.scope != QLatin1String("application") || r.trigger.isEmpty())
            continue;
        QVariantMap m;
        m["ref"] = r.ref;
        m["trigger"] = r.trigger;
        out.append(m);
    }
    return out;
}

int Shortcuts::unboundGlobalCount() const {
    int n = 0;
    for (const BindingRow& r : bindings_model_.rows())
        n += (r.scope == QLatin1String("global") && r.trigger.isEmpty()) ? 1 : 0;
    return n;
}

void Shortcuts::perform(const QString& ref) {
    const auto parsed = lsession::ActionRef::parse(to_std(ref));
    if (!parsed || !document_)
        return;

    // Workspace actions are the one family that does not touch the document, because the layout
    // is not part of it (ADR-0021). Routed by prefix rather than one case each, so adding a
    // direction to the catalogue does not also mean adding a branch here.
    if (parsed->action.starts_with("workspace.") && workspace_) {
        const std::string_view verb =
            std::string_view{parsed->action}.substr(std::string_view{"workspace."}.size());
        const auto suffix = [verb](std::string_view prefix) {
            return QString::fromUtf8(verb.data() + prefix.size(),
                                     static_cast<qsizetype>(verb.size() - prefix.size()));
        };
        if (verb == "undo")
            workspace_->undo();
        else if (verb == "redo")
            workspace_->redo();
        else if (verb == "reset")
            workspace_->resetLayout();
        else if (verb == "focus-next")
            workspace_->focusNext(false);
        else if (verb == "focus-previous")
            workspace_->focusNext(true);
        else if (verb.starts_with("focus-"))
            workspace_->focusNeighbour(suffix("focus-"));
        else if (verb.starts_with("move-"))
            workspace_->movePanel(suffix("move-"));
        else if (verb.starts_with("tab-with-"))
            workspace_->tabPanelWithNeighbour(suffix("tab-with-"));
        else if (verb.starts_with("edge-"))
            workspace_->movePanelToEdge(suffix("edge-"));
        return;
    }

    const auto resolved = lsession::resolve(*parsed, document_->projectForBindings());
    switch (resolved.outcome) {
    case lsession::Outcome::Undo:
        document_->undo();
        return;
    case lsession::Outcome::Redo:
        document_->redo();
        return;
    case lsession::Outcome::Save:
        document_->save();
        return;
    case lsession::Outcome::Command:
        if (resolved.command)
            document_->runCommand(*resolved.command);
        return;
    case lsession::Outcome::Workspace:
        return; // routed above; nothing in the document changed
    case lsession::Outcome::NotYetAvailable:
    case lsession::Outcome::Unknown:
        emit notice(qstr(resolved.note));
        return;
    }
}

void Shortcuts::setTrigger(const QString& ref, const QString& trigger) {
    const auto parsed = lsession::ActionRef::parse(to_std(ref));
    if (!parsed)
        return;
    bindings_.set_trigger(*parsed, to_std(trigger));
    persist();
    rebuild();
}

void Shortcuts::resetTrigger(const QString& ref) {
    const auto parsed = lsession::ActionRef::parse(to_std(ref));
    if (!parsed)
        return;
    bindings_.reset(*parsed);
    persist();
    rebuild();
}

void Shortcuts::resetAll() {
    bindings_.reset_all();
    persist();
    rebuild();
}

QString Shortcuts::formatKey(int key, int modifiers) const {
    // A bare modifier is not a shortcut; reporting one would let the table record "Ctrl" and then
    // never fire.
    switch (key) {
    case Qt::Key_Control:
    case Qt::Key_Shift:
    case Qt::Key_Alt:
    case Qt::Key_Meta:
    case Qt::Key_unknown:
        return {};
    default:
        break;
    }
    const QKeySequence seq{QKeyCombination(static_cast<Qt::KeyboardModifiers>(modifiers),
                                           static_cast<Qt::Key>(key))};
    return seq.toString(QKeySequence::PortableText);
}

void Shortcuts::persist() {
    if (file_.empty())
        return;
    if (const auto r = lc::write_atomic(file_, bindings_.to_toml()); !r)
        emit notice(QStringLiteral("Key bindings could not be saved: %1").arg(qstr(r.error())));
}

} // namespace libertas::ui
