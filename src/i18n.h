// i18n.h — 国际化
//
// 英文内置在代码里（不需要 .lng 文件），其他语言从 lang/<code>.lng 加载。
// .lng 格式：UTF-8 纯文本，key=value，# 开头为注释。
#pragma once

#include <string>

namespace i18n {

// 传入语言代码（如 "zh-CN"），会去 exe 目录下 lang/zh-CN.lng 加载。
// 传空串或 "en" 表示英文（内置，不加载文件）。
void Init(const std::wstring& langCode);

// 取翻译文本。找不到就返回内置英文。
const wchar_t* T(const char* key);

// 当前语言代码
const std::wstring& CurrentLang();

}  // namespace i18n
