# FZWYPROTOCOL

「无限挑战 / 翻转外语」微信小程序学习任务的协议分析与自动化工具（Windows，C++ / Qt6）。

## 功能

- 一键完成每日学习任务：单词卡、拼写、填空、听力、阅读、测试、词包完成、金币
- 静默登录，自动获取并刷新登录态
- 状态面板实时显示：调试通道 / 小程序 / 登录态

## 原理

注入 `FzwyHook.dll` 到微信小游戏宿主进程（WeChatAppEx.exe）开启 WMPF 调试通道，经 CDP 在小程序上下文调 `wx.login` / `wx.getUserInfo` 取登录材料，之后由协议层以 AES-128-CBC + RSA（PKCS1）混合加密体直调服务器 API。

## 使用

1. PC 微信登录，打开一次目标小程序（弹"仅支持手机端"和空白页属正常）
2. 运行 `FzwyTask.exe` → 「一键完成任务」
3. 异常时先点「刷新状态」，仍异常再「重启通道」

登录态由微信客户端实时产生，运行时需保持 PC 微信在线。

## 从源码构建

依赖：CMake + Qt 6（win64_mingw）+ MinGW 13.x：

```bat
cmake -G "MinGW Makefiles" -DCMAKE_PREFIX_PATH=C:/Qt6/6.7.3/mingw_64 -S . -B build
cmake --build build --config Release -j8
cmake --build build --target deploy
```

运行需要 exe 旁的 `config.json`（参照 `config.example.json`），字段含义与提取方法见 PROTOCOL.md 第 3、9 节。

## 目录结构

```
src/            界面、协议、调试通道实现
  wmpf/         调试通道（hook DLL、注入、偏移表、WARemoteDebug 协议翻译）
res/            图标、版本资源、WMPF 偏移表（按微信版本）
```

## 调试 CLI

在 exe 目录执行：

- `FzwyTask.exe --token` — 走一遍静默登录
- `FzwyTask.exe --run` — 登录并跑全题型
- `FzwyTask.exe --check` — 加密层自检 + 用现有 token 试 signin/任务查询
- `FzwyTask.exe --wmpf-probe` / `--wmpf-hook` / `--wmpf-serve <秒>` — 通道体检 / 手动注入 / 仅起通道

## 文档

- [PROTOCOL.md](PROTOCOL.md) — 接口与加密协议
- [ANALYSIS.md](ANALYSIS.md) — 小程序结构分析

## 免责声明

仅供学习与研究使用。
