# 交接提示词 · 第二批（复制整段发给新对话）

继续 Sunshine（Moonlight 服务端）Linux 分支的收尾。以下是完整上下文，先读完再动手。

## 我是谁 / 你怎么配合我
- NixOS 用户，shell 是 **fish**（heredoc 会失败，给 bash -c 或 fish 语法）。**绝不给我 sudo 密码** —— 需要就给我命令我自己跑。
- 主战场 **fork：`~/Downloads/fsl-refork`**（分支 `refork/linux`）；NixOS 配置仓 **`~/Downloads/nixos`**（flake，host 名 **`linux`**）；打包仓 **`~/Downloads/foundation-sunshine-linux.nix`**（CI 自动 pin）。
- **不要动我主力服务**（`systemctl --user` 的 `sunshine.service`，端口 47989/47984/47990/48010）；测试用独立实例（见下）。
- 工作方式：**先给证据再下结论**；报"在做"就必须真的已经在跑；**能你自己验证的别让我连一次**；要我做会话实测时先说清判据。
- 我这条链有"治标 vs 治本"的讲究：NixOS 侧改一行能修我自己，但**跨发行版的问题必须在代码/打包层修**。

## 现在的事实状态（2026-09-25 深夜）
- fork tag `v2026.10.01-linux` = `14a46eb4` ✓（已推、CI 已 pin：`d8f1770 pin(sunshine): v2026.10.01`）。其后还有若干未打 tag 的修复提交（构建脚本、`-Werror=unused-result`、文档）。
- **主力服务已在跑 `…-foundation-sunshine-upstream-2026.10.01`** ✓，内置路径**生产实测通过** ✓（绑触控 ✓ 组合模式 ✓ kwingrab 采集 ✓ hevc_nvenc ✓ Opus ✓ 客户端 CLIENT CONNECTED ✓）。
- **一/二期已全部落地并实测**：内置虚拟屏（建/绑触控/收）、物理+虚拟会话的触控绑定、5 个 `dd_configuration_option` 模式、会话前清残留。**NixOS 侧 427 行钩子脚本已退休**（`sunshine-vdisplay.nix` + `sunshine-topology.sh` 已删；`global_prep_cmd` 已去；用户 `~/.config/sunshine/sunshine.conf` 里的钩子条目也清了）。
- **nixos 仓有未提交改动**，等我 `nh os switch .`：`flake.lock`（`nix flake update foundation-sunshine-linux` 的结果）、`modules/home/optional/sunshine.nix`（删 import + **加 PATH 行**）、`modules/nixos/optional/sunshine.nix`（删死配置 + 注释）。
- **刚发现并已修的坑（最重要，见铁律 5）**：内置虚拟屏靠 PATH 找 `krfb-virtualmonitor` / `kscreen-doctor`，而服务 PATH 极简 → 虚拟屏**静默消失**。已在 HM 模块加：
  `"PATH=${pkgs.kdePackages.krfb}/bin:${pkgs.kdePackages.libkscreen}/bin:$PATH"`（已验证两个包含相应可执行文件）。**但这是治标**，代码层还没修。

## 原始三期计划 · 对账表（我最早定的 A/B/C/D + 真 HDR）
| 原计划项 | 状态 |
|---|---|
| **触控映射**（eDP-1 上位置不对） | ✅ 已修并实测（纯缩放映射 ✓；黑边是死区、夹到边缘 ✓） |
| **虚拟屏搬进 C++**（B：内置建/绑/收，免钩子） | ✅ 已完成并**生产实测** ✓ |
| **屏幕组合**（C：`dd_configuration_option` 五模式） | ✅ 钩子版 5/5 + **进程内版全部实测** ✓ |
| **真 HDR**（虚拟输出宣告 BT.2020/PQ；现在虚拟屏强制 SDR 8bit 不泛白） | ❌ **未做**（独立工程，见待办） |
| 麦克风（mic 不通 → 防火墙 base+12） | ✅ 已修（UDP 48001） |
| deb 分发 | ✅ 已产出并逐项验证（CPU 版即含 NVENC） |
| AppImage 分发 | ❌ 按用户要求**从本提示词移除**，不再跟踪 |
| 上游 LizardByte 同步 | ⏳ 人工分批（队列见 `docs/upstream-linux-sync.md`） |

