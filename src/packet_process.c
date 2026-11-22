#include "global.h"
#include "packet_history.h"
#include <ntsecapi.h>
#include <winevt.h>
#include <windns.h>
#include <winhttp.h>
#include <wbemidl.h>
#include <stdlib.h>

#pragma comment(lib, "wevtapi.lib")
#pragma comment(lib, "dnsapi.lib")
#pragma comment(lib, "winhttp.lib")
#pragma comment(lib, "wbemuuid.lib")

// Windows' Filtering Platform Connection audit subcategory.
static volatile LONG blocked_audit_enabled;
static const GUID connection_audit = {0x0cce9226, 0x69ae, 0x11d9, {0xbe, 0xd3, 0x50, 0x50, 0x54, 0x50, 0x30, 0x30}};
static const WCHAR audit_section[] = L"packet_history";
static ULONG audit_minutes;
static ULONGLONG audit_deadline, audit_retry;

static ULONG blocked_audit_flags(ULONG current, BOOLEAN enabled)
{
	ULONG flags = (current & POLICY_AUDIT_EVENT_SUCCESS) | (enabled ? POLICY_AUDIT_EVENT_FAILURE : 0);
	return flags ? flags : POLICY_AUDIT_EVENT_NONE;
}

DWORD _app_packet_query_blocked_audit(PBOOLEAN enabled)
{
	PAUDIT_POLICY_INFORMATION policy;
	if (!AuditQuerySystemPolicy(&connection_audit, 1, &policy))
	{
		DWORD status = GetLastError();
		InterlockedExchange(&blocked_audit_enabled, 0);
		return status;
	}

	*enabled = !!(policy->AuditingInformation & POLICY_AUDIT_EVENT_FAILURE);
	InterlockedExchange(&blocked_audit_enabled, *enabled);
	AuditFree(policy);
	return ERROR_SUCCESS;
}

static DWORD set_blocked_audit(BOOLEAN enabled)
{
	PAUDIT_POLICY_INFORMATION policy;
	if (!AuditQuerySystemPolicy(&connection_audit, 1, &policy))
		return GetLastError();

	DWORD status = ERROR_SUCCESS;
	if (!!(policy->AuditingInformation & POLICY_AUDIT_EVENT_FAILURE) != enabled)
	{
		policy->AuditingInformation = blocked_audit_flags(policy->AuditingInformation, enabled);
		if (!AuditSetSystemPolicy(policy, 1))
			status = GetLastError();
	}
	AuditFree(policy);
	if (status == ERROR_SUCCESS)
		InterlockedExchange(&blocked_audit_enabled, enabled);
	return status;
}

static ULONG audit_timer_remaining(ULONGLONG deadline, ULONGLONG now)
{
	return now < deadline ? (ULONG)((deadline - now + 999) / 1000) : 0;
}

DWORD _app_packet_set_audit_timer(ULONG minutes)
{
	BOOLEAN timed = minutes && minutes != PH_AUDIT_ALWAYS;
	LONG64 previous = 0;
	if (timed)
	{
		previous = _r_config_getlong64(L"AuditUntil", 0, audit_section);
		// Record ownership before enabling the system policy so a restart can finish cleanup.
		_r_config_setlong64(L"AuditUntil", _r_unixtime_now() + minutes * 60, audit_section);
	}

	DWORD status = set_blocked_audit(minutes != 0);
	if (status != ERROR_SUCCESS)
	{
		if (timed)
			_r_config_setlong64(L"AuditUntil", previous, audit_section);
		return status;
	}

	audit_minutes = timed ? minutes : 0;
	audit_deadline = timed ? GetTickCount64() + (ULONGLONG)minutes * 60000 : 0;
	audit_retry = 0;
	if (!timed)
		_r_config_setlong64(L"AuditUntil", 0, audit_section);
	_r_config_setlong(L"AuditMinutes", audit_minutes, audit_section);
	return ERROR_SUCCESS;
}

