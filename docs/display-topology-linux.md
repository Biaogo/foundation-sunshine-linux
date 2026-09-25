# Display topology modes on Linux — design

Goal: make the `dd_*` display-preparation modes (the "屏幕组合功能" list) actually work on this
Linux host, where upstream only implements the Windows backend and our base's
`display_device::configure_display()` is a stub.

Anchors: fork `src/display_device/session.cpp` (its device-prep state machine), upstream
`src/display_device.h` (`device_prep_e`), this host's krfb virtual-monitor hook
(`~/.local/bin/sunshine-vdisplay-*.sh`), and the config keys already present in this tree
(`dd_configuration_option`, `dd_resolution_option`, `dd_hdr_option`, `dd_config_revert_delay`,
`dd_config_revert_on_disconnect`, `dd_configuration_option`, `output_name`).

## 1. The modes

| UI wording (fork) | upstream `device_prep_e` | what it must do on Linux |
|---|---|---|
| 跟随主机 / no override | `verify_only` (or disabled) | change nothing; current behaviour (the hook still creates/enables the virtual output) |
| 无操作模式 | `no_operation` | keep the present layout; do not enable/disable/move anything |
| 激活模式 | `ensure_active` | enable the target display if it is disabled; leave the rest as-is |
| 主屏串流模式 | `ensure_primary` | enable the target and make it the primary output (in Plasma: `priority.1`) |
| 副屏串流模式 | `ensure_secondary` | the physical output stays primary; the target becomes an extra output to its right (`position` + `priority.2`) |
| 仅启用指定显示器模式 | `ensure_only_display` | only the target output enabled; every other output disabled |

The "target display" is whatever the session selected: the hook-managed virtual output
(`Virtual-SunshineVirt`) for the default/虚拟-KWin/虚拟-KMS picks, or a physical output for an
explicit pick. Physical picks must stay untouched by the topology layer unless the selected mode
says otherwise (that is what the current hook contract already guarantees).

## 2. How it maps onto this host

Plasma exposes everything needed through kscreen (`kscreen-doctor` is a CLI over the same API):

| Need | primitive |
|---|---|
| enable / disable an output | `output.<id>.enable` / `output.<id>.disable` |
| set the mode | `output.<id>.mode.<W>x<H>@<fps>` |
| set the primary | `output.<id>.priority.1` (higher number = lower priority) |
| position (secondary) | `output.<id>.position.<X>,<Y>` |
| scale | `output.<id>.scale.<F>` |
| snapshot for revert | `kscreen-doctor -j` (JSON) captured before the first change |

Lifecycle: **the topology change must happen after the virtual output exists**, i.e. inside the
existing do-hook, after `krfb-virtualmonitor` started and `kscreen-doctor output.<uuid>.enable`
succeeded — and it must be re-applied if KWin re-arranges outputs when a monitor is (dis)connected
(the fork has `session_listener.*` for exactly that; on this host a single re-apply after the
output appears is enough, see §4).

## 3. Phased implementation

**Phase 1 — hook-driven (smallest, covers the user-visible feature).**
Extend the existing do/undo hooks:
- the do-hook reads the display-preparation options from its environment and applies the matching
  kscreen actions after enabling the virtual output;
- the undo-hook restores the saved snapshot (`kscreen-doctor -j` taken at the start), honouring
  `dd_config_revert_delay` / `dd_config_revert_on_disconnect`.

**Done (2026-09-25, fork `refork/linux`):** the env plumbing. `proc_t::update_session_env()` now
exports, per session, alongside the existing display switch:

| variable | values |
|---|---|
| `SUNSHINE_CLIENT_DD_CONFIG` | `disabled` \| `verify_only` \| `ensure_active` \| `ensure_primary` \| `ensure_only_display` |
| `SUNSHINE_CLIENT_DD_RESOLUTION` | `disabled` \| `automatic` \| `manual` |
| `SUNSHINE_CLIENT_DD_REFRESH_RATE` | `disabled` \| `automatic` \| `manual` |
| `SUNSHINE_CLIENT_DD_HDR` | `disabled` \| `automatic` |

They are erased before being re-set (same anti-leak discipline as `SUNSHINE_CLIENT_DISPLAY_NAME`),
and the C++ enum → string mapping is one switch per enum in `process.cpp`. Remaining work: the hook
actions themselves. Note the UI's six wordings collapse onto five enum values plus the host's
`output_name` default, and that 副屏串流 has no upstream enum of its own — it is `ensure_active` with
a position/priority adjustment, so the hook must distinguish it from the wording, not the enum.

Pros: no C++ surface beyond the export, easy to iterate, matches the deployed architecture. Cons:
topology logic lives in a shell script, and it only applies to sessions that run the hook.

