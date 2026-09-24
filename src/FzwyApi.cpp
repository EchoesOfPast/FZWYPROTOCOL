#include "FzwyApi.h"

#include <QDebug>
#include <QEventLoop>
#include <QJsonArray>
#include <QJsonDocument>
#include <QNetworkAccessManager>
#include <QNetworkProxy>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QStringList>
#include <QTimer>

#include "Config.h"

FzwyApi::FzwyApi() : m_nam(new QNetworkAccessManager) {
    // Bypass system proxy: local proxies (Clash/V2Ray/VPN) can hang on these API hosts.
    m_nam->setProxy(QNetworkProxy::NoProxy);
}

FzwyApi::~FzwyApi() { delete m_nam; }

bool FzwyApi::initCrypto() {
    QByteArray spkRaw = QByteArray::fromHex(g_config.serverPubKey.toLatin1());
    QByteArray pt = crypto::aesCbcDecrypt(g_config.aesKey.toLatin1(),
                                          g_config.aesIv.toLatin1(), spkRaw);
    if (pt.isEmpty())
        return false;
    QString pem = QString::fromUtf8(pt);
    pem.remove(QStringLiteral("-----BEGIN PUBLIC KEY-----"))
        .remove(QStringLiteral("-----END PUBLIC KEY-----"))
        .remove('\n')
        .remove('\r')
        .remove(' ');
    m_serverPubDer = QByteArray::fromBase64(pem.toUtf8());
    if (m_serverPubDer.isEmpty())
        return false;
    m_key = crypto::rsaGenerate1024();
    return m_key.ok();
}

QJsonObject FzwyApi::erequest(const QJsonObject &plain) {
    QByteArray s = crypto::rand16();
    QByteArray c = crypto::rand16();
    // RSA input is a quoted JSON string literal, matching the mini-program.
    QByteArray akPlain = QByteArray("\"") + s + "##" + c + "\"";
    QByteArray ak = crypto::rsaEncryptPkcs1WithSpki(m_serverPubDer, akPlain);

    QJsonObject body;
    body["data"] = plain;
    body["rk"] = QString::fromLatin1(m_key.publicKeySpkiDer().toBase64());
    QByteArray bodyJson = QJsonDocument(body).toJson(QJsonDocument::Compact);
    QByteArray sdata = crypto::aesCbcEncrypt(s, c, bodyJson);

    QJsonObject out;
    out["ak"] = QString::fromLatin1(ak.toBase64());
    out["sdata"] = QString::fromLatin1(sdata.toHex().toUpper());
    return out;
}

QJsonValue FzwyApi::dresponse(const QJsonObject &resp) {
    QJsonObject data = resp["data"].toObject();
    QString ak = data["ak"].toString();
    QString sdata = data["sdata"].toString();
    if (ak.isEmpty() || sdata.isEmpty())
        return data;
    QByteArray plain = m_key.decryptPkcs1(QByteArray::fromBase64(ak.toLatin1()));
    if (plain.isEmpty()) {
        qWarning() << "dresponse: RSA decrypt failed (ak invalid or key mismatch)";
        return {};
    }
    QString sc = QString::fromUtf8(plain);
    if (sc.startsWith('"') && sc.endsWith('"') && sc.size() >= 2)
        sc = sc.mid(1, sc.size() - 2);
    int sep = sc.indexOf(QStringLiteral("##"));
    if (sep < 0) {
        qWarning() << "dresponse: '##' separator missing in decrypted key material";
        return {};
    }
    QByteArray s = sc.left(sep).toUtf8();
    QByteArray c = sc.mid(sep + 2).toUtf8();
    QByteArray json = crypto::aesCbcDecrypt(s, c, QByteArray::fromHex(sdata.toLatin1()));
    QJsonParseError pe{};
    QJsonDocument doc = QJsonDocument::fromJson(json, &pe);
    if (pe.error != QJsonParseError::NoError) {
        qWarning() << "dresponse: JSON parse failed:" << pe.errorString();
        return {};
    }
    if (doc.isObject())
        return doc.object();
    if (doc.isArray())
        return doc.array();
    return {};
}

