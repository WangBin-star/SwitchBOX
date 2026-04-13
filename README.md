# SwitchBOX

## 2026-04-13 UI约定补充

以下界面约定当前已经确认，后续默认按此继续开发：

- 顶部保留 `Borealis AppletFrame` 标题栏，不再改成自定义整页头部
- 底部保留 `Borealis` 按键提示栏，不再移除
- 底部原生时间、WiFi、电量显示继续隐藏
- 右上角使用自定义状态区显示时间和 WiFi 状态
- 当前右上角时间格式为 `HH:MM:SS`
- 当前右上角时间字号为 `14`
- 当前首页底部 `+` 按键提示保留，提示文字为 `设置`

当前实现约定：

- 首页的 `+ = 设置` 动作注册在首页根 `AppletFrame`
- 全局 `BUTTON_START = 退出` 已关闭，避免覆盖首页底部的 `设置` 提示

## 本项目的目的

让 `Nintendo Switch` 可以像电视盒子一样使用，支持两类核心能力：

- 播放 `NAS / SMB` 共享视频
- 播放 `IPTV` 电视节目

注意：

- 本项目是 `Switch-first`，`Windows` 仅作为开发和调试目标
- 当前阶段优先打通应用壳、构建链路和播放链路
- 项目默认面向已具备基础 Homebrew 环境的 Switch 设备

## 参考项目 & 感谢

IPTV 方向参考：

- `TsVitch`
- https://github.com/giovannimirulla/TsVitch

SMB / 本地媒体方向参考：

- `nxmp`
- https://github.com/proconsule/nxmp

UI 框架：

- `Borealis`
- https://github.com/natinusala/borealis

## 项目说明

### 当前项目目标

当前开发按下面顺序推进：

1. 搭建仓库骨架
2. 打通桌面调试构建
3. 打通 Switch 构建并生成 `.nro`
4. 接入应用壳和 UI 框架
5. 搭建播放入口与页面结构
6. 先完成 IPTV 验证，再推进 SMB / NAS

### 当前已完成

- 基础 `CMake` 多模块工程骨架
- `Desktop` 调试目标
- `Switch` 交叉编译配置
- `Switch` 构建链路切换到 `Ninja`
- `.nro` 生成链路已在当前机器复验通过
- `Borealis` 以 vendored 方式接入项目
- `resources/` 资源目录接入构建
- `HomeActivity` 已升级为主入口壳
- 已加入 `IPTV / SMB / Playback Test / Settings` 四个模块入口
- 已加入可复用的占位页面 `PlaceholderActivity`
- 桌面端应用壳启动与页面展示
- Switch 端 `ELF / NACP / NRO` 产物已成功生成
- VS Code 任务化构建
- VS Code 工作区终端 `UTF-8` 配置修正

### 当前界面状态

当前程序还不是播放器主体，现阶段是一个可启动的应用壳：

- 启动后进入 `HomeActivity`
- 首页现在是主入口，不再只是静态状态页
- 当前提供 4 个入口：`IPTV`、`SMB / NAS`、`Playback Test`、`Settings`
- 每个入口都会进入对应的占位 Activity，供后续逐步替换为真实功能
- 桌面端用于快速调试 UI 和应用流程
- Switch 端已用于验证真实目标平台构建、打包和 `.nro` 产出链路

### 下一阶段

接下来优先做以下内容：

1. 将 `Playback Test` 升级为真实播放器 Activity 骨架
2. 给 `IPTV` 页面接入数据模型和列表壳
3. 给 `SMB / NAS` 页面接入连接设置和浏览壳
4. 在已打通的 `Desktop + Switch` 构建基础上推进播放状态管理

## 开发环境

### 推荐环境

- `Windows`
- `VS Code`
- `Visual Studio 2026 Community`
- `Python 3`
- `devkitPro`

其中：

- `Visual Studio 2026` 需要安装 `使用 C++ 的桌面开发` 工作负载
- `VS Code` 是日常编辑器
- `Visual Studio` 主要提供 `MSVC`、`CMake`、`Ninja` 等桌面构建工具

### 必需环境变量

Windows 系统环境变量建议配置为：

```text
DEVKITPRO=C:\devkitPro
DEVKITA64=C:\devkitPro\devkitA64
```

验证命令：

```powershell
python --version
echo $env:DEVKITPRO
echo $env:DEVKITA64
```

期望结果：

- `python` 能输出版本号
- `DEVKITPRO` 指向 `C:\devkitPro`
- `DEVKITA64` 指向 `C:\devkitPro\devkitA64`

### 当前必需的 devkitPro 包

当前仓库的 Switch 构建已经依赖下面两个包：

- `switch-glm`
- `ninja`

安装命令：

```powershell
C:\devkitPro\msys2\usr\bin\pacman.exe -S switch-glm ninja
```

验证命令：

```powershell
C:\devkitPro\msys2\usr\bin\pacman.exe -Q switch-glm
C:\devkitPro\msys2\usr\bin\ninja.exe --version
```

### 环境说明

