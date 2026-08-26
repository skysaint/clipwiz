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
    std::vector<uint8_t> data; // unified binary content (text=UTF-16LE, image=PNG, etc.)
    uint32_t     imgW, imgH;  // pixel dimensions (kind==Image only)
    std::wstring preview;     // one-line summary, recomputed at runtime, not persisted
    uint64_t     hash;        // FNV-1a hash for dedup, computed at runtime, not persisted
};
```

Items are stored in a `std::vector<Item>`; vector order equals display order (pinned section first in manual order, unpinned sorted by usedAt descending). All content (text/image/HTML/RTF/file list) is stored uniformly as a binary blob in the `data` field. Hotkeys are not tied to items; they are managed positionally via config (the Nth pinned position gets the Nth hotkey).

### Disk Layout

```
Data directory (defaults to exe directory; configurable in settings)
    config.ini          Global settings (UTF-8 flat key=value text)
    store.dat           Item database (custom binary format, all content inline)
    store.dat.tmp       Temporary file during write; atomically replaced on completion
    clipwiz.log         Runtime log file (append mode)
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
    uint32   imgW
    uint32   imgH
    uint32   dataLen      byte count of unified content blob
    uint8    data[dataLen]
```

Binary format chosen over JSON/INI because clipboard text inevitably contains newlines, quotes, tabs, and emoji — text formats require escaping and introduce parsing ambiguity. Binary read/write by length fields is unambiguous and requires no parser library.

### config.ini

Flat key=value format, no sections. UTF-8 encoded.

```ini
MaxHistory=50            ; unpinned item limit, range 5–2000
ExpiryDays=5             ; expiry in days, 0 = never
PasteDelayMs=60          ; delay after focus restore before sending Ctrl+V
RowsVisible=10           ; visible rows in popup
PopupPosition=0          ; 0=mouse / 1=caret / 2=last position
Theme=0                  ; 0=auto / 1=light / 2=dark
MaxTextBytes=1048576     ; text larger than this is not captured (1MB)
MaxImagePixels=33177600  ; images larger than ~8K×4K are not captured
LargeItemThresholdMB=10  ; threshold for large-item cleanup (1–500)
LastPopupX=-1            ; last popup window position
LastPopupY=-1
PopupHotkey=196631       ; encoded: high16=MOD_* flags, low16=VK code
PinnedHotkey0=0          ; hotkey for pinned position 0 (0=unbound)
Language=zh-CN           ; locale code, empty = follow system
DataDir=                 ; data directory (relative or absolute), empty = exe directory
FontName=                ; custom font name, empty = system default
FontSize=0               ; custom font point size, 0 = system default
```

---

## Persistence Strategy

Data durability is a core reliability guarantee:

1. **Eviction only affects unpinned items.** Pinned items never participate in automatic deletion under any code path. The only way to remove a pinned item is explicit user deletion with confirmation.
2. **History limit counts only unpinned items.** Pinned items don't consume quota.
3. **Critical operations flush immediately:** pin, unpin, hotkey bind, delete.
4. **Normal changes use deferred writes:** SetTimer 800ms coalesces consecutive copies into one write.
5. **Exit forces a flush:** both tray-exit and WM_ENDSESSION trigger a save. On exit, if an async write is in progress, the main thread waits up to 5 seconds for completion.
6. **Atomic writes:** write tmp → FlushFileBuffers → MoveFileExW(MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH). Power failure at worst reverts to the previous complete version.
7. **Corruption protection:** if store.dat fails validation at startup, it is renamed to `store.corrupt.<timestamp>.dat` (never overwritten or deleted). The program starts with an empty database and notifies the user.

---

## Async Disk Writer (AsyncWriter)

Disk I/O runs on a dedicated background thread to avoid blocking the UI.

- The UI thread calls `Store::Serialize()` (microsecond-level in-memory serialization) and submits the buffer to AsyncWriter
- Internals: CRITICAL_SECTION protects shared state; auto-reset Event wakes the worker thread
- Concurrency control: a new submission replaces any queued (not-yet-started) data — no accumulation; at most one write operation executes at any time
- `std::atomic<bool> busy` flag allows the UI thread to query write status
- On exit, the main thread saves data synchronously first, then calls `Stop()` which signals the thread to terminate (does not block waiting)

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
  ├─ OpenClipboard: single attempt; give up on failure
  ├─ Exclusion check: "Clipboard Viewer Ignore" or
  │  "ExcludeClipboardContentFromMonitorProcessing" present → discard
  │  (respects password manager privacy flags)
  ├─ Content extraction (by priority):
  │    Rich Text Format → RTF item (discarded if > MaxTextBytes)
  │    HTML Format → HTML item (discarded if > MaxTextBytes)
  │    CF_DIBV5 / CF_DIB / CF_BITMAP → image item (discarded if > MaxImagePixels)
  │    CF_HDROP → file-drop item (stores path list only)
  │    CF_UNICODETEXT → text item (discarded if > MaxTextBytes)
  │    otherwise → discard
  ├─ Deduplication: identical content already exists → move existing item to front
  └─ Insert → eviction check (unpinned only) → deferred save
```

