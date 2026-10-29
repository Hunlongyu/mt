# 原型图标资源

- 全部来源：`lucide-static@0.460.0`，固定版本目录 `https://unpkg.com/lucide-static@0.460.0/icons/`。
- 原型直接读取工程共用的 `resources/icons/lucide/`，不再保留重复图标副本。
- 原始 SVG 保留许可证头，不手绘或修改路径；构建时仅补充装饰图标的可访问属性。
- ISC 完整许可：`resources/icons/lucide/LICENSE.txt`，亦嵌入 HTML 的“关于 MT”页。
- 文件名、来源与 SHA-256：`icon-manifest.json`，由 `build.mjs` 生成。
- 此目录只服务设置原型，不会自动注册到 Qt 资源或 CMake。
