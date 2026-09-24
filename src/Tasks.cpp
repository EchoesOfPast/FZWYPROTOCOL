#include "Tasks.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QFile>
#include <QJsonDocument>
#include <QThread>
#include <QUrlQuery>

#include "StateStore.h"

namespace {

qint64 nowSec() { return QDateTime::currentSecsSinceEpoch(); }

QJsonValue jv(const QJsonObject &o, const char *k) { return o.value(QLatin1String(k)); }

QString js(const QJsonObject &o, const char *k) {
    QJsonValue v = o.value(QLatin1String(k));
    if (v.isString())
        return v.toString();
    if (v.isDouble())
        return QString::number(v.toVariant().toLongLong());
    return {};
}

QString preview(const QJsonValue &v, int n) {
    QByteArray b;
    if (v.isObject())
        b = QJsonDocument(v.toObject()).toJson(QJsonDocument::Compact);
    else if (v.isArray())
        b = QJsonDocument(v.toArray()).toJson(QJsonDocument::Compact);
    else
        b = v.toVariant().toString().toUtf8();
    return QString::fromUtf8(b).left(n);
}

}  // namespace

Tasks::Tasks(std::function<void(const QString &)> logFn) : log(std::move(logFn)) {
    st = loadState();
    uid = st["user_id"].toVariant().toLongLong();
    pkg = st["package_uuid"].toString();
    userName = st["user_name"].toString();
    api.token = st["token"].toString();
    api.osValue = st["os_value"].toString();
    api.userId = uid;
    cryptoOk = api.initCrypto();
    if (!cryptoOk)
        log(QStringLiteral("密钥初始化失败，请检查 config.json（aesKey/aesIv/serverPubKey）"));

    if (userName.isEmpty())
        userName = fetchUserName();
    if (!uid) {
        QJsonObject r = api.get(QStringLiteral("capp/businessserver/mp/user/info/get"));
        QJsonValue data = r["data"];
        QJsonObject row = data.isArray() && !data.toArray().isEmpty()
                              ? data.toArray().first().toObject()
                              : data.toObject();
        qlonglong id = row["userId"].toVariant().toLongLong();
        if (id) {
            uid = id;
            api.userId = id;
            st["user_id"] = static_cast<double>(id);
            saveState(st);
            log(QStringLiteral("  userId=%1").arg(id));
        }
    }
    if (pkg.isEmpty()) {
        log(QStringLiteral("自动探测词包…"));
        pkg = discoverPackageUuid();
        if (!pkg.isEmpty()) {
            st["package_uuid"] = pkg;
            saveState(st);
            log(QStringLiteral("  packageUuid=%1").arg(pkg));
        } else {
            log(QStringLiteral("  探测失败：当前账号没有可用词包，或登录态无效"));
        }
    }
}

void Tasks::step(const QString &name, bool ok, const QString &extra) {
    if (ok)
        ++okCount;
    else
        ++failCount;
    log(QStringLiteral("  [%1] %2 %3").arg(ok ? "OK" : "FAIL", name, extra));
}

QString Tasks::fetchUserName() {
    QJsonObject r = api.get(QStringLiteral("capp/businessserver/mp/user/info/get"));
    QJsonValue data = r["data"];
    QJsonObject row = data.isArray() && !data.toArray().isEmpty()
                          ? data.toArray().first().toObject()
                          : data.toObject();
    QString name = js(row, "realName");
    if (name.isEmpty())
        name = js(row, "userName");
    if (!name.isEmpty()) {
        st["user_name"] = name;
        saveState(st);
        log(QStringLiteral("  userName=%1").arg(name));
    }
    return name;
}

QJsonObject Tasks::userRec(const QJsonValue &taskId, const QJsonValue &cardId,
                           const QString &cardType, qint64 learnTime) {
    return QJsonObject{
        {"schoolId", ""},
        {"userId", static_cast<double>(uid)},
        {"userName", userName},
        {"taskId", taskId},
        {"learnTime", static_cast<double>(learnTime)},
        {"taskName", ""},
        {"cardId", cardId},
        {"tleaderName", ""},
        {"teamId", ""},
        {"teamName", ""},
        {"tleaderId", ""},
        {"departmentId", ""},
        {"channel", 1},
        {"cardType", cardType},
    };
}