### Image Handling

- **Capture:** prefers CF_DIBV5 (includes alpha channel info), falls back to CF_DIB, then CF_BITMAP
- **Storage:** WIC-encodes to PNG bytes stored inline in store.dat's data blob. A 1920×1080 screenshot is ~8MB as DIB, typically a few hundred KB as PNG
- **Paste back:** decodes PNG to 32-bit DIB; sets CF_DIBV5, CF_DIB, and PNG format on the clipboard for maximum compatibility
- **Thumbnails:** generated on demand when popup is visible; max 32 cached; released on DPI change or window destroy

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
5. Modifier key release: use GetAsyncKeyState to detect physical state of Ctrl/Alt/Shift/Win (left+right variants); send keyup for any that are currently held
6. SendInput: Ctrl↓ V↓ V↑ Ctrl↑ (scan codes filled via MapVirtualKeyW)
7. Update item usedAt; move item to front of list (pinned items stay in pinned section)

### Why Modifier Release Matters

When pasting via Ctrl+1, the user's fingers are still holding Ctrl and 1. Sending Ctrl+V directly would produce a garbled key combination at the target (e.g. Ctrl+Alt+V). The solution: detect all physically-held modifier keys and send synthetic keyup events before issuing Ctrl+V. Released modifiers are not re-pressed afterward — the user has already lifted them by the time paste completes.

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
- Tooltip: `ClipWiz` (static; updated via `tray::SetTip` if needed)

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
- Size: 520dip wide, height from RowsVisible setting
- Position: mouse pointer / text caret / last opened position (draggable)
- Keyboard: ↑↓ navigate, Enter paste, Esc close, Alt+1–9 direct paste, Ctrl+D delete, Ctrl+P toggle pin
- Mouse: click select, double-click paste, right-click context menu
- Ctrl+hover preview: full text for long items, enlarged image for image items
- Pinned items support drag-to-reorder
- Auto-hides on focus loss (WM_ACTIVATE / WA_INACTIVE)
- Double-buffered drawing (memory DC + BitBlt), flicker-free
- Window created once, shown/hidden thereafter

### Settings Dialog

Single centered window with grouped sections (no Property Sheet / multi-tab). Built at runtime via in-memory DLGTEMPLATE + `DialogBoxIndirectParamW`; all controls created dynamically in `WM_INITDIALOG`. No dialog resources in .rc file.

**[General section]**
- Launch at startup (checkbox)
- Max history items (numeric input)
- Item expiry in days (numeric input)
- Language (dropdown: Follow system / English / 简体中文)
- Theme (follow system / light / dark)
- Popup position (mouse pointer / text caret / last position)
- Display font (button showing font name + size, with "Reset" button; uses ChooseFontW)
- Data directory (read-only path + browse button; uses SHBrowseForFolderW)

