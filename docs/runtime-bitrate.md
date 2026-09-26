# 运行中改码率（Linux / 客户端 `/bitrate`）

Moonlight 系客户端在**串流途中**改码率时，不发控制流消息，而是对**客户端控制口**（`base-5`，即 `48995`
当 `port=49000`）发一条 HTTP 请求：

```
GET /bitrate?uuid=<client uuid>&clientname=<name>&uniqueid=<moonlight unique id>&bitrate=<Kbps>
Tunnel: HTTPS（与 /launch、/resume 同一条通道，配对客户端证书认证）
User-Agent: okhttp/5.3.2         ← 实测客户端（OnePlus 13）
```

响应是 GameStream 风格的 XML：

| 情况 | `status_code` | 其余 |
|---|---|---|
| 已应用到正在跑的会话 | `200` | `root.bitrate=1`、`<xmlattr>.bitrate=<Kbps>`、`<xmlattr>.clientname` |
| 缺 `bitrate` | `400` | `<xmlattr>.status_message` |
| 码率不在 `1..800000` | `400` | `<xmlattr>.status_message` |
| 没有匹配的运行中会话 | `503` | `<xmlattr>.status_message` |

## 主机侧怎么落地这个改动

1. `nvhttp` 注册 `^/bitrate$`（客户端 HTTPS 服务器 ✔，和 `/launch` 同一套证书校验 ✔）；
2. 按**已认证的客户端证书**（`last_verified_client_cert`）匹配运行中的会话，其次按请求里的
   `uniqueid` 匹配（`stream::session::change_bitrate` ✔）；
3. 匹配到就向该会话的**视频线程**投递一个 mail 事件（`mail::bitrate`）——和既有的
   `mail::idr`（IDR 请求）走完全相同的通路 ✔；
4. 视频线程消费事件，调用编码会话的 `set_bitrate()`：
   - **NVENC**：保存初始化时的 `NV_ENC_INITIALIZE_PARAMS`/`NV_ENC_CONFIG`，用
     `nvEncReconfigureEncoder` 热改码率（并按同比例缩放 VBV 缓冲）✔；
   - 其它后端：默认返回"不支持"（诚实失败 ✔），客户端会按自己的策略处理。

## 为什么必须有这条路由

上游 LizardByte 血统**没有**这条路由 ✗，客户端拿不到它就会判定"主机不支持"，**退回到重开会话** ✗ ——
从操作者视角看就是"一改码率就断线，而且重连偶尔掉到物理屏" ✔。实测（2026-09-26）：同一台机器上
改一次码率 → 主机日志出现 `DESTINATION :: /bitrate` + `bitrate -- 2000`，8 秒后客户端重新
`/serverinfo` + `/resume` ⇒ 断开重连 ✔。

> 参考实现：`AlkaidLab/foundation-sunshine`（同为 GPL-3.0）的 `nvhttp/dynamic_params.cpp` +
> `stream::session::change_dynamic_param_for_client` + 编码器 `set_bitrate`。本实现只做**码率**
> 一种动态参数，且不引入它的 ABR/LLM 控制面。
