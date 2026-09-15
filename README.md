# ClipWiz

A lightweight Windows clipboard history tool. Single executable, zero dependencies, < 8MB resident memory.

---

## Background

Many everyday workflows involve repeatedly pasting the same fixed text: access passwords for certain websites, authorization codes, template snippets, frequently used paths. The Windows clipboard only holds one item at a time, which makes this tedious.

Existing tools like Ditto handle multi-item clipboards, but they tend to be bulky and suffer from a critical flaw — pinned items get silently cleaned up after a while despite being marked as permanent.

ClipWiz was built around a simple set of requirements:

- Maintain a history of clipboard items for quick reuse
- Allow critical items to be pinned so they are never automatically removed
- Bind global hotkeys to pinned items for one-keystroke pasting into any window

On top of that, ClipWiz pursues extreme lightness: pure Win32 API, no runtime dependencies, a single exe of a few hundred KB that runs from anywhere.

---

## Quick Start

### Prerequisites

| Item | Requirement |
| --- | --- |
| OS | Windows 10 1809+ / Windows 11, x64 |
| Compiler | Visual Studio 2022 (MSVC + Windows SDK) |
| Build tool | CMake 3.20+ (included with VS installer) |

### Build

Double-click `build-tool.bat` in the project root and select **2 (Build)**. CMake configuration runs automatically on first build.

Command line:

```cmd
build-tool.bat build
```

Or manually:

```cmd
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release
```

### Output

`build\Release\clipwiz.exe` — single file, statically linked CRT, no VC runtime required. Copy it anywhere and run.

### Usage

The program lives in the system tray. Click the tray icon to open the quick-paste popup; right-click for the context menu.

---

## Features

| Feature | Description |
| --- | --- |
| Clipboard monitoring | Automatically records every copy operation: text, images, HTML, RTF, file lists |
| Quick-paste popup | Tray click to open; keyword filtering, number shortcuts, mouse operations |
| Pin items | Any item can be pinned; pinned items are never auto-deleted and don't count toward history limit |
| Global hotkeys | Up to 10 pinned positions, each bindable to a global hotkey for instant paste |
| Full-text search | Filter box matches the entire content of every entry; multiple keywords AND; `kind:` / `app:` tokens narrow by type or source app |
| Multi-select & merge | Ctrl/Shift-click or Ctrl+A to select several rows; merge them into one entry (text flattens and joins, file lists concatenate) |
| Text transforms | 13 built-in transforms (trim, case folding, camelCase, slugify, append date/time, …) — copy as a new entry or paste once |
| Sequential paste queue | Line up several entries and paste them one per press of a dedicated hotkey |
| Source app tracking | Each entry records the process it was copied from; persisted and searchable via `app:` |
| App blocklist | Skip capture and/or global hotkeys for chosen processes (e.g. password managers) |
| Preview desensitization | Optionally mask card / phone / ID numbers, API keys and password fields in previews |
| Backup & restore | Export the whole history to a `.clpw` file and merge a backup back — deduplicated, pinned-safe, never destructive |
| Save as file | Save one entry to `.txt` (text) or `.png` (image); file lists are revealed in Explorer |
| Multi-format | CF_UNICODETEXT / CF_DIB / HTML Format / RTF / CF_HDROP |
| Persistence | Custom binary format; data survives restarts |
| Async disk writes | Background thread handles I/O; UI thread never blocks |
| Large data protection | Prompts cleanup when data exceeds threshold (configurable) |
| Internationalization | English built-in; Simplified Chinese compiled into exe; other languages via .lng files |
| Auto-start | Registry Run key; no admin privileges needed |
| Dark mode | Follows system theme in real time |
| High DPI | Per-Monitor V2; crisp rendering across multi-monitor setups |

### Design Boundaries

- Fully offline — no network, no telemetry, no cloud sync
- No third-party libraries or runtimes (no Qt / .NET / Electron)
- No plugin system, scripting, or multi-device sync
- No groups / tags / folders
- No clipboard content restoration after paste

---

## Version History

### v1.3.0

A large feature release about finding, reshaping and safeguarding clipboard content — still fully offline, single-exe, zero-dependency.

