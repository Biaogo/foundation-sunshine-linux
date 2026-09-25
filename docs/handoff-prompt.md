# 交接提示词（复制整段发给新对话）

继续 Sunshine Linux 分支的收尾工作。以下是完整上下文，请先读完再动手。

## 我是谁 / 你怎么配合我
- 我是 NixOS 用户（fish shell，**绝不给我 sudo 密码**，需要就给我命令我自己跑）。
- 我做 Sunshine（Moonlight 服务端）在 Linux 上的分发与修复；主战场是 **fork：`~/Downloads/fsl-refork`**，分支 `refork/linux`，tag `v2026.09.30-linux` = `2118576a`。
- **不要动我主力服务**（当前是 `foundation-sunshine-2026.09.28`，端口 47989/47984/47990/48010）；测试一律用独立实例（见下）。
- 我的工作方式偏好：**先给证据再下结论**；不要为了让我"看着有进展"而报假状态（报"在做"就必须真的已经在跑）；**能你自己验证的就别让我连一次**；每次要我测试前，先说清"你会看哪几行日志/哪几个判据"。

## 已完成（都实测过）
**一期**：触控映射（eDP-1 上实测正常）、显示器时序、**内置虚拟屏**（建/绑触控/收，无钩子可用）、看门狗诊断。
**二期**：`dd_configuration_option` 五个模式搬进进程内 —— `disabled`/`verify_only`（不动作）、`ensure_active`、`ensure_primary`、`ensure_only_display` 全部实测通过（含 apply→会话→断开→revert 闭环）。
- 验证构建：`/nix/store/z3g6pnh2kyfri27yp4d8mfhcz5f1apcn-sunshine-refork-probe-2026.09.25`
- **该构建与 tag `2118576a` 的源码字节级一致**（已用 `git diff fba05cac..HEAD -- src/` 验证为空）。
- 关键日志判据（有这些行就是成功）：
  `Virtual display [Virtual-SunshineVirt] created at WxH@FPS` / `topology: snapshot taken: N output(s)` / `topology: disabling <uuid> (<name>) for ensure_only_display` / `topology: mode=<m> applied to <屏>` / `Touch input bound to [Virtual-SunshineVirt]` / 断开后 `topology: revert: pre-session topology restored` + `Virtual display helper stopped; output removed`。

## 铁律 / 已知坑（**最容易浪费时间的地方**）
1. **在 Sunshine 进程内，绝不要读子进程的 stdout/stderr** —— 实测三连败（detached 线程 boost 管道、`std::system()`+文件重定向、`capture_stdout(kscreen-doctor -o)`），全是空；而同一命令在 shell 里、用该进程自身环境跑**完全正常**（进程自身 SIGCHLD/回收所致）。
   ⇒ **写（只起不读）完全正常**，随便用；**要读就用 GDBus 或读文件**。触控绑定就是改用 GDBus 后一次通过的。
2. **KWin 的 D-Bus 不暴露输出**；KScreen 的 `/backend` 是瞬时启动器（应用完就消失）。输出清单读 **`~/.config/kwinoutputconfig.json`**：顶层是**方案列表**不是输出列表；描述符里名字在 **`connectorName`**（不是 `name`）、uuid 在 `uuid`；开关/优先级在另一处 `outputs[]`，用 **`outputIndex`** 对应回描述符下标；同一 uuid 出现在多个方案时保留 `enabled` 那份；该文件**含 krfb 虚拟输出**；路径要兼顾 `$XDG_CONFIG_HOME` 与 `~/.config`。
3. **改解析逻辑前，先用 Python 镜像同一套逻辑跑真实数据再编译**（本项目编译分钟级、实测要你连一次，代价高）。
4. **先验证"需求是否已被满足"再写代码**：KWin 对新建输出**自己就会**设 `enabled` + `priority 1`，虚拟屏目标的 `ensure_active`/`ensure_primary` 根本不需要动作。
5. `ensure_only_display` 的"关掉其它屏"**必须在"目标是否已知"的分支之外**，否则虚拟目标会把它悄悄退化成 `ensure_primary`（踩过）。
6. 快照必须在**任何模式动作之前**采集，否则 revert 会还原成"模式之后"的状态。
7. **我的笔记本面板 eDP-1 物理损坏、常驻关闭是基线** —— 看到它 `disabled` 属正常，别当 bug、别去"顺手启用"；`ensure_only_display` 关它是无害的。
8. 新虚拟输出出现时 **KWin 会顺手关掉其它屏**（实测两次）→ `ensure_active`/`ensure_primary` 需要"把快照里原本 enabled 的屏重新 enable"的兜底。

