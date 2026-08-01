# ClipWiz

一个只做剪贴板池 + 置顶项快捷键粘贴的极小体积 Windows 工具，用来替代 Ditto。

- 目标平台：Windows 10 1809 及以上 / Windows 11，x64
- 技术栈：C++17 + 纯 Win32 API，MSVC 2022 编译，CMake 构建，无任何第三方库
- 预期指标：单个 exe 约 200~350KB，空闲常驻内存约 4~8MB，冷启动 < 100ms，弹出框显示 < 30ms

---

## 要解决的问题

下载电子书的网站每本书都要输入一个口令，口令来自公众号回复，同一个口令要反复粘贴很多次。

Ditto 能固定条目，但它的清理逻辑有毛病：置顶 + 禁止删除的条目，过一段时间还是会被它自己清掉。所以核心诉求是：

**置顶的条目必须永不消失，并且能用一个自定义快捷键（比如 Ctrl+1）直接把它打到光标处。**

这一条是第一优先级，所有设计都要保证它成立。

---

## 功能范围

### 要做的

| 编号 | 功能 |
| --- | --- |
| F1 | 后台监听系统剪贴板，每次内容变化都作为一条记录进池 |
| F2 | 单击托盘图标弹出快速粘贴框，选一条粘贴到之前那个窗口的光标处 |
| F3 | 任意一条或多条可置顶（固定），置顶项永不自动删除 |
| F4 | 置顶项按位置绑定全局快捷键（共 10 个位置），按下即粘贴 |
| F5 | 支持纯文本、图片、HTML、RTF、文件拖放（CF_HDROP）五类内容 |
| F6 | 历史条数上限可在设置里改（默认 50） |
| F7 | 数据落盘，重启电脑后置顶项和历史都还在 |
| F8 | 可选开机自启 |
| F9 | 支持 i18n，英文内置为默认，语言文件（.lng）可扩展其他语言 |
| F10 | 写盘异步化，不阻塞主线程 |
| F11 | 数据过大时提示用户清理，阈值可配置 |

### 明确不做的

- 不做云同步、不联网、不做任何遥测统计
- 不做多设备、不做插件、不做脚本
- 不做分组 / 文件夹 / 标签
- 不做粘贴后自动还原原剪贴板内容
- 不用 Electron / .NET / Qt / WebView 之类的运行时

---

## 整体结构

```
                    ┌──────────────────────────────┐
                    │   隐藏主窗口（消息中枢）      │
                    │   ClipWizMainWnd             │
                    └───┬──────────┬───────────┬───┘
      WM_CLIPBOARDUPDATE│          │WM_HOTKEY  │托盘消息 / 定时保存
                        │          │           │
              ┌─────────▼───┐  ┌───▼──────┐  ┌─▼─────────┐
              │ 剪贴板监听  │  │ 热键管理 │  │ 托盘图标  │
              │ clipboard   │  │ hotkey   │  │ tray      │
              └─────────┬───┘  └───┬──────┘  └─┬─────────┘
                        │          │           │
                    ┌───▼──────────▼───────────▼───┐
                    │      条目库 store            │
                    │  内存数组 + store.dat 持久化 │
                    └───┬──────────────────────┬───┘
                        │                      │
              ┌─────────▼────────┐   ┌─────────▼────────┐
              │ 快速粘贴框 popup │   │ 设置窗口 settings│
              └─────────┬────────┘   └──────────────────┘
                        │
              ┌─────────▼────────┐
              │ 粘贴执行 paste   │
              └──────────────────┘
```

主逻辑在 UI 线程的窗口消息里跑。写盘由独立的 AsyncWriter 后台线程执行，主线程只做内存序列化，不阻塞。

### 源文件划分

