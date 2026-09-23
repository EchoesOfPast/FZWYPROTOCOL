#include "WmpfInject.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QThread>

#include "WmpfOffsets.h"
#include "WmpfProbe.h"

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <tlhelp32.h>
#endif

namespace wmpf {
namespace {

#ifdef _WIN32

struct ProcInfo {
    quint32 pid = 0;
    quint32 ppid = 0;
};

QString winErr() { return QStringLiteral("Win32 错误 %1").arg(GetLastError()); }

// Must match the injector's max possible displacement: the hook DLL's displacement
// buffer is 32 bytes (kMaxDisplaced in FzwyHook.cpp; the probe buffer is 32 too), so
// restoring only 16 would leave patch residue when displacedLen > 16.
constexpr int kPrologueRestoreLen = 32;

QVector<ProcInfo> listWeChatAppEx() {
    QVector<ProcInfo> out;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE)
        return out;
    PROCESSENTRY32W pe{};
    pe.dwSize = sizeof(pe);
    if (Process32FirstW(snap, &pe)) {
        do {
            if (_wcsicmp(pe.szExeFile, L"WeChatAppEx.exe") == 0) {
                ProcInfo p;
                p.pid = pe.th32ProcessID;
                p.ppid = pe.th32ParentProcessID;
                out.append(p);
            }
        } while (Process32NextW(snap, &pe));
    }
    CloseHandle(snap);
    return out;
}

// Same strategy as frida's findWmpfProcess: the most frequently occurring ppid is the main host process pid
quint32 pickMainProcess(const QVector<ProcInfo> &procs) {
    if (procs.isEmpty())
        return 0;
    QHash<quint32, int> freq;
    for (const ProcInfo &p : procs)
        freq[p.ppid]++;
    quint32 best = 0;
    int bestCount = -1;
    for (auto it = freq.begin(); it != freq.end(); ++it) {
        if (it.value() > bestCount) {
            bestCount = it.value();
            best = it.key();
        }
    }
    for (const ProcInfo &p : procs) {
        if (p.pid == best)
            return best;
    }
    return 0;
}

QString modulePathOf(quint32 pid) {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, pid);
    if (snap == INVALID_HANDLE_VALUE)
        return {};
    MODULEENTRY32W me{};
    me.dwSize = sizeof(me);
    QString out;
    if (Module32FirstW(snap, &me))
        out = QString::fromWCharArray(me.szExePath);
    CloseHandle(snap);
    return out;
}

// Write FzwyHook.cfg (the hook DLL reads it from its own directory).
// hooks= lists only hooks actually enabled: cdpFilter is disabled wholesale in the DLL
// due to the CET shadow stack (see FzwyHook.cpp header); including it would just earn a
// misleading "SendToClientFilter=failed" log line every round.
bool writeConfig(const QString &cfgPath, const QString &module, const Offsets &off, QString *err) {
    QStringList scene;
    for (int v : off.sceneOffsets)
        scene << QString::number(v);
    const QString body = QStringLiteral(
                             "module=%1\n"
                             "loadStart=0x%2\n"
                             "cdpFilter=0x%3\n"
                             "hooks=loadStart\n"
                             "scene=%4\n")
                             .arg(module)
                             .arg(off.loadStart, 0, 16)
                             .arg(off.cdpFilter, 0, 16)
                             .arg(scene.join(QLatin1Char(',')));
    QFile f(cfgPath);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        if (err)
            *err = QStringLiteral("写配置失败：%1").arg(cfgPath);
        return false;
    }
    f.write(body.toUtf8());
    f.close();
    return true;
}