## 铁律 / 已知坑（最容易浪费时间的地方）
1. **在 Sunshine 进程内，绝不要读子进程的 stdout/stderr** —— 三连败（detached 线程 boost 管道、`system()`+重定向到文件、`capture_stdout(kscreen-doctor -o)`），全是空；同一命令在 shell 里正常（进程自身 SIGCHLD/回收）。**写（只起不读）完全正常**。要读就用 GDBus 或读文件。
2. **改解析逻辑前，先用 Python 镜像同一套逻辑跑真实数据再编译**（编译分钟级、实测要用户连一次，代价高）。KWin 配置那轮就是靠镜像当场发现结构猜错，省了一整轮。
3. **先验证"需求是否已被满足"再写代码**：KWin 对新建输出自己就会 `enabled` + `priority 1`，虚拟屏目标的 `ensure_active`/`ensure_primary` 本来不需要动作。
4. **harness 编过 ≠ 官方脚本编过** ✗：官方构建（fork 的 `scripts/linux_build.sh`，可在 Ubuntu 容器里跑；CI 同理）开 `-Werror=unused-result` 等更严的开关，任何忽略返回值都会**在容器里才炸**（今天 `std::system()` 就是这么挂的）。改完关键代码要用官方脚本编一次。
5. **内置实现依赖外部 helper 在 PATH 上（krfb-virtualmonitor + kscreen-doctor）且失败无日志** ✗ —— 这是我今天踩的最大坑：删钩子后虚拟屏静默消失（钩子当年自己 `export PATH`）。**代码层还没修**（见待办 1）。
6. `environment.etc."sunshine-vdisplay.conf"` 曾被当成配置，其实**从未被读取** ✗（服务 `ExecStart` 不带配置参数 → 真正生效的是 `~/.config/sunshine/sunshine.conf`）。已删并加注释。
7. **客户端 `-1` 常见真因**：`sunshine.conf` 里残留 `global_prep_cmd` 指向**已被删的钩子脚本** → 启动失败（日志 `Couldn't run [.../sunshine-vdisplay-do.sh]`）。切换实现时必须同时清掉用户侧那份配置。
8. `v*` tag：**CI 按 tag 名判断新旧**，且 `update-pin.py` 正则强制 `vYYYY.MM.DD-linux`（`.1` 后缀会被拒）→ **移动同名 tag 它看不见**，发新版必须用新名且格式合规。
9. 我的笔记本面板 **eDP-1 物理损坏、常驻关闭是基线** —— 看到 `disabled` 属正常，别当 bug、别顺手启用。
10. 新虚拟输出出现时 **KWin 会顺手关掉其它屏**（实测）；`ensure_active`/`ensure_primary` 需要"把快照里原本 enabled 的屏重新 enable"的兜底，但**存在 1 拍的竞态**（我的 enable 比 KWin 早）→ 待办里有"延迟复查"。

## 待办（按优先级）
1. **★ 代码层：helper 不该依赖 PATH**（今晚的关键教训）
   - 加配置项（如 `virtual_display_helper` / `kscreen_helper`，可在 `sunshine.conf` 指定绝对路径）
   - 找不到时按标准位置兜底：`/usr/bin`、`/usr/local/bin`、`/run/current-system/sw/bin`、应用自身目录旁（各类打包可自带）
   - **不可用时必须 `warning`**（现在完全静默 ✗）
   - 改完用官方 `scripts/linux_build.sh`（在 Ubuntu 容器里跑）编一次验证（铁律 4）
2. **★ 单元测试：目前为 0** —— 项目 `AGENTS.md` 硬要求（新增/修改代码要补测试）。新增约 300 行 C++（`src/platform/linux/virtual_display.{h,cpp}`、`src/nvhttp.cpp`、`src/stream.cpp`）；`kscreen_outputs_from_kwin_config` 是纯函数，喂样例 JSON 就能测（注意结构见铁律 11）。
3. **打包层**：deb 加 `Recommends: krfb, libkscreen`；fork README 补一节"虚拟屏依赖 krfb + libkscreen（KDE/KWin）"，并说明 helper 路径怎么配。
4. **KWin 抢跑竞态**：`ensure_active`/`ensure_primary` 之后加"启动线程上有界等 ~1s 再 enable 一次快照里原本 enabled 的屏"。
6. 清理临时 debug 日志（`Touch mapping:`、每轮 `Touch binding poll`）。
7. 真 HDR（独立工程：虚拟输出宣告 BT.2020/PQ）。
8. portal 采集默认值：Linux 优先 KWin。
9. 上游 LizardByte 同步（人工分批；队列 `docs/upstream-linux-sync.md`）。
10. 两套实现收敛：钩子版已从 nixos 删除 ✓（但**保留一版做回滚保险**——用户自己留了 `~/.config/sunshine/sunshine.conf.bak-hook-era-20260925`）。

