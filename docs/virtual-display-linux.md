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

#### 起 helper 要重试（2026-09-26 实测）

单次 spawn 不够：本机会在**几秒到几分钟的窗口**里对 helper 的 store 路径返回 `execve … ENOENT`，
而文件本身完好 —— `nix-store --verify-path` 通过、`test -x` 为真、**同一份字节拷到 `/tmp` 能跑**、
用 `ld.so` 显式加载也能跑、同目录的 `krfb` 也能跑，且 `systemd-run --user`（不受调用方沙箱影响）
同样复现。触发条件未知（1.2 万次高频采样 0 复现，当天集中在 store 被清理/重写前后），但**被删掉的
主机钩子当年正是靠"3 次尝试 + 每次重启 helper"扛过去的**，所以实现保留同一语义：

- `helper_start_poll_budgets()` 给出三次尝试的轮询预算（7/8/9 个 `OUTPUT_POLL`，约 2.1/2.4/2.7 s，
  合计与原单次 8 s 相同：**不能更短**（会把"慢但正常"的 helper 判死），**也不能更长**
  （客户端耐心约 10 s，spawn 返回时它的等待已经在走）；
- 每次尝试前先 `terminate()` 上一次的 helper —— 第二个 krfb 会抢同一端口/输出；
- 每次尝试**先等输出出现**（`wait_for_output(name, polls)`），于是"spawn 成功但输出没出现"
  这类失败也会被下一次尝试覆盖；
- spawn 抛异常时**不早退**：输出可能已存在（上次会话遗留、或操作员手工起的），由等待来判定。

对应的主机侧配套（`sunshine.nix`，同日提交）：profile 里装一个"先探针再 exec"的
`krfb-virtualmonitor` 包装器（外加无合成器时静默跳过、清同端口遗留），两层合起来覆盖约 8~9 s。

#### 真正的根因：`kscreen-doctor -o` 的输出被截断了（2026-09-26 定位）

现象：新实例上照样出现 `Virtual output [Virtual-SunshineVirt] appeared but could not be enabled`
（3 秒重试之后 ✗），随后 8 s 拿不到输出 ⇒ 回落 eDP-1，且**触控绑到一个没启用的输出** ⇒ 触控失效。

手工复现时反而一切正常：`krfb-virtualmonitor --name SunshineVirt` 建出的输出在 `kscreen-doctor -o` 里
**默认就是 `enabled`**、uuid 也能正常解析、`output.<uuid>.enable` 退出码 0 ✔。差别在于**大小**：

```
$ kscreen-doctor -o | wc -c
12818                      # 257 个模式（虚拟输出会累积历史模式 ✗）
$ kscreen-doctor -o | grep -n '^Output: '
1:Output: 1 eDP-1 …
29:Output: 2 Virtual-SunshineVirt …      # ← 目标块在第 29 行
```

而 `capture_stdout()` 原来是：

```cpp
while (child.running() && std::getline(out, line)) { … }   // ← 病根
```

**子进程退出后，管道中未读完的数据会被整段丢弃** ⇒ 12.8 KB 的列表可能在目标块之前就被截断 ⇒
`output_uuid()` 取空 ⇒ `enable_output()` 连执行机会都没有 ⇒ 报"appeared but could not be enabled"。
是否截断取决于子进程退出与读取的赛跑 ⇒ 表现成**间歇**（当天 3/21 ✗）。

修法：**读到 EOF**（去掉 `child.running()` 条件 ✗），并在放弃时把"列表多少字节、有没有列出该名字"
写进日志，让下次失败自证 ✔。

#### 真正的杀手：`kscreen-doctor -o` 带颜色，解析器却是按裸文本写的（同日二次定位）

截断修掉之后**依旧**掉 eDP-1，而新加的日志给出了决定性一行：

```
Could not enable [Virtual-SunshineVirt]: the kscreen listing was 12819 bytes and did list it
```

列表完整、名字在，enable 却不生效 ⇒ 直接取原始字节看：