**Phase 2 — C++ `display_device` for Linux.** Implement `display_device::configure_display()` (Linux
branch) to perform the same actions through the kscreen DBus interface, with the snapshot/revert
state machine owned by the process (upstream's `display_device/session.cpp` shape). Benefits:
applies on every launch path, reports failures into the log/HTTP response, survives hook changes.

Phase 1 is what I would ship first; Phase 2 only if the script becomes the bottleneck.

## 4. Pitfalls that this design must respect (all observed on this host)

1. **The virtual output does not exist until the hook creates it** — a topology change applied
   before `krfb-virtualmonitor` + `kscreen-doctor enable` will silently target nothing.
2. **KWin re-lays-out outputs on hotplug**; `krfb` appearing/disappearing triggers that. Applying the
   mode once, right after the enable, is enough today; a re-apply guard (compare `kscreen-doctor -j`
   against the desired state, retry a couple of times) makes it robust.
3. **eDP-1 is the login head** (forced-on dead panel with a fixed EDID). `ensure_only_display` on the
   virtual output means eDP-1 goes dark during the session — that is the intended semantics, but the
   session must not be *started* from a state where eDP-1's disable would take the compositor's only
   scanout away before the virtual output is enabled (order: enable target first, then disable
   others).
4. **Restore must be unconditional.** A dropped client, a crashed hook or a Sunshine restart must all
   leave the host in the pre-session layout; therefore the snapshot must be written to disk
   (`$XDG_RUNTIME_DIR`) and the undo-hook must be idempotent.
5. **The virtual output's scale is 1.5** (logical 1584×720 vs physical 2376×1080). Topology code that
   computes positions/sizes must use logical geometry (that is also the open coordinate bug for
   absolute input, tracked separately).
6. **`dd_config_revert_delay`** semantics: upstream waits before reverting so the client can
   reconnect into the same layout; the hook must honour it (a `sleep` in the undo path is fine).

## 5. Decisions (operator, 2026-09-25) — the questions this design asked

1. 副屏串流模式's "primary" side: **eDP-1 as the physical primary** (it is the host's only other
   output; it is an invisible forced-on head, so the mode is "virtual output extends to the right"),
   with the caveat that with no real monitor attached the mode is mostly a topology rehearsal.
2. 主屏串流模式: **priority only** — the virtual output already sits at the origin, no repositioning.
3. The modes **apply to the hook-managed virtual output only**; a physical pick never has its
   topology rewritten by this layer.
4. **Phase 1 (hook) first**; Phase 2 (C++) only if the script becomes the bottleneck.

These were answered as "默认" (accept the proposed defaults), so implementation may proceed on them —
re-confirm only if a mode's behaviour disagrees with what the operator actually wants.

## 6. Interaction with the absolute-input (touch/pen) binding

Every topology change re-creates or re-parents KWin's input devices, and KWin leaves `outputName`
**empty** on libvirtualhid's absolute-input devices (see `sunshine-touchbind.sh`): unbound devices get
their events dropped, which shows up as "touch does nothing / sticks to a corner". Therefore:

- the topology layer must **re-run the touch binding after it changes the layout** (same
  `busctl --user set-property … outputName s <output>` call, re-checking that the devices are still
  bound), and
