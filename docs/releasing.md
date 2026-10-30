# 构建与发布

用户于 2026-10-30 授权发布首个版本 v0.1.0，随后明确要求发布包含本轮修复的 v0.1.1。后续版本仍需明确发布指令；普通提交与推送不等于发布新版本。

## 工作流

GitHub Actions 仅在推送匹配 `v*.*.*` 的版本标签时触发；普通分支推送、PR 和手动入口均不启动工作流。检出源码后，先校验三段版本号为无前导零的非负整数，并与 CMake 项目版本一致；保留下文约定的预发布后缀支持。标签格式或版本不符时，在下载 SDK 和编译前终止。

`.github/workflows/windows.yml` 使用 Windows 2022 / MSVC 2022，下载 [yuanpeirong/buildQt 的 Qt6.11.2_rev0](https://github.com/yuanpeirong/buildQt/releases/tag/Qt6.11.2_rev0) 中的 `Qt_6.11.2-static-Release_msvc2022_64_RP.7z`。它以 `-static -static-runtime` 构建，TLS 使用系统 Schannel。RP 包含 Base、Tools、Translations、SVG，满足当前依赖；Tools 仅为构建工具，不作为应用运行组件分发。

版本、资产地址、SDK SHA-256 和官方对应源码的 SHA-256 均固定在 `scripts/dependencies.json`。CI 按清单与下载脚本哈希缓存，不使用浮动 latest；升级 SDK 时需同步清单、中文翻译、许可说明，并重跑构建与隔离验证。预编译包由第三方维护，不是 Qt 官方二进制；`scripts/build-qt.ps1` 提供官方源码构建与修改后重新链接的替代路径。

随后构建 MT、执行 CTest、重建 HTML 原型、检查独立目录预览、更新替换和失败保护，打包并保存 14 天。构建和验证成功后，才进入具有 `contents: write` 权限的 Release 任务。

## 版本与资产

版本唯一来源是 `CMakeLists.txt` 中的 `project(mt VERSION X.Y.Z)`。正式标签为对应的 `vX.Y.Z`；带 `-beta.N` 等后缀的标签发布为 prerelease，不进入稳定更新通道。禁止覆盖已有标签与资产。

- `mt.exe`：固定程序文件名，供直接下载和更新使用，不追加版本、平台或架构后缀。Actions 的 `mt-windows-x64` 是产物集合名，其中的程序同样为 `mt.exe`。
- `mt-X.Y.Z-windows-x64.zip`：EXE 与许可证。
- `mt-X.Y.Z-source-with-dependencies.zip`：应用源码、固定 Qt 与 WIL 源码、许可证和重建脚本。
- `SHA256SUMS.txt`：资产校验清单；应用直接核验 GitHub API 的资产 digest。

此命名从后续构建开始生效。已发布 v0.1.0 的程序名仍为 `mt-windows-x64.exe`，其更新检查也只识别旧名；用户需手动下载首个采用新文件名的版本一次，之后按 `mt.exe` 继续自动更新。不修改既有 Release 资产。

确认功能范围与本地相关验证后，在 `docs/releases/vX.Y.Z.md` 编写中文发布说明。将下例中的版本替换为 CMake 项目中的未发布版本，确认本地与远端不存在同名标签，再推送本次单个标签：

```powershell
$releaseTag = 'vX.Y.Z'
git tag $releaseTag
git push origin $releaseTag
```

标签工作流重新构建和验证该提交，成功后创建 Release 并上传资产。优先采用对应版本的发布说明；没有说明文件时才生成 GitHub 默认说明。发布后核对标签提交、资产 SHA-256、EXE 版本、源码材料和下载可用性，不覆盖已有版本。

## 使用修改后的 Qt 重建

安装支持 C++23 的 MSVC、Windows SDK、CMake 3.29+、Ninja、Python 3.12+、PowerShell 7，将源码材料解压到短路径，例如 `C:/mt-source`。发布打包同样需要 Python 3.12+，使用标准库流式读取 Qt 源码归档并提取许可，不依赖宿主的 xz 解码器。

```powershell
./scripts/build-qt.ps1 -InstallRoot C:/mt-deps/qt -WorkRoot C:/mt-deps/build -ArchiveRoot ./dependencies
./scripts/build.ps1 -QtRoot C:/mt-deps/qt
```

首次构建在 `WorkRoot` 下展开 Qt。可修改展开的 `qtbase-everywhere-src-6.11.2` 或 `qtsvg-everywhere-src-6.11.2` 后重跑脚本；脚本保留源码修改。存在 `third_party/wil` 时，CMake 使用本地依赖，不获取 WIL。源码构建脚本裁掉 MT 不使用的 OpenGL 和 SQL 驱动，目标是可重建、可重新链接，不承诺与预编译 SDK 逐字节一致。

提供完整应用源码，支持重新编译和链接，无需闭源对象文件。生成的 EXE 可直接运行，不要求官方签名。

## 发布前检查

- CI 编译、单测、原型、隔离启动与打包均通过。
- 标签、CMake 版本、EXE 版本和说明一致。
- 附带 Qt、WIL、Lucide 及内置第三方许可与完整对应源码。
- 在独立数据目录验证升级，确认配置与数据库保留、失败可恢复。
- 列出未验证的系统、硬件、权限和多屏范围；CI 不替代桌面或跨机器验收。