**[Shortcuts section]**
- Popup hotkey (hotkey control + "Win" checkbox)
- Pinned position hotkeys (10 positions, 2×5 grid, each with hotkey control + "Win" checkbox)

### Dark Mode

Reads `HKCU\...\Themes\Personalize\AppsUseLightTheme`. When Theme=auto, follows system setting. Listens for `WM_SETTINGCHANGE` to switch in real time. Only drawing color constants change.

### High DPI

Manifest declares Per-Monitor V2. All dimension constants stored in dip; converted to physical pixels via `GetDpiForWindow` at draw time. Handles `WM_DPICHANGED` for crisp cross-monitor dragging.

---

## Internationalization (i18n)

- English is built into the code (kDefaults table in `i18n.cpp`) as the ultimate fallback
- Simplified Chinese is compiled into the exe as an RCDATA resource (`lang/zh-CN.lng` → `IDR_LNG_ZHCN`)
- Language file format: `lang/<locale>.lng`, UTF-8 encoded, `key=value` plain text
- Load priority: external disk file `lang/<locale>.lng` → built-in resource (zh-CN only) → English
- External file always takes priority over built-in resource, allowing users to override translations without recompiling
- Other languages (Japanese, Korean, etc.) are loaded exclusively from disk; users distribute .lng files independently
- All user-visible strings go through `i18n::T(key)`; no hardcoded UI text in code
- Supports `%s`, `%d`, `%u` placeholders via `util::Format`

---

## Logging

Lightweight file logger (`log.h` / `log.cpp`):

- Output file: `<dataDir>\clipwiz.log`, append mode, each entry flushed immediately (`fflush`)
- Format: `[YYYY-MM-DD HH:MM:SS.mmm] [LEVEL] message`
- Levels: DEBUG / INFO / WARN / ERROR; minimum level configurable at init
- Thread safety: CRITICAL_SECTION guards concurrent writes
- No rotation or size limit — log is expected to remain small under normal operation
- Primary use: startup diagnostics, hotkey registration failures, save errors

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

user32, gdi32, msimg32, shell32, comctl32, advapi32, ole32, windowscodecs, comdlg32

All ship with the Windows SDK. Zero third-party dependencies.

### Expected Metrics

- Executable size: 200–350 KB
- Idle resident memory: 4–8 MB
- Cold start: < 100ms
- Popup display: < 30ms

### Source Layout

```
src/
  main.cpp           Entry, single-instance check, message loop
  app.h/.cpp         Global state, message dispatch
  store.h/.cpp       Item database, serialization, eviction
  clipboard.h/.cpp   Clipboard monitoring and read/write
  hotkey.h/.cpp      Global hotkey management
  paste.h/.cpp       Paste execution
  popup.h/.cpp       Quick-paste popup window
  settings.h/.cpp    Configuration and settings dialog
  imagecodec.h/.cpp  WIC image codec
  tray.h/.cpp        Tray icon
  i18n.h/.cpp        Internationalization
  asyncwriter.h/.cpp Async disk writer
  log.h/.cpp         Lightweight file logging
  raii.h             RAII wrappers (GlobalLock, HANDLE, GDI objects)
  util.h/.cpp        Utility functions
  resource.h         Control IDs and resource constants
  clipwiz.rc         Icon, manifest, version info (no dialog templates)
lang/
  zh-CN.lng          Simplified Chinese language pack
```

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
| Image items consume disk | PNG compression + large-data protection prompt + only evict unpinned items |
| Antivirus sensitivity to clipboard monitoring + key simulation | No network, no injection, no hook DLLs — official APIs only |
| Input method state | Only sends Ctrl+V combo, never character-by-character input |

---

## Version History

### v1.2.0

