#pragma once

#include <QJsonObject>

// auto_task_state.json (next to the exe, field names compatible with the legacy Python tool)
QJsonObject loadState();
bool saveState(const QJsonObject &patch);  // Merge-writes patch via temp file + rename; false + qWarning on error
QString stateFilePath();
