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
    uint32_t     order;       // single global sort key: pinned [1,9999], unpinned [10001,...]
    uint64_t     createdAt;   // FILETIME
    uint64_t     usedAt;      // last paste timestamp
    std::vector<uint8_t> data; // unified binary content (text=UTF-16LE, image=PNG, etc.)
    uint32_t     imgW, imgH;  // pixel dimensions (kind==Image only)
    std::wstring sourceApp;   // lowercased process name the copy came from, PERSISTED
    std::wstring preview;     // one-line summary, recomputed at runtime, not persisted
    std::wstring searchText;  // lowercased full content the filter matches against,
                              // recomputed at runtime, not persisted, NOT truncated
    uint64_t     hash;        // FNV-1a hash for dedup, computed at runtime, not persisted
};
```

`preview`, `searchText` and `hash` are derived data: all three can be rebuilt from `data` alone, so none of them is written to disk. `preview` and `searchText` are recomputed together by one `FillDerived()` call, so a content change cannot leave one of them describing the old bytes; `hash` is assigned alongside them wherever content actually changes (it falls out of the canonicalisation that deduplication already has to do).

`sourceApp` is the one runtime field that **is** persisted, and the distinction is the whole rule: it cannot be rebuilt from `data`, because the process that made the copy is usually gone by the time the store is reloaded. Derived data is recomputed on load; facts are read back verbatim. Empty means the owner could not be resolved, which is recorded as unknown rather than guessed at — a wrong source name is worse than none, since the user would believe it.

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
    uint32   version    = 3
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
    uint32   order
    uint32   dataLen      byte count of unified content blob
    uint8    data[dataLen]
    uint32   appLen       byte count of sourceApp, UTF-16LE, no terminator
    uint8    sourceApp[appLen]
```

Binary format chosen over JSON/INI because clipboard text inevitably contains newlines, quotes, tabs, and emoji — text formats require escaping and introduce parsing ambiguity. Binary read/write by length fields is unambiguous and requires no parser library.

**One readable version, no migration.** `Load()` accepts `version == 3` and nothing else. A file written by an older build is handled exactly like a damaged one: renamed to `store.corrupt.<timestamp>.dat` byte-for-byte, never parsed, and the run starts empty.

This is a deliberate departure from the usual practice, and the reasoning is worth stating because it is the kind of decision that gets silently reversed later. ClipWiz has one user and no installed base, and what it stores is clipboard history — by nature transient, and already subject to automatic eviction and expiry. Supporting an old layout would mean a second parser, a second set of tests, and a permanent source of bugs, all to protect data that is worth nothing. So the compatibility budget is spent where it actually buys something: on never damaging the file being written *now* (atomic write, corrupt-file preservation). `tests/test_store.cpp`'s `OldVersionIsRejectedNotMigrated` pins the decision down — adding a migration path makes that test fail, which forces the choice to be made deliberately rather than by accretion.

A consequence worth knowing: bumping `kStoreVersion` discards the existing history. That is accepted.

### config.ini

Flat key=value format, no sections. UTF-8 encoded.

```ini
MaxHistory=50            ; unpinned item limit, range 5–2000
ExpiryDays=5             ; expiry in days, 0 = never
PasteDelayMs=60          ; delay after focus restore before sending the paste chord
PasteKey=0               ; 0=Ctrl+V / 1=Shift+Insert
RowsVisible=10           ; visible rows in popup
PopupPosition=0          ; 0=mouse / 1=caret / 2=last position
HoverPreview=1           ; 1=preview a row after 300ms hover / 0=only on Ctrl+hover
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
7. **Corruption protection:** if store.dat fails validation at startup — bad magic, truncated, an item count over the cap, an unknown item kind, a lying length field, or a version this build does not write — it is renamed to `store.corrupt.<timestamp>.dat` (never overwritten or deleted). The program starts with an empty database and notifies the user.

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
  ├─ OpenClipboard: up to 4 attempts 20ms apart; give up if all fail
  │  (Office and some installer UIs still hold it right after a copy)
  ├─ Source app: GetClipboardOwner() → GetForegroundWindow() → process name
  ├─ Blocklist: source app matches a rule → discard (see Application Blocklist below)
  ├─ Exclusion markers → discard (see Exclusion Markers below)
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

### Exclusion Markers

Some applications ask not to be recorded. Windows has no single convention for how they ask, and telling the conventions apart is the whole job — reading an opt-out-by-value marker as a presence marker would exclude every application that bothered to declare itself recordable.

| Format | Rule |
|---|---|
| `Clipboard Viewer Ignore` | present at all → ignore |
| `ExcludeClipboardContentFromMonitorProcessing` | present at all → ignore |
| `CanIncludeInClipboardHistory` | **only a FALSE value** → ignore |

The three are [documented by Microsoft](https://learn.microsoft.com/en-us/windows/win32/dataxchg/clipboard-formats#cloud-clipboard-and-clipboard-history-formats) and used by Windows Credential Manager and most third-party password managers.

`CanIncludeInClipboardHistory` is an opt-out *by value*: applications place it on the clipboard with `1` to say "recording me is fine" and `0` to ask out. Presence alone therefore means nothing.

**Deliberately not honoured: `CanUploadToCloudClipboard`.** It is registered alongside the others and reads like a privacy marker, but the claim it makes is "do not upload this to a cloud clipboard". ClipWiz never uploads anywhere — a single offline executable is the core design constraint — so the restriction does not apply, and honouring it would discard ordinary local copies on account of a service this program does not use. zsclip reaches the same conclusion and asserts it in a test.

**Value parsing** (`privacy::PayloadMeansFalse`): the value is documented as a DWORD but nothing on the clipboard carries a type, and in practice it arrives as a DWORD, a WORD, ASCII text, or UTF-16LE text. Width alone cannot tell those apart — `"no"` and a WORD are both two bytes — so a payload made only of printable ASCII and NULs is read as text (`0` / `false` / `no`, case-insensitive) and anything else as a 2- or 4-byte integer. An empty or unrecognised payload is **not** false: the rule is "must positively read a false value to exclude", and guessing from bytes we cannot interpret would silently throw away ordinary copies.

**Two race guards:**

- Markers are checked **before** any content is read, so an excluded copy costs one clipboard round trip rather than a full RTF/HTML extraction that gets thrown away afterwards.
- `GetClipboardSequenceNumber()` is captured right after opening the clipboard and re-checked once the payloads are in hand. Holding the clipboard open does not stop a thread that already had it from replacing the content, so without the re-check a marker read from the new content could end up judging a payload taken from the old one. On mismatch the capture is dropped, which costs nothing: the replacement fires its own `WM_CLIPBOARDUPDATE` and gets captured on its own terms.

Implementation lives in `src/privacy.h` (marker table, one entry per marker with its rule adjacent) and `src/privacy.cpp` (value parsing plus the two named predicates `PresenceIsExclusion` / `FalseValueIsExclusion`). Neither opens the clipboard, so `tests/test_privacy.cpp` covers every rule headless.

### Source Application Tracking

Every captured item records which process made the copy, so an entry can be recognised by where it came from and not only by what it says.

**Resolution.** `GetClipboardOwner()` first — the owner is the window that put the data there, which is what "copied from" means — falling back to `GetForegroundWindow()` for the cases where ownership was claimed with a NULL window. The window becomes a process id via `GetWindowThreadProcessId`, and the process id becomes a name via `QueryFullProcessImageNameW`.

`QueryFullProcessImageNameW` rather than the more commonly used `GetModuleFileNameExW`: it lives in kernel32 and needs only `PROCESS_QUERY_LIMITED_INFORMATION`, so it reaches elevated and protected-process-light targets that `GetModuleFileNameExW` fails on, and it costs no new import library.

Resolution happens while the clipboard is still open. `GetClipboardOwner()` only describes the content about to be read, and the owner window can be destroyed at any moment after it is released.

**UWP.** A Store app's visible window belongs to `ApplicationFrameHost.exe`, which hosts the real app's window as a child. Reporting the frame would label every UWP copy with the same useless name, so when the resolved name is the frame, child windows are enumerated for one owned by a different process. If there is none, the frame is the answer.

**Storage.** Lowercased, path stripped: `C:\Windows\System32\notepad.exe` is stored as `notepad.exe`. Process names are case-insensitive on Windows, so one canonical spelling loses nothing, and it lets the `app:` filter run a plain substring search with no per-keystroke case folding. An unresolvable owner is stored as empty — see Item Structure above for why that is never guessed at.

Because the field is persisted, a re-copy of the same content refreshes it: on a deduplication hit the stored bytes are replaced with the newest copy, and the origin follows them. An entry reading `notepad.exe` should be holding what notepad last put there.

**Display.** The popup draws the name at the tail of each row in the small dim font, right-aligned in a slot of its own; the preview rectangle is narrowed to make room, so long content truncates against the label instead of running underneath it. The `.exe` suffix is dropped for display, and the slot is capped at half the row width — when a name does not fit, the content wins and the label is dropped.

### Application Blocklist

Exclusion markers cover apps that *ask* not to be recorded. The blocklist covers the ones that never ask — a password manager that copies secrets with no marker on the clipboard, a terminal scrolling past credentials, anything the user simply does not want kept. It is a user-maintained list of rules, matched against the same source app resolved for tracking above.

**Two tiers,** because "do not record this app" and "do not let clipwiz act inside this app" are different asks:

| Tier | Rule | Effect |
|---|---|---|
| No capture | `keepass.exe` | Clipboard changes from this app are not recorded. Hotkeys still work. |
| Full disable | `!keepass.exe` | Not recorded, **and** clipwiz hotkeys do nothing while this app is in the foreground. |

The leading `!` is the only difference between them. Full disable is for apps where even popping the history list is unwanted — while presenting, while screen-sharing, in a kiosk.

**Matching.** One rule per line. A rule with no `*` is a plain substring test; a rule containing `*` is an anchored glob, where `*` matches any run of characters. Each rule is tried against three fields — process name (`keepass.exe`), full process path (`c:\program files\keepass\keepass.exe`), and window title (`keepass - mydb.kdbx`) — and hits if it matches any one of them. Lines beginning with `#` are comments; blank lines are ignored.