void _app_packet_restore_audit_timer(void)
{
	LONG64 deadline = _r_config_getlong64(L"AuditUntil", 0, audit_section);
	if (deadline <= 0)
		return;

	LONG64 remaining = deadline - _r_unixtime_now();
	LONG saved = _r_config_getlong(L"AuditMinutes", 0, audit_section);
	audit_minutes = saved == 15 || saved == 30 || saved == 60 || saved == 720 || saved == PH_AUDIT_MAX_MINUTES
		? saved : 15;
	audit_deadline = GetTickCount64() + (ULONGLONG)min(PH_AUDIT_MAX_MINUTES * 60, max(0, remaining)) * 1000;
}

ULONG _app_packet_update_audit_timer(PULONG minutes)
{
	ULONGLONG now = GetTickCount64();
	if (audit_minutes && now >= audit_deadline && now >= audit_retry)
	{
		if (_app_packet_set_audit_timer(0) != ERROR_SUCCESS)
			audit_retry = now + 5000;
	}
	*minutes = audit_minutes;
	return audit_timer_remaining(audit_deadline, now);
}

void _app_packet_stop_audit_timer(void)
{
	if (audit_minutes)
		_app_packet_set_audit_timer(0);
}

BOOLEAN _app_packet_audit_enabled(void)
{
	return InterlockedCompareExchange(&blocked_audit_enabled, 0, 0) != 0;
}

static BOOLEAN process_matches_packet(LPCWSTR image, LPCWSTR path, ULONGLONG started, ULONGLONG packet_time)
{
	return started && started <= packet_time && _wcsicmp(image, path) == 0;
}

static ULONGLONG filetime_value(FILETIME time)
{
	return ((ULONGLONG)time.dwHighDateTime << 32) | time.dwLowDateTime;
}

static void audit_time(int64_t unix_time, WCHAR *out, size_t count)
{
	FILETIME time;
	SYSTEMTIME date = {0};
	_r_unixtime_to_filetime(&time, unix_time);
	FileTimeToSystemTime(&time, &date);
	swprintf_s(out, count, L"%04u-%02u-%02uT%02u:%02u:%02u.0000000Z",
		date.wYear, date.wMonth, date.wDay, date.wHour, date.wMinute, date.wSecond);
}

static LPCWSTR audit_string(const EVT_VARIANT *value)
{
	return value->Type == EvtVarTypeString ? value->StringVal : NULL;
}

static EVT_VARIANT *render_event_values(EVT_HANDLE context, EVT_HANDLE event, DWORD expected)
{
	DWORD bytes, count;
	if (EvtRender(context, event, EvtRenderEventValues, 0, NULL, &bytes, &count) ||
		GetLastError() != ERROR_INSUFFICIENT_BUFFER || bytes > 64 * 1024)
		return NULL;

	EVT_VARIANT *values = malloc(bytes);
	if (values && (!EvtRender(context, event, EvtRenderEventValues, bytes, values, &bytes, &count) ||
		count != expected))
	{
		free(values);
		return NULL;
	}
	return values;
}

static ULONGLONG audit_number(const EVT_VARIANT *value)
{
	switch (value->Type)
	{
	case EvtVarTypeUInt32:
	case EvtVarTypeHexInt32:
		return value->UInt32Val;
	case EvtVarTypeUInt64:
	case EvtVarTypeHexInt64:
		return value->UInt64Val;
	case EvtVarTypeString:
	{
		LPCWSTR digits = value->StringVal;
		if (!digits)
			return 0;
		WCHAR *end;
		int base = _wcsnicmp(digits, L"0x", 2) == 0 ? 16 : 10;
		ULONGLONG number = wcstoull(digits, &end, base);
		return end != digits && !*end ? number : 0;
	}
	default:
		return 0;
	}
}