- **Full-text search.** The filter box searches the entire content of every entry (not just the visible preview), ANDs multiple keywords, and supports `kind:text/rich/image/file` and `app:<name>` tokens.
- **Source app tracking.** Each entry records the process it was copied from — persisted in the store and searchable via `app:`.
- **App blocklist.** Skip capture and/or global hotkeys for chosen processes (e.g. password managers), per app.
- **Preview desensitization.** Optional masking of card numbers, phone numbers, national ID numbers, API keys and `password=`-style fields in list rows and hover previews.
- **Multi-select & merge.** Ctrl/Shift-click and Ctrl+A select several rows; merge them into one entry — text kinds flatten and join, file lists concatenate.
- **Text transforms.** Thirteen built-in transforms (trim, remove/normalize line breaks, upper/lower/capitalize/sentence/camel/invert case, ASCII-only, slugify, append date/time), applied as a copied new entry or a one-shot paste.
- **Sequential paste queue.** Queue several entries and paste them one at a time via a dedicated hotkey (default Ctrl+Alt+J).
- **Backup & restore (.clpw).** Export the whole history to a file and merge a backup back in — deduplicated, pinned-safe, and never destructive on an unreadable file.
- **Save as file.** Save a single entry to `.txt` (text/HTML/RTF as extracted plain text) or `.png` (image, stored bytes verbatim); file lists are revealed in Explorer.
- **Paste robustness.** The paste chord is configurable and Ctrl+V is sent by scan code for wider app compatibility; hover preview no longer requires holding Ctrl (configurable).
- **Headless test suite.** The pure logic — text conversion, filtering, privacy markers, blocklist, masking, merge, transforms and the store — is covered by a `clipwiz_tests` target (800+ checks, no third-party framework).

### v1.2.0

- **Type icons on every row.** Each entry now shows a fixed-width type marker (plain text / RTF / HTML / image / file list), each with its own shape and color so you can tell types apart at a glance. The pin, type icon, and number columns are evenly spaced and aligned across all rows.
- **"Convert to plain text" menu.** Right-click an RTF or HTML entry to strip its formatting in place — the same entry becomes plain text and keeps its position. If that makes it identical to an existing text entry, the two are merged (the higher one is kept; two pinned duplicates are both kept).
- **Fixed duplicate rich text.** Copying the same passage from Word or a browser repeatedly no longer piles up duplicate entries. Duplicate detection now compares the actual text content, and a repeat copy keeps the newest version so re-edited formatting isn't lost.
- **Fixed ordering.** Newly copied and newly pasted items now reliably appear at the top of their group.
- **Settings tooltips.** The Windows-key icon buttons in Settings now show a hint on hover.

### v1.1.0

- Settings dialog polish and layout improvements; item expiry based on last-use time; multilingual display and stability fixes.

### v1.0.0

- First release: clipboard history with pinning and auto-cleanup, quick-paste popup, global hotkeys, support for images / RTF / HTML / file lists, settings dialog, internationalization, dark mode, and high-DPI rendering.

---

## Architecture

```
┌─────────────────────────────────────────────────┐
│          Hidden main window (message hub)        │
└──┬──────────────┬──────────────┬────────────────┘
   │              │              │
┌──▼──────┐  ┌───▼─────┐  ┌────▼────┐
│clipboard│  │ hotkey  │  │  tray   │
└──┬──────┘  └───┬─────┘  └────┬────┘
   │              │              │
┌──▼──────────────▼──────────────▼────┐
│           store (item database)      │
│     in-memory vector + store.dat     │
└──┬──────────────────────────────┬───┘
   │                              │
┌──▼────────┐            ┌───────▼────────┐
│   popup   │            │   settings     │
└──┬────────┘            └────────────────┘
   │
┌──▼────────┐     ┌─────────────┐
│   paste   │     │ asyncwriter │
└───────────┘     └─────────────┘
```

- Single process; main logic runs in the UI thread message loop
- Disk writes handled by a dedicated AsyncWriter thread; UI thread only serializes in memory
- Image encoding/decoding via WIC (built into Windows)
- All UI is native Win32 controls + GDI custom drawing

### Source Layout

