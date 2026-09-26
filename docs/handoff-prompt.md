# 交接提示词 · 第三批（复制整段发给新对话）

继续 Sunshine（Moonlight 服务端）Linux 分支的收尾。上一批（第二批）的待办 1/2(测试部分)/3/6 已在
`refork/linux` 上落地并验证，待办 4 已写实现但**还差一次真实会话验收**。以下是完整上下文，先读完再动手。

## 我是谁 / 你怎么配合我
- NixOS 用户，shell 是 **fish**（heredoc 会失败，给 bash -c 或 fish 语法）。**绝不给我 sudo 密码** —— 需要就给我命令我自己跑。
- 主战场 **fork：`~/Downloads/fsl-refork`**（分支 `refork/linux`，是个 **linked worktree**，真 git 目录在 `~/Downloads/foundation-sunshine-linux/.git/worktrees/fsl-refork`）；NixOS 配置仓 **`~/Downloads/nixos`**（flake，host 名 **`linux`**）；打包仓 **`~/Downloads/foundation-sunshine-linux.nix`**（CI 自动 pin）。
- **不要动我主力服务**（`systemctl --user` 的 `sunshine.service`，端口 47989/47984/47990/48010）；测试用独立实例（见下）。
- 工作方式：**先给证据再下结论**；报"在做"就必须真的已经在跑；**能你自己验证的别让我连一次**；要我做会话实测时先说清判据。
- 我这条链有"治标 vs 治本"的讲究：NixOS 侧改一行能修我自己，但**跨发行版的问题必须在代码/打包层修**。

## 现在的事实状态（2026-09-25 深夜 · 第二批之后）
- fork tag `v2026.10.01-linux` = `14a46eb4` ✓（已推、CI 已 pin）。**其后 9 个提交已全部 push 到 `origin/refork/linux`**（`git log --oneline -9` 可看全）：
  - `8876ee6d feat(linux): make the built-in virtual display self-contained`（helper 不依赖 PATH + KWin 解析纯函数化 + 拓扑稳定后复查）
  - `785f204b chore(linux): drop the temporary touch diagnostics`
  - `ea54d3f6 build(packaging): recommend the KDE virtual-display helpers`
  - `7776915a docs: handoff — third batch`
  - `95371cf7 refactor(linux): make the client display pick testable`（+7 条单测）
  - `609bfeb8 fix(linux): let the compositor answer the display enumeration first`（portal 失败不再拖死 `wait_for_display()`）
  - `013c54ee docs: correct the HDR reconnaissance`（**HDR 采集侧早已实现，别再重写**）
  - 最后一条：`feat(linux): let a second instance own its own virtual output`（`SUNSHINE_VDISPLAY_NAME` / `SUNSHINE_VDISPLAY_PORT` 覆盖恢复 → **并排验收重新可行**）
- **部署状态：生产跑的是 pin 的 `2026.10.01` build，上面这些提交一个都不在里面**（二进制里搜不到 `was not found in $PATH` / `re-enabling` / overrides 那几条串）。要在生产看到效果，得发新 tag（`vYYYY.MM.DD-linux`，**不能移动同名 tag**）让 CI pin+build+cachix，或临时把 nixos 侧 pin 指到本地 tree。
- **NixOS 侧已切换完成**：`/run/current-system` → `system-216-link`（`…-nixos-system-linux-26.05.20260924.c508844`，9/25 21:28 生成），nixos 仓**已提交且干净**（HEAD `9fddd3c refactor(sunshine): use the fork's built-in virtual display`）。HM 模块里那行
  `PATH=${pkgs.kdePackages.krfb}/bin:${pkgs.kdePackages.libkscreen}/bin:$PATH` **保留**（现在只是"最先命中"，不再是必需）。
- **虚拟屏在生产里确实回来了**：日志实测 `Virtual-SunshineVirt` → `Touch input bound` → `Streaming bitrate` → `Opus initialized`，且**有一条 22:13 起的真会话**（`CLIENT CONNECTED`）。
- 一/二期全部落地并实测；NixOS 侧 427 行钩子脚本已退休（`~/.config/sunshine/sunshine.conf.bak-hook-era-20260925` 是回滚保险）。