int injectDll(quint32 pid, const QString &dllPath, QString *err) {
    const DWORD access = PROCESS_CREATE_THREAD | PROCESS_QUERY_INFORMATION | PROCESS_VM_OPERATION |
                         PROCESS_VM_WRITE | PROCESS_VM_READ;
    HANDLE h = OpenProcess(access, FALSE, pid);
    if (!h) {
        if (err)
            *err = QStringLiteral("OpenProcess 失败（%1）—— 可能需要管理员权限").arg(winErr());
        return 1;
    }

    const std::wstring wpath = dllPath.toStdWString();
    const SIZE_T bytes = (wpath.size() + 1) * sizeof(wchar_t);
    void *remote = VirtualAllocEx(h, nullptr, bytes, MEM_COMMIT | MEM_RESERVE,
                                  PAGE_READWRITE);
    if (!remote) {
        if (err)
            *err = QStringLiteral("VirtualAllocEx 失败（%1）").arg(winErr());
        CloseHandle(h);
        return 2;
    }
    if (!WriteProcessMemory(h, remote, wpath.c_str(), bytes, nullptr)) {
        if (err)
            *err = QStringLiteral("WriteProcessMemory 失败（%1）").arg(winErr());
        VirtualFreeEx(h, remote, 0, MEM_RELEASE);
        CloseHandle(h);
        return 3;
    }

    HMODULE k32 = GetModuleHandleW(L"kernel32.dll");
    auto loadLib = reinterpret_cast<LPTHREAD_START_ROUTINE>(
        reinterpret_cast<void *>(GetProcAddress(k32, "LoadLibraryW")));
    HANDLE th = CreateRemoteThread(h, nullptr, 0, loadLib, remote, 0, nullptr);
    if (!th) {
        if (err)
            *err = QStringLiteral("CreateRemoteThread 失败（%1）").arg(winErr());
        VirtualFreeEx(h, remote, 0, MEM_RELEASE);
        CloseHandle(h);
        return 4;
    }
    const DWORD w = WaitForSingleObject(th, 15000);
    if (w != WAIT_OBJECT_0) {
        // Do not VirtualFreeEx on timeout: the LoadLibraryW thread may still be reading
        // the remote path string; freeing it could crash the target process. Leak the
        // few bytes instead.
        CloseHandle(th);
        CloseHandle(h);
        if (err)
            *err = QStringLiteral("等待 LoadLibrary 线程超时");
        return 5;
    }
    DWORD exitCode = 0;
    GetExitCodeThread(th, &exitCode);
    CloseHandle(th);
    VirtualFreeEx(h, remote, 0, MEM_RELEASE);
    CloseHandle(h);

    // GetExitCodeThread returns only the low 32 bits of the HMODULE; on x64 a module
    // base whose low 32 bits happen to be 0 (extremely rare) would be misreported as a
    // load failure. LoadLibraryW offers no better cross-process return channel, so this
    // false-positive rate is accepted.
    if (exitCode == 0) {
        if (err)
            *err = QStringLiteral(
                "目标进程 LoadLibrary 返回 0。常见原因：DLL 与目标位数不符、路径不可达，"
                "或 DLL 仍依赖未静态链接的运行库（libstdc++-6.dll / libgcc_s_seh-1.dll）");
        return 6;
    }
    return 0;
}

bool queryTarget(quint32 pid, const QString &moduleName, const Offsets &off, quint64 *base,
                 QString *modulePath, bool *hooked) {
    *base = 0;
    *hooked = false;
    modulePath->clear();
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, pid);
    if (snap != INVALID_HANDLE_VALUE) {
        MODULEENTRY32W me{};
        me.dwSize = sizeof(me);
        if (Module32FirstW(snap, &me)) {
            do {
                if (moduleName.compare(QString::fromWCharArray(me.szModule),
                                       Qt::CaseInsensitive) == 0) {
                    *base = reinterpret_cast<quint64>(me.modBaseAddr);
                    *modulePath = QString::fromWCharArray(me.szExePath);
                    break;
                }
            } while (Module32NextW(snap, &me));
        }
        CloseHandle(snap);
    }
    if (!*base)
        return false;
    HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | PROCESS_VM_READ, FALSE, pid);
    if (!h)
        return true;  // got the base address, just can't read memory
    quint8 b = 0;
    SIZE_T got = 0;
    if (ReadProcessMemory(h, reinterpret_cast<LPCVOID>(*base + off.loadStart), &b, 1, &got) &&
        got == 1)
        *hooked = (b == 0xE9);
    CloseHandle(h);
    return true;
}

// Restore a function entry that was overwritten by a leftover hook.
// When the previous tool (frida) was killed with taskkill /F, the agent had no time to
// restore the entry, leaving E9 in memory and blocking us from hooking.
// Here we restore the original bytes from the on-disk module file.
bool restorePrologue(quint32 pid, quint64 moduleBase, const QString &modulePath, quint64 rva,
                     int len, QString *note) {
    QByteArray orig;
    if (!readModuleBytesFromDisk(modulePath, rva, len, &orig) || orig.size() < len) {
        *note = QStringLiteral("从磁盘取原始序言失败");
        return false;
    }
    HANDLE h = OpenProcess(PROCESS_VM_OPERATION | PROCESS_VM_WRITE | PROCESS_VM_READ, FALSE, pid);
    if (!h) {
        *note = QStringLiteral("OpenProcess 失败（%1）").arg(winErr());
        return false;
    }
    void *addr = reinterpret_cast<void *>(moduleBase + rva);
    DWORD oldProt = 0;
    bool ok = false;
    if (VirtualProtectEx(h, addr, SIZE_T(len), PAGE_EXECUTE_READWRITE, &oldProt)) {
        ok = WriteProcessMemory(h, addr, orig.constData(), SIZE_T(len), nullptr);
        VirtualProtectEx(h, addr, SIZE_T(len), oldProt, &oldProt);
        FlushInstructionCache(h, addr, SIZE_T(len));
    }
    CloseHandle(h);
    *note = ok ? QStringLiteral("已从磁盘还原 %1 字节原始序言").arg(len)
               : QStringLiteral("还原失败（%1）").arg(winErr());
    return ok;
}

