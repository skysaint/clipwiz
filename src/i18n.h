// i18n.h — Internationalization
//
// English is built into the code (no .lng file needed); other languages loaded from lang/<code>.lng.
// .lng format: UTF-8 plain text, key=value pairs, lines starting with # are comments.
#pragma once

#include <string>

namespace i18n {

// Pass language code (e.g. "zh-CN") to load lang/zh-CN.lng from exe directory.
// Pass empty string to follow system language (auto-detect system locale and load matching .lng).
// Pass "en" to force English (built-in, no file loaded).
void Init(const std::wstring& langCode);

// Get translated text. Returns built-in English if key not found.
const wchar_t* T(const char* key);

// Current language code
const std::wstring& CurrentLang();

}  // namespace i18n
