# ClipWiz Code Review

审查范围：全部源文件（src/*.cpp, src/*.h, CMakeLists.txt, clipwiz.rc, lang/zh-CN.lng）

严重程度分级：
- **P0** — 可能导致崩溃或数据丢失
- **P1** — 功能缺陷或逻辑错误
- **P2** — 稳定性隐患（极端条件下可能出问题）
- **P3** — 命名/风格不一致
- **P4** — 代码质量 / 完美主义
- **P5** — 建议性改进

---

## P0 — 崩溃 / 数据丢失风险

### 1. TogglePin 未检查 Find 返回值

**文件：** `src/app.cpp` L443

```cpp
void App::TogglePin(uint64_t id) {
    store_.SetPinned(id, !store_.Find(id)->pinned);  // Find 可能返回 nullptr
```

`store_.Find(id)` 在 id 不存在时返回 nullptr，直接解引用会崩溃。虽然正常流程中 id 来自列表，但边缘情况（如数据竞争、快速连续操作）可能触发。

**建议：** 加 null 检查，找不到则 return。

---

### 2. AsyncWriter::Stop() 不等线程退出即销毁同步对象

**文件：** `src/asyncwriter.cpp` L27-43

```cpp
void AsyncWriter::Stop() {
    ...
    SetEvent(event_);
    // No need to wait for thread; data already saved synchronously on exit
    CloseHandle(thread_);
    thread_ = nullptr;
    ...
    DeleteCriticalSection(&cs_);
}
```

SetEvent 后线程可能仍在 `Run()` 中执行 `EnterCriticalSection(&cs_)`。主线程紧接着 `DeleteCriticalSection`，若线程此时进入临界区 → 未定义行为（崩溃或静默损坏）。

注释说"数据已同步保存"，但问题不在数据，在于线程可能还在跑。

**建议：** `WaitForSingleObject(thread_, 5000)` 后再 CloseHandle / DeleteCriticalSection。5 秒超时足够任何磁盘写入完成。

---

## P1 — 功能缺陷

### 3. 托盘菜单硬编码快捷键提示

**文件：** `src/tray.cpp` L111

```cpp
showPopupText += L"\tCtrl+Alt+V";
```

用户可能在设置中把唤出快捷键改成别的组合，但托盘菜单永远显示 "Ctrl+Alt+V"。

**建议：** 从 config 读取 popupHotkey，动态格式化为字符串。

---

### 4. 设置对话框保存相对路径而非绝对路径

**文件：** `src/settings.cpp` L326

```cpp
std::wstring disp = util::MakeRelativePath(sel);
g_cfg->dataDir = disp;  // 存了相对路径
```

用户通过 SHBrowseForFolderW 选了绝对路径，但存入 config 的是相对路径。如果 exe 目录变了（用户移动了 exe），相对路径解析会指向错误位置。

**建议：** config 中存绝对路径，显示时再转相对。或者在 Load 时调用 MakeAbsolutePath 还原。

---

### 5. zh-CN.lng 中存在代码未使用的 key

**文件：** `lang/zh-CN.lng` L35-36, L48

```
settings.lang_en=English
settings.lang_zh=简体中文
settings.font_reset=重置
```

代码中语言下拉框直接硬编码 `L"English"` 和 `L"\x7B80\x4F53\x4E2D\x6587"`（settings.cpp L127-128），重置按钮用的是 `settings.font_default` 而非 `settings.font_reset`。这三个 key 是死数据。

**建议：** 要么代码改用 i18n::T() 读取这些 key，要么从 .lng 中删除。

---

## P2 — 稳定性隐患

### 6. log.cpp 使用 std::mutex，项目其余用 CRITICAL_SECTION

**文件：** `src/log.cpp` L17

项目整体风格是纯 Win32 API（CRITICAL_SECTION），asyncwriter.h 用的就是 CRITICAL_SECTION。log.cpp 引入了 `<mutex>` 和 `std::lock_guard`，混用两套同步原语。

功能上没问题，但如果将来需要统一为 Windows Slim Reader/Writer Lock 或做静态分析，混用增加复杂度。

---

### 7. log.cpp 无日志大小限制

**文件：** `src/log.cpp`

日志文件只追加不清理。如果程序长期运行（开机自启场景），且频繁触发 WARN/ERROR（如热键冲突反复注册失败），日志会无限增长。

**建议：** 加一个简单的大小检查（如 > 1MB 时截断或轮转），或者在 Init 时如果文件超过阈值就清空重写。

---

### 8. popup.cpp Edit 子类化未还原

**文件：** `src/popup.cpp` L907-908

```cpp
g.editOrig = reinterpret_cast<WNDPROC>(
    SetWindowLongPtrW(g.edit, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(EditProc)));
```

WM_DESTROY 中未调用 `SetWindowLongPtrW` 还原原始 WndProc。因为窗口销毁后进程也退出，实际无害，但不够干净。

---

### 9. WriteToFile 中 g_file 检查在锁外

**文件：** `src/log.cpp` L50-56

```cpp
void WriteToFile(const char* line) {
    if (!g_file) return;       // ← 锁外读
    std::lock_guard<std::mutex> lock(g_mutex);
    fprintf(g_file, "%s\n", line);  // ← 锁内用
```

如果 Shutdown() 在另一个线程把 g_file 置 nullptr，这里存在 TOCTOU 竞争。实际上 Shutdown 只在主线程退出时调用，而日志写入也在主线程，所以当前不会触发。但如果将来有后台线程写日志就会出问题。

**建议：** 把 null 检查移到锁内。

---

## P3 — 命名 / 风格不一致

### 10. log.cpp 局部变量用 snake_case，项目其余用 camelCase

**文件：** `src/log.cpp`

| log.cpp 中 | 项目其余风格 |
|---|---|
| `time_str` | `timeStr` |
| `line_buf` | `lineBuf` |
| `msg_buf` | `msgBuf` |
| `g_file` | `g_file`（全局变量 g_ 前缀一致，但后面跟的是 snake） |
| `g_mutex` | 项目其余全局用 `g_camelCase`（如 `g_hook`, `g_target`） |
| `g_minLevel` | 应为 `g_minLevel`（这个其实 OK） |
| `g_initialized` | 应为 `g_inited` 或 `g_ready`（项目无先例，但 initialized 太长） |

核心问题：`time_str`、`line_buf`、`msg_buf` 明显是 snake_case，而项目其余所有局部变量都是 camelCase（如 `pinSize`、`textRc`、`btnW`）。

---

### 11. log.cpp 的 SafeGetTime / SafeOpenFile 命名

项目其余函数命名是直接描述动作（`Utf8ToWide`、`LoadLngFile`、`MakeCtrl`），不加 "Safe" 前缀。`SafeOpenFile` 和 `SafeGetTime` 的 "Safe" 前缀暗示存在不安全的版本，但实际上没有。

---

### 12. log.h 的 `__func__` 传入但从不使用

**文件：** `src/log.h` L27-30, `src/log.cpp` L87

```cpp
#define LOG_DEBUG(fmt, ...) logger::Write(..., __func__, fmt, ...)
// log.cpp:
void Write(Level level, const char* file, int line, const char* func, const char* fmt, ...) {
    (void)func;  // Not using function name for now
```

参数传了又 `(void)` 掉。要么用起来（加到日志格式里），要么从签名和宏中去掉。

---

### 13. raii.h 注释风格

**文件：** `src/raii.h`

```cpp
// GlobalLock wrapper — fixes resource leak issues in clipboard.cpp
```

注释提到了具体修复历史（"fixes resource leak issues"），这属于 commit message 内容，不应出现在代码注释中。代码注释应描述"是什么"而非"修了什么 bug"。

---

## P4 — 代码质量 / 完美主义

### 14. popup.cpp 死代码：空 if 块

**文件：** `src/popup.cpp` L285-291

```cpp
// Positional hotkey hint (only first 10 pinned items)
if (row.pinned) {
    int pinnedIdx = index;
    if (pinnedIdx < 10) {
        // Get hotkey text from host (via config) — simplified: not drawn, shown in tray menu
    }
}
```

整个 if 块体为空，只有一行注释说"简化了不画"。这是典型的"占位代码忘了删"。

**建议：** 删掉整个块，或者真正实现快捷键提示绘制。

---

### 15. log.cpp Init() 中时间格式化重复

**文件：** `src/log.cpp` L73-79 vs L42-48

Init() 里手动写了一套 `time_t` → `localtime_s` → `strftime` 逻辑，而上面已经有 `SafeGetTime()` 函数做同样的事（只是格式不同）。可以统一为一个带格式参数的函数。

---

### 16. log.cpp 的 `#include <mutex>` 可替换为 Windows API

项目编译选项 `/GR-`（关闭 RTTI）且整体避免 C++ 标准库的线程设施。`std::mutex` 在 MSVC 下内部也是 CRITICAL_SECTION，但引入了 `<mutex>` 头文件的编译开销和风格不一致。

---

### 17. settings.cpp 语言下拉框硬编码字符串

**文件：** `src/settings.cpp` L127-128

```cpp
SendMessageW(cbLang, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"English"));
SendMessageW(cbLang, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"\x7B80\x4F53\x4E2D\x6587"));
```

"English" 和 "简体中文" 是语言自身的名字，不随界面语言变化，硬编码有合理性。但用 `\x7B80\x4F53\x4E2D\x6587` 转义序列而非直接写 `L"简体中文"` 降低了可读性。项目编译选项已有 `/utf-8`，源文件可以直接写中文。

---

### 18. i18n.cpp T() 英文回退用线性搜索

**文件：** `src/i18n.cpp` L252-257

```cpp
for (const Entry& e : kDefaults) {
    if (strcmp(e.key, key) == 0) {
        return e.en;
    }
}
```

kDefaults 有 ~60 条，每次 T() 调用在未命中 g_map 时都线性扫描。性能上无碍（调用频率低），但如果追求一致性，可以也放进 unordered_map。

---

## P5 — 建议性改进

### 19. 托盘 tooltip 可以显示条目数

当前 tooltip 只有 "ClipWiz"。Windows 托盘 tooltip 上限 128 字符，完全可以显示 "ClipWiz — 42 条记录，3 个置顶" 之类的信息，让用户不打开窗口就知道状态。

---

### 20. 日志格式缺少毫秒

当前格式 `[HH:MM:SS]` 精确到秒。调试快速连续事件（如连续复制触发的去重逻辑）时，秒级精度不够。建议改为 `[HH:MM:SS.mmm]`，用 `GetLocalTime()` 的 `wMilliseconds` 字段。

---

### 21. raii.h 的 HandleGuard 未处理 INVALID_HANDLE_VALUE 的创建

`CreateFileW` 失败返回 `INVALID_HANDLE_VALUE`（不是 nullptr）。当前构造函数 `explicit HandleGuard(HANDLE h)` 会接受它，析构时正确跳过（有 `!= INVALID_HANDLE_VALUE` 检查），但 `operator bool()` 也正确排除了。这点没问题，只是建议在注释中说明。

---

## 总结

| 级别 | 数量 | 关键项 |
|------|------|--------|
| P0 | 2 | TogglePin 空指针、AsyncWriter 竞态 |
| P1 | 3 | 托盘硬编码快捷键、dataDir 相对路径、死 i18n key |
| P2 | 4 | mutex 混用、日志无上限、子类化未还原、TOCTOU |
| P3 | 4 | snake_case 混入、Safe 前缀、func 参数、注释风格 |
| P4 | 5 | 死代码、重复逻辑、硬编码转义、线性搜索 |
| P5 | 3 | tooltip 增强、毫秒精度、INVALID_HANDLE_VALUE 注释 |

优先修复 P0 和 P1，P2 视情况处理，P3-P5 属于"有空就改"的范畴。
