# 学习任务助手（无限挑战 · 一键完成）

Windows 桌面工具（C++ / Qt6），自动完成「无限挑战 / 翻转外语」微信小程序的每日学习任务（单词卡、拼写、填空、听力、阅读、测试、词包完成、金币）。

## 原理概述

- 注入 `FzwyHook.dll` 到微信小游戏宿主进程（WeChatAppEx.exe），开启 WMPF 调试通道（本仓库为纯原生实现，不依赖 frida / Node.js）
- 通过 CDP 在小程序上下文里调 `wx.login` / `wx.getUserInfo` 获取登录材料
- 之后全部走协议层：AES-128-CBC + RSA-1024（PKCS1）混合加密体直调服务器 API（Windows CNG/NCrypt 实现，零第三方库依赖）

协议细节见 [PROTOCOL.md](PROTOCOL.md)，小程序结构分析见 [ANALYSIS.md](ANALYSIS.md)。

## 使用（分发包）

1. PC 微信登录，打开一次目标小程序（弹"仅支持手机端"和空白页属正常，不用理会）
2. 运行 `FzwyTask.exe` → 「一键完成任务」
3. 状态区实时显示 调试通道 / 小程序 / 登录态；异常时先点「刷新状态」，仍异常再「重启通道」

无需安装 Node.js 或任何调试工具。无微信客户端时无法登录（`wx.login` 的 code 必须由真实微信客户端产生）。

## 从源码构建

依赖：CMake + Qt 6（win64_mingw）+ MinGW 13.x：

```bat
cmake -G "MinGW Makefiles" -DCMAKE_PREFIX_PATH=C:/Qt6/6.7.3/mingw_64 -S . -B build
cmake --build build --config Release -j8
cmake --build build --target deploy   :: 收集 Qt/MinGW 运行时到 dist/FzwyTask
```

**运行前需要 `config.json`**（放在 exe 旁，参照 `config.example.json`）：协议密钥（`aesKey`/`aesIv`/`serverPubKey`）、`appId`、`mpCode`、`baseUrl` 等需自行从目标小程序提取（方法见 PROTOCOL.md 第 3、9 节）。本仓库不包含任何密钥。

## 目录结构

```
src/            C++ 源码（Qt6 Widgets 界面 + 协议逻辑 + WMPF 注入层）
  wmpf/         调试通道（hook DLL、注入、偏移表、WARemoteDebug 协议翻译）
res/            图标、版本资源、WMPF 偏移表（按微信版本）
config.example.json
```

## 调试 CLI

在 exe 目录执行：

- `FzwyTask.exe --token` — 走一遍静默登录
- `FzwyTask.exe --run` — 登录并跑全题型
- `FzwyTask.exe --check` — 加密层自检 + 用现有 token 试 signin/任务查询
- `FzwyTask.exe --wmpf-probe` / `--wmpf-hook` / `--wmpf-serve <秒>` — 通道体检 / 手动注入 / 仅起通道

## 免责声明

本项目仅供学习与研究使用，不得用于任何违反目标服务条款的用途。使用本工具产生的一切后果由使用者自行承担。
