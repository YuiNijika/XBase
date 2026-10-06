# XBase Panel

> XBase 自带的通用网页面板。模组挂进来就行，不用写前端。

## 边界

这里只有**面板壳**，没有任何业务控件。界面内容全部由模组在运行时通过 `XBase::Panel::Mount` 挂进来；一个 ASI 对应一个 Sidebar 项，`pages` 在项内作为 Tab 展示。

| 层 | 位置 | 归谁 |
|---|---|---|
| 挂载 API | `XBase/include/XBase/Panel.h` | 模组调用 |
| 注册表 | `XBase{ver}.dll` 内的 `Panel.cpp` | 共享运行时，进程内唯一 |
| 桥接方法 | `panel.schema` / `get` / `set` / `run` / `hide` / `setSize` / `setPos` / `rect` | 共享运行时注册 |
| 前端 | `panel/dist` → `XBase\Library\panel\` | 随 XBase 分发 |

模组侧不写 HTML、不写 JS、不带资源。

## 构建

```bash
npm install
npm run build
```

产物落到 `dist/index.html` 与 `dist/data/`，由 XBase 的 `Build.bat Release` stage 到 `build\bin\Release\XBase\Library\panel\`，与共享运行时 DLL 一起归档在 `Library\` 下。

产物是 file 协议与虚拟主机都能加载的经典脚本：Vite 插件剥掉 `type="module"` 与 `crossorigin`，输出 iife，资源路径全部相对。改构建配置时这两条不能丢，否则网页视图里页面是白的。

## 技术栈

React 19 + Vite + TypeScript + Tailwind 4。不引组件库，开关、拖动条、下拉都用原生元素加 Tailwind 类，装依赖时少一大半包。

视觉与 XMenu 同源：`--accent-hue` 一个变量派生全部交互色，侧栏图标轨 + 内容区独立滚动 + 右下角拖拽改尺寸 + 标题栏拖拽移动，滚动条与 `prefers-reduced-motion` 的收尾照搬。

## 桥接协议

| 方法 | 参数 | 返回 |
|---|---|---|
| `panel.schema` | — | 全部挂载内容、游戏版本、上次打开的模组 |
| `panel.get` | `{ id }` | `{ ok, value }`，值是 double |
| `panel.set` | `{ id, value }` | `{ ok }` |
| `panel.run` | `{ id }` | `{ ok }` |
| `panel.hide` | — | `{ ok }` |
| `panel.setSize` / `panel.setPos` | `{ width, height }` / `{ x, y }` | `{ ok }` |
| `panel.rect` | — | `{ x, y, width, height }` |

原生往网页推事件 `panel.changed`，载荷 `{ id, value }`，模组自己在游戏里改了状态时用它刷新显示。

值的约定：开关是 0 与 1，数值直接读写，下拉读写的是 `options` 下标。C 接口上因此不必传任何 STL 容器，跨模块也就没有 CRT 堆不匹配的问题。

## 排查路径

| 现象 | 优先检查 |
|---|---|
| 面板打不开，日志说资源缺失 | `dist` 没构建，或没跑 XBase 的 `Build.bat Release` |
| 页面全白 | Vite 插件被改坏，`type="module"` 没剥掉 |
| 侧栏空 | 没有模组调 `Panel::Mount`，或模组挂在自己的静态库副本上（共享库没加载） |
| 开关显示与真实相反 | 模组没绑 `BindValue`，或初始值 `panel.get` 取不到 |
| 改了不生效 | 模组没每帧推送状态，被下一帧覆盖 |