// card/package/set/query is a plaintext GET (no ERequest); data[].upgs[].packageId
// holds the real card-package id (packageUuid).
QStringList Tasks::packageIdsFromGroup(const QString &groupId) {
    QUrlQuery q;
    q.addQueryItem(QStringLiteral("groupId"), groupId);
    QJsonObject r =
        api.get(QStringLiteral("capp/businessserver/mp/card/package/set/query"), q);
    QStringList out;
    for (const auto &set : r["data"].toArray()) {
        for (const auto &u : jv(set.toObject(), "upgs").toArray()) {
            QString pid = js(u.toObject(), "packageId");
            if (!pid.isEmpty() && !out.contains(pid))
                out << pid;
        }
    }
    return out;
}

bool Tasks::verifyPackageCandidate(const QString &cand) {
    QJsonObject rr = api.postV2(QStringLiteral("capp/wordserver/mp/word/get/card/v2"),
                                QJsonObject{{"packageUuid", cand}});
    return rr["success"].toBool() &&
           !jv(rr["plain"].toObject(), "wordDtos").toArray().isEmpty();
}

QString Tasks::discoverPackageUuid() {
    QStringList seen;
    auto tryCandidates = [&](const QString &src, const QStringList &cands) -> QString {
        QStringList fresh;
        for (const QString &c : cands)
            if (!c.isEmpty() && !seen.contains(c)) {
                seen << c;
                fresh << c;
            }
        log(QStringLiteral("  词包候选来源 %1：%2 个").arg(src).arg(fresh.size()));
        for (const QString &cand : fresh.mid(0, 20))
            if (verifyPackageCandidate(cand)) {
                log(QStringLiteral("  词包探测成功 %1（来源 %2）").arg(cand, src));
                return cand;
            }
        return {};
    };

    // 1) package/show/v2 returns a word-book group id, not a card-package id;
    //    resolve group -> packages via set/query
    {
        QJsonObject r = api.postV2(QStringLiteral("capp/businessserver/mp/package/show/v2"),
                                   QJsonObject{{"type", 1}});
        QJsonObject plain = jv(r, "plain").toObject();
        QStringList cands;
        QString direct = js(plain, "packageUuid");
        if (!direct.isEmpty())
            cands << direct;
        QString groupId = js(plain, "groupId");
        if (groupId.isEmpty())
            groupId = js(plain, "recordId");
        if (!groupId.isEmpty()) {
            log(QStringLiteral("  词书组 show/v2.groupId=%1，经 set/query 解析卡包").arg(groupId));
            cands += packageIdsFromGroup(groupId);
        }
        QString hit = tryCandidates(QStringLiteral("show/v2 + set/query"), cands);
        if (!hit.isEmpty())
            return hit;
    }
    // 2) group/query/type/v2 also returns word-book groups; same set/query hop
    {
        QJsonObject r = api.postV2(QStringLiteral("capp/businessserver/mp/group/query/type/v2"),
                                   QJsonObject{{"type", 1}});
        QJsonValue plain = jv(r, "plain");
        QJsonArray groups;
        if (plain.isArray())
            groups = plain.toArray();
        if (plain.isObject()) {
            for (const auto &v : plain.toObject())
                if (v.isArray())
                    for (const auto &x : v.toArray())
                        groups.append(x);
        }
        QStringList groupIds;
        for (const auto &g : groups) {
            QJsonObject go = g.toObject();
            QString rid = js(go, "recordId");
            if (rid.isEmpty())
                rid = js(go, "groupId");
            if (!rid.isEmpty())
                groupIds << rid;
            for (const auto &sub : jv(go, "groups").toArray()) {
                QString sr = js(sub.toObject(), "recordId");
                if (!sr.isEmpty())
                    groupIds << sr;
            }
        }
        groupIds.removeDuplicates();
        log(QStringLiteral("  词书组 group/query/type/v2：%1 个，经 set/query 解析卡包")
                .arg(groupIds.size()));
        QStringList cands;
        for (const QString &gid : groupIds.mid(0, 6))
            cands += packageIdsFromGroup(gid);
        QString hit =
            tryCandidates(QStringLiteral("group/query/type/v2 + set/query"), cands);
        if (!hit.isEmpty())
            return hit;
    }
    // 3) task/queryall/v2: everydayTaskDto[].id is directly a card-package id
    {
        QJsonObject r = api.postV2(QStringLiteral("capp/businessserver/mp/task/queryall/v2"),
                                   QJsonObject{{"type", 1}});
        QJsonValue plain = jv(r, "plain");
        QStringList cands;
        if (plain.isArray()) {
            for (const auto &e : plain.toArray()) {
                for (const auto &t : jv(e.toObject(), "everydayTaskDto").toArray()) {
                    QString id = js(t.toObject(), "id");
                    if (!id.isEmpty())
                        cands << id;
                }
            }
        }
        QString hit = tryCandidates(QStringLiteral("task/queryall/v2"), cands);
        if (!hit.isEmpty())
            return hit;
    }
    return {};
}

