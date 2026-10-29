# 第三方组件声明

MT 自有源码使用 MIT 许可证。以下组件保留原许可。

| 组件 | 固定来源 | 许可 / 用途 |
| --- | --- | --- |
| Qt Base、Qt SVG | Qt 6.11.2，官方源码归档与 SHA-256 见 `scripts/dependencies.json` | LGPL-3.0；部分文件另有宽松许可；Core、Gui、Widgets、Sql、Svg、Network、Windows 插件和 Schannel TLS |
| Qt 中文翻译 | Qt 6.11.2 `qtbase_zh_CN.qm`，上游 qttranslations | 依上游文件许可；源翻译和许可随重建材料提供 |
| Lucide | lucide-static 0.460.0 | ISC；`resources/icons/lucide/LICENSE.txt` |
| WIL | microsoft/wil，`a1c0cbe1d7065aef6333399d3e25608eacdffc0f` | MIT；Windows 句柄与 COM 的 RAII |
| Qt 内置第三方代码 | 随固定 Qt 源码提供 | SQLite、zlib、PCRE2、FreeType、HarfBuzz、libpng、libjpeg 等；版权与许可保留在对应源码目录 |

本程序静态链接 LGPL 版本 Qt。可以修改 Qt、重新编译或重新链接 MT，并运行生成的 EXE；不限制为调试这些修改所需的逆向工程。没有签名验证阻止运行用户重建的版本。自动更新校验仅用于官方 Release 下载，可关闭自动检查。

CI 的静态 SDK 来自 [yuanpeirong/buildQt · Qt6.11.2_rev0](https://github.com/yuanpeirong/buildQt/releases/tag/Qt6.11.2_rev0) 的 MSVC 2022 x64 RP 包，以固定 SHA-256 校验。项目没有复制上游构建脚本；Qt 库仍按其自身许可使用。SDK 中的 Qt Tools 只用于构建，不作为运行依赖分发。

每次 Release 提供 `source-with-dependencies.zip`，包含该版本 MT 源码、使用的 Qt 源码归档、WIL 源码、许可证和构建脚本。按 `docs/releasing.md` 重建，不要求购买 Qt 商业许可。LGPL-3.0 和 GPL-3.0 文本位于 `resources/licenses/`，应用“关于 MT”也提供许可入口。

便携包的 `licenses/qt/` 附带对应 Qt 源码中的许可、版权和第三方声明文件；其中也可能包含未启用组件的声明，不表示它们全部链接进 MT。

官方参考：[Qt 开源许可义务](https://www.qt.io/development/open-source-lgpl-obligations)、[Qt 6.11 许可](https://doc.qt.io/qt-6.11/licensing.html)。
