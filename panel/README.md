# XBase Panel

> XBase 自带的通用网页面板。模组挂进来就行，不用写前端。

## 边界

这里只有**面板壳**，没有任何业务控件。界面内容全部由模组在运行时通过 `XBase::Panel::Mount` 挂进来；一个 ASI 对应一个 Sidebar 项，`pages` 在项内作为 Tab 展示。

| 层 | 位置 | 归谁 |
|---|---|---|
| 挂载 API | `XBase/include/XBase/Panel.h` | 模组调用 |
| 注册表 | `XBase{ver}.dll` 内的 `Panel.cpp` | 共享运行时，进程内唯一 |
| 桥接方法 | `panel.schema` / `get` / `set` / `getText` / `setText` / `run` / `hide` / `setSize` / `setPos` / `rect` | 共享运行时注册 |
| 前端 | `panel/dist` → `XBase\Library\panel\` | 随 XBase 分发 |

默认模式下模组侧不写 HTML、不写 JS、不带资源；需要完全自定义时可使用 `custom` 控件提供 HTML/CSS/JS。Custom 同时支持内联内容和独立文件，二者可以混用。

## 构建

```bash
npm install
npm run build
```

产物落到 `dist/index.html` 与 `dist/data/`，由 XBase 的 `Build.bat Release` stage 到 `build\bin\Release\XBase\Library\panel\`，与共享运行时 DLL 一起归档在 `Library\` 下。

产物是 file 协议与虚拟主机都能加载的经典脚本：Vite 插件剥掉 `type="module"` 与 `crossorigin`，输出 iife，资源路径全部相对。改构建配置时这两条不能丢，否则网页视图里页面是白的。

## 技术栈

React 19 + Vite + TypeScript + Tailwind 4。官方 shadcn/ui 组件位于 `src/components/ui`，通过官方 CLI 安装，适配逻辑位于 `src/components/component-tree`，不要直接修改官方组件文件。

已安装官方 registry 的 63 个 UI 项，包括 Radix 通用组件、React Aria 专属的 Attachment / Bubble / Marker / Message / Message Scroller / Questionnaire，以及 Base UI Toast。组件公开导出自动加入声明式 registry，不只支持每个文件的主组件，也支持它的 Trigger、Content、Item 等组合部件。

视觉与 XMenu 同源：`--accent-hue` 一个变量派生全部交互色，侧栏图标轨 + 内容区独立滚动 + 右下角拖拽改尺寸 + 标题栏拖拽移动，滚动条与 `prefers-reduced-motion` 的收尾照搬。

## 桥接协议

| 方法 | 参数 | 返回 |
|---|---|---|
| `panel.schema` | — | 全部挂载内容、游戏版本、上次打开的模组 |
| `panel.get` | `{ id }` | `{ ok, value }`，值是 double |
| `panel.set` | `{ id, value }` | `{ ok }` |
| `panel.getText` | `{ id }` | `{ ok, value }`，值是 string |
| `panel.setText` | `{ id, value }` | `{ ok }` |
| `panel.run` | `{ id }` | `{ ok }` |
| `panel.hide` | — | `{ ok }` |
| `panel.setSize` / `panel.setPos` | `{ width, height }` / `{ x, y }` | `{ ok }` |
| `panel.rect` | — | `{ x, y, width, height }` |

原生往网页推事件 `panel.changed`，载荷 `{ id, value }`，模组自己在游戏里改了状态时用它刷新显示。
文本控件对应事件是 `panel.textChanged`。

值的约定：开关是 0 与 1，数值直接读写，下拉读写的是 `options` 下标。C 接口上因此不必传任何 STL 容器，跨模块也就没有 CRT 堆不匹配的问题。

## 控件类型

除 `toggle`、`float`、`int`、`select`、`action` 外，还支持：

- `text`：单行文本，配合 `Panel::BindText` 使用；`readOnly=true` 时只展示。
- `textarea`：多行文本，失焦或回车时提交。
- `color`：颜色选择器，值按 `0xRRGGBB` 的数字传递。
- `progress`：只读进度条，使用 `min` / `max` / `format`。
- `radio`：单选组，值是 `options` 的下标。
- `multiselect`：复选组，值是按选项下标编码的 bitmask。
- `heading` / `separator`：纯布局控件，不需要绑定。
- `custom`：直接渲染模组提供的 `html`、`style`，并执行 `script`。也可以用 `htmlFile`、`styleFile`、`scriptFile` 从模组目录加载独立资源。
- `component`：通过 `Control::component` 挂载可嵌套的 shadcn/ui 组件树。

### shadcn/ui 组件树

公共入口是 `ComponentNode` 与 `ComponentBinding`，原来的十四种控件保持兼容。新组件仍然通过原有 JSON ABI 跨模块传递，不跨模块传递 React 对象或 STL 内存。

| 字段 | 内容 |
|---|---|
| `component` | 官方公开导出名，例如 `Button`、`DialogTrigger`、`TabsContent` |
| `text` | 子节点中的纯文本 |
| `props` | `Json::Value` 对象，传递 `variant`、`className`、数据等可序列化属性 |
| `children` | 有序子节点；`asChild` 使用单个实际 React 元素，不增加包装层 |
| `slots` | React 元素属性，例如 ChartTooltip 的 `content` |
| `templates` | 函数属性模板，例如 ComboboxList 的 `children` 或 AttachmentTrigger 的 `render` |
| `textPath` | 从模板首个参数取显示文本，点分隔路径；`$value` 表示参数本身 |
| `bindings` | 属性状态与事件到宿主回调的映射 |

绑定字段为 `controlId`、`property`、`event`、`kind`。节点声明的绑定 ID 会自动登记，挂载成功后再绑定宿主回调即可。

| `ComponentBindingKind` | 宿主绑定 | 行为 |
|---|---|---|
| `Value` | `BindValue` | 数值或布尔值；单值 Slider 自动包装为数组 |
| `Text` | `BindText` | 字符串；适合 Input、Select、Tabs、日期 |
| `Action` | `BindAction` | 事件只触发动作，`property` 留空 |
| `Json` | `BindText` | 状态文本解析为 JSON，事件参数序列化为 JSON；适合多选、日期范围、图表数据和表单提交 |

```cpp
XBase::Panel::Control control;
control.kind = XBase::Panel::ControlKind::Component;
control.id = "MyMod.settings.switch";
control.component.component = "Switch";
control.component.props.Set("aria-label", "启用功能");
control.component.bindings.push_back({
    "MyMod.enabled",
    "checked",
    "onCheckedChange",
    XBase::Panel::ComponentBindingKind::Value
});
section.controls.push_back(control);