```
src/
  main.cpp          WinMain、单实例检查、消息循环
  app.h/.cpp        全局状态、隐藏主窗口、消息分发、退出清理
  store.h/.cpp      条目数据结构、增删改查、置顶、淘汰、store.dat 序列化
  clipboard.h/.cpp  剪贴板监听与读写（CF_UNICODETEXT / CF_DIBV5 / CF_DIB / CF_HDROP）
  hotkey.h/.cpp     RegisterHotKey 管理、快捷键与文字互转、冲突处理
  paste.h/.cpp      目标窗口跟踪、写剪贴板 + SendInput 模拟 Ctrl+V
  popup.h/.cpp      快速粘贴框（自绘列表 + 过滤 + 右键菜单 + 拖动 + 预览）
  settings.h/.cpp   配置读写（config.ini）、设置对话框（多 Tab 页）
  imagecodec.h/.cpp WIC 封装：DIB 与 PNG 互转、缩略图生成
  tray.h/.cpp       托盘图标与托盘右键菜单
  i18n.h/.cpp       国际化：英文内置默认 + .lng 语言文件加载
  asyncwriter.h/.cpp 后台异步写盘线程
  util.h/.cpp       路径、DPI、主题色、字体、原子写文件、错误提示
  resource.h
  clipwiz.rc        图标、manifest
  app.manifest      DPI 感知、公共控件 v6、requestedExecutionLevel=asInvoker
lang/
  zh-CN.lng         简体中文语言包
CMakeLists.txt
```

图片用 WIC（windowscodecs.dll）编解码，Windows 自带组件，不算第三方依赖。

---

## 数据设计

### 条目结构（内存）

```cpp
enum class ItemKind : uint32_t { Text = 0, Image = 1, Html = 2, Rtf = 3, FileDrop = 4 };

struct Item {
    uint64_t     id;          // 自增，永不复用
    ItemKind     kind;
    bool         pinned;      // 置顶标记
    uint64_t     createdAt;   // FILETIME
    uint64_t     usedAt;      // 最后一次被粘贴的时间
    uint32_t     hotkey;      // 高 16 位修饰键 MOD_*，低 16 位虚拟键码；0 = 未绑定
    std::wstring text;        // 完整文本内容
    std::wstring preview;     // 列表里显示的一行摘要
    uint32_t     imgW, imgH;  // kind==Image：像素尺寸
    std::vector<uint8_t> data; // 原始数据（HTML/RTF/FileDrop 等）
};
```

内存里用一个 `std::vector<Item>`，顺序就是显示顺序（置顶区在前，历史区按最近使用排）。图片像素数据不常驻内存，需要时才从磁盘读。

### 磁盘布局

```
数据目录（默认为 exe 同目录，或 %APPDATA%\ClipWiz\）
    config.ini          全局设置（UTF-8 文本）
    store.dat           条目库（自定义二进制）
    store.dat.tmp       保存时的临时文件，写完原子替换
    images\
        000000000123.png  图片内容，文件名为条目 id
```

绿色便携：默认数据目录为 exe 同目录。也可在设置中指定自定义路径。

### store.dat 格式

小端，定长头 + 变长记录，不用任何解析库。

```
Header (32 字节)
    char     magic[4]   = "CLPW"
    uint32   version    = 1
    uint32   itemCount
    uint64   nextItemId
    uint8    reserved[12]

Record × itemCount
    uint64   id
    uint32   kind
    uint32   flags        bit0 = pinned
    uint64   createdAt
    uint64   usedAt
    uint32   hotkey
    uint32   imgW
    uint32   imgH
    uint32   textLen      UTF-16 码元个数
    wchar_t  text[textLen]
    uint32   dataLen      原始数据字节数
    uint8    data[dataLen]
```

### 保存策略（防丢的关键）

1. **淘汰只看未置顶条目**。置顶项在任何代码路径下都不参与自动删除，唯一能删掉置顶项的操作是用户在界面上明确点"删除"并过确认框。
2. **条数上限只统计未置顶条目**。置顶项不占额度。
3. **置顶、取消置顶、绑定快捷键、删除** 立即同步落盘。
4. 普通剪贴板变化走延迟保存：SetTimer 800ms 合并写一次。
5. 退出时（托盘退出 / WM_ENDSESSION）强制落盘。
6. 写盘一律是"写 tmp → FlushFileBuffers → MoveFileExW 原子替换"。中途断电最坏回到上一个完整版本。
7. 启动时 store.dat 校验失败不覆盖不清空，改名 `store.corrupt.<时间戳>.dat` 保留，以空库启动并提示。
8. 图片文件只在条目确认从库里移除之后才删对应文件。

### 异步写盘（AsyncWriter）