static BOOL endpoint_matches(int family, const char *address, int port, const EVT_VARIANT *audit_address, const EVT_VARIANT *audit_port)
{
	LPCWSTR address_text = audit_string(audit_address);
	IN6_ADDR stored, candidate;
	if (!address_text || (ULONGLONG)port != audit_number(audit_port) || inet_pton(family, address, &stored) != 1)
		return FALSE;

	if (InetPtonW(family, address_text, &candidate) == 1)
		return memcmp(&stored, &candidate, family == AF_INET ? sizeof(IN_ADDR) : sizeof(IN6_ADDR)) == 0;

	return family == AF_INET && InetPtonW(AF_INET6, address_text, &candidate) == 1 &&
		IN6_IS_ADDR_V4MAPPED(&candidate) && memcmp(&stored, candidate.u.Byte + 12, sizeof(IN_ADDR)) == 0;
}

static const WCHAR bits_channel[] = L"Microsoft-Windows-Bits-Client/Operational";

static BOOLEAN same_bits_job(const GUID *left, LPCWSTR left_host, const GUID *right, LPCWSTR right_host)
{
	return IsEqualGUID(left, right) && _wcsicmp(left_host, right_host) == 0;
}

// The DNS Client cache provider can expose records that a cache-only DnsQuery misses.
static BOOL bits_dns_client_cache_matches(const PH_ROW *row, LPCWSTR host, const IN6_ADDR *remote, size_t bytes)
{
	if (wcschr(host, L'\'') || wcschr(host, L'\\'))
		return FALSE;

	HRESULT initialized = CoInitializeEx(NULL, COINIT_MULTITHREADED);
	if (FAILED(initialized) && initialized != RPC_E_CHANGED_MODE)
		return FALSE;

	IWbemLocator *locator = NULL;
	IWbemServices *service = NULL;
	IEnumWbemClassObject *records = NULL;
	WCHAR query[512];
	swprintf_s(query, RTL_NUMBER_OF(query),
		L"SELECT Data FROM MSFT_DNSClientCache WHERE Entry = '%s' AND Type = %u AND Status = 0",
		host, row->af == AF_INET ? DNS_TYPE_A : DNS_TYPE_AAAA);
	BSTR namespace_name = SysAllocString(L"ROOT\\StandardCimv2");
	BSTR language = SysAllocString(L"WQL");
	BSTR expression = SysAllocString(query);
	BOOL match = FALSE;
	if (namespace_name && language && expression &&
		SUCCEEDED(CoCreateInstance(&CLSID_WbemLocator, NULL, CLSCTX_INPROC_SERVER, &IID_IWbemLocator, (PVOID *)&locator)) &&
		SUCCEEDED(locator->lpVtbl->ConnectServer(locator, namespace_name, NULL, NULL, NULL,
			WBEM_FLAG_CONNECT_USE_MAX_WAIT, NULL, NULL, &service)) &&
		SUCCEEDED(CoSetProxyBlanket((IUnknown *)service, RPC_C_AUTHN_WINNT, RPC_C_AUTHZ_NONE, NULL,
			RPC_C_AUTHN_LEVEL_CALL, RPC_C_IMP_LEVEL_IMPERSONATE, NULL, EOAC_NONE)) &&
		SUCCEEDED(service->lpVtbl->ExecQuery(service, language, expression,
			WBEM_FLAG_FORWARD_ONLY | WBEM_FLAG_RETURN_IMMEDIATELY, NULL, &records)))
	{
		for (ULONG i = 0; i < 16 && !match; i++)
		{
			IWbemClassObject *record;
			ULONG count;
			if (FAILED(records->lpVtbl->Next(records, 250, 1, &record, &count)) || !count)
				break;
			VARIANT value;
			VariantInit(&value);
			IN6_ADDR address;
			if (SUCCEEDED(record->lpVtbl->Get(record, L"Data", 0, &value, NULL, NULL)) &&
				value.vt == VT_BSTR && value.bstrVal && InetPtonW(row->af, value.bstrVal, &address) == 1)
				match = memcmp(remote, &address, bytes) == 0;
			VariantClear(&value);
			record->lpVtbl->Release(record);
		}
	}
	if (records)
		records->lpVtbl->Release(records);
	if (service)
		service->lpVtbl->Release(service);
	if (locator)
		locator->lpVtbl->Release(locator);
	SysFreeString(expression);
	SysFreeString(language);
	SysFreeString(namespace_name);
	if (SUCCEEDED(initialized))
		CoUninitialize();
	return match;
}

