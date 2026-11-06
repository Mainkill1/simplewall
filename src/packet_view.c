/* Packet controls are owned exclusively by this window's UI thread. */
#include "global.h"
#include "packet_history.h"
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
static HWND panel, main_window, app_list, rows, limit_window;
static PH_REQUEST request;
static PH_RESULT *shown;
static LONG seen_revision = -1, seen_dns_revision;
static BOOL applying, needs_query = TRUE, waiting;
static WCHAR selected_path[PH_TEXT], scratch[PH_TEXT + 512];
static HFONT view_font;
static HBRUSH background;
static int scale = 96;
static BOOL dark;
static int64_t limit_app;
static int px(int n)
{
	return MulDiv(n, scale, 96);
}
static void from_utf8(const char *s, WCHAR *b, int n)
{
	if (!s)
		s = "";
	if (!MultiByteToWideChar(CP_UTF8, 0, s, -1, b, n))
	{
		b[0] = 0;
		MultiByteToWideChar(CP_UTF8, 0, s, (int)strlen(s), b, n - 1);
		b[n - 1] = 0;
	}
}
static LPCWSTR base_name(LPCWSTR p)
{
	LPCWSTR n = wcsrchr(p, L'\\');
	return n ? n + 1 : p;
}
static void display_name(LPCWSTR path, WCHAR *out, int count)
{
	PITEM_APP app = _app_getappitem(_r_str_gethash2(path, TRUE));
	wcsncpy_s(out, count,
			  (app && app->display_name && !_r_obj_isstringempty(app->display_name))
				  ? app->display_name->buffer
				  : base_name(path),
			  _TRUNCATE);
	if (app)
		_r_obj_dereference(app);
}
static void date_text(int64_t t, WCHAR *b, int n)
{
	PR_STRING s = _r_format_unixtime(t, FDTF_SHORTDATE | FDTF_LONGTIME);
	wcsncpy_s(b, n, s ? s->buffer : L"", _TRUNCATE);
	if (s)
		_r_obj_dereference(s);
}
static HWND child(HWND parent, LPCWSTR cls, LPCWSTR text, DWORD style, int id)
{
	HWND w = CreateWindowExW((style & WS_BORDER) ? WS_EX_CLIENTEDGE : 0, cls, text,
							 WS_CHILD | WS_VISIBLE | WS_TABSTOP | style, 0, 0, 0, 0, parent,
							 (HMENU)(INT_PTR)id, _r_sys_getimagebase(), NULL);
	SendMessageW(w, WM_SETFONT, (WPARAM)view_font, TRUE);
	return w;
}
static void place(int id, int x, int y, int w, int h)
{
	MoveWindow(GetDlgItem(panel, id), px(x), px(y), max(1, px(w)), max(1, px(h)), TRUE);
}
static void layout(void);
static void place_window(HWND w, int x, int y, int width, int height)
{
	MoveWindow(w, px(x), px(y), px(width), px(height), TRUE);
}
static void rescale(void)
{
	int next = _r_dc_getwindowdpi(panel);
	if (next == scale)
		return;
	int oldscale = scale;
	scale = next;
	HFONT nextfont = CreateFontW(-px(13), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, 0, 0,
								 CLEARTYPE_QUALITY, 0, L"Segoe UI");
	for (int id = PH_APPLIST; id <= PH_LIMITPANEL; id++)
		if (GetDlgItem(panel, id))
			SendDlgItemMessageW(panel, id, WM_SETFONT, (WPARAM)nextfont, TRUE);
	for (int i = 0; i < 3; i++)
		ListView_SetColumnWidth(app_list, i, MulDiv(ListView_GetColumnWidth(app_list, i), scale, oldscale));
	for (int i = 0; i < 9; i++)
		ListView_SetColumnWidth(rows, i, MulDiv(ListView_GetColumnWidth(rows, i), scale, oldscale));
	DeleteObject(view_font);
	view_font = nextfont;
	layout();
}
static void column(HWND w, int i, LPCWSTR name, int width)
{
	LVCOLUMNW c = {0};
	c.mask = LVCF_TEXT | LVCF_WIDTH;
	c.pszText = (LPWSTR)name;
	c.cx = px(width);
	ListView_InsertColumn(w, i, &c);
}
static void layout(void)
{
	if (!panel || !app_list || !rows)
		return;
	RECT r;
	int w, h, left, right, rw;
	GetClientRect(panel, &r);
	w = MulDiv(r.right, 96, scale);
	h = MulDiv(r.bottom, 96, scale);
	left = min(300, max(190, w / 3));
	right = left + 12;
	rw = max(1, w - right - 12);
	place(PH_BLOCKED, 8, 8, (left - 24) / 2, 28);
	place(PH_ALLOWED, 16 + (left - 24) / 2, 8, (left - 24) / 2, 28);
	place(PH_FILTER, 8, 44, left - 16, 28);
	place(PH_APPLIST, 8, 80, left - 16, h - 122);
	place(PH_APPPREV, 8, h - 34, (left - 24) / 2, 26);
	place(PH_APPNEXT, 16 + (left - 24) / 2, h - 34, (left - 24) / 2, 26);
	place(PH_HEADER, right, 8, w - right - 12, 46);
	place(PH_CLEAR, right, 60, (rw - 16) * 42 / 100, 28);
	place(PH_ALLOW, right + 8 + (rw - 16) * 42 / 100, 60, (rw - 16) * 29 / 100, 28);
	place(PH_LIMIT, right + 16 + (rw - 16) * 71 / 100, 60, (rw - 16) * 29 / 100, 28);
	place(PH_STATS, right, 96, w - right - 12, 42);
	place(PH_DIRECTION, right, 144, min(140, rw / 2 - 4), 200);
	place(PH_GROUP, right + min(140, rw / 2 - 4) + 12, 144, min(132, rw / 2 - 8), 26);
	place(PH_ROWS, right, 178, w - right - 12, h - 254);
	place(PH_PREV, right, h - 68, (rw - 16) / 4, 26);
	place(PH_NEXT, right + 8 + (rw - 16) / 4, h - 68, (rw - 16) / 4, 26);
	place(PH_LIVE, right + 16 + (rw - 16) / 2, h - 68, (rw - 16) / 2, 26);
	place(PH_STATUS, right, h - 36, w - right - 12, 32);
}
static void theme(void)
{
	dark = _r_theme_isenabled();
	if (background)
		DeleteObject(background);
	background = CreateSolidBrush(dark ? RGB(30, 30, 30) : GetSysColor(COLOR_WINDOW));
	COLORREF bg = dark ? RGB(30, 30, 30) : GetSysColor(COLOR_WINDOW),
			 fg = dark ? RGB(232, 232, 232) : GetSysColor(COLOR_WINDOWTEXT);
	ListView_SetBkColor(app_list, bg);
	ListView_SetTextBkColor(app_list, bg);
	ListView_SetTextColor(app_list, fg);
	ListView_SetBkColor(rows, bg);
	ListView_SetTextBkColor(rows, bg);
	ListView_SetTextColor(rows, fg);
	_r_theme_initialize(panel);
	InvalidateRect(panel, NULL, TRUE);
}
static void query(BOOL reset)
{
	if (reset)
	{
		request.query.offset = 0;
		ListView_SetItemCountEx(rows, 0, 0);
	}
	request.apps_only = FALSE;
	request.query.grouped = (SendDlgItemMessageW(panel, PH_GROUP, BM_GETCHECK, 0, 0) == BST_CHECKED);
	request.query.direction = (int)SendDlgItemMessageW(panel, PH_DIRECTION, CB_GETCURSEL, 0, 0) - 1;
	GetDlgItemTextW(panel, PH_FILTER, request.filter, RTL_NUMBER_OF(request.filter));
	_app_packet_request(&request, TRUE);
	waiting = TRUE;
	needs_query = FALSE;
	seen_revision = _app_packet_revision();
}
VOID _app_packet_view_refresh(VOID)
{
	needs_query = TRUE;
}
VOID _app_packet_view_clear(VOID)
{
	if (panel)
		_app_packet_command(1, 0, 0, 0, 0);
	needs_query = TRUE;
}
static void status(void)
{
	WCHAR error[256], b[512];
	_app_packet_error(error, RTL_NUMBER_OF(error));
	if (error[0])
	{
		swprintf_s(b, RTL_NUMBER_OF(b), L"%s Lost events: %lld.", error,
				   (shown ? shown->lost : 0) + _app_packet_pending_loss());
		SetDlgItemTextW(panel, PH_STATUS, b);
		return;
	}
	if (shown && shown->error)
	{
		from_utf8(shown->message, b, RTL_NUMBER_OF(b));
		SetDlgItemTextW(panel, PH_STATUS, b[0] ? b : L"Packet history unavailable.");
		return;
	}
	if (request.query.allow && _r_config_getboolean(L"IsExcludeClassifyAllow", TRUE, NULL))
	{
		SetDlgItemTextW(panel, PH_STATUS,
						L"Allowed capture is off. Enable it in Settings > Exclude to record allowed events.");
		return;
	}
	swprintf_s(b, RTL_NUMBER_OF(b),
			   L"%s  Retained rows %lld–%lld%s. Lost: %lld; evicted: %lld. History: %lld MiB.%s",
			   waiting ? L"Loading…" : L"", request.query.offset + (shown && shown->page.count ? 1 : 0),
			   request.query.offset + (shown ? shown->page.count : 0), shown && shown->page.more ? L"+" : L"",
			   (shown ? shown->lost : 0) + _app_packet_pending_loss(), shown ? shown->evicted : 0,
			   shown ? shown->bytes / 1048576 : 0,
			   _r_config_getboolean(L"IsLogUiEnabled", FALSE, NULL) ? L"" : L" Packet capture is off.");
	SetDlgItemTextW(panel, PH_STATUS, b);
}
static void apply_result(PH_RESULT *r)
{
	int i, selection = -1, oldtop = ListView_GetTopIndex(app_list);
	int top = ListView_GetTopIndex(rows), selected = ListView_GetNextItem(rows, -1, LVNI_SELECTED);
	int64_t rowid =
		shown && selected >= 0 && selected < shown->page.count ? shown->page.rows[selected].id : 0;
	if (r->request.generation != request.generation)
	{
		free(r);
		return;
	}
	applying = TRUE;
	PH_RESULT *old = shown;
	if (r->request.apps_only && old)
		r->page = old->page;
	shown = r;
	waiting = FALSE;
	ListView_SetItemCountEx(app_list, r->apps.count + 1, LVSICF_NOSCROLL | LVSICF_NOINVALIDATEALL);
	if (!request.query.app)
		selection = 0;
	for (i = 0; i < r->apps.count; i++)
		if (r->apps.items[i].id == request.query.app)
		{
			selection = i + 1;
			from_utf8(r->apps.items[i].path, selected_path, RTL_NUMBER_OF(selected_path));
			break;
		}
	ListView_SetItemState(app_list, -1, 0, LVIS_SELECTED | LVIS_FOCUSED);
	if (selection >= 0)
		ListView_SetItemState(app_list, selection, LVIS_SELECTED | LVIS_FOCUSED,
							  LVIS_SELECTED | LVIS_FOCUSED);
	if (oldtop > 0 && oldtop <= r->apps.count)
		ListView_EnsureVisible(app_list, oldtop, FALSE);
	ListView_SetItemCountEx(rows, r->page.count, LVSICF_NOSCROLL | LVSICF_NOINVALIDATEALL);
	for (i = 0; i < r->page.count; i++)
		if (rowid && r->page.rows[i].id == rowid)
		{
			ListView_SetItemState(rows, i, LVIS_SELECTED, LVIS_SELECTED);
			break;
		}
	if (top > 0 && top < r->page.count)
		ListView_EnsureVisible(rows, top, FALSE);
	WCHAR title[PH_TEXT];
	display_name(selected_path, title, PH_TEXT);
	swprintf_s(scratch, RTL_NUMBER_OF(scratch), L"%s\r\n%s", request.query.app ? title : L"All apps",
			   request.query.app ? selected_path
								 : L"Newest matching activity first. Select an app to inspect its history.");
	SetDlgItemTextW(panel, PH_HEADER, scratch);
	WCHAR oldest[80], newest[80];
	date_text(r->page.oldest, oldest, 80);
	date_text(r->page.newest, newest, 80);
	swprintf_s(
		scratch, RTL_NUMBER_OF(scratch),
		L"%lld %s events   ·   %lld inbound   ·   %lld outbound   ·   %lld destinations\r\nRetained: %s — %s",
		r->page.total, request.query.allow ? L"allowed" : L"blocked", r->page.inbound, r->page.outbound,
		r->page.destinations, r->page.total ? oldest : L"—", r->page.total ? newest : L"—");
	SetDlgItemTextW(panel, PH_STATS, scratch);
	EnableWindow(GetDlgItem(panel, PH_CLEAR), request.query.app != 0);
	EnableWindow(GetDlgItem(panel, PH_ALLOW),
				 request.query.app != 0 && selected_path[0] && wcslen(selected_path) < PH_TEXT - 1);
	EnableWindow(GetDlgItem(panel, PH_PREV), request.query.offset > 0);
	EnableWindow(GetDlgItem(panel, PH_NEXT), r->page.more);
	EnableWindow(GetDlgItem(panel, PH_APPPREV), request.app_offset > 0);
	EnableWindow(GetDlgItem(panel, PH_APPNEXT), r->apps.more);
	ListView_RedrawItems(app_list, 0, r->apps.count);
	ListView_RedrawItems(rows, 0, r->page.count - 1);
	free(old);
	applying = FALSE;
	status();
}
static void app_text(int index, int sub, WCHAR *b, int n)
{
	if (!shown)
	{
		b[0] = 0;
		return;
	}
	if (index == 0)
	{
		if (sub == 0)
			wcscpy_s(b, n, L"All apps");
		else if (sub == 1)
			swprintf_s(b, n, L"%lld apps", shown->apps.total);
		else
			b[0] = 0;
		return;
	}
	index--;
	if (index >= shown->apps.count)
	{
		b[0] = 0;
		return;
	}
	PH_APP *a = &shown->apps.items[index];
	if (sub == 0)
	{
		WCHAR path[PH_TEXT];
		from_utf8(a->path, path, PH_TEXT);
		display_name(path, b, n);
	}
	else if (sub == 1)
		swprintf_s(b, n, L"%lld", a->count);
	else
	{
		PR_STRING time = _r_format_unixtime(a->last, FDTF_LONGTIME);
		wcsncpy_s(b, n, time ? time->buffer : L"", _TRUNCATE);
		if (time)
			_r_obj_dereference(time);
	}
}
static void row_text(int index, int sub, WCHAR *b, int n)
{
	if (!shown || index < 0 || index >= shown->page.count)
	{
		b[0] = 0;
		return;
	}
	PH_ROW *r = &shown->page.rows[index];
	switch (sub)
	{
	case 0:
		swprintf_s(b, n, L"%lld", request.query.grouped ? r->count : request.query.offset + index + 1);
		break;
	case 1:
		date_text(r->last, b, n);
		break;
	case 2: {
		WCHAR path[PH_TEXT];
		from_utf8(r->path, path, PH_TEXT);
		display_name(path, b, n);
		break;
	}
	case 3:
		from_utf8(r->remote, b, n);
		break;
	case 4:
		_app_packet_hostname(r->remote, r->af, b, n);
		if (!b[0])
			wcscpy_s(b, n, L"—");
		break;
	case 5:
		swprintf_s(b, n, L"%d", r->remote_port);
		break;
	case 6:
		swprintf_s(b, n, L"%s",
				   r->protocol == 6	   ? L"TCP"
				   : r->protocol == 17 ? L"UDP"
				   : r->protocol == 1  ? L"ICMP"
									   : L"Other");
		break;
	case 7:
		wcscpy_s(b, n, r->direction ? L"Outbound" : L"Inbound");
		break;
	case 8:
		from_utf8(r->rule, b, n);
		break;
	default:
		b[0] = 0;
	}
}
static void event_details(void)
{
	int i = ListView_GetNextItem(rows, -1, LVNI_SELECTED);
	if (!shown || i < 0 || i >= shown->page.count)
		return;
	PH_ROW *r = &shown->page.rows[i];
	WCHAR path[PH_TEXT], remote[64], local[64], rule[PH_RULE], user[256], layer[256], first[80], last[80],
		info[PH_TEXT + 2048];
	from_utf8(r->path, path, PH_TEXT);
	from_utf8(r->remote, remote, 64);
	from_utf8(r->local, local, 64);
	from_utf8(r->rule, rule, PH_RULE);
	from_utf8(r->user, user, 256);
	from_utf8(r->layer, layer, 256);
	date_text(r->first, first, 80);
	date_text(r->last, last, 80);
	swprintf_s(info, RTL_NUMBER_OF(info),
			   L"App: %s\nAction: %s\nEvents: %lld\nFirst seen: %s\nLast seen: %s\nRemote: %s : %d\nLocal: "
			   L"%s : %d%s\nProtocol: %d    Direction: %s\nRule: %s\nFilter: %lld\nUser: %s\nLayer: %s",
			   path, request.query.allow ? L"Allowed" : L"Blocked", r->count, first, last, remote,
			   r->remote_port, local, r->local_port,
			   request.query.grouped ? L" (local ports vary; disable grouping to inspect)" : L"", r->protocol,
			   r->direction ? L"Outbound" : L"Inbound", rule, r->filter, user, layer);
	MessageBoxW(panel, info, L"Packet history details", MB_OK | MB_ICONINFORMATION);
}
static void allow_selected(void)
{
	if (!request.query.app || !selected_path[0])
		return;
	PR_STRING path = _r_obj_createstring(selected_path);
	ULONG hash = _r_str_gethash(&path->sr, TRUE);
	PITEM_APP app = _app_getappitem(hash);
	if (app && app->original_path && _wcsicmp(app->original_path->buffer, selected_path) == 0)
	{
		int id = _app_listview_getbytype(app->type);
		int item = _app_listview_finditem(main_window, id, hash);
		if (item != INT_ERROR)
			_r_listview_setitemcheck(main_window, id, item, TRUE);
		else
			MessageBoxW(panel, L"This app is no longer in the current app list.", L"Allow app", MB_OK);
	}
	else
		MessageBoxW(panel,
					L"This history entry does not identify a known current app. Use the Apps tab to add or "
					L"configure it.",
					L"Allow app", MB_OK);
	if (app)
		_r_obj_dereference(app);
	_r_obj_dereference(path);
}
BOOLEAN _app_packet_view_command(INT id)
{
	if (!panel)
		return FALSE;
	switch (id)
	{
	case IDM_FIND:
		SetFocus(GetDlgItem(panel, PH_FILTER));
		return TRUE;
	case IDM_COPY:
	case IDM_COPY_VALUE: {
		HWND focus = GetFocus();
		if (focus == GetDlgItem(panel, PH_FILTER))
		{
			SendMessageW(focus, WM_COPY, 0, 0);
			return TRUE;
		}
		if (focus == app_list)
		{
			PR_STRING path = _r_obj_createstring(selected_path);
			_r_clipboard_set(panel, &path->sr);
			_r_obj_dereference(path);
			return TRUE;
		}
		R_STRINGBUILDER b;
		_r_obj_initializestringbuilder(&b, 1024);
		for (int i = ListView_GetNextItem(rows, -1, LVNI_SELECTED); i >= 0;
			 i = ListView_GetNextItem(rows, i, LVNI_SELECTED))
		{
			for (int j = 0; j < 9; j++)
			{
				WCHAR text[PH_TEXT];
				row_text(i, j, text, PH_TEXT);
				_r_obj_appendstringbuilder(&b, text);
				_r_obj_appendstringbuilder(&b, j == 8 ? L"\r\n" : L"\t");
			}
		}
		PR_STRING value = _r_obj_finalstringbuilder(&b);
		if (value)
			_r_clipboard_set(panel, &value->sr);
		_r_obj_deletestringbuilder(&b);
		return TRUE;
	}
	case IDM_SELECT_ALL:
		if (GetFocus() == GetDlgItem(panel, PH_FILTER))
			SendDlgItemMessageW(panel, PH_FILTER, EM_SETSEL, 0, -1);
		else
			ListView_SetItemState(rows, -1, LVIS_SELECTED, LVIS_SELECTED);
		return TRUE;
	case IDM_EXPLORE:
		if (request.query.app && selected_path[0])
		{
			PR_STRING path = _r_obj_createstring(selected_path);
			_r_shell_showfile(&path->sr);
			_r_obj_dereference(path);
		}
		return TRUE;
	case IDM_PROPERTIES:
		event_details();
		return TRUE;
	case IDM_DELETE:
		SendMessageW(panel, WM_COMMAND, PH_CLEAR, 0);
		return TRUE;
	case IDM_DISABLENOTIFICATIONS:
	case IDM_DISABLEREMOVAL:
	case IDM_DISABLETIMER:
		return TRUE;
	default:
		return FALSE;
	}
}
static LRESULT CALLBACK limits_proc(HWND w, UINT msg, WPARAM wp, LPARAM lp)
{
	switch (msg)
	{
	case WM_COMMAND:
		if (LOWORD(wp) == PH_SAVEGLOBAL || LOWORD(wp) == PH_SAVEAPP)
		{
			BOOL a, b, c;
			UINT count = GetDlgItemInt(w, PH_LIMITCOUNT, &a, FALSE),
				 days = GetDlgItemInt(w, PH_LIMITDAYS, &b, FALSE),
				 mib = GetDlgItemInt(w, PH_LIMITMIB, &c, FALSE);
			if (!a || !b || !c || count < 1 || count > 10000000 || days < 1 || days > 3650 || mib < 16 ||
				mib > 32768)
			{
				MessageBoxW(w, L"Use 1–10,000,000 events, 1–3,650 days and 16–32,768 MiB.",
							L"Retention limits", MB_OK);
				return 0;
			}
			_app_packet_command(2, LOWORD(wp) == PH_SAVEAPP ? limit_app : 0, count, days, mib);
			needs_query = TRUE;
			DestroyWindow(w);
			return 0;
		}
		break;
	case WM_CLOSE:
		DestroyWindow(w);
		return 0;
	case WM_DESTROY:
		limit_window = NULL;
		return 0;
	}
	return DefWindowProcW(w, msg, wp, lp);
}
static void limits(void)
{
	if (limit_window)
	{
		SetForegroundWindow(limit_window);
		return;
	}
	limit_app = request.query.app;
	WNDCLASSW c = {0};
	c.lpfnWndProc = limits_proc;
	c.hInstance = _r_sys_getimagebase();
	c.lpszClassName = L"SimplewallPacketLimits";
	c.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
	c.hCursor = LoadCursorW(NULL, IDC_ARROW);
	RegisterClassW(&c);
	limit_window = CreateWindowExW(WS_EX_DLGMODALFRAME, c.lpszClassName, L"Packet history retention",
								   WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU, CW_USEDEFAULT, CW_USEDEFAULT,
								   px(500), px(300), panel, NULL, c.hInstance, NULL);
	HWND x = child(limit_window, L"STATIC", L"Event limit per app", 0, 1);
	place_window(x, 16, 16, 220, 22);
	x = child(limit_window, L"EDIT", L"", WS_BORDER | ES_NUMBER, PH_LIMITCOUNT);
	place_window(x, 260, 16, 180, 24);
	SetDlgItemInt(limit_window, PH_LIMITCOUNT, (UINT)(shown ? shown->cap : 200000), FALSE);
	x = child(limit_window, L"STATIC", L"Maximum age (days, global)", 0, 2);
	place_window(x, 16, 54, 240, 22);
	x = child(limit_window, L"EDIT", L"", WS_BORDER | ES_NUMBER, PH_LIMITDAYS);
	place_window(x, 260, 54, 180, 24);
	SetDlgItemInt(limit_window, PH_LIMITDAYS, shown ? shown->days : 7, FALSE);
	x = child(limit_window, L"STATIC", L"Total history budget (MiB, global)", 0, 3);
	place_window(x, 16, 92, 240, 22);
	x = child(limit_window, L"EDIT", L"", WS_BORDER | ES_NUMBER, PH_LIMITMIB);
	place_window(x, 260, 92, 180, 24);
	SetDlgItemInt(limit_window, PH_LIMITMIB, shown ? shown->mib : 1024, FALSE);
	x = child(limit_window, L"STATIC",
			  L"Oldest events expire first. The total budget can evict events across apps. Counters cover "
			  L"retained history, not lifetime traffic.",
			  0, 4);
	place_window(x, 16, 130, 430, 48);
	x = child(limit_window, L"BUTTON", L"Save global defaults", BS_PUSHBUTTON, PH_SAVEGLOBAL);
	place_window(x, 16, 194, 200, 30);
	x = child(limit_window, L"BUTTON", L"Save this app's limit", BS_PUSHBUTTON, PH_SAVEAPP);
	place_window(x, 238, 194, 200, 30);
	EnableWindow(x, request.query.app != 0);
	ShowWindow(limit_window, SW_SHOW);
}
static LRESULT CALLBACK view_proc(HWND w, UINT msg, WPARAM wp, LPARAM lp)
{
	switch (msg)
	{
	case LVM_GETNEXTITEM:
	case LVM_GETSELECTIONMARK:
		return -1;
	case WM_SIZE:
		layout();
		return 0;
	case WM_SHOWWINDOW:
		if (wp)
			needs_query = TRUE;
		break;
	case WM_TIMER:
		if (wp == 1)
		{
			rescale();
			PH_RESULT *r = _app_packet_take();
			if (r)
				apply_result(r);
			if (!IsWindowVisible(w))
				return 0;
			if (seen_dns_revision != _app_packet_dns_revision())
			{
				seen_dns_revision = _app_packet_dns_revision();
				ListView_RedrawItems(rows, 0, shown ? shown->page.count - 1 : 0);
			}
			BOOL changed = seen_revision != _app_packet_revision();
			int selection = ListView_GetNextItem(rows, -1, LVNI_SELECTED);
			BOOL older = request.query.offset > 0 || ListView_GetTopIndex(rows) > 0 || selection >= 0;
			if (needs_query)
			{
				query(FALSE);
			}
			else if (changed && !waiting && !_app_packet_busy())
			{
				if (older)
				{
					SetDlgItemTextW(w, PH_LIVE, L"New activity — latest");
					request.apps_only = TRUE;
					_app_packet_request(&request, FALSE);
					waiting = TRUE;
					seen_revision = _app_packet_revision();
				}
				else
					query(FALSE);
			}
			status();
		}
		return 0;
	case WM_COMMAND: {
		int id = LOWORD(wp);
		if (id == PH_BLOCKED || id == PH_ALLOWED)
		{
			request.query.allow = (id == PH_ALLOWED);
			request.app_offset = 0;
			SendDlgItemMessageW(w, PH_BLOCKED, BM_SETCHECK, id == PH_BLOCKED, 0);
			SendDlgItemMessageW(w, PH_ALLOWED, BM_SETCHECK, id == PH_ALLOWED, 0);
			query(TRUE);
		}
		else if (id == PH_FILTER && HIWORD(wp) == EN_CHANGE)
		{
			request.app_offset = 0;
			needs_query = TRUE;
		}
		else if (id == PH_GROUP || (id == PH_DIRECTION && HIWORD(wp) == CBN_SELCHANGE))
		{
			query(TRUE);
		}
		else if (id == PH_PREV || id == PH_NEXT)
		{
			request.query.offset =
				max(0, request.query.offset + (id == PH_NEXT ? PH_PAGE_SIZE : -PH_PAGE_SIZE));
			query(FALSE);
		}
		else if (id == PH_APPPREV || id == PH_APPNEXT)
		{
			request.app_offset =
				max(0, request.app_offset + (id == PH_APPNEXT ? PH_PAGE_SIZE : -PH_PAGE_SIZE));
			query(FALSE);
		}
		else if (id == PH_LIVE)
		{
			SetDlgItemTextW(w, PH_LIVE, L"Latest / live");
			query(TRUE);
		}
		else if (id == PH_CLEAR)
		{
			if (request.query.app &&
				MessageBoxW(w, L"Clear all retained blocked and allowed events for this app?",
							L"Clear app history", MB_YESNO | MB_ICONQUESTION) == IDYES)
			{
				_app_packet_command(1, request.query.app, 0, 0, 0);
				needs_query = TRUE;
			}
		}
		else if (id == PH_ALLOW)
			allow_selected();
		else if (id == PH_LIMIT)
			limits();
		return 0;
	}
	case WM_NOTIFY: {
		NMHDR *n = (NMHDR *)lp;
		if (n->code == LVN_GETDISPINFOW)
		{
			NMLVDISPINFOW *d = (NMLVDISPINFOW *)lp;
			if (d->item.mask & LVIF_TEXT)
			{
				if (n->idFrom == PH_APPLIST)
					app_text(d->item.iItem, d->item.iSubItem, scratch, RTL_NUMBER_OF(scratch));
				else
					row_text(d->item.iItem, d->item.iSubItem, scratch, RTL_NUMBER_OF(scratch));
				wcsncpy_s(d->item.pszText, d->item.cchTextMax, scratch, _TRUNCATE);
			}
			if (d->item.mask & LVIF_IMAGE)
			{
				d->item.iImage = _app_icons_getdefaultapp_id(DATA_APP_REGULAR);
				if (n->idFrom == PH_APPLIST && shown && d->item.iItem > 0 &&
					d->item.iItem <= shown->apps.count)
				{
					WCHAR path[PH_TEXT];
					from_utf8(shown->apps.items[d->item.iItem - 1].path, path, PH_TEXT);
					PR_STRING name = _r_obj_createstring(path);
					PITEM_APP_INFO info = _app_getappinfobyhash2(_r_str_gethash(&name->sr, TRUE));
					if (info)
					{
						if (info->icon_id >= 0)
							d->item.iImage = info->icon_id;
						_r_obj_dereference(info);
					}
					_r_obj_dereference(name);
				}
			}
			return 0;
		}
		if (n->idFrom == PH_APPLIST && n->code == LVN_ITEMCHANGED && !applying)
		{
			NMLISTVIEW *d = (NMLISTVIEW *)lp;
			if ((d->uNewState & LVIS_SELECTED) && d->iItem >= 0 && shown)
			{
				int i = d->iItem - 1;
				request.query.app = i < 0 ? 0 : shown->apps.items[i].id;
				if (i >= 0)
					from_utf8(shown->apps.items[i].path, selected_path, RTL_NUMBER_OF(selected_path));
				else
					selected_path[0] = 0;
				query(TRUE);
			}
			return 0;
		}
		if (n->idFrom == PH_ROWS && (n->code == NM_DBLCLK || n->code == NM_RETURN))
		{
			event_details();
			return 0;
		}
		break;
	}
	case WM_THEMECHANGED:
		theme();
		return 0;
	case WM_ERASEBKGND: {
		RECT r;
		GetClientRect(w, &r);
		FillRect((HDC)wp, &r, background ? background : (HBRUSH)(COLOR_WINDOW + 1));
		return 1;
	}
	case WM_CTLCOLORSTATIC:
	case WM_CTLCOLOREDIT:
		SetTextColor((HDC)wp, dark ? RGB(232, 232, 232) : GetSysColor(COLOR_WINDOWTEXT));
		SetBkColor((HDC)wp, dark ? RGB(30, 30, 30) : GetSysColor(COLOR_WINDOW));
		return (LRESULT)background;
	case WM_DESTROY:
		KillTimer(w, 1);
		if (limit_window)
			DestroyWindow(limit_window);
		free(shown);
		shown = NULL;
		panel = NULL;
		if (view_font)
			DeleteObject(view_font);

		if (background)
			DeleteObject(background);
		return 0;
	}
	return DefWindowProcW(w, msg, wp, lp);
}
VOID _app_packet_view_init(HWND hwnd)
{
	WNDCLASSW c = {0};
	RECT r;
	HWND old = GetDlgItem(hwnd, IDC_LOG);
	GetWindowRect(old, &r);
	MapWindowPoints(NULL, hwnd, (POINT *)&r, 2);
	BOOL visible = IsWindowVisible(old);
	DestroyWindow(old);
	main_window = hwnd;
	scale = _r_dc_getwindowdpi(hwnd);
	view_font = CreateFontW(-px(13), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, 0, 0,
							CLEARTYPE_QUALITY, 0, L"Segoe UI");
	c.lpfnWndProc = view_proc;
	c.hInstance = _r_sys_getimagebase();
	c.lpszClassName = L"SimplewallPacketHistory";
	c.hCursor = LoadCursorW(NULL, IDC_ARROW);
	RegisterClassW(&c);
	panel = CreateWindowExW(
		WS_EX_CONTROLPARENT, c.lpszClassName, L"", WS_CHILD | WS_CLIPCHILDREN | (visible ? WS_VISIBLE : 0),
		r.left, r.top, r.right - r.left, r.bottom - r.top, hwnd, (HMENU)(INT_PTR)IDC_LOG, c.hInstance, NULL);
	child(panel, L"BUTTON", L"Blocked", BS_AUTORADIOBUTTON, PH_BLOCKED);
	child(panel, L"BUTTON", L"Allowed", BS_AUTORADIOBUTTON, PH_ALLOWED);
	SendDlgItemMessageW(panel, PH_BLOCKED, BM_SETCHECK, BST_CHECKED, 0);
	HWND filter = child(panel, L"EDIT", L"", WS_BORDER | ES_AUTOHSCROLL, PH_FILTER);
	SendMessageW(filter, EM_SETCUEBANNER, TRUE, (LPARAM)L"Filter apps by name or path");
	app_list = child(panel, WC_LISTVIEWW, L"",
					 WS_BORDER | LVS_REPORT | LVS_OWNERDATA | LVS_SINGLESEL | LVS_SHOWSELALWAYS |
						 LVS_SHAREIMAGELISTS,
					 PH_APPLIST);
	rows =
		child(panel, WC_LISTVIEWW, L"", WS_BORDER | LVS_REPORT | LVS_OWNERDATA | LVS_SHOWSELALWAYS, PH_ROWS);
	ListView_SetExtendedListViewStyle(app_list, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER | LVS_EX_LABELTIP);
	ListView_SetExtendedListViewStyle(rows, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER);
	HIMAGELIST system_images = NULL;
	_r_imagelist_getsystem(&system_images, SHIL_SMALL);
	if (system_images)
		ListView_SetImageList(app_list, system_images, LVSIL_SMALL);
	column(app_list, 0, L"App (latest first)", 130);
	column(app_list, 1, L"Events", 55);
	column(app_list, 2, L"Last seen", 90);
	LPCWSTR names[] = {
		L"Count / #", L"Last seen / Time", L"App", L"Remote address", L"Remote host (current DNS)", L"Port",
		L"Protocol",  L"Direction",		   L"Rule"};
	int widths[] = {78, 155, 130, 155, 210, 65, 65, 80, 260};
	for (int i = 0; i < 9; i++)
		column(rows, i, names[i], widths[i]);
	child(panel, L"BUTTON", L"Previous apps", BS_PUSHBUTTON, PH_APPPREV);
	child(panel, L"BUTTON", L"More apps", BS_PUSHBUTTON, PH_APPNEXT);
	child(panel, L"STATIC", L"Packet history", SS_LEFT, PH_HEADER);
	child(panel, L"STATIC", L"Loading retained history…", SS_LEFT, PH_STATS);
	child(panel, L"STATIC", L"", SS_LEFT, PH_STATUS);
	child(panel, L"BUTTON", L"Clear this app's log", BS_PUSHBUTTON, PH_CLEAR);
	child(panel, L"BUTTON", L"Allow app", BS_PUSHBUTTON, PH_ALLOW);
	child(panel, L"BUTTON", L"Retention…", BS_PUSHBUTTON, PH_LIMIT);
	HWND dir = child(panel, WC_COMBOBOXW, L"", CBS_DROPDOWNLIST | WS_VSCROLL, PH_DIRECTION);
	SendMessageW(dir, CB_ADDSTRING, 0, (LPARAM)L"All directions");
	SendMessageW(dir, CB_ADDSTRING, 0, (LPARAM)L"Inbound");
	SendMessageW(dir, CB_ADDSTRING, 0, (LPARAM)L"Outbound");
	SendMessageW(dir, CB_SETCURSEL, 0, 0);
	child(panel, L"BUTTON", L"Group repeats", BS_AUTOCHECKBOX, PH_GROUP);
	SendDlgItemMessageW(panel, PH_GROUP, BM_SETCHECK, BST_CHECKED, 0);
	child(panel, L"BUTTON", L"Previous", BS_PUSHBUTTON, PH_PREV);
	child(panel, L"BUTTON", L"Next", BS_PUSHBUTTON, PH_NEXT);
	child(panel, L"BUTTON", L"Latest / live", BS_PUSHBUTTON, PH_LIVE);
	request.query.direction = -1;
	request.query.grouped = 1;
	theme();
	layout();
	SetTimer(panel, 1, 250, NULL);
	query(FALSE);
}