- **Per-row type icons.** Every entry now shows a fixed-width type indicator (TXT / RTF / HTML / image / file list), each with a distinct shape and a fixed per-type color (with separate light/dark palettes) — a double cue that reads even at small sizes. The pin / type-icon / index columns are equally spaced and left-aligned across all rows.
- **"Convert to plain text" context menu.** Right-clicking an RTF or HTML entry offers an in-place conversion to plain text (same entry, same position — not a new copy). If the result duplicates an existing text entry, the two are merged: the entry earlier in display order is kept (which keeps a pinned duplicate across groups); when both are pinned, neither is removed. The surviving entry becomes selected.
- **Canonical deduplication.** Dedup now hashes the *extracted plain-text body* plus a per-kind prefix, instead of raw clipboard bytes. This fixes duplicate rich-text entries piling up when the same passage is copied repeatedly from Word/browsers (whose RTF/HTML bytes differ every time). On a dedup hit the newest content is kept, so re-editing formatting before recopying pastes the latest version.
- **Newest-on-top ordering fix.** The "selective front" behavior (pinned stay put; unpinned move to the top of the unpinned group) is consolidated into a single shared function, so newly copied and newly used items reliably land at the top of their group.
- **Settings tooltips.** Each Windows-key icon button in the settings dialog now shows a hover tooltip.

### v1.1.0

- Settings dialog polish and layout improvements; expiry based on last-use time; multilingual display and stability fixes.

### v1.0.0

- Initial release: clipboard history with pin/eviction, quick-paste popup, global hotkeys, image/RTF/HTML/file support, settings dialog, i18n, dark mode, high-DPI.

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
    std::vector<uint8_t> data; // 统一二进制内容（text=UTF-16LE, image=PNG 等）
    uint32_t     imgW, imgH;  // kind==Image：像素尺寸
    std::wstring preview;     // 列表里显示的一行摘要，运行时计算，不持久化
    uint64_t     hash;        // FNV-1a 去重哈希，运行时计算，不持久化
};
```

内存中用 `std::vector<Item>` 存储，顺序即显示顺序：置顶区在前（手动排序），历史区按 usedAt 降序排列。所有内容（文本/图片/HTML/RTF/文件列表）统一以二进制 blob 存于 `data` 字段。快捷键不绑定条目，按位置管理（第 N 个置顶位对应第 N 个快捷键）。

### 磁盘布局

```
数据目录（默认为 exe 同目录，可在设置中自定义）
    config.ini          全局设置（UTF-8 平铺 key=value 文本）
    store.dat           条目库（自定义二进制格式，所有内容内联）
    store.dat.tmp       写入时的临时文件，完成后原子替换
    clipwiz.log         运行日志（追加模式）
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
    uint32   imgW
    uint32   imgH
    uint32   dataLen      统一内容 blob 字节数
    uint8    data[dataLen]