QJsonObject Tasks::discover() {
    QJsonObject d;
    QJsonArray teamIds;
    {
        QUrlQuery q;
        q.addQueryItem("packageId", pkg);
        QJsonObject r = api.get(QStringLiteral("capp/businessserver/mp/package/task/team"), q);
        if (r["data"].isArray()) {
            for (const auto &x : r["data"].toArray())
                teamIds.append(x.toVariant().toString());
        }
        step(QStringLiteral("discover team ids"), r["success"].toBool(),
             QString::fromUtf8(QJsonDocument(teamIds).toJson(QJsonDocument::Compact)));
    }
    d["team_ids"] = teamIds;
    {
        QJsonObject r = api.postV2(QStringLiteral("capp/businessserver/mp/task/query/data/v2"),
                                   {});
        d["tasks"] = jv(r, "plain");
        step(QStringLiteral("discover tasks"), r["success"].toBool(),
             preview(jv(r, "plain"), 180));
    }
    {
        QJsonObject r = api.postV2(QStringLiteral("capp/businessserver/mp/listening/package/config"),
                                   {});
        d["listen_cfg"] = jv(r, "plain");
        step(QStringLiteral("listening/config"), r["success"].toBool(),
             preview(jv(r, "plain"), 120));
    }
    {
        QJsonObject r = api.postV2(QStringLiteral("capp/businessserver/mp/teamtest/queryallnow/v2"),
                                   {});
        d["exam_now"] = jv(r, "plain");
        step(QStringLiteral("teamtest/queryallnow"), r["success"].toBool(),
             preview(jv(r, "plain"), 160));
    }
    {
        QUrlQuery q;
        q.addQueryItem("packageId", pkg);
        QJsonObject r = api.get(QStringLiteral("capp/businessserver/mp/exam/task/team"), q);
        d["exam_task_team"] = r["data"];
        step(QStringLiteral("exam/task/team"), r["success"].toBool(), preview(r["data"], 120));
    }
    return d;
}

void Tasks::doWords(int n) {
    log(QStringLiteral("== 单词 =="));
    QJsonObject r = api.postV2(QStringLiteral("capp/wordserver/mp/word/get/card/v2"),
                               QJsonObject{{"packageUuid", pkg}});
    QJsonArray words = jv(r["plain"].toObject(), "wordDtos").toArray();
    step(QStringLiteral("word/get/card"), r["success"].toBool(),
         QStringLiteral("n=%1").arg(words.size()));
    for (int i = 0; i < qMin<int>(n, words.size()); ++i) {
        if (cancelled() || api.authFailureSeen)
            return;
        QJsonObject w = words[i].toObject();
        qint64 t0 = nowSec();
        QJsonObject item{
            {"userId", static_cast<double>(uid)},
            {"packageId", pkg},
            {"wordId", jv(w, "wordId")},
            {"word", jv(w, "name")},
            {"wordType", 1},
            {"clientStartTime", static_cast<double>(t0)},
            {"clientEndTime", static_cast<double>(t0 + 3)},
            {"effective", true},
            {"stayTime", 3},
            {"flipCount", 2},
            {"playCount", 1},
            {"flipTimes", QStringLiteral("%1,%2").arg(t0).arg(t0 + 1)},
            {"testAnswer", 1},
            {"testAnswerCorrect", true},
        };
        QJsonObject sr{
            {"userId", static_cast<double>(uid)},
            {"packageId", pkg},
            {"progress", i + 1},
            {"startTime", static_cast<double>(t0)},
            {"endTime", static_cast<double>(t0 + 3)},
            {"wordType", 1},
            {"cardId", jv(w, "wordId")},
            {"testChooseCorrect", true},
            {"testAnswer", 1},
        };
        QJsonObject a = api.postPlain(
            QStringLiteral("capp/datacenterserver/mp/learn_word/study_word"), item);
        QJsonObject b = api.postPlain(
            QStringLiteral("capp/datacenterserver/mp/learn_card/record"),
            userRec(pkg, jv(w, "wordId"), QStringLiteral("word")));
        QJsonObject d = api.postV2(QStringLiteral("capp/wordserver/mp/word/run/v2"), sr);
        step(QStringLiteral("word[%1]%2").arg(i).arg(js(w, "name")),
             a["success"].toBool() && b["success"].toBool() && d["success"].toBool());
        QThread::msleep(100);
    }
}

