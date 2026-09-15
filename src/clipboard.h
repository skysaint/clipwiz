// clipboard.h — System clipboard listening, reading, and writing
//
// Capture priority: RTF > HTML > Image > FileDrop > Text
// Write-back restores the original format + plain text fallback
#pragma once

#include <windows.h>

#include <cstdint>
#include <string>
#include <vector>

#include "blocklist.h"
#include "store.h"

namespace clip {

bool StartListening(HWND hwnd);
void StopListening(HWND hwnd);

// Whether this clipboard change was written by us (determined by clipboard sequence number)
bool IsSelfWrite();

// Resolve the blocklist match target for a window: lowercased process name,
// full image path, and window title. Any field that cannot be resolved is left
// empty. For a UWP app the name and path come from the hosted child process,
// not the ApplicationFrameHost that owns the visible window.
blocklist::Target ResolveTarget(HWND hwnd);

// Read current clipboard, pick one primary format by priority. Returns false if oversized or excluded.
//
// `sourceApp` receives the lowercased process name the copy came from (see
// Item::sourceApp), or stays empty when the owner cannot be resolved. It is
// filled in before any content is read, so callers that need to decide whether
// to record this copy at all can do so without paying for the extraction.
//
// `block` is the parsed blocklist. When the source window matches any rule, the
// copy is dropped before extraction — the same "decide first, read later"
// ordering the exclusion markers use.
bool Capture(ItemKind& kind, std::vector<uint8_t>& data, uint32_t& imgW, uint32_t& imgH,
             std::wstring& sourceApp, const blocklist::RuleSet& block, uint32_t maxTextBytes,
             uint32_t maxImagePixels);

// Write item content back to system clipboard
bool WriteItem(HWND owner, const Item& item);

}  // namespace clip