All comparison is lowercased, and the *caller* lowercases the three fields before matching, so the matcher never folds case itself — the same contract `filter.cpp` uses. An empty field never matches, not even `*`: an unresolved window title must not become a wildcard hit against every rule.

**Where each gate sits.** On the capture side the blocklist is checked *before* any content is read — right after the source app is resolved, ahead of the exclusion markers — so a blocked copy costs one window lookup and one clipboard round trip, never a full RTF/HTML/image extraction thrown away afterwards. On the paste side, `App::OnHotkey` resolves the foreground window and returns at once if it classifies as full disable; that check short-circuits on an empty rule set, so a user with no blocklist pays no window query on the hotkey path.

**Classification** returns the strongest tier that matched — full disable beats no-capture beats none — and stops early once full disable is reached, there being no tier above it. Implementation lives in `src/blocklist.h` and `src/blocklist.cpp`: pure logic over plain strings, no Win32 and no clipboard access, so `tests/test_blocklist.cpp` covers parsing, matching, and classification headless.

### Preview Desensitization

The blocklist stops a secret from being *stored*. Desensitization is the other half: for content already in history — a phone number pasted into a form, an ID card, a password copied from a note — it stops the secret from being *readable on screen*. It masks sensitive spans wherever clipwiz renders text back to the user: the list-row preview, the search field, and the hover preview.

**What is never touched: `item.data`.** Masking applies only to the derived `preview` and `searchText` and to the hover view. The stored bytes are the full original, so what actually gets pasted is always complete — this hides a secret from a glance over the shoulder, it does not remove it from the entry.

**Five hand-written scanners,** each independently toggleable:

| Scanner | Detects | Default |
|---|---|---|
| Email | `local@domain.tld`, TLD must be two or more letters | off |
| Phone | Chinese mainland mobile: `1[3-9]` then 9 digits, tolerating `+86` and separators | on |
| ID card | 18-digit resident ID: region, birth date (leap-aware) and the GB 11643 MOD 11-2 checksum all verified | on |
| API key | known prefixes (`sk-`/`pk-`/`ghp_`/`AKIA`/`AIza`/…) + separator + 20 or more `[\w-]` | off |
| Password | whole text is 8–64 chars, no whitespace, and has upper + lower + digit + symbol | on |

A detected span keeps its first and last few characters and replaces the middle with `****`, so the entry stays recognisable without being readable. Email keeps the `@domain` and masks only the local part.

**Two guard rails,** both carried over from TieZ: text longer than 5000 characters is returned unchanged (a huge blob is not worth scanning on a per-keystroke recompute), and text starting with `data:` is returned unchanged (a data URI is machine-generated content, not a secret a user typed).

**Deliberately no `std::regex`.** These patterns are simple enough to scan by hand, a hand scanner is faster on every keystroke-driven recompute, and it costs none of `std::regex`'s code size — the same reasoning that keeps the whole tool a single small exe.

**Case and the two derived fields.** `preview` keeps its original case, so every scanner fires on it including the password heuristic (which needs an uppercase letter). `searchText` is lowercased before it is stored, so only the case-insensitive structural scanners — email, phone, ID, API key — match there, and the password heuristic harmlessly skips it. That split is acceptable: a password is still hidden in the list row and the hover view, `searchText` is never shown on screen, and `data` is never masked. Mask-then-lowercase would be the only correct order if both lived in one string, but they do not — `MakeItemSearchText` lowercases internally, so `FillDerived` masks its already-lowercased result.

**Where it is applied.** `store.cpp`'s `FillDerived` masks both derived fields as it computes them, so every path that builds an item — `Add`, `Load`, `ConvertToPlainText`, `RefreshPreviews` — is covered by one call. The hover preview masks the GDI text path directly. RTF is the exception: its hover view streams the raw RTF into a RichEdit control, which cannot be masked in place, so when masking is active *and* would change that item's text, the rich render is skipped and the hover falls back to drawing the masked plain text — that one preview loses its formatting, the right trade for not leaking the secret. With masking switched off, `AnyEnabled` short-circuits and both the rich path and the recompute cost exactly what they did before this existed.

**Deduplication is unaffected.** The canonical hash is computed over `item.data` (the raw bytes), never over the masked preview, so two entries differing only in a masked span still dedup correctly and a masked entry still matches its own re-copy.

Implementation lives in `src/mask.h` and `src/mask.cpp`: pure logic over plain strings, no Win32, no clipboard, no config access, so `tests/test_mask.cpp` covers every scanner, both guard rails, and the span-merge order headless.

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
6. SendInput: the configured paste chord (Ctrl+V by default, Shift+Insert optional) as mod↓ key↓ key↑ mod↑, all four events in one call, injected **by scan code**
7. Update item usedAt; move item to front of list (pinned items stay in pinned section)

### Why Scan Codes

The chord is injected with `wVk = 0` and `KEYEVENTF_SCANCODE`, not as a virtual-key event. An event carrying only `wVk` has no hardware scan code, and a surprising number of receivers discard those: RDP and other remote stacks, virtual machines, raw-input readers, and edit controls that use the scan code to tell left from right modifiers. With `wVk` zeroed, the event is indistinguishable from a real keypress at every layer above the driver.

Two details that matter:

- `MapVirtualKeyW(vk, MAPVK_VK_TO_VSC)` returns the scan code but hides whether the key is extended, and `VK_INSERT` **is**: injecting its `0x52` without `KEYEVENTF_EXTENDEDKEY` arrives as a different key entirely. `MAPVK_VK_TO_VSC_EX` reports the `E0` prefix in bits `0xE000`, so extended-ness is derived rather than hardcoded per key.
- If a keyboard layout cannot map one of the two keys, the code falls back to the virtual-key form. That form is what remote desktops tend to drop, but it still works locally, and a paste that reaches most applications beats one that silently reaches none.

`ReleaseHeldModifiers()` deliberately stays virtual-key based. Its job is to lift a modifier the user is *physically* holding, and only the VK identifies which one; a scan code cannot tell left from right without the extended flag, and getting that wrong would leave a modifier stuck down.

### Configurable Paste Chord

`PasteKey` in config.ini selects between `CtrlV` (0, default) and `ShiftInsert` (1). Ctrl+V is the near-universal binding; Shift+Insert is the older one and remains the right answer for applications that bind Ctrl+V to something else, plus terminal-style hosts where Ctrl+V is a literal control character. The setting is on the Shortcuts page; its two entries are key names rather than prose, so they are not translated.

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

### Validation on Save

Clicking OK reads every shortcut into locals and validates them *before* anything reaches the live config (`Config& cfg` aliases the global, so a rejected key must never be written to it). The first problem raises a modal message box and keeps the dialog open with nothing committed — the user has to pick a different combination, not acknowledge and proceed. Three classes are rejected:

- **No real modifier** — a bare key, or Shift alone (which would swallow ordinary uppercase typing), is refused; Ctrl / Alt / Win is required.
- **System-common combinations** — Ctrl+number, Ctrl+C/V/X/Z/A/S and Alt+Tab collide with shortcuts every browser, editor and the OS itself rely on, so they are refused with the specific reason spelled out.
- **Duplicate combinations** — reusing the popup combination for the queue or a pinned slot is refused with a popup-specific message; any other duplicate pair (the queue against a pinned slot, or two pinned slots sharing one combination) is refused as ambiguous, since a single press could then mean two things.

An unbound slot (no key set) is fine and skipped. This gate is separate from the conflict handling above: validation rejects a combination clipwiz will not even attempt to register, whereas `RegisterHotKey` can still fail at runtime for a key another program already holds. The check reuses `hotkey::IsUsable` (the modifier rule) and `hotkey::LooksRisky` (the system-common rule), which the settings dialog now drives on OK.

---

## UI Implementation

All UI uses native Win32 controls + GDI custom drawing. No UI framework. Font is the system UI font from `SystemParametersInfo(SPI_GETNONCLIENTMETRICS)`.

### Tray Icon

- Left click: open quick-paste popup
- Right click: context menu (Quick Paste / Settings / Auto-start / Clear History / Backup to File / Restore from File / About / Exit)
- Tooltip: `ClipWiz` (static; updated via `tray::SetTip` if needed)

### Backup and Restore (.clpw)

Two tray-menu commands move the whole history through a single file: **Backup to File** writes it out, **Restore from File** merges one back in. The extension is `.clpw`, but there is no separate backup format — a `.clpw` file *is* a `store.dat`, the exact v3 bytes `Store::Serialize()` produces, so export needs no encoder of its own.

**Export** suggests `clipwiz-backup-<timestamp>.clpw`, takes a path through `GetSaveFileNameW`, and writes `Serialize()` with `util::WriteFileAtomic` — the same atomic tmp-then-rename the background saver uses, so a half-written backup can never masquerade as a good one. It runs synchronously on the UI thread: this is a one-shot modal action the user is waiting on, whereas AsyncWriter exists to keep the *frequent* automatic `store.dat` saves off the UI thread, not to serialize arbitrary export targets.