- 独立后台线程 + CRITICAL_SECTION + auto-reset Event
- 主线程调用 `Store::Serialize()` 做内存序列化（微秒级），然后把 buffer 提交给 AsyncWriter
- 新请求替换排队中的旧数据（不堆积），同一时刻最多一个写操作在执行
- `atomic<bool> busy` 标志供主线程查询写盘状态
- 退出时 `Stop()` 等待当前写操作完成

### 大数据保护

- 总数据量超过 100MB 时弹出提示，询问用户是否清理
- 清理逻辑：删除非置顶且单条数据超过 `largeItemThresholdMB`（默认 10MB，可配置 1~500）的条目
- 每次运行只提示一次

### config.ini

```ini
[General]
MaxHistory=50
Autostart=0
ExpiryDays=0
Language=
Theme=auto
PopupPosition=mouse
FontName=
FontHeight=0
DataDir=

[Paste]
PopupHotkey=Ctrl+Alt+V
PasteDelayMs=60
CloseAfterPaste=1

[Limits]
MaxTextBytes=1048576
MaxImagePixels=33177600
ImageDiskBudgetMB=200
LargeItemThresholdMB=10

[UI]
RowsVisible=10
```

---

## 剪贴板监听

### 监听方式

用 `AddClipboardFormatListener(hwnd)` 接收 `WM_CLIPBOARDUPDATE`。Vista 之后的官方接口，不轮询，空闲 CPU 占用为 0。

### 处理流程

```
WM_CLIPBOARDUPDATE
  ├─ 序列号检查：自己写入的 → 丢弃（防止粘贴时产生重复）
  ├─ OpenClipboard 重试：失败则 50ms 后重试，最多 5 次
  ├─ 排除标记检查：存在 "Clipboard Viewer Ignore" 或
  │  "ExcludeClipboardContentFromMonitorProcessing" → 丢弃（尊重密码管理器）
  ├─ 取内容（按优先级）：
  │    CF_UNICODETEXT → 文本条目
  │    CF_DIBV5 / CF_DIB / CF_BITMAP → 图片条目，WIC 编码为 PNG 存盘
  │    CF_HDROP → 文件拖放条目（只存路径列表）
  │    HTML Format → HTML 条目
  │    Rich Text Format → RTF 条目
  │    否则 → 丢弃
  ├─ 去重：内容与池中已有条目完全相同 → 不新增，把老条目提到最前
  └─ 入池 → 淘汰检查（只动未置顶）→ 延迟保存
```

### 图片处理

- 读：优先 CF_DIBV5（带 alpha），退化到 CF_DIB，再退化到 CF_BITMAP
- 存：WIC 转 PNG 落盘（1920×1080 截图 DIB 8MB → PNG 几百 KB）
- 写回剪贴板：从 PNG 解码回 32 位 DIB，同时放 CF_DIBV5 和 CF_DIB 两种格式
- 缩略图：弹出框打开时按需生成，最多缓存 24 张，关框后释放

---

## 粘贴执行（核心链路）

### 目标窗口

用 `SetWinEventHook(EVENT_SYSTEM_FOREGROUND)` 全程跟踪"上一个正常的前台窗口"，过滤掉本进程窗口、任务栏、桌面、开始菜单等。热键直接粘贴时取当前前台窗口。

### 粘贴步骤

1. 把条目内容写入系统剪贴板，记录序列号供自我过滤
2. 隐藏弹出框
3. SetForegroundWindow 把焦点还给目标窗口（失败则 AttachThreadInput 兜底）
4. Sleep(PasteDelayMs) 等目标窗口拿到焦点
5. 修正修饰键状态（抬起不该按下的 Shift/Alt/Win/主键）
6. SendInput：Ctrl↓ V↓ V↑ Ctrl↑
7. 还原修饰键状态
8. 更新条目 usedAt，提到列表最前

### 为什么不还原原剪贴板内容

粘贴完剪贴板里就是刚粘的那条内容，不做还原。目标程序可能异步取剪贴板，还原太快粘到旧内容，还原太慢用户已经又复制了别的。

---

## 快捷键

### 注册

`RegisterHotKey` 全部注册在隐藏主窗口上：

