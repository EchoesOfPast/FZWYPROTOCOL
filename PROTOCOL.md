# 无限挑战 / 翻转外语 小程序全套协议

- AppID: `wx3130e983e955b53e`
- mpCode: `fzwy`
- API Base: `https://api.wuxiantiaozhan.com/`
- 静态资源: `https://mp.wuxiantiaozhan.com/`、`https://qimg.wuxiantiaozhan.com/`
- 业务源码: `scripts_full/466_app-service.js.js`

协议分两层：**明文 HTTP + Header 鉴权**（大多数 GET），以及 **AES+RSA 混合加密体**（关键 POST `/v2`）。

---

## 1. 通用请求头（所有业务接口）

封装位置：`app-service.js` 模块 `86648887A49727BFE002E080A7E65F06.js`

```http
GET/POST https://api.wuxiantiaozhan.com/<path>
Content-Type: application/json
CToken: <本地 token，登录后写入>
version: <md5(_os)>
mpCode: fzwy
```

| 字段 | 来源 | 说明 |
|---|---|---|
| `CToken` | `wx.getStorageSync("token")` | 登录接口返回 |
| `version` | `hexMD5(wx.getStorageSync("_os"))` | `_os` 由签到/用户信息接口写入 |
| `mpCode` | 常量 `fzwy` | 固定 |

业务失败且带 token 时：若 `desc` 含 `userId` 且含 `Missing` → 清缓存并跳登录。

---

## 2. 登录协议

### 2.1 老登录（code 换 token）

```http
POST /capp/crbac/mp/login
Content-Type: application/json

{
  "code": "<wx.login code>",
  "iv": "<getUserInfo iv>",
  "encryptedData": "<getUserInfo encryptedData>",
  "appId": "wx3130e983e955b53e",
  "mpCode": "fzwy"
}
```

响应（明文 JSON）：

```json
{
  "success": 1,
  "data": {
    "token": "a03e8ebf0813f0d614a036ea082286b4",
    "userId": 123456
  }
}
```

客户端写入：`token`、`userId`。

### 2.2 新登录 v2（加密体）

```http
POST /capp/crbac/mp/login/v2
Content-Type: application/json

{
  "ak": "<RSA 加密的 s##c>",
  "sdata": "<AES 加密的业务体>"
}
```

明文业务体示例（加密前）：

```json
{
  "code": "<wx.login code>",
  "mpCode": "fzwy",
  "avatarUrl": "...",
  "nickName": "..."
}
```

响应：

```json
{
  "success": 1,
  "data": {
    "ak": "...",
    "sdata": "..."
  }
}
```

客户端 `DResponse` 解密后得到 `token`、`userId` 等，写入 storage。

### 2.3 登录态检查

```http
POST /capp/crbac/mp/login/status/wy
Header: CToken / version / mpCode
```

---

## 3. 密钥体系（加密层前置）

模块：`app.js` onLaunch + `E858EFC3...js`

### 3.1 启动预取密钥包

```js
wx.getBackgroundFetchData({ fetchType: "pre", success })
// 或失败后 app.loadKeysDirectly(cb)
```

预取 JSON：

```json
{
  "spk": "<服务端 AES 密钥材料，十六进制>",
  "ak":  "<默认 AES key，用于解 spk>",
  "aiv": "<默认 AES IV>"
}
```

### 3.2 客户端运行时密钥

```js
gData.spk = t.spk
gData.ak  = t.ak
gData.aiv = t.aiv
gData.thisKeyPair = new JSEncrypt({ default_key_size: 521 })
gData.cpuk = strip(thisKeyPair.getPublicKey())   // 客户端 RSA 公钥
gData.cprk = strip(thisKeyPair.getPrivateKey())  // 客户端 RSA 私钥
gData.keyReady = true
```

`strip` 去掉 `-----BEGIN/END PUBLIC KEY-----` 与换行。

---

