# 虚拟显示器 / 屏幕组合：从主机脚本搬进 Sunshine 本体（Linux）

目标：**客户端选“虚拟-KWin / 虚拟-KMS”就能直接用**，不需要用户在主机上装脚本、
改 `sunshine.conf`。今天（2026-09-25）这些能力全部由外部钩子提供
（`~/.local/bin/sunshine-vdisplay-do.sh` + `krfb-virtualmonitor` + `kscreen-doctor`），
所以任何“打包好的安装版”都不成立 —— 打包无法给用户预置 `prep-cmd`，
Sunshine 也只读用户目录下的配置。

## 现状（为什么现在不行）

| 位置 | 情况 |
|---|---|
| `src/platform/linux/virtual_display.h` | 只有客户端可见的 id 名字；`OFFER_VIRTUAL_DISPLAY_IDS = true` 是**编译期**常量 |
| `src/platform/linux/` 其余文件 | 无任何创建/销毁虚拟输出的实现 |
| 主机侧钩子 | 唯一的实现路径：`krfb-virtualmonitor` 起输出 + `kscreen-doctor` enable/改模式 + `busctl` 绑触控 |
| 后果 | 没配钩子的主机：客户端看得到选项，选了必然 `Requested display [...] did not appear within 20000 ms`，客户端 ~10 s 放弃 |

## 设计（分三步，各自独立可上线）

### 第 1 步：运行期决定“是否 advertise”（小、立刻见效）

把 `OFFER_VIRTUAL_DISPLAY_IDS` 从编译期常量改成运行期判断：

```
advertise = <能创建> && <会话是 Plasma/Wayland>
能创建 = krfb-virtualmonitor 可执行（PATH 或配置指定）&& kscreen-doctor 可执行
```

实现要点：`display_names()` 里那次 `if (OFFER_VIRTUAL_DISPLAY_IDS)` 改为调用一个
`platf::virtual_display_available()`（新增，Linux 实现，其它平台恒 true）。
好处：菜单不再骗人；朋友那种“选了永远超时”的场景从根上消失。
失败时也不要傻等 20 s —— 目标屏不在枚举里就快速返回一个明确错误。

补充（2026-09-25 第二批）：**枚举顺序里 KWin 也必须排在 portal 前面**。`display_names()` 是
`wait_for_display()` 判断"客户端要的输出出现了没"的唯一依据，而它原来是 `x11 → portal → kwin`：
本机（service 里没有 `DISPLAY`，X11 源不成立）实际返回的是 **portal 的列表** —— portal 恰好能枚举到
krfb 的虚拟输出，所以平时看不出问题；但它自己会失败（没 token、pre-login SDDM 没有会话），一失败就
等于"即使 KWin 能供屏，也判定输出没出现" → 客户端 `Requested display [...] did not appear within 20000 ms`。
现在顺序是 `nvfbc → kwin → wlroots → kms → x11 → portal`，与 `display()` 的派发顺序、客户端列表
（`client_display_names()`）一致：**只要 KWin 源活着，就由它回答**。

### 第 2 步：虚拟输出的生命周期放进 Linux 后端

新增 `src/platform/linux/virtual_display.cpp`（.h 已有），实现：

1. **创建**：`krfb-virtualmonitor --resolution <client WxH> --name SunshineVirt --port <port> --password <随机>`
   - 作为 Sunshine 的**子进程**持有（`boost::process` 已有依赖），会话结束即 `terminate()`；
   - 端口从 `5910` 起探测空闲值；密码每次会话随机，仅本机使用（VNC 仅绑 127.0.0.1）。
2. **等待 + 接管**：轮询 KWin 枚举（复用现有 `client_display_names()`）等到
   `Virtual-SunshineVirt` 出现，然后用 `kscreen-doctor`（发行版包 `kscreen`）执行
   `output.<uuid>.enable` + 自定义模式/刷新率。
3. **触控绑定**（替代 `sunshine-touchbind.sh`）：用 QtDBus（Sunshine 已链接 Qt）
   监听 `org.kde.KWin.InputDeviceManager.devicesSysNames`，把每个 `libvirtualhid`
   绝对输入设备的 `outputName` 设成本会话的输出名 —— 设备在客户端连上后才出现，
   所以需要一段有上限的等待（现有脚本是 ~2 min）。
