#include "MainWindow.h"

#include <QApplication>
#include <QClipboard>
#include <QCloseEvent>
#include <QDateTime>
#include <QFrame>
#include <QHBoxLayout>
#include <QIcon>
#include <QMessageBox>
#include <QShowEvent>
#include <QVBoxLayout>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

#include "StateStore.h"

namespace {

const char *kOk = "#15803d";
const char *kWarn = "#b45309";
const char *kErr = "#DC2626";
const char *kAccent = "#0D9488";
const char *kFaint = "#64748b";
const char *kInactive = "#94A3B8";

enum class Level { Info, Ok, Warn, Err, Head };

bool hasAny(const QString &s, const char *const *keys, int n) {
    for (int i = 0; i < n; ++i)
        if (s.contains(QLatin1String(keys[i])))
            return true;
    return false;
}

Level levelOf(const QString &msg) {
    static const char *errKeys[] = {"失败", "错误", "未取到", "未就绪", "缺失", "未通过",
                                    "未完成", "超时", "拒绝", "中断", "无法", "不可用"};
    static const char *okKeys[] = {"成功", "已获取", "验证通过", "已就绪", "已接入",
                                   "完成", "OK", "已刷新"};
    static const char *warnKeys[] = {"重启", "跳过", "僵尸", "注意", "等待", "提示", "⚠"};
    static const char *headKeys[] = {"=="};

    const QString t = msg.trimmed();
    if (hasAny(t, headKeys, 1))
        return Level::Head;
    if (hasAny(t, errKeys, 12))
        return Level::Err;
    if (t.contains(QLatin1String("fail")) || t.contains(QLatin1String("401")) ||
        t.contains(QLatin1String("error"), Qt::CaseInsensitive) ||
        t.contains(QLatin1String("timeout"), Qt::CaseInsensitive))
        return Level::Err;
    if (hasAny(t, okKeys, 8))
        return Level::Ok;
    if (t.contains(QLatin1String("ok")))
        return Level::Ok;
    if (hasAny(t, warnKeys, 7))
        return Level::Warn;
    return Level::Info;
}

QString colorOf(Level l) {
    switch (l) {
        case Level::Ok: return QLatin1String(kOk);
        case Level::Warn: return QLatin1String(kWarn);
        case Level::Err: return QLatin1String(kErr);
        case Level::Head: return QLatin1String(kAccent);
        default: return QLatin1String("#134E4A");
    }
}

// This Qt installation lacks translation files; standard buttons show English, so we build custom ones
bool confirmDialog(QWidget *parent, const QString &title, const QString &text) {
    QMessageBox box(parent);
    box.setWindowTitle(title);
    box.setText(text);
    box.setIcon(QMessageBox::Question);
    box.addButton(QStringLiteral("确定"), QMessageBox::AcceptRole);
    box.addButton(QStringLiteral("取消"), QMessageBox::RejectRole);
    box.exec();
    return box.clickedButton() &&
           box.buttonRole(box.clickedButton()) == QMessageBox::AcceptRole;
}

void applyLightTitleBar(QWidget *w) {
#ifdef _WIN32
    using Pfn = HRESULT(WINAPI *)(HWND, DWORD, LPCVOID, DWORD);
    HMODULE dwm = LoadLibraryW(L"dwmapi.dll");
    if (!dwm)
        return;
    auto fn = reinterpret_cast<Pfn>(reinterpret_cast<void *>(
        GetProcAddress(dwm, "DwmSetWindowAttribute")));
    if (fn) {
        HWND hwnd = reinterpret_cast<HWND>(w->winId());
        BOOL dark = FALSE;
        if (FAILED(fn(hwnd, 20, &dark, sizeof(dark))))  // DWMWA_USE_IMMERSIVE_DARK_MODE
            fn(hwnd, 19, &dark, sizeof(dark));          // legacy attribute number
    }
    FreeLibrary(dwm);
#else
    Q_UNUSED(w);
#endif
}

const char *kStyleTemplate = R"CSS(
QWidget { color: @text@; font-family: "Inter", "Segoe UI", "Microsoft YaHei", sans-serif; font-size: 13px; }
QWidget#root { background: @bg@; }

QLabel#title { font-size: 19px; font-weight: 600; color: @text@; }
QLabel#subtitle { font-size: 12px; color: @muted@; }
QLabel#cardTitle { font-size: 11px; font-weight: 600; color: @muted@; }
QLabel#rowName { font-size: 13px; color: @muted@; }
QLabel#rowValue { font-size: 13px; font-weight: 600; color: @text@; }
QLabel#stage { font-size: 12px; color: @muted@; }
QLabel#footer { font-size: 11px; color: @faint@; }

QLabel#pill {
    background: @surface@; border: 1px solid @border@; border-radius: 11px;
    padding: 3px 12px; font-size: 12px; font-weight: 600;
}