```
$ kscreen-doctor -o | head -2 | cat -v
^[[01;32mOutput: ^[[0;0m1 eDP-1 d0212254-…
	^[[01;31mdisabled^[[0;0m
```

**输出到管道也照样上色**（实测 47 行含转义序列 ✗）。于是：

- `line.rfind("Output: ", 0) == 0` ⇒ 行首其实是 `\x1b[01;32m` ⇒ **块识别整条失效** ✗
- `trimmed == "enabled"` ⇒ 实际是 `\x1b[01;32menabled\x1b[0;0m` ⇒ **永远不相等** ✗

⇒ `enable_output_by_name()` 永远返回 false ⇒ 杀掉 helper ⇒ 回落 eDP-1 + 触控绑到已消失的输出。
（这也是为什么手工测试"怎么测都正常" ✗：手工命令里都带 `sed 's/\x1b\[[0-9;]*m//g'` ✗。）

修法：**在读取处统一剥掉 ANSI**（`capture_stdout()` 出口 `strip_ansi()` ✗）+ 纯函数 `kscreen_output_is_enabled()`
自身也先剥一遍 ✔；回归测试用**彩色列表**作为输入 ✔（`KscreenOutputEnabled.ReadsAColouredListing` ✗）。

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

**配置的路径：无条件信任（2026-09-26 两轮修正后的最终语义）**：可执行性检查只是防手滑，它在这台机器上会**误报**
—— 同一个 helper 路径会出现"`access()`/`stat` 说不在、`ls` 说在"的**分钟级**窗口（运行中的服务也撞上过），
连"看父目录是否存在"也会在同一次抽风里失效。所以：
- 配置了绝对路径 → 检查失败（或检查窗口内读不到）也**照用**，只打一条
  `could not be verified as an executable file right now; using it anyway`；
- 检查的**重试只用于配置路径**（单次候选，15/60/200 ms）；`$PATH`/标准目录的搜索保持**零延迟单次检查**
  —— 早期版本把重试放进通用检查，导致客户端拉一次列表要几秒而超时（且列表空白）；
- 路径真不可用时会在**启动 helper 那一步**响亮失败，而不是静默把功能藏起来。

失败长什么样（真机上抓到的）：列表空白/加载超时 + 会话回落到第一块物理屏（`[kwingrab] Screencasting output name eDP-1`）
+ `Warning: Could not create the virtual display for this session` + 客户端 `Initial Ping Timeout`。

#### 并排验收：`SUNSHINE_VDISPLAY_NAME` / `SUNSHINE_VDISPLAY_PORT`

进程内创建虚拟输出之后，**端口（`5910`）和输出名（`SunshineVirt` → `Virtual-SunshineVirt`）曾是编译期常量**，导致第二个实例必定和正在跑的服务抢同一个输出（端口占用 + 同名输出不会被重复枚举）——本仓的「并排测试实例」习惯（不动生产服务）因此在虚拟屏这条路上失效。现在两个覆盖回来了：

| 变量 | 默认 | 作用 |
|---|---|---|
| `SUNSHINE_VDISPLAY_NAME` | `SunshineVirt` | 传给 helper 的 `--name`；输出名固定是 `Virtual-<该值>`，**客户端可见的虚拟 id 也翻译到它**，所以三处一定一致 |
| `SUNSHINE_VDISPLAY_PORT` | `5910` | helper 监听的本地 VNC 端口 |

- 名字会先 trim；**留空/纯空白**、端口非数字或不在 `1-65535` → 回退默认，并各打一条 `Ignoring SUNSHINE_VDISPLAY_*` 警告（不静默）。
- 生效时会打一条 `Virtual display overrides in use: output [Virtual-ProbeVirt], port 5920`，便于确认覆盖真的进来了。
- 两个覆盖都是**进程级**（启动 Sunshine 时导出即可，不需要改配置），与旧 hook 脚本的约定一致。

```bash
export SUNSHINE_VDISPLAY_NAME=ProbeVirt SUNSHINE_VDISPLAY_PORT=5920
```