static BOOL bits_host_matches(const PH_ROW *row, LPCWSTR host)
{
	IN6_ADDR remote, address;
	int family = row->af;
	if (inet_pton(family, row->remote, &remote) != 1)
		return FALSE;

	size_t bytes = family == AF_INET ? sizeof(IN_ADDR) : sizeof(IN6_ADDR);
	if (InetPtonW(family, host, &address) == 1)
		return memcmp(&remote, &address, bytes) == 0;

	WORD type = family == AF_INET ? DNS_TYPE_A : DNS_TYPE_AAAA;
	PDNS_RECORDW records = NULL;
	DNS_STATUS status = DnsQuery_W(host, type, DNS_QUERY_CACHE_ONLY, NULL, &records, NULL);
	BOOL match = FALSE;
	for (const DNS_RECORDW *entry = records; status == ERROR_SUCCESS && entry; entry = entry->pNext)
	{
		if (entry->wType != type)
			continue;
		const void *ip = family == AF_INET ? (const void *)&entry->Data.A.IpAddress : &entry->Data.AAAA.Ip6Address;
		if (memcmp(&remote, ip, bytes) == 0)
		{
			match = TRUE;
			break;
		}
	}
	if (records)
		DnsRecordListFree(records, DnsFreeRecordList);
	return match || bits_dns_client_cache_matches(row, host, &remote, bytes);
}

static BOOL bits_url_host(const PH_ROW *row, LPCWSTR url, WCHAR *host, size_t count)
{
	URL_COMPONENTSW parts = { .dwStructSize = sizeof(parts), .dwHostNameLength = (DWORD)-1 };
	if (!url || !WinHttpCrackUrl(url, 0, 0, &parts) ||
		(parts.nScheme != INTERNET_SCHEME_HTTP && parts.nScheme != INTERNET_SCHEME_HTTPS) ||
		parts.nPort != row->remote_port)
		return FALSE;

	size_t size = parts.dwHostNameLength;
	LPCWSTR name = parts.lpszHostName;
	if (size > 2 && name[0] == L'[' && name[size - 1] == L']')
	{
		name++;
		size -= 2;
	}
	if (!size || size >= count)
		return FALSE;
	memcpy(host, name, size * sizeof(WCHAR));
	host[size] = L'\0';
	return TRUE;
}

static PR_STRING find_bits_creator(const GUID *job)
{
	WCHAR id[40], xpath[256];
	if (!StringFromGUID2(job, id, RTL_NUMBER_OF(id)))
		return NULL;
	CharLowerBuffW(id, (DWORD)wcslen(id));
	swprintf_s(xpath, RTL_NUMBER_OF(xpath),
		L"*[System[EventID=3] and EventData[Data[@Name='jobId']='%s']]", id);
	EVT_HANDLE query = EvtQuery(NULL, bits_channel, xpath, EvtQueryChannelPath | EvtQueryReverseDirection);
	if (!query)
		return NULL;

	static LPCWSTR fields[] = { L"Event/EventData/Data[@Name='processPath']" };
	EVT_HANDLE render = EvtCreateRenderContext(RTL_NUMBER_OF(fields), fields, EvtRenderContextValues);
	PR_STRING creator = NULL;
	EVT_HANDLE event;
	DWORD received;
	if (render && EvtNext(query, 1, &event, 50, 0, &received))
	{
		EVT_VARIANT *values = render_event_values(render, event, RTL_NUMBER_OF(fields));
		LPCWSTR path = values ? audit_string(values) : NULL;
		if (path && path[0])
		{
			LPCWSTR slash = wcsrchr(path, L'\\');
			creator = _r_obj_createstring(slash ? slash + 1 : path);
		}
		free(values);
		EvtClose(event);
	}
	if (render)
		EvtClose(render);
	EvtClose(query);
	return creator;
}