// All loaded hook DLL names in the target process.
// Note the emphasis on "all": during an upgrade, multiple variants may be loaded sequentially
// in the same process; taking only the first would get the old version, and commanding the old
// version to unhook would also wipe the new version's hook — all variants share the same
// target entry bytes (verified the hard way).
QStringList loadedHookDllNames(quint32 pid) {
    QStringList found;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, pid);
    if (snap == INVALID_HANDLE_VALUE)
        return found;
    MODULEENTRY32W me{};
    me.dwSize = sizeof(me);
    if (Module32FirstW(snap, &me)) {
        do {
            const QString n = QString::fromWCharArray(me.szModule);
            if (n.startsWith(QStringLiteral("FzwyHook"), Qt::CaseInsensitive) &&
                n.endsWith(QStringLiteral(".dll"), Qt::CaseInsensitive))
                found.append(n);
        } while (Module32NextW(snap, &me));
    }
    CloseHandle(snap);
    return found;
}

// The command event name includes the DLL file name: multiple versions may be loaded
// sequentially in the same process; we must be able to command a specific one precisely
// (an auto-reset event would be grabbed by a random waiting thread otherwise).
QString dllBaseName(const QString &dllFileName) {
    QString n = dllFileName;
    const int dot = n.lastIndexOf(QLatin1Char('.'));
    return dot > 0 ? n.left(dot) : n;
}

bool setNamedEvent(const QString &eventName) {
    HANDLE ev = OpenEventW(EVENT_MODIFY_STATE, FALSE,
                           reinterpret_cast<const wchar_t *>(eventName.utf16()));
    if (!ev)
        return false;
    const bool ok = SetEvent(ev) != 0;
    CloseHandle(ev);
    return ok;
}

bool triggerReinstall(quint32 pid, const QString &dllFileName) {
    return setNamedEvent(QStringLiteral("FzwyHookCmd_%1_%2")
                             .arg(pid)
                             .arg(dllBaseName(dllFileName)));
}

bool triggerUnhook(quint32 pid, const QString &dllFileName) {
    return setNamedEvent(QStringLiteral("FzwyHookUnhook_%1_%2")
                             .arg(pid)
                             .arg(dllBaseName(dllFileName)));
}

#endif  // _WIN32

}  // namespace

QString hookDllPath() {
    return QCoreApplication::applicationDirPath() + QStringLiteral("/FzwyHook.dll");
}

// Actual file path used for injection: copies a FzwyHook_<hash>.dll variant by content hash.
// Rationale: once a DLL is injected, Windows locks the file and recompilation fails
// ("ld: cannot open output file FzwyHook.dll: Permission denied").
// By using a different name each time the content changes, the original file is never locked.
QString hookDllVariantPath(QString *err) {
    const QString src = hookDllPath();
    QFile f(src);
    if (!f.open(QIODevice::ReadOnly)) {
        if (err)
            *err = QStringLiteral("读不到 %1").arg(src);
        return {};
    }
    QCryptographicHash h(QCryptographicHash::Sha256);
    h.addData(&f);
    f.close();
    const QString tag = QString::fromLatin1(h.result().toHex().left(12));
    const QString dst =
        QFileInfo(src).absolutePath() + QStringLiteral("/FzwyHook_%1.dll").arg(tag);
    if (!QFileInfo::exists(dst) && !QFile::copy(src, dst)) {
        if (err)
            *err = QStringLiteral("复制副本失败：%1").arg(dst);
        return {};
    }
    // Clean up old variants (loaded ones can't be deleted; ignore failures)
    const QDir dir(QFileInfo(src).absolutePath());
    for (const QString &name : dir.entryList({QStringLiteral("FzwyHook_*.dll")}, QDir::Files)) {
        if (name == QFileInfo(dst).fileName())
            continue;
        QFile::remove(dir.absoluteFilePath(name));
    }
    return dst;
}

