#include "Backend.h"

#include <QJsonDocument>
#include <QJsonObject>
#include <QThread>

#include "CdpClient.h"
#include "Config.h"
#include "FzwyApi.h"
#include "StateStore.h"
#include "Tasks.h"

namespace {

// Platform disguise patch: getSystemInfoSync/getDeviceInfo report android and
// "mobile only" restriction dialogs are auto-confirmed
const char* kPatchJs = R"JS(
(function(){
  try {
    if (typeof wx === 'undefined') return 'no-wx';
    try {
      const _g = wx.getSystemInfoSync.bind(wx);
      Object.defineProperty(wx, 'getSystemInfoSync', {
        configurable: true, writable: true,
        value: function(){
          const i = _g() || {};
          if (['windows','mac','ohos_pc'].includes(i.platform)) i.platform = 'android';
          return i;
        }
      });
    } catch(e) {}
    try {
      const _d = wx.getDeviceInfo.bind(wx);
      Object.defineProperty(wx, 'getDeviceInfo', {
        configurable: true, writable: true,
        value: function(){
          const i = _d() || {};
          if (['windows','mac','ohos_pc'].includes(i.platform)) i.platform = 'android';
          return i;
        }
      });
    } catch(e) {}
    try {
      const _m = wx.showModal.bind(wx);
      Object.defineProperty(wx, 'showModal', {
        configurable: true, writable: true,
        value: function(opts){
          opts = opts || {};
          const c = String(opts.content||'');
          if (c.indexOf('仅支持在手机端')>=0 || c.indexOf('手机端使用')>=0) {
            try { opts.success && opts.success({confirm:true}); } catch(e){}
            try { opts.complete && opts.complete({confirm:true}); } catch(e){}
            return;
          }
          return _m(opts);
        }
      });
    } catch(e) {}
    try {
      const util = require('75ABC157A49727BF13CDA95071075F06.js');
      if (util) util.checkPlatformAndRestrict = function(){ return true; };
    } catch(e) {}
    return 'patched';
  } catch(e) { return 'err:'+e.message; }
})()
)JS";

// Get login materials (getUserInfo, silent if already authorized, + wx.login
// code); the login request itself is login/v2 on the C++ side
const char* kLoginInfoJs = R"JS(
new Promise(function(resolve){
  var out = {iv:'', enc:'', nick:'', avatar:'', code:'', err:''};
  var finished = false;
  function finish(){ if (!finished){ finished = true; resolve(JSON.stringify(out)); } }
  function doLogin(){
    try {
      wx.login({
        success: function(le){ out.code = (le && le.code) || ''; finish(); },
        fail: function(e){ out.err = 'wxlogin:' + JSON.stringify(e); finish(); }
      });
    } catch(e) { out.err = 'throw:' + e.message; finish(); }
  }
  function fill(res){
    try {
      out.iv = res.iv || ''; out.enc = res.encryptedData || '';
      out.nick = (res.userInfo && res.userInfo.nickName) || '';
      out.avatar = (res.userInfo && res.userInfo.avatarUrl) || '';
    } catch(e) {}
  }
  try {
    wx.getUserInfo({
      success: function(res){ fill(res); doLogin(); },
      fail: function(){ doLogin(); }
    });
  } catch(e) { doLogin(); }
  setTimeout(finish, 10000);
})
)JS";

const int kPageWaitSec = 120;

}  // namespace

const char *Backend::failText(LoginFail f) {
    switch (f) {
        case LoginFail::NoChannel: return "调试通道连接不上";
        case LoginFail::NoPage:    return "小程序页面未接入调试通道";
        case LoginFail::NoCode:    return "没拿到 wx.login code";
        case LoginFail::Rejected:  return "服务器拒绝登录";
        case LoginFail::NoToken:   return "登录响应里没有 token";
        default:                   return "未知";
    }
}