注意：`output_name` 写死 `Virtual-SunshineVirt` 的配置在覆盖生效后不再命中"默认"pick（默认 pick 比对的是**当时的输出名**），要么把 `output_name` 一起改成 `Virtual-ProbeVirt`，要么只对**显式选虚拟 id**的客户端用覆盖。

**完整可跑流程（2026-09-26 实测到"实例起来 + 配置被读到 + 四个端口 bind"这一步）**：

```bash
B=/tmp/fst-probe; D="$B/sunshine"; mkdir -p "$D"
cp ~/.config/sunshine/sunshine.conf ~/.config/sunshine/apps.json \
   ~/.config/sunshine/sunshine_state.json "$D"/
cp -r ~/.config/sunshine/credentials "$D"/

printf 'port = 49000\n' >> "$D/sunshine.conf"                 # 任意空闲 base
sed -i 's|^output_name = .*|output_name = Virtual-ProbeVirt|' "$D/sunshine.conf"
# 若 helper 不在调用者的 PATH 里（普通登录 shell 通常不在），直接把绝对路径写进副本：
printf 'virtual_display_helper = /path/to/krfb-virtualmonitor\n' >> "$D/sunshine.conf"
printf 'kscreen_helper = /path/to/kscreen-doctor\n' >> "$D/sunshine.conf"

# 防火墙放行移位端口（TCP: base, base-5, base+1, base+21；UDP: base+9/10/11，麦克风再加 base+12）
# ⚠️ 一次只能开一个端口：该工具只读 $2/$3，多写会静默地只开第一个！
sudo nixos-firewall-tool open tcp 49000
sudo nixos-firewall-tool open tcp 48995
sudo nixos-firewall-tool open tcp 49001
sudo nixos-firewall-tool open tcp 49021
sudo nixos-firewall-tool open udp 49009
sudo nixos-firewall-tool open udp 49010
sudo nixos-firewall-tool open udp 49011
sudo nixos-firewall-tool show | grep -E '4899|4900|4901|4902'   # 8 条规则（4 TCP + 3 UDP × v4/v6）都要在

env XDG_CONFIG_HOME="$B" SUNSHINE_VDISPLAY_NAME=ProbeVirt SUNSHINE_VDISPLAY_PORT=5920 \
  <store>/bin/sunshine &                                     # 记下 PID

ss -ltn | grep -E '4900[01]|48995|49021'                     # 4 个端口都要在
grep -aE "config: '(port|output_name)'" "$D/sunshine.log"    # 确认读的是副本
grep -aiE 'fatal|already in use' "$D/sunshine.log"           # 必须为空
```

- **`nixos-firewall-tool` 一次只接受一个端口**（它只读 `$2/$3`，多端口静默只开第一个）：漏开 `base-5`（HTTPS）时客户端表现为**显示器列表加载缓慢/为空、启动失败**，而服务器侧日志里**一条客户端请求都没有** —— 这个症状组合先查防火墙，别查代码。
- **`$XDG_CONFIG_HOME/sunshine/` 这一层是必须的**：Sunshine 在该子目录里找配置，少一层就等于全新安装 —— 表现为索要新账号密码、`port` 被忽略、随后 `Fatal: Couldn't bind RTSP server to port [48010], Address already in use` 撞上正在跑的服务（伴随 `File <cfg>/sunshine/sunshine_state.json doesn't exist`，即没配对）。
- 会话开始后日志应出现 `Virtual display overrides in use: output [Virtual-ProbeVirt], port 5920`（这行只在会话启动时打）。
- harness 构建**不带 cap**（没走 setcap wrapper）⇒ 会话内 kwin/portal 可用（虚拟屏正是 kwin），**KMS/SDDM 不在这个形态的范围**。
- 第二实例不注册 mDNS（`avahi::entry_group_new() failed: Not permitted`），客户端要手动加 `IP:49000`，且需要能自定义端口的客户端（Artemis / moonlight-vplus，原版 Moonlight 不行）。
- 收尾：`kill <PID>`（按号，别按模式）、`sudo nixos-firewall-tool reset`、`rm -rf $B`。

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

