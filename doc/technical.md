# ClipWiz Technical Documentation

Detailed technical implementation of ClipWiz. For project overview and quick start, see [README.md](../README.md).

---

## Data Design

### Item Structure (In-Memory)

```cpp
enum class ItemKind : uint32_t { Text = 0, Image = 1, Html = 2, Rtf = 3, FileDrop = 4 };

struct Item {
    uint64_t     id;          // auto-increment, never reused
    ItemKind     kind;
    bool         pinned;      // pin flag
    uint64_t     createdAt;   // FILETIME
    uint64_t     usedAt;      // last paste timestamp
    uint32_t     hotkey;      // high 16 bits = MOD_* modifiers, low 16 bits = VK code; 0 = unbound
    std::wstring text;        // full text content
    std::wstring preview;     // one-line summary for list display
    uint32_t     imgW, imgH;  // pixel dimensions (kind==Image)
    std::vector<uint8_t> data; // raw data (HTML/RTF/FileDrop etc.)
};
```

Items are stored in a `std::vector<Item>`; vector order equals display order (pinned section first, history sorted by most recent use). Image pixel data is not kept in memory — loaded from disk on demand.

### Disk Layout

```
Data directory (defaults to exe directory; configurable in settings)
    config.ini          Global settings (UTF-8 text)
    store.dat           Item database (custom binary format)
    store.dat.tmp       Temporary file during write; atomically replaced on completion
    images\
        000000000123.png  Image items, filename = item id
```

### store.dat Binary Format

Little-endian. Fixed-size header followed by variable-length records.

```
Header (32 bytes)
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
    uint32   textLen      UTF-16 code unit count
    wchar_t  text[textLen]
    uint32   dataLen      raw data byte count
    uint8    data[dataLen]
```

Binary format chosen over JSON/INI because clipboard text inevitably contains newlines, quotes, tabs, and emoji — text formats require escaping and introduce parsing ambiguity. Binary read/write by length fields is unambiguous and requires no parser library.

### config.ini

```ini
[General]
MaxHistory=50            ; unpinned item limit, range 5–2000
Autostart=0              ; launch at system startup
ExpiryDays=0             ; expiry in days, 0 = never
Language=                ; locale code, empty = follow system
Theme=auto               ; auto / light / dark
PopupPosition=mouse      ; mouse / caret / last
FontName=                ; custom font name, empty = system default
FontHeight=0             ; custom font size, 0 = system default
DataDir=                 ; data directory, empty = exe directory

[Paste]
PopupHotkey=Ctrl+Alt+V   ; hotkey to open popup
PasteDelayMs=60          ; delay after focus restore before sending Ctrl+V
CloseAfterPaste=1        ; close popup after paste

[Limits]
MaxTextBytes=1048576     ; text larger than this is not captured (1MB)
MaxImagePixels=33177600  ; images larger than ~8K×4K are not captured
ImageDiskBudgetMB=200    ; soft limit for images directory
LargeItemThresholdMB=10  ; threshold for large-item cleanup (1–500)

[UI]
RowsVisible=10           ; visible rows in popup
```

---

## Persistence Strategy

Data durability is a core reliability guarantee:

1. **Eviction only affects unpinned items.** Pinned items never participate in automatic deletion under any code path. The only way to remove a pinned item is explicit user deletion with confirmation.
2. **History limit counts only unpinned items.** Pinned items don't consume quota.
3. **Critical operations flush immediately:** pin, unpin, hotkey bind, delete.
4. **Normal changes use deferred writes:** SetTimer 800ms coalesces consecutive copies into one write.
5. **Exit forces a flush:** both tray-exit and WM_ENDSESSION trigger a save.
6. **Atomic writes:** write tmp → FlushFileBuffers → MoveFileExW(MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH). Power failure at worst reverts to the previous complete version.
7. **Corruption protection:** if store.dat fails validation at startup, it is renamed to `store.corrupt.<timestamp>.dat` (never overwritten or deleted). The program starts with an empty database and notifies the user.
8. **Deferred image deletion:** image files are only removed after the corresponding item is confirmed deleted from the database.

---

