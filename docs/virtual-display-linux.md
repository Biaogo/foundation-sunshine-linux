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

依赖声明：deb 的 `CPACK_DEBIAN_PACKAGE_DEPENDS` 增加 `krfb, kscreen`（Plasma 用户默认已有）。
非 Plasma（GNOME 等）不满足 → 第 1 步就不 advertise。

### 第 3 步：屏幕组合（`dd_*` 模式）也进 C++

把 `sunshine-topology.sh` 的语义搬进同一处：`ensure_active / ensure_primary /
ensure_only_display / verify_only / disabled` 映射到上面同一组 kscreen 原语，
在**任何**启动路径都生效（不再依赖钩子），失败也能进日志/HTTP 响应。
这一步同时消掉“钩子必须和 Sunshine 同版本演进”的耦合。

## 测试

* 单元：id → 名字映射、`virtual_display_available()` 的探测逻辑（可注入 PATH 桩）。
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
* 第 1 步先上，能把“菜单骗人 + 20 s 超时”这两件事一次性消灭，且不依赖第 2、3 步。