void Tasks::doSpellFill(const QString &teamId, int n) {
    log(QStringLiteral("== 拼写/填空 =="));
    {
        QJsonObject r = api.postV2(QStringLiteral("capp/wordserver/mp/word/spelling/get/v2"),
                                   QJsonObject{{"taskId", teamId}});
        QJsonArray wl = jv(r["plain"].toObject(), "wordDtos").toArray();
        step(QStringLiteral("spell/get"), r["success"].toBool(),
             QStringLiteral("n=%1").arg(wl.size()));
        for (int i = 0; i < qMin<int>(n, wl.size()); ++i) {
            if (cancelled() || api.authFailureSeen)
                return;
            QJsonObject w = wl[i].toObject();
            QString name = js(w, "name");
            QJsonValue cid = jv(w, "recordId").isUndefined() ? jv(w, "wordId") : jv(w, "recordId");
            QJsonObject d = api.postV2(
                QStringLiteral("capp/wordserver/mp/word/spelling/run/v2"),
                QJsonObject{{"cardId", cid},
                            {"userId", static_cast<double>(uid)},
                            {"taskId", teamId},
                            {"progress", i + 1}});
            QJsonObject e = api.postPlain(
                QStringLiteral("capp/datacenterserver/mp/record/spell_word"),
                QJsonObject{{"groupId", js(w, "groupId")},
                            {"spellWord", name},
                            {"stayTime", 3},
                            {"taskId", teamId},
                            {"teamId", ""},
                            {"userId", static_cast<double>(uid)},
                            {"word", name},
                            {"wordId", cid}});
            step(QStringLiteral("spell[%1]").arg(i),
                 d["success"].toBool() && e["success"].toBool());
        }
        if (!wl.isEmpty()) {
            QJsonObject f = api.postV2(
                QStringLiteral("capp/businessserver/mp/word/spell/finish/day/v2"),
                QJsonObject{{"teamTaskId", teamId}});
            step(QStringLiteral("spell/finish"), f["success"].toBool(), js(f, "desc"));
        }
    }
    {
        QJsonObject r = api.postV2(QStringLiteral("capp/wordserver/mp/word/fill/get/v2"),
                                   QJsonObject{{"taskId", teamId}});
        QJsonArray wl = jv(r["plain"].toObject(), "wordDtos").toArray();
        step(QStringLiteral("fill/get"), r["success"].toBool(),
             QStringLiteral("n=%1").arg(wl.size()));
        for (int i = 0; i < qMin<int>(n, wl.size()); ++i) {
            if (cancelled() || api.authFailureSeen)
                return;
            QJsonObject w = wl[i].toObject();
            QJsonValue cid = jv(w, "cardId");
            if (cid.isUndefined())
                cid = jv(w, "recordId");
            if (cid.isUndefined())
                cid = jv(w, "wordId");
            QJsonObject d = api.postV2(
                QStringLiteral("capp/wordserver/mp/word/fill/run/v2"),
                QJsonObject{{"cardId", cid},
                            {"testChooseCorrect", true},
                            {"taskId", teamId}});
            QJsonObject e = api.postPlain(
                QStringLiteral("capp/datacenterserver/mp/record/fill_sentence"),
                QJsonObject{{"groupId", js(w, "groupId")},
                            {"selectOption", 1},
                            {"sentence", js(w, "sentence")},
                            {"sentenceId", js(w, "sentenceId")},
                            {"stayTime", 3},
                            {"taskId", teamId},
                            {"teamId", ""},
                            {"userId", static_cast<double>(uid)},
                            {"word", js(w, "name")},
                            {"wordId", cid}});
            step(QStringLiteral("fill[%1]").arg(i),
                 d["success"].toBool() && e["success"].toBool());
        }
        if (!wl.isEmpty()) {
            QJsonObject f = api.postV2(
                QStringLiteral("capp/businessserver/mp/word/fill/finish/day/v2"),
                QJsonObject{{"teamTaskId", teamId}});
            step(QStringLiteral("fill/finish"), f["success"].toBool(), js(f, "desc"));
        }
    }
}

