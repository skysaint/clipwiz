// resource.h — Resource ID definitions
#ifndef CLIPWIZ_RESOURCE_H
#define CLIPWIZ_RESOURCE_H

// Icon resources
#define IDI_APPICON                 101

// Settings dialog pages
#define IDD_PAGE_GENERAL            201
#define IDD_PAGE_TYPES              202
#define IDD_PAGE_SHORTCUTS          203

// General page controls
#define IDC_AUTOSTART               1001
#define IDC_MAXHISTORY              1002
#define IDC_EXPIRYDAYS              1003
#define IDC_LANGUAGE                1004
#define IDC_THEME                   1005
#define IDC_POPUPPOS                1006
#define IDC_FONT_BTN                1007
#define IDC_FONT_RESET              1008
#define IDC_DATADIR                 1009
#define IDC_DATADIR_BTN             1010

// Types page controls
#define IDC_TYPELIST                1020
#define IDC_TYPEDESC                1021

// Shortcuts page controls
#define IDC_POPUP_HK                1030
#define IDC_POPUP_WIN               1031
#define IDC_PIN_HK_BASE             1040   // 1040..1049
#define IDC_PIN_WIN_BASE            1060   // 1060..1069

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
