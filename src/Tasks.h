#pragma once

#include <QJsonArray>
#include <QJsonObject>
#include <QStringList>
#include <functional>

#include "FzwyApi.h"

class Tasks {
public:
    explicit Tasks(std::function<void(const QString &)> log);

    void runAll(int words = 10, int spell = 5, int fill = 5, int listen = 5,
                const std::function<bool()> &cancelRequested = {});
    // True when a step was aborted by a 401: Backend may re-login and retry.
    bool authFailedFlag() const { return authFailed; }

private:
    void step(const QString &name, bool ok, const QString &extra = {});
    QJsonObject userRec(const QJsonValue &taskId, const QJsonValue &cardId,
                        const QString &cardType, qint64 learnTime = 0);  // <=0: random 2-6s
    bool cancelled() const { return cancelRequested && cancelRequested(); }

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
    std::function<bool()> cancelRequested;
    FzwyApi api;
    QJsonObject st;
    qlonglong uid = 0;
    QString pkg;
    QString userName;
    bool cryptoOk = false;
    bool authFailed = false;
    int okCount = 0;
    int failCount = 0;
};
