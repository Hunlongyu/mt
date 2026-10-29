# MT

[![Windows 构建](https://github.com/Hunlongyu/mt/actions/workflows/windows.yml/badge.svg)](https://github.com/Hunlongyu/mt/actions/workflows/windows.yml)
[![MIT License](https://img.shields.io/badge/license-MIT-blue.svg)](LICENSE)

**MT（my tool）** 将启动面板、剪贴板历史、鼠标手势和黑屏遮挡整合到一个 Windows 托盘入口。使用 C++23、Qt 6 Widgets 和 CMake，目标是静态 Qt、静态 CRT、单个 `mt.exe`。

目前处于开发阶段，尚未发布首个 Release。功能缺项与验证边界见 [开发状态](docs/development.md)。

品牌名固定为 **MT**，程序名为 `mt.exe`。正式 Logo 为 [`resources/logo.ico`](resources/logo.ico)，用于 EXE、应用窗口和托盘；功能按钮继续使用 Lucide 图标。

## 功能

| 功能 | 当前能力 |
| --- | --- |
| 启动面板 | 分类、固定槽位、搜索、拖放、快捷键、边缘唤起和使用排序；默认 4 列 × 7 行，可在设置中调整 |
| 剪贴板 | 文本、图片和文件历史，搜索、过滤、收藏、置顶、预览、回贴和纯文本粘贴 |
| 鼠标手势 | 轨迹识别、按键与窗口操作、程序规则、轨迹与动作名称显示 |
| 黑屏遮挡 | 覆盖全部显示器、简单或密码解锁、暂停；不代替 Windows 账户锁定 |
| 统一设置 | 纯中文、浅色/深色/跟随系统、面板不透明度、独立功能开关 |
| 软件更新 | 后台检查 GitHub 正式版本，下载并校验，确认后替换程序并重启 |

当前开发验证目标为 **Windows 11 x64**。跨机器部署、管理员窗口和完整多屏矩阵仍在验证。鼠标手势与黑屏遮挡默认关闭；开机启动接管暂未开放。

## 使用

首个版本发布后的下载入口为 [Releases](https://github.com/Hunlongyu/mt/releases)。目前可自行构建，或从通过的 [Actions 运行](https://github.com/Hunlongyu/mt/actions) 下载 `mt-windows-x64` 产物。

运行 `mt.exe`，通过系统托盘打开功能和设置。默认快捷键：

| 快捷键 | 功能 |
| --- | --- |
| `Alt+Space` | 启动面板 |
| `Alt+C` | 剪贴板 |
| `Alt+L` | 黑屏遮挡，需先启用 |

配置和历史数据独立于 EXE。当前开发身份的默认目录是 `%LOCALAPPDATA%\HunlongyuDevelopment\mt`；设置 → 常规显示实际目录。可指定独立目录：

```powershell
./mt.exe --data-dir D:/MT-data
./mt.exe --settings
```

设置 → 关于 MT 提供检查、下载、取消与安装更新。自动检查在启动约 30 秒后及每 24 小时进行，仅接收更新的正式 Release。安装前提示重启，并保留原数据目录。详见 [更新设计](docs/updating.md)。

## 本地构建

需要 Visual Studio 2022 或更新版本的 C++ 工具链、Windows SDK、CMake 3.29+、Ninja、PowerShell 7 和静态 Qt SDK。CI 固定使用 **Qt 6.11.2**；MSVC 通过 `vswhere -prerelease` 自动发现。

下载与 CI 相同的静态 SDK（[yuanpeirong/buildQt · Qt6.11.2_rev0](https://github.com/yuanpeirong/buildQt/releases/tag/Qt6.11.2_rev0)）：

```powershell
./scripts/get-qt.ps1
./scripts/build.ps1 -QtRoot C:/mt-deps/sdk/6.11.2-static/msvc2022_64_RP
```

脚本固定下载 `Qt_6.11.2-static-Release_msvc2022_64_RP.7z`，核验 SHA-256，并获取相同版本的官方源码归档用于发布材料。RP 精简包包含 Qt Base、Tools、Translations、SVG，满足当前组件需要；不自动追踪上游 latest。

已有静态 SDK 时，直接把其目录传给 `-QtRoot`。也可从校验过的官方源码自行构建：

```powershell
./scripts/build-qt.ps1 -InstallRoot C:/mt-deps/qt
./scripts/build.ps1 -QtRoot C:/mt-deps/qt
```

产物位于 `out/build/win-static-release/mt.exe`。默认执行 CTest；`-SkipTests` 仅跳过测试运行，`-Fresh` 重新探测编译器。

独立示例预览：

```powershell
./scripts/preview.ps1
./scripts/preview.ps1 -Settings
./scripts/verify-preview.ps1
./scripts/verify-update.ps1
```

`--preview` 使用示例数据，不安装输入钩子、不监听剪贴板、不启动外部程序、不联网检查更新。`verify-update.ps1` 只在临时目录中替换程序副本，并以预览模式重启，验证成功、损坏文件、占用失败和取消安装。原生输入与黑屏测试需单独显式执行，普通 CTest 不会锁住桌面。

## 项目结构

```text
src/app/                    应用装配与生命周期
src/modules/                四项内置功能
src/infrastructure/         设置、存储、更新与中文文本
src/platform/windows/       Windows 输入、窗口、Shell 与更新助手
src/ui/                     共享主题、控件和设置窗口
resources/                  统一主题、图标、翻译和许可证
tests/                      单测与显式开启的桌面集成测试
scripts/                    SDK、应用构建、验证和发布打包
.github/workflows/          Windows CI 与标签发布
prototypes/                 可重建的 HTML 界面原型
docs/                       更新、发布、开发契约和验证记录
```

原型通过 `node prototypes/desktop/build.mjs` 和 `node prototypes/settings/build.mjs` 生成。原生与原型共用 `resources/theme/theme.json` 和 `resources/icons/lucide/`。

## 发布与许可

主分支和 PR 构建、测试并保存产物。只有版本标签触发 Release，带后缀标签产生预发布版，手动运行只构建。发布包含 EXE、便携 ZIP、SHA-256 清单与完整依赖源码材料，见 [发布说明](docs/releasing.md)。

自有源码使用 [MIT](LICENSE)。Qt、Lucide、WIL 等保留各自许可，见 [第三方声明](THIRD_PARTY_NOTICES.md)。静态 Qt 发布提供库源码、许可证与重建说明；用户可以修改 Qt 并运行重新链接后的程序。