## 第二批交付 · 对账表
| 项 | 状态 | 证据 |
|---|---|---|
| **1 helper 不依赖 PATH + 不可用 warning** | ✅ 已实现 | `virtual_display_helper`/`kscreen_helper` 配置项 + 兜底目录；三处 warning（display-list 探测 / 会话启动 / 拓扑）；7 条单测 |
| **2 单元测试** | ⚠️ 部分 | 已补 22 条：helper 定位 7 + KWin 解析 8 + 拓扑复查 6 + 显示选择翻译 7 + 配置解析 2（最后这批随 `test(linux): cover the client display pick` 提交）；**`stream.cpp` 的收尾调用以及 `session_virtual_display_start()` 的调用分支仍未补测**（集成路径，靠会话实测） |
| **3 打包层** | ✅ 已做 | `cmake/packaging/linux.cmake` 加 `Recommends: krfb, libkf5screen-bin \| libkscreen-bin`；README 加一节；真 cpack+dpkg 验到 control 字段 |
| **4 KWin 抢跑竞态** | ⚠️ **实现完成，待你实测** | 立即 pass + `TOPOLOGY_SETTLE`(1s) 后复查（`outputs_to_reenable()` 纯函数，6 条单测）；判据见下 |
| **6 清临时 debug 日志** | ✅ | 删 `Touch mapping:`、删每轮 `Touch binding poll`、顺手清掉 GDBus 重构遗留的孤儿 doxygen |
| 7 真 HDR / 8 portal 默认 KWin / 9 上游同步 | **7 ✅ 侦查已更正** · **8 ✅** · 9 ❌ | 8：`display_names()` 改成 `nvfbc → kwin → wlroots → kms → x11 → portal`（原来 kwin 排在 portal 之后，等于让 portal 的失败拖死 `wait_for_display()`）。7：**采集侧本仓早就实现**（`is_hdr()`/`get_hdr_metadata()`/BT2020+P2084 协商都在 `pipewire.cpp`），交接词里"没写"是误判；真正只缺"把虚拟输出开成 HDR"这一步，硬门见 `docs/virtual-display-linux.md` 的 HDR 节 |

## 待办（按优先级）
1. **★ 待办 4 的实测验收（唯一需要我连一次的事）**：**现在有两种做法** ——（a）发新 tag 让生产 build 含这些提交后连一次；（b）**先不动生产**：另起一个并排实例（复制配置目录 + 移位端口 + `SUNSHINE_VDISPLAY_NAME=ProbeVirt SUNSHINE_VDISPLAY_PORT=5920`，见 `docs/virtual-display-linux.md` 的"并排验收"节）连一次即可。判据：
   1. 会话**进行中** `kscreen-doctor -o`：`eDP-1`（及其它原本 enabled 的屏）仍是 `enabled`；
   2. 日志出现 `topology: re-enabling <uuid> after the compositor settled`（KWin 没抢跑时也可能不出现——那时第一趟就够了）；
   3. 断开后布局回基线（`topology: revert: pre-session topology restored`）。
