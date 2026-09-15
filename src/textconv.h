// textconv.h — Plain-text extraction and canonical-content comparison
//
// Everything declared here takes plain data in and returns plain data out:
// no window, no clipboard, no config, no i18n, no logging. That is the whole
// point of this unit — it can be exercised headless from tests/ without
// dragging in the UI, and it keeps store.cpp about storage only.
//
// Derived-data rule: text produced here is always recomputed from Item::data
// and never persisted. See doc/technical.md.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "store.h"

namespace textconv {

// Extract plain text from "HTML Format" raw data (rough: strip tags).
// maxBytes limits output length (in UTF-8 bytes before final decode); 0 = unlimited.
std::wstring HtmlToPlainText(const std::vector<uint8_t>& data, size_t maxBytes = 0);

// Extract plain text from RTF raw data.
// Handles: \uNNNN (Unicode), \'xx (code page bytes), \ansicpgN, \ucN
// Uses a per-group skip stack so nested destinations are handled correctly.
// maxChars limits output length (in wchar_t); 0 = unlimited.
std::wstring RtfToPlainText(const std::vector<uint8_t>& data, size_t maxChars = 0);

// Canonical dedup hash.
//
// Two entries are considered "the same content" when their *meaningful*
// content matches, not their raw clipboard bytes. Rich text (RTF/HTML) from
// editors like Word embeds volatile bytes (revision ids, timestamps, font
// tables, GUIDs) that differ on every copy of the exact same passage, so
// hashing raw bytes wrongly treats them as distinct. Instead we hash the
// extracted plain-text body.
//
// A per-kind prefix keeps different kinds distinct: a plain-text "hello" and
// an RTF "hello" must remain two separate entries (one carries formatting).
// Images have no text body, so they hash their raw (PNG) bytes.

// The meaningful body used for dedup: extracted plain text for text kinds,
// raw bytes for images. Empty for rich text that carries no extractable text
// (formatting-only or unparseable) — callers must NOT treat two empty bodies
// as duplicates; fall back to raw-byte comparison in that case.
std::vector<uint8_t> CanonicalBody(ItemKind kind, const std::vector<uint8_t>& data);

// Short ASCII tag mixed into the hash so different kinds never collide.
const char* CanonicalPrefix(ItemKind kind);

uint64_t CanonicalHash(ItemKind kind, const std::vector<uint8_t>& data);

// The single shared "these two are the same content" predicate, used by
// Store::Add(), Load() dedup, and conversion merges. Two items match when they
// are the same kind and same canonical hash AND either:
//   - the incoming canonical body is non-empty (a real plain-text/image match), or
//   - their raw bytes are byte-equal (fallback for empty-body rich text and as
//     a hash-collision safety belt — prevents unrelated formatting-only RTF/HTML
//     from merging just because both extract to empty text).
// `incomingBodyEmpty` is precomputed once by the caller to avoid re-extracting
// the incoming plain text on every comparison.
bool SameCanonicalContent(const Item& existing, ItemKind kind,
                          const std::vector<uint8_t>& data, uint64_t hash,
                          bool incomingBodyEmpty);

// Convenience overload: extracts the body itself (single-comparison callers).
bool SameCanonicalContent(const Item& existing, ItemKind kind,
                          const std::vector<uint8_t>& data, uint64_t hash);

}  // namespace textconv