```

选择二进制而非 JSON/INI 的原因：剪贴板文本中必然包含换行、引号、制表符、表情等字符，文本格式需要转义处理，存在解析歧义；二进制按长度字段读写，无歧义且无需解析库。

### config.ini

平铺 key=value 格式，无分节。UTF-8 编码。

```ini
MaxHistory=50            ; 未置顶条目上限，范围 5~2000
ExpiryDays=5             ; 过期天数，0=不过期
PasteDelayMs=60          ; 焦点切回后到发 Ctrl+V 的等待
RowsVisible=10           ; 弹出框一屏显示行数
PopupPosition=0          ; 0=鼠标 / 1=光标 / 2=上次位置
Theme=0                  ; 0=auto / 1=light / 2=dark
MaxTextBytes=1048576     ; 超过此大小的文本不入池（1MB）
MaxImagePixels=33177600  ; 超过约 8K×4K 的图片不入池
LargeItemThresholdMB=10  ; 清理大条目时的阈值（1~500）
LastPopupX=-1            ; 上次弹出窗口位置
LastPopupY=-1
PopupHotkey=196631       ; 编码：高16位=MOD_* 修饰键，低16位=VK 码
PinnedHotkey0=0          ; 置顶位 0 的快捷键（0=未绑定）
Language=zh-CN           ; 语言代码，空=跟随系统
DataDir=                 ; 数据目录（相对或绝对），空=exe 同目录
FontName=                ; 自定义字体名，空=系统默认
FontSize=0               ; 自定义字号，0=系统默认
```

---

## 保存策略

数据持久化是核心可靠性保证，规则如下：

1. **淘汰只作用于未置顶条目。** 置顶项在任何代码路径下都不参与自动删除。唯一能移除置顶项的操作是用户明确点击"删除"并通过确认框。
2. **条数上限只统计未置顶条目。** 置顶项不占额度。
3. **关键操作立即落盘：** 置顶、取消置顶、绑定快捷键、删除。
4. **普通变化延迟合并：** SetTimer 800ms，连续复制时合并为一次写入。
5. **退出时强制落盘：** 托盘退出和 WM_ENDSESSION 均触发。退出时若有异步写入正在进行，主线程最多等待 5 秒。
6. **原子写入：** 写 tmp → FlushFileBuffers → MoveFileExW(MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)。中途断电最坏回退到上一个完整版本。
7. **损坏保护：** 启动时 store.dat 校验失败不覆盖不清空，改名为 `store.corrupt.<时间戳>.dat` 保留，以空库启动并提示用户。

---

## 异步写盘（AsyncWriter）

写盘操作由独立后台线程执行，避免磁盘 I/O 阻塞 UI。

- 主线程调用 `Store::Serialize()` 完成内存序列化（微秒级），将 buffer 提交给 AsyncWriter
- AsyncWriter 内部：CRITICAL_SECTION 保护共享状态 + auto-reset Event 唤醒线程
- 并发控制：新请求替换排队中的旧数据（不堆积），同一时刻最多一个写操作在执行
- `std::atomic<bool> busy` 标志供主线程查询写盘状态
- 退出时主线程先同步保存数据，然后调用 `Stop()` 通知线程退出（不阻塞等待）

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
  ├─ OpenClipboard：单次尝试，失败即放弃
  ├─ 排除标记检查：存在 "Clipboard Viewer Ignore" 或
  │  "ExcludeClipboardContentFromMonitorProcessing" 格式 → 丢弃
  │  （尊重密码管理器的隐私标记）
  ├─ 取内容（按优先级）：
  │    Rich Text Format → RTF 条目（超 MaxTextBytes 丢弃）
  │    HTML Format → HTML 条目（超 MaxTextBytes 丢弃）
  │    CF_DIBV5 / CF_DIB / CF_BITMAP → 图片条目（超 MaxImagePixels 丢弃）
  │    CF_HDROP → 文件拖放条目（只存路径列表）
  │    CF_UNICODETEXT → 文本条目（超 MaxTextBytes 丢弃）
  │    否则 → 丢弃
  ├─ 去重：内容与已有条目完全相同 → 不新增，将旧条目提到最前
  └─ 入池 → 淘汰检查（只动未置顶）→ 延迟保存
```

### 图片处理

- **读取：** 优先 CF_DIBV5（含 alpha 通道信息），退化到 CF_DIB，再退化到 CF_BITMAP
- **存储：** WIC 编码为 PNG 字节，内联存于 store.dat 的 data blob。1920×1080 截图 DIB 约 8MB，PNG 通常几百 KB
- **写回剪贴板：** PNG 解码为 32 位 DIB，同时设置 CF_DIBV5、CF_DIB 和 PNG 三种格式，最大兼容性
- **缩略图：** 弹出框打开时按需生成，最多缓存 32 张，DPI 变化或窗口销毁时释放

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
5. 释放修饰键：用 GetAsyncKeyState 检测 Ctrl/Alt/Shift/Win（左右分别检测）物理状态，对仍按下的键补发 keyup
6. SendInput 发送 Ctrl↓ V↓ V↑ Ctrl↑（扫描码通过 MapVirtualKeyW 填充）
7. 更新条目 usedAt，将条目提到列表最前（置顶项留在置顶区）

### 修饰键释放的必要性

