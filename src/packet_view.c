/* Packet controls are owned exclusively by this window's UI thread. */
#include "global.h"
#include "packet_history.h"
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
static HWND panel, main_window, app_list, rows, limit_window;
static PH_REQUEST request;
static PH_RESULT *shown;
static LONG seen_revision = -1, seen_dns_revision, queried_dns_revision;
static LONG seen_audit_revision;
static BOOL applying, needs_query = TRUE, waiting;
static BOOL reset_app_scroll, reset_row_scroll;
static BOOL show_service;
static BOOL inspecting_process;
static BOOLEAN selected_path_complete;
static WCHAR selected_path[PH_TEXT], scratch[PH_TEXT + 512];
static HFONT view_font;
static HBRUSH background;
static int scale = 96;
static BOOL dark;
static int64_t limit_app;
static const WCHAR audit_label[] = L"Audit Filtering Platform Connection";
static const struct
{
	LPCWSTR label;
	ULONG minutes;
} audit_options[] = {
	{L"Off", 0}, {L"15m", 15}, {L"30m", 30}, {L"1h", 60},
	{L"12h", 720}, {L"24h", PH_AUDIT_MAX_MINUTES}, {L"Always On", PH_AUDIT_ALWAYS}};
typedef struct
{
	LPCWSTR label;
	PH_SORT_COLUMN sort;
	int width;
} PH_COLUMN;
static const PH_COLUMN app_columns[] = {
	{L"App", PH_SORT_APP, 130}, {L"Events", PH_SORT_COUNT, 55}, {L"Last seen", PH_SORT_LAST, 90}};
static const PH_COLUMN row_columns[] = {
	{L"Count / #", PH_SORT_COUNT, 78}, {L"Last seen / Time", PH_SORT_LAST, 155}, {L"App", PH_SORT_APP, 130},
	{L"ServiceID", PH_SORT_SERVICE, 105},
	{L"Remote address", PH_SORT_REMOTE, 155}, {L"Remote host (current DNS)", PH_SORT_HOST, 210},
	{L"Port", PH_SORT_PORT, 65}, {L"Protocol", PH_SORT_PROTOCOL, 65}, {L"Direction", PH_SORT_DIRECTION, 80},
	{L"Rule", PH_SORT_RULE, 260}};
#define SERVICE_COLUMN 3

static int row_column_count(void)
{
	return RTL_NUMBER_OF(row_columns) - !show_service;
}