## 4. 加密协议 ERequest / DResponse

模块：`E858EFC3A49727BF8E3E87C4B4F65F06.js`  
依赖：CryptoJS（`5552D277...js`）+ JSEncrypt（`190E24C4...js`）

### 4.1 算法

| 步骤 | 算法 | 参数 |
|---|---|---|
| 解 spk | AES-128/256-CBC + PKCS7 | key=`ak`, iv=`aiv`；输入按 Hex→Base64 再 decrypt |
| 会话钥 s,c | 随机 16 位 | 字符集 `A-Za-z0-9` |
| 传输 s##c | RSA（JSEncrypt，默认 521 bit） | 公钥=`spk` 解出的材料 `i` |
| 业务体 | AES-CBC + PKCS7 | key=`s`, iv=`c`；密文 **大写 Hex** |

### 4.2 ERequest(plainObject) → { ak, sdata }

```
i     = AES_decrypt_hex_as_b64(spk, key=ak, iv=aiv)   // 得到 RSA 公钥字符串
s     = random16()
c     = random16()
ak    = RSA_encrypt( JSON.stringify(s + "##" + c), publicKey=i )
body  = JSON.stringify({ data: plainObject, rk: cpuk })
sdata = AES_encrypt(body, key=s, iv=c).toUpperCase()  // hex
return { ak, sdata }
```

HTTP body：

```json
{ "ak": "<base64 RSA>", "sdata": "<HEX AES>" }
```

### 4.3 DResponse(resp) → plainObject

```
plain = RSA_decrypt(resp.data.ak, privateKey=cprk)   // "s##c" 或已解析 JSON
s, c  = plain.split("##")
json  = AES_decrypt_hex(resp.data.sdata, key=s, iv=c)
return JSON.parse(json)
```

响应外层仍是 `{ success, data: { ak, sdata } }` 或 `{ success, data: { ak, sdata }, ... }`。

### 4.4 使用加密体的接口（节选）

- `POST /capp/crbac/mp/login/v2`
- `POST /capp/businessserver/mp/task/query/data/v2`
- `POST /capp/businessserver/mp/task/queryall/v2`
- `POST /capp/businessserver/mp/user/signin/v2`
- `POST /capp/businessserver/mp/package/show/v2`
- `POST /capp/businessserver/mp/query/exam/list/v2`
- `POST /capp/businessserver/mp/group/query/type/v2`
- `POST /capp/businessserver/mp/team/task/test/query/all_type/v2`

命名规律：路径带 `/v2` 且 `method: POST` + `data: ERequest(...)`。

---

## 5. 业务字段与存储

| Storage Key | 写入时机 | 用途 |
|---|---|---|
| `token` | 登录成功 | `CToken` |
| `userId` | 登录成功 | 接口参数 |
| `_os` | 签到/用户信息 `data.uo` | `version=md5(_os)` |
| `nickName` / `avatar` | 登录/用户信息 | 展示与 signin |
| `currentSkin` | 用户信息 `data.skin` | UI |
| `audioType` | 音标配置 | 发音 |

MD5（模块 `67A3DAC0...`）除 `version` 外还用于答题：

```js
hexMD5("fzyy_" + answer) === item.answer
```

---

## 6. 响应约定

### 明文接口

```json
{ "success": 1, "data": { ... }, "desc": "错误文案" }
```

`success == 1` 为成功。

### 加密接口

```json
{ "success": 1, "data": { "ak": "...", "sdata": "..." } }
```

必须 `DResponse` 后才是业务 JSON。

---

## 7. 完整 API 目录（77）

### crbac

- `POST /capp/crbac/mp/login`
- `POST /capp/crbac/mp/login/v2`  *(加密)*
- `POST /capp/crbac/mp/login/status/wy`

### businessserver — 用户

