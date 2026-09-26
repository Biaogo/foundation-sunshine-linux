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


## Linux 现状（2026-09-26 实测，必须知道）

**这个端点在 Linux 上"能收、能保住会话"，但改不动码率。** 实测链（CUDA 构建，`h264_nvenc`）：

```
Info: Bitrate change to 200000 Kbps handed to a running session   ← /bitrate 收到并找到会话
Warning: The running encoder could not change its bitrate to 200000 Kbps   ← 22 ms 后
```

原因不是逻辑错，是**平台错**：

- Linux 上 NVENC 编码走的是 **ffmpeg 的 `h264_nvenc` / `hevc_nvenc`**（`video.cpp` 里 `nvenc` 这个 encoder_t 的
  内部名字就是它们），**不是** `src/nvenc/` 那套 NVENC SDK 封装 ✗；
- `src/nvenc/` 的 SDK 封装在 Linux 构建里**没有编进来**（二进制里 `NvEncReconfigureEncoder` 出现 0 次 ✗）；
- 因此 `encode_session` 的 `set_bitrate()` 覆盖里 `!device->nvenc` 恒为真 ⇒ 直接 `return false` ✗；
- 而 `nvEncReconfigureEncoder()` 那条热改路径（`nvEncReconfigureEncoder` + VBV 同比缩放 ✗）只在
  **Windows（SDK 路径）** 上成立 ✗。

**LLM/开发者注意**：`GET /bitrate` 的存在 + 会话不中断，**不等于**码率真的变了 ✗。日志里出现
`NvEnc: video bitrate changed to … Kbps` 才算真的改了 ✔ —— 在 Linux 上这条**永远不会**出现 ✗。

### 已实现并实测通过（2026-09-26 晚）

Linux 上**不需要重建上下文** ✗：ffmpeg 的 NVENC 封装在**每一帧提交前**都会比对 `avctx->bit_rate` /
`rc_max_rate` / `rc_buffer_size` 与编码器当前配置 ✗，任一不同就调用 `nvEncReconfigureEncoder()` 并
**自己 forceIDR** ✔。所以只要在编码线程里改这三个字段即可 ✔。

实现（`src/video.cpp`）：`set_bitrate()` 在 HTTP 线程**只记下请求**（`std::atomic` ✗），
`apply_pending_bitrate()` 在编码线程**下一帧前应用** ✔；VBV 按同比例缩放以保持帧数深度 ✔
（纯函数 `video::scale_bitrate_budget()` @ `src/video_bitrate.h` ✔，带单测 ✔）。

实测：V+ 在**一条会话里连改 5 次**（2000 → 1000 → 200000 → 10000 → 3000 Kbps ✗）⇒ 零失败、零掉线 ✔：

```
Info:  Video bitrate: codec context moved to 1000 Kbps          ← 本仓库
Debug: [hevc_nvenc] avg bitrate change: 2000000 -> 1000000      ← ffmpeg 内部日志
Debug: [hevc_nvenc] max bitrate change: 2000000 -> 1000000      ← ffmpeg 内部日志
Debug: [hevc_nvenc] vbv buffer size change: 22222 -> 11111      ← ffmpeg 内部日志（按比例 ✔）
Debug: Frame 565: IDR Keyframe (AV_FRAME_FLAG_KEY)             ← 重配后自动 IDR ✔
```

**判定标准**：出现 `avg bitrate change:` 才算真的改了 ✔ —— 这行是 **ffmpeg 自己打的** ✗，
不受本仓库日志代码影响 ✗，无法被伪造 ✔。失败会出现 `failed to reconfigure nvenc` ✗。
