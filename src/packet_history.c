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
static volatile LONG ph_audit_revision;
static LONG64 ph_dropped, ph_reported;
LONG64 _app_packet_pending_loss(VOID)
{
	return InterlockedCompareExchange64(&ph_dropped, 0, 0) - InterlockedCompareExchange64(&ph_reported, 0, 0);
}
static BOOLEAN ph_requested;
static WCHAR ph_path[32768], ph_failure[256];
static SRWLOCK watches_lock = SRWLOCK_INIT;
static PR_STRING watched_apps;
static const WCHAR watches_key[] = L"AllowedApps";
static const WCHAR watches_section[] = L"packet_history";

VOID _app_packet_reloadwatches(VOID)
{
	PR_STRING paths = _r_config_getstring(watches_key, NULL, watches_section);
	AcquireSRWLockExclusive(&watches_lock);
	_r_obj_movereference((PVOID_PTR)&watched_apps, paths);
	ReleaseSRWLockExclusive(&watches_lock);
}

BOOLEAN _app_packet_haswatches(VOID)
{
	AcquireSRWLockShared(&watches_lock);
	BOOLEAN any = !_r_obj_isstringempty(watched_apps);
	ReleaseSRWLockShared(&watches_lock);
	return any;
}

BOOLEAN _app_packet_iswatched(LPCWSTR path)
{
	if (!path || !*path)
		return FALSE;
	R_STRINGREF identity, remaining = {0}, entry;
	_r_obj_initializestringref(&identity, path);
	AcquireSRWLockShared(&watches_lock);
	if (watched_apps)
		remaining = watched_apps->sr;
	BOOLEAN found = FALSE;
	while (remaining.length)
	{
		_r_str_splitatchar(&remaining, L'|', &entry, &remaining);
		if (_r_str_isequal(&entry, &identity, TRUE))
		{
			found = TRUE;
			break;
		}
	}
	ReleaseSRWLockShared(&watches_lock);
	return found;
}

BOOLEAN _app_packet_setwatch(LPCWSTR path, BOOLEAN enabled)
{
	if (!path || !*path || wcspbrk(path, L"|\r\n"))
		return FALSE;
	R_STRINGREF identity, remaining = {0}, entry;
	R_STRINGBUILDER paths;
	_r_obj_initializestringref(&identity, path);
	_r_obj_initializestringbuilder(&paths, 256);
	if (enabled)
		_r_obj_appendstringbuilder2(&paths, &identity);
	AcquireSRWLockExclusive(&watches_lock);
	if (watched_apps)
		remaining = watched_apps->sr;
	while (remaining.length)
	{
		_r_str_splitatchar(&remaining, L'|', &entry, &remaining);
		if (!entry.length || _r_str_isequal(&entry, &identity, TRUE))
			continue;
		if (paths.string->length)
			_r_obj_appendstringbuilder(&paths, L"|");
		_r_obj_appendstringbuilder2(&paths, &entry);
	}
	// Keep the serialized value within the Windows INI reader's string limit.
	BOOLEAN fits = paths.string->length / sizeof(WCHAR) < 32700;
	if (fits)
		_r_obj_movereference((PVOID_PTR)&watched_apps, _r_obj_reference(paths.string));
	ReleaseSRWLockExclusive(&watches_lock);
	if (fits)
		_r_config_setstring(watches_key, paths.string->length ? paths.string->buffer : NULL, watches_section);
	_r_obj_deletestringbuilder(&paths);
	return fits;
}

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
			if (ph_dns[j].host[0])
				InterlockedIncrement(&ph_dns_changed);
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
		if (wcscmp(ph_dns[index].host, host) != 0)
		{
			wcscpy_s(ph_dns[index].host, 256, host);
			InterlockedIncrement(&ph_dns_changed);
		}
		ph_dns[index].state = 2;
		ph_dns[index].expires = GetTickCount64() + 300000;
		LeaveCriticalSection(&ph_lock);
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

VOID _app_packet_displayname(LPCWSTR path, WCHAR *out, int count)
{
	PITEM_APP app = _app_getappitem(_r_str_gethash2(path, TRUE));
	LPCWSTR name = wcsrchr(path, L'\\');
	wcsncpy_s(out, count, app && !_r_obj_isstringempty(app->display_name) ? app->display_name->buffer
		: name ? name + 1 : path, _TRUNCATE);
	if (app)
		_r_obj_dereference(app);
}

static void PH_CALL app_sort_name(const char *path, char *out, int size)
{
	WCHAR wide_path[PH_TEXT], label[PH_TEXT];
	if (MultiByteToWideChar(CP_UTF8, 0, path, -1, wide_path, RTL_NUMBER_OF(wide_path)))
	{
		_app_packet_displayname(wide_path, label, RTL_NUMBER_OF(label));
		if (!WideCharToMultiByte(CP_UTF8, 0, label, -1, out, size, NULL, NULL))
			out[0] = 0;
	}
}