// 页面与分区先加入模组结构 挂载成功后才能绑定
if (XBase::Panel::Mount(spec)) {
    XBase::Panel::BindValue(
        "MyMod.enabled",
        [] { return enabled ? 1.0 : 0.0; },
        [](double value) { enabled = value != 0.0; });
}
```

Dialog 使用 Dialog / DialogTrigger / DialogContent / DialogTitle / DialogDescription；Tabs、菜单、Sidebar、Resizable 等同样遵守官方组件的父子关系。必须提供需要的 Provider 和无障碍标签。TooltipProvider 已由每棵组件树提供。

### 组合适配

| 名称 | 属性与事件 |
|---|---|
| `DatePicker` | `value` 为本地日期字符串 `YYYY-MM-DD`，`onValueChange` 配合 Text 绑定 |
| `DataTable` | `columns` 为 `{ key, label }[]`，`data` 为对象数组，支持排序、筛选、分页，`onRowClick` 可配合 Json 绑定 |
| `Form` | `defaultValues`、`disabled`、`onSubmit`；提交事件配合 Json 绑定 |
| `FormField` | `name`、可序列化 `rules` 和子节点；自动给 Input / Textarea / Select / RadioGroup / Checkbox / Switch 接入表单状态 |
| `ToastButton` | `title`、`description`、`variant`、`engine`，点击显示通知 |
| `SonnerToaster` / `Toaster` | Sonner 通知容器，面板已挂载一份，通常无需重复声明 |
| `BaseToaster` | Base UI 通知容器，内部 ToastButton 使用 `engine=base` |

ChartContainer 内可直接声明 Recharts 的 AreaChart、BarChart、LineChart、PieChart、RadarChart、RadialBarChart、ScatterChart、ComposedChart 及其轴、图元、图例。Tooltip 的 `content` 使用 slots，图表数据可通过 Json 绑定刷新。

模板属性中的 `{ "$arg": "label" }` 从回调首个参数取属性，空路径表示参数本身。模板 `textPath="$value"` 显示整个参数，例如 ComboboxList 的字符串项。模板不可覆盖事件处理器，事件仍由 bindings 描述。

### 边界与验证

支持全部已安装的 UI 组件，并不意味着 C++ 可以序列化任意 JavaScript 函数。函数子节点使用 templates，元素属性使用 slots；复杂的前端业务逻辑、hooks 或自定义解析器仍应使用 Custom 或独立 WebView。`props` 中的事件处理器、裸 ref 和直接 HTML 注入不会执行。

未知组件和不合法组合会在局部显示错误，不让整张面板白屏。能力门控与只读状态会阻止声明式绑定写回，JSON 解析失败保留该属性的声明值。业务校验仍由宿主完成。

```bash
npm test
```

测试使用本机 Edge，自动启动并关闭本地 Vite 服务，覆盖 registry、旧控件、数值与文本桥接、推送、弹层、表单验证、日期、模板集合、表格、通知、图表与桌面和窄屏布局。

`custom.script` 接收 `(root, xbase)` 两个参数。`xbase` 提供 `call`、`get`、`set`、`getText`、`setText`、`run`、`on`，例如：

```js
const button = root.querySelector('[data-action="reset"]')
button?.addEventListener('click', () => xbase.run('example.reset'))
```

### 独立 HTML / JavaScript / CSS 文件

文件路径相对于 `XBase\Mods\<modId>\`，推荐把面板资源放到模组自己的 `ui/` 目录：

```cpp
XBase::Panel::Control custom;
custom.kind = XBase::Panel::ControlKind::Custom;
custom.id = "mymod.tools";
custom.label = "工具";
custom.htmlFile = "ui/tools.html";
custom.scriptFile = "ui/tools.js";
custom.styleFile = "ui/tools.css";
```

构建时将 `ui/` 复制到 `XBase\Mods\<modId>\ui\`，运行时在 `Panel::Mount` 时读取文件。文件字段优先于对应的 `html`、`script`、`style` 内联字段；文件不存在、路径不安全或读取失败时，会记录警告并保留内联内容作为 fallback。因此可以只把其中一部分拆成文件，也可以文件与内联混用。

文件路径只能使用模组目录内的相对路径，不能使用盘符、绝对路径或 `..` 穿越目录。独立脚本与内联脚本使用完全相同的 `(root, xbase)` 约定，返回函数时控件销毁会调用该函数清理事件监听。自定义内容来自已加载的 ASI，请只挂载可信模组。

## 排查路径

| 现象 | 优先检查 |
|---|---|
| 面板打不开，日志说资源缺失 | `dist` 没构建，或没跑 XBase 的 `Build.bat Release` |
| 页面全白 | Vite 插件被改坏，`type="module"` 没剥掉 |
| 侧栏空 | 没有模组调 `Panel::Mount`，或模组挂在自己的静态库副本上（共享库没加载） |
| 开关显示与真实相反 | 模组没绑 `BindValue`，或初始值 `panel.get` 取不到 |
| 改了不生效 | 模组没每帧推送状态，被下一帧覆盖 |
