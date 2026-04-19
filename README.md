# SwitchBOX

## 本项目的目的 / Project Goal

让 `Nintendo Switch` 可以像电视盒子一样使用，支持两类核心能力。  
SwitchBOX aims to make `Nintendo Switch` work like a set-top box, with two core capabilities.

- 播放 `SMB` 共享视频。  
  Play videos from `SMB` shares.
- 播放 `IPTV` 电视节目。  
  Play `IPTV` channels and programs.

注意 / Notes：

- 请在 **大气层虚拟系统** 环境中使用，因为当前开发与验证都只围绕该使用场景展开。  
  Please use it in an **Atmosphere emuMMC / virtual system** environment, because current development and validation are focused on that usage scenario only.
  
- 如遇问题，可在 issue 反馈，也可以加 QQ群：`1022585620` 找群主。  
  If you run into problems, you can open an issue or join the QQ group: `1022585620` and contact the group owner.

## 参考项目 & 感谢 / References & Thanks

IPTV 方向参考 / IPTV reference:

- `TsVitch`
- https://github.com/giovannimirulla/TsVitch

SMB / 本地媒体 / 流式播放实现路径参考 / SMB, local media, and streaming playback reference:

- `nxmp`
- https://github.com/proconsule/nxmp

UI 框架 / UI framework:

- `Borealis`
- https://github.com/natinusala/borealis

## 软件操作说明（当前版本） / Operation Guide (Current Version)

本章节用于说明当前可直接使用的操作，重点是播放器交互。  
This section describes the currently available controls, with focus on player interaction.

### 主页 / Home

- 左右切换卡片，`A` 进入当前卡片。  
  Move left or right between cards, and press `A` to enter the current card.
- 开启触控后，也可以直接点击主页卡片进入。  
  When touch is enabled, you can also tap a Home card directly to enter it.
- `+` 进入设置页。  
  Press `+` to open the settings page.
- `B` 退出软件，会弹出确认框。  
  Press `B` to exit the app, with a confirmation dialog.

### 设置页 / Settings

- 左侧切换分类，右侧编辑当前分类内容。  
  Switch categories on the left and edit the current category on the right.
- `A`：进入字段、打开下拉框、打开当前条目编辑。  
  `A`: enter a field, open a dropdown, or open the current entry editor.
- `+`：保存并立即应用当前修改，然后直接返回主页。  
  `+`: save and apply current changes immediately, then return to Home.
- `B`：退出设置；如果存在未保存修改，会弹出“取消 / 确定”确认框，默认焦点在取消。  
  `B`: leave Settings; if there are unsaved changes, a `Cancel / Confirm` dialog appears with default focus on Cancel.
- 在 `IPTV / SMB` 条目列表中，`Y`：快速启用 / 禁用当前条目。  
  In `IPTV / SMB` entry lists, `Y`: quickly enable or disable the current entry.
- 在 `IPTV / SMB` 条目列表中，`X`：删除当前条目，会弹出确认框。  
  In `IPTV / SMB` entry lists, `X`: delete the current entry, with a confirmation dialog.

### SMB 浏览页 / SMB Browser

- `A`：进入文件夹或播放当前文件。  
  `A`: enter a folder or play the current file.
- `B`：返回上一级，或退出当前 SMB 浏览页。  
  `B`: return to the parent directory, or leave the current SMB browser page.
- `X`：删除当前焦点条目，会弹出确认框。  
  `X`: delete the currently focused item, with a confirmation dialog.
- `Y`：直接返回主页。  
  `Y`: return directly to Home.

### IPTV 浏览页 / IPTV Browser

- 左侧为分组列表，右侧为当前分组下的频道列表。  
  The left side shows the group list, and the right side shows channels in the current group.
- 默认焦点在“收藏”分组。  
  Default focus starts on the `Favorites` group.
- 左右在分组区与频道区之间切换，上下在当前区域内移动焦点。  
  Left and right switch between the group pane and the channel pane; up and down move focus within the current pane.
- `A`：选中分组，或播放当前频道。  
  `A`: select a group, or play the current channel.
- `X`：收藏 / 取消收藏当前频道。  
  `X`: favorite or unfavorite the current channel.
- `B`：返回主页；如果列表已经加载完成，会弹出确认框。  
  `B`: return to Home; if the playlist has already loaded, a confirmation dialog will appear.
- `Y`：与 `B` 相同，也用于返回主页确认。  
  `Y`: same as `B`, also used for the return-to-Home confirmation flow.

### 播放器（SMB / IPTV 共用播放页） / Player (Shared by SMB and IPTV)

基础操作 / Basic Controls：

- `A`：播放 / 暂停。  
  `A`: play / pause.
- `B`：退出播放器，回到来源页面。  
  `B`: exit the player and return to the source page.
- `X`：仅对 `SMB` 文件有效；确认后退出播放器、删除文件并回到列表。  
  `X`: only valid for `SMB` files; after confirmation, exit the player, delete the file, and return to the list.
- `↑ / ↓`：调节音量，会显示音量浮窗。  
  `↑ / ↓`: adjust volume and show the volume OSD.

跳转与倍速 / Seeking and Speed：

- `L`：短退。  
  `L`: short backward seek.
- `R`：短进。  
  `R`: short forward seek.
- `ZL`：长退。  
  `ZL`: long backward seek.
- `ZR`：长进。  
  `ZR`: long forward seek.
- `Y`：点按开/关倍速；长按为临时倍速；若已点按开启，再长按会再叠加一次同值倍速。  
  `Y`: tap to toggle speed mode on or off; hold for temporary speed-up; if sticky speed is already on, holding `Y` adds one more same-speed step.
- `R + 左/右`：连续短退 / 连续短进。  
  `R + Left/Right`: continuous short backward / forward seek.
