#pragma once

#include <QLabel>
#include <QMainWindow>
#include <QProgressBar>
#include <QPushButton>
#include <QTextEdit>
#include <QThread>

#include <atomic>
#include <functional>
#include <memory>

#include "Backend.h"

// Must be QThread, not std::thread: only QThreadPrivate::start sets up the event
// dispatcher; in a std::thread the first QAbstractSocket creation silently fails,
// so healthy channels get misdiagnosed as zombies and killed.
class JobThread : public QThread {
public:
    explicit JobThread(std::function<void()> fn) : m_fn(std::move(fn)) {}

protected:
    void run() override {
        if (m_fn)
            m_fn();
    }

private:
    std::function<void()> m_fn;
};

class MainWindow : public QMainWindow {
    Q_OBJECT
public:
    MainWindow();
    ~MainWindow() override;

protected:
    void closeEvent(QCloseEvent *e) override;
    void showEvent(QShowEvent *e) override;

private slots:
    void appendLog(const QString &msg);
    void jobDone();

private:
    struct StatusRow {
        QLabel *dot = nullptr;
        QLabel *value = nullptr;
        void set(const QString &text, const QString &color);
    };

    void buildUi();
    void applyStatus(const Backend::Status &st);
    void setBusy(bool busy, const QString &label = {});
    void startJob(int kind);  // 0=get token, 1=run tasks, 2=refresh status, 3=restart channel,
                              // 4=export wrongbook, 5=export study report
    void setLoginChip(const Backend::Status &st);
    void toggleSchedule();
    void refreshScheduleButton();

    Backend m_backend;
    bool m_busy = false;
    JobThread *m_worker = nullptr;
    // Lets worker-thread callbacks exit quietly when the window is destroyed
    // before the worker, avoiding dangling pointers
    std::shared_ptr<std::atomic_bool> m_alive = std::make_shared<std::atomic_bool>(true);

    QLabel *m_pill = nullptr;
    QLabel *m_stage = nullptr;
    StatusRow m_rowChannel;
    StatusRow m_rowPage;
    StatusRow m_rowLogin;
    QPushButton *m_btnRun = nullptr;
    QPushButton *m_btnToken = nullptr;
    QPushButton *m_btnRefresh = nullptr;
    QPushButton *m_btnRestart = nullptr;
    QPushButton *m_btnSchedule = nullptr;
    QPushButton *m_btnExportWrong = nullptr;
    QPushButton *m_btnExportReport = nullptr;
    QProgressBar *m_prog = nullptr;
    QTextEdit *m_log = nullptr;
};
