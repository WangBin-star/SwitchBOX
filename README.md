# SwitchBOX

## 本项目的目的

让 `Nintendo Switch` 可以像电视盒子一样使用，支持两类核心能力：

- 播放 `SMB` 共享视频
- 播放 `IPTV` 电视节目

注意：

- 请在 **大气层虚拟系统** 环境中使用，因为当前开发与验证都只围绕该使用场景展开

## 参考项目 & 感谢

IPTV 方向参考：

- `TsVitch`
- https://github.com/giovannimirulla/TsVitch

SMB / 本地媒体 / 流式播放实现路径参考：

- `nxmp`
- https://github.com/proconsule/nxmp

UI 框架：

- `Borealis`
- https://github.com/natinusala/borealis

## 软件操作说明（当前版本）

本章节用于说明当前可直接使用的操作，重点是播放器交互。

### 主页

- 左右切换卡片，`A` 进入当前卡片。
- `+` 进入设置页。

### SMB 浏览页

- `A`：进入文件夹或播放当前文件。
- `B`：返回上一级，或退出当前 SMB 浏览页。
- `X`：删除当前焦点条目，会弹出确认框。
- `Y`：直接返回主页。

### 播放器（SMB 文件播放页）

基础操作：

- `A`：播放 / 暂停。
- `B`：退出播放器，回到 SMB 列表。
- `X`：删除当前播放文件，确认后退出播放器、删除文件并回到列表。
- `↑ / ↓`：调节音量，会显示音量浮窗。

跳转与倍速：

- `L`：短退。
- `R`：短进。
- `ZL`：长退。
- `ZR`：长进。
- `Y`：按住倍速播放，松开恢复。
- `R + 左/右`：连续短退 / 连续短进。
- `ZR + 左/右`：连续长退 / 连续长进。

浮窗与面板：

- `左`：打开左侧文件列表浮窗，再按一次关闭。
- `+`：打开底部进度控制面板，再按 `+` 或 `B` 关闭。
- 底部面板当前支持：旋转、长退、短退、播放/暂停、短进、长进、音轨、字幕。

## 当前项目状态

截至 `2026-04-16`，项目已调整为 **纯 Switch 项目**，桌面端构建、桌面端代码分支和相关脚本均已移除。

当前已完成：

- Switch 端可稳定构建、打包并生成 `.nro`
- 设置页可编辑基础设置、`IPTV` 条目、`SMB` 条目
- 语言资源支持外置 `langs/`
- 首页已接入横向卡片布局
- `SMB` 浏览页可列目录、进目录、返回上级、删除条目、返回主页
- `SMB` 文件已切换为纯流式播放并进入真实播放器页
- `SMB` 播放工作流已基本打通：从首页卡片进入、浏览、播放、控制、删除、返回列表均可用
- 播放器已接入暂停、退出、删除、跳转、倍速、音量、左侧浮窗、底部控制面板、音轨/字幕、画面旋转
- 已修正首播报错、高码率视频首帧黑屏、方向输入重复触发等关键稳定性问题
- `sort_order` 已真实作用于 `SMB` 浏览页和播放器左侧浮窗

当前未完成：

- `IPTV` 实播
- 播放历史
- 断点续播

## 开发环境

推荐环境：

- `Windows`
- `VS Code`
- `devkitPro`
- `Python 3`

必需环境变量：

```text
DEVKITPRO=C:\devkitPro
DEVKITA64=C:\devkitPro\devkitA64
```

推荐验证命令：

```powershell
python --version
echo $env:DEVKITPRO
echo $env:DEVKITA64
```

当前必需依赖：

- `switch-glm`
- `ninja`
- `libsmb2`（Switch portlibs）
- `libmpv`（Switch portlibs，播放器功能依赖）

## 构建与打包

### VS Code 任务

当前保留的任务：

- `switchbox: configure switch`
- `switchbox: build switch`
- `switchbox: package switch`

### PowerShell 直接执行

进入项目目录：

```powershell
cd "D:\Github项目\SwitchBOX\SwitchBOX"
```

配置：

```powershell
$env:Path = "C:\devkitPro\msys2\usr\bin;" + $env:Path
$env:DEVKITPRO = "/opt/devkitpro"
$env:DEVKITA64 = "/opt/devkitpro/devkitA64"
C:\devkitPro\msys2\usr\bin\cmake.exe --% -S . -B build/switch -G Ninja -DCMAKE_MAKE_PROGRAM=C:/devkitPro/msys2/usr/bin/ninja.exe -DCMAKE_TOOLCHAIN_FILE:FILEPATH=cmake/toolchains/switch.cmake
```

编译：

```powershell
$env:Path = "C:\devkitPro\msys2\usr\bin;" + $env:Path
$env:DEVKITPRO = "/opt/devkitpro"
$env:DEVKITA64 = "/opt/devkitpro/devkitA64"
C:\devkitPro\msys2\usr\bin\cmake.exe --build build/switch --parallel
```

打包：

```powershell
$env:Path = "C:\devkitPro\msys2\usr\bin;" + $env:Path
$env:DEVKITPRO = "/opt/devkitpro"
$env:DEVKITA64 = "/opt/devkitpro/devkitA64"
C:\devkitPro\msys2\usr\bin\cmake.exe --build build/switch --target switchbox_switch_package --parallel
```

也可以直接使用：

- `tools/build-switch-package.cmd`

## 当前产物位置

编译产物：

- `build/switch/app/switchbox_switch.elf`
- `build/switch/app/switchbox_switch.nacp`
- `build/switch/app/switchbox_switch.nro`

打包产物：

- `build/releases/<version>/switch/SwitchBOX.nro`
- `build/releases/<version>/switch/switchbox.ini`
- `build/releases/<version>/switch/langs/`

当前约定是：

- 当需要“打包”时，统一输出到 `build/releases/<version>/switch/`
- 该目录下的内容可直接复制到设备的 `switch/SwitchBOX/` 目录

## 配置与安装约定

安装时需要一起复制：

- `.nro`
- `switchbox.ini`
- `langs/`

当前 `switchbox.ini` 查找顺序：

1. `sdmc:/switch/SwitchBOX/switchbox.ini`
2. `<nro 所在目录>/switchbox.ini`

如果两个位置都找不到：

- 不自动创建配置文件
- 直接提示安装不完整

## 项目结构

```text
SwitchBOX/
├─ app/            应用层与页面逻辑
├─ core/           配置、语言、播放目标、SMB/播放器核心能力
├─ platform/       Switch 平台适配
├─ resources/      内置资源
├─ langs/          外置语言资源
├─ library/        第三方库
├─ cmake/          工具链与构建配置
├─ tools/          辅助脚本
├─ .vscode/        VS Code 任务与工作区配置
└─ build/          本地构建输出
```

## 协作约定

- 助手负责本地代码修改与 `commit`
- 用户负责 `push`
- 提交信息默认使用中文
- 每次继续开发前，先读取 `AI记忆/switchbox-current-work-item.md`