- `ZR + 左/右`：连续长退 / 连续长进。  
  `ZR + Left/Right`: continuous long backward / forward seek.

浮窗与面板 / Overlays and Panels：

- `十字键左 / 左摇杆左 / 右摇杆左`：打开左侧文件列表浮窗，再按一次关闭。  
  `D-Pad Left / Left Stick Left / Right Stick Left`: open the left file-list overlay; press again to close it.
- `+`：打开底部进度控制面板，再按 `+` 或 `B` 关闭。  
  `+`: open the bottom progress control panel; press `+` again or `B` to close it.
- 底部面板当前支持：旋转、长退、短退、播放/暂停、短进、长进、音轨、字幕。  
  The bottom panel currently supports: rotate, long backward seek, short backward seek, play/pause, short forward seek, long forward seek, audio track, and subtitle.

触控操作 / Touch Controls：

- 前提：设置中开启 `启用触控`。  
  Prerequisite: `Touch Controls` must be enabled in Settings.
- 播放器手势另外受 `播放器滑动控制` 开关控制。  
  Player gestures are additionally controlled by the `Player Gesture Control` switch.
- 双击屏幕：暂停当前播放。  
  Double-tap the screen: pause current playback.
- 横向滑动：按整段视频比例跳转，并自动打开底部进度控制面板。  
  Horizontal swipe: seek by the full-duration ratio and automatically open the bottom progress panel.
- 纵向滑动：调节音量。  
  Vertical swipe: adjust volume.
- 点击底部进度条：直接定位播放进度。  
  Tap the bottom progress bar: jump directly to the target position.
- 暂停时点击屏幕中央暂停图标：恢复播放。  
  When paused, tap the center pause icon on screen to resume playback.

## 当前项目状态

截至 `2026-04-18`，项目已调整为 **纯 Switch 项目**，桌面端构建、桌面端代码分支和相关脚本均已移除。

当前已完成：

- Switch 端可稳定构建、打包并生成 `.nro`
- 设置页可编辑基础设置、`IPTV` 条目、`SMB` 条目
- 语言资源支持外置 `.SwitchBOX-Data/Langs/`
- 首页已接入横向卡片布局
- 首页卡片已支持受全局触控开关控制的点击进入
- `IPTV` 浏览页可加载播放列表、按分组浏览、收藏频道、写回收藏状态并返回主页
- `IPTV` 首页加载链路已稳定：首页负责列表加载浮层，播放器加载上下文延后到真正进入播放页时创建
- `IPTV` 实播链路已基本打通，当前代表性 `HLS master/variant`、重定向 `HLS live/VOD`、敏感直链 `FLV` 测试源已可播放
- 当前已验证可播的代表样例包括：`人在囧途`、`JJ斗地主`、`漫画解说`、`电影功夫片`，分别覆盖 `HLS master/variant`、重定向 `HLS live`、重定向 `HLS VOD` 与敏感直链 `FLV`
- `SMB` 浏览页可列目录、进目录、返回上级、删除条目、返回主页
- `SMB` 文件已切换为纯流式播放并进入真实播放器页
- `SMB` 播放工作流已基本打通：从首页卡片进入、浏览、播放、控制、删除、返回列表均可用
- 共用播放器已接入暂停、退出、删除、跳转、倍速、音量、左侧浮窗、底部控制面板、音轨/字幕、画面旋转
- 播放器已接入触控总开关、播放器手势、点击进度条定位、暂停态点击恢复播放
- 偏好音轨 / 字幕语言已接入设置页并真实生效，支持 `中文 / 英文 / 自定义`
- 中文字幕显示已改为内置字体链路，不再运行时导出字体目录
- 已修正首播报错、高码率视频首帧黑屏、方向输入重复触发，以及多类 IPTV 首帧/重定向/流类型判定问题
- `sort_order` 已真实作用于 `SMB` 浏览页和播放器左侧浮窗
- 此前约定暴露到设置页的参数，当前都已完成真实接入

当前未完成：

- `IPTV` 更多源兼容性与质量优化（当前主线）
- 播放历史
- 断点续播
- `WebDAV`

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
- `build/releases/<version>/switch/.SwitchBOX-Data/Langs/`

当前约定是：

- 当需要“打包”时，统一输出到 `build/releases/<version>/switch/`
- 该目录下的内容可直接复制到设备的 `switch/SwitchBOX/` 目录

## 配置与安装约定

安装时需要一起复制：

- `.nro`
- `switchbox.ini`
- `.SwitchBOX-Data/Langs/`

当前 `switchbox.ini` 查找顺序：

1. `sdmc:/switch/SwitchBOX/switchbox.ini`
2. `<nro 所在目录>/switchbox.ini`

如果两个位置都找不到：

- 不自动创建配置文件
- 直接提示安装不完整

其他当前约定：

- `.SwitchBOX-Data/Langs/` 相对当前实际使用的 `switchbox.ini` 所在目录查找。
- 不再兼容旧的 `langs/` 目录名；安装包仅保留 `.SwitchBOX-Data/Langs/`。
- 如果 `[general]` 缺少基础参数，程序会按缺项补齐默认值与双语注释，不覆盖已有值。
- 当前配置分组约定：
  - 基础设置使用 `[general]`
  - IPTV 条目使用 `[iptv-xxx]`
  - SMB 条目使用 `[smb-xxx]`

## 项目结构

```text
SwitchBOX/
├─ app/            应用层与页面逻辑
├─ core/           配置、语言、播放目标、SMB/播放器核心能力
├─ platform/       Switch 平台适配
├─ resources/      内置资源（含字幕字体）
├─ .SwitchBOX-Data/
│  └─ Langs/ 外置语言资源
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