- `GET  /capp/businessserver/mp/user/signin`
- `GET  /capp/businessserver/mp/user/signin/new`
- `GET  /capp/businessserver/mp/user/signin/new/1`
- `POST /capp/businessserver/mp/user/signin/v2`  *(加密)*
- `GET  /capp/businessserver/mp/user/info/get`
- `GET  /capp/businessserver/mp/user/info/edit`
- `GET  /capp/businessserver/mp/user/info/save`
- `GET  /capp/businessserver/mp/user/gold/coin/get`
- `POST /capp/businessserver/mp/user/gold/coin/budget`
- `GET  /capp/businessserver/mp/user/audio/symbol/config/query`
- `GET  /capp/businessserver/mp/user/diary/query`
- `GET  /capp/businessserver/mp/user/diary/query/list`
- `GET  /capp/businessserver/mp/user/skin/set`

### businessserver — 任务/词书/考试

- `POST /capp/businessserver/mp/task/query/data/v2`  *(加密)*
- `POST /capp/businessserver/mp/task/queryall/v2`  *(加密)*
- `GET  /capp/businessserver/mp/package/change`
- `GET  /capp/businessserver/mp/package/judge/task`
- `POST /capp/businessserver/mp/package/show/v2`  *(加密)*
- `POST /capp/businessserver/mp/query/exam/list/v2`  *(加密)*
- `POST /capp/businessserver/mp/group/query/type/v2`  *(加密)*
- `GET  /capp/businessserver/mp/group/query/wrod_fill`
- `GET  /capp/businessserver/mp/card/package/group/add`
- `GET  /capp/businessserver/mp/card/package/set/query`

### businessserver — 小组

- `GET /capp/businessserver/mp/team/list/query/newest`
- `GET /capp/businessserver/mp/team/new/add`
- `GET /capp/businessserver/mp/team/new/edit`
- `GET /capp/businessserver/mp/team/remove`
- `GET /capp/businessserver/mp/team/detail/query`
- `GET /capp/businessserver/mp/team/member/add`
- `GET /capp/businessserver/mp/team/members/new/query`
- `GET /capp/businessserver/mp/team/batch/remove`
- `GET /capp/businessserver/mp/team/leader/add`
- `GET /capp/businessserver/mp/team/leader/list/query`
- `GET /capp/businessserver/mp/team/manage/query`
- `GET /capp/businessserver/mp/team/department/query`
- `GET /capp/businessserver/mp/team/squad/member/list/query`
- `GET /capp/businessserver/mp/team/activation_code/join`
- `GET /capp/businessserver/mp/team/authcode/query`
- `GET /capp/businessserver/mp/team/condition/query`
- `GET /capp/businessserver/mp/team/move/notify/get`
- `GET /capp/businessserver/mp/team/task/add`
- `GET /capp/businessserver/mp/team/task/query`
- `GET /capp/businessserver/mp/team/task/group/query/new`
- `GET /capp/businessserver/mp/team/task/predict/query`
- `GET /capp/businessserver/mp/team/task/times/query`
- `GET /capp/businessserver/mp/team/task/query/word/count`
- `POST /capp/businessserver/mp/team/task/test/query/all_type/v2`  *(加密)*

### businessserver — 其它

- `GET /capp/businessserver/mp/notice/get`
- `GET /capp/businessserver/mp/system/time/query`
- `GET /capp/businessserver/mp/systemConfiguration/query`
- `GET /capp/businessserver/mp/systemConfiguration/edit`
- `GET /capp/businessserver/mp/skin/get`
- `GET /capp/businessserver/mp/school/accredit/get`
- `GET /capp/businessserver/mp/squad/list/query`
- `GET /capp/businessserver/mp/super/topic/user/notice/query`
- `GET /capp/businessserver/mp/super/topic/user/history/comment/query`
- `GET /capp/businessserver/mp/weekly/status`
- `GET /capp/businessserver/mp/weekly/comments/newMessage/get`
- `GET /capp/businessserver/mp/weekly/teach/note/count/query`
- `GET /capp/businessserver/mp/weekly/unread`
- `GET /capp/businessserver/mp/flush/box`
- `GET /capp/businessserver/mp/get/box`
- `GET /capp/businessserver/mp/enLevel/english_test_level`
- `GET /capp/businessserver/mp/query/wrong_exam/word`
- `GET /capp/businessserver/mp/query/wrong_exam/grammar`
- `GET /capp/businessserver/mp/query/wrong_exam/listen`

