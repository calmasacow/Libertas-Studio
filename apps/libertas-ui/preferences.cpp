// SPDX-FileCopyrightText: Libertas-Studio contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include "preferences.hpp"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>

namespace libertas::ui {
namespace {
QString g_path;
int g_gpu_budget_mib = 0;

void export_budget() {
    if (g_gpu_budget_mib > 0)
        qputenv("LIBERTAS_GPU_BUDGET_MIB", QByteArray::number(g_gpu_budget_mib));
    else
        qunsetenv("LIBERTAS_GPU_BUDGET_MIB");
}

bool valid_budget(int mib) { return mib == 0 || (mib >= 256 && mib <= 65536); }
} // namespace

void Preferences::configure(std::filesystem::path path) {
    g_path = QString::fromStdString(path.string());
    QFile file(g_path);
    if (file.open(QIODevice::ReadOnly) && file.size() <= 64 * 1024) {
        const auto json = QJsonDocument::fromJson(file.readAll()).object();
        const int mib = json.value("gpuBudgetMib").toInt(0);
        g_gpu_budget_mib = valid_budget(mib) ? mib : 0;
    }
    export_budget();
}

Preferences* Preferences::create(QQmlEngine*, QJSEngine*) { return new Preferences(nullptr); }

int Preferences::gpuBudgetMib() const { return g_gpu_budget_mib; }

bool Preferences::setGpuBudgetMib(int mib) {
    if (!valid_budget(mib) || g_path.isEmpty() || !QDir().mkpath(QFileInfo(g_path).absolutePath()))
        return false;
    if (mib == g_gpu_budget_mib)
        return true;
    QSaveFile file(g_path);
    if (!file.open(QIODevice::WriteOnly))
        return false;
    file.write(QJsonDocument(QJsonObject{{"version", 1}, {"gpuBudgetMib", mib}}).toJson());
    if (!file.commit())
        return false;
    g_gpu_budget_mib = mib;
    export_budget();
    emit changed();
    emit restartRenderer();
    return true;
}

} // namespace libertas::ui
