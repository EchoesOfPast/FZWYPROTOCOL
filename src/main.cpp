#include <QApplication>
#include <QFont>
#include <QGuiApplication>
#include <QIcon>
#include <QJsonDocument>
#include <QThread>
#include <QTimer>

#include <cstdio>
#include <cstring>
#include <functional>

#include "Backend.h"
#include "CdpClient.h"
#include "Config.h"
#include "FzwyApi.h"
#include "MainWindow.h"
#include "StateStore.h"
#include "wmpf/WmpfChannel.h"
#include "wmpf/WmpfInject.h"
#include "wmpf/WmpfProbe.h"
#include "wmpf/WmpfSession.h"

#ifdef _WIN32
#include <windows.h>

static void cliPrint(const QString &m) {
    QByteArray b = m.toUtf8() + "\n";
    HANDLE h = GetStdHandle(STD_OUTPUT_HANDLE);
    if (!h || h == INVALID_HANDLE_VALUE) {
        if (AttachConsole(ATTACH_PARENT_PROCESS))
            h = GetStdHandle(STD_OUTPUT_HANDLE);
    }
    if (h && h != INVALID_HANDLE_VALUE) {
        DWORD written = 0;
        WriteFile(h, b.constData(), static_cast<DWORD>(b.size()), &written, nullptr);
    }
}
#else
static void cliPrint(const QString &m) {
    std::printf("%s\n", m.toUtf8().constData());
    std::fflush(stdout);
}
#endif