QFrame#card { background: @surface@; border: 1px solid @border@; border-radius: 10px; }

QPushButton { border-radius: 8px; font-size: 13px; font-weight: 600; padding: 8px 14px; }
QPushButton#primary {
    color: #FFFFFF; background: @cta@; border: 1px solid @ctaEdge@;
}
QPushButton#primary:hover { background: @ctaH@; border: 1px solid @ctaP@; }
QPushButton#primary:pressed { background: @ctaP@; }
QPushButton#primary:focus { border: 2px solid @accentDeep@; }
QPushButton#primary:disabled {
    background: @surface2@; color: @faint@; border: 1px solid @border@;
}

QPushButton#secondary { color: @accentDeep@; background: @surface@; border: 1px solid @accent@; }
QPushButton#secondary:hover { background: @bg@; border: 1px solid @accentDeep@; }
QPushButton#secondary:pressed { background: @surface2@; }
QPushButton#secondary:focus { border: 2px solid @accentDeep@; }
QPushButton#secondary:disabled { color: @faint@; background: @surface@; border: 1px solid @border@; }

QPushButton#ghost { color: @muted@; background: @surface@; border: 1px solid @grayEdge@; font-weight: 500; }
QPushButton#ghost:hover { color: @text@; background: @bg@; border: 1px solid @accent@; }
QPushButton#ghost:pressed { background: @surface2@; }
QPushButton#ghost:focus { border: 2px solid @accentDeep@; }
QPushButton#ghost:disabled { color: @faint@; border: 1px solid @border@; }

/* Log card top-right "copy/clear" buttons: need narrower padding to avoid text clipping */
QPushButton#mini {
    color: @muted@; background: @surface@; border: 1px solid @grayEdge@;
    border-radius: 6px; font-size: 12px; font-weight: 500; padding: 0px 12px;
}
QPushButton#mini:hover { color: @text@; background: @bg@; border: 1px solid @accent@; }
QPushButton#mini:pressed { background: @surface2@; }
QPushButton#mini:focus { border: 2px solid @accentDeep@; }

QTextEdit#log {
    background: @logBg@; color: @logText@; border: 1px solid @border@; border-radius: 8px;
    font-family: "JetBrains Mono", Consolas, "Courier New", "Microsoft YaHei", monospace;
    font-size: 12px; padding: 6px 8px;
    selection-background-color: @accent@; selection-color: #FFFFFF;
}
QTextEdit#log:focus { border: 1px solid @accent@; }

QProgressBar { background: @surface2@; border: none; border-radius: 3px; }
QProgressBar::chunk { border-radius: 3px; background: @accent@; }

QScrollBar:vertical { background: transparent; width: 10px; margin: 2px; }
QScrollBar::handle:vertical { background: @border@; border-radius: 5px; min-height: 28px; }
QScrollBar::handle:vertical:hover { background: @accent2@; }
QScrollBar::add-line:vertical, QScrollBar::sub-line:vertical { height: 0px; }
QScrollBar::add-page:vertical, QScrollBar::sub-page:vertical { background: transparent; }
QScrollBar:horizontal { background: transparent; height: 10px; margin: 2px; }
QScrollBar::handle:horizontal { background: @border@; border-radius: 5px; min-width: 28px; }
QScrollBar::handle:horizontal:hover { background: @accent2@; }
QScrollBar::add-line:horizontal, QScrollBar::sub-line:horizontal { width: 0px; }

QMessageBox { background: @surface@; }
QMessageBox QLabel { color: @text@; font-size: 13px; }
QMessageBox QPushButton { color: @accentDeep@; background: @surface@; border: 1px solid @accent@;
    border-radius: 6px; padding: 8px 20px; font-size: 13px; min-width: 64px; }
QMessageBox QPushButton:hover { background: @bg@; border: 1px solid @accentDeep@; }
QMessageBox QPushButton:focus { border: 2px solid @accentDeep@; }
)CSS";