bool Backend::silentLoginOnce(LoginFail *fail) {
    const auto lg = [this](const QString &m) { if (log) log(m); };
    if (fail)
        *fail = LoginFail::None;
    if (m_cancel.load())
        return false;

    lg(QStringLiteral("  注入平台补丁…"));
    QString err;
    EvalError ec = EvalError::None;
    QJsonValue v = evalAppService(QString::fromLatin1(kPatchJs), false, 8000, &err, &ec);
    lg(QStringLiteral("     %1").arg(v.isString() ? v.toString()
                                                  : (err.isEmpty() ? QStringLiteral("(无返回)") : err)));
    if (ec == EvalError::NoChannel) {
        if (fail)
            *fail = LoginFail::NoChannel;
        return false;
    }
    if (ec == EvalError::NoPage || ec == EvalError::NoContext) {
        if (fail)
            *fail = LoginFail::NoPage;
        return false;
    }
    // A patch failure is non-fatal; continue with the login flow

    lg(QStringLiteral("  取登录材料（getUserInfo + wx.login）…"));
    err.clear();
    v = evalAppService(QString::fromLatin1(kLoginInfoJs), true, 15000, &err, &ec);
    QJsonObject info;
    if (v.isString()) {
        QJsonParseError pe{};
        QJsonDocument doc = QJsonDocument::fromJson(v.toString().toUtf8(), &pe);
        if (pe.error == QJsonParseError::NoError)
            info = doc.object();
    }
    const QString code = info["code"].toString();
    if (code.isEmpty()) {
        lg(QStringLiteral("     %1")
               .arg(info["err"].toString().isEmpty()
                        ? (err.isEmpty() ? QStringLiteral("未知原因") : err)
                        : info["err"].toString()));
        if (fail) {
            if (ec == EvalError::NoChannel)
                *fail = LoginFail::NoChannel;
            else if (ec == EvalError::NoPage || ec == EvalError::NoContext)
                *fail = LoginFail::NoPage;
            else
                *fail = LoginFail::NoCode;
        }
        return false;
    }

    lg(QStringLiteral("  login/v2 协议登录…"));
    FzwyApi api;
    if (!api.initCrypto()) {
        lg(QStringLiteral("     密钥初始化失败"));
        if (!g_config.loadError.isEmpty())
            lg(QStringLiteral("     %1").arg(g_config.loadError));
        else if (g_config.aesKey.isEmpty() || g_config.serverPubKey.isEmpty())
            lg(QStringLiteral("     config.json 缺少密钥字段（请参照 config.example.json 补全）"));
        if (fail)
            *fail = LoginFail::NoToken;
        return false;
    }
    if (!api.loginV2(code, info["iv"].toString(), info["enc"].toString(),
                     info["nick"].toString(), info["avatar"].toString(), &err)) {
        lg(QStringLiteral("     %1").arg(err));
        if (fail)
            *fail = LoginFail::Rejected;
        return false;
    }
    lg(QStringLiteral("     登录成功 uid=%1").arg(api.userId));

    // The token is embedded into a JS string literal, so escape \ and " first
    QString jsToken = api.token;
    jsToken.replace(QLatin1String("\\"), QLatin1String("\\\\"))
        .replace(QLatin1String("\""), QLatin1String("\\\""));
    const QString syncJs = QStringLiteral(
        "wx.setStorageSync(\"token\", \"%1\");"
        "wx.setStorageSync(\"userId\", %2);"
        "JSON.stringify({_os:wx.getStorageSync(\"_os\")})")
                               .arg(jsToken)
                               .arg(api.userId);
    QString osFromStorage;
    for (int attempt = 0; attempt < 2 && osFromStorage.isEmpty(); ++attempt) {
        QString e2;
        EvalError ec2 = EvalError::None;
        const QJsonValue r = evalAppService(syncJs, false, 8000, &e2, &ec2);
        if (r.isString()) {
            const QJsonDocument doc = QJsonDocument::fromJson(r.toString().toUtf8());
            osFromStorage = doc.object()["_os"].toString();
        }
        if (osFromStorage.isEmpty())
            QThread::msleep(600);
    }

    QJsonObject patch;
    patch["token"] = api.token;
    patch["user_id"] = static_cast<double>(api.userId);
    patch["user_name"] = info["nick"].toString();
    if (!osFromStorage.isEmpty())
        patch["os_value"] = osFromStorage;
    // saveState merges, so a stale package_uuid would survive an account change and
    // Tasks would skip re-probing. Clear it when user_id changes to force re-probing.
    if (loadState()["user_id"].toVariant().toLongLong() != api.userId)
        patch["package_uuid"] = QString();
    if (!saveState(patch))
        lg(QStringLiteral("   ⚠ 登录状态写入失败（%1），本次 token 重启后需重新获取")
               .arg(stateFilePath()));

    lg(QStringLiteral("  校验 token（signin）…"));
    api.osValue = osFromStorage;
    QString uo;
    if (!api.validate(&uo, &err)) {
        // The token was just issued by login/v2; not getting _os only affects the
        // version header. Save to disk and warn, don't treat as failure.
        lg(QStringLiteral("     %1（token 已保存，若后续接口 401 请重新获取）").arg(err));
        return true;
    }
    if (!uo.isEmpty()) {
        saveState(QJsonObject{{"os_value", uo}});
        lg(QStringLiteral("     _os 已刷新"));
    }
    lg(QStringLiteral("   Token 验证通过 uid=%1").arg(api.userId));
    return true;
}

