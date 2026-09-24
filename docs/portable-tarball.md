# Portable tarball —— 非 Nix 发行版的免依赖分发包

> 目标读者：想在 **Ubuntu/Debian/Fedora/Arch 等非 NixOS** 发行版上直接运行
> Foundation Sunshine 的用户，以及维护者（如何产出这个包）。
>
> 脚本：[`scripts/make-portable-tarball.sh`](../scripts/make-portable-tarball.sh)
> （本 fork，`linux-support` 分支）。Nix 闭包来自
> [Biaogo/foundation-sunshine-linux.nix](https://github.com/Biaogo/foundation-sunshine-linux.nix)。

## 为什么 Releases 里的裸 tarball 在非 Nix 主机上跑不起来

Releases 附带的 tarball 是 **Nix store 原始产物**，三样东西全部绑死在
`/nix/store` 绝对路径上：

| 现象 | 原因 |
|---|---|
| `cannot execute: required file not found` | ELF interpreter 是 `/nix/store/…-glibc…/lib/ld-linux-x86-64.so.2`，主机上不存在 |
| 大量 `libboost_*.so … not found` | RPATH 全指向 `/nix/store`，主机没有这些库 |
| `…/bin/bash: bad interpreter` | `bin/sunshine` 是 bash wrapper，shebang 指向 store 内的 bash |

手工 patchelf 改 interpreter 也不行：二进制按 nixpkgs 的 glibc 2.42 编译，
Ubuntu 24.04 的 glibc 2.39 太老（向后不兼容），改完会报 `GLIBC_2.42 not found`。

## 正确姿势：解压即用（目标机零依赖）

portable tarball 把**整个运行时闭包**（glibc/ld.so、boost、ffmpeg 静态库、
pipewire、openssl……）一起搬进固定前缀 `/opt/sunshine-portable`，并把所有
`/nix/store` 引用重写到该前缀下。目标机器**什么都不用装** —— 没有 Nix、
没有 patchelf、不需要 fuse（比 AppImage 依赖更少），自带 glibc 意味着主机
glibc 更老也能跑。

```bash
# 目标机（任意 x86_64-linux 发行版）
sudo tar -C / -xzf foundation-sunshine-<版本>-portable-linux-x86_64.tar.gz
/opt/sunshine-portable/bin/sunshine
```

然后按正常 Sunshine 流程使用：

- Web UI：<http://localhost:47990>（配置在 `~/.config/sunshine/`）
- Moonlight 自动发现需要主机运行 `avahi-daemon`（Ubuntu 默认已装）；没有的话
  在 Moonlight 里手填主机 IP 直连即可
- TLS 证书校验使用主机 CA bundle（`/etc/ssl/certs/ca-certificates.crt`）
- 防火墙/端口要求与上游一致（TCP 47984/47989/47990、UDP 47998-48000/48010 等）
- 卸载：`sudo rm -rf /opt/sunshine-portable`

**注意**：

- `bin/sunshine` 仍是 wrapper 脚本，真身是 `bin/sunshine-<版本>`；
  需要 KMS 捕获时按 README 的运维要点给二进制设 `cap_sys_admin`（setcap）。
- NixOS 用户**不要**用这个包 —— 请用 flake（`/nix/store` 已由 Nix 管理，
  解到系统里纯属污染）。
- 升级 = 下载新版本 tarball 重新解压覆盖（前缀内自包含，无残留状态）。

## 维护者：如何产出 portable tarball

构建机需要 `nix` + `patchelf`（没有 patchelf 时脚本会自动 `nix build nixpkgs#patchelf`）：

```bash
# 默认：从 .nix flake 仓库拉/构建 CPU 变体并打包
scripts/make-portable-tarball.sh [OUT.tar.gz]

# 指定一个 store path（例如本机已有的、或 cachix 拉下来的）：
scripts/make-portable-tarball.sh OUT.tar.gz --store-path /nix/store/xxx-foundation-sunshine-<版本>

# 自定义安装前缀（默认 /opt/sunshine-portable）
scripts/make-portable-tarball.sh --prefix /opt/my-sunshine
```

产出前脚本会做结构守卫（staged 路径数 == 闭包路径数、ld.so 存在、无
`nix/store/nix` 嵌套）与重写完备性校验（文本 PCRE 负向后顾 + ELF
interp/RPATH 残余检查）。

**发布前必做的端到端验证**（在构建机或任意测试机上）：

```bash
tar -xzf OUT.tar.gz -C /          # 或用 --prefix 打测试包解到对应根
ldd /opt/sunshine-portable/bin/sunshine-<版本> | grep -c 'not found'   # 必须为 0
/opt/sunshine-portable/bin/sunshine --version
```

CPU 变体 2026.09.10 实测：239 个闭包路径 / 761 MB → 压缩后约 293 MB。

### 实现要点（改脚本前必读，全是踩过的坑）

1. **拷闭包用 tar 管道，不用 `cp -a -t DEST <一堆参数>`** —— cp 会解引用
   argv 里显式给出的符号链接，跨包链接（如 `libgcc_s.so.1`）直接丢失。
2. **GNU tar 剥掉成员一层前导 `/`** —— 必须解包到 payload 根目录，树才会
   落在 `$PAYLOAD/nix/store/...`；解错一层会静默嵌套成 `nix/store/nix/...`。
3. **重写后绝不清理"悬空"符号链接** —— 指向 `$PREFIX/...` 的绝对链接在
   staging 里必然悬空，要等解压到 `/` 才成立；清理会把 libgcc 等全删光。
4. **文本幂等**：sed 先把旧前缀归一再重写，重跑安全。
5. **校验要排除新前缀**（`(?<!$PREFIX)/nix/store/`），否则每个改写成功的
   引用都会被误报为 leftover。