## 测试环境（已备好，端口 49500）
- 无钩子实例配置：`/tmp/fst-nohook/sunshine/sunshine.conf`；带钩子实例：`/tmp/fst-gate/sunshine/sunshine.conf`（两者共用 49500，一次只起一个）。
- 端口族：49500(HTTP) / 49495(HTTPS) / 49501(WebUI) / 49521(RTSP) / 49509-49512(UDP)；防火墙已放行。
- 起实例（**后台方式**，别用前台；env 要带 XDG_RUNTIME_DIR / DBUS_SESSION_BUS_ADDRESS / WAYLAND_DISPLAY）：
  ```bash
  export XDG_CONFIG_HOME=/tmp/fst-nohook XDG_RUNTIME_DIR=/run/user/1000 \
         DBUS_SESSION_BUS_ADDRESS=unix:path=/run/user/1000/bus WAYLAND_DISPLAY=wayland-0
  exec /nix/store/<build>/bin/sunshine /tmp/fst-nohook/sunshine/sunshine.conf > /tmp/fst-nohook/sunshine.log 2>&1
  ```
- 编译（harness，快）：
  ```bash
  cd ~/Downloads/fsl-refork && rsync -a --delete --exclude='.git' --exclude='third-party/build-deps' \
    --exclude='build' ./ /tmp/fsl-refork-src/ && cd /tmp/fsl-harness && \
    nix-build upstream-sunshine.nix -o result-x > /tmp/build.log 2>&1; grep -aE 'error: ' /tmp/build.log | head
  ```
  ⚠️ **`/tmp/fsl-refork-src`（约 1.8G）千万别删** —— 里面是构建所需的 `third-party/build-deps`，删了要重头准备。
- 会话中读布局：`kscreen-doctor -o | sed 's/\x1b\[[0-9;]*m//g'`；读触控绑定：`busctl --user get-property org.kde.KWin /org/kde/KWin/InputDevice/<eventNN> org.kde.KWin.InputDevice outputName`。
- 收尾必查：`topology: revert: pre-session topology restored`、`Virtual display helper stopped`、布局回到基线、无 `krfb-virtualmonitor` 残留。
- 我的自建工具：`/tmp/nixos-sunshine-builtin-migration.sh`（NixOS 侧钩子迁移脚本，已用过 ✓）。

## 关键事实速查
- **KWin 配置** `~/.config/kwinoutputconfig.json`：顶层是**方案列表**；描述符里名字在 **`connectorName`**（不是 `name`）、uuid 在 `uuid`；开关/优先级在另一处 `outputs[]`，靠 `outputIndex` 对应回下标；同一 uuid 出现在多方案时保留 `enabled` 那份；**含 krfb 虚拟输出**；路径要兼顾 `$XDG_CONFIG_HOME` 与 `~/.config`。
- KWin 的 D-Bus **不暴露输出**；KScreen 的 `/backend` 是**瞬时启动器**；触控绑定用 **GDBus**（`org.kde.KWin` 的 `/org/kde/KWin/InputDevice`）✓ 已验证。
- 我惯用的重建方式：`nh os switch .`（flake 路径必须显式给，`--flake .#linux` 亦可）；`nixos-rebuild` 不带 `--flake` 会因 `nixos-config` 缺失而失败 ✗。
- 相关文档：fork 的 `docs/display-topology-linux.md`（二期实测结论与最终实现）、`docs/virtual-display-linux.md`、`docs/handoff-prompt.md`（上一批交接）；技能 `sunshine-linux-hosting`（含 `references/virtual-display-cpp-notes.md`）。

## 请求
先确认你理解铁律（尤其 1、2、4、5），然后从**待办 1（代码层：helper 不依赖 PATH + 明确的不可用 warning）**开始。若我这边还有未完成的 `switch`/验证（虚拟屏是否回来），先跟我确认状态再动手。
