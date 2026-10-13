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
    case LayerCountRole:  return r->layerCount;
    case IsProgramRole:   return r->isProgram;
    case IsPreviewRole:   return r->isPreview;
    case IsEmergencyRole: return r->isEmergency;
    default:              return {};
    }
}

QHash<int, QByteArray> SceneModel::roleNames() const {
    return {
        {IdRole, "sceneId"},        {NameRole, "name"},
        {LayerCountRole, "layerCount"}, {IsProgramRole, "isProgram"},
        {IsPreviewRole, "isPreview"},   {IsEmergencyRole, "isEmergency"},
    };
}

int LayerModel::rowCount(const QModelIndex& parent) const {
    return parent.isValid() ? 0 : static_cast<int>(rows_.size());
}

QVariant LayerModel::data(const QModelIndex& index, int role) const {
    const LayerRow* r = at(rows_, index);
    if (!r)
        return {};
    switch (role) {
    case IdRole:          return r->id;
    case NameRole:        return r->name;
    case KindRole:        return r->kind;
    case SourceNameRole:  return r->sourceName;
    case DepthRole:       return r->depth;
    case VisibleRole:     return r->visible;
    case LockedRole:      return r->locked;
    case IsGroupRole:     return r->isGroup;
    case EffectCountRole: return r->effectCount;
    default:              return {};
    }
}

QHash<int, QByteArray> LayerModel::roleNames() const {
    return {
        {IdRole, "layerId"},     {NameRole, "name"},
        {KindRole, "kind"},      {SourceNameRole, "sourceName"},
        {DepthRole, "depth"},    {VisibleRole, "layerVisible"},
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

} // namespace libertas::ui