**Import** reads the chosen file whole (`util::ReadWholeFile`) and hands the bytes to `Store::ImportMerge`. That method parses through `ParseBuffer` — the one true reader for the `Serialize()` layout, now shared by `Load()` and `ImportMerge()` so "what counts as a valid backup" is defined exactly once — into a throwaway list that never touches live state, then routes every entry through `Add()`. Going through `Add()` is the whole design: an import gets content dedup, pinned-section preservation and capacity eviction for free, exactly as a fresh copy would.

Three decisions shape the failure and merge behaviour:

- **One version, no migration.** `ParseBuffer` accepts only the v3 layout this build writes; anything else — bad magic, a truncated tail, an older version — is rejected and `ImportMerge` returns `-1`. This mirrors the store's single-version stance on load.
- **A rejected import changes nothing, and touches no file.** On `-1` the live store is left byte-for-byte as it was. Unlike a corrupt `store.dat` at startup, the offending file is *not* renamed aside: it is a file the user pointed us at, not our own database, so quietly moving it would be presumptuous. The caller simply reports "not a readable backup".
- **Imported entries land unpinned.** They arrive as new items with fresh ids and timestamps, carrying their recorded `sourceApp`. This is a *content merge, not a state restore* — pinning is a decision about the current library, and letting a backup file silently fill the never-evicted pinned section could wedge the store at its cap. "Preserves pinned" therefore means an import never disturbs the pinned items already present, not that imported items keep a pinned flag.

On success the popup refreshes, the merged store is flushed with `SaveNow()` (async, like Clear History), and an info box reports the merged count. `tests/test_store.cpp` covers the merge headlessly in eight cases — full round-trip through `Serialize()`→`ImportMerge`, unpinned arrival, dedup against existing content, existing pinned left intact, corrupt and truncated input rejected with the store unchanged, wrong-version rejection, an empty backup as a no-op, and the capacity cap enforced. `ParseBuffer`/`ImportMerge` are `Store` members rather than a standalone pure module — they read and write `items_` — so they are exercised through the store rather than isolated like `merge` or `transform`.

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
- Keyboard: ↑↓ navigate, Enter paste, Esc close, Alt+1–9 direct paste, Ctrl+D delete, Ctrl+P toggle pin, Ctrl+↑/↓ reorder the pinned item under the cursor
- Multi-select: Shift+↑/↓ extend the selection, Ctrl+click toggle a row, Shift+click select a range, Ctrl+A select all; selected rows draw with the theme's selection fill. The selection is a sorted, de-duplicated set of item ids (not row indices) so it survives a filter rebuild; it is empty in the common single-cursor case, so the "type → Enter → paste" main path never touches it and runs exactly as before. Shift (not Ctrl) drives keyboard extension because Ctrl+↑/↓ is already taken by pinned-item reordering.
- Mouse: click select, double-click paste, right-click context menu
- Hover preview: full text for long items, enlarged image for image items (see Search and Filtering below)
- Pinned items support drag-to-reorder
- Auto-hides on focus loss (WM_ACTIVATE / WA_INACTIVE)
- Double-buffered drawing (memory DC + BitBlt), flicker-free
- Window created once, shown/hidden thereafter

### Merge Selected Items

Multi-selection in the popup exists mainly to feed one action: merging several entries into a new one. Right-click any selected row and **Merge selected** combines them; the item is greyed out unless at least two mergeable rows are in the selection.

Rules:

- **Text kinds** (Text / HTML / RTF) merge freely with one another. Each is first flattened to plain text by `CanonicalBody`, so an HTML row and an RTF row combine into one plain-**Text** entry — formatting is dropped deliberately, there being no sane way to splice two rich documents.
- **File lists** (FileDrop) merge with file lists only. clipwiz stores a drop as "one path per line", so merging is line concatenation: each body's trailing newline is trimmed, the lists join on a single `\n`, and the result is re-terminated so it parses exactly like a freshly captured drop. The output stays **FileDrop**.
- **Images are refused** — merging pictures would mean canvas compositing, out of scope for a manager that stays light.
- **Mixing a file list with text is refused** — paths and prose combine into nothing worth pasting.

The separator between merged text bodies is configurable (Settings → General → *Merge separator*): blank line (default), newline, space, or a custom single-line string. It governs text merges only; file lists always join on a newline because their format is fixed.

The result is a **brand-new entry** via `Store::Add()` — sources are never edited in place — so it enjoys canonical deduplication for free: a result already in history surfaces that entry instead of storing a duplicate. Implementation lives in `src/merge.h` and `src/merge.cpp`: pure logic (items in, merged kind+data out), no window, no clipboard, no store mutation, so `tests/test_merge.cpp` covers every rule headless. This is a deliberately flatter shape than Ditto's `IClipAggregator` (a per-format polymorphic accumulator): Ditto needs that because it rebuilds binary `DROPFILES` blobs, whereas every merge here is string concatenation, so two functions hide the canonical extraction and type dispatch just as well without the machinery.

### Text Transforms

Right-click any text row and two submenus offer the same fixed set of thirteen transforms: **Copy transformed** stores the result as a new entry, **Paste transformed** writes it straight to the clipboard and pastes it without touching history. A separate **Paste as plain text** is the paste-time sibling of *Convert to plain text* — it flattens an HTML or RTF row to plain text on the way out instead of editing the stored entry.

The set borrows the *shape* of Ditto's `CSpecialPasteOptions` — a fixed list of named operations, not a rule engine — a deliberate fit with the "no regex, stay light" boundary: picking a transform is an enum value, never a pattern to compile.

| Transform | Effect |
|---|---|
| Trim whitespace | strip leading/trailing whitespace |
| Remove line breaks | delete every CR/LF, gluing the lines together |
| One line between paragraphs | normalize paragraph breaks to exactly one newline |
| Blank line between paragraphs | normalize paragraph breaks to one blank line |
| UPPERCASE / lowercase | every character up / down |
| Capitalize Each Word | first letter of each word up, the rest down |
| Sentence case | first letter of each sentence up, the rest down |
| camelCase | `some text here` → `someTextHere` |
| Invert case | swap the case of every letter |
| ASCII only | drop every code point above U+007F |
| Slugify | lower-case URL slug: runs of non `[a-z0-9]` collapse into one separator |
| Append date/time | tack the current local timestamp on the end |

Transforms apply to **Text / HTML / RTF** only; both submenus grey out for images and file lists, whose payloads (raw PNG bytes, one path per line) hold no prose to case-fold or slugify.

The separator Slugify joins on is configurable in config.ini (`SlugSep`, default `-`); an empty value falls back to `-` so words never silently glue together. It stays out of the Settings dialog on purpose — Slugify is one of thirteen transforms and a rarely-touched knob, so it does not earn a UI field.

Implementation lives in `src/transform.h` and `src/transform.cpp`: `Apply(kind, text, options)` is a pure `std::wstring → std::wstring` function with no window, no clipboard, no config and no clock. The caller turns an Item into text first (`textconv::CanonicalBody`), so the unit never sees `store.h`. *Append date/time* keeps `Apply` pure by taking the timestamp as an injected string — production passes the formatted local time, tests pass a fixed literal — rather than reading the clock inside, and `tests/test_transform.cpp` exercises every rule headless. Ditto's Typoglycemia, pasteAsImage, PosixifyPaths and GUID generation are deliberately not borrowed (none are clipboard-management essentials), and its slugify in particular drags in `<regex>` plus a 200-entry accent-folding table; clipwiz's stays ASCII-simple — anything not `[a-z0-9]` collapses into the separator, no transliteration, matching ASCII-only's "drop, don't transliterate" philosophy.

### Sequential Paste Queue

Multi-selection feeds a second action besides merging: **Add to paste queue** lines the selected entries up to be pasted one at a time. Each press of the queue hotkey (default **Ctrl+Alt+J**, configurable in Settings → Shortcuts beside the popup hotkey) pastes the front item and drops it from the queue, so keeping a rhythm of presses walks straight down the list — the classic "copy five things, then paste them in order into five places" flow.

The queue is `App::queue_`, a plain `std::vector<uint64_t>` of item ids. It is **runtime-only working state, never persisted**: it is not data but what you are doing right now, so a restart clears it and the store format is left untouched. Adding a selection appends its ids in order, skipping any already queued so re-adding never pastes one item twice.

Popping reuses `PasteItem` — the same path an ordinary paste takes — so the front item is written to the clipboard under the self-write marker (it is not re-captured into history), promoted by `Touch`, and the reorder is scheduled to disk. Ids whose item has since been deleted are skipped rather than swallowing a keypress, and an **empty queue is a silent no-op**: the hotkey never raises an error box.

Queued rows wear a small accent-coloured pill at the right edge showing their 1-based position. Only queued rows reserve that strip, so the common empty-queue case keeps full text width; on pinned rows the badge sits just left of the reorder icons, and the row tail reads `[text][source app][badge][sort icons]` with no overlap.

The shape follows TieZ's `paste_queue.rs` (GPL, rewritten here): a FIFO of ids where one press pops and pastes the front. It is deliberately leaner — TieZ also carries last-paste fingerprints and a delete-after-paste step, neither of which fits clipwiz's model, so only the id list is kept. There is no pure module and no test file for it: the queue is three trivial operations (dedup-append, position-lookup, pop-front) over a vector that lives in `App`, with no algorithm to exercise headless — unlike merge or transform, whose branching earns its tests.

### Save as File / Show in Explorer

Right-click a row and one more slot writes it out to a real file. The label adapts to the row: **Save as File** for anything with content to write, **Show in Explorer** for a file list — because a captured drop is a list of paths to files that already exist on disk, so there is nothing to save and the useful action is to reveal them.

