#include "global.h"
#include "packet_history.h"
#include "packet_queue.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
typedef struct PH_NODE
{
	struct PH_NODE *next;
	PITEM_LOG log;
	SIZE_T bytes;
	int command;
	int64_t app, cap;
	int days, mib;
} PH_NODE;
static CRITICAL_SECTION ph_lock;
static PH_BUDGET ph_capture_budget, ph_history_budget;
static PH_NODE *ph_head, *ph_tail;
static HANDLE ph_write_event, ph_read_event, ph_ready_event, ph_writer, ph_reader;
static PH_STORE *ph_reader_store;
static PH_REQUEST ph_request;
static PH_RESULT *ph_result;
static LONG ph_generation, ph_revision, ph_initialized, ph_stopping, ph_read_busy;
static LONG64 ph_dropped, ph_reported;
LONG64 _app_packet_pending_loss(VOID)
{
	return InterlockedCompareExchange64(&ph_dropped, 0, 0) - InterlockedCompareExchange64(&ph_reported, 0, 0);
}
static BOOLEAN ph_requested;
static WCHAR ph_path[32768], ph_failure[256];
typedef struct
{
	char address[64];
	WCHAR host[256];
	int af, state;
	ULONGLONG expires;
} PH_DNS;
static PH_DNS ph_dns[256];
static HANDLE ph_dns_event, ph_dns_thread;
static LONG ph_dns_changed;
static unsigned ph_dns_next;
LONG _app_packet_dns_revision(VOID)
{
	return InterlockedCompareExchange(&ph_dns_changed, 0, 0);
}
VOID _app_packet_hostname(const char *address, int af, WCHAR *out, int count)
{
	int i;
	out[0] = 0;
	if (!_r_config_getboolean(L"IsNetworkResolutionsEnabled", TRUE, NULL))
		return;
	EnterCriticalSection(&ph_lock);
	for (i = 0; i < 256; i++)
		if (ph_dns[i].state && ph_dns[i].af == af && !strcmp(ph_dns[i].address, address))
		{
			if (ph_dns[i].state == 2 && ph_dns[i].expires < GetTickCount64())
			{
				ph_dns[i].state = 1;
				SetEvent(ph_dns_event);
			}
			wcsncpy_s(out, count, ph_dns[i].host, _TRUNCATE);
			LeaveCriticalSection(&ph_lock);
			return;
		}
	/* Never replace pending work; this bounds queued reverse lookups at 256. */
	for (i = 0; i < 256; i++)
	{
		unsigned j = (ph_dns_next + i) % 256;
		if (ph_dns[j].state != 1)
		{
			memset(&ph_dns[j], 0, sizeof(ph_dns[j]));
			strncpy_s(ph_dns[j].address, 64, address, _TRUNCATE);
			ph_dns[j].af = af;
			ph_dns[j].state = 1;
			ph_dns_next = (j + 1) % 256;
			SetEvent(ph_dns_event);
			break;
		}
	}
	LeaveCriticalSection(&ph_lock);
}
static DWORD WINAPI dns_thread(PVOID arg)
{
	WSADATA data;
	WSAStartup(MAKEWORD(2, 2), &data);
	(void)arg;
	while (!ph_stopping)
	{
		int index = -1;
		PH_DNS entry;
		WaitForSingleObject(ph_dns_event, 250);
		if (ph_stopping)
			break;
		EnterCriticalSection(&ph_lock);
		for (int i = 0; i < 256; i++)
			if (ph_dns[i].state == 1)
			{
				index = i;
				entry = ph_dns[i];
				break;
			}
		LeaveCriticalSection(&ph_lock);
		if (index < 0)
			continue;
		WCHAR address[64], host[256] = {0};
		MultiByteToWideChar(CP_UTF8, 0, entry.address, -1, address, 64);
		if (entry.af == AF_INET)
		{
			SOCKADDR_IN sa = {0};
			sa.sin_family = AF_INET;
			if (InetPtonW(AF_INET, address, &sa.sin_addr) == 1)
				GetNameInfoW((SOCKADDR *)&sa, sizeof(sa), host, 256, NULL, 0, NI_NAMEREQD);
		}
		else if (entry.af == AF_INET6)
		{
			SOCKADDR_IN6 sa = {0};
			sa.sin6_family = AF_INET6;
			if (InetPtonW(AF_INET6, address, &sa.sin6_addr) == 1)
				GetNameInfoW((SOCKADDR *)&sa, sizeof(sa), host, 256, NULL, 0, NI_NAMEREQD);
		}
		EnterCriticalSection(&ph_lock);
		wcscpy_s(ph_dns[index].host, 256, host);
		ph_dns[index].state = 2;
		ph_dns[index].expires = GetTickCount64() + 300000;
		LeaveCriticalSection(&ph_lock);
		InterlockedIncrement(&ph_dns_changed);
		SetEvent(ph_dns_event);
	}
	WSACleanup();
	return 0;
}
static char *utf8(LPCWSTR s)
{
	int n;
	char *b;
	if (!s)
		s = L"";
	n = WideCharToMultiByte(CP_UTF8, 0, s, -1, NULL, 0, NULL, NULL);
	b = malloc(n ? n : 1);
	if (b)
		WideCharToMultiByte(CP_UTF8, 0, s, -1, b, n, NULL, NULL);
	return b;
}
static void error_text(LPCWSTR s)
{
	EnterCriticalSection(&ph_lock);
	wcsncpy_s(ph_failure, RTL_NUMBER_OF(ph_failure), s, _TRUNCATE);
	LeaveCriticalSection(&ph_lock);
	InterlockedIncrement(&ph_revision);
}
VOID _app_packet_error(WCHAR *b, SIZE_T n)
{
	EnterCriticalSection(&ph_lock);
	wcsncpy_s(b, n, ph_failure, _TRUNCATE);
	LeaveCriticalSection(&ph_lock);
}
VOID _app_packet_lost(VOID)
{
	InterlockedIncrement64(&ph_dropped);
}
BOOLEAN _app_packet_capture_reserve(SIZE_T bytes)
{
	int ok;
	if (!ph_initialized)
		return FALSE;
	EnterCriticalSection(&ph_lock);
	ok = ph_reserve(&ph_capture_budget, bytes);
	LeaveCriticalSection(&ph_lock);
	if (!ok)
		_app_packet_lost();
	return (BOOLEAN)ok;
}
VOID _app_packet_capture_release(SIZE_T bytes)
{
	if (!bytes)
		return;
	EnterCriticalSection(&ph_lock);
	ph_release(&ph_capture_budget, bytes);
	LeaveCriticalSection(&ph_lock);
}
static void enqueue(PH_NODE *n)
{
	int ok;
	EnterCriticalSection(&ph_lock);
	ok = ph_reserve(&ph_history_budget, n->bytes);
	if (ok)
	{
		if (ph_tail)
			ph_tail->next = n;
		else
			ph_head = n;
		ph_tail = n;
	}
	LeaveCriticalSection(&ph_lock);
	if (!ok)
	{
		if (n->log)
		{
			_app_packet_lost();
			_r_obj_dereference(n->log);
		}
		else
			error_text(L"History command queue is full. Please try again.");
		free(n);
	}
	else
		SetEvent(ph_write_event);
}
VOID _app_packet_submit(PITEM_LOG log)
{
	PH_NODE *n;
	if (!ph_initialized || !log->is_myprovider || !_r_config_getboolean(L"IsLogUiEnabled", FALSE, NULL))
		return;
	if ((log->is_allow && _r_config_getboolean(L"IsExcludeClassifyAllow", TRUE, NULL)) ||
		(log->is_system && _r_config_getboolean(L"IsExcludeStealth", TRUE, NULL)))
		return;
	n = calloc(1, sizeof(*n));
	if (!n)
	{
		_app_packet_lost();
		return;
	}
	n->log = _r_obj_reference(log);
	n->bytes = log->capture_bytes + sizeof(*n);
	enqueue(n);
}
VOID _app_packet_command(int type, int64_t app, int64_t cap, int days, int mib)
{
	PH_NODE *n = calloc(1, sizeof(*n));
	if (!n)
	{
		error_text(L"Not enough memory for history command.");
		return;
	}
	n->bytes = sizeof(*n);
	n->command = type;
	n->app = app;
	n->cap = cap;
	n->days = days;
	n->mib = mib;
	enqueue(n);
}
static int store_log(PH_STORE *s, PITEM_LOG log)
{
	PH_EVENT e = {0};
	PR_STRING remote = NULL, local = NULL;
	WCHAR *key = NULL;
	char *values[7] = {0};
	int rc = 7;
	SIZE_T len;
	int i;
	remote = _app_formataddress(log->af, log->protocol, &log->remote_addr, 0, 0);
	local = _app_formataddress(log->af, log->protocol, &log->local_addr, 0, 0);
	LPCWSTR path =
		log->path ? log->path->buffer
				  : (log->app_hash == config.ntoskrnl_hash ? L"System" : L"Unknown / unattributed traffic");
	len = wcslen(path);
	key = malloc((len + 1) * sizeof(WCHAR));
	if (!key)
		goto done;
	memcpy(key, path, (len + 1) * sizeof(WCHAR));
	CharLowerBuffW(key, (DWORD)len);
	values[0] = utf8(key);
	values[1] = utf8(path);
	values[2] = utf8(_r_obj_getstring(log->filter_name));
	values[3] = utf8(_r_obj_getstring(remote));
	values[4] = utf8(_r_obj_getstring(local));
	values[5] = utf8(_r_obj_getstring(log->username));
	values[6] = utf8(_r_obj_getstring(log->layer_name));
	for (i = 0; i < 7; i++)
		if (!values[i])
			goto done;
	e.identity = values[0];
	e.path = values[1];
	e.rule = values[2];
	e.remote = values[3];
	e.local = values[4];
	e.user = values[5];
	e.layer = values[6];
	e.timestamp = log->timestamp;
	e.filter = log->filter_id;
	e.allow = log->is_allow;
	e.direction = (log->direction == FWP_DIRECTION_INBOUND ? 0 : 1);
	e.protocol = log->protocol;
	e.af = log->af;
	e.remote_port = log->remote_port;
	e.local_port = log->local_port;
	rc = ph_append(s, &e);
done:
	for (i = 0; i < 7; i++)
		free(values[i]);
	free(key);
	if (remote)
		_r_obj_dereference(remote);
	if (local)
		_r_obj_dereference(local);
	return rc;
}
static DWORD WINAPI writer_thread(PVOID arg)
{
	PH_STORE *s = NULL;
	char *path = utf8(ph_path);
	int rc = path ? ph_open(&s, path, 0) : 7;
	LONG64 reported = 0;
	free(path);
	(void)arg;
	if (rc)
	{
		error_text(L"Cannot open packet history database. History capture is unavailable; existing data was "
				   L"preserved.");
		SetEvent(ph_ready_event);
	}
	else
	{
		int n;
		do
		{
			n = ph_retention(s, _r_unixtime_now(), 1024);
		} while (n > 0 && !ph_stopping);
		SetEvent(ph_ready_event);
	}
	for (;;)
	{
		PH_NODE *batch = NULL, *last = NULL, *n;
		int count = 0, records = 0;
		LONG64 lost;
		WaitForSingleObject(ph_write_event, 250);
		EnterCriticalSection(&ph_lock);
		while (ph_head && count++ < 256)
		{
			if (batch && (!!ph_head->command != !!batch->command))
				break;
			n = ph_head;
			ph_head = n->next;
			n->next = NULL;
			if (last)
				last->next = n;
			else
				batch = n;
			last = n;
		}
		if (!ph_head)
			ph_tail = NULL;
		else
			SetEvent(ph_write_event);
		LeaveCriticalSection(&ph_lock);
		if (batch)
		{
			rc = s ? (batch->command ? ph_begin_cleanup(s) : ph_begin(s)) : 1;
			for (n = batch; n; n = n->next)
			{
				if (n->log)
				{
					records++;
					if (!rc)
						rc = store_log(s, n->log);
				}
				else if (!rc)
				{
					if (n->command == 1)
						rc = ph_clear(s, n->app);
					else
						rc = ph_limits(s, n->app, n->cap, n->days, n->mib);
				}
			}
			if (!rc)
				rc = ph_commit(s);
			else if (s)
				ph_rollback(s);
			if (rc)
			{
				if (s)
					ph_rollback(s);
				InterlockedAdd64(&ph_dropped, records);
				error_text(L"Packet history write failed. Events were dropped; check disk space and database "
						   L"permissions.");
			}
			for (n = batch; n;)
			{
				PH_NODE *next = n->next;
				if (n->log)
					_r_obj_dereference(n->log);
				EnterCriticalSection(&ph_lock);
				ph_release(&ph_history_budget, n->bytes);
				LeaveCriticalSection(&ph_lock);
				free(n);
				n = next;
			}
			InterlockedIncrement(&ph_revision);
		}
		lost = InterlockedCompareExchange64(&ph_dropped, 0, 0);
		if (s && lost > reported)
		{
			if (!ph_gap(s, lost - reported, _r_unixtime_now(),
						"Queue saturation, allocation or storage failure"))
			{
				reported = lost;
				InterlockedExchange64(&ph_reported, lost);
				InterlockedIncrement(&ph_revision);
			}
		}
		if (s)
		{
			int removed = ph_retention(s, _r_unixtime_now(), 1024);
			if (removed > 0)
				InterlockedIncrement(&ph_revision);
			else if (removed < 0)
				error_text(L"Packet history cleanup failed; check disk space, the history budget and "
						   L"database permissions.");
		}
		EnterCriticalSection(&ph_lock);
		int stop = ph_stopping && !ph_head;
		LeaveCriticalSection(&ph_lock);
		if (stop)
			break;
	}
	ph_close(s);
	return 0;
}
static DWORD WINAPI reader_thread(PVOID arg)
{
	PH_STORE *s = NULL;
	char *path;
	(void)arg;
	WaitForSingleObject(ph_ready_event, INFINITE);
	path = utf8(ph_path);
	if (path)
	{
		ph_open(&s, path, 1);
		free(path);
	}
	EnterCriticalSection(&ph_lock);
	ph_reader_store = s;
	LeaveCriticalSection(&ph_lock);
	while (!ph_stopping)
	{
		PH_REQUEST request;
		PH_RESULT *r;
		char *filter;
		WaitForSingleObject(ph_read_event, INFINITE);
		if (ph_stopping)
			break;
		EnterCriticalSection(&ph_lock);
		if (!ph_requested)
		{
			LeaveCriticalSection(&ph_lock);
			continue;
		}
		request = ph_request;
		ph_requested = FALSE;
		InterlockedExchange(&ph_read_busy, 1);
		LeaveCriticalSection(&ph_lock);
		r = calloc(1, sizeof(*r));
		if (!r)
		{
			error_text(L"Not enough memory to display packet history.");
			InterlockedExchange(&ph_read_busy, 0);
			continue;
		}
		r->request = request;
		filter = utf8(request.filter);
		if (!s)
			r->error = 1;
		else
		{
			r->error = ph_apps(s, request.query.allow, filter ? filter : "", request.app_offset, &r->apps);
			if (!r->error && !request.apps_only)
				r->error = ph_query(s, &request.query, &r->page);
			if (!r->error)
				r->error = ph_getlimits(s, request.query.app, &r->cap, &r->days, &r->mib);
			r->lost = ph_lost(s);
			r->evicted = ph_evicted(s);
			r->bytes = ph_storage_bytes(s);
			if (r->error)
				snprintf(r->message, sizeof(r->message), "%s", ph_error(s));
		}
		free(filter);
		EnterCriticalSection(&ph_lock);
		if (request.generation == ph_generation && !ph_stopping)
		{
			free(ph_result);
			ph_result = r;
		}
		else
			free(r);
		InterlockedExchange(&ph_read_busy, 0);
		LeaveCriticalSection(&ph_lock);
	}
	EnterCriticalSection(&ph_lock);
	ph_reader_store = NULL;
	LeaveCriticalSection(&ph_lock);
	ph_close(s);
	return 0;
}
VOID _app_packet_request(PH_REQUEST *r, BOOLEAN cancel)
{
	EnterCriticalSection(&ph_lock);
	r->generation = ++ph_generation;
	ph_request = *r;
	ph_requested = TRUE;
	if (cancel && ph_reader_store)
		ph_cancel(ph_reader_store);
	LeaveCriticalSection(&ph_lock);
	SetEvent(ph_read_event);
}
PH_RESULT *_app_packet_take(VOID)
{
	PH_RESULT *r;
	EnterCriticalSection(&ph_lock);
	r = ph_result;
	ph_result = NULL;
	LeaveCriticalSection(&ph_lock);
	return r;
}
BOOLEAN _app_packet_busy(VOID)
{
	return (BOOLEAN)InterlockedCompareExchange(&ph_read_busy, 0, 0);
}
LONG _app_packet_revision(VOID)
{
	return InterlockedCompareExchange(&ph_revision, 0, 0);
}
VOID _app_packet_init(HWND hwnd)
{
	PR_STRING dir = _r_app_getprofiledirectory();
	InitializeCriticalSection(&ph_lock);
	swprintf_s(ph_path, RTL_NUMBER_OF(ph_path), L"%s\\packet-history.db", dir->buffer);
	ph_write_event = CreateEventW(NULL, FALSE, FALSE, NULL);
	ph_read_event = CreateEventW(NULL, FALSE, FALSE, NULL);
	ph_ready_event = CreateEventW(NULL, TRUE, FALSE, NULL);
	InterlockedExchange(&ph_initialized, 1);
	ph_dns_event = CreateEventW(NULL, FALSE, FALSE, NULL);
	ph_dns_thread = CreateThread(NULL, 0, dns_thread, NULL, 0, NULL);
	ph_writer = CreateThread(NULL, 0, writer_thread, NULL, 0, NULL);
	ph_reader = CreateThread(NULL, 0, reader_thread, NULL, 0, NULL);
	if (!ph_writer || !ph_reader)
	{
		error_text(L"Could not start packet history workers.");
		if (!ph_writer)
			SetEvent(ph_ready_event);
	}
	_app_packet_view_init(hwnd);
}
VOID _app_packet_shutdown(VOID)
{
	if (!ph_initialized)
		return;
	EnterCriticalSection(&ph_lock);
	ph_capture_budget.closed = 1;
	ph_history_budget.closed = 1;
	InterlockedExchange(&ph_stopping, 1);
	if (ph_reader_store)
		ph_cancel(ph_reader_store);
	LeaveCriticalSection(&ph_lock);
	SetEvent(ph_write_event);
	SetEvent(ph_read_event);
	SetEvent(ph_dns_event);
	if (ph_dns_thread)
		WaitForSingleObject(ph_dns_thread, 1000);
	if (ph_reader)
		WaitForSingleObject(ph_reader, INFINITE);
	if (ph_writer)
		WaitForSingleObject(ph_writer, INFINITE);
	free(ph_result);
	ph_result = NULL;
	/* Synchronization remains alive until process exit: existing notification/CSV
	   workers may still release their upstream capture reservations. */
}