QJsonObject FzwyApi::httpJson(const QString &method, const QString &path, const QUrlQuery &query,
                              const QByteArray &body, QString *err) {
    QUrl url(g_config.baseUrl + path);
    if (!query.isEmpty())
        url.setQuery(query);
    QNetworkRequest req(url);
    // Note: sending userId header causes 401; server extracts user from CToken
    req.setRawHeader("CToken", token.toUtf8());
    req.setRawHeader("version", crypto::md5Hex(osValue.toUtf8()));
    req.setRawHeader("mpCode", g_config.mpCode.toUtf8());
    req.setRawHeader("content-type", "application/json");
    req.setRawHeader("User-Agent", "Mozilla/5.0 MicroMessenger/7.0.20.1781");
    QString referer = QStringLiteral("https://servicewechat.com/%1/%2/page-frame.html")
                          .arg(g_config.appId)
                          .arg(g_config.mpVersion);
    req.setRawHeader("Referer", referer.toUtf8());

    QNetworkReply *reply = nullptr;
    if (method == QLatin1String("GET"))
        reply = m_nam->get(req);
    else
        reply = m_nam->post(req, body);

    QEventLoop loop;
    QTimer timer;
    timer.setSingleShot(true);
    QObject::connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
    QObject::connect(&timer, &QTimer::timeout, &loop, &QEventLoop::quit);
    timer.start(20000);
    loop.exec();
    if (timer.isActive()) {
        timer.stop();
    } else {
        reply->abort();
    }
    int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    QByteArray payload = reply->readAll();
    reply->deleteLater();

    if (status != 200) {
        if (err)
            *err = QStringLiteral("HTTP %1").arg(status);
        return QJsonObject{{"__http_error__", status}};
    }
    QJsonParseError pe{};
    QJsonDocument doc = QJsonDocument::fromJson(payload, &pe);
    if (pe.error != QJsonParseError::NoError || !doc.isObject()) {
        if (err)
            *err = QStringLiteral("Response is not JSON");
        return {};
    }
    return doc.object();
}

bool FzwyApi::isAuthError(const QJsonObject &resp) {
    if (resp["__http_error__"].toInt() == 401)
        return true;
    // Auth failure also arrives as HTTP 200 with a business desc like
    // "Missing request header 'userId'..." — the mini program treats that
    // exact pattern as "clear cache and re-login" too (see PROTOCOL.md section 1).
    const QString desc = resp["desc"].toString();
    return desc.contains(QLatin1String("Missing")) && desc.contains(QLatin1String("userId"));
}

QJsonObject FzwyApi::get(const QString &path, const QUrlQuery &query) {
    return httpJson(QStringLiteral("GET"), path, query, {}, nullptr);
}

QJsonObject FzwyApi::postPlain(const QString &path, const QJsonObject &body) {
    return httpJson(QStringLiteral("POST"), path, {},
                    QJsonDocument(body).toJson(QJsonDocument::Compact), nullptr);
}

QJsonObject FzwyApi::postV2(const QString &path, const QJsonObject &plain) {
    QJsonObject body = erequest(plain);
    QJsonObject j = httpJson(QStringLiteral("POST"), path, {},
                             QJsonDocument(body).toJson(QJsonDocument::Compact), nullptr);
    if (j["success"].toBool() && j["data"].toObject().contains("ak"))
        j["plain"] = dresponse(j);
    return j;
}

bool FzwyApi::loginV2(const QString &code, const QString &iv, const QString &enc,
                      const QString &nick, const QString &avatar, QString *err) {
    QJsonObject plain;
    plain["code"] = code;
    plain["iv"] = iv;
    plain["encryptedData"] = enc;
    plain["appId"] = g_config.appId;
    plain["mpCode"] = g_config.mpCode;
    plain["avatarUrl"] = avatar;
    plain["nickName"] = nick;

    // No token or os during login; version header uses md5(empty string)
    QString savedToken = token;
    QString savedOs = osValue;
    token.clear();
    osValue.clear();
    QJsonObject body = erequest(plain);
    QJsonObject j = httpJson(QStringLiteral("POST"),
                             QStringLiteral("capp/crbac/mp/login/v2"), {},
                             QJsonDocument(body).toJson(QJsonDocument::Compact), err);
    token = savedToken;
    osValue = savedOs;
    if (j.contains("__http_error__"))
        return false;
    if (!j["success"].toBool()) {
        if (err)
            *err = QStringLiteral("Login rejected: %1").arg(j["desc"].toString());
        return false;
    }
    QJsonValue data = dresponse(j);
    QJsonObject d = data.toObject();
    QString tok = d["token"].toString();
    if (tok.isEmpty()) {
        if (err)
            *err = QStringLiteral("login/v2 did not return a token");
        return false;
    }
    token = tok;
    userId = d["userId"].toVariant().toLongLong();
    return true;
}

bool FzwyApi::validate(QString *uoOut, QString *err) {
    // Retry once with empty osValue (no version header), unless it is already empty
    // (both tries would be identical). The returned "uo" refreshes the version header.
    QStringList tries{osValue};
    if (!osValue.isEmpty())
        tries.append(QString());
    for (const QString &osv : tries) {
        QString saved = osValue;
        osValue = osv;
        QString e;
        QJsonObject j = httpJson(
            QStringLiteral("GET"),
            QStringLiteral("capp/businessserver/mp/user/signin/new/1?imageUrl=&nickName="),
            {}, {}, &e);
        osValue = saved;
        if (j.contains("__http_error__"))
            continue;
        if (j["success"].toBool()) {
            QJsonObject data = j["data"].toObject();
            if (uoOut)
                *uoOut = data["uo"].toString();
            return true;
        }
    }
    if (err)
        *err = QStringLiteral("Token validation failed");
    return false;
}