4. **销毁**：杀掉子进程 → 输出随 krfb 一起消失 → 用 create 时保存的快照
   （`kscreen-doctor` 读到的布局）恢复其它输出。

依赖声明：deb 用 **Recommends**（不是 Depends，缺了只是没有虚拟屏，不该拦安装）声明
`krfb, libkf5screen-bin | libkscreen-bin`，见 `cmake/packaging/linux.cmake`。
2026-09-25 对着 Ubuntu 24.04 的包实测过：`krfb` 提供 `krfb-virtualmonitor`；`kscreen-doctor` 在
Plasma 5/KF5 的 **`libkf5screen-bin`** 里（`kscreen` 包只有 `kscreen-console`，别写错），Plasma 6/KF6
是 `libkscreen-bin`，NixOS 是 `libkscreen`。非 Plasma（GNOME 等）不满足 → 第 1 步就不 advertise。

#### helper 定位：不依赖服务的 `PATH`（2026-09-25 实测坑，已修）

内置实现要起两个外部命令（`krfb-virtualmonitor`、`kscreen-doctor`），原先只做一次
`boost::process::search_path`（即 `$PATH`）。systemd --user 服务的 `PATH` 极简，NixOS 上 `/usr/bin`
根本不存在 → 两个 helper 都找不到，虚拟屏**静默消失**：客户端列表里没有虚拟 id，日志一行都没有
（钩子时代脚本自己 `export PATH`，所以删掉钩子才第一次暴露）。

现在的定位顺序（`platf::find_helper()`，实现见 `src/platform/linux/virtual_display.cpp`）：

1. `sunshine.conf` 显式配置的绝对路径：`virtual_display_helper` / `kscreen_helper`；
2. 进程 `$PATH`（老行为；NixOS 模块正是靠它注入 store 路径）；
3. 标准位置兜底：`/usr/bin`、`/usr/local/bin`、`/run/current-system/sw/bin`，最后是
   **可执行文件自身目录**与它旁边的 `bin/`（便携式打包自带 helper 的情况）。

每一步都要求候选文件可执行（`access(X_OK)`）；配置了但不可执行时**降级继续搜**，只报一次警告。
**unavailable 必须有 warning**：`virtual_display_t::start()`（真要建屏时）、
`virtual_display_available()`（客户端列表探测，虚拟 id 消失的唯一可观测点）与拓扑用的
`run_kscreen()` 都会报，且每个 helper 每进程只报一次（探测按客户端请求触发，否则刷屏）。
消息里给出该装哪个包（`krfb` / `libkscreen`）与配置项示例。

跨发行版的问题在**代码层**修；NixOS 模块里那行
`PATH=…krfb/bin:…libkscreen/bin:$PATH` 仍可保留（顺序上最先命中），但已不是必需。

### 第 3 步：屏幕组合（`dd_*` 模式）也进 C++

把 `sunshine-topology.sh` 的语义搬进同一处：`ensure_active / ensure_primary /
ensure_only_display / verify_only / disabled` 映射到上面同一组 kscreen 原语，
在**任何**启动路径都生效（不再依赖钩子），失败也能进日志/HTTP 响应。
这一步同时消掉“钩子必须和 Sunshine 同版本演进”的耦合。

## HDR（第二期：虚拟屏）

**采集侧不是缺口 —— 本仓早就实现了**（2026-09-25 逐行核实；此前交接里"采集环节完全没写"的说法有误，以本节为准）：

| 环节 | 实现位置 |
|---|---|
| 检测 | `pipewire.cpp` 的格式回调把**合成器协商出的** `color_primaries`/`transfer_function` 记进 `shared_state`（同时打 `[pipewire] Color primaries:` / `Transfer function:` 日志）；`pipewire_display_t::is_hdr()` 就是判 "BT2020 + SMPTE2084"，`get_hdr_metadata()` 给出 Rec2020 原色 + HDR10 标称亮度（4000/1；CLL 这个接口不给） |
| 协商 | `format_map` 把 10-bit 格式排在表首；协商到 `xBGR_210LE` 时 `build_format_parameter` 自动声明 `colorPrimaries=BT2020` + `transferFunction=SMPTE2084` |
| 编码 | `colorspace_from_client_config(config, is_hdr())`（`video.cpp:2648`）：客户端要 HDR **且**流是 HDR → BT.2020/PQ |