QString buildStyleSheet() {
    QString s = QString::fromLatin1(kStyleTemplate);
    const std::pair<const char *, const char *> vars[] = {
        {"@bg@", "#F0FDFA"},
        {"@surface@", "#FFFFFF"},
        {"@surface2@", "#E8F1F4"},
        {"@border@", "#99F6E4"},
        {"@grayEdge@", "#CBD5E1"},
        {"@text@", "#134E4A"},
        {"@muted@", "#475569"},
        {"@faint@", "#64748b"},
        {"@accent@", "#0D9488"},
        {"@accentDeep@", "#0F766E"},
        {"@accent2@", "#14B8A6"},
        {"@cta@", "#EA580C"},
        {"@ctaEdge@", "#C2410C"},
        {"@ctaH@", "#C2410C"},
        {"@ctaP@", "#9A3412"},
        {"@logBg@", "#FFFFFF"},
        {"@logText@", "#134E4A"},
    };
    for (const auto &v : vars)
        s.replace(QLatin1String(v.first), QLatin1String(v.second));
    return s;
}

}  // namespace

void MainWindow::StatusRow::set(const QString &text, const QString &color) {
    if (value) {
        value->setText(text);
        // The text label carries the status; color is only a reinforcement, never the sole indicator
        value->setStyleSheet(QStringLiteral("color:%1;").arg(color));
    }
    if (dot)
        dot->setStyleSheet(QStringLiteral("background:%1; border-radius:4px;").arg(color));
}

MainWindow::MainWindow() {
    buildUi();
    startJob(2);  // Silent status refresh on startup
}