## 测试环境（已备好，端口 49500）
- 无钩子实例配置：`/tmp/fst-nohook/sunshine/sunshine.conf`（验证内置路径）
- 带钩子实例配置：`/tmp/fst-gate/sunshine/sunshine.conf`（验证钩子路径；两者共用 49500，一次只起一个）
- 端口族：49500(HTTP) / 49495(HTTPS) / 49501(WebUI) / 49521(RTSP) / 49509-49512(UDP)；防火墙已放行。
- 起实例（在 Hermes 里用 background 方式起，别用前台）：
  ```bash
  export XDG_CONFIG_HOME=/tmp/fst-nohook XDG_RUNTIME_DIR=/run/user/1000 \
         DBUS_SESSION_BUS_ADDRESS=unix:path=/run/user/1000/bus WAYLAND_DISPLAY=wayland-0
  exec /nix/store/z3g6pnh2kyfri27yp4d8mfhcz5f1apcn-sunshine-refork-probe-2026.09.25/bin/sunshine \
       /tmp/fst-nohook/sunshine/sunshine.conf > /tmp/fst-nohook/sunshine.log 2>&1
  ```
- 编译（harness）：
  ```bash
  cd ~/Downloads/fsl-refork && rsync -a --delete --exclude='.git' --exclude='third-party/build-deps' \
    --exclude='build' ./ /tmp/fsl-refork-src/ && cd /tmp/fsl-harness && \
    nix-build upstream-sunshine.nix -o result-x > /tmp/build.log 2>&1; grep -aE 'error: ' /tmp/build.log | head
  ```
- 会话中读布局：`kscreen-doctor -o | sed 's/\x1b\[[0-9;]*m//g'`；读触控绑定：`busctl --user get-property org.kde.KWin /org/kde/KWin/InputDevice/<eventNN> org.kde.KWin.InputDevice outputName`。
- 收尾必查四项：`topology: revert: pre-session topology restored`、`Virtual display helper stopped`、布局回到基线、无 `krfb-virtualmonitor` 残留。

## 待办（按优先级）
1. **⚠️ 单元测试：目前为 0** —— 项目 `AGENTS.md` 要求为新增/修改代码补测试（目标 100% 覆盖改动）。昨晚新增约 300 行 C++（`src/platform/linux/virtual_display.{h,cpp}`、`src/nvhttp.cpp`、`src/stream.cpp`）。解析那部分（`kscreen_outputs_from_kwin_config`）是纯函数，喂样例 JSON 就能测（注意 `connectorName` + `outputIndex` 结构）。
2. `ensure_active` 在内置版下未单独实测（与 `ensure_primary` 同路径，仅少一条 priority）。
3. 清理临时 debug 日志（`Touch mapping:` 与每轮轮询行）。
4. 两套实现收敛：钩子版 `~/Downloads/nixos/modules/home/optional/sunshine-topology.sh` 与 C++ 版现在功能对等 → 长期把钩子退化成薄封装或删掉。
5. 真 HDR（独立工程：虚拟输出宣告 BT.2020/PQ）。
6. 收尾挂死：朋友那边出现过 `Hang detected!` core dump，已加线程点名诊断，等他新日志出现 `(still waiting for the video thread)` 再对症。
7. portal 采集默认值：Linux 优先 KWin。
8. 上游 LizardByte 同步（人工分批；队列见 `docs/upstream-linux-sync.md`）。
9. 朋友侧：装官方 NVIDIA 驱动后复测 NVENC；若他要组合模式，从 `2118576a` 编新 deb（`sunshine_2026.09.30.2` 只含一期）。

## 相关文件
- fork 文档：`docs/display-topology-linux.md`（含一期 5/5 与二期最终实现+踩坑）、`docs/virtual-display-linux.md`、`docs/upstream-linux-sync.md`
- nixos 侧：`~/Downloads/nixos/modules/home/optional/sunshine-vdisplay.nix` 与 `sunshine-topology.sh`（VNAME/VPORT 已参数化）
- 技能：`sunshine-linux-hosting`（含 `references/virtual-display-cpp-notes.md`，写着上面这些铁律）
- 用户手上待办：**CI dispatch**（tag `v2026.09.30-linux`，force 不勾）→ `nix flake update` + switch + 重启 Sunshine

## 请求
先确认你理解上述铁律（尤其第 1、2、3 条），再从待办 **1（补单元测试）** 开始动手。动手前如要改解析逻辑，请先用 Python 镜像验证；要我做会话实测时，先告诉我判据。
