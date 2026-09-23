#pragma once

#include <atomic>
#include <functional>

#include "wmpf/WmpfInject.h"
#include "wmpf/WmpfSession.h"

// Flow: channel -> wait for mini-program -> silent login (getUserInfo + wx.login
// + login/v2) -> validate -> run tasks.
class Backend {
public:
    struct Status {
        bool channelUp = false;      // Channel server listening + hook attached
        bool channelZombie = false;  // Server listening but hook not attached
        bool pageReady = false;      // Mini-program connected to debug channel
        bool loggedIn = false;
        qlonglong uid = 0;
        QString userName;
    };

    std::function<void(const QString &)> log;

    bool getToken();
    bool runTasks();  // false = cancelled or login failed
    Status probeStatus();

    bool restartChannel();

    void stopChannel() {
        m_chan.stop();
        wmpf::unhookAll(log);
    }

    // Cancel contract: the GUI thread sets this on window close; a running
    // getToken/runTasks polls it at checkpoints (page-wait loop, between steps)
    // so closeEvent can wait() for the worker and avoid dangling access.
    void cancel() { m_cancel.store(true); }
    void resetCancel() { m_cancel.store(false); }

private:
    // Login failure classification: only NoChannel means the channel itself is broken
    enum class LoginFail { None, NoChannel, NoPage, NoCode, Rejected, NoToken };

    bool ensureChannelAndPage();

    bool silentLoginOnce(LoginFail *fail);
    static const char *failText(LoginFail f);

    wmpf::Session m_chan;
    std::atomic_bool m_cancel{false};
};
