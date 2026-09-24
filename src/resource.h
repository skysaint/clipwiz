// resource.h — Resource ID definitions
#ifndef CLIPWIZ_RESOURCE_H
#define CLIPWIZ_RESOURCE_H

// Icon resources
#define IDI_APPICON                 101
#define IDI_WINBADGE                102

// Per-row content type icons (light / dark theme variants)
#define IDI_TYPE_TEXT_LIGHT         110
#define IDI_TYPE_TEXT_DARK          111
#define IDI_TYPE_RTF_LIGHT          112
#define IDI_TYPE_RTF_DARK           113
#define IDI_TYPE_HTML_LIGHT         114
#define IDI_TYPE_HTML_DARK          115
#define IDI_TYPE_IMAGE_LIGHT        116
#define IDI_TYPE_IMAGE_DARK         117
#define IDI_TYPE_FILE_LIGHT         118
#define IDI_TYPE_FILE_DARK          119

// Built-in language resources (compiled from lang/*.lng)
#define IDR_LNG_ZHCN                2001

// 设置页签控件
#define IDC_SETTINGS_TABS          1090

// Settings dialog pages
#define IDD_PAGE_GENERAL            201
#define IDD_PAGE_TYPES              202
#define IDD_PAGE_SHORTCUTS          203

// General page controls
#define IDC_AUTOSTART               1001
#define IDC_MAXHISTORY              1002
#define IDC_EXPIRYDAYS              1003
#define IDC_CLEAN_ON_EXIT           1004
#define IDC_LANGUAGE                1005
#define IDC_THEME                   1006
#define IDC_POPUPPOS                1007
#define IDC_FONT_BTN                1008
#define IDC_FONT_RESET              1009
#define IDC_DATADIR                 1010
#define IDC_DATADIR_OPEN            1011
#define IDC_HOVER_PREVIEW           1012
#define IDC_MERGE_SEP               1013   // text-merge separator combo
#define IDC_MERGE_SEP_CUSTOM        1014   // custom separator field, enabled only in Custom mode

// Types page controls
#define IDC_TYPELIST                1020
#define IDC_TYPEDESC                1021

// Privacy page controls
#define IDC_BLOCKLIST               1030   // multiline per-app blocklist rules
#define IDC_MASK_PHONE              1031   // preview desensitization toggles
#define IDC_MASK_IDCARD             1032
#define IDC_MASK_PASSWORD           1033
#define IDC_MASK_EMAIL              1034
#define IDC_MASK_APIKEY             1035

// Shortcuts page controls
#define IDC_POPUP_HK                1040
#define IDC_POPUP_WIN               1041
#define IDC_PASTE_KEY               1042   // Ctrl+V vs Shift+Insert combo
#define IDC_QUEUE_HK                1043   // sequential paste-queue hotkey
#define IDC_QUEUE_WIN               1044   // Win-key checkbox for the queue hotkey
#define IDC_PIN_HK_BASE             1050   // 1050..1059
#define IDC_PIN_WIN_BASE            1070   // 1070..1079

// Error codes
#define ERR_COMMON_CONTROLS         1000
#define ERR_WIC_INIT                1001
#define ERR_WINDOW_CLASS            1002
#define ERR_WINDOW_CREATE           1003
#define ERR_CLIPBOARD_LISTENER      1004
#define ERR_TRAY_ICON              1005
#define ERR_ASYNC_WRITER           1006
#define ERR_POPUP_INIT             1007
#define ERR_COM_INIT                1008

#endif // CLIPWIZ_RESOURCE_H