```
src/
  main.cpp           Entry, single-instance check, message loop
  app.h/.cpp         Global state, message dispatch, backup / save-as actions
  store.h/.cpp       Item database, serialization, eviction, import-merge
  textconv.h/.cpp    HTML/RTF → plain text; canonical form used for deduplication
  filter.h/.cpp      Popup filter parsing and matching (keywords + tokens)
  privacy.h/.cpp     Clipboard exclusion markers
  blocklist.h/.cpp   Per-app capture and hotkey suppression
  mask.h/.cpp        Preview desensitization (sensitive-span scanners)
  merge.h/.cpp       Combine selected items into one (text / file-list joins)
  transform.h/.cpp   Text transforms (case / whitespace / slug / timestamp)
  clipboard.h/.cpp   Clipboard monitoring and read/write
  hotkey.h/.cpp      Global hotkey management
  paste.h/.cpp       Paste execution
  popup.h/.cpp       Quick-paste popup window
  settings.h/.cpp    Configuration and settings dialog
  imagecodec.h/.cpp  WIC image codec
  tray.h/.cpp        Tray icon and menu
  i18n.h/.cpp        Internationalization
  asyncwriter.h/.cpp Async disk writer
  log.h/.cpp         Lightweight file logging
  raii.h             RAII wrappers (GlobalLock, HANDLE, GDI objects, clipboard open)
  util.h/.cpp        Utility functions
  resource.h         Control IDs and resource constants
  clipwiz.rc         Icon, manifest, version info, embedded language pack
lang/
  zh-CN.lng          Simplified Chinese language pack (compiled into exe at build time)
tests/
  testfw.h           Check macros and pass/fail tally — no third-party framework
  test_main.cpp      Entry; redirects the data directory to a scratch dir
  test_*.cpp         One per pure unit (textconv, store, filter, privacy, blocklist, mask, merge, transform)
```

---

## Technical Details

Data formats, storage strategy, paste pipeline, hotkey mechanics, and UI implementation details are documented in [doc/technical.md](doc/technical.md).

---

## Project Structure

```
clipwiz/
├── CMakeLists.txt        Build configuration
├── build-tool.bat        One-click build script
├── README.md
├── .gitignore
├── doc/
│   ├── technical.md      Detailed technical documentation
│   └── variables_win.md  Windows environment variables reference (reserved)
├── lang/
│   └── zh-CN.lng         Simplified Chinese (compiled into exe)
├── src/                  All source code
└── tests/                Headless unit tests (clipwiz_tests target)
```

---
---

# ClipWiz

轻量级 Windows 剪贴板历史记录工具。单文件、零依赖、常驻内存 < 8MB。

---

## 项目背景

日常工作中经常遇到这类场景：某些网站需要输入固定口令才能访问内容，口令本身不变，但每次都要手动复制粘贴；类似地，各种重复性的文本片段（授权码、模板段落、常用路径）也需要反复从某个地方找出来再粘贴。

Windows 自带的剪贴板只保留最近一条内容，无法满足"多条内容快速复用"的需求。市面上有 Ditto 等工具，但体积偏大，且存在置顶条目被自动清理的问题——明明标记了固定，过段时间还是消失了。

ClipWiz 的原始需求很简单：

- 多条剪贴板内容的历史记录与快速粘贴
- 关键条目可以固定（置顶），永不被自动清理
- 固定条目可绑定全局快捷键，一键粘贴到任意窗口

在此基础上，ClipWiz 追求极致的轻量：纯 Win32 API 实现，不依赖任何运行时，产物是一个几百 KB 的 exe，拷走即用。

---

## 快速开始

### 环境要求

| 项目 | 要求 |
| --- | --- |
| 操作系统 | Windows 10 1809+ / Windows 11，x64 |
| 编译器 | Visual Studio 2022（含 MSVC 和 Windows SDK） |
| 构建工具 | CMake 3.20+（VS 安装器勾选即可） |

### 构建

双击项目根目录的 `build-tool.bat`，在交互菜单中选择 **2 (Build)**。首次构建会自动执行 CMake 配置。

命令行：

```cmd
build-tool.bat build
```

或手动执行：

```cmd
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release
```

### 产物

`build\Release\clipwiz.exe`——单文件，静态链接 CRT，不需要安装 VC 运行库，复制到任意目录即可运行。

### 运行

双击 exe 后程序驻留系统托盘。单击托盘图标打开快速粘贴框，右键托盘图标打开菜单。

---

## 核心功能