static const PH_COLUMN *visible_row_column(int index)
{
	return &row_columns[index + (!show_service && index >= SERVICE_COLUMN)];
}
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
							 WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_CLIPSIBLINGS | (style & ~WS_BORDER),
							 0, 0, 0, 0, parent,
							 (HMENU)(INT_PTR)id, _r_sys_getimagebase(), NULL);
	SendMessageW(w, WM_SETFONT, (WPARAM)view_font, TRUE);
	return w;
}
static void place(int id, int x, int y, int w, int h)
{
	MoveWindow(GetDlgItem(panel, id), px(x), px(y), max(1, px(w)), max(1, px(h)), FALSE);
}
static void redraw_panel(HWND w)
{
	RedrawWindow(w, NULL, NULL, RDW_INVALIDATE | RDW_ERASE | RDW_FRAME | RDW_ALLCHILDREN);
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
	for (int id = PH_APPLIST; id <= PH_AUDIT_TIME; id++)
		if (GetDlgItem(panel, id))
			SendDlgItemMessageW(panel, id, WM_SETFONT, (WPARAM)nextfont, TRUE);
	for (int i = 0; i < RTL_NUMBER_OF(app_columns); i++)
		ListView_SetColumnWidth(app_list, i, MulDiv(ListView_GetColumnWidth(app_list, i), scale, oldscale));
	for (int i = 0; i < row_column_count(); i++)
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

static void update_sort_arrow(HWND list, PH_ORDER order)
{
	BOOL is_app = list == app_list;
	int count = is_app ? RTL_NUMBER_OF(app_columns) : row_column_count();
	for (int i = 0; i < count; i++)
	{
		const PH_COLUMN *item = is_app ? &app_columns[i] : visible_row_column(i);
		_r_listview_setcolumnsortindex(list, 0, i, item->sort == order.column ? (order.ascending ? 1 : -1) : 0);
	}
}

static void update_service_column(void)
{
	BOOL visible = request.query.app && _app_packet_issvchost(selected_path);
	if (show_service == visible)
		return;

	show_service = visible;
	if (visible)
		column(rows, SERVICE_COLUMN, row_columns[SERVICE_COLUMN].label, row_columns[SERVICE_COLUMN].width);
	else
	{
		ListView_DeleteColumn(rows, SERVICE_COLUMN);
		if (request.query.order.column == PH_SORT_SERVICE)
			request.query.order = (PH_ORDER){0};
	}
	update_sort_arrow(rows, request.query.order);
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
	int audit_offset = rw < 464 ? 30 : 0;
	int audit_x = right + (audit_offset ? 0 : 230);
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
	place(PH_AUDIT_HELP, right, 178, 222, 24);
	place(PH_AUDIT, audit_x, 174 + audit_offset, 132, 180);
	place(PH_AUDIT_TIME, audit_x + 140, 178 + audit_offset, 92, 24);
	place(PH_ROWS, right, 206 + audit_offset, rw, h - 282 - audit_offset);
	place(PH_PREV, right, h - 68, (rw - 16) / 4, 26);
	place(PH_NEXT, right + 8 + (rw - 16) / 4, h - 68, (rw - 16) / 4, 26);
	place(PH_LIVE, right + 16 + (rw - 16) / 2, h - 68, (rw - 16) / 2, 26);
	place(PH_STATUS, right, h - 36, w - right - 12, 32);
	redraw_panel(panel);
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
	if (limit_window)
	{
		_r_theme_initialize(limit_window);
		redraw_panel(limit_window);
	}
	redraw_panel(panel);
}

static void update_audit_controls(BOOL refresh_policy)
{
	static ULONG shown_minutes = ULONG_MAX, shown_seconds = ULONG_MAX;
	ULONG minutes, seconds = _app_packet_update_audit_timer(&minutes);
	if (!refresh_policy && (!IsWindowVisible(panel) || (minutes == shown_minutes && seconds == shown_seconds)))
		return;

	HWND combo = GetDlgItem(panel, PH_AUDIT);
	if (!refresh_policy && SendMessageW(combo, CB_GETDROPPEDSTATE, 0, 0))
		return;

	if (refresh_policy)
	{
		BOOLEAN enabled;
		EnableWindow(combo, _app_packet_query_blocked_audit(&enabled) == ERROR_SUCCESS);
	}

	int selection = 0;
	ULONG option = !minutes && _app_packet_audit_enabled() ? PH_AUDIT_ALWAYS : minutes;
	for (int i = 1; i < RTL_NUMBER_OF(audit_options); i++)
		if (audit_options[i].minutes == option)
			selection = i;
	if (selection != SendMessageW(combo, CB_GETCURSEL, 0, 0))
		SendMessageW(combo, CB_SETCURSEL, selection, 0);

	if (minutes != shown_minutes || seconds != shown_seconds)
	{
		WCHAR countdown[24] = L"";
		if (seconds >= 3600)
			swprintf_s(countdown, RTL_NUMBER_OF(countdown), L"%lu:%02lu:%02lu left", seconds / 3600, seconds / 60 % 60, seconds % 60);
		else if (seconds)
			swprintf_s(countdown, RTL_NUMBER_OF(countdown), L"%lu:%02lu left", seconds / 60, seconds % 60);
		else if (minutes)
			wcscpy_s(countdown, RTL_NUMBER_OF(countdown), L"Turning off…");
		SetDlgItemTextW(panel, PH_AUDIT_TIME, countdown);
		shown_minutes = minutes;
		shown_seconds = seconds;
	}
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
	queried_dns_revision = _app_packet_dns_revision();
	_app_packet_request(&request, TRUE);
	waiting = TRUE;
	needs_query = FALSE;
	seen_revision = _app_packet_revision();
	seen_audit_revision = _app_packet_audit_revision();
}

static void sort_column(HWND list, int column_index)
{
	BOOL is_app = list == app_list;
	int count = is_app ? RTL_NUMBER_OF(app_columns) : row_column_count();
	PH_ORDER *order = is_app ? &request.app_order : &request.query.order;
	if (column_index < 0 || column_index >= count)
		return;
	PH_SORT_COLUMN selected = (is_app ? &app_columns[column_index] : visible_row_column(column_index))->sort;
	if (order->column == selected)
		order->ascending = !order->ascending;
	else
	{
		order->column = selected;
		order->ascending = selected != PH_SORT_LAST &&
			(selected != PH_SORT_COUNT || (!is_app && !request.query.grouped));
	}
	if (is_app)
	{
		request.app_offset = 0;
		reset_app_scroll = TRUE;
	}
	else
	{
		request.query.offset = 0;
		reset_row_scroll = TRUE;
		SetDlgItemTextW(panel, PH_LIVE, L"Latest / live");
	}
	update_sort_arrow(list, *order);
	// A completed packet page can stay on screen while the app list reorders.
	if (is_app && !needs_query && (!waiting || request.apps_only) && shown && !shown->error)
	{
		request.apps_only = TRUE;
		_app_packet_request(&request, TRUE);
		waiting = TRUE;
		return;
	}
	query(FALSE);
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
static void set_status_text(LPCWSTR text)
{
	WCHAR current[512] = {0};
	GetDlgItemTextW(panel, PH_STATUS, current, RTL_NUMBER_OF(current));
	if (wcscmp(current, text) != 0)
		SetDlgItemTextW(panel, PH_STATUS, text);
}
static void status(void)
{
	WCHAR error[256], b[512];
	_app_packet_error(error, RTL_NUMBER_OF(error));
	if (error[0])
	{
		swprintf_s(b, RTL_NUMBER_OF(b), L"%s Lost events: %lld.", error,
				   (shown ? shown->lost : 0) + _app_packet_pending_loss());
		set_status_text(b);
		return;
	}
	if (shown && shown->error)
	{
		from_utf8(shown->message, b, RTL_NUMBER_OF(b));
		set_status_text(b[0] ? b : L"Packet history unavailable.");
		return;
	}
	if (request.query.allow && _r_config_getboolean(L"IsExcludeClassifyAllow", TRUE, NULL) &&
		!(request.query.app ? _app_packet_iswatched(selected_path) : _app_packet_haswatches()))
	{
		set_status_text(L"Allowed capture is off. Enable monitoring from an app's context menu or Settings > Exclude.");
		return;
	}
	if (!shown)
		return;
	int64_t offset = shown->request.query.offset;
	swprintf_s(b, RTL_NUMBER_OF(b),
			   L"Retained rows %lld–%lld%s. Lost: %lld; evicted: %lld. History: %lld MiB.%s",
			   offset + (shown->page.count ? 1 : 0), offset + shown->page.count, shown->page.more ? L"+" : L"",
			   shown->lost + _app_packet_pending_loss(), shown->evicted, shown->bytes / 1048576,
			   _r_config_getboolean(L"IsLogUiEnabled", FALSE, NULL) ? L"" : L" Packet capture is off.");
	set_status_text(b);
}
static void set_selected_path(const char *path)
{
	// The fixed-size history result can truncate a long UTF-8 path. A prefix
	// of that path must never become a new app identity.
	selected_path_complete = strlen(path) < PH_TEXT - 1;
	from_utf8(path, selected_path, RTL_NUMBER_OF(selected_path));
}

static void update_app_action(void)
{
	const BOOLEAN has_app = request.query.app != 0 && selected_path[0] && selected_path_complete;
	PR_STRING share_path = has_app ? _app_getnetworkapppath(selected_path) : NULL;

	SetDlgItemTextW(panel, PH_ALLOW, share_path ? L"Manage app" : L"Allow app");
	EnableWindow(GetDlgItem(panel, PH_ALLOW), has_app);
	if (share_path)
		_r_obj_dereference(share_path);
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
			set_selected_path(r->apps.items[i].path);
			break;
		}
	ListView_SetItemState(app_list, -1, 0, LVIS_SELECTED | LVIS_FOCUSED);
	if (selection >= 0)
		ListView_SetItemState(app_list, selection, LVIS_SELECTED | LVIS_FOCUSED,
							  LVIS_SELECTED | LVIS_FOCUSED);
	if (reset_app_scroll)
		ListView_EnsureVisible(app_list, 0, FALSE);
	else if (oldtop > 0 && oldtop <= r->apps.count)
		ListView_EnsureVisible(app_list, oldtop, FALSE);
	reset_app_scroll = FALSE;
	ListView_SetItemCountEx(rows, r->page.count, LVSICF_NOSCROLL | LVSICF_NOINVALIDATEALL);
	if (reset_row_scroll)
		ListView_SetItemState(rows, -1, 0, LVIS_SELECTED | LVIS_FOCUSED);
	for (i = 0; i < r->page.count; i++)
		if (rowid && r->page.rows[i].id == rowid)
		{
			ListView_SetItemState(rows, i, LVIS_SELECTED, LVIS_SELECTED);
			break;
		}
	if (reset_row_scroll && r->page.count)
		ListView_EnsureVisible(rows, 0, FALSE);
	else if (top > 0 && top < r->page.count)
		ListView_EnsureVisible(rows, top, FALSE);
	reset_row_scroll = FALSE;
	WCHAR title[PH_TEXT];
	_app_packet_displayname(selected_path, title, PH_TEXT);
	swprintf_s(scratch, RTL_NUMBER_OF(scratch), L"%s\r\n%s", request.query.app ? title : L"All apps",
			   request.query.app ? selected_path
								 : L"Select an app to inspect its history.");
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
	update_app_action();
	EnableWindow(GetDlgItem(panel, PH_PREV), request.query.offset > 0);
	EnableWindow(GetDlgItem(panel, PH_NEXT), r->page.more);
	EnableWindow(GetDlgItem(panel, PH_APPPREV), request.app_offset > 0);
	EnableWindow(GetDlgItem(panel, PH_APPNEXT), r->apps.more);
	InvalidateRect(app_list, NULL, TRUE);
	InvalidateRect(rows, NULL, TRUE);
	free(old);
	applying = FALSE;
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
		_app_packet_displayname(path, b, n);
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
	if (!shown || index < 0 || index >= shown->page.count || sub < 0 || sub >= row_column_count())
	{
		b[0] = 0;
		return;
	}
	PH_ROW *r = &shown->page.rows[index];
	switch (visible_row_column(sub)->sort)
	{
	case PH_SORT_COUNT:
		swprintf_s(b, n, L"%lld", shown->request.query.grouped ? r->count : r->id);
		break;
	case PH_SORT_LAST:
		date_text(r->last, b, n);
		break;
	case PH_SORT_APP: {
		WCHAR path[PH_TEXT];
		from_utf8(r->path, path, PH_TEXT);
		_app_packet_displayname(path, b, n);
		break;
	}
	case PH_SORT_SERVICE:
		from_utf8(r->service[0] ? r->service : NULL, b, n);
		if (!b[0])
			wcscpy_s(b, n, L"—");
		break;
	case PH_SORT_REMOTE:
		from_utf8(r->remote, b, n);
		break;
	case PH_SORT_HOST:
		_app_packet_hostname(r->remote, r->af, b, n);
		if (!b[0])
			wcscpy_s(b, n, L"—");
		break;
	case PH_SORT_PORT:
		swprintf_s(b, n, L"%d", r->remote_port);
		break;
	case PH_SORT_PROTOCOL:
		swprintf_s(b, n, L"%s",
				   r->protocol == 6	   ? L"TCP"
				   : r->protocol == 17 ? L"UDP"
				   : r->protocol == 1  ? L"ICMP"
									   : L"Other");
		break;
	case PH_SORT_DIRECTION:
		wcscpy_s(b, n, r->direction ? L"Outbound" : L"Inbound");
		break;
	case PH_SORT_RULE:
		from_utf8(r->rule, b, n);
		break;
	default:
		b[0] = 0;
	}
}
typedef struct
{
	HWND window;
	PH_ROW row;
	WCHAR path[PH_TEXT];
	PR_STRING details;
} PACKET_PROCESS_REQUEST;

static VOID NTAPI inspect_process_worker(PVOID context)
{
	PACKET_PROCESS_REQUEST *request = context;
	BOOL is_details = request->details != NULL;
	PR_STRING info = request->details;
	if (is_details)
	{
		PR_STRING job = _app_packet_bits_job_info(&request->row);
		_r_obj_movereference((PVOID_PTR)&info, _r_obj_concatstrings(2, info->buffer,
			job ? job->buffer : L"\n\nBITS job: No unique match in the Windows transfer log."));
		if (job)
			_r_obj_dereference(job);
	}
	else
		info = _app_packet_process_info(&request->row, request->path);

	if (!PostMessageW(request->window, PH_PROCESS_READY, is_details, (LPARAM)info) && info)
		_r_obj_dereference(info);
	_r_mem_free(request);
}

static void inspect_packet(const PH_ROW *row, LPCWSTR path, LPCWSTR details)
{
	if (inspecting_process)
		return;
	PACKET_PROCESS_REQUEST *inspection = _r_mem_allocate(sizeof(*inspection));
	inspection->window = panel;
	inspection->row = *row;
	wcscpy_s(inspection->path, RTL_NUMBER_OF(inspection->path), path);
	inspection->details = details ? _r_obj_createstring(details) : NULL;
	inspecting_process = TRUE;
	_r_workqueue_queueitem(&file_queue, &inspect_process_worker, inspection);
}

static void event_details(void)
{
	int i = ListView_GetNextItem(rows, -1, LVNI_SELECTED);
	if (!shown || i < 0 || i >= shown->page.count)
		return;
	PH_ROW *r = &shown->page.rows[i];
	WCHAR path[PH_TEXT], remote[64], local[64], rule[PH_RULE], user[256], layer[256], first[80], last[80],
		info[PH_TEXT + 2048], process[160] = L"";
	from_utf8(r->path, path, PH_TEXT);
	from_utf8(r->remote, remote, 64);
	from_utf8(r->local, local, 64);
	from_utf8(r->rule, rule, PH_RULE);
	from_utf8(r->user, user, 256);
	from_utf8(r->layer, layer, 256);
	date_text(r->first, first, 80);
	date_text(r->last, last, 80);
	if (r->pid)
	{
		WCHAR started[80] = L"";
		if (r->process_started)
			date_text(r->process_started, started, RTL_NUMBER_OF(started));
		swprintf_s(process, RTL_NUMBER_OF(process), L"\nPID: %lu%s%s", (ULONG)r->pid,
			started[0] ? L"\nProcess started: " : L"", started);
	}
	swprintf_s(info, RTL_NUMBER_OF(info),
			   L"App: %s\nAction: %s\nEvents: %lld\nFirst seen: %s\nLast seen: %s\nRemote: %s : %d\nLocal: "
			   L"%s : %d%s\nProtocol: %d    Direction: %s\nRule: %s\nFilter: %lld\nUser: %s\nLayer: %s%s",
			   path, shown->request.query.allow ? L"Allowed" : L"Blocked", r->count, first, last, remote,
			   r->remote_port, local, r->local_port,
			   shown->request.query.grouped ? L" (one recorded event from this group)" : L"", r->protocol,
			   r->direction ? L"Outbound" : L"Inbound", rule, r->filter, user, layer, process);
	if (_stricmp(r->service, "BITS") == 0)
		inspect_packet(r, path, info);
	else
		MessageBoxW(panel, info, L"Packet history details", MB_OK | MB_ICONINFORMATION);
}
static void app_action(void)
{
	if (!request.query.app || !selected_path_complete || !IsWindowEnabled(GetDlgItem(panel, PH_ALLOW)))
		return;

	PR_STRING share_path = _app_getnetworkapppath(selected_path);
	if (share_path)
	{
		PITEM_APP share_app = _app_addapplication(main_window, DATA_APP_NETWORK, share_path, NULL, NULL);
		if (share_app)
		{
			if (_wcsicmp(share_app->original_path->buffer, share_path->buffer) == 0)
			{
				_app_profile_requestsave(main_window);
				_app_listview_showitemby_param(main_window, share_app->app_hash, TRUE);
			}
			_r_obj_dereference(share_app);
		}
		_r_obj_dereference(share_path);
		return;
	}

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

enum
{
	PH_MENU_MONITOR = 1,
	PH_MENU_FOLDER,
	PH_MENU_RULES,
	PH_MENU_SUSPEND,
	PH_MENU_RESUME,
	PH_MENU_TERMINATE,
	PH_MENU_PROPERTIES
};

static void app_process_action(PR_STRING executable, UINT command)
{
	LPCWSTR verb = command == PH_MENU_TERMINATE ? L"Terminate" : command == PH_MENU_SUSPEND ? L"Suspend" : L"Resume";
	WCHAR prompt[PH_TEXT + 96];
	swprintf_s(prompt, RTL_NUMBER_OF(prompt), L"%s all running instances of this executable?\n\n%s", verb, executable->buffer);
	if (MessageBoxW(panel, prompt, L"Manage running app", MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2) != IDYES)
		return;

	PSYSTEM_PROCESS_INFORMATION processes;
	NTSTATUS status = _r_sys_enumprocesses(&processes);
	if (!NT_SUCCESS(status))
	{
		_r_show_errormessage(panel, L"Could not enumerate running processes.", NULL, status, ET_NATIVE);
		return;
	}
	LPCWSTR name = base_name(executable->buffer);
	int name_length = (int)wcslen(name);
	DWORD access = PROCESS_QUERY_LIMITED_INFORMATION |
		(command == PH_MENU_TERMINATE ? PROCESS_TERMINATE : PROCESS_SUSPEND_RESUME);
	ULONG matched = 0, completed = 0;
	WCHAR image_path[32768];
	for (PSYSTEM_PROCESS_INFORMATION process = PR_FIRST_PROCESS(processes); process; process = PR_NEXT_PROCESS(process))
	{
		DWORD pid = HandleToULong(process->UniqueProcessId);
		if (pid <= 4 || pid == GetCurrentProcessId() || !process->ImageName.Buffer ||
			CompareStringOrdinal(name, name_length, process->ImageName.Buffer,
				process->ImageName.Length / sizeof(WCHAR), TRUE) != CSTR_EQUAL)
			continue;
		HANDLE handle = OpenProcess(access, FALSE, pid);
		if (!handle)
			continue;
		DWORD length = RTL_NUMBER_OF(image_path);
		if (QueryFullProcessImageNameW(handle, 0, image_path, &length))
		{
			PR_STRING share = executable->buffer[0] == L'\\' ? _app_getnetworkapppath(image_path) : NULL;
			if (_wcsicmp(executable->buffer, share ? share->buffer : image_path) == 0)
			{
				matched++;
				ULONG critical;
				if (NT_SUCCESS(NtQueryInformationProcess(handle, ProcessBreakOnTermination, &critical, sizeof(critical), NULL)) && !critical)
				{
					if (command == PH_MENU_TERMINATE)
						status = NtTerminateProcess(handle, STATUS_SUCCESS);
					else if (command == PH_MENU_SUSPEND)
						status = NtSuspendProcess(handle);
					else
						status = NtResumeProcess(handle);
					if (NT_SUCCESS(status))
						completed++;
				}
			}
			if (share)
				_r_obj_dereference(share);
		}
		CloseHandle(handle);
	}
	_r_mem_free(processes);
	if (!matched)
		MessageBoxW(panel, L"No accessible running instances matched this executable.", L"Manage running app", MB_OK | MB_ICONINFORMATION);
	else if (completed != matched)
		MessageBoxW(panel, L"Some matching processes could not be changed.", L"Manage running app", MB_OK | MB_ICONWARNING);
}

static void app_rules(PITEM_APP app)
{
	if (!app)
	{
		MessageBoxW(panel, L"No current Simplewall rules are associated with this history entry.", L"App rules", MB_OK | MB_ICONINFORMATION);
		return;
	}
	PEDITOR_CONTEXT editor = _app_editor_createwindow(main_window, app, 1, FALSE);
	if (editor)
	{
		_app_editor_deletewindow(editor);
		_app_listview_updateitemby_param(main_window, app->app_hash, TRUE);
		_app_profile_requestsave(main_window);
	}
}

static void app_context_menu(int index, POINT screen)
{
	if (!shown || index <= 0 || index > shown->apps.count)
		return;
	const char *stored_path = shown->apps.items[index - 1].path;
	if (strlen(stored_path) >= PH_TEXT - 1)
		return;

	HMENU menu = CreatePopupMenu();
	if (!menu)
		return;
	WCHAR path[PH_TEXT];
	from_utf8(stored_path, path, RTL_NUMBER_OF(path));
	PR_STRING identity = _r_obj_createstring(path);
	PITEM_APP app = _app_getappitem(_r_str_gethash(&identity->sr, TRUE));
	if (app && (!app->original_path || _wcsicmp(app->original_path->buffer, path) != 0))
	{
		_r_obj_dereference(app);
		app = NULL;
	}

	PR_STRING file = _app_isappvalidpath(identity) ? identity : app ? app->real_path : NULL;
	if (file)
		_r_obj_reference(file);
	BOOL can_open = _app_isappvalidpath(file);
	BOOL can_control = _app_isappvalidbinary(identity) && (!app || (app->type != DATA_APP_SERVICE && app->type != DATA_APP_UWP)) &&
		(!config.my_path || _wcsicmp(config.my_path->buffer, path) != 0);
	BOOL can_monitor = (can_open || app) && _r_sys_isosversiongreaterorequal(WINDOWS_8);
	BOOL watched = can_monitor && _app_packet_iswatched(path);
	AppendMenuW(menu, MF_STRING | (can_monitor ? MF_ENABLED : MF_GRAYED) | (watched ? MF_CHECKED : MF_UNCHECKED),
		PH_MENU_MONITOR, L"Monitor allowed packets");
	AppendMenuW(menu, MF_STRING | (can_open ? MF_ENABLED : MF_GRAYED), PH_MENU_FOLDER, L"Open file location");
	AppendMenuW(menu, MF_STRING, PH_MENU_RULES, L"View app rules");
	AppendMenuW(menu, MF_SEPARATOR, 0, NULL);
	UINT process_flags = MF_STRING | (can_control ? MF_ENABLED : MF_GRAYED);
	AppendMenuW(menu, process_flags, PH_MENU_SUSPEND, L"Suspend running instances");
	AppendMenuW(menu, process_flags, PH_MENU_RESUME, L"Resume running instances");
	AppendMenuW(menu, process_flags, PH_MENU_TERMINATE, L"Terminate running instances");
	AppendMenuW(menu, MF_STRING | (_app_isappvalidbinary(file) ? MF_ENABLED : MF_GRAYED), PH_MENU_PROPERTIES, L"Executable properties");

	UINT command = TrackPopupMenuEx(menu, TPM_RETURNCMD | TPM_NONOTIFY | TPM_RIGHTBUTTON, screen.x, screen.y, panel, NULL);
	DestroyMenu(menu);
	switch (command)
	{
	case PH_MENU_MONITOR:
		if (_app_packet_setwatch(path, !watched))
		{
			_wfp_logsetoption(_wfp_getenginehandle());
			status();
		}
		else
			MessageBoxW(panel, L"Could not save monitoring for this app.", L"Monitor allowed packets", MB_OK | MB_ICONWARNING);
		break;
	case PH_MENU_FOLDER:
		if (FAILED(_r_shell_showfile(&file->sr)))
			MessageBoxW(panel, L"Could not open the file location.", L"Open file location", MB_OK | MB_ICONWARNING);
		break;
	case PH_MENU_RULES:
		app_rules(app);
		break;
	case PH_MENU_SUSPEND:
	case PH_MENU_RESUME:
	case PH_MENU_TERMINATE:
		app_process_action(identity, command);
		break;
	case PH_MENU_PROPERTIES:
		if (!SHObjectProperties(panel, SHOP_FILEPATH, file->buffer, NULL))
			MessageBoxW(panel, L"Could not open the executable properties.", L"Executable properties", MB_OK | MB_ICONWARNING);
		break;
	}
	if (file)
		_r_obj_dereference(file);
	if (app)
		_r_obj_dereference(app);
	_r_obj_dereference(identity);
}

static void list_context_menu(HWND list, POINT screen)
{
	LVHITTESTINFO hit = {0};
	if (screen.x == -1 && screen.y == -1)
	{
		hit.iItem = ListView_GetNextItem(list, -1, LVNI_SELECTED);
		RECT bounds;
		if (hit.iItem < 0 || !ListView_GetItemRect(list, hit.iItem, &bounds, LVIR_LABEL))
			return;
		screen = (POINT){bounds.left, bounds.bottom};
		ClientToScreen(list, &screen);
	}
	else
	{
		hit.pt = screen;
		ScreenToClient(list, &hit.pt);
		hit.iItem = ListView_SubItemHitTest(list, &hit);
	}
	if (list == app_list)
	{
		app_context_menu(hit.iItem, screen);
		return;
	}
	if (!shown || hit.iItem < 0 || hit.iItem >= shown->page.count || hit.iSubItem < 0 || hit.iSubItem >= row_column_count())
		return;

	const PH_ROW row = shown->page.rows[hit.iItem];
	WCHAR value[PH_TEXT + 512], label[96], path[PH_TEXT];
	from_utf8(row.path, path, RTL_NUMBER_OF(path));
	BOOL can_inspect = !shown->request.query.allow && _app_packet_issvchost(path) &&
		row.sample_time > 0 && row.filter && row.local_port && row.remote_port &&
		(row.protocol == IPPROTO_TCP || row.protocol == IPPROTO_UDP);
	row_text(hit.iItem, hit.iSubItem, value, RTL_NUMBER_OF(value));
	if (!value[0] && !can_inspect)
		return;
	HMENU menu = CreatePopupMenu();
	if (!menu)
		return;
	if (value[0])
	{
		swprintf_s(label, RTL_NUMBER_OF(label), L"Copy %s", visible_row_column(hit.iSubItem)->label);
		AppendMenuW(menu, MF_STRING, 1, label);
	}
	if (can_inspect)
		AppendMenuW(menu, MF_STRING | (inspecting_process ? MF_GRAYED : 0), 2, L"Inspect process…");
	UINT command = TrackPopupMenuEx(menu, TPM_RETURNCMD | TPM_NONOTIFY | TPM_RIGHTBUTTON, screen.x, screen.y, panel, NULL);
	DestroyMenu(menu);
	if (command == 1)
	{
		R_STRINGREF text;
		_r_obj_initializestringref(&text, value);
		_r_clipboard_set(panel, &text);
	}
	else if (command == 2)
		inspect_packet(&row, path, NULL);
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
			for (int j = 0; j < row_column_count(); j++)
			{
				WCHAR text[PH_TEXT];
				row_text(i, j, text, PH_TEXT);
				_r_obj_appendstringbuilder(&b, text);
				_r_obj_appendstringbuilder(&b, j == row_column_count() - 1 ? L"\r\n" : L"\t");
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
	case WM_ERASEBKGND: {
		RECT r;
		GetClientRect(w, &r);
		FillRect((HDC)wp, &r, background ? background : (HBRUSH)(COLOR_WINDOW + 1));
		return 1;
	}
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
	_r_theme_initialize(limit_window);
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
	case WM_CONTEXTMENU:
		if ((HWND)wp == app_list || (HWND)wp == rows)
		{
			list_context_menu((HWND)wp, (POINT){GET_X_LPARAM(lp), GET_Y_LPARAM(lp)});
			return 0;
		}
		break;
	case WM_SHOWWINDOW:
		if (wp)
		{
			needs_query = TRUE;
			update_audit_controls(TRUE);
			redraw_panel(w);
		}
		break;
	case PH_RESULT_READY: {
		PH_RESULT *r = _app_packet_take();
		if (r)
		{
			apply_result(r);
			status();
		}
		return 0;
	}
	case PH_PROCESS_READY: {
		PR_STRING info = (PR_STRING)lp;
		inspecting_process = FALSE;
		if (info)
		{
			MessageBoxW(w, info->buffer, wp ? L"Packet history details" : L"Packet process", MB_OK | MB_ICONINFORMATION);
			_r_obj_dereference(info);
		}
		return 0;
	}
	case WM_TIMER:
		if (wp == 1)
		{
			update_audit_controls(FALSE);
			rescale();
			PH_RESULT *r = _app_packet_take();
			if (r)
				apply_result(r);
			if (!IsWindowVisible(w))
				return 0;
			LONG dns_revision = _app_packet_dns_revision();
			if (seen_dns_revision != dns_revision)
			{
				seen_dns_revision = dns_revision;
				if (shown && shown->page.count)
					ListView_RedrawItems(rows, 0, shown->page.count - 1);
			}
			BOOL changed = seen_revision != _app_packet_revision();
			BOOL audit_changed = !request.query.allow && (!request.query.app || show_service) &&
				seen_audit_revision != _app_packet_audit_revision();
			int selection = ListView_GetNextItem(rows, -1, LVNI_SELECTED);
			BOOL older = request.query.offset > 0 || ListView_GetTopIndex(rows) > 0 || selection >= 0;
			BOOL hosts_changed = request.query.order.column == PH_SORT_HOST && dns_revision != queried_dns_revision;
			if (needs_query)
			{
				query(FALSE);
			}
			else if ((changed || audit_changed || (hosts_changed && !older)) && !waiting && !_app_packet_busy())
			{
				if (older && !audit_changed)
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
			request.query.order = (PH_ORDER){0};
			update_sort_arrow(rows, request.query.order);
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
			app_action();
		else if (id == PH_LIMIT)
			limits();
		else if (id == PH_AUDIT && (HIWORD(wp) == CBN_SELENDOK ||
			(HIWORD(wp) == CBN_SELCHANGE && !SendDlgItemMessageW(w, PH_AUDIT, CB_GETDROPPEDSTATE, 0, 0))))
		{
			LRESULT selection = SendDlgItemMessageW(w, PH_AUDIT, CB_GETCURSEL, 0, 0);
			if (selection >= 0 && selection < RTL_NUMBER_OF(audit_options))
			{
				DWORD result = _app_packet_set_audit_timer(audit_options[selection].minutes);
				update_audit_controls(TRUE);
				if (result != ERROR_SUCCESS)
					_r_show_errormessage(w, L"Could not change Audit Filtering Platform Connection.", NULL, result, ET_WINDOWS);
			}
		}
		return 0;
	}
	case WM_NOTIFY: {
		NMHDR *n = (NMHDR *)lp;
		if (n->idFrom == PH_AUDIT_HELP && (n->code == NM_CLICK || n->code == NM_RETURN))
		{
			MessageBoxW(w,
				L"This Windows policy records blocked connections from all apps in the Security log (event 5157). "
				L"Simplewall uses those events to match blocked svchost.exe connections to the recorded process ID. "
				L"On busy systems, the extra events can fill the Security log faster.\n\n"
				L"A timed choice turns failure auditing off when the timer expires or you exit Simplewall. "
				L"Always On keeps Windows recording these events after you exit Simplewall. Choose Off to stop auditing.",
				audit_label, MB_OK | MB_ICONINFORMATION);
			return 0;
		}
		if (n->code == LVN_COLUMNCLICK && (n->hwndFrom == app_list || n->hwndFrom == rows))
		{
			sort_column(n->hwndFrom, ((NMLISTVIEW *)lp)->iSubItem);
			return 0;
		}
		if (n->code == NM_RCLICK && (n->hwndFrom == rows || n->hwndFrom == app_list))
		{
			POINT screen = ((NMITEMACTIVATE *)lp)->ptAction;
			ClientToScreen(n->hwndFrom, &screen);
			list_context_menu(n->hwndFrom, screen);
			return TRUE;
		}
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
					set_selected_path(shown->apps.items[i].path);
				else
					selected_path[0] = 0;
				update_service_column();
				update_app_action();
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
		WS_EX_CONTROLPARENT, c.lpszClassName, L"",
		WS_CHILD | WS_CLIPCHILDREN | WS_CLIPSIBLINGS | (visible ? WS_VISIBLE : 0),
		r.left, r.top, r.right - r.left, r.bottom - r.top, hwnd, (HMENU)(INT_PTR)IDC_LOG, c.hInstance, NULL);
	// Keep the replacement above the tab control, like the original page window.
	SetWindowPos(panel, HWND_TOP, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
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
	for (int i = 0; i < RTL_NUMBER_OF(app_columns); i++)
		column(app_list, i, app_columns[i].label, app_columns[i].width);
	for (int i = 0; i < row_column_count(); i++)
	{
		const PH_COLUMN *item = visible_row_column(i);
		column(rows, i, item->label, item->width);
	}
	update_sort_arrow(app_list, request.app_order);
	update_sort_arrow(rows, request.query.order);
	child(panel, L"BUTTON", L"Previous apps", BS_PUSHBUTTON, PH_APPPREV);
	child(panel, L"BUTTON", L"More apps", BS_PUSHBUTTON, PH_APPNEXT);
	child(panel, L"STATIC", L"Packet history", SS_LEFT, PH_HEADER);
	child(panel, L"STATIC", L"", SS_LEFT, PH_STATS);
	child(panel, L"STATIC", L"", SS_LEFT, PH_STATUS);
	child(panel, L"BUTTON", L"Clear this app's log", BS_PUSHBUTTON, PH_CLEAR);
	EnableWindow(child(panel, L"BUTTON", L"Allow app", BS_PUSHBUTTON, PH_ALLOW), FALSE);
	child(panel, L"BUTTON", L"Retention…", BS_PUSHBUTTON, PH_LIMIT);
	HWND dir = child(panel, WC_COMBOBOXW, L"", CBS_DROPDOWNLIST | WS_VSCROLL, PH_DIRECTION);
	SendMessageW(dir, CB_ADDSTRING, 0, (LPARAM)L"All directions");
	SendMessageW(dir, CB_ADDSTRING, 0, (LPARAM)L"Inbound");
	SendMessageW(dir, CB_ADDSTRING, 0, (LPARAM)L"Outbound");
	SendMessageW(dir, CB_SETCURSEL, 0, 0);
	child(panel, L"BUTTON", L"Group repeats", BS_AUTOCHECKBOX, PH_GROUP);
	SendDlgItemMessageW(panel, PH_GROUP, BM_SETCHECK, BST_CHECKED, 0);
	WCHAR audit_link[64];
	swprintf_s(audit_link, RTL_NUMBER_OF(audit_link), L"<a>%ls</a>", audit_label);
	child(panel, WC_LINK, audit_link, LWS_TRANSPARENT, PH_AUDIT_HELP);
	HWND audit = child(panel, WC_COMBOBOXW, L"", CBS_DROPDOWNLIST | WS_VSCROLL, PH_AUDIT);
	for (int i = 0; i < RTL_NUMBER_OF(audit_options); i++)
		SendMessageW(audit, CB_ADDSTRING, 0, (LPARAM)audit_options[i].label);
	child(panel, L"STATIC", L"", SS_LEFT, PH_AUDIT_TIME);
	_app_packet_restore_audit_timer();
	update_audit_controls(TRUE);
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
