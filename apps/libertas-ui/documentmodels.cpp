// SPDX-FileCopyrightText: Libertas-Studio contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include "documentmodels.hpp"

namespace libertas::ui {

int SceneModel::rowCount(const QModelIndex& parent) const {
    return parent.isValid() ? 0 : static_cast<int>(rows_.size());
}

QVariant SceneModel::data(const QModelIndex& index, int role) const {
    const SceneRow* r = at(rows_, index);
    if (!r)
        return {};
    switch (role) {
    case IdRole:          return r->id;
    case NameRole:        return r->name;
    case NodeCountRole:   return r->nodeCount;
    case IsProgramRole:   return r->isProgram;
    case IsPreviewRole:   return r->isPreview;
    case IsEmergencyRole: return r->isEmergency;
    case IsFavouriteRole: return r->isFavourite;
    default:              return {};
    }
}

QHash<int, QByteArray> SceneModel::roleNames() const {
    return {
        {IdRole, "sceneId"},          {NameRole, "name"},
        {NodeCountRole, "nodeCount"}, {IsProgramRole, "isProgram"},
        {IsPreviewRole, "isPreview"}, {IsEmergencyRole, "isEmergency"},
        {IsFavouriteRole, "isFavourite"},
    };
}

int NodeModel::rowCount(const QModelIndex& parent) const {
    return parent.isValid() ? 0 : static_cast<int>(rows_.size());
}

QVariant NodeModel::data(const QModelIndex& index, int role) const {
    const NodeRow* r = at(rows_, index);
    if (!r)
        return {};
    switch (role) {
    case IdRole:          return r->id;
    case NameRole:        return r->name;
    case KindRole:        return r->kind;
    case SourceNameRole:  return r->sourceName;
    case SourceIdRole:    return r->sourceId;
    case DepthRole:       return r->depth;
    case VisibleRole:     return r->visible;
    case LockedRole:      return r->locked;
    case IsGroupRole:     return r->isGroup;
    case EffectCountRole: return r->effectCount;
    default:              return {};
    }
}

QHash<int, QByteArray> NodeModel::roleNames() const {
    return {
        {IdRole, "nodeId"},      {NameRole, "name"},
        {KindRole, "kind"},      {SourceNameRole, "sourceName"},
        {SourceIdRole, "sourceId"},
        {DepthRole, "depth"},    {VisibleRole, "nodeVisible"},
        {LockedRole, "locked"},  {IsGroupRole, "isGroup"},
        {EffectCountRole, "effectCount"},
    };
}

int SourceModel::rowCount(const QModelIndex& parent) const {
    return parent.isValid() ? 0 : static_cast<int>(rows_.size());
}

QVariant SourceModel::data(const QModelIndex& index, int role) const {
    const SourceRow* r = at(rows_, index);
    if (!r)
        return {};
    switch (role) {
    case IdRole:            return r->id;
    case NameRole:          return r->name;
    case TypeRole:          return r->type;
    case UseCountRole:      return r->useCount;
    case SelfContainedRole: return r->selfContained;
    default:                return {};
    }
}

QHash<int, QByteArray> SourceModel::roleNames() const {
    return {
        {IdRole, "sourceId"},  {NameRole, "name"},
        {TypeRole, "type"},    {UseCountRole, "useCount"},
        {SelfContainedRole, "selfContained"},
    };
}

int BusModel::rowCount(const QModelIndex& parent) const {
    return parent.isValid() ? 0 : static_cast<int>(rows_.size());
}

QVariant BusModel::data(const QModelIndex& index, int role) const {
    const BusRow* r = at(rows_, index);
    if (!r)
        return {};
    switch (role) {
    case IdRole:               return r->id;
    case NameRole:             return r->name;
    case KindRole:             return r->kind;
    case GainDbRole:           return r->gainDb;
    case PanRole:              return r->pan;
    case MuteRole:             return r->mute;
    case SoloRole:             return r->solo;
    case ChannelsRole:         return r->channels;
    case FeedCountRole:        return r->feedCount;
    case SendCountRole:        return r->sendCount;
    case CarriesSelectionRole: return r->carriesSelection;
    default:                   return {};
    }
}

QHash<int, QByteArray> BusModel::roleNames() const {
    return {
        {IdRole, "busId"},           {NameRole, "name"},
        {KindRole, "kind"},          {GainDbRole, "gainDb"},
        {PanRole, "pan"},            {MuteRole, "mute"},
        {SoloRole, "solo"},          {ChannelsRole, "channels"},
        {FeedCountRole, "feedCount"}, {SendCountRole, "sendCount"},
        {CarriesSelectionRole, "carriesSelection"},
    };
}

int DeckModel::rowCount(const QModelIndex& parent) const {
    return parent.isValid() ? 0 : static_cast<int>(rows_.size());
}

QVariant DeckModel::data(const QModelIndex& index, int role) const {
    const DeckRow* r = at(rows_, index);
    if (!r)
        return {};
    switch (role) {
    case IdRole:                return r->id;
    case NameRole:              return r->name;
    case CanvasNameRole:        return r->canvasName;
    case ProgramSceneRole:      return r->programScene;
    case PreviewSceneRole:      return r->previewScene;
    case ProgramSceneIdRole:    return r->programSceneId;
    case PreviewSceneIdRole:    return r->previewSceneId;
    case StudioModeRole:        return r->studioMode;
    case TransitionRole:        return r->transition;
    case TransitionMsRole:      return r->transitionMs;
    case HasEmergencySceneRole: return r->hasEmergencyScene;
    default:                    return {};
    }
}

QHash<int, QByteArray> DeckModel::roleNames() const {
    return {
        {IdRole, "deckId"},
        {NameRole, "name"},
        {CanvasNameRole, "canvasName"},
        {ProgramSceneRole, "programScene"},
        {PreviewSceneRole, "previewScene"},
        {ProgramSceneIdRole, "programSceneId"},
        {PreviewSceneIdRole, "previewSceneId"},
        {StudioModeRole, "studioMode"},
        {TransitionRole, "transition"},
        {TransitionMsRole, "transitionMs"},
        {HasEmergencySceneRole, "hasEmergencyScene"},
    };
}

int OutputModel::rowCount(const QModelIndex& parent) const {
    return parent.isValid() ? 0 : static_cast<int>(rows_.size());
}

QVariant OutputModel::data(const QModelIndex& index, int role) const {
    const OutputRow* r = at(rows_, index);
    if (!r)
        return {};
    switch (role) {
    case IdRole:              return r->id;
    case NameRole:            return r->name;
    case CanvasNameRole:      return r->canvasName;
    case CodecRole:           return r->codec;
    case ImplementationRole:  return r->implementation;
    case BitrateRole:         return r->bitrateKbps;
    case DestinationKindRole: return r->destinationKind;
    case UrlRole:             return r->url;
    case CredentialRefRole:   return r->credentialRef;
    case EnabledRole:         return r->enabled;
    case MisconfiguredRole:   return r->misconfigured;
    default:                  return {};
    }
}

QHash<int, QByteArray> OutputModel::roleNames() const {
    return {
        {IdRole, "outputId"},
        {NameRole, "name"},
        {CanvasNameRole, "canvasName"},
        {CodecRole, "codec"},
        {ImplementationRole, "implementation"},
        {BitrateRole, "bitrateKbps"},
        {DestinationKindRole, "destinationKind"},
        {UrlRole, "url"},
        {CredentialRefRole, "credentialRef"},
        // Not "enabled": that is QQuickItem's own property, and a delegate exposing a role
        // by that name would disable itself whenever the output was disabled.
        {EnabledRole, "isEnabled"},
        {MisconfiguredRole, "misconfigured"},
    };
}

} // namespace libertas::ui