- id 1：唤出快速粘贴框（默认 Ctrl+Alt+V）
- id 1000 + n：第 n 个置顶位置的快捷键

修饰键支持 Ctrl / Alt / Shift / Win 的任意组合 + 一个主键，一律带 `MOD_NOREPEAT`。

### 位置绑定

快捷键是为置顶的前 10 个"位置"设置的，跟里边的内容无关。谁进了某个位置，谁就用该位置的快捷键。设置界面中 10 个位置以 2×5 网格排列。

### 冲突处理

- 设置界面里绑定失败：弹提示"该快捷键已被其他程序占用"，不保存
- 启动时批量注册失败：托盘弹一次气泡提示

### 风险提示

检测到用户选了 Ctrl+数字、Ctrl+C/V/X、Alt+Tab 等常用组合时，显示灰色提示说明会抢占，用户知道了照样能保存。

---

## 界面

全部用 Win32 原生控件 + GDI 自绘，不引入任何 UI 框架。

### 托盘图标

- 左键单击 → 打开快速粘贴框
- 右键 → 菜单：快速粘贴 / 设置 / 开机自启 / 清空历史 / 关于 / 退出
- 悬浮提示：`ClipWiz — N 条记录，M 个置顶`

### 快速粘贴框

```
┌──────────────────────────────────────────┐
│  ClipWiz                            [×]  │  ← 标题栏，可拖动
├──────────────────────────────────────────┤
│  输入关键字过滤...                        │  ← 过滤框，打开即聚焦
├──────────────────────────────────────────┤
│  📌 1  book-token-2024xxxx      Ctrl+1   │  ← 置顶区，编号连续
│  📌 2  另一个常用口令           Ctrl+Alt+2│
│     3  https://example.com/download…     │  ← 历史区，编号顺延
│     4  [图片 1920×1080]                  │
│     5  上次复制的一段文字…                │
├──────────────────────────────────────────┤
│  Enter 粘贴 · Alt+数字 直接粘贴 · ...    │  ← 快捷键提示栏
└──────────────────────────────────────────┘
```

- 编号全局连续：置顶从 1 开始，非置顶顺延
- 置顶项有图钉图标，与编号统一对齐
- 尺寸：宽 420dip，高按 RowsVisible 算
- 位置：可选鼠标指针处 / 光标处 / 上次打开的位置（支持拖动）
- 键盘：↑↓ 移动，Enter 粘贴，Esc 关闭，Alt+1~9 直接粘贴，Ctrl+D 删除，Ctrl+P 切换置顶
- 鼠标：单击选中，双击粘贴，右键上下文菜单
- 右键菜单：粘贴 / 复制 / 置顶 / 删除（非置顶项不显示"设置快捷键"）
- Ctrl 悬停预览：长文本显示完整内容，图片显示大图
- 置顶项允许拖动调整顺序
- 失去焦点自动隐藏
- 列表双缓冲绘制，无闪烁
- 窗口只创建一次，之后 show/hide

### 设置窗口

多 Tab 页对话框：

**[常规]**
- 开机自动启动：checkbox
- 保存复制项目的最大数量：数字框
- 粘贴条目的过期天数：数字框
- 语言：下拉框（默认系统语言，不匹配 fallback 英文）
- 主题：跟随系统 / 浅色 / 深色
- 窗口弹出位置：鼠标指针处 / 光标处 / 上次打开的位置
- 显示字体：按钮（显示字体名+字号）+ 恢复默认按钮
- 数据库路径：只读路径框 + 浏览按钮

**[支持类型]**
- 左栏列表：纯文本、图片、HTML、RTF、文件拖放
- 右栏描述：该格式是什么、哪些软件产生

**[快捷键]**
- 唤出快速粘贴框：hotkey 控件
- 置顶项快捷键：10 个位置，2×5 网格，每个含 hotkey 控件 + "含Win" checkbox

### 深色模式与 DPI

- 深色：读注册表 AppsUseLightTheme，Theme=auto 时跟随，监听 WM_SETTINGCHANGE 实时切换
- DPI：manifest 声明 PerMonitorV2，尺寸以 dip 存，绘制时 GetDpiForWindow 换算

---

## 国际化（i18n）