### wordserver

- `GET /capp/wordserver/mp/card/runLeft`

### datacenterserver

- `GET /capp/datacenterserver/mp/exam_rank/team_member_rank`
- `GET /capp/datacenterserver/mp/exam_rank/team_rank`
- `GET /capp/datacenterserver/mp/memberTaskRecord`
- `GET /capp/datacenterserver/mp/squad/red/dot`
- `GET /capp/datacenterserver/mp/team/exam/not/finish/query`
- `GET /capp/datacenterserver/mp/teamRank/groupMemberNew`
- `GET /capp/datacenterserver/mp/teamRank/squad/teamMemberList/detail`
- `GET /capp/datacenterserver/mp/teamRank/teamMemberList/detail`
- `GET /capp/datacenterserver/mp/teamRank/teamMemberList/detail/by_squad`
- `GET /capp/datacenterserver/mp/teamRank/teamMemberRankListNew`
- `GET /capp/datacenterserver/mp/teamRank/v2/teamRankList`
- `GET /capp/datacenterserver/mp/v2/memberLearnRecord`

### 上传

- `GET /cpc/qiniu/upload-policy` → 七牛 uptoken
- 上传域名：`https://up.qiniup.com`、`up-z1/z2`、`up-na0`、`up-as0`

---

## 8. 实测请求样例（明文层）

```http
GET /capp/businessserver/mp/systemConfiguration/query HTTP/1.1
Host: api.wuxiantiaozhan.com
CToken: a03e8ebf0813f0d614a036ea082286b4
version: <md5(_os)>
mpCode: fzwy
content-type: application/json
```

```http
GET /capp/businessserver/mp/enLevel/english_test_level?userId=123456 HTTP/1.1
Host: api.wuxiantiaozhan.com
CToken: a03e8ebf0813f0d614a036ea082286b4
version: ...
mpCode: fzwy
```

---

## 9. 还原步骤（脱机/脚本）

1. **拿密钥包**  
   运行时从 `getApp().gData` 读 `spk/ak/aiv/cpuk/cprk`；或拦截 `getBackgroundFetchData` 预取包。

2. **明文接口**  
   只需 `CToken` + `version=md5(_os)` + `mpCode=fzwy`。  
   `_os` 从 `GET /capp/businessserver/mp/user/signin/new/1` 响应 `data.uo` 取得。

3. **加密接口**  
   实现 `ERequest`/`DResponse`（见 §4），body 用 `{ak, sdata}`。  
   注意：`spk` 用 `ak/aiv` AES 解出来的才是 RSA 公钥；会话钥 `s,c` 每次随机。

4. **答题校验**  
   `answer_hash = md5("fzyy_" + 明文答案)`。

5. **调试入口**  
   - 业务主包：`scripts_full/466_app-service.js.js`  
   - 请求封装：模块 `86648887...`  
   - 加解密：模块 `E858EFC3...`  
   - MD5：模块 `67A3DAC0...`  
   - 平台限制：模块 `75ABC157...`（`checkPlatformAndRestrict`）

---

## 10. 注意

- PC 端有 `checkPlatformAndRestrict`，会弹窗并跳过密钥初始化；调试需伪装 `platform=ios` 或改掉该函数。
- 会话密钥 `s,c` 与 RSA 密钥对每次启动/每次请求都可能不同，不能写死。
- 密钥包来自微信预取，**没有 `spk/ak/aiv` 就无法构造加密接口**。
- 仅用于授权范围内的学习与研究。