用 Ctrl+1 触发粘贴时，用户手指仍按着 Ctrl 和 1。直接发 Ctrl+V 会导致目标程序收到异常组合键（如 Ctrl+Alt+V）。因此发送前必须检测所有物理按下的修饰键并补发 keyup。释放后不再还原——粘贴完成时用户手指通常已抬起。

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
- 悬浮提示：`ClipWiz`（静态文本，可通过 `tray::SetTip` 更新）

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
- 尺寸：宽 520dip，高按 RowsVisible 配置计算
- 弹出位置：鼠标指针处 / 光标处 / 上次打开的位置（可拖动）
- 键盘操作：↑↓ 移动、Enter 粘贴、Esc 关闭、Alt+1~9 直接粘贴、Ctrl+D 删除、Ctrl+P 切换置顶
- 鼠标操作：单击选中、双击粘贴、右键上下文菜单
- Ctrl 悬停预览：长文本显示完整内容，图片显示放大预览
- 置顶项支持拖动调整顺序
- 失去焦点自动隐藏（WM_ACTIVATE / WA_INACTIVE）
- 双缓冲绘制（内存 DC + BitBlt），无闪烁
- 窗口只创建一次，后续 show/hide 复用

### 设置对话框

单窗口分组式布局（非 Property Sheet / 多 Tab）。运行时通过内存 DLGTEMPLATE + `DialogBoxIndirectParamW` 创建，所有控件在 `WM_INITDIALOG` 中动态生成。.rc 文件中不含任何对话框资源。

**[常规分组]**
- 开机自动启动（checkbox）
- 保存条数上限（数字框）
- 最多保存天数（数字框）
- 语言（下拉框：跟随系统 / English / 简体中文）
- 主题（跟随系统 / 浅色 / 深色）
- 窗口弹出位置（鼠标指针处 / 光标处 / 上次打开的位置）
- 显示字体（按钮显示字体名+字号，附"重置"按钮；调用 ChooseFontW）
- 数据库路径（只读路径框 + 浏览按钮；调用 SHBrowseForFolderW）

**[快捷键分组]**
- 唤出快速粘贴框（hotkey 控件 + "含Win" checkbox）
- 置顶项快捷键（10 个位置，2×5 网格，每个含 hotkey 控件 + "含Win" checkbox）

### 深色模式

读取注册表 `HKCU\...\Themes\Personalize\AppsUseLightTheme`，Theme=auto 时跟随系统设置。监听 `WM_SETTINGCHANGE` 实时切换，只更换绘制颜色常量。

### 高 DPI

manifest 声明 Per-Monitor V2。所有尺寸常量以 dip 存储，绘制时用 `GetDpiForWindow` 换算为物理像素。处理 `WM_DPICHANGED` 消息，跨屏拖动不模糊。

---

## 国际化（i18n）

- 英文内置于代码（`i18n.cpp` 中的 kDefaults 表），作为终极回退
- 简体中文在编译时以 RCDATA 资源打包进 exe（`lang/zh-CN.lng` → `IDR_LNG_ZHCN`）
- 语言文件格式：`lang/<locale>.lng`，UTF-8 编码，`key=value` 纯文本
- 加载优先级：磁盘文件 `lang/<locale>.lng` → 内置资源（仅 zh-CN）→ 英文
- 外部文件始终优先于内置资源，用户可自行覆盖翻译而无需重新编译
- 其他语言（日文、韩文等）仅从磁盘加载，由用户自行传播 .lng 文件
- 所有用户可见文字通过 `i18n::T(key)` 获取，代码中不存在硬编码界面文字
- 支持 `%s`、`%d`、`%u` 占位符，通过 `util::Format` 填充

---

## 日志

轻量文件日志（`log.h` / `log.cpp`）：

- 输出文件：`<数据目录>\clipwiz.log`，追加模式，每条日志立即刷新（`fflush`）
- 格式：`[YYYY-MM-DD HH:MM:SS.mmm] [LEVEL] message`
- 级别：DEBUG / INFO / WARN / ERROR，初始化时可设最低级别
- 线程安全：CRITICAL_SECTION 保护并发写入
- 无日志轮转和大小限制——正常运行下日志量很小
- 主要用途：启动诊断、热键注册失败、保存错误

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