- `ensure_only_display`/`ensure_primary` — the modes that change which output is active — are exactly
  the cases where KWin may drop the binding, so the binding must be re-verified *after* them, not
  before. Binding once at session start (today's hook) is sufficient only while the topology stays
  untouched.

## 二期：把组合逻辑搬进 C++（方案已定，2026-09-25 晚）

一期（钩子 `sunshine-topology.sh` + kscreen）已在本机对全部 5 个 `config_option_e` 值端到端验证通过：

| 模式 | 钩子动作 | 实测 |
|---|---|---|
| `disabled` / `verify_only` | 不动作 | ✓ |
| `ensure_active` | `enable` 目标 | ✓ |
| `ensure_primary` | `enable` + `priority 1` | ✓ |
| `ensure_only_display` | `enable` 目标 + `disable` 其它 | ✓ |

（`ensure_active` / `ensure_primary` / `ensure_only_display` 三轮均跑完整闭环：apply → 会话 → 断开 → `revert: pre-session topology restored`。）

二期就是把这套逻辑搬进进程内，让没有钩子的安装（.deb / AppImage）也能用。

### 落点
- 入口：`src/nvhttp.cpp` 的会话启动路径，**紧挨内置虚拟屏那次调用**（同一处 `if (…virtual_display…)` 分支），
  启动后调用 `session_apply_topology(<目标屏名>, config::video.dd.configuration_option)`。
- 释放：`src/stream.cpp` 最后一个会话结束处（与 `session_virtual_display_stop()` 同一位置）调用 `session_revert_topology()`。
- 实现：`src/platform/linux/virtual_display.cpp`（已有 `capture_stdout` / `kwin_call` 两个可用原语）。

### 实现要点（含今晚踩过的坑）
1. **不要用 detached 线程 + 自建子进程读输出**：会话进行中它会被进程自己的 SIGCHLD 处理抢走回收，
   `system()` 返回非零、自己的管道读到空——同一命令在 shell 里完全正常。进程内能用的两条路是：
   `capture_stdout()`（会话**启动阶段**已实测可用，与 `kscreen-output` 同源）和 `kwin_call()`（GDBus）。
2. `disabled` / `verify_only`：直接返回，日志写 `topology: mode=<m> — leaving the topology untouched`。
3. `ensure_active`：`enable(目标)`。
4. `ensure_primary`：`enable(目标)` + `priority(目标, 1)`。
5. `ensure_only_display`：`enable(目标)` + 对其余**当前 enabled** 的输出逐个 `disable`。
6. 快照/还原：会话开始前记录 `{uuid: {enabled, priority, position}}` 到进程内状态，
   结束时按快照还原（对应钩子的 `revert: pre-session topology restored`）。
7. 日志文案与钩子保持一致（`topology: mode=… applied to <屏>`），便于与一期结果逐行对照。

### 不变量
- **钩子存在时完全不执行**（纯增量；回退方式 = 删掉这两处调用）。
- 只动 `enabled` / `priority`，**不改分辨率**（一期约定）。
- **eDP-1 的 `disabled` 是用户基线**（该机笔记本面板物理损坏、常驻关闭）：还原必须回到该基线，
  任何模式下都不得"顺手启用"它。
- 不做 `SUNSHINE_CLIENT_*` 环境依赖（那是钩子的接口）——C++ 侧直接读 `config::video.dd`。


### 二期实测结论（2026-09-25 晚，补记）：必须改用 libkscreen，不要读子进程输出

二期第一版（进程内 `kscreen-doctor` + 解析 stdout）在**无钩子**实例上实测失败，日志：

```
Virtual display [Virtual-SunshineVirt] created at 3168x1440@90
Warning: topology: output [Virtual-SunshineVirt] is not in kscreen; not applying ensure_primary   <- 等了 3.4s 仍空
```

同一个进程里，"自己起子进程并**读它的输出**"这条路**三次全败**：

| # | 做法 | 结果 |
|---|---|---|
| 1 | detached 线程里轮询 `busctl`（boost 管道） | 每次空 |
| 2 | 启动路径 `std::system()` + 重定向到文件 | 0 字节（连文件都空，不只是退出码问题） |
| 3 | 启动路径 `capture_stdout(kscreen-doctor -o)` | 空 → UUID 拿不到 → 模式没应用 |

对照组：**同一命令在我 shell 里、以及用该进程自身环境跑，均正常**（26 个设备 / 完整输出）；
而同进程里的 **GDBus 调用（`kwin_call`）完全正常** —— 触控绑定就是它，四个设备全部绑上。

⇒ 结论：**根因在"读子进程输出"这件事本身**（本进程的 SIGCHLD/回收行为），与命令、环境、权限无关。
**不要再试图读任何子进程的 stdout/stderr**（钩子能工作是因为它只起不读）。

**输出枚举的正确做法：链 libkscreen（KF6 KScreen），用它的 API 在同一进程内完成**：

- KWin 自己的 D-Bus **不暴露输出**（已实测：`busctl --user tree org.kde.KWin` 里没有任何输出对象）。
- KScreen 的 `/backend` 由一个**瞬时启动器**（`kscreen_backend_launcher`）持有，
  只在应用配置期间存在于总线上（实测：应用完就消失）→ 不适合按名字长期调用。
- 而 `kscreen-doctor` 本身就是 libkscreen 的前端（本机路径 `/nix/store/jfzxihw…-libkscreen-6.6.6/bin/`）→
  **直接链接 libkscreen**，用 `KScreen::Config` 做：读输出列表 / `setOutputEnabled` / 优先级 / `apply`，
  同时把现有 `run_kscreen()` 的子进程调用一并删掉（写操作也走同一 API）。

附带待办（本轮实测新发现）：
- `ensure_primary` / `ensure_active` 下，**KWin 会在新虚拟输出出现时把 eDP-1 关掉**（实测两次）。
  语义上这两个模式**只该改优先级/开关目标屏**，不该让邻屏消失 → libkscreen 版要显式保持其它屏的
  enabled 状态，快照还原也必须把 eDP-1 的 `disabled` 基线还原回去。