- **Text / HTML / RTF → `.txt`.** The extracted plain text (`Store::TextOf`, the same canonical extraction merge and transform rely on) is written as UTF-8 through `util::Narrow`. HTML and RTF lose their formatting here by design: save-as is for getting the *words* into a file, and keeping every original byte is exactly what the whole-history `.clpw` backup is for.
- **Image → `.png`.** Image payloads are stored as PNG on disk already (see Image Handling), so the stored bytes go out verbatim — no re-encode, no WIC round trip, no quality loss.
- **File list → Explorer.** `SHOpenFolderAndSelectItems` opens the first path's parent folder with that file selected. A drop can span several folders and Explorer selects within one, so revealing the first path is the honest, predictable choice rather than a partial multi-folder selection.

Both save paths run through `GetSaveFileNameW` and `util::WriteFileAtomic` — the same atomic write the backup export uses — and the default name is `clipwiz-<timestamp>.txt` / `.png`. The name is timestamp-based, not content-derived, on purpose: deriving it from the text would mean sanitising invalid characters, dodging reserved DOS names (`CON`, `PRN`) and handling empty content, all to produce a filename the user is about to edit in the dialog anyway.

The action is synchronous and modal, and the popup hides first (the shape `PasteTransformed` uses), because it is a one-shot thing the user is waiting on — not the frequent automatic `store.dat` saves AsyncWriter exists to keep off the UI thread.

Like the paste queue, there is no pure module and no test file: this is a common dialog, a shell call and a file write, with no algorithm to exercise headless. The only parsing — splitting the FileDrop payload's UTF-16LE "one path per line" — is trivial and shares its format with the file-list handling `merge.cpp` already covers.

### Search and Filtering

ClipWiz has no tags, no groups and no folders — that boundary is deliberate — so the filter box is the only way to organise entries, which makes it the most load-bearing UI logic in the program.

**What is searched.** Each item carries a runtime-only `searchText` field: the full extracted content, lowercased, never persisted (it is derived data, and the invariant is that derived data can always be rebuilt from `data`).

- Text / FileDrop: the whole payload. For a file list that means **every** path, not just the first file name the preview shows.
- Html / Rtf: their extracted plain text, capped at 4096 wchars so that one pathological multi-megabyte RTF cannot turn a single `Add()` or `Load()` into a full-document parse.
- Image: its preview string — the localised `[Image W×H]` is the only text an image has.

Matching used to run against `preview`, a 160-character one-line summary, so anything past the 160th character was unfindable and searching for the second file in a selection never worked.

`searchText` is stored **already lowercased**, and the query is lowercased once per keystroke in `Parse()`. The per-item cost is then a plain substring search with no allocation and no case folding, which is cheaper than the old code that copied and lowercased `preview` for every item on every keystroke. With the 9999-item cap, a full scan per keystroke stays well inside budget.

**Multi-keyword AND.** The filter is split on whitespace and *every* word must appear somewhere in the content. Order does not matter, so `report q3 draft` finds an entry containing all three in any arrangement.

**Tokens.** A word is treated as a query token only when it sits at the **start** of the filter, its prefix is one we know, and something follows the colon:

| Token | Effect |
|---|---|
| `kind:text` | only plain-text entries |
| `kind:rich` | only Html or Rtf entries |
| `kind:image` | only images |
| `kind:file` | only file lists |
| `app:<name>` | substring match on the source process name |

Everything after the leading tokens is a keyword, so `kind:image screenshot` means "images whose content mentions screenshot".

The leniency is the point. This is a search field first and a query language second: swallowing a colon that appears later in the query would silently return zero results for perfectly ordinary searches such as `https://example.com` or `注意:kind`. An unrecognised `kind:` value (`kind:pdf`) is likewise left alone and searched for as text. `app:` matches the lowercased source process name recorded at capture time (see Source Application Tracking); an entry whose owner could not be resolved has no name, so an `app:` query does not match it — an honest empty result rather than a false positive.

Parsing and matching live in `src/filter.h` / `src/filter.cpp`: raw text in, a verdict out, no window and no store access, which is why `tests/test_filter.cpp` can cover it headless.

**Hover preview.** Resting the mouse on a row for 300ms shows its preview; holding Ctrl shows it immediately. The delay is what keeps sweeping down the list from flashing a window per row, and the timer is re-armed only when the hovered *row changes* — re-arming on every `WM_MOUSEMOVE` would mean the preview never appears while the hand is moving at all, which is most of the time. `HoverPreview=0` in config.ini restricts preview to Ctrl+hover, for people who find the window appearing uninvited; Ctrl keeps working either way.

### Settings Dialog

Single centered window with grouped sections (no Property Sheet / multi-tab). Built at runtime via in-memory DLGTEMPLATE + `DialogBoxIndirectParamW`; all controls created dynamically in `WM_INITDIALOG`. No dialog resources in .rc file.

**[General section]**
- Launch at startup (checkbox)
- Max history items (numeric input)
- Item expiry in days (numeric input)
- Language (dropdown: Follow system / English / 简体中文)
- Theme (follow system / light / dark)
- Popup position (mouse pointer / text caret / last position) + "Hover to preview" checkbox on the same row
- Display font (button showing font name + size, with "Reset" button; uses ChooseFontW)
- Data directory (read-only path + browse button; uses SHBrowseForFolderW)

**[Shortcuts section]**
- Popup hotkey (hotkey control + "Win" checkbox)
- Paste keystroke (dropdown: Ctrl+V / Shift+Insert)
- Pinned position hotkeys (10 positions, 2×5 grid, each with hotkey control + "Win" checkbox)

The dialog is laid out by advancing a `y` cursor and its height is taken from that cursor at the end, so adding a row cannot clip the controls below it.

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
  app.h/.cpp         Global state, message dispatch, backup / save-as actions
  store.h/.cpp       Item database, serialization, eviction, import-merge
  textconv.h/.cpp    HTML/RTF → plain text; canonical form used for deduplication
  filter.h/.cpp      Popup filter parsing and matching
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
  tray.h/.cpp        Tray icon
  i18n.h/.cpp        Internationalization
  asyncwriter.h/.cpp Async disk writer
  log.h/.cpp         Lightweight file logging
  raii.h             RAII wrappers (GlobalLock, HANDLE, GDI objects, clipboard open)
  util.h/.cpp        Utility functions
  resource.h         Control IDs and resource constants
  clipwiz.rc         Icon, manifest, version info (no dialog templates)
lang/
  zh-CN.lng          Simplified Chinese language pack
tests/
  testfw.h           Check macros and the pass/fail tally — no third-party framework
  test_main.cpp      Entry; redirects the data directory to a scratch dir first
  test_textconv.cpp  Covers src/textconv.cpp
  test_filter.cpp    Covers src/filter.cpp
  test_privacy.cpp   Covers src/privacy.cpp
  test_blocklist.cpp Covers src/blocklist.cpp
  test_mask.cpp      Covers src/mask.cpp
  test_merge.cpp     Covers src/merge.cpp
  test_transform.cpp Covers src/transform.cpp
  test_store.cpp     Covers src/store.cpp
```

Seven units are pure: `textconv`, `filter`, `privacy`, `blocklist`, `mask`, `merge` and `transform`. Each takes ordinary data in and gives ordinary data out, touching no window, no clipboard and no configuration — the first three were pulled out of larger files, the other four written pure from the start. That is the whole test for whether a piece of ClipWiz belongs in its own unit, and it is what lets `clipwiz_tests` link them without dragging in popup, settings, app, tray or hotkey.

Test file names mirror the unit under test (`tests/test_filter.cpp` exercises `src/filter.cpp` and nothing else) — the C++ equivalent of the "tests live next to the pure logic" layout used by zsclip and QuickClipboard, same discoverability, no inline-test syntax needed. `clipwiz_tests` is a separate CMake target built with the identical `/W4 /WX` and static-CRT settings, marked `EXCLUDE_FROM_ALL` so a normal build stays fast; `build-tool.bat test` builds and runs it.

The store tests restate the binary format constants themselves rather than including them from `store.cpp`. If the tests read the format from the code, changing the code would silently change the expectation too, and the guardrail would be gone exactly when it was needed.

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
| Input method state | Only sends the paste chord, never character-by-character input |

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
    std::wstring searchText;  // 过滤框比对的小写化全文，运行时计算，
                              // 不持久化，**不截断**
    uint64_t     hash;        // FNV-1a 去重哈希，运行时计算，不持久化
};
```

`preview`、`searchText` 和 `hash` 都是派生数据：三者都能单从 `data` 重建，所以都不写盘。`preview` 和 `searchText` 由同一个 `FillDerived()` 一同重算，所以内容变更不可能让其中一个还在描述旧字节；`hash` 在内容真正发生变化的地方与它们相邻赋值（它是去重本来就要做的 canonical 化顺手掉出来的）。

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
PasteDelayMs=60          ; 焦点切回后到发粘贴组合键的等待
PasteKey=0               ; 0=Ctrl+V / 1=Shift+Insert
RowsVisible=10           ; 弹出框一屏显示行数
PopupPosition=0          ; 0=鼠标 / 1=光标 / 2=上次位置
HoverPreview=1           ; 1=悬停 300ms 后预览 / 0=仅 Ctrl+悬停预览
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
  ├─ OpenClipboard：最多 4 次尝试，间隔 20ms；全失败则放弃
  │  （Office 和部分安装程序在复制后的一瞬仍持有剪贴板）
  ├─ 来源程序：GetClipboardOwner() → GetForegroundWindow() → 进程名
  ├─ 黑名单：来源程序命中规则 → 丢弃（详见下文「应用黑名单」）
  ├─ 排除标记检查 → 丢弃（详见下文「排除标记」）
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