**真正缺的只有一步**：虚拟输出只能用 SDR 创建（krfb 无色彩参数，`kwinoutputconfig.json` 里 `highDynamicRange=false`），链路上游起不来。要接通就得在创建后 `kscreen-doctor output.<uuid>.hdr.enable`，并在收尾恢复（`kscreen_output_t` 快照需带上 HDR 位）。

> 对照：**物理 HDR 屏上的 HDR 串流今天就应该已经能工作**（`is_hdr()` 会如实上报）——缺的只有虚拟输出这一条。

**唯一硬门（待实测一次）**：KWin 允不允许给 krfb 的虚拟输出开 HDR。

```bash
kscreen-doctor -o | sed 's/\x1b\[[0-9;]*m//g' | grep -A3 'Virtual-SunshineVirt'
kscreen-doctor output.Virtual-SunshineVirt.hdr.enable   # 会在真实桌面生效
kscreen-doctor -o | sed 's/\x1b\[[0-9;]*m//g' | grep -A3 'Virtual-SunshineVirt' | grep -i hdr
kscreen-doctor output.Virtual-SunshineVirt.hdr.disable  # 回滚
```

门开后：合成器给 10-bit → 我们声明 BT2020/PQ → `is_hdr()` 如实上报 → 编码选 BT.2020/PQ（全部现成，只需接上开关）。

## 测试

* 单元：id → 名字映射、`virtual_display_available()` 的探测逻辑（可注入 PATH 桩）。
  已实现：`tests/unit/platform/linux/test_virtual_display.cpp`
  （`VirtualDisplayHelperLookup.*` 7 条：配置优先 / `$PATH` / 跳过不可执行项 / 跳过空 PATH 项 /
  兜底目录内容 / 找不到时返回空；`KwinOutputConfigParsing.*` 8 条：`connectorName` 与
  `name` 兜底 / 空 uuid 与错类型跳过 / `outputIndex` 状态匹配 / 无状态时的保守默认 /
  重复 uuid 保留 enabled 那份 / 真机那个"先列输出、再按 lid 变体给状态"的嵌套结构 / 坏 JSON）
  与 `tests/unit/test_config.cpp` 里两个配置解析用例（`virtual_display_helper`、`kscreen_helper`）。
* 手工矩阵（本机 Plasma Wayland + RTX 3070Ti）：
  1. 没装 krfb 时客户端**看不到**虚拟选项（第 1 步）；
  2. 装了 krfb：选虚拟-KWin → 输出出现、可采集、触控跟手、断开后布局复原；
  3. 选虚拟-KMS（打包安装、有 `cap_sys_admin`）→ KMS 采集可用；
  4. 六个 dd 模式逐个验证；断开后 `kscreen-doctor -o` 与开播前一致；
  5. 反例：会话在虚拟输出创建中途被打断（客户端秒断）→ 不留 krfb、不留幽灵输出。

## 风险 / 取舍

* `krfb-virtualmonitor` 是 KDE 组件 → 这是 Plasma 专属路径；GNOME 下只能不做（第 1 步已覆盖）。
* 子进程管理要防僵尸（`boost::process` 的 `child` 析构会 wait）与端口冲突（探测）。
* `kscreen-doctor` 是外部命令而非库；好处是与现有实现一致、行为可预期，坏处是依赖发行版包。
  若将来要彻底内化，可换成 KScreen 的 DBus API（工作量大，暂不做）。
* 定位顺序（配置 → `$PATH` → 标准目录）同时写在 `docs/configuration.md` 的两个配置项说明里，
  改顺序要一起改，否则用户按文档排查会走偏。
* 第 1 步先上，能把“菜单骗人 + 20 s 超时”这两件事一次性消灭，且不依赖第 2、3 步。