bool Backend::ensureChannelAndPage() {
    const auto lg = [this](const QString &m) { if (log) log(m); };
    m_chan.setLog(log);

    lg(QStringLiteral("检查调试通道…"));
    if (!m_chan.ensure()) {
        lg(QStringLiteral("   通道不可用，已中止。请确认 PC 微信已登录。"));
        return false;
    }
    if (m_cancel.load())
        return false;

    // Wait for the mini-program: only wait and prompt, never kill processes
    // just because it is "not connected yet"
    lg(QStringLiteral("检查小程序是否已接入调试通道…"));
    if (!m_chan.pageReady()) {
        lg(QStringLiteral("   未检测到小程序。请切到微信，把该小程序打开（或关掉重开一次）并保持前台。"));
        lg(QStringLiteral("   正在等待小程序接入，最多 %1 秒…").arg(kPageWaitSec));
        // Wait in 1-second slices so the cancel flag is polled and window close exits promptly
        int lastShown = -1;
        bool ok = false;
        for (int remain = kPageWaitSec; remain > 0; --remain) {
            if (m_chan.waitForPage(1)) {
                ok = true;
                break;
            }
            if (m_cancel.load())
                break;
            if (remain % 15 == 0 && remain != lastShown) {
                lastShown = remain;
                lg(QStringLiteral("   等待中…剩余 %1 秒").arg(remain));
            }
        }
        if (!ok) {
            if (m_cancel.load()) {
                lg(QStringLiteral("   等待已取消。"));
            } else {
                lg(QStringLiteral("   等待超时：小程序仍未接入。"));
                lg(QStringLiteral("   ⚠ 调试通道保持不动，请不要关闭本工具——"
                                  "在微信里把小程序窗口关掉、再重新打开一次即可。"));
            }
            return false;
        }
    }
    return true;
}

bool Backend::getToken() {
    const auto lg = [this](const QString &m) { if (log) log(m); };
    if (m_cancel.load())
        return false;
    if (!ensureChannelAndPage())
        return false;

    // Never restart the channel on login failure - restarting would interrupt
    // the just-connected mini-program
    lg(QStringLiteral("静默登录…"));
    LoginFail fail = LoginFail::None;
    if (silentLoginOnce(&fail))
        return true;

    switch (fail) {
        case LoginFail::NoChannel:
            lg(QStringLiteral("   通道连接中断（%1）。").arg(failText(fail)));
            if (m_chan.probe() != wmpf::Session::State::Alive) {
                lg(QStringLiteral("   通道确已不可用，尝试重启一次…"));
                if (m_chan.restart()) {
                    lg(QStringLiteral("   通道已重启，请重新打开一次小程序后再点按钮。"));
                }
            } else {
                lg(QStringLiteral("   通道本身正常，稍后重试即可。"));
            }
            break;
        case LoginFail::NoPage:
            lg(QStringLiteral("   小程序页面在过程中断开了。请在微信里重新打开小程序后再点一次。"));
            break;
        case LoginFail::NoCode:
            lg(QStringLiteral("   没拿到 wx.login code：请确认该微信号在手机端登录过本小程序并完成过信息授权。"));
            break;
        case LoginFail::Rejected:
            lg(QStringLiteral("   服务器拒绝登录：code 可能已被用过，请重新打开小程序后再试。"));
            break;
        default:
            lg(QStringLiteral("   登录未完成。"));
            break;
    }
    return false;
}

bool Backend::runTasks() {
    const auto lg = [this](const QString &m) { if (log) log(m); };
    if (m_cancel.load())
        return false;

    // Tasks run over plain HTTP: with a saved token the debug channel is not
    // needed, so don't force ensureChannelAndPage() and its mini-program wait
    const QJsonObject s = loadState();
    const QString token = s["token"].toString();
    if (token.isEmpty()) {
        if (!getToken())
            return false;
    } else {
        lg(QStringLiteral("已有 Token（uid=%1），跳过登录").arg(
            s["user_id"].toVariant().toLongLong()));
    }
    if (m_cancel.load()) {
        lg(QStringLiteral("已取消，任务未开始。"));
        return false;
    }

    lg(QStringLiteral("Keys ready (built-in)"));
    auto cancelFn = [this] { return m_cancel.load(); };
    Tasks tasks(log);
    tasks.runAll(10, 5, 5, 5, cancelFn);

    // Token rejected mid-run: re-login silently and retry once. The login flow
    // itself prompts and waits for the mini-program if it is not attached yet.
    if (!tasks.authFailedFlag())
        return true;
    if (m_cancel.load())
        return false;
    lg(QStringLiteral("登录态已失效，自动重新登录（若提示请打开小程序）…"));
    if (!getToken())
        return false;
    lg(QStringLiteral("已重新登录，续跑任务…"));
    Tasks retry(log);
    retry.runAll(10, 5, 5, 5, cancelFn);
    return !retry.authFailedFlag();
}

Backend::Status Backend::probeStatus() {
    m_chan.setLog(log);
    Status st;
    switch (m_chan.probe(2500)) {
        case wmpf::Session::State::Alive: st.channelUp = true; break;
        case wmpf::Session::State::Zombie: st.channelZombie = true; break;
        default: break;
    }
    if (st.channelUp)
        st.pageReady = m_chan.pageReady(2500);
    const QJsonObject s = loadState();
    st.loggedIn = !s["token"].toString().isEmpty();
    st.uid = s["user_id"].toVariant().toLongLong();
    st.userName = s["user_name"].toString();
    return st;
}

bool Backend::restartChannel() {
    const auto lg = [this](const QString &m) { if (log) log(m); };
    m_chan.setLog(log);
    lg(QStringLiteral("手动重启调试通道…"));
    const bool ok = m_chan.restart();
    if (ok)
        lg(QStringLiteral("通道已重启，请重新打开一次小程序。"));
    return ok;
}
