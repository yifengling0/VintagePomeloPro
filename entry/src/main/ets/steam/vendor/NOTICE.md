# NOTICE

本目录（`entry/src/main/ets/steam/vendor/`）包含来自
[OpenHarmonyASF](https://github.com/DaLongZhuaZi/OpenHarmonyASF)（`steam_core`）的源码文件，
按 **Apache License 2.0** 授权引入。原许可全文见本目录 `LICENSE`。

- 引入版本：2026-09 从 main 分支快照拷贝
- 修改说明：仅裁剪了与本工程无关的可选依赖文件；逻辑未改动
- 逻辑修改（2026-09-28，对真 Steam 实测后）：
  `auth/SteamAuthService.ets` beginFields 移除 `device_details` 表单字段 ——
  Steam 表单模式拒收该字段（HTTP 400），任何 JSON 内容变体均被拒，省略即通过。
  上游原实现对真 Steam 的 HTTP 登录未做过端到端验证（RSA 公钥注释自述 fixture-facing）。
  `auth/SteamAuthProtocol.ets` SteamAuthEndpoint 枚举新增 `BEGIN_QR_SESSION` ——
  实测 BeginAuthSessionViaQR 纯 HTTP 表单即可用（上游注释称 QR 需 CM WebSocket，与实测不符），
  QR 发起/轮询逻辑在宿主 `steam/SteamService.ets`。
  `auth/SteamAuthService.ets` completeChallenge 行为修改：MOBILE_CONFIRMATION 会话收到验证码时
  改为按令牌码(code_type 3)提交而非无视（Steam 的 allowed_confirmations 通常同时含手机确认与 TOTP，
  parseChallenge 原实现无条件优先手机确认，导致只能去手机上点确认）。
- 平台适配（HTTP 传输 / 加密 / 时钟 / 存储）在本目录之外的 `entry/src/main/ets/steam/` 提供；
  其中 `SteamHttpTransport.postForm` 对 GET-only 端点（GetPasswordRSAPublicKey，POST 405）做 405→GET 重发。