| 功能 | 说明 |
| --- | --- |
| 剪贴板监听 | 后台自动记录每次复制内容，支持文本、图片、HTML、RTF、文件列表 |
| 快速粘贴框 | 单击托盘弹出，支持关键字过滤、序号直选、鼠标操作 |
| 置顶固定 | 任意条目可置顶，置顶项永不自动删除，不占历史额度 |
| 全局快捷键 | 置顶区前 10 个位置各可绑定一个全局热键，按下即粘贴 |
| 全文搜索 | 过滤框匹配每条内容的完整正文（不止预览）；多关键字 AND；`kind:` / `app:` 令牌按类型或来源程序筛选 |
| 多选与合并 | Ctrl/Shift 单击或 Ctrl+A 选中多行，合并为一条（文本展平拼接，文件列表串联） |
| 文本变换 | 13 种内置变换（去空白、大小写折叠、驼峰、slug、追加日期时间……）——复制为新条目或一次性粘贴 |
| 顺序粘贴队列 | 把多条内容排入队列，每按一次专用热键粘贴一条 |
| 来源程序记录 | 每条内容记录它复制自哪个进程；持久化并可用 `app:` 搜索 |
| 应用黑名单 | 对指定进程跳过捕获和/或全局热键（如密码管理器） |
| 预览脱敏 | 可选择在预览中屏蔽银行卡 / 手机号 / 身份证号、API 密钥和密码字段 |
| 备份与恢复 | 把整个历史导出为 `.clpw` 文件，也可将备份合并回来——去重、保护置顶、绝不破坏现有数据 |
| 另存为文件 | 把单条内容存为 `.txt`（文本）或 `.png`（图片）；文件列表则在资源管理器中定位 |
| 多格式支持 | CF_UNICODETEXT / CF_DIB / HTML Format / RTF / CF_HDROP |
| 持久化 | 自定义二进制格式落盘，重启后数据完整保留 |
| 异步写盘 | 后台线程执行 I/O，主线程零阻塞 |
| 大数据保护 | 数据量超阈值时提示清理，阈值可配置 |
| 国际化 | 英文内置，简体中文编译进 exe，其他语言通过 .lng 文件扩展 |
| 开机自启 | 写注册表 Run 项，无需管理员权限 |
| 深色模式 | 跟随系统主题实时切换 |
| 高 DPI | Per-Monitor V2，多显示器不同缩放清晰渲染 |

### 设计边界

- 纯本地运行，不联网、不遥测、不云同步
- 不引入第三方库和运行时（无 Qt / .NET / Electron）
- 不做插件系统、脚本扩展、多设备同步
- 不做分组 / 标签 / 文件夹
- 不做粘贴后剪贴板内容还原

---

## 版本历史

### v1.3.0

一次围绕"查找、重塑、守护剪贴板内容"的大版本更新——依旧全程离线、单文件、零依赖。

- **全文搜索。** 过滤框搜索每条内容的完整正文（不止可见预览），多关键字 AND，并支持 `kind:text/rich/image/file` 与 `app:<名称>` 令牌。
- **来源程序记录。** 每条内容记录它复制自哪个进程——持久化到存储，并可用 `app:` 搜索。
- **应用黑名单。** 对指定进程跳过捕获和/或全局热键（如密码管理器），按程序分别设置。
- **预览脱敏。** 可选屏蔽列表行与悬浮预览中的银行卡号、手机号、身份证号、API 密钥以及 `password=` 之类字段。
- **多选与合并。** Ctrl/Shift 单击与 Ctrl+A 选中多行，合并为一条——文本类展平拼接，文件列表串联。
- **文本变换。** 13 种内置变换（去空白、删除/规范换行、大写/小写/首字母大写/句首大写/驼峰/反写大小写、仅 ASCII、slug、追加日期时间），可复制为新条目或一次性粘贴。
- **顺序粘贴队列。** 把多条内容排入队列，通过专用热键（默认 Ctrl+Alt+J）逐条粘贴。
- **备份与恢复（.clpw）。** 把整个历史导出为文件，也可将备份合并回来——去重、保护置顶，遇到读不出的文件绝不破坏现有数据。
- **另存为文件。** 把单条内容存为 `.txt`（文本/HTML/RTF 取展平纯文本）或 `.png`（图片，原样写出存储字节）；文件列表则在资源管理器中定位。
- **粘贴健壮性。** 粘贴组合键可配置，Ctrl+V 以扫描码发送以兼容更多程序；悬浮预览不再需要按住 Ctrl（可配置）。
- **无界面测试套件。** 纯逻辑——文本转换、过滤、隐私标记、黑名单、脱敏、合并、变换与存储——由 `clipwiz_tests` 目标覆盖（800+ 断言，无第三方框架）。

### v1.2.0

- **每行类型图标。** 每个条目前显示一个等宽的类型标识（纯文本 / RTF / HTML / 图片 / 文件列表），每种类型造型和颜色各不相同，一眼即可区分类型。图钉、类型图标、序号三列等间距，所有行左对齐。
- **"转为纯文本"菜单。** 右键 RTF 或 HTML 条目可将其原地转为纯文本——同一条变成纯文本、位置不变。若转换后与已有纯文本条目相同，则自动合并（保留靠上的那条；两条都置顶时都保留）。
- **修复富文本重复。** 从 Word 或浏览器反复复制同一段内容，不再堆积重复条目。去重改为比较实际文本内容，重复复制时保留最新版本，重新排版后不会丢。
- **修复排序。** 新复制和新粘贴的条目现在都会稳定地出现在所属分组的最上方。
- **设置项悬浮提示。** 设置界面里的 Win 键图标按钮，鼠标悬停会显示提示。