PR_STRING _app_packet_bits_job_info(const PH_ROW *row)
{
	if (!row->direction || row->protocol != IPPROTO_TCP || row->sample_time <= 0 ||
		(row->af != AF_INET && row->af != AF_INET6))
		return NULL;

	static LPCWSTR fields[] = {
		L"Event/System/Execution/@ProcessID",
		L"Event/EventData/Data[@Name='name']",
		L"Event/EventData/Data[@Name='Id']",
		L"Event/EventData/Data[@Name='url']",
		L"Event/EventData/Data[@Name='proxy']",
		L"Event/EventData/Data[@Name='peer']"
	};
	WCHAR start[40], end[40], xpath[256];
	audit_time(row->sample_time - 2, start, RTL_NUMBER_OF(start));
	audit_time(row->sample_time + 3, end, RTL_NUMBER_OF(end));
	swprintf_s(xpath, RTL_NUMBER_OF(xpath),
		L"*[System[(EventID=59 or EventID=60 or EventID=61) and TimeCreated[@SystemTime >= '%s' and @SystemTime < '%s']]]",
		start, end);

	EVT_HANDLE query = EvtQuery(NULL, bits_channel, xpath, EvtQueryChannelPath | EvtQueryReverseDirection);
	if (!query)
		return NULL;

	EVT_HANDLE render = EvtCreateRenderContext(RTL_NUMBER_OF(fields), fields, EvtRenderContextValues);
	WCHAR title[257] = L"", hostname[256] = L"";
	GUID job = {0};
	BOOL complete = FALSE;
	if (render)
	{
		EVT_HANDLE event;
		DWORD received;
		for (ULONG scanned = 0; scanned < 64; scanned++)
		{
			if (!EvtNext(query, 1, &event, 50, 0, &received))
			{
				complete = GetLastError() == ERROR_NO_MORE_ITEMS;
				break;
			}
			EVT_VARIANT *values = render_event_values(render, event, RTL_NUMBER_OF(fields));
			EvtClose(event);
			if (!values)
				continue;

			const GUID *id = values[2].Type == EvtVarTypeGuid ? values[2].GuidVal : NULL;
			LPCWSTR name = audit_string(&values[1]), proxy = audit_string(&values[4]), peer = audit_string(&values[5]);
			WCHAR host[RTL_NUMBER_OF(hostname)];
			if ((!row->pid || row->pid == audit_number(&values[0])) && id && name && name[0] &&
				(!proxy || !proxy[0]) && (!peer || !peer[0]) &&
				bits_url_host(row, audit_string(&values[3]), host, RTL_NUMBER_OF(host)) &&
				((hostname[0] && _wcsicmp(hostname, host) == 0) || bits_host_matches(row, host)))
			{
				if (title[0] && !same_bits_job(&job, hostname, id, host))
				{
					free(values);
					break;
				}
				job = *id;
				wcsncpy_s(title, RTL_NUMBER_OF(title), name, _TRUNCATE);
				wcscpy_s(hostname, RTL_NUMBER_OF(hostname), host);
			}
			free(values);
		}
		EvtClose(render);
	}
	EvtClose(query);
	if (!complete || !title[0])
		return NULL;

	PR_STRING creator = find_bits_creator(&job);
	PR_STRING info = _r_format_string(L"\n\nBITS job: %s\nRequested host: %s%s%s", title, hostname,
		creator ? L"\nQueued by: " : L"", creator ? creator->buffer : L"");
	if (creator)
		_r_obj_dereference(creator);
	return info;
}

static DWORD audit_event_pid(const PH_ROW *row, LPCWSTR path, const EVT_VARIANT *values)
{
	ULONGLONG pid = audit_number(&values[0]);
	LPCWSTR image = audit_string(&values[1]);
	LPCWSTR direction = audit_string(&values[2]);
	if (!pid || pid > MAXDWORD || !image || !direction ||
		wcscmp(direction, row->direction ? L"%%14593" : L"%%14592") != 0 ||
		audit_number(&values[7]) != (ULONGLONG)row->protocol ||
		audit_number(&values[8]) != (ULONGLONG)row->filter)
		return 0;

	const EVT_VARIANT *local = &values[row->direction ? 3 : 5];
	const EVT_VARIANT *remote = &values[row->direction ? 5 : 3];
	if (!endpoint_matches(row->af, row->local, row->local_port, local, local + 1) ||
		!endpoint_matches(row->af, row->remote, row->remote_port, remote, remote + 1))
		return 0;

	if (_wcsicmp(image, path) == 0)
		return (DWORD)pid;

	R_STRINGREF native_path;
	_r_obj_initializestringref(&native_path, image);
	PR_STRING converted = _r_path_dospathfromnt(&native_path);
	BOOL matches = converted && _wcsicmp(converted->buffer, path) == 0;
	if (converted)
		_r_obj_dereference(converted);

	return matches ? (DWORD)pid : 0;
}