- `Desktop` 构建使用 `Visual Studio` 自带的 `cmake` / `ninja`
- `Switch` 构建使用 `devkitPro MSYS2` 自带的 `cmake`，并配合 `devkitPro MSYS2` 里的 `ninja`
- VS Code 任务已经处理了这两套环境差异，日常开发优先直接使用任务
- 当前仓库路径包含中文目录名，`Switch` 构建已经避开 `Unix Makefiles`
- `.nro` 打包阶段会先把 `resources/` 同步到 `C:\devkitPro\tmp\switchbox-switch-romfs`
- 不要随意把 Switch 生成器改回 `Unix Makefiles`，否则很容易重新遇到路径解析问题

## 现在可以直接使用的命令

### 方式一：VS Code 任务

在 `VS Code` 中执行 `Tasks: Run Task`，可以直接使用：

- `switchbox: configure desktop`
- `switchbox: build desktop`
- `switchbox: run desktop`
- `switchbox: configure switch`
- `switchbox: build switch`

最常用的是：

- 桌面编译：`switchbox: build desktop`
- 桌面运行：`switchbox: run desktop`
- Switch 编译：`switchbox: build switch`

### 方式二：PowerShell 直接执行

先进入项目目录：

```powershell
cd "D:\Github项目\SwitchBOX\SwitchBOX"
```

桌面目标配置：

```powershell
cmd /d /s /c "\"C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat\" >nul && \"C:\Program Files\Microsoft Visual Studio\18\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe\" -S . -B build\desktop-vs -G Ninja -DCMAKE_MAKE_PROGRAM=\"C:\Program Files\Microsoft Visual Studio\18\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja\ninja.exe\" -DSWITCHBOX_BUILD_DESKTOP=ON -DSWITCHBOX_BUILD_SWITCH=OFF"
```

桌面目标编译：

```powershell
cmd /d /s /c "\"C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat\" >nul && \"C:\Program Files\Microsoft Visual Studio\18\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe\" --build build\desktop-vs"
```

补充说明：

- 手动执行桌面端构建时，必须带上 `vcvars64.bat` 这层开发环境
- 如果直接在普通 PowerShell 里运行 `cmake --build build/desktop-vs`，可能会出现找不到 `string`、`algorithm` 等标准库头文件的情况
- 日常最稳的方式仍然是优先使用 `VS Code` 任务：`switchbox: build desktop`

桌面目标运行：

```powershell
.\build\desktop-vs\app\switchbox_desktop.exe
```

Switch 目标配置：

```powershell
$env:Path = "C:\devkitPro\msys2\usr\bin;" + $env:Path
$env:DEVKITPRO = "/opt/devkitpro"
$env:DEVKITA64 = "/opt/devkitpro/devkitA64"
C:\devkitPro\msys2\usr\bin\cmake.exe --% -S . -B build/switch -G Ninja -DCMAKE_MAKE_PROGRAM=C:/devkitPro/msys2/usr/bin/ninja.exe -DCMAKE_TOOLCHAIN_FILE:FILEPATH=D:/Github项目/SwitchBOX/SwitchBOX/cmake/toolchains/switch.cmake -DSWITCHBOX_BUILD_DESKTOP=OFF -DSWITCHBOX_BUILD_SWITCH=ON
```

Switch 目标编译：

```powershell
cd .\build\switch
$env:Path = "C:\devkitPro\msys2\usr\bin;" + $env:Path
$env:DEVKITPRO = "/opt/devkitpro"
$env:DEVKITA64 = "/opt/devkitpro/devkitA64"
C:\devkitPro\msys2\usr\bin\cmake.exe --build .
```

## 当前产物位置

桌面可执行文件：

- `build/desktop-vs/app/switchbox_desktop.exe`

当前桌面端最新构建也已经确认成功：

- `build/desktop-vs/app/switchbox_desktop.exe`

Switch 构建成功后应生成：

- `build/switch/app/switchbox_switch.elf`
- `build/switch/app/switchbox_switch.nacp`
- `build/switch/app/switchbox_switch.nro`

当前已经确认生成成功：

- `build/switch/app/switchbox_switch.elf`
- `build/switch/app/switchbox_switch.nacp`
- `build/switch/app/switchbox_switch.nro`

## 当前项目结构

```text
SwitchBOX/
├─ app/            应用层与页面逻辑
├─ core/           通用核心信息
├─ platform/       桌面 / Switch 平台适配
├─ resources/      字体、图片、i18n 等资源
├─ library/        第三方库，目前包含 Borealis
├─ cmake/          工具链与构建配置
├─ .vscode/        VS Code 任务与工作区配置
└─ build/          本地构建输出目录
```

## 终端乱码说明

如果在 `VS Code` 终端里看到 `README.md` 中文乱码，先确认以下几点：

- 当前工作区已经配置 `UTF-8` 终端启动参数
- 需要关闭旧终端后重新打开新终端
- 建议在本工作区内使用默认的 `PowerShell UTF-8` 终端配置

可用下面的命令验证：

```powershell
chcp
[Console]::InputEncoding.WebName
[Console]::OutputEncoding.WebName
Get-Content README.md -Encoding utf8 -Head 10
```

期望结果：

- `chcp` 输出 `65001`
- 编码输出 `utf-8`
- `README.md` 中文正常显示
