# XBase 示例骨架

> 三份可以直接拷贝出来改的模组骨架，覆盖最小插件、菜单界面与网页面板三条路径。

## 边界

这里是**骨架**，不是功能模组。每份骨架只做一件事：

| 目录 | 演示内容 | 用到的 XBase 能力 |
|---|---|---|
| `01-hello` | 生命周期注册、日志、游戏内提示 | `Host` `Log` `Platform` |
| `02-menu` | 界面绘制、配置持久化、热键、每帧状态推送 | `Hooks` `UI` `Config` `Input` `Player` |
| `03-webui` | 本地页面映射、原生方法注册、页面反向调用 | `WebView` `WebBridge` `Config` |

三份都用**单文件 asi 形态**：asi 自带业务代码，不需要额外的 payload dll，同一个源文件在 SA / VC / III 三个版本上都能跑，版本差异由 Bootstrap 加载的共享运行时承担。

本目录不覆盖：三段式 payload 形态、能力矩阵全量说明、XBase 公共 API 逐项文档。这些看在线文档 [xbase](https://blog.miomoe.cn/docs/xbase)。

## 前置条件

`example/include` 与 `example/lib` 不是手写内容，由 XBase 构建时统一 stage 过来，三个示例共用同一份。

```bash
# 在 XBase 根目录执行一次，产物会 stage 到 example/include 与 example/lib
Build.bat Release
```

`lib` 里应包含 `XBaseModEntry` `XBaseSA` `XBaseVC` `XBaseIII` `PluginSA` `PluginVC` `PluginIII`。缺任何一个，示例的构建脚本会直接报错并提示先跑 XBase 构建。

## 构建

```bash
cd 01-hello
Build.bat Release
```

产物落在 `build\bin\`：

```txt
build\bin\HelloSA.asi
build\bin\HelloVC.asi
build\bin\HelloIII.asi
```

把对应游戏版本的 asi 拷进游戏的 `plugins` 目录即可，数据目录由 XBase 自动建在游戏根目录的 `XBase\Mods\<模组名>\`。

脚本参数：

| 参数 | 说明 |
|---|---|
| `Release` / `Debug` | 构建配置，默认 Release。XBase 只 stage Release 库，传 Debug 会直接报错 |
| `--toolset v143` | 覆盖平台工具集，默认 `v145`。VS 2022 用 `v143` |
| `--no-pause` | 结束不暂停，适合 CI 与脚本调用 |

三个示例的 `Build.bat` 内容完全相同，工程名从生成的 sln 推导，复制到新工程不用改任何一行。

## 改成自己的模组

| 位置 | 要改什么 |
|---|---|
| `src/main.cpp` 的 `XBasePayloadBaseName` | 返回的基名决定数据目录与载荷名，不带游戏后缀 |
| `premake5.lua` 的 `workspace` | 决定 sln 名与 asi 文件名 |
| `src/main.cpp` 的业务函数 | `OnGameInit` 与 `OnProcess` 里换成自己的逻辑 |

四个导出缺一不可，且必须带 `__declspec(dllexport)`，只写 `extern "C"` 不会真正导出：

```cpp
extern "C" __declspec(dllexport) const char* XBasePayloadBaseName();
extern "C" __declspec(dllexport) const char* XBaseModTargetGame();
extern "C" __declspec(dllexport) void XBasePayloadAttach();
extern "C" __declspec(dllexport) void XBasePayloadDetach();
```

不导出基名时 Bootstrap 会按 asi 文件名推导，把 `MyModVC.asi` 推成 `MyModVC`，数据目录、载荷名、清单校验三处会一起错。

## 模组清单 package.json

每个示例都带 `data/package.json`，构建时落到 `build\bin\XBase\Mods\<模组名>\package.json`。Bootstrap 在挂载模组**之前**读它做约束校验，不满足就拒绝挂载并弹出原因，所以清单不是可选装饰。

| 字段 | 作用 |
|---|---|
| `name` | 显示名，缺省用目录名 |
| `version` | 模组版本 |
| `author` | 作者 |
| `description` | 说明 |
| `license` | SPDX 标识，例如 `MIT` |
| `homepage` | 主页 |
| `engines.xbase` | 所需 XBase 版本区间，不满足直接拒绝挂载 |
| `dependencies` | 依赖声明，值是版本区间或发布通道标记 |

依赖分两类：名字能对应到其它模组的，按对方清单里的版本校验；对应不到的是第三方运行库（加载器、WebView2 运行时这类），只声明不做自动校验。

清单不存在时按无约束放行，不报错，但同时也失去了约束保护。

```json
{
  "name": "Hello",
  "version": "v0.1.0",
  "author": "Your Name",
  "description": "最小 XBase 模组骨架，演示生命周期注册与日志",
  "homepage": "",
  "engines": {
    "xbase": ">=0.1.0"
  },
  "dependencies": {
    "Ultimate ASI Loader": "latest"
  },
  "license": "MIT"
}
```

目录名由 `XBasePayloadBaseName()` 的返回值决定，清单里的 `name` 只是显示名，两者建议保持一致。`01-hello` 用 `Package::Load` 读自己的清单，版本与约束都从那里来，源码里不写死。

## 正确写法与错误写法

开关类状态必须由宿主持有，每帧推给领域。只在点击时调一次，下一帧就会被覆盖，表现为点了没反应。

```cpp
// 错误：网页或热键入口直接调一次领域开关
XBase::Player::SetGodMode(true);

// 正确：入口改宿主状态，每帧由宿主推送
void OnProcess() {
    XBase::Player::SetGodMode(gGodMode);
}
```

其它边界：

- 宿主只能包含 `XBase/*.h`，不能碰 plugin-sdk、ImGui、D3D 或裸地址
- 配置与日志走 `Platform::Mod*` 与 `InitForMod`，禁止写到 asi 所在目录
- 调用领域能力前先查 `HasCapability`，`Unsupported` 要禁用入口而不是照常调用
- 网页面板必须先 `MapFolder` 映射成虚拟 https 主机，`file:` 协议下模块脚本会被浏览器拦下

## 排查路径

| 现象 | 优先检查 |
|---|---|
| 提示缺 `..\lib\*.lib` | 先跑 XBase 的 `Build.bat Release` 生成 SDK |
| 提示找不到 `premake5.exe` | 脚本找的是 `..\..\tools\premake5.exe`，确认 XBase 的 tools 目录还在 |
| 提示 MSBuild 找不到 | 装 VS 的 Desktop development with C++ 工作负载，或用 Developer Command Prompt 跑 |
| 平台工具集报错 | 加 `--toolset v143` 之类的实际值 |
| 游戏里完全没反应 | asi 是否放进 `plugins`，四个导出是否都带 `dllexport` |
| 弹 Failed to detect | 游戏版本不支持，或 asi 放错了目录 |
| 数据目录名带版本后缀 | 没导出 `XBasePayloadBaseName`，被按文件名推导了 |
| 启动弹版本不满足 | `package.json` 的 `engines.xbase` 高于当前 XBase 版本 |
| 清单改了没生效 | 清单要落到数据目录，只改源码里的 `data/package.json` 不重新构建不会更新 |
| 菜单点开关没反应 | 状态没每帧推送，被下一帧覆盖了 |
| 网页面板空白 | 页面没映射成虚拟 https 主机，或 `ui` 目录没拷进数据目录 |

日志在 `XBase\Mods\<模组名>\debug.log`，也可以用 XBase 自带的查看器打开。