### v1.1.0

- 设置对话框细节与布局优化；按最后使用时间过期；多语言显示与稳定性修复。

### v1.0.0

- 首个版本：剪贴板历史（置顶与自动清理）、快速粘贴框、全局快捷键，支持图片 / RTF / HTML / 文件列表、设置对话框、国际化、深色模式、高 DPI。

---

## 技术架构

```
┌─────────────────────────────────────────────────┐
│            隐藏主窗口（消息中枢）                 │
└──┬──────────────┬──────────────┬────────────────┘
   │              │              │
┌──▼──────┐  ┌───▼─────┐  ┌────▼────┐
│clipboard│  │ hotkey  │  │  tray   │
│剪贴板监听│  │热键管理  │  │托盘图标 │
└──┬──────┘  └───┬─────┘  └────┬────┘
   │              │              │
┌──▼──────────────▼──────────────▼────┐
│           store（条目库）             │
│     内存 vector + store.dat 持久化   │
└──┬──────────────────────────────┬───┘
   │                              │
┌──▼────────┐            ┌───────▼────────┐
│   popup   │            │   settings     │
│ 快速粘贴框 │            │   设置对话框    │
└──┬────────┘            └────────────────┘
   │
┌──▼────────┐     ┌─────────────┐
│   paste   │     │ asyncwriter │
│ 粘贴执行   │     │ 后台写盘线程 │
└───────────┘     └─────────────┘
```

- 单进程，主逻辑在 UI 线程消息循环中执行
- 写盘由 AsyncWriter 独立线程完成，主线程仅做内存序列化
- 图片编解码使用 WIC（Windows 内置组件）
- 全部界面为 Win32 原生控件 + GDI 自绘

### 源文件

```
src/
  main.cpp           入口、单实例、消息循环
  app.h/.cpp         全局状态、消息分发、备份 / 另存为动作
  store.h/.cpp       条目库、序列化、淘汰、导入合并
  textconv.h/.cpp    HTML/RTF → 纯文本；用于去重的规范化形式
  filter.h/.cpp      过滤框解析与匹配（关键字 + 令牌）
  privacy.h/.cpp     剪贴板排除标记
  blocklist.h/.cpp   按程序的捕获与热键抑制
  mask.h/.cpp        预览脱敏（敏感片段扫描器）
  merge.h/.cpp       把选中项合并为一条（文本 / 文件列表拼接）
  transform.h/.cpp   文本变换（大小写 / 空白 / slug / 时间戳）
  clipboard.h/.cpp   剪贴板监听与读写
  hotkey.h/.cpp      全局热键管理
  paste.h/.cpp       粘贴执行
  popup.h/.cpp       快速粘贴框
  settings.h/.cpp    配置与设置对话框
  imagecodec.h/.cpp  WIC 图片编解码
  tray.h/.cpp        托盘图标与菜单
  i18n.h/.cpp        国际化
  asyncwriter.h/.cpp 异步写盘
  log.h/.cpp         轻量文件日志
  raii.h             RAII 封装（GlobalLock、HANDLE、GDI 对象、剪贴板打开）
  util.h/.cpp        工具函数
  resource.h         控件 ID 与资源常量
  clipwiz.rc         图标、清单、版本信息、内嵌语言包
lang/
  zh-CN.lng          简体中文语言包（编译时打包进 exe）
tests/
  testfw.h           断言宏与通过/失败计数——无第三方框架
  test_main.cpp      入口；把数据目录重定向到临时目录
  test_*.cpp         每个纯逻辑单元一个（textconv、store、filter、privacy、blocklist、mask、merge、transform）
```

---

## 技术细节

数据格式、存储策略、粘贴链路、快捷键机制、界面实现等详细技术说明见 [doc/technical.md](doc/technical.md)。

---

## 目录结构

```
clipwiz/
├── CMakeLists.txt        构建配置
├── build-tool.bat        一键构建脚本
├── README.md
├── .gitignore
├── doc/
│   ├── technical.md      详细技术文档
│   └── variables_win.md  Windows 环境变量参考（预留）
├── lang/
│   └── zh-CN.lng         简体中文（编译时打包进 exe）
├── src/                  全部源码
└── tests/                无界面单元测试（clipwiz_tests 目标）
```