**唯一硬门 —— 已实测，门是关的（2026-09-25/26）**：对活的虚拟输出发
`kscreen-doctor output.Virtual-SunshineVirt.hdr.enable`，KWin 的答复是 **`HDR: incapable`** ——
krfb 的虚拟输出没有 HDR 能力位，链路上游根本起不来。

**而且这不是"补一个开关"能解决的（结构性障碍）**：本机会话的采集走
**`kwingrab`（KWin 合成器协议）**，而 `src/video.cpp` 的 HDR 判定写得很直白：

> HDR can only be served by a KMS/DRM capture whose output advertises HDR metadata.
> A compositor capture (KWin/PipeWire) has no HDR pipeline at all, and a client HDR request
> there produced a still-SDR-coded 10-bit stream …

⇒ 能出 HDR 的只有 **KMS/DRM** 采集，而那是**物理 CRTC**；虚拟输出不在 DRM 里。
**两者当前互斥** —— "虚拟屏 + 真 HDR"不是本仓改几行能做到的 ✔。

**现实可行的真 HDR 路径（本仓侧已就绪）**：物理 HDR 显示器 + `capture = kms`（需 `CAP_SYS_ADMIN`）。
采集侧 `is_hdr()` / BT2020+P2084 协商早已实现，客户端要 HDR 且流确实是 HDR 时，编码自动走
BT.2020/PQ —— 缺的只有"那条采集路径 + 一块真 HDR 输出" ✔。

## 真 HDR 的 Linux 路线评估（2026-09-26 调研）

