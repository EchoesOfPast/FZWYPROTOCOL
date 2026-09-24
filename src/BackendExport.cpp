#include "Backend.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QUrlQuery>

#include "FzwyApi.h"
#include "StateStore.h"

namespace {

QString csvCell(const QString &s) {
    if (!s.contains(QLatin1Char('"')) && !s.contains(QLatin1Char(',')) &&
        !s.contains(QLatin1Char('\n')) && !s.contains(QLatin1Char('\r')))
        return s;
    QString q = s;
    q.replace(QLatin1Char('"'), QStringLiteral("\"\""));
    return QStringLiteral("\"%1\"").arg(q);
}

QString jsonToText(const QJsonValue &v) {
    if (v.isString())
        return v.toString();
    if (v.isDouble()) {
        const double d = v.toDouble();
        const qlonglong i = static_cast<qlonglong>(d);
        return d == static_cast<double>(i) ? QString::number(i) : QString::number(d);
    }
    if (v.isBool())
        return v.toBool() ? QStringLiteral("true") : QStringLiteral("false");
    if (v.isObject())
        return QString::fromUtf8(QJsonDocument(v.toObject()).toJson(QJsonDocument::Compact));
    if (v.isArray())
        return QString::fromUtf8(QJsonDocument(v.toArray()).toJson(QJsonDocument::Compact));
    return {};
}

// Response schemas of the export endpoints are undocumented, so every field is
// looked up by a list of candidate names; missing fields just export empty.
QString pickField(const QJsonObject &o, std::initializer_list<const char *> keys) {
    for (const char *k : keys) {
        const QString s = jsonToText(o.value(QLatin1String(k)));
        if (!s.isEmpty())
            return s;
    }
    return {};
}

bool writeFile(const QString &path, const QByteArray &content, QString *err) {
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        if (err)
            *err = f.errorString();
        return false;
    }
    if (f.write(content) != content.size() || !f.flush()) {
        if (err)
            *err = f.errorString();
        return false;
    }
    return true;
}

}  // namespace

bool Backend::exportWrongBook() {
    const auto lg = [this](const QString &m) { if (log) log(m); };
    const QJsonObject s = loadState();
    if (s["token"].toString().isEmpty()) {
        lg(QStringLiteral("未登录，无法导出错题本。请先获取 Token。"));
        return false;
    }

    FzwyApi api;
    api.token = s["token"].toString();
    api.osValue = s["os_value"].toString();
    api.userId = s["user_id"].toVariant().toLongLong();

    lg(QStringLiteral("导出错题本…"));
    static const struct {
        const char *path;
        const char *type;
    } endpoints[] = {
        {"capp/businessserver/mp/query/wrong_exam/word", "word"},
        {"capp/businessserver/mp/query/wrong_exam/grammar", "grammar"},
        {"capp/businessserver/mp/query/wrong_exam/listen", "listen"},
    };

    struct Row {
        QString type, word, meaning, sentence, audio;
    };
    QList<Row> rows;
    int counts[] = {0, 0, 0};
    QJsonObject raw;  // original responses, kept so unknown fields are not lost

    for (int i = 0; i < 3; ++i) {
        const QString type = QString::fromLatin1(endpoints[i].type);
        const QJsonObject resp = api.get(QString::fromLatin1(endpoints[i].path));
        if (FzwyApi::isAuthError(resp)) {
            lg(QStringLiteral("登录态已失效，请先重新获取 Token。"));
            return false;
        }
        raw[type] = resp;
        if (resp.contains("__http_error__")) {
            lg(QStringLiteral("  %1: HTTP %2，计 0 条")
                   .arg(type)
                   .arg(resp["__http_error__"].toInt()));
            continue;
        }
        for (const auto &item : resp["data"].toArray()) {
            const QJsonObject o = item.toObject();
            rows.append({type,
                         pickField(o, {"word", "name", "spellWord", "spell_word"}),
                         pickField(o, {"means", "translation", "translate", "中文", "mean"}),
                         pickField(o, {"sentence"}),
                         pickField(o, {"soundUrl", "audio", "audioUrl", "sound"})});
            ++counts[i];
        }
        lg(QStringLiteral("  %1: %2 条").arg(type).arg(counts[i]));
    }

    const QString ts = QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMdd-HHmmss"));
    const QString dir = QCoreApplication::applicationDirPath();
    const QString csvPath = dir + QStringLiteral("/wrongbook-%1.csv").arg(ts);
    const QString jsonPath = dir + QStringLiteral("/wrongbook-%1.json").arg(ts);

    QByteArray csv("\xEF\xBB\xBF", 3);  // BOM so Excel reads the UTF-8 correctly
    csv += "type,word,meaning,sentence,audio\r\n";
    for (const Row &r : rows) {
        csv += csvCell(r.type).toUtf8() + ',' + csvCell(r.word).toUtf8() + ',' +
               csvCell(r.meaning).toUtf8() + ',' + csvCell(r.sentence).toUtf8() + ',' +
               csvCell(r.audio).toUtf8() + "\r\n";
    }
    QString err;
    if (!writeFile(csvPath, csv, &err)) {
        lg(QStringLiteral("写出 CSV 失败：%1").arg(err));
        return false;
    }
    if (!writeFile(jsonPath, QJsonDocument(raw).toJson(), &err)) {
        lg(QStringLiteral("写出原始 JSON 失败：%1").arg(err));
        return false;
    }
    lg(QStringLiteral("  [OK] 错题本已导出 word=%1 grammar=%2 listen=%3")
           .arg(counts[0])
           .arg(counts[1])
           .arg(counts[2]));
    lg(QStringLiteral("  CSV: %1").arg(QDir::toNativeSeparators(csvPath)));
    lg(QStringLiteral("  JSON: %1").arg(QDir::toNativeSeparators(jsonPath)));
    return true;
}

