# 小程序离线分析报告（miniapp-cdp）

分析时间：2026-09-13  
数据目录：`~/miniapp-analysis`  
业务脚本：`scripts_full/466_app-service.js.js`（约 457KB）

## 1. 应用身份

| 项 | 值 |
|---|---|
| 产品 | 无限挑战 / 英语单词学习小程序 |
| mpCode | `fzwy` |
| API 基址 | `https://api.wuxiantiaozhan.com/` |
| 静态/音频 | `https://mp.wuxiantiaozhan.com/word/audio/`、`https://qimg.wuxiantiaozhan.com/` |
| 对象存储 | 七牛云（`up.qiniup.com` 等） |
| 备用发音 | 有道词典 `dict.youdao.com/dictvoice` |

## 2. 平台限制

`checkPlatformAndRestrict`（模块 `75ABC157...`）会在 Windows / Mac / ohos_pc 上弹窗并禁止使用，**仅允许手机端**。  
用 PC 微信 + CDP 调试时，部分页面逻辑可能被这条限制拦住。

## 3. 网络与鉴权

统一请求封装 `86648887...js`：

```js
header.CToken  = wx.getStorageSync("token")
header.version = hexMD5(wx.getStorageSync("_os"))  // 登录后写入 _os
header.mpCode  = "fzwy"
content-type   = application/json
```

失败分支：`userId Missing` → 清缓存并跳登录；其它业务错误 toast `desc`。

登录相关接口：
- `capp/crbac/mp/login`
- `capp/crbac/mp/login/v2`
- `capp/crbac/mp/login/status/wy`
- `capp/businessserver/mp/user/signin/new/1`（会写入 `_os`、皮肤、系统配置）

## 4. 加密层（核心）

模块 `E858EFC3A49727BF8E3E87C4B4F65F06.js`：

1. 启动时通过 `wx.getBackgroundFetchData({fetchType:"pre"})` 拉取密钥包：`spk / ak / aiv`
2. 本地生成 RSA 密钥对（JSEncrypt，默认 521 bit）
3. **ERequest（出站）**：
   - 用 `spk` 解出 AES key/IV 相关材料
   - 生成随机 16 字符 `s`、`c` 作为 AES key/iv
   - `ak` 字段：RSA 公钥加密 `s##c` + 明文业务 JSON
   - `sdata` 字段：AES-CBC-PKCS7 加密 `{data, rk: clientPublicKey}`，hex 大写
4. **DResponse（入站）**：RSA 私钥解 `ak` 取 `s##c`，再 AES 解 `sdata`

业务里示例：`task/query/data/v2` 使用 `ERequest({})` / `DResponse`。

另有标准 MD5（`hexMD5`）：
- 版本头 `version`
- 答题校验：`hexMD5("fzyy_" + answer) === item.answer`

## 5. 主要 API 面（77 条）

按服务前缀：
- `capp/businessserver/mp/...`：用户、签到、任务、班级/小组、皮肤、公告、周报
- `capp/wordserver/mp/...`：卡片/例句
- `capp/datacenterserver/mp/...`：排名、学习记录
- `capp/crbac/mp/...`：登录
- `cpc/qiniu/upload-policy`：上传凭证

## 6. 页面清单

- 学习：`pages/index/index`、`pages/index/wordTask`、`pages/selfLearn/index`
- 词书：`pages/wordBook/index`、`change`
- 小组：`pages/team/*`（创建/加入/管理/排行）
- 任务：`pages/createTask/*`
- 我的：`pages/my/*`、皮肤商店
- 登录/错误：`pages/login/login`、`pages/netError/netError`

## 7. 本地资产

```
miniapp-analysis/
  scripts_full/          # 完整 JS（可脱机阅读）
    466_app-service.js.js   # 业务主包（页面+逻辑）
    472_usr.js              # 同源业务补充
    435_*ServiceMainContext*  # 微信框架（体量大，一般不必细读）
  meta/                  # 目标列表、脚本清单、搜索结果
  analyze_step*.py       # 采集脚本，可重复执行
```

## 8. 脱机分析建议

1. 优先读 `466_app-service.js.js` 里 `define("...js"` 的业务模块（约 6700 行后）。
2. 关注模块 ID：
   - `86648887` 请求封装
   - `E858EFC3` 加解密
   - `67A3DAC0` MD5
   - `75ABC157` 平台限制
3. 若要还原加密，需要运行时从 `gData` 取 `spk/ak/aiv` 与本机 RSA 密钥；静态包里只有算法，没有会话密钥。
4. 在小程序里点几下任务/签到，再 `list_network_requests` 可拿到真实请求样例。

## 9. 安全备注

本分析仅针对用户本机已打开的小程序调试上下文，用于学习/研究。请勿用于未授权访问或绕过业务风控。