QString hookLogTail(int lines) {
    QFile f(QCoreApplication::applicationDirPath() + QStringLiteral("/FzwyHook.log"));
    if (!f.open(QIODevice::ReadOnly))
        return {};
    const QString all = QString::fromUtf8(f.readAll());
    const QStringList rows = all.split(QLatin1Char('\n'), Qt::SkipEmptyParts);
    return rows.mid(qMax(0, rows.size() - lines)).join(QLatin1Char('\n'));
}

HookInstallResult ensureHooked(const std::function<void(const QString &)> &log) {
    HookInstallResult r;
#ifdef _WIN32
    // Variant path is cached by content hash to avoid recomputing every round (requires reading the entire DLL)
    static QString cachedVariant;
    if (cachedVariant.isEmpty()) {
        QString err;
        cachedVariant = hookDllVariantPath(&err);
        if (cachedVariant.isEmpty()) {
            r.message = err;
            return r;
        }
    }
    const QString want = QFileInfo(cachedVariant).fileName();

    const QVector<ProcInfo> procs = listWeChatAppEx();
    const quint32 pid = pickMainProcess(procs);
    if (!pid) {
        r.message = QStringLiteral("当前没有 WeChatAppEx 主宿主进程");
        return r;
    }

    const QString exePath = modulePathOf(pid);
    const int version = versionFromPath(exePath);
    Offsets off;
    if (!lookup(version, &off)) {
        r.message = QStringLiteral("WMPF 版本 %1 无内置偏移").arg(version);
        return r;
    }
    const QString module = targetModuleFor(version);
    quint64 base = 0;
    QString modulePath;
    bool hooked = false;
    if (queryTarget(pid, module, off, &base, &modulePath, &hooked) && hooked &&
        loadedHookDllNames(pid).contains(want, Qt::CaseInsensitive)) {
        r.ok = true;
        r.alreadyHooked = true;
        r.message = QStringLiteral("hook 已就绪（pid=%1）").arg(pid);
        return r;
    }

    const HookInstallResult hr = installHook(log);
    r.ok = hr.ok;
    r.message = hr.message;
    r.hookLogTail = hr.hookLogTail;
#else
    r.message = QStringLiteral("只支持 Windows");
#endif
    return r;
}