bool Backend::exportStudyReport() {
    const auto lg = [this](const QString &m) { if (log) log(m); };
    const QJsonObject s = loadState();
    if (s["token"].toString().isEmpty()) {
        lg(QStringLiteral("未登录，无法导出学情报告。请先获取 Token。"));
        return false;
    }

    FzwyApi api;
    api.token = s["token"].toString();
    api.osValue = s["os_value"].toString();
    api.userId = s["user_id"].toVariant().toLongLong();
    // Needed only for the encrypted task/query/data/v2 call; on failure that
    // one section degrades to 0 items, the rest still exports
    const bool cryptoOk = api.initCrypto();

    lg(QStringLiteral("导出学情报告…"));
    QJsonObject raw;  // original responses of every source
    const auto aborted = [&lg](const QJsonObject &resp) {
        if (!FzwyApi::isAuthError(resp))
            return false;
        lg(QStringLiteral("登录态已失效，请先重新获取 Token。"));
        return true;
    };

    // 1. user info
    const QJsonObject userInfo = api.get(QStringLiteral("capp/businessserver/mp/user/info/get"));
    if (aborted(userInfo))
        return false;
    raw["userInfo"] = userInfo;

    // 2. gold coin (data may be a bare number or an object)
    const QJsonObject gold = api.get(QStringLiteral("capp/businessserver/mp/user/gold/coin/get"));
    if (aborted(gold))
        return false;
    raw["goldCoin"] = gold;
    QString goldNum;
    const QJsonValue gd = gold["data"];
    if (gd.isDouble())
        goldNum = jsonToText(gd);
    else if (gd.isObject())
        goldNum = pickField(gd.toObject(), {"coin", "gold", "goldCoin", "balance", "total"});

    // 3. task progress (encrypted); numeric keys are flattened as-is
    QList<QPair<QString, QString>> progressKv;
    if (cryptoOk) {
        const QJsonObject taskResp =
            api.postV2(QStringLiteral("capp/businessserver/mp/task/query/data/v2"), {});
        if (aborted(taskResp))
            return false;
        raw["taskData"] = taskResp;
        const QJsonObject plain = taskResp["plain"].toObject();
        for (auto it = plain.begin(); it != plain.end(); ++it)
            if (it.value().isDouble())
                progressKv.append({it.key(), jsonToText(it.value())});
        lg(QStringLiteral("  任务进度: %1 项").arg(progressKv.size()));
    } else {
        lg(QStringLiteral("  密钥初始化失败，任务进度段记 0 项"));
    }

    // 4. weekly status; numeric keys join the progress section with a prefix
    const QJsonObject weekly = api.get(QStringLiteral("capp/businessserver/mp/weekly/status"));
    if (aborted(weekly))
        return false;
    raw["weeklyStatus"] = weekly;
    const QJsonObject wd = weekly["data"].toObject();
    for (auto it = wd.begin(); it != wd.end(); ++it)
        if (it.value().isDouble())
            progressKv.append({QStringLiteral("weekly.%1").arg(it.key()), jsonToText(it.value())});

    // 5. team member learn record, only when the account is in a team
    QString teamId;
    QList<QJsonObject> teamRows;
    const QString pkg = s["package_uuid"].toString();
    if (pkg.isEmpty()) {
        lg(QStringLiteral("  无词包信息，跳过小组学习记录"));
    } else {
        QUrlQuery q;
        q.addQueryItem(QStringLiteral("packageId"), pkg);
        const QJsonObject team =
            api.get(QStringLiteral("capp/businessserver/mp/package/task/team"), q);
        if (aborted(team))
            return false;
        raw["team"] = team;
        const QJsonArray arr = team["data"].toArray();
        if (!arr.isEmpty())
            teamId = pickField(arr.first().toObject(), {"id", "teamId"});
        if (teamId.isEmpty()) {
            lg(QStringLiteral("  未加入小组，跳过小组学习记录"));
        } else {
            QUrlQuery q2;
            q2.addQueryItem(QStringLiteral("teamId"), teamId);
            const QJsonObject rec =
                api.get(QStringLiteral("capp/datacenterserver/mp/v2/memberLearnRecord"), q2);
            if (aborted(rec))
                return false;
            raw["memberLearnRecord"] = rec;
            for (const auto &v : rec["data"].toArray()) {
                if (v.isObject())
                    teamRows.append(v.toObject());
            }
            lg(QStringLiteral("  小组学习记录: %1 条").arg(teamRows.size()));
        }
    }

    const QString ts = QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMdd-HHmmss"));
    const QString dir = QCoreApplication::applicationDirPath();
    const QString csvPath = dir + QStringLiteral("/study-report-%1.csv").arg(ts);
    const QString jsonPath = dir + QStringLiteral("/study-report-%1.json").arg(ts);

    const qlonglong uid = s["user_id"].toVariant().toLongLong();
    QString userName = s["user_name"].toString();
    if (userName.isEmpty()) {
        QJsonObject row;
        const QJsonValue ud = userInfo["data"];
        const QJsonArray ua = ud.toArray();
        row = ua.isEmpty() ? ud.toObject() : ua.first().toObject();
        userName = pickField(row, {"realName", "userName", "nickName"});
    }

    QByteArray csv("\xEF\xBB\xBF", 3);  // BOM so Excel reads the UTF-8 correctly
    csv += "账户\r\n";
    csv += QByteArray("uid,") + csvCell(QString::number(uid)).toUtf8() + "\r\n";
    csv += QByteArray("userName,") + csvCell(userName).toUtf8() + "\r\n";
    csv += QByteArray("goldCoin,") + csvCell(goldNum).toUtf8() + "\r\n";
    csv += QByteArray("packageUuid,") + csvCell(pkg).toUtf8() + "\r\n";
    csv += "\r\n任务进度\r\nkey,value\r\n";
    for (const auto &kv : progressKv)
        csv += csvCell(kv.first).toUtf8() + ',' + csvCell(kv.second).toUtf8() + "\r\n";

    if (!teamRows.isEmpty()) {
        csv += "\r\n小组学习记录\r\n";
        QStringList cols;  // ordered union of keys across all rows
        for (const QJsonObject &row : teamRows) {
            for (auto it = row.begin(); it != row.end(); ++it)
                if (!cols.contains(it.key()))
                    cols.append(it.key());
        }
        QStringList header;
        for (const QString &c : cols)
            header << csvCell(c);
        csv += header.join(QStringLiteral(",")).toUtf8() + "\r\n";
        for (const QJsonObject &row : teamRows) {
            QStringList cells;
            for (const QString &c : cols)
                cells << csvCell(jsonToText(row.value(c)));
            csv += cells.join(QStringLiteral(",")).toUtf8() + "\r\n";
        }
    }

    QString err;
    if (!writeFile(csvPath, csv, &err)) {
        lg(QStringLiteral("写出 CSV 失败：%1").arg(err));
        return false;
    }
    if (!writeFile(jsonPath, QJsonDocument(raw).toJson(), &err)) {
        lg(QStringLiteral("写出原始 JSON 失败：%1").arg(err));
        return false;
    }
    QStringList doneItems;
    for (const auto &kv : progressKv)
        if (kv.first.contains(QLatin1String("Done")) && kv.second != QLatin1String("0"))
            doneItems << QStringLiteral("%1=%2").arg(kv.first, kv.second);
    lg(QStringLiteral("  [OK] 学情报告已导出：金币 %1，已完成 %2")
           .arg(goldNum.isEmpty() ? QStringLiteral("?") : goldNum,
                doneItems.isEmpty() ? QStringLiteral("(无)") : doneItems.join(QStringLiteral(", "))));
    lg(QStringLiteral("  CSV: %1").arg(QDir::toNativeSeparators(csvPath)));
    lg(QStringLiteral("  JSON: %1").arg(QDir::toNativeSeparators(jsonPath)));
    return true;
}
