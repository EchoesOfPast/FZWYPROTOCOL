#pragma once

#include <QJsonObject>
#include <QUrlQuery>

#include "Crypto.h"

class QNetworkAccessManager;

class FzwyApi {
public:
    FzwyApi();
    ~FzwyApi();

    QString token;
    QString osValue;
    qlonglong userId = 0;

    bool initCrypto();

    QJsonObject erequest(const QJsonObject &plain);
    QJsonValue dresponse(const QJsonObject &resp);

    QJsonObject get(const QString &path, const QUrlQuery &query = {});
    QJsonObject postPlain(const QString &path, const QJsonObject &body);
    QJsonObject postV2(const QString &path, const QJsonObject &plain);

    static bool isAuthError(const QJsonObject &resp);

    bool loginV2(const QString &code, const QString &iv, const QString &enc,
                 const QString &nick, const QString &avatar, QString *err);
    // Only login/v2-issued tokens pass validation.
    bool validate(QString *uoOut, QString *err);

private:
    QJsonObject httpJson(const QString &method, const QString &path, const QUrlQuery &query,
                         const QByteArray &body, QString *err);

    QByteArray m_serverPubDer;
    crypto::RsaKeyPair m_key;
    QNetworkAccessManager *m_nam = nullptr;
};