**Windows 那边怎么做的**：AlkaidLab / qiin2333 的 Foundation Sunshine 用
[`qiin2333/zako-vdd`](https://github.com/qiin2333/zako-vdd)（C++，MIT）—— 一个 **Windows IDD 内核驱动**，
README 自述"专为 Foundation Sunshine 设计的 Windows 虚拟显示器驱动，提供完整的 HDR 支持"，
特性含 **EDID 支持（可模拟特定显示器特性）**；要求 Windows 10 22H2+/x64/DX11/管理员装驱动。

⇒ **代码不能参考**（Windows 内核驱动模型 + DX11，Linux 无对应 API），
**但思路完全对应**：给虚拟输出一份**声明 HDR 的 EDID + 颜色空间能力**。

**Linux 的原生对应物是 VKMS 的 configfs 接口**（内核文档 `gpu/vkms` + Bootlin/Louis Chauvet 的
补丁系列）：`/sys/kernel/config/vkms/<DEV>` 下可建 device / connector / crtc / encoder / plane，
connector 的 **`supported_colorspaces` 非 0 时内核会创建 `HDR_OUTPUT_METADATA`**，另有
**`edid` / `edid_enabled`** 可注入原始 EDID。这条路能**同时**解掉本文件上面那两处障碍：
虚拟输出变成真 DRM 输出（KWin 认它的 HDR 能力），而且它是 KMS/DRM ⇒ `capture = kms` 可用。

**但本机当前内核还没有这组能力（2026-09-26 实测）**：`7.2.7-cachyos` 的 `vkms.ko` 里
`supported_colorspaces` / `edid_enabled` / `HDR_OUTPUT_METADATA` / `drm_edid_read_custom`
**全部 0 次命中** ⇒ 该系列补丁（v4）尚未合并。`CONFIG_DRM_VKMS=m`、`CONFIG_CONFIGFS_FS=y` 都在，
所以等它合并后**不需要换发行版，只需要新内核**。

**当前可落地的真 HDR 路径（不需要等内核）**：本机 `/sys/class/drm/` 有 `DP-1` / `DP-2` / `HDMI-A-1`
三个物理连接器 ⇒ 给其中一个接上
（a）一块 HDR 显示器，或（b）一个**带 HDR EDID 的假负载（EDID emulator / headless 插头）**，
就能得到一块真 HDR 源，再配 `capture = kms`（需 `CAP_SYS_ADMIN`）。
采集侧 `is_hdr()`/BT2020+P2084 早已就绪，客户端（如 OnePlus 13：HDR10/HDR10+/HLG/DV、
P010、PQ 透传 ✔）本来就能解 —— 缺的一直只是主机这一端的 HDR 源。

## 结论（2026-09-26 加测）：当前代码拿不到 HDR —— **不是配置问题，是判据错了**

`src/video.cpp` 的 HDR 放行判据是"**显示 id 是不是空或纯数字**"：

```cpp
const auto &hdr_target = config.display_name.empty() ? config::video.output_name : config.display_name;
if (config.dynamicRange && !is_kms_display_id(hdr_target)) {   // is_kms_display_id = 空或纯数字
  BOOST_LOG(warning) << "HDR requested but " << … << " is a compositor output without an HDR pipeline, falling back to SDR";
  config.dynamicRange = 0;
}
```

而客户端能看到的列表（`platform/linux/misc.cpp` 的 `client_display_names()`）**故意只给合成器名字** ——
它的注释写明：KMS 枚举在没有采集所需权限时也能成功，所以不能把连接器名字给客户端
（"实测：客户端列表里是 `eDP-1`，而会话走的是 KWin"）。

两条合起来 ⇒ 客户端选 **`eDP-1`** 会被判成"合成器输出" ⇒ **无论 `capture` 怎么配（含 `capture = kms`）都回退 SDR** ✗
（只有 KMS 那一侧的**数字 id** 才放行）。

**要让它成立，需要一起改三处**：
1. **判据改成"本次会话实际使用的采集后端"**（或 `capture = kms` + 目标屏确实可由 KMS 采），而不是看名字像不像数字；
2. **连接器名 ⇄ KMS 索引的映射** —— kmsgrab 需要的是 KMS 监视器/CRTC 侧的对象，客户端给的是 `eDP-1` 这种名字；
3. **`capture` 未显式配置 + 客户端请求 HDR ⇒ 优先 kms**，失败时诚实回退并写明原因
   （用户态体验对齐 alkaid 在 Windows 上的 VDD：客户端开 HDR 就自动生效）。

> 依赖确认（2026-09-26 实测）：`/run/wrappers/bin/sunshine` 带 `cap_setpcap,cap_sys_admin=p`，
> 运行进程 `CapInh=0x200100`（含 `cap_sys_admin`）⇒ **kmsgrab 提权的前提已经具备**，缺的只是上面的接线。

## 实测：在 NVIDIA + KWin 上给 eDP-1 开 HDR 会被驱动拒绝（2026-09-26）

操作：V+ 开 HDR + 选 eDP-1 + 系统设置里打开 eDP-1 并开 HDR。

- KDE 报错：**"无法应用显示器配置：驱动程序拒绝了输出配置"**
- 内核日志：`[nvidia-drm] Infoframe changed on CRTC 0 (hdr=1 colorspace=1 vsif=0), adding planes …`
  ⇒ **HDR 请求确实到了驱动**，infoframe 也生成了；eDP-1 在 DRM 层变成 `enabled`
- KWin 日志：`GL_INVALID_OPERATION … <image> and <target> are incompatible` +
  `Invalid framebuffer status: "GL_FRAMEBUFFER_INCOMPLETE_ATTACHMENT"`（刷屏）

**这是已知的上游缺陷，不是本机 eDP-1 的问题**：KDE bug **491751** 症状逐字吻合（同样的
"driver rejected the output configuration" + 同样的 GL 刷屏），报告者为 NVIDIA RTX 4070 + Wayland；
同一份报告里他在 **HDMI 2.1 接 LG C1 电视时 HDR 正常**，只在某台 DP 显示器上失败。
相关：bug 519593（Blackwell + 595.x，HDR + 全屏视频导致崩溃，"resolved upstream" 但未真修）。

⇒ **结论**：在 Linux 上"某块输出能不能开 HDR"取决于**该输出的 EDID / 链路**与 NVIDIA 驱动 +
KWin 这条 10-bit 合成路径是否合得来，**不是 Sunsine 侧能决定的**。

**一个值得先试的旁路**：`KWIN_DRM_NO_DIRECT_SCANOUT=1`（KWin 官方 wiki 记录的环境变量，见 bug 497991）
—— 若失败点在 10-bit 的 direct-scanout 路径上，关掉它可能就让 HDR 应用成功（代价：略有性能损失，
且需要重启 KWin）。**先试它，再谈买 EDID 插头**：借一台在 Linux 下能开 HDR 的电视/显示器试一次，
成功则把那块屏的 EDID dump 出来烧进可写 EDID 的假负载（emulator），失败则省下这笔钱。

**与目标的关系**（"V+ 开 HDR + 选任意屏都生效"）：
- 物理输出（真 HDR 屏 / EDID 假负载）⇒ 可行，前提是上面这条上游缺陷不挡路 + fork 侧实现
  "检测 HDR ⇒ 自动用 KMS 采集"；
- **krfb 虚拟输出 ⇒ 短期做不到**（KDE 不给它 HDR 能力位，`HDR: incapable`）；
- 折中：让虚拟输出与假负载做 KWin **replication（镜像）**，会话里"选虚拟屏"时实际采集假负载那块，
  即可等价做到"选任意屏都出 HDR"。

> 备注：想在**服务上下文之外**手工起 `krfb-virtualmonitor` 复现探针，实测会**静默退出**
> （无输出、不监听端口）；由 Sunshine 自己拉起时才正常。要复现这类实验，必须让探测逻辑跑在
> 会话里（或临时用测试实例），不要指望从普通 shell 起 helper。

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

## 现状补记（2026-09-26 实测）：EDID 覆盖与 NVIDIA 的 10-bit 路径

**门已经改了**：HDR 的放行判据从"显示 id 的形状"换成 `display_id_uses_kms(display_name, capture)`
（`src/video.h` / `src/video.cpp`），与 `platform/linux` 的路由同源 —— 显式 `capture = kms` 时 KMS 是唯一
候选，而 kmsgrab 的 `map_display_name_to_monitor_index()` 本来就能解析连接器名，所以 `eDP-1` 这类名字
在 `capture = kms` 下应当允许 HDR。`auto`/空 下的行为与旧判据逐条一致（已用真机 id 集合镜像核对）。

**但端到端仍不通，卡在输出侧，与本仓无关**：把 eDP-1 的 EDID 改成带 HDR10(PQ) 的版本后，KWin 依旧刷

```
kwin_wayland: 0x502: GL_INVALID_OPERATION error generated. <image> and <target> are incompatible
kwin_wayland: Invalid framebuffer status:  "GL_FRAMEBUFFER_INCOMPLETE_ATTACHMENT"
```
内核侧同时照旧生成 `[nvidia-drm] Infoframe changed on CRTC … (hdr=1 colorspace=1 …)`。

> **更正（同日实测）**：先前把上面那两条 GL 报错当作 HDR 的失败原因是**错的**。一次干净开机里它们
> **6 条全部出现在生产实例的 KWin ScreenCast 会话开始的同一分钟**，当时**任何 HDR 尝试都没有**
> ⇒ 它在普通串流会话里就会刷。所以它们**不是**可信的 HDR 诊断信号，失败原因尚未定位；
> 判断 HDR 只认 `Sent HDR mode:` / `Color coding: HDR (BT.2020)` / 内核的 `hdr=1` infoframe 这几条。

**实测补充**：`drm.edid_firmware`（启动期固件）**是有效的** —— 在一台已用
`hardware.display.edid.packages` + `hardware.display.outputs.eDP-1` 生成 `video=eDP-1:e` 与
`drm.edid_firmware=eDP-1:edid/legion-eDP-1.edid` 的主机上，`/sys/class/drm/card0-eDP-1/edid`
与那份固件文件**逐字节一致**。所以要做 EDID 实验就走这条路（改仓里那份 .edid ⇒ 重建 ⇒ 重启），
运行时 debugfs 覆盖不可靠：

**EDID 覆盖在 nvidia-drm 上不生效**（实测）：`/sys/kernel/debug/dri/0/eDP-1/edid_override` 写入成功
（读到 `0x8a = 07`），但连接器实际报出的 `/sys/class/drm/card0-eDP-1/edid` 仍是 `05`，且
`kscreen-doctor output.eDP-1.disable/enable` **不会**触发内核重读。要做 EDID 实验必须走启动期参数：

```nix
hardware.firmware = [ (pkgs.runCommandLocal "edid-edp-1-hdr10" {} ''
  mkdir -p $out/lib/firmware/edid
  cp /path/to/eDP-1-hdr10.edid $out/lib/firmware/edid/eDP-1-hdr10.bin
'') ];
boot.kernelParams = [ "drm.edid_firmware=eDP-1:edid/eDP-1-hdr10.bin" ];
```
（挂起/唤醒或重启后核对：`dd if=/sys/class/drm/card0-eDP-1/edid bs=1 count=384 status=none | xxd | sed -n '9,11p'`，
看到 `07` 才说明生效。）

**根因已定位为 NVIDIA 驱动缺陷（2026-09-26 实测 + 上游报告对照）**：本机 `drm_info` 读到
eDP-1 有 `HDR_OUTPUT_METADATA` ✔ 与 `Colorspace {Default, BT2020_RGB, BT2020_YCC}` ✔、平面也支持
`ABGR2101010`/`XBGR2101010`/`P010` ✔，**但整个 card0 上没有任何 `max bpc` 属性** ✗ —— KWin 无法把输出
切到 10 bpc，于是 atomic commit 被驱动静默 EINVAL 拒绝。这与 NVIDIA 开发者论坛的现行未解决报告逐条吻合
（RTX 5070 + 610.43.02 + Plasma 6.6.5 Wayland：同为"EDID 含 HDR10、有 Colorspace/HDR_OUTPUT_METADATA、
无 max bpc、内核侧静默、KWin GL 帧缓冲刷屏、Windows 下同硬件正常"）。报告者试过不同刷新率、单显示器、
只开 WCG、`nvidia_drm.color_pipeline=0/1` 均失败 ⇒ **没有 workaround**。
（本机：RTX 3070 Ti Laptop + 615.71.09 + KWin/Plasma 6.6.6 + `7.2.7-cachyos`。）
⇒ 所以 eDP-1 上的 HDR **要等 NVIDIA 修**；本仓这边的准备工作（HDR 门按采集路由判定 + 引擎侧 HDR 支持）
一旦输出侧能开就会立刻生效。

**另一个独立障碍（实测 2026-09-26）**：强制 `capture = kms` 启动一个**用户身份**的实例时，
`kmsgrab` 的 `cap_sys_admin` RAII 提权会一路失败（`Failed to gain CAP_SYS_ADMIN` 刷屏 ⇒ KMS 源
`Final KMS display_names return list:` 为空 ⇒ `/displays` 空列表 ⇒ 客户端 503 "Failed to initialize
video capture/encoding"）。已排除的原因：给二进制加文件 capability（`cap_setpcap,cap_sys_admin=eip`，
属主 root、非用户可写、/opt 在 btrfs 无 nosuid）**仍然失败** ⇒ 不是 cap 授予方式的问题，而是本进程
自己先清掉了 PERMITTED（`misc.cpp` 的 `drop_elevated_privileges()`，其注释明确写"清掉 PERMITTED 会让
KMS 在进程余下生命周期内不可用"）。⇒ 用测试实例做 **KMS 采集端到端验证**之前，必须先解决这条
（或改用别的途径验证 KMS 选择逻辑）。

**留给下一步的路**：借一块在 Linux+NVIDIA 下真能开 HDR 的屏/电视实测一次（判断是"屏"还是"驱动"，
或更换 NVIDIA 驱动分支；虚拟输出那条路仍要等 VKMS 的 `supported_colorspaces`/`edid` 补丁合并。