2. **单元测试补齐（部分已完成）**：`nvhttp.cpp` 里"客户端/主机配置的显示选择 → 输出名 + hook 开关"已抽成纯函数 `resolve_display_pick()`（`virtual_display.h`）并补 7 条测试；**剩下的**是 `session_virtual_display_start()` 的调用分支与 `stream.cpp` 收尾调用（属集成路径，靠会话实测覆盖）。
3. **`src/rtsp.cpp:72` 的 GCC 15 告警**（**不是本次改动引入**，属 mic lane）：`-Werror=stringop-overflow`，GCC 15 在多层 inline 后的**误报**（守卫已挡 `channel_count > sizeof(mapping)`）。两条路：循环上界补一条本地可证的 `&& i < static_cast<int>(sizeof(result.mapping))`，或构建里加 `-Wno-error=stringop-overflow`。**Ubuntu 的 GCC 13/14 CI 不受影响**，只在"NixOS/GCC 15 + BUILD_WERROR=ON"时才炸。
4. **新提交与发版**：第二批的 4 个提交连同本轮的 `test(linux): cover the client display pick` **已 push 到 `origin/refork/linux`**（未打 tag）。要发版时注意：`v*` tag 必须 `vYYYY.MM.DD-linux`、**不能移动同名 tag**（CI 按名字判新旧）。
5. 真 HDR：**采集侧无需改动**（`pipewire.cpp` 里 `is_hdr()` / `get_hdr_metadata()` / BT2020+P2084 协商早已实现，我之前说"没写"是只看了 `kwingrab.cpp` 的误判）。真正只剩一步：**让虚拟输出变成 HDR** —— 客户端要 HDR 时创建后调 `kscreen-doctor output.<uuid>.hdr.enable`，收尾恢复（`kscreen_output_t` 快照加 HDR 位）。**动之前先跑硬门**：KWin 允不允许给 krfb 的虚拟输出开 HDR（步骤与回滚见 `docs/virtual-display-linux.md` 的 HDR 节）。
6. ✅ portal 采集默认值：Linux 优先 KWin —— `display_names()` 已改成 `kwin` 优先于 `x11/portal`（见对账表最后一行的理由）。实测判据：会话里先出现 `Requested display [...] is available`，紧随 `Screencasting with KWin ScreenCast`。
7. 上游 LizardByte 同步（人工分批；队列 `docs/upstream-linux-sync.md`）。
8. **harness 的 `--argstr testFilter` 实际不生效**（drv 的 `checkPhase` 里带了 `--gtest_filter=…`，但跑的还是全量套件）——gate 参数现在是摆设，值得单独查。

## 铁律 / 已知坑（最容易浪费时间的地方）
1. **在 Sunshine 进程内，绝不要读子进程的 stdout/stderr** —— 三连败（detached 线程 boost 管道、`system()`+重定向到文件、`capture_stdout(kscreen-doctor -o)`），全是空；同一命令在 shell 里正常（进程自身 SIGCHLD/回收）。**写（只起不读）完全正常**。要读就用 GDBus 或读文件。
2. **改解析逻辑前，先用 Python 镜像同一套逻辑跑真实数据再编译**（编译分钟级、实测要用户连一次，代价高）。KWin 配置那轮就是靠镜像当场发现结构猜错，省了一整轮。
3. **先验证"需求是否已被满足"再写代码**：KWin 对新建输出自己就会 `enabled` + `priority 1`，虚拟屏目标的 `ensure_active`/`ensure_primary` 本来不需要动作。
4. **harness 编过 ≠ 官方严格度编过**：官方构建开 `BUILD_WERROR=ON`。**容器路线在本机网络里走不通**（`linux_build.sh` 的 deps 步骤必须 `git clone github.com/nvm-sh/nvm`，容器内不行；宿主上没问题）。替代做法（已用、可复用）：`/tmp/fsl-harness/strict-build.nix` 与 `strict-build-relaxed.nix`（我加的变体，**没改你的 `upstream-sunshine.nix`**）——`-DBUILD_WERROR:BOOL=TRUE -DCMAKE_BUILD_TYPE=Release`，后者额外 `-Wno-error=stringop-overflow` 绕开 rtsp 那条 GCC 15 误报。旧的单 TU 复核法：从 `compile_commands.json` 取命令行，把 `/build/fsl-refork-src` 换成 `/tmp/fsl-refork-src`、加 `-Werror`、补 `/tmp/dep_includes.txt` 里的 `-isystem`。
5. **内置实现依赖外部 helper（`krfb-virtualmonitor` + `kscreen-doctor`）** ✗→✅：第二批已在代码层修好（配置项 + 兜底目录 + 不可用 warning）。**但**：NixOS 上 `krfb-virtualmonitor` 不在系统 profile 里，`kscreen-doctor` 在（`/run/current-system/sw/bin`）——所以 HM 那行 PATH 仍建议保留。
6. `environment.etc."sunshine-vdisplay.conf"` 曾被当成配置，其实**从未被读取** ✗（服务 `ExecStart` 不带配置参数 → 真正生效的是 `~/.config/sunshine/sunshine.conf`）。已删并加注释。
7. **客户端 `-1` 常见真因**：`sunshine.conf` 里残留 `global_prep_cmd` 指向**已被删的钩子脚本** → 启动失败（日志 `Couldn't run [.../sunshine-vdisplay-do.sh]`）。切换实现时必须同时清掉用户侧那份配置。
8. `v*` tag：**CI 按 tag 名判断新旧**，且 `update-pin.py` 正则强制 `vYYYY.MM.DD-linux`（`.1` 后缀会被拒）→ **移动同名 tag 它看不见**，发新版必须用新名且格式合规。
9. 我的笔记本面板 **eDP-1 物理损坏、常驻关闭是基线** —— 看到 `disabled` 属正常，别当 bug、别顺手启用。
10. 新虚拟输出出现时 **KWin 会顺手关掉其它屏**（实测）；`ensure_active`/`ensure_primary` 需要"把快照里原本 enabled 的屏重新 enable"的兜底，且**存在 1 拍竞态**（我的 enable 可能比 KWin 早）→ 第二批已加"稳定 1s 后复查"（待办 1 的实测就是验它）。
11. **判回归别用 `--keep-failed` 留下的构建目录**：那里面 `configure_file` 复制的那批 fixture（`src/config.cpp`、`docs/configuration.md`、`Config.vue`…）**全都不在**，而测试二进制读的是编译期常量 `SUNSHINE_TEST_BIN_DIR`（指向已删除的沙箱路径）→ 大量失败甚至**假通过**（空对空断言）。要真机跑就：
    `cd <kept dir>/build && HOME=/home/biaogo ./tests/test_sunshine --gtest_filter='<你的 suite>.*'`，并且**只用自足用例**判。
