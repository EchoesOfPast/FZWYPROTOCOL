#pragma once

#include <QJsonArray>
#include <QJsonObject>
#include <QStringList>
#include <functional>

#include "FzwyApi.h"

class Tasks {
public:
    explicit Tasks(std::function<void(const QString &)> log);

    void runAll(int words = 10, int spell = 5, int fill = 5, int listen = 5);

private:
    void step(const QString &name, bool ok, const QString &extra = {});
    QJsonObject userRec(const QJsonValue &taskId, const QJsonValue &cardId,
                        const QString &cardType, qint64 learnTime = 3);

    QString fetchUserName();
    QString discoverPackageUuid();
    QStringList packageIdsFromGroup(const QString &groupId);
    bool verifyPackageCandidate(const QString &cand);
    QJsonObject discover();

    void doWords(int n);
    void doSpellFill(const QString &teamId, int n);
    void doListen(const QString &packageId, int n);
    void doReading(const QString &packageId);
    void doExams();
    void doFinish();

    std::function<void(const QString &)> log;
    FzwyApi api;
    QJsonObject st;
    qlonglong uid = 0;
    QString pkg;
    QString userName;
    bool cryptoOk = false;
    int okCount = 0;
    int failCount = 0;
};