HookInstallResult installHook(const std::function<void(const QString &)> &log) {
    const auto lg = [&log](const QString &m) {
        if (log)
            log(m);
    };
    HookInstallResult r;

#ifdef _WIN32
    const QString srcDll = hookDllPath();
    if (!QFileInfo::exists(srcDll)) {
        r.message = QStringLiteral("找不到 %1").arg(srcDll);
        return r;
    }
    QString variantErr;
    const QString dll = hookDllVariantPath(&variantErr);
    if (dll.isEmpty()) {
        r.message = variantErr;
        return r;
    }

    const QVector<ProcInfo> procs = listWeChatAppEx();
    if (procs.isEmpty()) {
        r.message = QStringLiteral("没有 WeChatAppEx.exe 进程——请先登录 PC 微信并打开一次小程序");
        return r;
    }
    const quint32 pid = pickMainProcess(procs);
    if (!pid) {
        r.message = QStringLiteral("无法确定承载小程序的主宿主进程");
        return r;
    }
    lg(QStringLiteral("主宿主进程 pid=%1（共 %2 个 WeChatAppEx）").arg(pid).arg(procs.size()));
    const QString exePath = modulePathOf(pid);
    const int version = versionFromPath(exePath);
    Offsets off;
    if (!lookup(version, &off)) {
        const QVector<int> vers = availableVersions();
        r.message = vers.isEmpty()
                        ? QStringLiteral("WMPF 版本 %1 没有内置偏移（内置偏移表为空，请检查资源是否打包）")
                              .arg(version)
                        : QStringLiteral("WMPF 版本 %1 没有内置偏移（内置 %2–%3）")
                              .arg(version)
                              .arg(vers.first())
                              .arg(vers.last());
        return r;
    }
    const QString module = targetModuleFor(version);
    lg(QStringLiteral("WMPF %1，目标模块 %2，偏移 loadStart=0x%3 cdpFilter=0x%4")
           .arg(version)
           .arg(module)
           .arg(off.loadStart, 0, 16)
           .arg(off.cdpFilter, 0, 16));

    quint64 base = 0;
    QString modulePath;
    bool hooked = false;
    if (!queryTarget(pid, module, off, &base, &modulePath, &hooked)) {
        r.message = QStringLiteral("进程 %1 里没找到模块 %2").arg(pid).arg(module);
        return r;
    }

    const QString dllName = QFileInfo(dll).fileName();
    const QStringList loadedNames = loadedHookDllNames(pid);
    const bool wantLoaded =
        loadedNames.contains(dllName, Qt::CaseInsensitive);
    // Command all non-current-version old variants to unhook.
    // They share the same target entry bytes; if even one old version is still hooked
    // (or still holds restore capability), it will wipe the new version's hook.
    int oldCount = 0;
    for (const QString &n : loadedNames) {
        if (n.compare(dllName, Qt::CaseInsensitive) == 0)
            continue;
        if (triggerUnhook(pid, n)) {
            ++oldCount;
            QThread::msleep(300);
        }
    }
    if (oldCount > 0)
        lg(QStringLiteral("已清理 %1 个旧版 hook").arg(oldCount));
    if (!loadedNames.isEmpty() && !wantLoaded)
        QThread::msleep(300);
    if (!loadedNames.isEmpty())
        queryTarget(pid, module, off, &base, &modulePath, &hooked);
    const bool dllLoaded = wantLoaded;
    if (hooked) {
        if (dllLoaded) {
            // Our own hook: have the DLL re-install itself (idempotent; will detect E9 and skip)
            r.alreadyHooked = true;
        } else {
            // Leftover from another tool (frida) that was taskkill /F'd before the agent could restore
            lg(QStringLiteral("目标入口残留 E9 但本工具 DLL 不在 → 先从磁盘还原原始序言"));
            QString note;
            if (!restorePrologue(pid, base, modulePath, off.loadStart, kPrologueRestoreLen, &note)) {
                r.message = QStringLiteral("还原残留 hook 失败：%1").arg(note);
                return r;
            }
            lg(QStringLiteral("  %1").arg(note));
        }
    }

    const QString dir = QFileInfo(dll).absolutePath();
    const QString cfg = dir + QStringLiteral("/FzwyHook.cfg");
    QString err;
    if (!writeConfig(cfg, module, off, &err)) {
        r.message = err;
        return r;
    }
    // The ready event must include the version name: after the old DLL is unhooked it still
    // calls signalReady(); if the name were the same it would prematurely satisfy our wait,
    // resulting in "reports success but the new version was never loaded".
    wchar_t evName[128];
    swprintf(evName, 128, L"FzwyHookReady_%lu_%ls", static_cast<unsigned long>(pid),
             reinterpret_cast<const wchar_t *>(dllBaseName(dllName).utf16()));
    HANDLE ev = CreateEventW(nullptr, TRUE, FALSE, evName);
    if (!ev) {
        r.message = QStringLiteral("CreateEvent 失败（%1）").arg(winErr());
        return r;
    }
    // A same-named event may linger from a previous run (manual-reset, already signaled
    // by the old DLL); CreateEventW ignores the initial-state argument when opening an
    // existing object, so without ResetEvent the wait below would falsely succeed at once.
    ResetEvent(ev);

    lg(QStringLiteral("注入 %1 → pid %2").arg(QFileInfo(dll).fileName()).arg(pid));
    if (dllLoaded) {
        if (!triggerReinstall(pid, dllName)) {
            r.message = QStringLiteral("触发重新安装失败（找不到命令事件）");
            CloseHandle(ev);
            return r;
        }
    } else {
        const int rc = injectDll(pid, dll, &err);
        if (rc != 0) {
            r.message = err;
            r.hookLogTail = hookLogTail();
            CloseHandle(ev);
            return r;
        }
    }

    const DWORD w = WaitForSingleObject(ev, 20000);
    CloseHandle(ev);
    r.hookLogTail = hookLogTail();
    if (w != WAIT_OBJECT_0) {
        r.message = QStringLiteral("注入成功但 hook 未在 20 秒内就绪（看日志尾部）");
        return r;
    }
    r.ok = true;
    r.message = QStringLiteral("hook 已安装（pid=%1）").arg(pid);
    lg(r.message);
#else
    r.message = QStringLiteral("只支持 Windows");
#endif

    return r;
}

int unhookAll(const std::function<void(const QString &)> &log) {
#ifdef _WIN32
    const auto lg = [&log](const QString &m) { if (log) log(m); };
    const QVector<ProcInfo> procs = listWeChatAppEx();
    int commanded = 0;
    for (const ProcInfo &p : procs) {
        const QStringList names = loadedHookDllNames(p.pid);
        for (const QString &n : names) {
            if (triggerUnhook(p.pid, n)) {
                lg(QStringLiteral("已卸载 %1 (pid=%2)").arg(n).arg(p.pid));
                ++commanded;
            }
        }
    }
    return commanded;
#else
    Q_UNUSED(log);
    return 0;
#endif
}

}  // namespace wmpf
