# AGENTS.md

## 项目一句话

Windows 桌面工具（C++ / Qt6 Widgets + MinGW），通过注入 WeChatAppEx.exe 开启 WMPF 调试通道拿到小程序登录态，再用 AES-128-CBC + RSA(PKCS1) 加密协议直调服务器 API，自动完成学习任务。

## 构建

```bat
cmake -G "MinGW Makefiles" -DCMAKE_PREFIX_PATH=C:/Qt6/6.7.3/mingw_64 -S . -B build
cmake --build build --config Release -j8
cmake --build build --target deploy      :: 收集 Qt/MinGW 运行时到 dist/FzwyTask
```

- 依赖：Qt 6.7.3（win64_mingw）+ MinGW 13.1。Qt 是在线安装的，两者 ABI 必须匹配，混用别的 MinGW 会让 moc 起不来
- exe 运行需要旁边的 `config.json`（协议密钥，不进仓库；仓库里只有 `config.example.json`）

## 发版

推 tag 即可，CI（`.github/workflows/release.yml`）自动构建、注入 `CONFIG_JSON` secret、打包、建 Release：

```bat
git tag v1.2.x && git push origin v1.2.x
```

- 版本号：`CMakeLists.txt` 的 `FZWY_VERSION` 默认值（本地构建用）；CI 用 tag 名注入
- `CONFIG_JSON` secret 在仓库 Settings → Secrets 维护

## 验证

- `dist/FzwyTask/FzwyTask.exe --check`：AES/RSA 加密层自检（不需要微信）
- `--token` / `--run`：需微信在线 + 小程序打开，验证完整链路
- `--screenshot <png>`：离屏渲染截图验证 UI

## 目录

```
src/            UI（MainWindow）、流程编排（Backend）、CDP 客户端、协议（FzwyApi/Crypto/Tasks）
src/wmpf/       调试通道：hook DLL（hook/FzwyHook.cpp）、注入（WmpfInject）、
                偏移表（WmpfOffsets）、WARemoteDebug↔CDP 翻译（WarRemoteDebug/WmpfChannel）
res/            图标（make_icon.py 重新生成）、app.rc.in（版本资源模板）、wmpf-offsets/（按微信版本）
```

## 容易踩的坑（都真实发生过）

- **关窗卡死**：moveToThread 只能在对象的家线程做（"push"），在 worker 线程调用会被 Qt 静默拒绝——通道线程在 Session 构造时创建，不要在 worker 里创建
- **改 app.ico 后 exe 图标不变**：app.rc 是 configure 阶段生成的，ico 变化不触发 windres 重编，删 `build/CMakeFiles/FzwyTask.dir/app.rc.obj` 再构建
- **链接报 multiple definition**：MinGW make 并行偶发 objects.a 损坏，`rm build/CMakeFiles/*.dir/objects.a` 重建
- **hook 装上却检测不到小程序**：小程序必须在 hook 安装之后重新打开（loadStart 钩子只在加载瞬间触发）
- **401**：协议请求绝不能带 `userId` 头（带了必 401），用户身份服务端从 CToken 解析
- **词包探测**：`package/show/v2` 返回的是词书组 id，要经 `card/package/set/query` 解析成卡包 id（packageUuid），直接用 groupId 必失败
- **杀软**：360/电脑管家/Defender 会拦截向微信注入未签名 DLL（CreateRemoteThread+LoadLibrary），工具内已有分类诊断；分发上上策是代码签名