void MainWindow::buildUi() {
    setWindowTitle(QStringLiteral("学习任务助手"));
    setWindowIcon(QIcon(QStringLiteral(":/app.ico")));
    resize(520, 600);
    setMinimumSize(460, 520);

    qApp->setStyleSheet(buildStyleSheet());

    auto *root = new QWidget(this);
    root->setObjectName(QStringLiteral("root"));
    auto *lay = new QVBoxLayout(root);
    lay->setContentsMargins(14, 12, 14, 10);
    lay->setSpacing(10);

    auto *head = new QHBoxLayout;
    head->setSpacing(12);
    auto *logo = new QLabel;
    logo->setFixedSize(30, 30);
    logo->setPixmap(QIcon(QStringLiteral(":/app.ico"))
                        .pixmap(QSize(30, 30), devicePixelRatioF()));
    logo->setScaledContents(true);
    head->addWidget(logo);

    auto *tbox = new QVBoxLayout;
    tbox->setSpacing(1);
    auto *title = new QLabel(QStringLiteral("学习任务助手"));
    title->setObjectName(QStringLiteral("title"));
    tbox->addWidget(title);
    auto *subtitle = new QLabel(QStringLiteral("无限挑战 · 一键完成"));
    subtitle->setObjectName(QStringLiteral("subtitle"));
    tbox->addWidget(subtitle);
    head->addLayout(tbox);
    head->addStretch();

    m_pill = new QLabel(QStringLiteral("● 未登录"));
    m_pill->setObjectName(QStringLiteral("pill"));
    m_pill->setStyleSheet(QStringLiteral("color:%1;").arg(QLatin1String(kInactive)));
    head->addWidget(m_pill, 0, Qt::AlignTop);
    lay->addLayout(head);

    auto *card = new QFrame;
    card->setObjectName(QStringLiteral("card"));
    auto *cl = new QVBoxLayout(card);
    cl->setContentsMargins(14, 10, 14, 10);
    cl->setSpacing(7);
    auto *ct = new QLabel(QStringLiteral("运行状态"));
    ct->setObjectName(QStringLiteral("cardTitle"));
    cl->addWidget(ct);

    auto makeRow = [&](const QString &name, StatusRow *out) {
        auto *w = new QWidget;
        auto *h = new QHBoxLayout(w);
        h->setContentsMargins(0, 0, 0, 0);
        h->setSpacing(10);
        auto *dot = new QLabel;
        dot->setFixedSize(9, 9);
        dot->setStyleSheet(QStringLiteral("background:%1; border-radius:4px;").arg(QLatin1String(kInactive)));
        h->addWidget(dot);
        auto *n = new QLabel(name);
        n->setObjectName(QStringLiteral("rowName"));
        n->setFixedWidth(74);
        h->addWidget(n);
        auto *v = new QLabel(QStringLiteral("—"));
        v->setObjectName(QStringLiteral("rowValue"));
        h->addWidget(v);
        h->addStretch();
        out->dot = dot;
        out->value = v;
        cl->addWidget(w);
    };
    makeRow(QStringLiteral("调试通道"), &m_rowChannel);
    makeRow(QStringLiteral("小程序"), &m_rowPage);
    makeRow(QStringLiteral("登录态"), &m_rowLogin);
    lay->addWidget(card);

    m_btnRun = new QPushButton(QStringLiteral("一键完成任务"));
    m_btnRun->setObjectName(QStringLiteral("primary"));
    m_btnRun->setCursor(Qt::PointingHandCursor);
    m_btnRun->setMinimumHeight(40);
    connect(m_btnRun, &QPushButton::clicked, this, [this] { startJob(1); });
    lay->addWidget(m_btnRun);

    auto *row = new QHBoxLayout;
    row->setSpacing(10);
    m_btnToken = new QPushButton(QStringLiteral("仅获取 Token"));
    m_btnToken->setObjectName(QStringLiteral("secondary"));
    m_btnToken->setCursor(Qt::PointingHandCursor);
    connect(m_btnToken, &QPushButton::clicked, this, [this] { startJob(0); });
    row->addWidget(m_btnToken, 1);

    m_btnRefresh = new QPushButton(QStringLiteral("刷新状态"));
    m_btnRefresh->setObjectName(QStringLiteral("ghost"));
    m_btnRefresh->setCursor(Qt::PointingHandCursor);
    connect(m_btnRefresh, &QPushButton::clicked, this, [this] { startJob(2); });
    row->addWidget(m_btnRefresh);

    m_btnRestart = new QPushButton(QStringLiteral("重启通道"));
    m_btnRestart->setObjectName(QStringLiteral("ghost"));
    m_btnRestart->setCursor(Qt::PointingHandCursor);
    m_btnRestart->setToolTip(QStringLiteral(
        "只在通道真的卡死时才需要点。正常取 Token 不会重启通道，也不会影响已打开的小程序。"));
    connect(m_btnRestart, &QPushButton::clicked, this, [this] {
        if (confirmDialog(this, QStringLiteral("重启调试通道"),
                          QStringLiteral("重启会结束当前通道进程，已打开的小程序需要重新打开一次。\n\n确定继续？")))
            startJob(3);
    });
    row->addWidget(m_btnRestart);
    lay->addLayout(row);

    m_prog = new QProgressBar;
    m_prog->setRange(0, 0);
    m_prog->setTextVisible(false);
    m_prog->setFixedHeight(5);
    m_prog->setVisible(false);
    lay->addWidget(m_prog);

    m_stage = new QLabel(QStringLiteral("就绪"));
    m_stage->setObjectName(QStringLiteral("stage"));
    m_stage->setWordWrap(true);
    lay->addWidget(m_stage);

    auto *logCard = new QFrame;
    logCard->setObjectName(QStringLiteral("card"));
    auto *ll = new QVBoxLayout(logCard);
    ll->setContentsMargins(12, 9, 12, 10);
    ll->setSpacing(6);
    auto *lh = new QHBoxLayout;
    lh->setSpacing(10);
    auto *lt = new QLabel(QStringLiteral("运行日志"));
    lt->setObjectName(QStringLiteral("cardTitle"));
    lh->addWidget(lt);
    lh->addStretch();
    auto *btnCopy = new QPushButton(QStringLiteral("复制"));
    btnCopy->setObjectName(QStringLiteral("mini"));
    btnCopy->setCursor(Qt::PointingHandCursor);
    btnCopy->setFixedSize(58, 24);
    connect(btnCopy, &QPushButton::clicked, this, [this] {
        QApplication::clipboard()->setText(m_log->toPlainText());
    });
    lh->addWidget(btnCopy);
    auto *btnClear = new QPushButton(QStringLiteral("清空"));
    btnClear->setObjectName(QStringLiteral("mini"));
    btnClear->setCursor(Qt::PointingHandCursor);
    btnClear->setFixedSize(58, 24);
    connect(btnClear, &QPushButton::clicked, this, [this] { m_log->clear(); });
    lh->addWidget(btnClear);
    ll->addLayout(lh);

    m_log = new QTextEdit;
    m_log->setObjectName(QStringLiteral("log"));
    m_log->setReadOnly(true);
    m_log->setLineWrapMode(QTextEdit::WidgetWidth);
    // Cap the log line count so long runs cannot grow memory without bound
    m_log->document()->setMaximumBlockCount(20000);
    m_log->document()->setDocumentMargin(6);
    m_log->document()->setDefaultStyleSheet(
        QStringLiteral("p { margin: 0px; padding: 0px; -qt-block-indent: 0; }"));
    ll->addWidget(m_log, 1);
    lay->addWidget(logCard, 1);

    auto *hint = new QLabel(QStringLiteral(
        "使用顺序：先启动本工具 → 再在微信里打开小程序（“仅支持手机端”弹窗属正常）→ 再点按钮。"
        "通道健康时不会被重启。"));
    hint->setObjectName(QStringLiteral("footer"));
    hint->setWordWrap(true);
    lay->addWidget(hint);

    setCentralWidget(root);
}