void Tasks::doListen(const QString &packageId, int n) {
    log(QStringLiteral("== 听力 =="));
    QJsonObject r = api.postV2(QStringLiteral("capp/businessserver/mp/listening/get/card/v2"),
                               QJsonObject{{"packageId", packageId}});
    QJsonObject plain = r["plain"].toObject();
    QJsonArray cards = jv(plain, "cards").toArray();
    step(QStringLiteral("listening/get/card"), r["success"].toBool(),
         QStringLiteral("n=%1 desc=%2").arg(cards.size()).arg(js(r, "desc")));
    for (int i = 0; i < qMin<int>(n, cards.size()); ++i) {
        if (cancelled() || api.authFailureSeen)
            return;
        QJsonObject card = cards[i].toObject();
        qint64 t0 = nowSec();
        QJsonValue cid = jv(card, "cardId");
        if (cid.isUndefined())
            cid = jv(card, "id");
        if (cid.isUndefined())
            cid = jv(card, "recordId");
        QJsonObject run{
            {"userId", static_cast<double>(uid)},
            {"packageId", packageId},
            {"cardId", cid},
            {"progress", i + 1},
            {"startTime", static_cast<double>(t0)},
            {"endTime", static_cast<double>(t0 + 5)},
            {"testChooseCorrect", true},
            {"stayTime", 5},
        };
        QJsonObject d = api.postV2(QStringLiteral("capp/businessserver/mp/listening/run/v2"), run);
        QJsonObject rec = api.postPlain(
            QStringLiteral("capp/datacenterserver/mp/record/listening"),
            QJsonObject{{"userId", static_cast<double>(uid)},
                        {"packageId", packageId},
                        {"cardId", cid},
                        {"stayTime", 5},
                        {"progress", i + 1}});
        QJsonObject lcard = api.postPlain(
            QStringLiteral("capp/datacenterserver/mp/learn_card/record"),
            userRec(packageId, cid, QStringLiteral("listen")));
        step(QStringLiteral("listen[%1]").arg(i),
             d["success"].toBool() && rec["success"].toBool() && lcard["success"].toBool(),
             js(d, "desc"));
    }
}

void Tasks::doReading(const QString &packageId) {
    log(QStringLiteral("== 阅读 =="));
    qint64 t0 = nowSec(), t1 = t0 + 8;
    QJsonObject run{
        {"userId", static_cast<double>(uid)},
        {"startTime", static_cast<double>(t0)},
        {"endTime", static_cast<double>(t1)},
        {"packageId", packageId},
        {"progress", 1},
    };
    QJsonObject d = api.postV2(QStringLiteral("capp/businessserver/mp/reading/page/run/v2"), run);
    step(QStringLiteral("reading/page/run"), d["success"].toBool(), js(d, "desc"));
    QJsonObject e = api.postPlain(QStringLiteral("capp/datacenterserver/mp/reading/record"),
                                  QJsonObject{{"userId", static_cast<double>(uid)},
                                              {"packageId", packageId},
                                              {"stayTime", 8},
                                              {"progress", 1}});
    step(QStringLiteral("reading/record"), e["success"].toBool(), js(e, "desc"));
}