user32、gdi32、msimg32、shell32、comctl32、advapi32、ole32、windowscodecs、comdlg32

均为 Windows SDK 自带，无第三方依赖。

### 预期产物指标

- exe 体积：200~350 KB
- 空闲常驻内存：4~8 MB
- 冷启动：< 100ms
- 弹出框显示：< 30ms

### 源文件布局

```
src/
  main.cpp           入口、单实例、消息循环
  app.h/.cpp         全局状态、消息分发
  store.h/.cpp       条目库、序列化、淘汰
  clipboard.h/.cpp   剪贴板监听与读写
  hotkey.h/.cpp      全局热键管理
  paste.h/.cpp       粘贴执行
  popup.h/.cpp       快速粘贴框
  settings.h/.cpp    配置与设置对话框
  imagecodec.h/.cpp  WIC 图片编解码
  tray.h/.cpp        托盘图标
  i18n.h/.cpp        国际化
  asyncwriter.h/.cpp 异步写盘
  log.h/.cpp         轻量文件日志
  raii.h             RAII 封装（GlobalLock、HANDLE、GDI 对象）
  util.h/.cpp        工具函数
  resource.h         控件 ID 与资源常量
  clipwiz.rc         图标、manifest、版本信息（无对话框模板）
lang/
  zh-CN.lng          简体中文语言包
```

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
| 图片条目占磁盘 | PNG 压缩 + 大数据保护提示 + 只淘汰未置顶条目 |
| 杀毒软件对剪贴板监听+模拟按键敏感 | 不联网、不注入、不装钩子 DLL，仅用官方 API |
| 输入法状态 | 只发 Ctrl+V 组合键，不逐字符输入，不受输入法影响 |

---

## v2 目标

| 项目 | 说明 |
| --- | --- |
| i18n 查找优化 | T() 英文回退目前线性扫描 kDefaults（~60 条），可改为 unordered_map 统一查找 |
| 托盘 tooltip 增强 | 显示条目数和置顶数，如 "ClipWiz — 42 条记录，3 个置顶"，无需打开窗口即可了解状态 |

---

## 版本历史

### v1.2.0

- **每行类型图标。** 每个条目前显示一个等宽的类型标识（纯文本 / RTF / HTML / 图片 / 文件列表），每种类型造型不同、颜色固定（深浅主题各一套），形状 + 颜色双保险，小尺寸下也能一眼区分。图钉 / 类型图标 / 序号三列等间距、全部左对齐。
- **右键"转为纯文本"。** 右键 RTF 或 HTML 条目可将其**原地**转为纯文本（同一条、位置不变，不是另存一份）。若转换后与已有纯文本条目重复，则合并：保留显示顺序更靠前的那条（因此跨组时优先保留置顶项）；两条都置顶时都不删除。合并后光标定位到保留的那一条。
- **规范化去重。** 去重改用"提取出的纯文本正文 + 类型前缀"计算哈希，而非剪贴板原始字节。修复了从 Word/浏览器反复复制同一段内容时堆积重复富文本条目的问题（它们每次的 RTF/HTML 字节都不同）。命中重复时保留最新内容，用户调整排版后再复制，粘贴到的是最新版本。
- **最新置顶排序修复。** 将"选择性前置"行为（置顶项不动；非置顶项提到非置顶组最上方）收敛为单一共用函数，新复制和新使用的条目都能稳定地落到所属组的最上方。
- **设置项悬浮提示。** 设置对话框里每个 Win 键图标按钮加了 hover 提示。

### v1.1.0

- 设置对话框细节与布局优化；按最后使用时间过期；多语言显示与稳定性修复。

### v1.0.0

- 首个版本：剪贴板历史（置顶/淘汰）、快速粘贴框、全局热键、图片/RTF/HTML/文件支持、设置对话框、国际化、深色模式、高 DPI。