typedef struct
{
	int family;
	char address[64], name[1024];
} PH_HOST_KEY;

typedef struct
{
	size_t count;
	PH_HOST_KEY entries[RTL_NUMBER_OF(ph_dns)];
} PH_HOST_KEYS;

static int PH_CALL compare_host_key(const void *left, const void *right)
{
	const PH_HOST_KEY *a = left, *b = right;
	int address = strcmp(a->address, b->address);
	return address ? address : (a->family > b->family) - (a->family < b->family);
}

static void snapshot_host_keys(PH_HOST_KEYS *keys)
{
	keys->count = 0;
	if (!_r_config_getboolean(L"IsNetworkResolutionsEnabled", TRUE, NULL))
		return;
	EnterCriticalSection(&ph_lock);
	for (size_t i = 0; i < RTL_NUMBER_OF(ph_dns); i++)
	{
		const PH_DNS *entry = &ph_dns[i];
		if (!entry->state || !entry->host[0])
			continue;
		PH_HOST_KEY *key = &keys->entries[keys->count];
		key->family = entry->af;
		strcpy_s(key->address, sizeof(key->address), entry->address);
		if (WideCharToMultiByte(CP_UTF8, 0, entry->host, -1, key->name, sizeof(key->name), NULL, NULL))
			keys->count++;
	}
	LeaveCriticalSection(&ph_lock);
	qsort(keys->entries, keys->count, sizeof(keys->entries[0]), compare_host_key);
}