- 英文内置在代码中（`i18n.cpp` 的 kDefaults 表），作为终极回退
- 语言文件为 `lang/<locale>.lng`，UTF-8 编码，`key=value` 格式
- 加载逻辑：按 config.ini 中 Language 设置 → 系统语言 → 英文回退
- 所有用户可见文字均通过 `i18n::T(key)` 获取，代码中不写死任何界面文字
- 自带简体中文语言包 `lang/zh-CN.lng`

---

## 进程与生命周期

- **单实例**：CreateMutexW，已存在则广播自定义消息让第一个实例弹出粘贴框，自己退出
- **无主窗口**：只有一个消息用的隐藏窗口，不出现在任务栏和 Alt+Tab 里
- **注销/关机**：处理 WM_QUERYENDSESSION / WM_ENDSESSION，先落盘再让系统继续
- **退出清理**：RemoveClipboardFormatListener、UnregisterHotKey、UnhookWinEvent、Shell_NotifyIcon(NIM_DELETE)，强制保存
- **开机自启**：写 HKCU\...\Run 下的 ClipWiz 值，不需要管理员权限

---

## 构建

### 环境要求

- Windows 10/11 x64
- Visual Studio 2022（含 MSVC 和 Windows SDK）
- CMake 3.20+

### 构建命令

```cmd
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release
```

产物 `build\Release\clipwiz.exe`，单文件，拷走就能用，不用安装、不用装 VC 运行库。

### 编译选项

- `/W4 /WX`：警告当错误，构建输出必须零警告
- `/permissive-`：严格标准一致性
- `/utf-8`：源文件 UTF-8
- `/GR-`：关闭 RTTI
- 静态 CRT（/MT）：不依赖 VC 运行库
- Release：/OPT:REF /OPT:ICF /INCREMENTAL:NO

### 链接库

user32、gdi32、msimg32、shell32、comctl32、advapi32、ole32、windowscodecs

---

## 支持的内容类型

| 格式 | 说明 |
| --- | --- |
| CF_UNICODETEXT | 纯 Unicode 文本，几乎所有程序复制文字时都会产生 |
| CF_DIB / CF_DIBV5 / PNG | 位图图像，截图工具、图片编辑器等复制图片时产生 |
| HTML Format | 富网页内容，浏览器和邮件客户端复制带格式文字时产生 |
| Rich Text Format | 带格式文档内容，Word、写字板等办公软件复制时产生 |
| CF_HDROP | 文件/文件夹路径列表，资源管理器中复制文件时产生，只存路径不存文件 |

---

## 风险与对策

| 风险 | 对策 |
| --- | --- |
| 目标窗口是管理员权限程序 | 说明书写明：要往提权程序里粘贴就让 ClipWiz 也以管理员身份运行 |
| SetForegroundWindow 被拒绝 | AttachThreadInput 兜底 |
| 用户设的快捷键抢占常用按键 | 设置界面明确提示，随时可改 |
| 图片占磁盘 | PNG 压缩 + 磁盘预算软上限 + 只淘汰未置顶图片 |
| 杀软敏感 | 不做网络请求、不注入进程、不装钩子 DLL，只用官方 API |
| 输入法状态影响模拟按键 | 只发 Ctrl+V 组合键，不逐字符输入 |

---

## 目录结构

```
clipwiz/
├── CMakeLists.txt
├── README.md
├── .gitignore
├── doc/
│   └── variables_win.md    Windows 环境变量参考（预留）
├── lang/
│   └── zh-CN.lng           简体中文语言包
└── src/
    ├── main.cpp
    ├── app.h / app.cpp
    ├── store.h / store.cpp
    ├── clipboard.h / clipboard.cpp
    ├── hotkey.h / hotkey.cpp
    ├── paste.h / paste.cpp
    ├── popup.h / popup.cpp
    ├── settings.h / settings.cpp
    ├── imagecodec.h / imagecodec.cpp
    ├── tray.h / tray.cpp
    ├── i18n.h / i18n.cpp
    ├── asyncwriter.h / asyncwriter.cpp
    ├── util.h / util.cpp
    ├── resource.h
    ├── clipwiz.rc
    ├── clipwiz.ico
    └── app.manifest
```
