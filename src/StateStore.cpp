#include "StateStore.h"

#include <QCoreApplication>
#include <QDebug>
#include <QFile>
#include <QJsonDocument>

QString stateFilePath() {
    return QCoreApplication::applicationDirPath() + QStringLiteral("/auto_task_state.json");
}

QJsonObject loadState() {
    QFile f(stateFilePath());
    if (f.open(QIODevice::ReadOnly)) {
        QJsonParseError pe{};
        QJsonDocument doc = QJsonDocument::fromJson(f.readAll(), &pe);
        if (pe.error == QJsonParseError::NoError && doc.isObject())
            return doc.object();
    }
    return QJsonObject{{"token", ""}, {"os_value", ""}, {"user_id", 0},
                       {"package_uuid", ""}, {"user_name", ""}};
}

bool saveState(const QJsonObject &patch) {
    QJsonObject st = loadState();
    for (auto it = patch.begin(); it != patch.end(); ++it)
        st[it.key()] = it.value();
    const QString path = stateFilePath();
    const QString tmpPath = path + QStringLiteral(".tmp");
    // Atomic write: temp file first, rename only after flush/close check out,
    // so a crash or power loss cannot leave a half-written JSON
    QFile f(tmpPath);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        qWarning() << "saveState: cannot open temp file" << tmpPath << ":" << f.errorString();
        return false;
    }
    const QByteArray json = QJsonDocument(st).toJson(QJsonDocument::Indented);
    if (f.write(json) != json.size() || !f.flush()) {
        qWarning() << "saveState: write failed" << tmpPath << ":" << f.errorString();
        f.close();
        QFile::remove(tmpPath);
        return false;
    }
    f.close();
    if (f.error() != QFileDevice::NoError) {
        qWarning() << "saveState: close failed" << tmpPath << ":" << f.errorString();
        QFile::remove(tmpPath);
        return false;
    }
    // Windows rename cannot overwrite an existing target; remove it first
    if (QFile::exists(path) && !QFile::remove(path)) {
        qWarning() << "saveState: cannot remove old file" << path;
        QFile::remove(tmpPath);
        return false;
    }
    if (!QFile::rename(tmpPath, path)) {
        qWarning() << "saveState: rename failed" << tmpPath << "->" << path;
        QFile::remove(tmpPath);
        return false;
    }
    return true;
}