12. 沙箱内跑全量套件时，**固定 15 条环境类失败**是预期的（`HOME=/homeless-shelter` 不可写、无网络、需可写 cwd），别当回归。

## 测试环境（已备好，端口 49500）
- 无钩子实例配置：`/tmp/fst-nohook/sunshine/sunshine.conf`；带钩子实例：`/tmp/fst-gate/sunshine/sunshine.conf`（两者共用 49500，一次只起一个）。
- 端口族：49500(HTTP) / 49495(HTTPS) / 49501(WebUI) / 49521(RTSP) / 49509-49512(UDP)；防火墙已放行。
- 起实例（**后台方式**，别用前台；env 要带 XDG_RUNTIME_DIR / DBUS_SESSION_BUS_ADDRESS / WAYLAND_DISPLAY）：
  ```bash
  export XDG_CONFIG_HOME=/tmp/fst-nohook XDG_RUNTIME_DIR=/run/user/1000 \
         DBUS_SESSION_BUS_ADDRESS=unix:path=/run/user/1000/bus WAYLAND_DISPLAY=wayland-0
  exec /nix/store/<build>/bin/sunshine /tmp/fst-nohook/sunshine/sunshine.conf > /tmp/fst-nohook/sunshine.log 2>&1
  ```
- 编译 + 跑测试（harness）：
  ```bash
  cd ~/Downloads/fsl-refork && rsync -a --delete --exclude='.git' --exclude='third-party/build-deps' \
    --exclude='build' --exclude='artifacts' ./ /tmp/fsl-refork-src/ && cd /tmp/fsl-harness && \
    nix-build upstream-sunshine.nix --arg buildTests true --keep-failed -o /tmp/result-x > /tmp/build.log 2>&1
  grep -aE 'PASSED  \]|FAILED  \] [0-9]+ tests' /tmp/build.log | tail -2
  ```
  ⚠️ **`/tmp/fsl-refork-src`（约 1.8G）千万别删** —— 里面是构建所需的 `third-party/build-deps`，删了要重头准备。