static void PH_CALL host_sort_name(void *context, const char *address, int family, char *out, int size)
{
	PH_HOST_KEYS *keys = context;
	if (!keys->count || strlen(address) >= sizeof(keys->entries[0].address))
		return;
	PH_HOST_KEY key;
	key.family = family;
	strcpy_s(key.address, sizeof(key.address), address);
	const PH_HOST_KEY *found = bsearch(&key, keys->entries, keys->count, sizeof(key), compare_host_key);
	if (found)
		strcpy_s(out, size, found->name);
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
	if ((log->is_allow && _r_config_getboolean(L"IsExcludeClassifyAllow", TRUE, NULL) &&
		 !_app_packet_iswatched(log->path ? log->path->buffer : NULL)) ||
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
BOOLEAN _app_packet_issvchost(LPCWSTR path)
{
	return path && config.svchost_path && _wcsicmp(path, config.svchost_path->buffer) == 0;
}

static int store_log(PH_STORE *s, PITEM_LOG log, PH_ROW *audit)
{
	PH_EVENT e = {0};
	PR_STRING remote = NULL, local = NULL;
	WCHAR *key = NULL;
	char *values[8] = {0};
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
	values[7] = utf8(_r_obj_getstring(log->service));
	for (i = 0; i < RTL_NUMBER_OF(values); i++)
		if (!values[i])
			goto done;
	e.identity = values[0];
	e.path = values[1];
	e.rule = values[2];
	e.remote = values[3];
	e.local = values[4];
	e.user = values[5];
	e.layer = values[6];
	e.service = values[7];
	e.timestamp = log->timestamp;
	e.filter = log->filter_id;
	e.allow = log->is_allow;
	e.direction = (log->direction == FWP_DIRECTION_INBOUND ? 0 : 1);
	e.protocol = log->protocol;
	e.af = log->af;
	e.remote_port = log->remote_port;
	e.local_port = log->local_port;
	rc = ph_append(s, &e);
	if (!rc && audit && !e.allow && e.filter && e.local_port && e.remote_port &&
		(e.protocol == IPPROTO_TCP || e.protocol == IPPROTO_UDP) && _app_packet_issvchost(path))
	{
		memset(audit, 0, sizeof(*audit));
		audit->id = ph_inserted_id(s);
		audit->sample_time = e.timestamp;
		audit->filter = e.filter;
		audit->direction = e.direction;
		audit->protocol = e.protocol;
		audit->af = e.af;
		audit->local_port = e.local_port;
		audit->remote_port = e.remote_port;
		snprintf(audit->local, sizeof(audit->local), "%s", e.local);
		snprintf(audit->remote, sizeof(audit->remote), "%s", e.remote);
	}
done:
	for (i = 0; i < RTL_NUMBER_OF(values); i++)
		free(values[i]);
	free(key);
	if (remote)
		_r_obj_dereference(remote);
	if (local)
		_r_obj_dereference(local);
	return rc;
}

static size_t resolve_pending_audit(PH_STORE *s, PH_ROW *rows, ULONGLONG *deadlines, size_t count, ULONGLONG now)
{
	_app_packet_resolve_audit(rows, count);
	if (!_app_packet_audit_enabled())
		return 0;

	int rc = 0;
	BOOL writing = FALSE;
	for (size_t i = 0; i < count && !rc; i++)
	{
		if (!rows[i].pid)
			continue;
		if (!writing)
		{
			rc = ph_begin_cleanup(s);
			writing = !rc;
		}
		if (!rc)
			rc = ph_set_packet_process(s, rows[i].id, rows[i].pid, rows[i].process_started, rows[i].service);
	}
	if (writing)
	{
		if (!rc)
			rc = ph_commit(s);
		if (rc)
			ph_rollback(s);
		else
			InterlockedIncrement(&ph_audit_revision);
	}

	size_t remaining = 0;
	for (size_t i = 0; i < count; i++)
	{
		if ((rows[i].pid && !rc) || now >= deadlines[i])
			continue;
		rows[remaining] = rows[i];
		deadlines[remaining++] = deadlines[i];
	}
	return remaining;
}

static DWORD WINAPI writer_thread(PVOID arg)
{
	PH_STORE *s = NULL;
	PH_ROW *pending_audit = NULL;
	ULONGLONG deadlines[PH_AUDIT_BATCH], next_audit = 0;
	size_t pending_count = 0;
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
		BOOL audit = !ph_stopping && _app_packet_audit_enabled();
		ULONGLONG now = GetTickCount64();
		if (!audit)
			pending_count = 0;
		else if (!pending_audit)
			pending_audit = calloc(PH_AUDIT_BATCH, sizeof(*pending_audit));
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
			size_t before = pending_count;
			rc = s ? (batch->command ? ph_begin_cleanup(s) : ph_begin(s)) : 1;
			for (n = batch; n; n = n->next)
			{
				if (n->log)
				{
					records++;
					if (!rc)
					{
						PH_ROW *candidate = audit && pending_audit && pending_count < PH_AUDIT_BATCH
							? &pending_audit[pending_count] : NULL;
						if (candidate)
							candidate->id = 0;
						rc = store_log(s, n->log, candidate);
						if (!rc && candidate && candidate->id)
							deadlines[pending_count++] = now + 3000;
					}
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
				pending_count = before;
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
		if (pending_count && now >= next_audit && !ph_stopping)
		{
			next_audit = now + 500;
			pending_count = resolve_pending_audit(s, pending_audit, deadlines, pending_count, now);
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
	free(pending_audit);
	ph_close(s);
	return 0;
}
static DWORD WINAPI reader_thread(PVOID arg)
{
	HWND hwnd = (HWND)arg;
	PH_STORE *s = NULL;
	PH_HOST_KEYS host_keys;
	const PH_SORT_KEYS keys = {&host_keys, app_sort_name, host_sort_name};
	char *path;
	WaitForSingleObject(ph_ready_event, INFINITE);
	path = utf8(ph_path);
	if (path)
	{
		ph_open(&s, path, 1);
		free(path);
	}
	if (s)
		ph_set_sort_keys(s, &keys);
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
			r->error = ph_apps(s, request.query.allow, filter ? filter : "", request.app_offset, request.app_order, &r->apps);
			if (!r->error && !request.apps_only)
			{
				if (request.query.order.column == PH_SORT_HOST)
					snapshot_host_keys(&host_keys);
				r->error = ph_query(s, &request.query, &r->page);
			}
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
			PostMessageW(GetDlgItem(hwnd, IDC_LOG), PH_RESULT_READY, 0, 0);
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
LONG _app_packet_audit_revision(VOID)
{
	return InterlockedCompareExchange(&ph_audit_revision, 0, 0);
}
VOID _app_packet_init(HWND hwnd)
{
	PR_STRING dir = _r_app_getprofiledirectory();
	_app_packet_reloadwatches();
	InitializeCriticalSection(&ph_lock);
	swprintf_s(ph_path, RTL_NUMBER_OF(ph_path), L"%s\\packet-history.db", dir->buffer);
	ph_write_event = CreateEventW(NULL, FALSE, FALSE, NULL);
	ph_read_event = CreateEventW(NULL, FALSE, FALSE, NULL);
	ph_ready_event = CreateEventW(NULL, TRUE, FALSE, NULL);
	InterlockedExchange(&ph_initialized, 1);
	ph_dns_event = CreateEventW(NULL, FALSE, FALSE, NULL);
	ph_dns_thread = CreateThread(NULL, 0, dns_thread, NULL, 0, NULL);
	ph_writer = CreateThread(NULL, 0, writer_thread, NULL, 0, NULL);
	ph_reader = CreateThread(NULL, 0, reader_thread, hwnd, 0, NULL);
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
	_app_packet_stop_audit_timer();
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