static BOOL read_audit_pids(const PH_ROW *rows, size_t count, LPCWSTR path, DWORD *pids)
{
	static LPCWSTR fields[] = {
		L"Event/EventData/Data[@Name='ProcessID']",
		L"Event/EventData/Data[@Name='Application']",
		L"Event/EventData/Data[@Name='Direction']",
		L"Event/EventData/Data[@Name='SourceAddress']",
		L"Event/EventData/Data[@Name='SourcePort']",
		L"Event/EventData/Data[@Name='DestAddress']",
		L"Event/EventData/Data[@Name='DestPort']",
		L"Event/EventData/Data[@Name='Protocol']",
		L"Event/EventData/Data[@Name='FilterRTID']",
		L"Event/System/TimeCreated/@SystemTime"
	};
	int64_t oldest = rows[0].sample_time, newest = oldest;
	for (size_t i = 1; i < count; i++)
	{
		oldest = min(oldest, rows[i].sample_time);
		newest = max(newest, rows[i].sample_time);
	}

	WCHAR start[40], end[40], xpath[768], predicate[MAX_PATH + 96];
	audit_time(oldest - 1, start, RTL_NUMBER_OF(start));
	audit_time(newest + 2, end, RTL_NUMBER_OF(end));
	if (count == 1)
	{
		swprintf_s(predicate, RTL_NUMBER_OF(predicate),
			L"Data[@Name='FilterRTID']='%llu' and Data[@Name='SourcePort']='%u' and Data[@Name='DestPort']='%u'",
			(ULONGLONG)rows->filter, rows->direction ? rows->local_port : rows->remote_port,
			rows->direction ? rows->remote_port : rows->local_port);
	}
	else
	{
		FWP_BYTE_BLOB *app_id = NULL;
		if (FwpmGetAppIdFromFileName0(path, &app_id) != ERROR_SUCCESS || !app_id)
			return FALSE;

		LPCWSTR image = (LPCWSTR)app_id->data;
		BOOL valid = image && app_id->size && app_id->size <= MAX_PATH * sizeof(WCHAR) &&
			!wcschr(image, L'\'');
		if (valid)
			swprintf_s(predicate, RTL_NUMBER_OF(predicate), L"Data[@Name='Application']='%s'", image);

		FwpmFreeMemory0((PVOID_PTR)&app_id);
		if (!valid)
			return FALSE;
	}
	swprintf_s(xpath, RTL_NUMBER_OF(xpath),
		L"*[System[EventID=5157 and TimeCreated[@SystemTime >= '%s' and @SystemTime < '%s']] "
		L"and EventData[%s]]", start, end, predicate);

	EVT_HANDLE query = EvtQuery(NULL, L"Security", xpath, EvtQueryChannelPath | EvtQueryReverseDirection);
	if (!query)
		return FALSE;

	EVT_HANDLE render = EvtCreateRenderContext(RTL_NUMBER_OF(fields), fields, EvtRenderContextValues);
	BOOL complete = FALSE;
	if (render)
	{
		DWORD received;
		EVT_HANDLE event;
		ULONG scanned = 0;
		// A busy Security log must not hold up the history writer indefinitely.
		while (scanned < 256)
		{
			if (!EvtNext(query, 1, &event, 50, 0, &received))
			{
				complete = GetLastError() == ERROR_NO_MORE_ITEMS;
				break;
			}
			scanned++;
			EVT_VARIANT *values = render_event_values(render, event, RTL_NUMBER_OF(fields));
			if (values && values[9].Type == EvtVarTypeFileTime)
			{
				ULARGE_INTEGER ticks = { .QuadPart = values[9].FileTimeVal };
				FILETIME time = { ticks.LowPart, ticks.HighPart };
				int64_t sample = _r_unixtime_from_filetime(&time);
				for (size_t i = 0; i < count; i++)
				{
					if (sample < rows[i].sample_time - 1 || sample > rows[i].sample_time + 1 || pids[i] == MAXDWORD)
						continue;
					DWORD candidate = audit_event_pid(&rows[i], path, values);
					if (candidate)
						pids[i] = pids[i] && pids[i] != candidate ? MAXDWORD : candidate;
				}
			}
			free(values);
			EvtClose(event);
		}
		EvtClose(render);
	}
	EvtClose(query);
	return complete;
}