## Async Disk Writer (AsyncWriter)

Disk I/O runs on a dedicated background thread to avoid blocking the UI.

- The UI thread calls `Store::Serialize()` (microsecond-level in-memory serialization) and submits the buffer to AsyncWriter
- Internals: CRITICAL_SECTION protects shared state; auto-reset Event wakes the worker thread
- Concurrency control: a new submission replaces any queued (not-yet-started) data — no accumulation; at most one write operation executes at any time
- `std::atomic<bool> busy` flag allows the UI thread to query write status
- `Stop()` waits for the in-flight write to complete before the thread exits

---

## Large Data Protection

- When total data size (sum of all items' data fields) exceeds 100 MB, a prompt appears
- On user confirmation, non-pinned items whose individual size exceeds `LargeItemThresholdMB` are deleted
- The prompt appears at most once per program run

---

## Clipboard Monitoring

### Mechanism

Uses `AddClipboardFormatListener(hwnd)` to receive `WM_CLIPBOARDUPDATE`. This is the official post-Vista API — no polling, zero idle CPU usage, and immune to chain breakage from crashed clipboard viewers.

### Processing Pipeline

```
WM_CLIPBOARDUPDATE
  ├─ Sequence check: GetClipboardSequenceNumber() == our last write → discard
  │  (prevents self-paste from creating duplicates)
  ├─ OpenClipboard retry: 50ms intervals, max 5 attempts; give up on total failure
  ├─ Exclusion check: "Clipboard Viewer Ignore" or
  │  "ExcludeClipboardContentFromMonitorProcessing" present → discard
  │  (respects password manager privacy flags)
  ├─ Content extraction (by priority):
  │    CF_UNICODETEXT → text item (discarded if > MaxTextBytes)
  │    CF_DIBV5 / CF_DIB / CF_BITMAP → image item
  │    CF_HDROP → file-drop item (stores path list only)
  │    HTML Format → HTML item
  │    Rich Text Format → RTF item
  │    otherwise → discard
  ├─ Deduplication: identical content already exists → move existing item to front
  └─ Insert → eviction check (unpinned only) → deferred save
```

### Image Handling

- **Capture:** prefers CF_DIBV5 (includes alpha channel info), falls back to CF_DIB, then CF_BITMAP
- **Storage:** WIC-encodes to PNG on disk. A 1920×1080 screenshot is ~8MB as DIB, typically a few hundred KB as PNG
- **Paste back:** decodes PNG to 32-bit DIB; sets both CF_DIBV5 and CF_DIB on the clipboard for legacy compatibility
- **Thumbnails:** generated on demand when popup opens; height matches row height; max 24 cached; released on close

---

## Paste Execution

### Target Window Tracking

`SetWinEventHook(EVENT_SYSTEM_FOREGROUND)` tracks foreground window changes continuously, filtering out:

- Own process windows
- Shell_TrayWnd (taskbar)
- Progman / WorkerW (desktop)
- NotifyIconOverflowWindow (tray overflow)
- Windows.UI.Core.CoreWindow (Start menu / Search)

The most recent window passing the filter is recorded as the paste target. For direct hotkey paste, the current foreground window is used.

### Paste Sequence

1. Write item content to system clipboard (OpenClipboard → EmptyClipboard → SetClipboardData → CloseClipboard); record sequence number for self-filtering
2. Hide popup
3. `SetForegroundWindow(target)` to restore focus; on failure, AttachThreadInput + SetForegroundWindow + BringWindowToTop as fallback
4. `Sleep(PasteDelayMs)` to let the target window acquire focus
5. Modifier key correction: use GetAsyncKeyState to detect physical state of Shift/Alt/Win/main key; send keyup for keys that shouldn't be held
6. SendInput: Ctrl↓ V↓ V↑ Ctrl↑
7. Restore modifier keys to their physical state
8. Update item usedAt; move item to front of list (pinned items stay in pinned section)

### Why Modifier Correction Matters

When pasting via Ctrl+1, the user's fingers are still holding Ctrl and 1. Sending Ctrl+V directly would produce a garbled key combination at the target. The solution: detect and release extraneous physical keys before sending, then restore afterward. Synthetic events carry `KEYEVENTF_EXTENDEDKEY` to distinguish left/right keys, and `INPUT.dwExtraInfo` carries a signature value to identify our own events.

### No Clipboard Restoration

After paste, the clipboard retains the pasted content — no restoration of previous content. Rationale: target applications may read the clipboard asynchronously (Office, some browsers); restoring too fast pastes stale content, restoring too slow conflicts with the user's next copy operation.

---

## Hotkey System

### Registration

All hotkeys are registered on the hidden main window via `RegisterHotKey`, dispatched by id in `WM_HOTKEY`:

- id 1: open quick-paste popup (default Ctrl+Alt+V)
- id 1000 + n: hotkey for pinned position n

Modifiers support any combination of Ctrl / Alt / Shift / Win + one main key. All registrations include `MOD_NOREPEAT` to prevent key-repeat spam.

### Position-Based Binding

Hotkeys bind to the first 10 pinned *positions*, not to specific item content. Whichever item occupies a position inherits that position's hotkey. The settings UI arranges 10 positions in a 2×5 grid.

### Conflict Handling

- Binding fails in settings UI: shows "hotkey is occupied by another application" message; binding is not saved
- Batch registration partially fails at startup: tray balloon notifies "N hotkeys failed to register"

### Risk Warnings

When the user selects system-common combinations (Ctrl+number, Ctrl+C/V/X, Alt+Tab), a gray hint explains the impact. The user can still save after acknowledging.

---

## UI Implementation

All UI uses native Win32 controls + GDI custom drawing. No UI framework. Font is the system UI font from `SystemParametersInfo(SPI_GETNONCLIENTMETRICS)`.

### Tray Icon

- Left click: open quick-paste popup
- Right click: context menu (Quick Paste / Settings / Auto-start / Clear History / About / Exit)
- Tooltip: `ClipWiz — N items, M pinned`

### Quick-Paste Popup

```
┌──────────────────────────────────────────┐
│  ClipWiz                            [×]  │  ← title bar, draggable
├──────────────────────────────────────────┤
│  Type to filter...                       │  ← filter box, focused on open
├──────────────────────────────────────────┤
│  📌 1  book-token-2024xxxx      Ctrl+1   │  ← pinned section
│  📌 2  another password        Ctrl+Alt+2│
│     3  https://example.com/download…     │  ← history, numbering continues
│     4  [Image 1920×1080]                 │
│     5  some text copied earlier…         │
├──────────────────────────────────────────┤
│  Enter paste · Alt+N direct paste · ...  │  ← shortcut hint bar
└──────────────────────────────────────────┘
```

Implementation details:

- Global sequential numbering: pinned starts at 1, history continues without gaps
- Pin icon and number column have unified width for visual alignment
- Size: 420dip wide, height from RowsVisible setting
- Position: mouse pointer / text caret / last opened position (draggable)
- Keyboard: ↑↓ navigate, Enter paste, Esc close, Alt+1–9 direct paste, Ctrl+D delete, Ctrl+P toggle pin
- Mouse: click select, double-click paste, right-click context menu
- Ctrl+hover preview: full text for long items, enlarged image for image items
- Pinned items support drag-to-reorder
- Auto-hides on focus loss (WM_ACTIVATE / WA_INACTIVE)
- Double-buffered drawing (memory DC + BitBlt), flicker-free
- Window created once, shown/hidden thereafter

### Settings Dialog

Multi-tab dialog:

**[General]**
- Launch at startup (checkbox)
- Max history items (numeric input)
- Item expiry in days (numeric input)
- Language (dropdown; defaults to system locale; falls back to English)
- Theme (follow system / light / dark)
- Popup position (mouse pointer / text caret / last position)
- Display font (button showing font name + size, with "Reset" button)
- Data directory (read-only path + browse button)

**[Supported Types]**
- Left panel: list of formats (text, image, HTML, RTF, file drop)
- Right panel: description and typical source applications

**[Hotkeys]**
- Popup hotkey (hotkey control)
- Pinned position hotkeys (10 positions, 2×5 grid, each with hotkey control + "Include Win" checkbox)

### Dark Mode

Reads `HKCU\...\Themes\Personalize\AppsUseLightTheme`. When Theme=auto, follows system setting. Listens for `WM_SETTINGCHANGE` to switch in real time. Only drawing color constants change.

### High DPI

Manifest declares Per-Monitor V2. All dimension constants stored in dip; converted to physical pixels via `GetDpiForWindow` at draw time. Handles `WM_DPICHANGED` for crisp cross-monitor dragging.

---

## Internationalization (i18n)

- English is built into the code (kDefaults table in `i18n.cpp`) as the ultimate fallback
- Language file format: `lang/<locale>.lng`, UTF-8 encoded, `key=value` plain text
- Load priority: Language setting in config.ini → system UI language → English
- All user-visible strings go through `i18n::T(key)`; no hardcoded UI text in code
- Ships with Simplified Chinese pack `lang/zh-CN.lng`
- Supports `%s`, `%d`, `%u` placeholders via `util::Format`

---

## Process Lifecycle

- **Single instance:** `CreateMutexW(L"Local\\ClipWiz.SingleInstance")`. If mutex exists, broadcasts a custom message to show the first instance's popup, then exits.
- **No visible main window:** only a hidden message-only window (WS_POPUP, never shown). Does not appear in taskbar or Alt+Tab.
- **Logoff/shutdown:** handles WM_QUERYENDSESSION / WM_ENDSESSION; flushes data before allowing system to proceed.
- **Exit cleanup:** RemoveClipboardFormatListener → UnregisterHotKey → UnhookWinEvent → Shell_NotifyIcon(NIM_DELETE) → forced save.
- **Auto-start:** writes `HKCU\Software\Microsoft\Windows\CurrentVersion\Run\ClipWiz` with exe path + `--autostart`. No admin privileges required.

---

## Build Configuration

### Compiler Options

| Option | Purpose |
| --- | --- |
| /W4 /WX | Maximum warning level; warnings are errors |
| /permissive- | Strict standards conformance |
| /utf-8 | Source and execution charset UTF-8 |
| /GR- | Disable RTTI |
| /Gy | Function-level linking (enables /OPT:REF to strip unused code) |
| /MT | Static CRT linkage; no VC runtime dependency |
| /OPT:REF /OPT:ICF | Release: remove unreferenced functions, merge identical function bodies |
| /INCREMENTAL:NO | Release: disable incremental linking |

### Linked Libraries

user32, gdi32, msimg32, shell32, comctl32, advapi32, ole32, windowscodecs

All ship with the Windows SDK. Zero third-party dependencies.

### Expected Metrics

- Executable size: 200–350 KB
- Idle resident memory: 4–8 MB
- Cold start: < 100ms
- Popup display: < 30ms

---

## Supported Content Types

| Format | Description | Typical Source |
| --- | --- | --- |
| CF_UNICODETEXT | Plain Unicode text | Nearly all applications |
| CF_DIB / CF_DIBV5 | Bitmap image | Screenshot tools, image editors |
| HTML Format | Rich web content | Browsers, email clients |
| Rich Text Format | Formatted document content | Word, WordPad |
| CF_HDROP | File path list | Explorer (stores paths only, not file contents) |

---

## Known Limitations

| Scenario | Handling |
| --- | --- |
| Target window runs as administrator | ClipWiz must also run elevated |
| SetForegroundWindow denied by system | AttachThreadInput fallback |
| Hotkey conflicts with system/common apps | Settings UI warns explicitly; user can rebind |
| Image items consume disk | PNG compression + soft disk budget + only evict unpinned images |
| Antivirus sensitivity to clipboard monitoring + key simulation | No network, no injection, no hook DLLs — official APIs only |
| Input method state | Only sends Ctrl+V combo, never character-by-character input |

---
---

# ClipWiz 技术文档

本文档包含 ClipWiz 的详细技术实现说明。项目概览和快速开始见 [README.md](../README.md)。

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

内存中用 `std::vector<Item>` 存储，顺序即显示顺序：置顶区在前，历史区按最近使用时间排列。图片像素数据不常驻内存，按需从磁盘读取。

### 磁盘布局

```
数据目录（默认为 exe 同目录，可在设置中自定义）
    config.ini          全局设置（UTF-8 文本）
    store.dat           条目库（自定义二进制格式）
    store.dat.tmp       写入时的临时文件，完成后原子替换
    images\
        000000000123.png  图片条目，文件名为条目 id
```

### store.dat 二进制格式

小端序，定长头 + 变长记录序列。

```
Header (32 bytes)
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

选择二进制而非 JSON/INI 的原因：剪贴板文本中必然包含换行、引号、制表符、表情等字符，文本格式需要转义处理，存在解析歧义；二进制按长度字段读写，无歧义且无需解析库。

### config.ini

```ini
[General]
MaxHistory=50            ; 未置顶条目上限，范围 5~2000
Autostart=0              ; 开机自启
ExpiryDays=0             ; 过期天数，0=不过期
Language=                ; 语言代码，空=跟随系统
Theme=auto               ; auto / light / dark
PopupPosition=mouse      ; mouse / caret / last
FontName=                ; 自定义字体名，空=系统默认
FontHeight=0             ; 自定义字号，0=系统默认
DataDir=                 ; 数据目录，空=exe 同目录

[Paste]
PopupHotkey=Ctrl+Alt+V   ; 唤出快速粘贴框
PasteDelayMs=60          ; 焦点切回后到发 Ctrl+V 的等待
CloseAfterPaste=1        ; 粘贴后关闭弹出框

[Limits]
MaxTextBytes=1048576     ; 超过此大小的文本不入池（1MB）
MaxImagePixels=33177600  ; 超过约 8K×4K 的图片不入池
ImageDiskBudgetMB=200    ; images 目录软上限
LargeItemThresholdMB=10  ; 清理大条目时的阈值（1~500）

[UI]
RowsVisible=10           ; 弹出框一屏显示行数
```

---

## 保存策略

数据持久化是核心可靠性保证，规则如下：

1. **淘汰只作用于未置顶条目。** 置顶项在任何代码路径下都不参与自动删除。唯一能移除置顶项的操作是用户明确点击"删除"并通过确认框。
2. **条数上限只统计未置顶条目。** 置顶项不占额度。
3. **关键操作立即落盘：** 置顶、取消置顶、绑定快捷键、删除。
4. **普通变化延迟合并：** SetTimer 800ms，连续复制时合并为一次写入。
5. **退出时强制落盘：** 托盘退出和 WM_ENDSESSION 均触发。
6. **原子写入：** 写 tmp → FlushFileBuffers → MoveFileExW(MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)。中途断电最坏回退到上一个完整版本。
7. **损坏保护：** 启动时 store.dat 校验失败不覆盖不清空，改名为 `store.corrupt.<时间戳>.dat` 保留，以空库启动并提示用户。
8. **图片文件延迟删除：** 仅在条目确认从库中移除后才删除对应的 images 目录文件。

---

## 异步写盘（AsyncWriter）

写盘操作由独立后台线程执行，避免磁盘 I/O 阻塞 UI。

- 主线程调用 `Store::Serialize()` 完成内存序列化（微秒级），将 buffer 提交给 AsyncWriter
- AsyncWriter 内部：CRITICAL_SECTION 保护共享状态 + auto-reset Event 唤醒线程
- 并发控制：新请求替换排队中的旧数据（不堆积），同一时刻最多一个写操作在执行
- `std::atomic<bool> busy` 标志供主线程查询写盘状态
- 退出时 `Stop()` 等待当前写操作完成后线程退出

---

## 大数据保护

- 总数据量（所有条目 data 字段之和）超过 100MB 时弹出提示
- 用户确认后，删除非置顶且单条数据超过 `LargeItemThresholdMB` 的条目
- 每次程序运行只提示一次，避免反复打扰

---

## 剪贴板监听

### 监听机制

使用 `AddClipboardFormatListener(hwnd)` 接收 `WM_CLIPBOARDUPDATE` 消息。这是 Windows Vista 之后的官方 API，无需轮询，空闲时 CPU 占用为 0，不会因链式钩子上某程序崩溃而断链。

### 处理流程

```
WM_CLIPBOARDUPDATE
  ├─ 序列号检查：GetClipboardSequenceNumber() == 自己上次写入值 → 丢弃
  │  （防止自己粘贴时写的内容被当成新条目）
  ├─ OpenClipboard 重试：失败则 50ms 后重试，最多 5 次；全失败放弃
  ├─ 排除标记检查：存在 "Clipboard Viewer Ignore" 或
  │  "ExcludeClipboardContentFromMonitorProcessing" 格式 → 丢弃
  │  （尊重密码管理器的隐私标记）
  ├─ 取内容（按优先级）：
  │    CF_UNICODETEXT → 文本条目（超 MaxTextBytes 丢弃）
  │    CF_DIBV5 / CF_DIB / CF_BITMAP → 图片条目
  │    CF_HDROP → 文件拖放条目（只存路径列表）
  │    HTML Format → HTML 条目
  │    Rich Text Format → RTF 条目
  │    否则 → 丢弃
  ├─ 去重：内容与已有条目完全相同 → 不新增，将旧条目提到最前
  └─ 入池 → 淘汰检查（只动未置顶）→ 延迟保存
```

### 图片处理

- **读取：** 优先 CF_DIBV5（含 alpha 通道信息），退化到 CF_DIB，再退化到 CF_BITMAP
- **存储：** WIC 编码为 PNG 落盘。1920×1080 截图 DIB 约 8MB，PNG 通常几百 KB
- **写回剪贴板：** PNG 解码为 32 位 DIB，同时设置 CF_DIBV5 和 CF_DIB 两种格式，兼容旧程序
- **缩略图：** 弹出框打开时按需生成，高度等于行高，最多缓存 24 张，关框后释放

---

## 粘贴执行

### 目标窗口跟踪

使用 `SetWinEventHook(EVENT_SYSTEM_FOREGROUND)` 全程跟踪前台窗口变化，过滤掉：

- 本进程窗口
- Shell_TrayWnd（任务栏）
- Progman / WorkerW（桌面）
- NotifyIconOverflowWindow（托盘溢出区）
- Windows.UI.Core.CoreWindow（开始菜单/搜索）

记录最近一个通过过滤的 HWND 作为粘贴目标。热键直接粘贴时取当前前台窗口。

### 粘贴步骤

1. 将条目内容写入系统剪贴板（OpenClipboard → EmptyClipboard → SetClipboardData → CloseClipboard），记录序列号供自我过滤
2. 隐藏弹出框
3. `SetForegroundWindow(target)` 归还焦点；失败则 AttachThreadInput + SetForegroundWindow + BringWindowToTop 兜底
4. `Sleep(PasteDelayMs)` 等待目标窗口获得焦点
5. 修正修饰键状态：用 GetAsyncKeyState 检测 Shift/Alt/Win/主键物理状态，补发 keyup 抬起不该按下的键
6. SendInput 发送 Ctrl↓ V↓ V↑ Ctrl↑
7. 按物理状态还原修饰键
8. 更新条目 usedAt，将条目提到列表最前（置顶项留在置顶区）

### 修饰键修正的必要性

用 Ctrl+1 触发粘贴时，用户手指仍按着 Ctrl 和 1。直接发 Ctrl+V 会导致目标程序收到异常组合键。因此发送前必须检测并抬起多余的物理按键，发送后还原。合成事件标记 `KEYEVENTF_EXTENDEDKEY` 区分左右键，`INPUT.dwExtraInfo` 带签名值标识自身发送的事件。

### 不还原原剪贴板内容

粘贴完成后剪贴板保留刚粘贴的内容，不做还原。原因：目标程序可能异步读取剪贴板（Office、部分浏览器），还原过快会粘到旧内容；还原过慢则用户可能已经复制了新内容。

---

## 快捷键系统

### 注册方式

`RegisterHotKey` 全部注册在隐藏主窗口上，`WM_HOTKEY` 按 id 分发：

- id 1：唤出快速粘贴框（默认 Ctrl+Alt+V）
- id 1000 + n：第 n 个置顶位置的快捷键

修饰键支持 Ctrl / Alt / Shift / Win 任意组合 + 一个主键，一律带 `MOD_NOREPEAT` 防止长按连发。

### 位置绑定模型

快捷键绑定到置顶区的前 10 个"位置"，而非具体条目内容。任何条目进入某位置即继承该位置的快捷键。设置界面中 10 个位置以 2 列 × 5 行网格排列。

### 冲突处理

- 设置界面绑定时 `RegisterHotKey` 失败：弹提示说明被占用，本次绑定不保存
- 启动时批量注册部分失败：托盘气泡提示"N 个快捷键注册失败"

### 风险提示

检测到用户选择 Ctrl+数字、Ctrl+C/V/X、Alt+Tab 等系统常用组合时，界面显示灰色提示文字说明影响，用户确认后仍可保存。

---

## 界面实现

全部使用 Win32 原生控件 + GDI 自绘，不引入 UI 框架。字体取 `SystemParametersInfo(SPI_GETNONCLIENTMETRICS)` 中的系统 UI 字体。

### 托盘图标

- 左键单击：打开快速粘贴框
- 右键：菜单（快速粘贴 / 设置 / 开机自启 / 清空历史 / 关于 / 退出）
- 悬浮提示：`ClipWiz — N 条记录，M 个置顶`

### 快速粘贴框

```
┌──────────────────────────────────────────┐
│  ClipWiz                            [×]  │  ← 标题栏，可拖动
├──────────────────────────────────────────┤
│  输入关键字过滤...                        │  ← 过滤框，打开即聚焦
├──────────────────────────────────────────┤
│  📌 1  book-token-2024xxxx      Ctrl+1   │  ← 置顶区
│  📌 2  另一个常用口令           Ctrl+Alt+2│
│     3  https://example.com/download…     │  ← 历史区，编号顺延
│     4  [图片 1920×1080]                  │
│     5  上次复制的一段文字…                │
├──────────────────────────────────────────┤
│  Enter 粘贴 · Alt+数字 直接粘贴 · ...    │  ← 快捷键提示栏
└──────────────────────────────────────────┘
```

实现要点：

- 编号全局连续：置顶从 1 开始，非置顶顺延，不重复
- 置顶项图钉图标与编号区域统一宽度，视觉对齐
- 尺寸：宽 420dip，高按 RowsVisible 配置计算
- 弹出位置：鼠标指针处 / 光标处 / 上次打开的位置（可拖动）
- 键盘操作：↑↓ 移动、Enter 粘贴、Esc 关闭、Alt+1~9 直接粘贴、Ctrl+D 删除、Ctrl+P 切换置顶
- 鼠标操作：单击选中、双击粘贴、右键上下文菜单
- Ctrl 悬停预览：长文本显示完整内容，图片显示放大预览
- 置顶项支持拖动调整顺序
- 失去焦点自动隐藏（WM_ACTIVATE / WA_INACTIVE）
- 双缓冲绘制（内存 DC + BitBlt），无闪烁
- 窗口只创建一次，后续 show/hide 复用

### 设置对话框

多 Tab 页：

**[常规]**
- 开机自动启动（checkbox）
- 保存复制项目的最大数量（数字框）
- 粘贴条目的过期天数（数字框）
- 语言（下拉框，默认系统语言，不匹配回退英文）
- 主题（跟随系统 / 浅色 / 深色）
- 窗口弹出位置（鼠标指针处 / 光标处 / 上次打开的位置）
- 显示字体（按钮显示字体名+字号，附"恢复默认"按钮）
- 数据库路径（只读路径框 + 浏览按钮）

**[支持类型]**
- 左栏列表：纯文本、图片、HTML、RTF、文件拖放
- 右栏：格式说明、产生该格式的典型软件

**[快捷键]**
- 唤出快速粘贴框（hotkey 控件）
- 置顶项快捷键（10 个位置，2×5 网格，每个含 hotkey 控件 + "含Win" checkbox）

### 深色模式

读取注册表 `HKCU\...\Themes\Personalize\AppsUseLightTheme`，Theme=auto 时跟随系统设置。监听 `WM_SETTINGCHANGE` 实时切换，只更换绘制颜色常量。

### 高 DPI

manifest 声明 Per-Monitor V2。所有尺寸常量以 dip 存储，绘制时用 `GetDpiForWindow` 换算为物理像素。处理 `WM_DPICHANGED` 消息，跨屏拖动不模糊。

---

## 国际化（i18n）

- 英文内置于代码（`i18n.cpp` 中的 kDefaults 表），作为终极回退
- 语言文件格式：`lang/<locale>.lng`，UTF-8 编码，`key=value` 纯文本
- 加载优先级：config.ini 中 Language 设置 → 系统 UI 语言 → 英文
- 所有用户可见文字通过 `i18n::T(key)` 获取，代码中不存在硬编码界面文字
- 自带简体中文语言包 `lang/zh-CN.lng`
- 支持 `%s`、`%d`、`%u` 占位符，通过 `util::Format` 填充

---

## 进程与生命周期

- **单实例：** `CreateMutexW(L"Local\\ClipWiz.SingleInstance")`，已存在则广播自定义消息让第一个实例弹出粘贴框，自身退出
- **无主窗口：** 仅一个消息驱动的隐藏窗口（WS_POPUP，不 ShowWindow），不出现在任务栏和 Alt+Tab
- **注销/关机：** 处理 WM_QUERYENDSESSION / WM_ENDSESSION，先落盘再放行
- **退出清理：** RemoveClipboardFormatListener → UnregisterHotKey → UnhookWinEvent → Shell_NotifyIcon(NIM_DELETE) → 强制保存
- **开机自启：** 写 `HKCU\Software\Microsoft\Windows\CurrentVersion\Run\ClipWiz`，值为 exe 路径 + `--autostart`，无需管理员权限

---

## 构建配置

### 编译选项

| 选项 | 作用 |
| --- | --- |
| /W4 /WX | 最高警告级别，警告视为错误 |
| /permissive- | 严格标准一致性 |
| /utf-8 | 源文件和执行字符集均为 UTF-8 |
| /GR- | 关闭 RTTI |
| /Gy | 函数级链接（配合 /OPT:REF 裁剪未用代码） |
| /MT | 静态链接 CRT，产物不依赖 VC 运行库 |
| /OPT:REF /OPT:ICF | Release 下移除未引用函数、合并相同函数体 |
| /INCREMENTAL:NO | Release 下关闭增量链接 |

### 链接库

user32、gdi32、msimg32、shell32、comctl32、advapi32、ole32、windowscodecs

均为 Windows SDK 自带，无第三方依赖。

### 预期产物指标

- exe 体积：200~350 KB
- 空闲常驻内存：4~8 MB
- 冷启动：< 100ms
- 弹出框显示：< 30ms

---

## 支持的内容类型

| 格式 | 说明 | 典型来源 |
| --- | --- | --- |
| CF_UNICODETEXT | 纯 Unicode 文本 | 几乎所有程序 |
| CF_DIB / CF_DIBV5 | 位图图像 | 截图工具、图片编辑器 |
| HTML Format | 富网页内容 | 浏览器、邮件客户端 |
| Rich Text Format | 带格式文档 | Word、写字板 |
| CF_HDROP | 文件路径列表 | 资源管理器（只存路径，不存文件本身） |

---

## 已知限制与对策

| 场景 | 处理方式 |
| --- | --- |
| 目标窗口为管理员权限程序 | 需以管理员身份运行 ClipWiz |
| SetForegroundWindow 被系统拒绝 | AttachThreadInput 兜底 |
| 快捷键与系统/常用软件冲突 | 设置界面明确提示，用户可自行更换 |
| 图片条目占磁盘 | PNG 压缩 + 磁盘预算软上限 + 只淘汰未置顶图片 |
| 杀毒软件对剪贴板监听+模拟按键敏感 | 不联网、不注入、不装钩子 DLL，仅用官方 API |
| 输入法状态 | 只发 Ctrl+V 组合键，不逐字符输入，不受输入法影响 |
