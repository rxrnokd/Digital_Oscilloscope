// SIGNAL LAB PRO - instrument setup <-> JSON (Qt Core only).
// Used for "Save/Load Setup", the automatic session restore and the example setups.
#pragma once

#include "ScopeSettings.h"

#include <QJsonObject>
#include <QString>

namespace slp {

QJsonObject settingsToJson(const ScopeSettings& s);
// Missing or invalid keys fall back to the defaults (forward/backward compatible).
ScopeSettings settingsFromJson(const QJsonObject& o);

bool saveSettingsFile(const ScopeSettings& s, const QString& path, QString* error = nullptr);
bool loadSettingsFile(ScopeSettings& s, const QString& path, QString* error = nullptr);

} // namespace slp