static DWORD find_audit_pid(const PH_ROW *row, LPCWSTR path, PBOOLEAN ambiguous)
{
	DWORD pid = 0;
	if (!read_audit_pids(row, 1, path, &pid) || pid == MAXDWORD)
	{
		*ambiguous = pid != 0;
		return 0;
	}
	return pid;
}

static HANDLE open_packet_process(DWORD pid, LPCWSTR path, ULONGLONG packet_time, FILETIME *started)
{
	HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
	if (!process)
		return NULL;

	WCHAR image[MAX_PATH];
	DWORD length = RTL_NUMBER_OF(image);
	DWORD exit_code;
	FILETIME exited, kernel, user;
	if (QueryFullProcessImageNameW(process, 0, image, &length) &&
		GetProcessTimes(process, started, &exited, &kernel, &user) &&
		GetExitCodeProcess(process, &exit_code) && exit_code == STILL_ACTIVE &&
		process_matches_packet(image, path, filetime_value(*started), packet_time))
		return process;

	CloseHandle(process);
	return NULL;
}

static LPCWSTR unique_pid_service(DWORD pid, const ENUM_SERVICE_STATUS_PROCESS *services, ULONG count)
{
	LPCWSTR name = NULL;
	for (ULONG i = 0; i < count; i++)
	{
		const SERVICE_STATUS_PROCESS *status = &services[i].ServiceStatusProcess;
		if (status->dwProcessId != pid || status->dwCurrentState == SERVICE_STOPPED)
			continue;

		if (name || status->dwCurrentState != SERVICE_RUNNING)
			return NULL;
		name = services[i].lpServiceName;
	}
	return name;
}

void _app_packet_resolve_audit(PH_ROW *rows, size_t count)
{
	LPCWSTR path = _r_obj_getstring(config.svchost_path);
	DWORD pids[PH_AUDIT_BATCH] = {0};
	if (!path || !count || count > RTL_NUMBER_OF(pids) || !read_audit_pids(rows, count, path, pids))
		return;

	LPENUM_SERVICE_STATUS_PROCESS services = NULL;
	ULONG total = 0;
	BOOL queried_services = FALSE;
	for (size_t i = 0; i < count; i++)
	{
		if (!pids[i] || pids[i] == MAXDWORD)
			continue;

		PH_ROW *row = &rows[i];
		FILETIME packet_time, started;
		row->pid = pids[i];
		_r_unixtime_to_filetime(&packet_time, row->sample_time);
		HANDLE process = open_packet_process(row->pid, path, filetime_value(packet_time), &started);
		if (!process)
			continue;

		row->process_started = _r_unixtime_from_filetime(&started);
		if (!queried_services)
		{
			services = _app_package_enumservices(NULL, TRUE, &total);
			queried_services = TRUE;
		}
		LPCWSTR service = services ? unique_pid_service(row->pid, services, total) : NULL;
		if (service)
			WideCharToMultiByte(CP_UTF8, 0, service, -1, row->service, sizeof(row->service), NULL, NULL);
		CloseHandle(process);
	}

	if (services)
		_r_mem_free(services);
}

