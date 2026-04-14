# SwitchBOX

## 本项目的目的

让 `Nintendo Switch` 可以像电视盒子一样使用，支持两类核心能力：

- 播放 `NAS / SMB` 共享视频
- 播放 `IPTV` 电视节目

注意：

- 请在 **大气层虚拟系统** 环境中使用，因为开发的时候只考虑了支持这种使用状况
- 请在 **大气层虚拟系统** 环境中使用，因为开发的时候只考虑了支持这种使用状况
- 请在 **大气层虚拟系统** 环境中使用，因为开发的时候只考虑了支持这种使用状况

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

截至 `2026-04-14`，项目已经从“仓库骨架 + 应用壳”阶段，推进到“设置系统、配置文件、多语言、来源条目管理”阶段。

当前仍保持以下总体方向：

- `Switch` 是唯一产品目标平台
- `Windows` 只是开发和调试目标
- 先把应用壳、设置、语言、来源配置打稳
- 再进入真实播放壳、IPTV 播放流和 SMB 浏览/播放流

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
- `Desktop` 调试目标已建立
- `Switch` 交叉编译配置已建立
- `Switch` 构建链路已切换到 `Ninja`
- 当前机器已成功生成 `.elf / .nacp / .nro`
- `Borealis` 已以 vendored 方式接入项目
- 应用首页主壳已经可运行
- 当前首页已经改为单行横向卡片流
- 当前首页卡片顺序规则为：
  - `播放历史`
  - 已启用的 `IPTV`
  - 已启用的 `SMB`
- `设置` 不再占用首页卡片位，继续由底部 `+` 进入
- `播放历史` 当前为需求占位卡片，后续统一承接 IPTV / SMB 的最近播放记录
- `Settings` 已从占位页升级为真实设置页
- 设置页已改为接近 Switch 原生的左右分列结构
- 左侧当前分类为：
  - `基础设置`
  - `IPTV`
  - `SMB`
- 右侧当前已支持：
  - 语言设置
  - IPTV 条目列表
  - SMB 条目列表
- IPTV / SMB 条目已支持：
  - 新增
  - 编辑
  - 删除确认
  - 快速启用 / 停用
- IPTV / SMB 的新增与编辑当前走独立编辑页，而不是在列表页内直接堆叠复杂表单
- 设置页的保存动作当前统一使用 `+`
- 设置页修改会先保留在草稿状态，按 `+` 后写入 `switchbox.ini`
- 设置页保存后会重建根界面，首页卡片会按最新配置立即刷新
- 底部 `A / 确定`、`B / 返回` 提示已纳入简体中文切换范围
- 已完成一轮保守清理，移除了无用日志和部分 `CMake` 探测残留目录
- `VS Code` 任务化构建已可直接使用
- `VS Code` 工作区终端 `UTF-8` 配置已修正

### 当前界面状态

当前程序还不是播放器主体，现阶段是“可启动、可切页、可改语言”的应用壳：

- 启动后进入 `HomeActivity`
- 首页是主入口，不再只是静态状态页
- 首页当前不再承担“说明文档式”信息展示，而是直接展示来源入口卡片
- `Settings` 已为真实页面
- 其他三个模块仍主要作为后续功能入口
- 桌面端用于快速调试 UI 和应用流程
- Switch 端已用于验证真实目标平台构建、打包和 `.nro` 产出链路
- 当前还没有进入真实播放页面，仍处于“设置先行、播放待接入”的阶段

### 当前 UI 约定

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

### 当前首页约定

当前首页默认按下面的方式继续推进：

- 首页布局为单行横向滚动卡片
- 首页数据来自当前已保存配置
- 第一张卡片固定为 `播放历史`
- 后续卡片先展开已启用的 `IPTV`，再展开已启用的 `SMB`
- 禁用条目不会出现在首页
- 当没有任何已启用来源时，首页显示“按 + 前往设置添加来源”的提示
- 当前卡片点击后仍主要进入占位页，后续再逐步替换成真实模块页

### 当前设置页交互约定

当前设置页按下面的方式继续推进：

- 左侧分类负责切换设置域
- 右侧列表负责展示当前分类下的项目
- `A` 用于进入字段、下拉框或条目编辑页
- `B` 用于返回或取消退出
- `+` 用于保存当前草稿并写入 `switchbox.ini`
- 普通设置保存后会刷新首页
- 在 `IPTV` / `SMB` 列表中：
  - 条目前的 `● / ○` 表示启用状态
  - `Y` 直接切换启用 / 停用
  - `X` 删除条目，并弹确认框
