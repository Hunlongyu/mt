# Lucide 图标

2026-10-28 用户决定：界面全部图标使用 Lucide，后续新增图标也从 Lucide 选取；Lucide 没有合适图标时才用同风格的 Tabler Icons（MIT），并同样记录来源与许可（DEC-026）。不再使用手绘 QPainter 图标或来源不明的 SVG。

- 来源：npm 包 `lucide-static@0.460.0`（https://unpkg.com/lucide-static@0.460.0/icons/），文件原样保存，含 `@license lucide-static v0.460.0 - ISC` 头注释。
- 许可：ISC，原文见 LICENSE.txt，随资源嵌入 EXE。
- 风格：24×24 网格、2px 圆头圆角描边、`stroke="currentColor"`；运行时由 `Theme::icon` 按共享主题着色。
- 新增图标：放入本目录，在 CMakeLists.txt 的 `lucide_icons` 资源和 `Theme::iconAsset` 中登记，并更新下表。

| Symbol | Lucide | 用途 |
| --- | --- | --- |
| Menu | menu | 启动面板菜单 |
| Pin | pin | 启动面板保持打开 |
| PinToTop | arrow-up-to-line | 剪贴板置顶 |
| Close | x | 隐藏面板 |
| Search | search | 搜索 |
| More | ellipsis | 更多操作 |
| Text | text | 文本/富文本过滤、类型标记 |
| Image | image | 图片过滤、类型标记 |
| Files | file-text | 文件过滤、类型标记、启动项缺省图标 |
| Star | star | 收藏过滤、收藏 |
| Settings | settings | 设置 |
| Back | arrow-left | 退出搜索 |
| Trash | circle-x | 删除记录 |
| Pause | pause | 暂停记录 |
| Play | play | 恢复 |
| Sun / Moon | sun / moon | 主题 |
| Layers | layers | 托盘图标 |
| Copy | copy | 回贴 |
| Open | external-link | 打开链接或文件 |
| Help | circle-help | 快捷键说明 |
| ChevronLeft / ChevronRight | chevron-left / chevron-right | 启动面板分类栏左右滚动 |

## 设置界面新增图标（2026-10-29）

同一 lucide-static@0.460.0 来源，原样沿用已接受设置原型的资源。

| Symbol | Lucide |
| --- | --- |
| Palette | palette |
| Grid | layout-grid |
| Gesture | mouse-pointer-2 |
| MonitorOff | monitor-off |
| Info | info |
| Monitor | monitor |
| Plus | plus |
| Pencil | pencil |
| Delete | trash-2 |
| Folder | folder |
| Keyboard | keyboard |
| Shield | shield-check |
| Upload | upload |
| Check | check |
