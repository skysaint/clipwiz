// clipboard.h — System clipboard listening, reading, and writing
//
// Capture priority: RTF > HTML > Image > FileDrop > Text
// Write-back restores the original format + plain text fallback
#pragma once

#include <windows.h>

#include <cstdint>
#include <string>
#include <vector>

#include "store.h"

namespace clip {

bool StartListening(HWND hwnd);
void StopListening(HWND hwnd);

// Whether this clipboard change was written by us (determined by clipboard sequence number)
bool IsSelfWrite();

// Read current clipboard, pick one primary format by priority. Returns false if oversized or excluded.
bool Capture(ItemKind& kind, std::vector<uint8_t>& data, uint32_t& imgW, uint32_t& imgH,
             uint32_t maxTextBytes, uint32_t maxImagePixels);

// Write item content back to system clipboard
bool WriteItem(HWND owner, const Item& item);

}  // namespace clip