void MainWindow::appendLog(const QString &msg) {
    const Level lv = levelOf(msg);
    const QString ts = QDateTime::currentDateTime().toString(QStringLiteral("HH:mm:ss"));
    QString body = msg.toHtmlEscaped();
    body.replace(QLatin1Char('\n'), QLatin1String("<br>"));

    QString html;
    if (lv == Level::Head) {
        html = QStringLiteral(
                   "<span style='color:%1;'>%2</span> "
                   "<span style='color:%3; font-weight:bold;'>%4</span>")
                   .arg(QLatin1String(kFaint), ts, colorOf(lv), body);
    } else {
        html = QStringLiteral(
                   "<span style='color:%1;'>%2</span> "
                   "<span style='color:%3;'>%4</span>")
                   .arg(QLatin1String(kFaint), ts, colorOf(lv), body);
    }
    m_log->append(html);
    if (m_busy && !msg.trimmed().isEmpty()) {
        QString first = msg.trimmed().section(QLatin1Char('\n'), 0, 0);
        if (first.size() > 68)
            first = first.left(67) + QStringLiteral("…");
        m_stage->setText(first);
    }
}

void MainWindow::setBusy(bool busy, const QString &label) {
    m_busy = busy;
    m_btnRun->setEnabled(!busy);
    m_btnToken->setEnabled(!busy);
    m_btnRefresh->setEnabled(!busy);
    m_btnRestart->setEnabled(!busy);
    m_prog->setVisible(busy);
    if (busy) {
        m_stage->setText(label.isEmpty() ? QStringLiteral("执行中…") : label);
        m_stage->setStyleSheet(QStringLiteral("color:%1;").arg(QLatin1String(kAccent)));
    } else {
        m_stage->setText(QStringLiteral("就绪"));
        m_stage->setStyleSheet(QStringLiteral("color:%1;").arg(QLatin1String(kFaint)));
    }
}

void MainWindow::setLoginChip(const Backend::Status &st) {
    if (st.loggedIn) {
        m_pill->setText(QStringLiteral("● 已登录 uid=%1").arg(st.uid));
        m_pill->setStyleSheet(QStringLiteral("color:%1;").arg(QLatin1String(kOk)));
    } else {
        m_pill->setText(QStringLiteral("● 未登录"));
        m_pill->setStyleSheet(QStringLiteral("color:%1;").arg(QLatin1String(kInactive)));
    }
}

void MainWindow::applyStatus(const Backend::Status &st) {
    if (st.channelZombie)
        m_rowChannel.set(QStringLiteral("端口被占用，握手无响应"), QLatin1String(kErr));
    else if (st.channelUp)
        m_rowChannel.set(QStringLiteral("运行中"), QLatin1String(kOk));
    else
        m_rowChannel.set(QStringLiteral("未运行"), QLatin1String(kInactive));

    if (!st.channelUp)
        m_rowPage.set(QStringLiteral("通道未运行"), QLatin1String(kInactive));
    else if (st.pageReady)
        m_rowPage.set(QStringLiteral("已接入调试通道"), QLatin1String(kOk));
    else
        m_rowPage.set(QStringLiteral("未接入（请在微信里打开小程序）"), QLatin1String(kWarn));

    if (st.loggedIn) {
        QString v = QStringLiteral("已登录 uid=%1").arg(st.uid);
        if (!st.userName.isEmpty())
            v += QStringLiteral("（%1）").arg(st.userName);
        m_rowLogin.set(v, QLatin1String(kOk));
    } else {
        m_rowLogin.set(QStringLiteral("未登录"), QLatin1String(kInactive));
    }
    setLoginChip(st);
}

