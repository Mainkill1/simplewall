# Fix the notification and logging toolbar toggles

[Back to upstream candidates](Home.md) · **Send first · small**

## What changes and why

The app used the toolbar's transient press state as a persistent setting indicator. The fork clears the press bit and applies the checked bit from actual configuration. This fixes a misleading button, not a frozen notification popup.

## Code difference

Selected verbatim diff lines from the fork (omissions marked). This is an illustration, not an apply-ready patch. The merge base, not current upstream, is the left side; the pinned upstream links show what a new PR should target.

```diff
--- a/src/controls.c
+++ b/src/controls.c
@@ -304,2 +304,13 @@ VOID _app_getapptooltipstring (
 
+VOID _app_toolbar_setchecked (
+	_In_ HWND hwnd,
+	_In_ INT ctrl_id,
+	_In_ UINT command_id,
+	_In_ BOOLEAN is_checked
+)
+{
+	_r_wnd_sendmessage (hwnd, ctrl_id, TB_PRESSBUTTON, command_id, FALSE);
+	_r_wnd_sendmessage (hwnd, ctrl_id, TB_CHECKBUTTON, command_id, MAKELPARAM (is_checked, 0));
+}
+
 _Ret_maybenull_
--- a/src/messages.c
+++ b/src/messages.c
@@ -107,5 +107,5 @@ VOID _app_message_initialize (
 
-	_r_toolbar_pressbutton (config.hrebar, IDC_TOOLBAR, IDM_TRAY_ENABLENOTIFICATIONS_CHK, _r_config_getboolean (L"IsNotificationsEnabled", TRUE, NULL));
-	_r_toolbar_pressbutton (config.hrebar, IDC_TOOLBAR, IDM_TRAY_ENABLELOG_CHK, _r_config_getboolean (L"IsLogEnabled", FALSE, NULL));
-	_r_toolbar_pressbutton (config.hrebar, IDC_TOOLBAR, IDM_TRAY_ENABLEUILOG_CHK, _r_config_getboolean (L"IsLogUiEnabled", FALSE, NULL));
+	_app_toolbar_setchecked (config.hrebar, IDC_TOOLBAR, IDM_TRAY_ENABLENOTIFICATIONS_CHK, _r_config_getboolean (L"IsNotificationsEnabled", TRUE, NULL));
+	_app_toolbar_setchecked (config.hrebar, IDC_TOOLBAR, IDM_TRAY_ENABLELOG_CHK, _r_config_getboolean (L"IsLogEnabled", FALSE, NULL));
+	_app_toolbar_setchecked (config.hrebar, IDC_TOOLBAR, IDM_TRAY_ENABLEUILOG_CHK, _r_config_getboolean (L"IsLogUiEnabled", FALSE, NULL));
 }
```

- `_app_toolbar_setchecked` (src/controls.c): [fork.3](https://github.com/Mainkill1/simplewall/blob/2b0a5ca6891f6c0f57387e29cf22d4db3e1880db/src/controls.c#L305)
- `_app_message_initialize` (src/messages.c): [upstream](https://github.com/henrypp/simplewall/blob/8c8bbbae82d42c325632c2cfe868679abfa000fa/src/messages.c#L6) · [fork.3](https://github.com/Mainkill1/simplewall/blob/2b0a5ca6891f6c0f57387e29cf22d4db3e1880db/src/messages.c#L6)
- `SettingsProc` (src/main.c): [upstream](https://github.com/henrypp/simplewall/blob/8c8bbbae82d42c325632c2cfe868679abfa000fa/src/main.c#L634) · [fork.3](https://github.com/Mainkill1/simplewall/blob/2b0a5ca6891f6c0f57387e29cf22d4db3e1880db/src/main.c#L636)

[Fork commit, full context](https://github.com/Mainkill1/simplewall/commit/8a17a63b825d63c751ded48cc85fa26f21cd7f31) · [Pinned upstream baseline](https://github.com/henrypp/simplewall/tree/8c8bbbae82d42c325632c2cfe868679abfa000fa)

Exact local diff, from a clone with the fork commits:

```sh
git diff 24bbd2ce 8a17a63b -- src/controls.c src/messages.c src/main.c
```

## Smallest upstream scope and acceptance check

Port the helper and the startup/settings/tray/toolbar call sites together. Current upstream still calls `_r_toolbar_pressbutton`; use its current SDK, not the fork's compatibility wrappers. Verify enabled → checked → enabled and the three entry points stay in sync. Existing Windows evidence reports state 4 → 5 → 4; portable toggle test passed.