### 排除标记

部分程序会请求不被记录。Windows 对此没有统一约定，而区分这几种约定正是全部难点所在——把一个「值为假才排除」的标记当成「存在即排除」来读，会把所有主动声明自己可被记录的程序全部排除掉。

| 格式名 | 判定规则 |
|---|---|
| `Clipboard Viewer Ignore` | 存在即忽略 |
| `ExcludeClipboardContentFromMonitorProcessing` | 存在即忽略 |
| `CanIncludeInClipboardHistory` | **仅当值为假时**忽略 |

这三个标记见 [微软官方文档](https://learn.microsoft.com/en-us/windows/win32/dataxchg/clipboard-formats#cloud-clipboard-and-clipboard-history-formats)，Windows 凭据管理器和绝大多数第三方密码管理器都在用。

`CanIncludeInClipboardHistory` 是**按值退出**：程序把它放上剪贴板时，`1` 表示「可以记录我」，`0` 才是请求排除。所以它存在与否本身不传递任何信息。

**故意不遵守：`CanUploadToCloudClipboard`。** 它和上面几个一同注册，看上去也像隐私标记，但它表达的意思是「不要把这个上传到云剪贴板」。ClipWiz 从不向任何地方上传——单个离线可执行文件就是它的核心设计约束——所以这条限制根本不适用；遵守它只会因为一个本程序压根不使用的服务而丢掉大量正常的本地复制。zsclip 得出的是同一个结论，并专门写了测试断言这件事。

**值解析**（`privacy::PayloadMeansFalse`）：文档说这个值是 DWORD，但剪贴板上并不携带类型信息，实际会遇到 DWORD、WORD、ASCII 文本、UTF-16LE 文本四种形式。光看字节宽度区分不了它们——`"no"` 和一个 WORD 都是两字节——因此：只由可打印 ASCII 和 NUL 组成的载荷按文本读（`0` / `false` / `no`，不分大小写），其余按 2 或 4 字节整数读。空的或无法识别的载荷**不算假**：规则是「必须确实读出假值才排除」，对读不懂的字节去猜会静默地丢掉正常复制。

**两处竞态护栏：**

- 标记检查放在读取任何内容**之前**，所以一次被排除的复制只花一个剪贴板往返，而不是先做完整的 RTF/HTML 提取再丢掉。
- 打开剪贴板后立即记下 `GetClipboardSequenceNumber()`，拿到载荷后再复核一次。持有剪贴板并不能阻止一个已经拿到它的线程替换内容，少了这道复核，就可能出现「用新内容的标记去判定旧内容的载荷」。不一致时直接放弃本次捕获，代价为零：那次替换会自己触发一条 `WM_CLIPBOARDUPDATE`，按它自己的情况被捕获。

实现位于 `src/privacy.h`（标记表，每个标记和它的规则写在同一行）和 `src/privacy.cpp`（值解析，以及两个具名谓词 `PresenceIsExclusion` / `FalseValueIsExclusion`）。两者都不打开剪贴板，因此 `tests/test_privacy.cpp` 能在无窗口、无真实剪贴板的情况下覆盖每一条规则。

### 来源程序跟踪

每条捕获的条目都会记下是哪个进程做的这次复制，于是一条记录不仅能凭它的内容、也能凭它来自哪里被辨认出来。

**解析。** 先取 `GetClipboardOwner()`——owner 就是把数据放上剪贴板的那个窗口，正是「复制自」的含义——当所有权是以 NULL 窗口声明时，退化到 `GetForegroundWindow()`。窗口经 `GetWindowThreadProcessId` 变成进程 id，进程 id 再经 `QueryFullProcessImageNameW` 变成名字。

用 `QueryFullProcessImageNameW` 而不是更常见的 `GetModuleFileNameExW`：前者在 kernel32 里，只需要 `PROCESS_QUERY_LIMITED_INFORMATION`，因此能触及 `GetModuleFileNameExW` 会失败的提权进程和 protected-process-light 目标，且不引入新的导入库。

解析在剪贴板仍打开时完成。`GetClipboardOwner()` 只描述即将读取的这份内容，而 owner 窗口在剪贴板释放后随时可能被销毁。

**UWP。** 商店应用可见的窗口属于 `ApplicationFrameHost.exe`，它把真身应用的窗口作为子窗口承载。若报告这个框架，每条 UWP 复制都会被打上同一个没用的名字，所以当解析出的名字是框架时，会枚举子窗口找一个属于不同进程的；若没有，框架就是答案。

**存储。** 转小写、剥掉路径：`C:\Windows\System32\notepad.exe` 存为 `notepad.exe`。Windows 上进程名不分大小写，所以统一成一种写法不丢信息，还能让 `app:` 过滤直接做子串搜索，无需每次按键都折叠大小写。无法解析的 owner 存为空——为什么绝不臆测，见上文「条目结构」。

由于这个字段是持久化的，重新复制相同内容会刷新它：去重命中时，存着的字节会被最新一份替换，来源也跟着走。一条显示 `notepad.exe` 的记录，装的应当就是 notepad 最后一次放进去的东西。

**显示。** 弹出框在每行末尾用小号暗色字体画出这个名字，右对齐、独占一个槽位；预览矩形被相应收窄，于是过长的内容会顶着标签截断，而不是钻到它下面。显示时去掉 `.exe` 后缀，槽位上限为行宽的一半——名字放不下时，内容优先，标签被丢弃。

### 应用黑名单

排除标记覆盖的是那些*主动请求*不被记录的程序。黑名单覆盖的是从不请求的——一个把密钥复制上剪贴板却不带任何标记的密码管理器、一屏滚过凭据的终端、任何用户就是不想留下的东西。它是用户自己维护的一份规则列表，比对的就是上文为来源跟踪解析出的同一个程序。

**两档，**因为「别记录这个程序」和「别让 clipwiz 在这个程序里动作」是两种不同的诉求：

| 档位 | 规则 | 效果 |
|---|---|---|
| 不捕获 | `keepass.exe` | 不记录来自该程序的剪贴板变化。热键照常可用。 |
| 完全禁用 | `!keepass.exe` | 不记录，**且**该程序处于前台时 clipwiz 热键毫无反应。 |

两者唯一的区别就是行首那个 `!`。完全禁用是给那种连弹出历史列表都不希望的场合——演示时、共享屏幕时、kiosk 里。

**匹配。** 一行一条规则。不含 `*` 的规则是普通子串测试；含 `*` 的规则是锚定 glob，`*` 匹配任意长度的字符。每条规则会拿三个字段去试——进程名（`keepass.exe`）、完整进程路径（`c:\program files\keepass\keepass.exe`）、窗口标题（`keepass - mydb.kdbx`）——命中其中任意一个即算匹配。以 `#` 开头的行是注释，空行忽略。

所有比较都转小写，且是*调用方*在匹配前把这三个字段转小写，所以匹配器自己不折叠大小写——和 `filter.cpp` 用的是同一套约定。空字段永不匹配，`*` 也不行：一个没解析出来的窗口标题，不该变成对每条规则的通配命中。

**两道闸门各自的位置。** 捕获侧，黑名单在读取任何内容*之前*检查——就在来源程序解析之后、排除标记之前——于是一次被拦下的复制只花一个窗口查询和一个剪贴板往返，绝不会先做完 RTF/HTML/图片提取再丢掉。粘贴侧，`App::OnHotkey` 解析前台窗口，若判为完全禁用就立即返回；这个检查在规则集为空时短路，所以没设黑名单的用户在热键路径上不付任何窗口查询开销。

**判定**返回命中的最强档——完全禁用压过不捕获、不捕获压过无——一旦达到完全禁用就提前停止，因为它上面没有更强的档了。实现位于 `src/blocklist.h` 和 `src/blocklist.cpp`：对纯字符串做纯逻辑，不碰 Win32、不碰剪贴板，因此 `tests/test_blocklist.cpp` 能在无窗口的情况下覆盖解析、匹配与判定。

### 预览脱敏

黑名单阻止的是密钥被*存下来*。脱敏是另一半：对于那些已经在历史里的内容——粘进表单的手机号、一张身份证、从便签复制来的密码——它阻止密钥在*屏幕上被读到*。凡 clipwiz 把文本回显给用户的地方，它都会打码：列表行预览、搜索字段、悬停预览。

**绝不触碰的是 `item.data`。** 打码只作用于派生的 `preview`、`searchText` 和悬停视图。存着的字节是完整的原文，所以真正粘贴出去的永远是全份——这是把密钥藏起来不让瞥一眼看到，而不是把它从条目里删掉。

**五个手写扫描器，**每个都能独立开关：

| 扫描器 | 识别 | 默认 |
|---|---|---|
| 邮箱 | `local@domain.tld`，TLD 至少 2 个字母 | 关 |
| 手机号 | 中国大陆手机号：`1[3-9]` 后接 9 位数字，容忍 `+86` 与分隔符 | 开 |
| 身份证 | 18 位居民身份证：区域、出生日期（含闰年）与 GB 11643 MOD 11-2 校验位全部验证 | 开 |
| API 密钥 | 已知前缀（`sk-`/`pk-`/`ghp_`/`AKIA`/`AIza`/…）+ 分隔符 + ≥20 个 `[\w-]` | 关 |
| 密码 | 整段文本 8–64 字符、无空白，且同时含大写 + 小写 + 数字 + 符号 | 开 |

命中的片段保留首尾几个字符、中间替换为 `****`，于是条目仍可辨认却读不出全文。邮箱保留 `@域名`，只打码本地部分。

**两道护栏，**都从 TieZ 沿用：超过 5000 字符的文本原样返回（一大坨内容不值得在每次按键重算时去扫），以 `data:` 开头的文本原样返回（data URI 是机器生成的内容，不是用户敲进去的密钥）。

**故意不用 `std::regex`。** 这些模式简单到手写就能扫，手写扫描器在每次按键触发的重算上都更快，且不占 `std::regex` 那点代码体积——和让整个工具保持为单个小 exe 是同一套理由。

**大小写与两个派生字段。** `preview` 保留原始大小写，所以每个扫描器都在它上面生效，包括密码启发式（它需要一个大写字母）。`searchText` 存之前已转小写，所以只有大小写无关的结构类扫描器——邮箱、手机号、身份证、API 密钥——在那里命中，密码启发式则无害地跳过。这个分工是可以接受的：密码在列表行和悬停视图里仍被藏住，`searchText` 从不显示在屏幕上，而 `data` 从不打码。若两者同处一个字符串，先打码再转小写才是唯一正确的顺序，但它们并不在一处——`MakeItemSearchText` 内部转小写，所以 `FillDerived` 是对它已转小写的结果打码。

**在哪些地方生效。** `store.cpp` 的 `FillDerived` 在计算两个派生字段的同时就打码，于是所有构造条目的路径——`Add`、`Load`、`ConvertToPlainText`、`RefreshPreviews`——都被这一处覆盖。悬停预览直接对 GDI 文本路径打码。RTF 是例外：它的悬停视图把原始 RTF 流式送进 RichEdit 控件，无法就地打码，所以当脱敏激活*且*会改变该条文本时，就跳过富渲染、让悬停退回去画打码后的纯文本——这一条预览会丢掉格式，这是不泄漏密钥的正确代价。脱敏关闭时，`AnyEnabled` 短路，富路径与重算的开销和这功能存在之前完全一样。

**去重不受影响。** canonical 哈希是对 `item.data`（原始字节）算的，从不对打码后的预览算，所以仅在一个打码片段上不同的两条仍能正确去重，一条打过码的条目也仍与它自己的再次复制相匹配。

实现位于 `src/mask.h` 和 `src/mask.cpp`：对纯字符串做纯逻辑，不碰 Win32、不碰剪贴板、不碰配置，因此 `tests/test_mask.cpp` 能在无窗口的情况下覆盖每个扫描器、两道护栏以及片段合并顺序。

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
6. SendInput 发送配置的粘贴组合键（默认 Ctrl+V，可选 Shift+Insert），四个事件 修饰键↓ 主键↓ 主键↑ 修饰键↑ 在一次调用里全部发出，**按扫描码**注入
7. 更新条目 usedAt，将条目提到列表最前（置顶项留在置顶区）

### 为何用扫描码

组合键以 `wVk = 0` + `KEYEVENTF_SCANCODE` 注入，而不是发虚拟键事件。只带 `wVk` 的事件没有硬件扫描码，而相当多的接收方会直接丢弃这种事件：RDP 等远程栈、虚拟机、用 raw input 读键盘的程序，以及靠扫描码区分左右修饰键的编辑控件。`wVk` 置零后，这个事件在驱动之上的每一层看来都和真实按键无法区分。

两个必须注意的细节：

- `MapVirtualKeyW(vk, MAPVK_VK_TO_VSC)` 只给扫描码，不告知这个键是否属于扩展键，而 `VK_INSERT` **就是**扩展键：不带 `KEYEVENTF_EXTENDEDKEY` 直接注入它的 `0x52`，到达目标时完全变成了另一个键。`MAPVK_VK_TO_VSC_EX` 会在 `0xE000` 位里报出 `E0` 前缀，所以扩展与否是推导出来的，而不是逐键写死的。
- 如果当前键盘布局映射不了其中某个键，代码退化到虚拟键形式。那种形式正是远程桌面容易丢的，但本地仍可用；一个能到达大多数程序的粘贴，胜过一个静默地谁都到达不了的粘贴。

`ReleaseHeldModifiers()` 故意保留虚拟键方式。它的职责是抬起用户**物理按住**的修饰键，而只有 VK 能说明抬的是哪一个；扫描码不带扩展标志就分不出左右，弄错会把一个修饰键永久卡在按下状态。

### 粘贴组合键可配

config.ini 的 `PasteKey` 在 `CtrlV`（0，默认）和 `ShiftInsert`（1）之间选择。Ctrl+V 是几乎通用的绑定；Shift+Insert 是更早的那个，但对那些把 Ctrl+V 绑到别的功能的程序、以及把 Ctrl+V 当成字面控制字符的终端类宿主，它仍然是正确答案。开关在「快捷键」页；它的两个选项是键名而不是叙述文本，所以不参与翻译。

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

### 保存时的校验

点击“确定”会先把所有快捷键读进局部变量，校验通过后才写入生效的配置（`Config& cfg` 是全局配置的引用，被拒的组合绝不能写进去）。遇到第一个问题就弹出模态提示框、保持对话框打开且什么都不提交，用户必须换一个组合，而不是“确认后继续”。共拒绝三类：

- **没有真正的修饰键**：光一个主键、或只带 Shift（会吞掉正常的大写输入）都被拒，必须含 Ctrl / Alt / Win。
- **系统常用组合**：Ctrl+数字、Ctrl+C/V/X/Z/A/S、Alt+Tab 与浏览器、编辑器和系统本身赖以工作的快捷键冲突，直接拒绝并说明具体原因。
- **组合重复**：队列槽或某个置顶位置复用唤出组合，会以针对唤出键的专门提示拒绝；其他任意一对重复（队列与某个置顶槽、或两个置顶槽用了同一组合）也因含义不清而拒绝，因为一次按键会对应两个动作。

未绑定的槽位（没设键）视为正常，直接跳过。这道校验与上面的冲突处理是两回事：校验拒绝的是 clipwiz 根本不会去注册的组合，而 `RegisterHotKey` 仍可能在运行时因别的程序已占用而失败。校验复用了 `hotkey::IsUsable`（修饰键规则）和 `hotkey::LooksRisky`（系统常用规则），现在由设置对话框在“确定”时驱动。

---

## 界面实现

全部使用 Win32 原生控件 + GDI 自绘，不引入 UI 框架。字体取 `SystemParametersInfo(SPI_GETNONCLIENTMETRICS)` 中的系统 UI 字体。

### 托盘图标

- 左键单击：打开快速粘贴框
- 右键：菜单（快速粘贴 / 设置 / 开机自启 / 清空历史 / 备份到文件 / 从文件恢复 / 关于 / 退出）
- 悬浮提示：`ClipWiz`（静态文本，可通过 `tray::SetTip` 更新）

### 备份与恢复（.clpw）

托盘菜单里的两条命令通过单个文件搬移整段历史：**备份到文件**把它写出，**从文件恢复**把一份合并回来。扩展名是 `.clpw`，但并没有独立的备份格式——一个 `.clpw` 文件*就是*一份 `store.dat`，即 `Store::Serialize()` 产出的那份 v3 字节，所以导出无需自己的编码器。

**导出**会建议文件名 `clipwiz-backup-<时间戳>.clpw`，通过 `GetSaveFileNameW` 取得路径，再用 `util::WriteFileAtomic` 写出 `Serialize()`——与后台保存所用的同一套“先写 tmp 再原子替换”，因此半截的备份永远不会冒充完好文件。它在 UI 线程上同步执行：这是用户正在等待的一次性模态操作，而 AsyncWriter 的存在是为了把*频繁的*自动 `store.dat` 保存挪出 UI 线程，并非用来序列化任意的导出目标。

**导入**把选中的文件整体读入（`util::ReadWholeFile`），交给 `Store::ImportMerge`。该方法通过 `ParseBuffer` 解析——这是 `Serialize()` 布局唯一真正的读取器，现由 `Load()` 与 `ImportMerge()` 共享，于是“什么才算有效备份”只定义一次——解析进一个绝不触碰现有状态的临时列表，然后把每一条都走一遍 `Add()`。走 `Add()` 正是整个设计的关键：导入因此免费获得内容去重、置顶区保护与容量淘汰，和一次全新复制完全一致。

有三个决策塑造了失败与合并的行为：

- **单一版本，不做迁移。** `ParseBuffer` 只接受本构建所写的 v3 布局；其余一切——坏 magic、截断的尾部、更旧的版本——都被拒绝，`ImportMerge` 返回 `-1`。这与 store 在加载时只认单一版本的立场一致。
- **被拒的导入不改动任何东西，也不碰文件。** 返回 `-1` 时，现有 store 保持逐字节不变。与启动时遇到损坏 `store.dat` 不同，出问题的文件*不会*被改名留存：它是用户指给我们的文件，而非我们自己的数据库，悄悄挪走它未免自作主张。调用方只是报告“不是可读取的备份”。
- **导入的条目落地为未置顶。** 它们以全新 id 与时间戳作为新条目到来，并带上其记录的 `sourceApp`。这是*内容合并，而非状态恢复*——置顶是对当前库的决定，让一个备份文件静默填满永不淘汰的置顶区，可能把 store 楔死在容量上限。因此“保护置顶”指的是导入绝不打扰已在库中的置顶项，而非导入项保留其置顶标志。

成功后弹窗刷新，合并后的 store 用 `SaveNow()` 落盘（异步，与清空历史一致），并弹出信息框报告合并条数。`tests/test_store.cpp` 以八个用例对合并做了 headless 覆盖——经 `Serialize()`→`ImportMerge` 的完整往返、未置顶落地、与现有内容去重、现有置顶项保持不变、损坏与截断输入被拒且 store 不变、错误版本被拒、空备份作为空操作、以及容量上限被强制。`ParseBuffer`/`ImportMerge` 是 `Store` 的成员方法而非独立纯模块——它们读写 `items_`——因此是通过 store 来验证，而不像 `merge` 或 `transform` 那样被隔离。

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
- 键盘操作：↑↓ 移动、Enter 粘贴、Esc 关闭、Alt+1~9 直接粘贴、Ctrl+D 删除、Ctrl+P 切换置顶、Ctrl+↑/↓ 调整光标所在置顶项的顺序
- 多选：Shift+↑/↓ 扩展选区、Ctrl+单击切换某行、Shift+单击选中一段区间、Ctrl+A 全选；选中行用主题的选中底色绘制。选中集合是一组有序去重的条目 id（而非行号），因此在过滤重建后依然稳定；常见的单光标场景下它为空，所以「打字 → 回车 → 粘贴」这条主路径完全不碰它、行为与从前一模一样。键盘扩选用 Shift 而非 Ctrl，是因为 Ctrl+↑/↓ 已被置顶项重排占用。
- 鼠标操作：单击选中、双击粘贴、右键上下文菜单
- 悬停预览：长文本显示完整内容，图片显示放大预览（详见下文「搜索与过滤」）
- 置顶项支持拖动调整顺序
- 失去焦点自动隐藏（WM_ACTIVATE / WA_INACTIVE）
- 双缓冲绘制（内存 DC + BitBlt），无闪烁
- 窗口只创建一次，后续 show/hide 复用

### 合并选中项

弹出框的多选主要是为了喂给一个操作：把若干条目合并成一条新条目。右键任意选中行，**合并选中项** 把它们合并；除非选区里至少有两个可合并的行，否则该项置灰。

规则：

- **文本类**（Text / HTML / RTF）之间可自由合并。每一条先经 `CanonicalBody` 抽成纯文本，所以一个 HTML 行和一个 RTF 行会合并成一条纯 **Text** 条目——格式被有意丢弃，因为没有合理办法把两个富文本文档拼接起来。
- **文件列表**（FileDrop）只能和文件列表合并。clipwiz 把一次拖放存成「一行一个路径」，所以合并就是按行拼接：去掉每个 body 末尾的换行，用单个 `\n` 连接，再补上结尾换行，使结果与一次全新捕获的拖放解析方式完全一致。输出仍是 **FileDrop**。
- **图片被拒绝**——合并图片意味着画布合成，超出了一个保持轻量的管理器的范围。
- **文件列表与文本混选被拒绝**——路径和正文拼不出任何值得粘贴的东西。

合并文本 body 之间的分隔符可配（设置 → 常规 → *合并分隔符*）：空行（默认）、换行、空格，或自定义的单行字符串。它只作用于文本合并；文件列表永远用换行拼接，因为其格式固定。

结果是通过 `Store::Add()` 生成的**全新条目**——绝不原地修改来源——因此自动享受 canonical 去重：若结果已在历史中，就浮现那条已有条目，而不是存一个重复。实现位于 `src/merge.h` 和 `src/merge.cpp`：纯逻辑（条目进、合并后的 kind+data 出），不碰窗口、不碰剪贴板、不碰 store 修改，因此 `tests/test_merge.cpp` 能在无窗口情况下覆盖每条规则。这是比 Ditto 的 `IClipAggregator`（按格式的多态累加器）刻意更扁平的形状：Ditto 需要那形状是因为它要重建二进制 `DROPFILES` blob，而这里每次合并都是字符串拼接，所以两个函数就能把 canonical 抽取和类型分派一样藏好，无需那套机械。

### 文本变换

右键任意文本行，两个子菜单提供同一组固定的十三种变换：**变换后复制** 把结果存成一条新条目，**变换后粘贴** 直接写入剪贴板并粘贴、不动历史。另有一个 **粘贴为纯文本**，是 *转为纯文本* 的粘贴时姊妹操作——它在出站途中把 HTML 或 RTF 行压成纯文本，而不是修改已存的条目。

这组变换借的是 Ditto `CSpecialPasteOptions` 的*形状*——一份固定的具名操作清单，而非规则引擎——与「不用正则、保持轻量」的边界刻意契合：选一个变换是一个枚举值，绝不是一个要编译的模式。

| 变换 | 效果 |
|---|---|
| 去除首尾空白 | 去掉开头/结尾的空白 |
| 去掉所有换行 | 删除每一个 CR/LF，把各行粘在一起 |
| 段落间单换行 | 把段落间隔规整为恰好一个换行 |
| 段落间空一行 | 把段落间隔规整为一个空行 |
| 全大写 / 全小写 | 每个字符转大写 / 转小写 |
| 每词首字母大写 | 每个词首字母大写、其余小写 |
| 句首大写 | 每句首字母大写、其余小写 |
| 驼峰式 | `some text here` → `someTextHere` |
| 反转大小写 | 交换每个字母的大小写 |
| 只保留 ASCII | 丢弃所有 U+007F 以上的码点 |
| 生成 slug | 小写 URL slug：连续的非 `[a-z0-9]` 折叠成一个分隔符 |
| 追加日期时间 | 在末尾接上当前本地时间戳 |

变换只作用于 **Text / HTML / RTF**；对图片和文件列表，两个子菜单都置灰——它们的载荷（原始 PNG 字节、一行一个路径）没有可供大小写折叠或 slug 化的正文。

Slugify 拼接用的分隔符可在 config.ini 配置（`SlugSep`，默认 `-`）；空值回退到 `-`，以免词被悄悄粘在一起。它有意不进设置对话框——Slugify 只是十三种变换之一、且是极少改动的旋钮，不值当占一个 UI 字段。

实现位于 `src/transform.h` 和 `src/transform.cpp`：`Apply(kind, text, options)` 是一个纯粹的 `std::wstring → std::wstring` 函数，不碰窗口、不碰剪贴板、不碰配置、也不碰时钟。调用方先把 Item 转成文本（`textconv::CanonicalBody`），所以这个单元从不接触 `store.h`。*追加日期时间* 通过把时间戳作为注入的字符串来保持 `Apply` 纯净——生产环境传入格式化后的本地时间、测试传入固定字面量——而不是在内部读时钟，`tests/test_transform.cpp` 则在无窗口情况下覆盖每条规则。Ditto 的 Typoglycemia、pasteAsImage、PosixifyPaths 和 GUID 生成被有意不借（都不是剪贴板管理的必需品），而它的 slugify 尤其会拖进 `<regex>` 加一张 200 项的重音折叠表；clipwiz 的 slugify 保持 ASCII 极简——任何非 `[a-z0-9]` 都折叠成分隔符、不做音译，与「只保留 ASCII」的「丢弃而非音译」哲学一致。

### 顺序粘贴队列

多选除了合并，还喂给第二个操作：**加入粘贴队列** 把选中的条目排成一队，供逐条粘贴。每按一次队列热键（默认 **Ctrl+Alt+J**，可在设置 → 快捷键里、紧挨唤出热键处配置），就粘贴队首那条并把它移出队列，所以按出一个节奏就能顺着列表一路走下去——正是「先复制五样东西，再按顺序粘到五个地方」这一经典流程。

队列就是 `App::queue_`，一个普通的 `std::vector<uint64_t>`（条目 id）。它是**只在运行时存在的工作状态、从不落盘**：它不是数据，而是你此刻正在做的事，所以重启即清空、store 格式分毫不动。加入一个选区会按顺序追加其 id，并跳过已在队列中的，以免重复加入把同一条粘两次。

弹出复用 `PasteItem`——与普通粘贴同一条路径——所以队首那条会在自写标记下写入剪贴板（不会被重新捕获进历史）、经 `Touch` 提前、并把这次重排调度落盘。若某 id 的条目在此期间已被删除，则跳过它而不是白白吞掉一次按键；**空队列则静默无操作**：热键绝不弹错误框。

在队列中的行，其右边缘戴一个强调色的小药丸，显示它的 1 起始位次。只有在队列中的行才保留这条宽度，所以常见的空队列情形仍占满文本宽度；在置顶行上，角标恰好排在重排图标左侧，行尾读作 `[文本][来源 App][角标][排序图标]`，互不重叠。

形状参照 TieZ 的 `paste_queue.rs`（GPL，此处重写）：一个 id 的 FIFO，每按一次弹出并粘贴队首。它刻意更精简——TieZ 还携带上次粘贴的指纹和「粘贴后删除」步骤，两者都不契合 clipwiz 的模型，所以只保留 id 列表。它没有纯模块、也没有测试文件：队列只是在一个住于 `App` 的 vector 上做三个平凡操作（去重追加、位次查询、弹出队首），没有可供无窗口测试的算法——不像合并或变换，那些的分支才值当测。

### 另存为文件 / 在资源管理器中显示

右键某一行，还有一个槽位把它写出为真正的文件。标签随行类型自适应：对任何有内容可写的行显示**另存为文件**，对文件列表显示**在资源管理器中显示**——因为捕获的一次拖放是磁盘上已存在文件的路径列表，没有东西可“存”，有用的动作是把它们显示出来。

- **文本 / HTML / RTF → `.txt`。** 抽取出的纯文本（`Store::TextOf`，与合并、变换所依赖的同一套规范化抽取）经 `util::Narrow` 以 UTF-8 写出。HTML 与 RTF 在此丢失其格式，这是有意为之：另存为是为了把*文字*弄进文件，而保留每一个原始字节正是整段历史 `.clpw` 备份的职责。
- **图片 → `.png`。** 图片载荷在磁盘上本就以 PNG 存储（见图片处理），因此存储的字节原样写出——不重编码、不走 WIC 往返、无质量损失。
- **文件列表 → 资源管理器。** `SHOpenFolderAndSelectItems` 打开首个路径的父文件夹并选中该文件。一次拖放可能横跨多个文件夹，而资源管理器在单个文件夹内选择，因此显示首个路径是诚实、可预期的选择，而非跨多文件夹的部分选中。

两条保存路径都经过 `GetSaveFileNameW` 与 `util::WriteFileAtomic`——与备份导出所用的同一套原子写——默认文件名为 `clipwiz-<时间戳>.txt` / `.png`。文件名基于时间戳而非内容，是刻意的：从文本派生意味着要净化非法字符、避开保留的 DOS 名（`CON`、`PRN`）、处理空内容，而这一切只是为了产出一个用户马上就会在对话框里改掉的文件名。

该动作是同步且模态的，并先隐藏弹窗（`PasteTransformed` 所用的形态），因为它是用户正在等待的一次性操作——而非 AsyncWriter 存在的意义所在、那些频繁的自动 `store.dat` 保存。

与粘贴队列一样，没有纯模块也没有测试文件：这是一个通用对话框、一次 shell 调用和一次文件写入，没有可 headless 验证的算法。唯一的解析——拆分 FileDrop 载荷那“每行一个路径”的 UTF-16LE——很简单，且与 `merge.cpp` 已覆盖的文件列表处理共用同一格式。

### 搜索与过滤

ClipWiz 没有标签、没有分组、没有文件夹——这条边界是故意划的——所以过滤框是组织条目的**唯一**手段，也是整个程序里承重最大的 UI 逻辑。

**搜的是什么。** 每个条目带一个只在运行时存在的 `searchText` 字段：完整的抽取文本，已小写化，从不落盘（它是派生数据，而不变量是派生数据必须能随时从 `data` 重建）。

- 文本 / 文件列表：整个载荷。对文件列表而言就是**每一个**路径，而不是预览里只显示的第一个文件名。
- Html / Rtf：抽取出的纯文本，上限 4096 个 wchar，以防一个病态的几 MB RTF 把一次 `Add()` 或 `Load()` 变成全文档解析。
- 图片：它的预览字符串——本地化的 `[Image W×H]` 是一张图片唯一拥有的文本。

以前比对的是 `preview`，一个 160 字符的单行摘要，所以第 160 字之后的内容搜不到，搜文件选区里的第二个文件也永远搜不到。

`searchText` 存的就是**已经小写化**的文本，而查询在 `Parse()` 里每次按键只小写化一次。于是每条条目的代价就是一次不分配内存、不做大小写折叠的子串查找，比旧代码（每次按键都把每条的 `preview` 拷一份再逐字 towlower）更便宜。在 9999 条上限下，每次按键全量扫描完全在预算之内。

**多关键词 AND。** 过滤文本按空白切词，**每一个**词都必须在内容里出现。顺序无关，所以 `report q3 draft` 能找到以任意次序包含这三个词的条目。

**token。** 一个词只有在同时满足三个条件时才被当成查询 token：它处在过滤文本的**开头**、它的前缀是我们认识的、而且冒号后面有内容：

| token | 作用 |
|---|---|
| `kind:text` | 只要纯文本条目 |
| `kind:rich` | 只要 Html 或 Rtf 条目 |
| `kind:image` | 只要图片 |
| `kind:file` | 只要文件列表 |
| `app:<进程名>` | 对来源进程名做子串匹配 |

开头 token 之后的部分全部是关键词，所以 `kind:image screenshot` 的意思是「内容里提到 screenshot 的图片」。

这份宽容正是重点。它首先是一个搜索框，其次才是一个查询语言：如果把查询里靠后出现的冒号也吞掉，`https://example.com` 或 `注意:kind` 这类完全正常的搜索会静默地返回零结果。同理，认不出的 `kind:` 值（`kind:pdf`）也不动它，当普通文本去搜。`app:` 在来源跟踪给条目记下进程名之前什么都匹配不到——宁可诚实地给空结果，也不给假阳性。

解析和匹配位于 `src/filter.h` / `src/filter.cpp`：进原始文本，出判定结果，不碰窗口也不碰 store，所以 `tests/test_filter.cpp` 能在无窗口环境下覆盖它。

**悬停预览。** 鼠标在一行上停 300ms 就显示它的预览；按住 Ctrl 则立即显示。这个延时是为了让鼠标扫过列表时不会逐行闪窗口，而计时器只在**悬停的行发生变化**时重新计时——如果每收到一条 `WM_MOUSEMOVE` 就重新计时，那只要手在动（而大多数时候手都在动）预览就永远不会出现。config.ini 里 `HoverPreview=0` 可以把预览限制为只由 Ctrl+悬停触发，给那些嫌窗口不请自来的人；Ctrl 这条路两种设置下都有效。

### 设置对话框

单窗口分组式布局（非 Property Sheet / 多 Tab）。运行时通过内存 DLGTEMPLATE + `DialogBoxIndirectParamW` 创建，所有控件在 `WM_INITDIALOG` 中动态生成。.rc 文件中不含任何对话框资源。

**[常规分组]**
- 开机自动启动（checkbox）
- 保存条数上限（数字框）
- 最多保存天数（数字框）
- 语言（下拉框：跟随系统 / English / 简体中文）
- 主题（跟随系统 / 浅色 / 深色）
- 窗口弹出位置（鼠标指针处 / 光标处 / 上次打开的位置），同一行附「悬停时预览」checkbox
- 显示字体（按钮显示字体名+字号，附"重置"按钮；调用 ChooseFontW）
- 数据库路径（只读路径框 + 浏览按钮；调用 SHBrowseForFolderW）

**[快捷键分组]**
- 唤出快速粘贴框（hotkey 控件 + "含Win" checkbox）
- 粘贴按键（下拉框：Ctrl+V / Shift+Insert）
- 置顶项快捷键（10 个位置，2×5 网格，每个含 hotkey 控件 + "含Win" checkbox）

对话框靠一个 `y` 游标逐行往下摆，高度在最后从游标取值，所以新增一行不可能把下面的控件裁掉。

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
  app.h/.cpp         全局状态、消息分发、备份 / 另存为动作
  store.h/.cpp       条目库、序列化、淘汰、导入合并
  textconv.h/.cpp    HTML/RTF → 纯文本；去重用的 canonical 形式
  filter.h/.cpp      弹出框过滤文本的解析与匹配
  privacy.h/.cpp     剪贴板排除标记
  blocklist.h/.cpp   按程序抑制捕获与热键
  mask.h/.cpp        预览脱敏（敏感片段扫描器）
  merge.h/.cpp       合并选中项为一条（文本 / 文件列表拼接）
  transform.h/.cpp   文本变换（大小写 / 空白 / slug / 时间戳）
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
  raii.h             RAII 封装（GlobalLock、HANDLE、GDI 对象、剪贴板开关）
  util.h/.cpp        工具函数
  resource.h         控件 ID 与资源常量
  clipwiz.rc         图标、manifest、版本信息（无对话框模板）
lang/
  zh-CN.lng          简体中文语言包
tests/
  testfw.h           断言宏与通过/失败计数——不用第三方测试框架
  test_main.cpp      入口；第一件事就是把数据目录重定向到 scratch 目录
  test_textconv.cpp  只测 src/textconv.cpp
  test_filter.cpp    只测 src/filter.cpp
  test_privacy.cpp   只测 src/privacy.cpp
  test_blocklist.cpp 只测 src/blocklist.cpp
  test_mask.cpp      只测 src/mask.cpp
  test_merge.cpp     只测 src/merge.cpp
  test_transform.cpp 只测 src/transform.cpp
  test_store.cpp     只测 src/store.cpp
```

有七个单元是纯粹的：`textconv`、`filter`、`privacy`、`blocklist`、`mask`、`merge`、`transform`。每一个都输入普通数据、输出普通数据，不碰窗口、不碰剪贴板、不碰配置——其中前三个是从更大的文件里抽出来的，另外四个一开始就是按纯逻辑写的。这就是判定一块逻辑是否该独立成单元的全部标准，也正是 `clipwiz_tests` 能只链接它们、而不把 popup / settings / app / tray / hotkey 一同拖进来的原因。

测试文件名镜像被测单元（`tests/test_filter.cpp` 只测 `src/filter.cpp`）——这是 zsclip 和 QuickClipboard 那种「测试就贴在纯逻辑旁边」布局在 C++ 里的等价物，同样的可发现性，不需要内联测试语法。`clipwiz_tests` 是一个独立的 CMake 目标，用完全相同的 `/W4 /WX` 和静态 CRT 设置构建，标了 `EXCLUDE_FROM_ALL` 以保证常规构建仍然快；`build-tool.bat test` 会构建并运行它。

store 的测试自己重述二进制格式常量，而不是从 `store.cpp` include 过来。如果测试从代码里读格式，那改代码就会静默地连期望值一同改掉，而护栏恰好在最需要它的那一刻失效。

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
| 输入法状态 | 只发粘贴组合键，不逐字符输入，不受输入法影响 |

---

## v2 目标

| 项目 | 说明 |
| --- | --- |
| i18n 查找优化 | T() 英文回退目前线性扫描 kDefaults（~60 条），可改为 unordered_map 统一查找 |
| 托盘 tooltip 增强 | 显示条目数和置顶数，如 "ClipWiz — 42 条记录，3 个置顶"，无需打开窗口即可了解状态 |