int main(int argc, char *argv[]) {
    // CLI debug modes: --token (login only), --run (login + tasks),
    // --check (crypto self-test), --screenshot <path> (offscreen render)
    bool cliToken = false, cliRun = false, cliCheck = false, cliWmpfProbe = false,
         cliWmpfHook = false;
    int cliServe = 0;
    QString cliShot;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--token") == 0)
            cliToken = true;
        if (std::strcmp(argv[i], "--run") == 0)
            cliRun = true;
        if (std::strcmp(argv[i], "--check") == 0)
            cliCheck = true;
        if (std::strcmp(argv[i], "--wmpf-probe") == 0)
            cliWmpfProbe = true;
        if (std::strcmp(argv[i], "--wmpf-hook") == 0)
            cliWmpfHook = true;
        if (std::strcmp(argv[i], "--wmpf-serve") == 0) {
            if (i + 1 < argc && argv[i + 1][0] != '-') {
                // Invalid seconds must error out, never silently fall into GUI mode
                bool okNum = false;
                cliServe = QString::fromLocal8Bit(argv[++i]).toInt(&okNum);
                if (!okNum || cliServe <= 0) {
                    cliPrint(QStringLiteral("错误：--wmpf-serve 需要正整数秒数，收到 \"%1\"")
                                 .arg(QString::fromLocal8Bit(argv[i])));
                    return 2;
                }
            } else {
                cliServe = 60;
            }
        }
        if (std::strcmp(argv[i], "--screenshot") == 0 && i + 1 < argc)
            cliShot = QString::fromLocal8Bit(argv[++i]);
    }

    // High-DPI: preserve non-integer scaling to avoid blurry fonts at 125%/150%
    QGuiApplication::setHighDpiScaleFactorRoundingPolicy(
        Qt::HighDpiScaleFactorRoundingPolicy::PassThrough);

    QApplication app(argc, argv);
    QApplication::setApplicationName(QStringLiteral("Study Task Assistant"));
    QApplication::setApplicationVersion(QStringLiteral("1.2.0"));
    QApplication::setOrganizationName(QStringLiteral("fzwy"));
    QApplication::setWindowIcon(QIcon(QStringLiteral(":/app.ico")));
    QFont appFont;
    appFont.setFamilies({QStringLiteral("Inter"), QStringLiteral("Segoe UI"),
                         QStringLiteral("Microsoft YaHei")});
    appFont.setPointSize(9);
    QApplication::setFont(appFont);

    // Load configuration before anything uses protocol constants
    g_config = AppConfig::load();

    if (!cliShot.isEmpty()) {
        MainWindow w;
        w.ensurePolished();
        w.grab().save(cliShot);
        return 0;
    }

    if (cliServe > 0) {
        wmpf::Session sess;
        sess.setLog([](const QString &m) { cliPrint(m); });
        if (!sess.ensure()) {
            cliPrint(QStringLiteral("通道启动失败"));
            return 1;
        }
        cliPrint(QStringLiteral("运行 %1 秒。现在请在微信里打开（或重新打开）该小程序。").arg(cliServe));
        QTimer tick;
        QObject::connect(&tick, &QTimer::timeout, [&sess] { cliPrint(sess.statsText()); });
        tick.start(5000);
        QTimer::singleShot(cliServe * 1000, &app, &QCoreApplication::quit);
        app.exec();
        cliPrint(QStringLiteral("结束。最终统计：%1").arg(sess.statsText()));
        return 0;
    }

    if (cliWmpfHook) {
        // Gate on a read-only probe first: if the prologue can't be relocated, never write memory
        cliPrint(QStringLiteral("== Step 1: Read-only probe (no memory writes) =="));
        const wmpf::ProbeReport pr =
            wmpf::runProbe([](const QString &m) { cliPrint(QStringLiteral("  ") + m); });
        if (!pr.overallOk) {
            cliPrint(QStringLiteral("Probe failed, aborted. No memory was written."));
            cliPrint(wmpf::formatProbeReport(pr));
            return 2;
        }
        cliPrint(QStringLiteral("Probe passed."));
        cliPrint(QString());
        cliPrint(QStringLiteral("== Step 2: Inject hook (will write to target process memory) =="));
        const wmpf::HookInstallResult hr =
            wmpf::installHook([](const QString &m) { cliPrint(QStringLiteral("  ") + m); });
        cliPrint(QStringLiteral("结果：%1").arg(hr.message));
        if (!hr.hookLogTail.isEmpty()) {
            cliPrint(QStringLiteral("---- FzwyHook.log tail ----"));
            cliPrint(hr.hookLogTail);
        }
        return hr.ok ? 0 : 3;
    }

    if (cliWmpfProbe) {
        const wmpf::ProbeReport r = wmpf::runProbe(
            [](const QString &m) { cliPrint(QStringLiteral("  ") + m); });
        cliPrint(wmpf::formatProbeReport(r));
        return r.overallOk ? 0 : 2;
    }

    if (cliCheck) {
        {
            QByteArray key = crypto::rand16(), iv = crypto::rand16();
            QByteArray src = "hello fzwy 123 crypto test";
            QByteArray dec = crypto::aesCbcDecrypt(key, iv, crypto::aesCbcEncrypt(key, iv, src));
            cliPrint(QStringLiteral("AES self-test: %1").arg(dec == src ? "OK" : "FAIL"));
            auto kp = crypto::rsaGenerate1024();
            cliPrint(QStringLiteral("  keygen: ok=%1 spkiDer=%2 bytes")
                         .arg(kp.ok())
                         .arg(kp.publicKeySpkiDer().size()));
            QByteArray ct = crypto::rsaEncryptPkcs1WithSpki(kp.publicKeySpkiDer(), "rsa-test");
            QByteArray pt = kp.decryptPkcs1(ct);
            cliPrint(QStringLiteral("RSA self-test: %1 (ct=%2 bytes)")
                         .arg(pt == "rsa-test" ? "OK" : "FAIL")
                         .arg(ct.size()));
        }
        QJsonObject st = loadState();
        FzwyApi api;
        api.token = st["token"].toString();
        api.osValue = st["os_value"].toString();
        api.userId = st["user_id"].toVariant().toLongLong();
        if (!api.initCrypto()) {
            cliPrint(QStringLiteral("Crypto init failed"));
            return 1;
        }
        QString uo, err;
        bool ok = api.validate(&uo, &err);
        cliPrint(QStringLiteral("validate(signin): %1 %2").arg(ok ? "OK" : "FAIL", ok ? uo.left(12) : err));
        QJsonObject r = api.postV2(QStringLiteral("capp/businessserver/mp/task/query/data/v2"), {});
        cliPrint(QStringLiteral("task/query/data/v2: success=%1 plain=%2")
                     .arg(r["success"].toBool())
                     .arg(QString::fromUtf8(QJsonDocument(r["plain"].toObject())
                                                .toJson(QJsonDocument::Compact))
                              .left(200)));
        // Invalid code on purpose: if the server can decrypt the body and returns
        // a business error, the encryption protocol is correct
        FzwyApi api2;
        api2.initCrypto();
        QJsonObject lp;
        lp["code"] = QStringLiteral("000000invalidcode");
        lp["iv"] = "";
        lp["encryptedData"] = "";
        lp["appId"] = g_config.appId;
        lp["mpCode"] = g_config.mpCode;
        lp["avatarUrl"] = "";
        lp["nickName"] = "";
        QJsonObject lj = api2.postV2(QStringLiteral("capp/crbac/mp/login/v2"), lp);
        cliPrint(QStringLiteral("login/v2(bad code) raw: success=%1 desc=%2")
                     .arg(lj["success"].toBool())
                     .arg(lj["desc"].toString()));
        cliPrint(QStringLiteral("login/v2(bad code) decrypted: %1")
                     .arg(QString::fromUtf8(QJsonDocument(lj["plain"].toObject())
                                                .toJson(QJsonDocument::Compact))
                              .left(200)));
        return ok && r["success"].toBool() ? 0 : 1;
    }

    if (cliToken || cliRun) {
        // Use the same QThread path as the GUI so the CLI reproduces GUI behavior;
        // a main-thread run can succeed while a worker-thread run fails (see the
        // JobThread comment in MainWindow.h)
        bool ok = false;
        JobThread job([&] {
            Backend b;
            b.log = [](const QString &m) { cliPrint(m); };
            if (cliRun) {
                // A failed run (cancelled / login failed) must propagate to the exit code
                ok = b.runTasks();
            } else {
                ok = b.getToken();
            }
        });
        job.start();
        job.wait();
        return ok ? 0 : 1;
    }

    MainWindow w;
    w.show();
    return QApplication::exec();
}
