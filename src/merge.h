// merge.h — Combine several clipboard items into one new item
//
// Pure logic: items in, the merged kind+data out. No window, no clipboard, no
// config, no store mutation — the caller (App::MergeItems) decides what to do
// with the result: hand it to Store::Add(), Save, refresh the list. That keeps
// this unit headless-testable from tests/ exactly like textconv and mask.
//
// Why two free functions and not Ditto's IClipAggregator (a two-method
// AddClip/GetHGlobal interface with one implementation per clipboard format):
// Ditto needs that shape because merging there rebuilds binary clipboard
// structures — a CF_HDROP merge has to reconstruct a DROPFILES blob. clipwiz
// stores a file list as plain "one path per line" UTF-16 text, so merging any
// two mergeable kinds collapses to string concatenation. A polymorphic
// accumulator over two near-identical text joins would be machinery for its own
// sake; the genuinely deep part — flattening each kind to its canonical body
// and dispatching on type — hides just as well behind two functions.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "store.h"  // ItemKind, Item

namespace merge {

// What goes between two merged text bodies. File lists ignore this and always
// join on a single newline, because "one path per line" is a fixed format, not
// a stylistic choice. Lives here so settings, App and the tests all name the
// same four options instead of each hard-coding a literal.
enum class Separator : int {
    BlankLine = 0,  // "\n\n" — the default (QuickClipboard merges this way too)
    Newline   = 1,  // "\n"
    Space     = 2,  // " "
    Custom    = 3,  // whatever string the user typed
};

// The literal text a separator stands for. Custom returns `custom` verbatim
// (empty is fine — it means "join with nothing"). An out-of-range value falls
// back to the default rather than trusting a hand-edited config.ini.
std::wstring SeparatorText(Separator sep, const std::wstring& custom = {});

// Can these items become one? Rules:
//   - at least two items, none null
//   - no images: merging pictures would mean canvas compositing, out of scope
//   - no mixing a file list with text: paths and prose do not combine into
//     anything a user would want to paste
// Text kinds (Text/Html/Rtf) merge freely with one another — each is flattened
// to plain text first, so an HTML row and an RTF row can combine into one Text
// entry.
bool CanMerge(const std::vector<const Item*>& items);

// Merge into a single new item's kind+data, ready for Store::Add(). Text kinds
// yield ItemKind::Text joined by `sep`; a file list yields ItemKind::FileDrop
// joined on newlines and re-terminated so the result parses exactly like a
// freshly captured drop. Returns false (leaving out* untouched) if !CanMerge,
// or if a file-list merge has no non-empty path left to emit.
bool Merge(const std::vector<const Item*>& items, const std::wstring& sep,
           ItemKind& outKind, std::vector<uint8_t>& outData);

}  // namespace merge