- 会话中读布局：`kscreen-doctor -o | sed 's/\x1b\[[0-9;]*m//g'`；读触控绑定：`busctl --user get-property org.kde.KWin /org/kde/KWin/InputDevice/<eventNN> org.kde.KWin.InputDevice outputName`。
- 收尾必查：`topology: revert: pre-session topology restored`、`Virtual display helper stopped`、布局回到基线、无 `krfb-virtualmonitor` 残留。

## 第二批的验证证据（已跑过，别重复烧时间）
- 沙箱全量套件：**626 passed / 15 failed**（15 条全是环境类，见铁律 12）。
- 真机过滤跑（`HOME` 可写）：`VirtualDisplayHelperLookup` + `KwinOutputConfigParsing` + `TopologyReenable` + `DisplayPickResolution` + `LinuxHelperPathConfigTest` + `LocaleConsistencyTest` = **40/40 PASS**。
- 官方严格度：`strict-build-relaxed.nix` **EXIT=0**（全项目 `-Werror` + Release，仅降级 rtsp 那条）；`strict-build.nix`（不降级）在 `src/rtsp.cpp:72` 停住，**那就是待办 3 的来源**。
- 打包：真 cpack 出 `.deb`，`dpkg-deb -I` 里 `Recommends: krfb, libkf5screen-bin | libkscreen-bin`。
- 改动过的每个 TU 都单独用官方 flag + `-Werror` 编过：CLEAN。

## 关键事实速查
- **helper 定位顺序**（`platf::find_helper()`，`src/platform/linux/virtual_display.cpp`）：配置项绝对路径 → `$PATH` → `/usr/bin` → `/usr/local/bin` → `/run/current-system/sw/bin` → 可执行文件自身目录及其旁 `bin/`。每一步都要求可执行（`access(X_OK)`）；失败**每 helper 每进程只报一次**。
- **可测的纯函数**（都在 `virtual_display.h` 里声明）：`kscreen_outputs_from_json_text()`（KWin 配置 → 输出列表）、`find_helper()`、`helper_fallback_dirs()`、`outputs_to_reenable()`（稳定后复查该 enable 谁）。
- **KWin 配置** `~/.config/kwinoutputconfig.json`：顶层是**方案列表**；描述符里名字在 **`connectorName`**（不是 `name`）、uuid 在 `uuid`；开关/优先级在另一处 `outputs[]`，靠 `outputIndex` 对应回下标；同一 uuid 出现在多方案时保留 `enabled` 那份；路径要兼顾 `$XDG_CONFIG_HOME` 与 `~/.config`。
- **helper 的发行版包名**（Ubuntu 24.04 实包核过）：`krfb` → `krfb-virtualmonitor`；`kscreen-doctor` 在 **`libkf5screen-bin`**（Plasma 5/KF5；`kscreen` 包只有 `kscreen-console`）或 `libkscreen-bin`（Plasma 6/KF6）；NixOS 是 `libkscreen`。
- KWin 的 D-Bus **不暴露输出**；KScreen 的 `/backend` 是**瞬时启动器**；触控绑定用 **GDBus**（`org.kde.KWin` 的 `/org/kde/KWin/InputDevice`）✓ 已验证。
- 我惯用的重建方式：`nh os switch .`（flake 路径必须显式给，`--flake .#linux` 亦可）；`nixos-rebuild` 不带 `--flake` 会因 `nixos-config` 缺失而失败 ✗。
- 相关文档：`docs/display-topology-linux.md`（二期实测 + 新的稳定后复查）、`docs/virtual-display-linux.md`（helper 定位与依赖声明）、技能 `sunshine-linux-hosting`（含 `references/virtual-display-cpp-notes.md`）。

## 请求
先确认你理解铁律（尤其 **1、2、4、5**，外加 11/12 这两条"别误判回归"的），然后从**待办 1（待办 4 的真实会话验收）**开始：先把判据跟我说清楚，我连一次给你结果；在你我都不确定要不要动 `src/rtsp.cpp` 之前，**别改它**（那是 mic lane 的生产代码）。