static void append_pid_services(PR_STRINGBUILDER text, DWORD pid, const ENUM_SERVICE_STATUS_PROCESS *services, ULONG total)
{
	ULONG count = 0;
	for (ULONG i = 0; i < total; i++)
	{
		if (services[i].ServiceStatusProcess.dwProcessId != pid ||
			services[i].ServiceStatusProcess.dwCurrentState != SERVICE_RUNNING)
			continue;

		_r_obj_appendstringbuilderformat(text, count ? L", %s" : L"   %s", services[i].lpServiceName);
		if (++count == 8)
		{
			_r_obj_appendstringbuilder(text, L", …");
			break;
		}
	}
}

PR_STRING _app_packet_process_info(const PH_ROW *row, LPCWSTR path)
{
	R_STRINGBUILDER text;
	_r_obj_initializestringbuilder(&text, 512);
	BOOLEAN ambiguous = FALSE;
	DWORD pid = row->pid ? row->pid : find_audit_pid(row, path, &ambiguous);
	FILETIME packet_time, started;
	_r_unixtime_to_filetime(&packet_time, row->sample_time);
	ULONGLONG timestamp = filetime_value(packet_time);
	ULONG total = 0;
	LPENUM_SERVICE_STATUS_PROCESS services = _app_package_enumservices(NULL, TRUE, &total);
	if (!services)
		total = 0;

	if (row->count > 1)
		_r_obj_appendstringbuilder(&text, L"Inspecting one recorded connection from this group.\n\n");

	if (pid)
	{
		_r_obj_appendstringbuilderformat(&text, L"Windows recorded PID %lu for this connection.\n%s", pid, path);
		HANDLE process = open_packet_process(pid, path, timestamp, &started);
		if (process)
		{
			PR_STRING date = _r_format_unixtime(_r_unixtime_from_filetime(&started), FDTF_SHORTDATE | FDTF_LONGTIME);
			if (date)
				_r_obj_appendstringbuilderformat(&text, L"\nStarted %s, before the packet.", date->buffer);
			else
				_r_obj_appendstringbuilder(&text, L"\nThe process started before the packet.");
			if (date)
				_r_obj_dereference(date);
			_r_obj_appendstringbuilder(&text, L"\n\nCurrent service names, if available");
			append_pid_services(&text, pid, services, total);
			CloseHandle(process);
		}
		else
			_r_obj_appendstringbuilder(&text, L"\nThe live PID could not be verified against the recorded executable and start time.");
	}
	else
	{
		_r_obj_appendstringbuilder(&text, ambiguous ?
			L"The Windows Security log did not identify a unique PID." :
			L"No matching blocked-connection event was found in the Windows Security log.");
		PSYSTEM_PROCESS_INFORMATION processes;
		if (NT_SUCCESS(_r_sys_enumprocesses(&processes)))
		{
			INT count = 0;
			for (PSYSTEM_PROCESS_INFORMATION item = PR_FIRST_PROCESS(processes); item; item = PR_NEXT_PROCESS(item))
			{
				if (!item->ImageName.Buffer ||
					CompareStringOrdinal(item->ImageName.Buffer, item->ImageName.Length / sizeof(WCHAR),
						L"svchost.exe", -1, TRUE) != CSTR_EQUAL)
					continue;

				DWORD candidate = HandleToULong(item->UniqueProcessId);
				HANDLE process = open_packet_process(candidate, path, timestamp, &started);
				if (!process)
					continue;

				if (!count)
					_r_obj_appendstringbuilder(&text, L"\n\nRunning candidates that started before the packet");
				if (++count <= 8)
				{
					_r_obj_appendstringbuilderformat(&text, L"\nPID %lu", candidate);
					append_pid_services(&text, candidate, services, total);
				}
				CloseHandle(process);
			}
			if (count > 8)
				_r_obj_appendstringbuilderformat(&text, L"\n%d more candidates", count - 8);
			_r_mem_free(processes);
		}
	}

	if (services)
		_r_mem_free(services);
	// The caller takes ownership of the builder's string.
	return _r_obj_finalstringbuilder(&text);
}