void Tasks::doExams() {
    log(QStringLiteral("== 测试类 =="));
    {
        QJsonObject r = api.postV2(QStringLiteral("capp/businessserver/mp/query/exam/list/v2"),
                                   QJsonObject{{"type", 1}});
        step(QStringLiteral("exam/list"), r["success"].toBool(), preview(jv(r, "plain"), 140));
    }
    struct ExamGetter {
        const char *path;
        int examType;
    };
    static const ExamGetter kGetters[] = {
        {"word/exam/get/v2", 1},
        {"grammer/exam/get/v2", 2},
        {"wordfill/exam/get/v2", 3},
        {"speaking/exam/get/new/v2", 4},
        {"chtoen/exam/get/v2", 5},
    };
    for (const auto &g : kGetters) {
        QString path = QString::fromLatin1(g.path);
        QJsonObject body{
            {"examTemplateId", ""}, {"examType", g.examType}, {"groupId", pkg}};
        QJsonObject r =
            api.postV2(QStringLiteral("capp/businessserver/mp/%1").arg(path), body);
        QJsonValue plain = jv(r, "plain");
        step(path, r["success"].toBool(),
             plain.isObject() ? preview(plain, 100) : js(r, "desc"));
        if (!r["success"].toBool() || !plain.isObject())
            continue;
        QJsonObject po = plain.toObject();
        QJsonArray tests = jv(po, "test").toArray();
        if (tests.isEmpty())
            tests = jv(po, "list").toArray();
        if (tests.isEmpty())
            continue;
        QString tid = js(po, "examTaskId");
        if (tid.isEmpty())
            tid = js(po, "cgExamTaskId");
        if (tid.isEmpty())
            tid = js(po, "taskId");
        QString gid = js(po, "groupId");
        if (gid.isEmpty())
            gid = pkg;
        QJsonObject item = tests.first().toObject();
        QJsonValue cid = jv(item, "id");
        if (cid.isUndefined())
            cid = jv(item, "cardId");
        if (cid.isUndefined())
            cid = jv(item, "entityId");
        QJsonObject runBody{
            {"cardId", cid},
            {"userId", static_cast<double>(uid)},
            {"taskId", tid},
            {"progress", 1},
            {"testChooseCorrect", true},
            {"testAnswer", jv(item, "correctA").isUndefined() ? QJsonValue(1)
                                                              : jv(item, "correctA")},
        };
        QString runPath = path;
        runPath.replace(QStringLiteral("/get/new/v2"), QStringLiteral("/run/v2"));
        runPath.replace(QStringLiteral("/get/v2"), QStringLiteral("/run/v2"));
        QJsonObject rr =
            api.postV2(QStringLiteral("capp/businessserver/mp/%1").arg(runPath), runBody);
        step(QStringLiteral("  %1").arg(runPath), rr["success"].toBool(), js(rr, "desc"));
        QJsonObject fin = api.postV2(
            QStringLiteral("capp/businessserver/mp/exam/finish/v2"),
            QJsonObject{{"category", 1},
                        {"cgExamTaskId", tid},
                        {"examType", g.examType},
                        {"groupId", gid}});
        step(QStringLiteral("  exam/finish(%1)").arg(path.left(12)), fin["success"].toBool(),
             js(fin, "desc"));
    }
}

void Tasks::doFinish() {
    log(QStringLiteral("== 词包完成 =="));
    {
        QJsonObject fin{
            {"finishCount", 1},       {"schoolId", ""},
            {"taskCount", 1},         {"taskId", pkg},
            {"taskName", ""},         {"teamId", ""},
            {"teamName", ""},         {"tleaderName", ""},
            {"userId", static_cast<double>(uid)},
            {"userName", userName},   {"departmentId", ""},
            {"cardPackageId", pkg},   {"packageType", 1},
        };
        QJsonObject r = api.postV2(
            QStringLiteral("capp/businessserver/mp/package/finish/new/v2"), fin);
        step(QStringLiteral("package/finish"), r["success"].toBool(), js(r, "desc"));
        QJsonObject r2 = api.postV2(
            QStringLiteral("capp/businessserver/mp/team/task/package/finish/add/v2"),
            QJsonObject{{"packageId", pkg}});
        step(QStringLiteral("team package finish"), r2["success"].toBool(), js(r2, "desc"));
    }
    {
        QJsonObject r = api.postPlain(
            QStringLiteral("capp/businessserver/mp/user/gold/coin/budget"),
            QJsonObject{{"budgetSource", QStringLiteral("任务完成")},
                        {"budgetType", 0},
                        {"goldCoin", 100},
                        {"signIn", true},
                        {"timeStamp", static_cast<double>(nowSec())},
                        {"userId", static_cast<double>(uid)}});
        step(QStringLiteral("gold"), r["success"].toBool());
    }
}