- 用户可见名称使用 `title`
- 内部配置键仍保留 `key`
- `key` 不应在普通设置 UI 中直接暴露给用户

### 配置与语言资源约定

Desktop 端当前按开发便利方式工作：

- 配置文件路径：
  - `build/desktop-vs/switchbox.ini`
- 外部语言目录：
  - `build/desktop-vs/langs/`
- 若缺少 `switchbox.ini`，桌面端允许自动生成默认文件

Switch 端当前按“安装包完整复制”方式工作：

- 安装时需要同时复制：
  - `.nro`
  - `switchbox.ini`
  - `langs/`
- Switch 端缺少配置时不自动创建 `switchbox.ini`

Switch 端配置查找顺序：

1. `sdmc:/switch/SwitchBOX/switchbox.ini`
2. `<nro 所在目录>/switchbox.ini`

如果两个位置都找不到：

- 不自动创建配置
- 启动缺失配置提示页
- 提示安装不完整

当前语言系统约定：

- 内置资源路径：
  - `resources/i18n/<locale>/`
- 外部资源路径：
  - `langs/<locale>/`
- 当前已提供：
  - `en-US`
  - `zh-Hans`
- 当前项目文案由 `switchbox.json` 维护
- 当前设置页会把语言写入 `switchbox.ini`
- 当前目标是“保存后立即应用当前界面语言”
- 这条即时应用链路仍在继续稳定

### 当前配置文件约定

`switchbox.ini` 当前采用以下结构：

- 基础设置使用 `[general]`
- IPTV 来源允许多个，分组名格式为 `[iptv-xxx]`
- SMB 来源允许多个，分组名格式为 `[smb-xxx]`

当前字段约定：

- `IPTV`
  - `title`
  - `enabled`
  - `url`
- `SMB`
  - `title`
  - `enabled`
  - `host`
  - `share`
  - `username`
  - `password`

补充约定：

- `SMB` 当前不设 `port`
- `SMB` 当前不再保留 `base_path`
- `title` 允许作为用户可见名称
- 实际分组名仍使用稳定的内部 `key`

### 当前已知问题

- 设置页“语言修改后立即刷新整个 UI”这条链路近期在桌面端出现过崩溃，仍需继续稳定
- 处理桌面端这类问题时，不要只看 `cmake --build` 返回码，还要确认 `.obj` 和 `.exe` 时间戳确实更新

### 下一阶段

接下来优先做以下内容：

1. 继续稳定设置页，优先收敛语言即时生效链路
2. 完成 IPTV / SMB 条目编辑与保存的稳定性验证
3. 将 `Playback Test` 升级为真实播放器 Activity 骨架
4. 给 `IPTV` 页面接入数据模型和列表壳
5. 给 `SMB` 页面接入连接设置和浏览壳
6. 在已打通的 `Desktop + Switch` 构建基础上推进播放状态管理

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

桌面端：

- `build/desktop-vs/app/switchbox_desktop.exe`

Switch 端：

- `build/switch/app/switchbox_switch.elf`
- `build/switch/app/switchbox_switch.nacp`
- `build/switch/app/switchbox_switch.nro`

## 当前项目结构

```text
SwitchBOX/
├─ app/            应用层与页面逻辑
├─ core/           配置、语言、构建信息等核心能力
├─ platform/       桌面 / Switch 平台适配
├─ resources/      内置资源
├─ langs/          外部语言资源
├─ library/        第三方库，目前包含 Borealis
├─ cmake/          工具链与构建配置
├─ .vscode/        VS Code 任务与工作区配置
└─ build/          本地构建输出目录
```

## 当前关键源码点

- 设置页：
  - `app/source/settings_activity.cpp`
- 配置读写：
  - `core/source/app_config.cpp`
- 语言解析：
  - `core/source/language.cpp`
- 应用启动与语言初始化：
  - `app/source/application.cpp`
- 语言切换后的 UI 重建入口：
  - `app/source/application.cpp`
- Borealis Activity 栈清理与 locale override 支持：
  - `library/borealis/library/lib/core/application.cpp`
- Borealis i18n 兼容修正：
  - `library/borealis/library/lib/core/i18n.cpp`

## 当前协作约定

- 代码修改与本地提交由助手负责
- 推送由用户自行完成
- 提交信息默认使用中文

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