void MainWindow::startJob(int kind) {
    if (m_busy)
        return;
    // A previous job may have been cancel-requested by a close attempt; reset the
    // flag before starting a new one. Safe: no worker is running at this point.
    m_backend.resetCancel();
    const QString label = kind == 1 ? QStringLiteral("正在执行任务…")
                                    : (kind == 0 ? QStringLiteral("正在获取 Token…")
                                                 : (kind == 3 ? QStringLiteral("正在重启调试通道…")
                                                              : QStringLiteral("正在检查状态…")));
    setBusy(true, label);

    auto alive = m_alive;
    auto *job = new JobThread([this, kind, alive] {
        if (kind != 2) {
            m_backend.log = [this, alive](const QString &m) {
                if (!alive->load())
                    return;
                QMetaObject::invokeMethod(this, "appendLog", Qt::QueuedConnection,
                                          Q_ARG(QString, m));
            };
        } else {
            m_backend.log = nullptr;
        }
        switch (kind) {
            case 1: m_backend.runTasks(); break;
            case 3: m_backend.restartChannel(); break;
            case 2: break;
            default: m_backend.getToken(); break;
        }
        const Backend::Status st = m_backend.probeStatus();
        if (!alive->load())
            return;
        QMetaObject::invokeMethod(
            this, [this, st] { applyStatus(st); }, Qt::QueuedConnection);
        QMetaObject::invokeMethod(this, "jobDone", Qt::QueuedConnection);
    });
    m_worker = job;
    connect(job, &QThread::finished, job, &QObject::deleteLater);
    job->start();
}

void MainWindow::jobDone() {
    m_worker = nullptr;
    setBusy(false);
    m_backend.log = nullptr;
}

MainWindow::~MainWindow() {
    // Let the still-running task thread exit quietly, avoiding callbacks into a destroyed window
    m_alive->store(false);
    // Fallback: normally closeEvent has already waited for the worker. If the window
    // is destroyed with the worker still running, cancel and wait briefly to avoid
    // dangling access to m_backend.
    if (m_worker && m_worker->isRunning()) {
        m_backend.cancel();
        m_worker->wait(3000);
    }
    // Stop the debug channel while QApplication is still alive, so the
    // BlockingQueuedConnection in Session::stop() can reach the channel thread's event loop.
    m_backend.stopChannel();
}

void MainWindow::showEvent(QShowEvent *e) {
    QMainWindow::showEvent(e);
    applyLightTitleBar(this);
}

void MainWindow::closeEvent(QCloseEvent *e) {
    if (m_busy && m_worker) {
        if (!confirmDialog(this, QStringLiteral("任务进行中"),
                           QStringLiteral("当前任务还没结束，关闭时会先请求中断任务并等待其退出。\n\n确定关闭吗？"))) {
            e->ignore();
            return;
        }
        // confirmDialog's exec() is a nested event loop: the worker may have finished
        // and the queued jobDone() may have nulled m_worker meanwhile. Re-check, or
        // the wait() below dereferences a null pointer.
        if (m_worker) {
            m_backend.cancel();
            // The worker touches m_backend directly, so it must finish before the window
            // is destroyed (use-after-free otherwise). Refusing to close beats crashing.
            if (!m_worker->wait(10000)) {
                // Refusing to close means the task keeps running: undo the cancel so it
                // completes. Do NOT touch m_alive here - once false, the worker's
                // if(!alive) return skips jobDone forever (buttons stay disabled) and
                // m_worker dangles after deleteLater (UAF on the next close).
                m_backend.resetCancel();
                QMessageBox box(this);
                box.setWindowTitle(QStringLiteral("请稍候"));
                box.setText(QStringLiteral("任务未能及时停止（可能正在等待网络响应）。\n请稍候几秒再关闭。"));
                box.setIcon(QMessageBox::Information);
                box.addButton(QStringLiteral("确定"), QMessageBox::AcceptRole);
                box.exec();
                e->ignore();
                return;
            }
            m_worker = nullptr;
        }
        // Only silence callbacks once closing is certain (worker finished, or was
        // already done before the confirm), so leftover worker callbacks exit quietly
        m_alive->store(false);
    }
    // Stop the debug channel on exit (releases ports 9421/62000, joins the watchdog
    // thread). Must run after the worker has finished to avoid concurrent Session access.
    m_backend.stopChannel();
    e->accept();
}