void Tasks::runAll(int words, int spell, int fill, int listen,
                   const std::function<bool()> &cancelReq) {
    cancelRequested = cancelReq;
    log(QStringLiteral("======== 全题型自动完成 ========"));
    if (!cryptoOk) {
        log(QStringLiteral("密钥初始化失败，请检查 config.json，已中止本次任务"));
        return;
    }
    if (!uid) {
        log(QStringLiteral("未能获取 userId（登录态可能无效），已中止本次任务"));
        return;
    }
    // Auth check comes BEFORE the pkg check: with an expired token the constructor's
    // package probe already failed, and aborting here would bypass the re-login retry.
    {
        QUrlQuery q;
        q.addQueryItem("imageUrl", "");
        q.addQueryItem("nickName", "");
        QJsonObject r = api.get(QStringLiteral("capp/businessserver/mp/user/signin/new/1"), q);
        if (FzwyApi::isAuthError(r)) {
            // Caller decides whether to re-login and retry (authFailedFlag)
            authFailed = true;
            log(QStringLiteral("登录态已失效（401）"));
            return;
        }
        QString uo = js(r["data"].toObject(), "uo");
        if (!uo.isEmpty()) {
            api.osValue = uo;
            st["os_value"] = uo;
            saveState(st);
        }
        step(QStringLiteral("signin"), r["success"].toBool());
    }
    {
        QJsonObject r = api.postV2(QStringLiteral("capp/businessserver/mp/user/signin/v2"), {});
        if (FzwyApi::isAuthError(r)) {
            authFailed = true;
            log(QStringLiteral("登录态已失效（401）"));
            return;
        }
        step(QStringLiteral("signin/v2"), r["success"].toBool());
    }
    if (pkg.isEmpty()) {
        log(QStringLiteral("无可用词包（packageUuid 为空），已中止本次任务"));
        return;
    }
    if (cancelled()) {
        log(QStringLiteral("已取消。"));
        return;
    }

    QJsonObject info = discover();
    QJsonArray teamIds = info["team_ids"].toArray();
    QString teamId = !teamIds.isEmpty() ? teamIds.first().toVariant().toString() : pkg;

    // aborted() = user cancel or token rejected mid-run (matches the official
    // client, which checks that desc pattern on every response)
    auto aborted = [this] { return cancelled() || api.authFailureSeen; };
    if (!aborted())
        doWords(words);
    if (!aborted())
        doSpellFill(teamId, spell);
    if (!aborted())
        doListen(pkg, listen);
    if (!aborted())
        doReading(pkg);
    if (!aborted())
        doExams();
    if (!aborted())
        doFinish();
    if (api.authFailureSeen) {
        authFailed = true;
        log(QStringLiteral("登录态在任务中途失效（401）"));
        return;
    }
    if (cancelled()) {
        log(QStringLiteral("已取消，任务提前结束。"));
        return;
    }

    QJsonObject tasks2;
    {
        QJsonObject r = api.postV2(QStringLiteral("capp/businessserver/mp/task/query/data/v2"),
                                   {});
        tasks2 = r["plain"].toObject();
        log(QStringLiteral("最终任务: %1").arg(preview(tasks2, 500)));
    }
    log(QStringLiteral("汇总 OK=%1 FAIL=%2").arg(okCount).arg(failCount));

    QJsonObject out;
    out["ok"] = okCount;
    out["fail"] = failCount;
    out["tasks"] = tasks2;
    out["discover"] = info;
    QFile f(QCoreApplication::applicationDirPath() + QStringLiteral("/all_types_log.json"));
    if (f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        f.write(QJsonDocument(out).toJson(QJsonDocument::Indented));
    } else {
        log(QStringLiteral("写入 all_types_log.json 失败: %1").arg(f.errorString()));
    }
}
