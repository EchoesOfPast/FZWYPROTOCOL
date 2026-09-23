#include "Config.h"

#include <QCoreApplication>
#include <QDebug>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>

AppConfig g_config;

AppConfig AppConfig::loadFrom(const QString &path) {
    AppConfig cfg;
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) {
        cfg.loadError = QStringLiteral("缺少 config.json（请参照 config.example.json 创建并放在程序同目录）");
        qWarning() << "Config file not found:" << path
                   << "- using empty defaults. Create config.json from config.example.json.";
        return cfg;
    }
    QJsonParseError pe{};
    QJsonDocument doc = QJsonDocument::fromJson(f.readAll(), &pe);
    if (pe.error != QJsonParseError::NoError || !doc.isObject()) {
        cfg.loadError = QStringLiteral("config.json 解析失败：%1").arg(pe.errorString());
        qWarning() << "Config parse error:" << pe.errorString();
        return cfg;
    }
    QJsonObject o = doc.object();
    cfg.aesKey = o["aesKey"].toString();
    cfg.aesIv = o["aesIv"].toString();
    cfg.serverPubKey = o["serverPubKey"].toString();
    cfg.appId = o["appId"].toString();
    cfg.mpCode = o["mpCode"].toString();
    cfg.baseUrl = o["baseUrl"].toString();
    cfg.mpVersion = o["mpVersion"].toInt(0);
    return cfg;
}

AppConfig AppConfig::load() {
    return loadFrom(QCoreApplication::applicationDirPath() + QStringLiteral("/config.json"));
}
