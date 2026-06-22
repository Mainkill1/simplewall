// simplewall
// Copyright (c) 2016-2026 Henry++

#include "global.h"

static R_INITONCE _app_log_initonce = PR_INITONCE_INIT;
static R_STRINGBUILDER _app_log_buffer = {0};
static PR_LIST _app_log_ui_list = NULL;
static R_QUEUED_LOCK _app_log_buffer_lock = PR_QUEUED_LOCK_INIT;
static R_QUEUED_LOCK _app_log_file_lock = PR_QUEUED_LOCK_INIT;
static R_QUEUED_LOCK _app_log_ui_lock = PR_QUEUED_LOCK_INIT;
static volatile LONG _app_log_flush_queued = 0;
static volatile LONG _app_profile_save_queued = 0;
static volatile LONG _app_log_ui_flush_queued = 0;

static VOID _app_loginitializebuffer ()
{
	if (_r_initonce_begin (&_app_log_initonce))
	{
		_r_obj_initializestringbuilder (&_app_log_buffer, _r_calc_kilobytes2bytes (LOG_BUFFER_LIMIT_DEFAULT));

		_app_log_ui_list = _r_obj_createlist (64, &_r_obj_dereference);

		_r_initonce_end (&_app_log_initonce);
	}
}

static ULONG _app_loggetbufferlimit ()
{
	ULONG limit_kb;

	limit_kb = _r_config_getulong (L"LogBufferLimitKb", LOG_BUFFER_LIMIT_DEFAULT, NULL);

	if (limit_kb < 4)
		return 4;

	if (limit_kb > 4096)
		return 4096;

	return limit_kb;
}

static ULONG _app_loggetflushtimeout ()
{
	ULONG timeout_ms;

	timeout_ms = _r_config_getulong (L"LogFlushTimeoutMs", LOG_FLUSH_TIMEOUT_DEFAULT, NULL);

	if (timeout_ms < 100)
		return 100;

	if (timeout_ms > 60000)
		return 60000;

	return timeout_ms;
}

static BOOLEAN _app_logislimitreached2 (
	_In_ HANDLE hfile,
	_In_ ULONG_PTR incoming_length
)
{
	LONG64 file_size = 0;
	LONG64 limit;

	limit = _r_config_getlong64 (L"LogSizeLimitKb", LOG_SIZE_LIMIT_DEFAULT, NULL);

	if (!limit)
		return FALSE;

	_r_fs_getsize (NULL, hfile, &file_size);

	return ((file_size + (LONG64)incoming_length) >= _r_calc_kilobytes2bytes64 (limit));
}

static VOID _app_logclear_nolock (
	_In_opt_ HANDLE hfile
)
{
	PR_STRING log_path;

	if (hfile)
	{
		_r_fs_clearfile (hfile);

		_app_loginitfile (hfile);

		return;
	}

	log_path = _app_getlogpath ();

	if (!log_path)
		return;

	_r_fs_deletefile (&log_path->sr, NULL);

	_r_obj_dereference (log_path);
}

static _Ret_maybenull_ PR_STRING _app_logdetachbuffer ()
{
	PR_STRING buffer = NULL;

	_app_loginitializebuffer ();

	_r_queuedlock_acquireexclusive (&_app_log_buffer_lock);

	if (_app_log_buffer.string && _app_log_buffer.string->length)
	{
		buffer = _app_log_buffer.string;

		_r_obj_initializestringbuilder (&_app_log_buffer, _r_calc_kilobytes2bytes (_app_loggetbufferlimit ()));
	}

	_r_queuedlock_releaseexclusive (&_app_log_buffer_lock);

	return buffer;
}

static VOID _app_logwritebuffer (
	_In_ PR_STRING buffer
)
{
	HANDLE current_handle;

	if (_r_obj_isstringempty (buffer))
		return;

	_r_queuedlock_acquireexclusive (&_app_log_file_lock);

	current_handle = _InterlockedCompareExchangePointer ((volatile PVOID_PTR)&config.hlogfile, NULL, NULL);

	if (current_handle)
	{
		if (_app_logislimitreached2 (current_handle, buffer->length))
			_app_logclear_nolock (current_handle);

		_r_fs_writefile (current_handle, buffer->buffer, (ULONG)buffer->length);
	}

	_r_queuedlock_releaseexclusive (&_app_log_file_lock);
}

static VOID _app_logflushbuffer ()
{
	PR_STRING buffer;

	buffer = _app_logdetachbuffer ();

	if (!buffer)
		return;

	_app_logwritebuffer (buffer);

	_r_obj_dereference (buffer);
}

static VOID NTAPI _app_logflushthread (
	_In_opt_ PVOID arglist
)
{
	_app_logflushbuffer ();
}

static VOID NTAPI _app_logflushdelaythread (
	_In_opt_ PVOID arglist
)
{
	_r_sys_sleep (_app_loggetflushtimeout ());

	_InterlockedExchange (&_app_log_flush_queued, 0);

	_r_workqueue_queueitem (&log_writer_queue, &_app_logflushthread, NULL);
}

static VOID _app_logqueueflush (
	_In_ BOOLEAN is_force
)
{
	if (is_force)
	{
		_r_workqueue_queueitem (&log_writer_queue, &_app_logflushthread, NULL);

		return;
	}

	if (_InterlockedCompareExchange (&_app_log_flush_queued, 1, 0) == 0)
		_r_workqueue_queueitem (&log_queue, &_app_logflushdelaythread, NULL);
}

static VOID _app_logappendbuffer (
	_In_ PR_STRING buffer
)
{
	ULONG_PTR limit;
	ULONG_PTR length;

	if (_r_obj_isstringempty (buffer))
		return;

	_app_loginitializebuffer ();

	limit = _r_calc_kilobytes2bytes ((LONG64)_app_loggetbufferlimit ());

	_r_queuedlock_acquireexclusive (&_app_log_buffer_lock);

	_r_obj_appendstringbuilder2 (&_app_log_buffer, &buffer->sr);

	length = _app_log_buffer.string->length;

	_r_queuedlock_releaseexclusive (&_app_log_buffer_lock);

	_app_logqueueflush (length >= limit);
}

static VOID NTAPI _app_profilesavethread (
	_In_opt_ PVOID arglist
)
{
	HWND hwnd;

	_r_sys_sleep (2000);

	_InterlockedExchange (&_app_profile_save_queued, 0);

	hwnd = _r_app_gethwnd ();

	if (hwnd)
		_app_profile_save (hwnd);
}

VOID _app_profile_save_deferred ()
{
	if (_InterlockedCompareExchange (&_app_profile_save_queued, 1, 0) == 0)
		_r_workqueue_queueitem (&log_queue, &_app_profilesavethread, NULL);
}

VOID _app_loginit (
	_In_ BOOLEAN is_install
)
{
	HANDLE current_handle;
	HANDLE new_handle;
	PR_STRING log_path;
	NTSTATUS status;

	_app_logflushbuffer ();

	_r_queuedlock_acquireexclusive (&_app_log_file_lock);

	current_handle = config.hlogfile;
	config.hlogfile = NULL;

	// reset log handle
	if (current_handle)
		NtClose (current_handle);

	if (!is_install || !_r_config_getboolean (L"IsLogEnabled", FALSE, NULL))
	{
		_r_queuedlock_releaseexclusive (&_app_log_file_lock);

		return; // already closed or not enabled
	}

	log_path = _app_getlogpath ();

	if (!log_path)
	{
		_r_queuedlock_releaseexclusive (&_app_log_file_lock);

		return;
	}

	status = _r_fs_createfile (&new_handle, &log_path->sr, FILE_OPEN_IF, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, FILE_ATTRIBUTE_NORMAL, 0, FALSE, NULL);

	if (NT_SUCCESS (status))
	{
		_app_loginitfile (new_handle);

		config.hlogfile = new_handle;
	}

	_r_queuedlock_releaseexclusive (&_app_log_file_lock);

	_r_obj_dereference (log_path);
}

VOID _app_loginitfile (
	_In_ HANDLE hfile
)
{
	BYTE bom[] = {0xFF, 0xFE};
	LONG64 file_size;

	_r_fs_getsize (NULL, hfile, &file_size);

	if (!file_size)
	{
		// write utf-16 le byte order mask
		_r_fs_writefile (hfile, bom, sizeof (bom));

		// write csv header
		_r_fs_writefile (hfile, SZ_LOG_TITLE, (ULONG)(_r_str_getlength (SZ_LOG_TITLE) * sizeof (WCHAR)));
	}
	else
	{
		// move to eof
		_r_fs_setpos (hfile, file_size);
	}
}

ULONG _app_getloghash (
	_In_ PITEM_LOG ptr_log
)
{
	PR_STRING string;
	ULONG log_hash;

	string = _r_format_string (
		L"%" TEXT (PRIu8) L"_%" TEXT (PR_ULONG) L"_%" TEXT (PRIu8) L"_%" \
		TEXT (PRIu8) L"_%" TEXT (PRIu16) L"_%" TEXT (PRIu16) L"_%s_%s_IJAHudm4*^(uaokUH_!&%#!",
		ptr_log->af,
		ptr_log->app_hash,
		ptr_log->protocol,
		ptr_log->direction,
		ptr_log->local_port,
		ptr_log->remote_port,
		_r_obj_getstring (ptr_log->local_addr_str),
		_r_obj_getstring (ptr_log->remote_addr_str)
	);

	log_hash = _r_str_gethash (&string->sr, TRUE);

	_r_obj_dereference (string);

	return log_hash;
}

_Ret_maybenull_
PR_STRING _app_getlogpath ()
{
	PR_STRING path;

	path = _r_config_getstringexpand (L"LogPath", LOG_PATH_DEFAULT, NULL);

	return path;
}

_Ret_maybenull_
PR_STRING _app_getlogviewer ()
{
	R_STRINGREF sr;
	PR_STRING path;

	path = _r_config_getstringexpand (L"LogViewer", LOG_VIEWER_DEFAULT, NULL);

	if (!path || !_r_fs_isexists (&path->sr))
	{
		if (path)
			_r_obj_dereference (path);

		_r_obj_initializestringref (&sr, LOG_VIEWER_DEFAULT);

		_r_str_environmentexpandstring (NULL, &sr, &path);
	}

	return path;
}

BOOLEAN _app_islogfound (
	_In_ ULONG log_hash
)
{
	BOOLEAN is_found;

	_r_queuedlock_acquireshared (&lock_loglist);
	is_found = (_r_obj_findhashtable (log_table, log_hash) != NULL);
	_r_queuedlock_releaseshared (&lock_loglist);

	return is_found;
}

BOOLEAN _app_logislimitreached (
	_In_ HANDLE hfile
)
{
	LONG64 file_size;
	LONG64 limit;

	limit = _r_config_getlong64 (L"LogSizeLimitKb", LOG_SIZE_LIMIT_DEFAULT, NULL);

	if (!limit)
		return FALSE;

	_r_fs_getsize (NULL, hfile, &file_size);

	return (file_size >= (_r_calc_kilobytes2bytes64 (limit)));
}

VOID _app_logclear (
	_In_opt_ HANDLE hfile
)
{
	_r_queuedlock_acquireexclusive (&_app_log_file_lock);

	_app_logclear_nolock (hfile);

	_r_queuedlock_releaseexclusive (&_app_log_file_lock);
}

VOID _app_logclear_ui (
	_In_ HWND hwnd
)
{
	_InterlockedExchange (&config.log_id, 0);

	_r_listview_deleteallitems (hwnd, IDC_LOG);

	_app_listview_resize (hwnd, IDC_LOG, FALSE);

	_r_queuedlock_acquireexclusive (&lock_loglist);
	_r_obj_clearhashtable (log_table);
	_r_queuedlock_releaseexclusive (&lock_loglist);
}

VOID _app_logwrite (
	_In_ PITEM_LOG ptr_log
)
{
	PR_STRING remote_port_string;
	PR_STRING local_port_string;
	PR_STRING direction_string;
	PR_STRING date_string;
	PR_STRING path = NULL;
	PR_STRING buffer;
	PITEM_APP ptr_app;
	HANDLE current_handle;

	current_handle = _InterlockedCompareExchangePointer ((volatile PVOID_PTR)&config.hlogfile, NULL, NULL);

	if (!current_handle)
		return;

	// parse path
	ptr_app = _app_getappitem (ptr_log->app_hash);

	if (ptr_app)
	{
		path = _app_getapppath (ptr_app, _r_config_getboolean (L"IsShortPath", TRUE, NULL));

		_r_obj_dereference (ptr_app);
	}

	date_string = _r_format_unixtime (ptr_log->timestamp, FDTF_SHORTDATE | FDTF_LONGTIME);

	local_port_string = _app_formatport (ptr_log->local_port, ptr_log->protocol);
	remote_port_string = _app_formatport (ptr_log->remote_port, ptr_log->protocol);

	direction_string = _app_db_getdirectionname (ptr_log->direction, ptr_log->is_loopback, FALSE);

	buffer = _r_format_string (
		SZ_LOG_BODY,
		_r_obj_getstringordefault (date_string, _r_locale_getstring (IDS_STATUS_EMPTY)),
		_r_obj_getstringordefault (ptr_log->username, _r_locale_getstring (IDS_STATUS_EMPTY)),
		_r_obj_getstringordefault (path, _r_locale_getstring (IDS_STATUS_EMPTY)),
		_r_obj_getstringordefault (ptr_log->local_addr_str, _r_locale_getstring (IDS_STATUS_EMPTY)),
		local_port_string->buffer,
		_r_obj_getstringordefault (ptr_log->remote_addr_str, _r_locale_getstring (IDS_STATUS_EMPTY)),
		remote_port_string->buffer,
		_r_obj_getstringordefault (ptr_log->protocol_str, SZ_DIRECTION_ANY),
		_r_obj_getstringordefault (ptr_log->layer_name, _r_locale_getstring (IDS_STATUS_EMPTY)),
		_r_obj_getstringordefault (ptr_log->filter_name, _r_locale_getstring (IDS_STATUS_EMPTY)),
		ptr_log->filter_id,
		_r_obj_getstringordefault (direction_string, _r_locale_getstring (IDS_STATUS_EMPTY)),
		(ptr_log->is_allow ? SZ_STATE_ALLOW : SZ_STATE_BLOCK)
	);

	_app_logappendbuffer (buffer);

	if (date_string)
		_r_obj_dereference (date_string);

	if (direction_string)
		_r_obj_dereference (direction_string);

	if (path)
		_r_obj_dereference (path);

	_r_obj_dereference (local_port_string);
	_r_obj_dereference (remote_port_string);

	_r_obj_dereference (buffer);
}

VOID _app_logwrite_ui (
	_In_ HWND hwnd,
	_In_ PITEM_LOG ptr_log
)
{
	ULONG_PTR table_size;
	ULONG log_hash;
	ULONG hash_code;
	INT item_id;

	log_hash = _app_getloghash (ptr_log);

	if (!log_hash)
		return;

	// log was already in list, so skip it...
	if (_app_islogfound (log_hash))
		return;

	_r_queuedlock_acquireshared (&lock_loglist);
	table_size = _r_obj_gethashtablesize (log_table);
	_r_queuedlock_releaseshared (&lock_loglist);

	if (table_size >= MAP_CACHE_MAX)
	{
		item_id = _r_listview_getitemcount (hwnd, IDC_LOG) - 1;

		hash_code = (ULONG)_app_listview_getitemcontext (hwnd, IDC_LOG, item_id);

		_r_listview_deleteitem (hwnd, IDC_LOG, item_id);

		if (hash_code)
		{
			_r_queuedlock_acquireexclusive (&lock_loglist);
			_r_obj_removehashtableitem (log_table, hash_code);
			_r_queuedlock_releaseexclusive (&lock_loglist);
		}
	}

	_r_queuedlock_acquireexclusive (&lock_loglist);
	_r_obj_addhashtablepointer (log_table, log_hash, _r_obj_reference (ptr_log));
	_r_queuedlock_releaseexclusive (&lock_loglist);

	_app_listview_addlogitem (hwnd, ptr_log, log_hash);

	_app_queue_resolver (hwnd, IDC_LOG, log_hash, ptr_log);

	_app_listview_updateby_id (hwnd, IDC_LOG, 0);
}

VOID _app_logwrite_ui_async (
	_In_ HWND hwnd,
	_In_ PITEM_LOG ptr_log
)
{
	BOOLEAN is_added = FALSE;

	if (!hwnd || !ptr_log)
		return;

	_app_loginitializebuffer ();

	_r_queuedlock_acquireexclusive (&_app_log_ui_lock);

	if (_r_obj_getlistsize (_app_log_ui_list) < MAP_CACHE_MAX)
	{
		_r_obj_addlistitem (_app_log_ui_list, _r_obj_reference (ptr_log), NULL);

		is_added = TRUE;
	}

	_r_queuedlock_releaseexclusive (&_app_log_ui_lock);

	if (is_added && _InterlockedCompareExchange (&_app_log_ui_flush_queued, 1, 0) == 0)
	{
		if (!PostMessageW (hwnd, WM_LOG_UPDATED, 0, 0))
			_InterlockedExchange (&_app_log_ui_flush_queued, 0);
	}
}

VOID _app_logwrite_ui_flush (
	_In_ HWND hwnd
)
{
	ULONG_PTR batch_count;
	ULONG_PTR list_size;
	PR_LIST list;
	PITEM_LOG ptr_log;

	_app_loginitializebuffer ();

	_r_queuedlock_acquireexclusive (&_app_log_ui_lock);

	list = _app_log_ui_list;
	_app_log_ui_list = _r_obj_createlist (64, &_r_obj_dereference);

	list_size = _r_obj_getlistsize (list);
	batch_count = min (list_size, LOG_UI_FLUSH_BATCH);

	for (ULONG_PTR i = batch_count; i < list_size && _r_obj_getlistsize (_app_log_ui_list) < MAP_CACHE_MAX; i++)
	{
		ptr_log = _r_obj_getlistitem (list, i);

		if (ptr_log)
			_r_obj_addlistitem (_app_log_ui_list, _r_obj_reference (ptr_log), NULL);
	}

	_r_queuedlock_releaseexclusive (&_app_log_ui_lock);

	_InterlockedExchange (&_app_log_ui_flush_queued, 0);

	for (ULONG_PTR i = 0; i < batch_count; i++)
	{
		ptr_log = _r_obj_getlistitem (list, i);

		if (ptr_log)
			_app_logwrite_ui (hwnd, ptr_log);
	}

	_r_obj_dereference (list);

	_r_queuedlock_acquireshared (&_app_log_ui_lock);

	if (!_r_obj_isempty (_app_log_ui_list))
	{
		if (_InterlockedCompareExchange (&_app_log_ui_flush_queued, 1, 0) == 0)
			PostMessageW (hwnd, WM_LOG_UPDATED, 0, 0);
	}

	_r_queuedlock_releaseshared (&_app_log_ui_lock);
}

VOID _wfp_logsubscribe (
	_In_opt_ HWND hwnd,
	_In_ HANDLE engine_handle
)
{
	static R_INITONCE init_once = PR_INITONCE_INIT;
	static FWPMNES4 _FwpmNetEventSubscribe4 = NULL;
	static FWPMNES3 _FwpmNetEventSubscribe3 = NULL;
	static FWPMNES2 _FwpmNetEventSubscribe2 = NULL;
	static FWPMNES1 _FwpmNetEventSubscribe1 = NULL;

	FWPM_NET_EVENT_ENUM_TEMPLATE0 enum_template = {0};
	FWPM_NET_EVENT_SUBSCRIPTION0 subscription = {0};
	HANDLE new_handle = NULL;
	HANDLE current_handle;
	PVOID hfwpuclnt;
	BOOLEAN is_success = FALSE;
	NTSTATUS status;

	current_handle = _InterlockedCompareExchangePointer ((volatile PVOID_PTR)&config.hnetevent, NULL, NULL);

	if (current_handle)
		return; // already subscribed

	if (_r_initonce_begin (&init_once))
	{
		status = _r_sys_loadlibrary2 (&hfwpuclnt, L"fwpuclnt.dll", 0);

		if (NT_SUCCESS (status))
		{
			_FwpmNetEventSubscribe4 = (FWPMNES4)_r_sys_getprocaddress (hfwpuclnt, "FwpmNetEventSubscribe4", 0);
			_FwpmNetEventSubscribe3 = (FWPMNES3)_r_sys_getprocaddress (hfwpuclnt, "FwpmNetEventSubscribe3", 0);
			_FwpmNetEventSubscribe2 = (FWPMNES2)_r_sys_getprocaddress (hfwpuclnt, "FwpmNetEventSubscribe2", 0);
			_FwpmNetEventSubscribe1 = (FWPMNES1)_r_sys_getprocaddress (hfwpuclnt, "FwpmNetEventSubscribe1", 0);

			_r_sys_freelibrary (hfwpuclnt);
		}
		else
		{
			if (hwnd)
			{
				_r_show_errormessage (hwnd, L"Could not load \"fwpuclnt.dll\" library!", status, NULL, ET_NATIVE);
			}
			else
			{
				_r_log (LOG_LEVEL_WARNING, NULL, L"_r_sys_loadlibrary", status, L"fwpuclnt.dll");
			}
		}

		_r_initonce_end (&init_once);
	}

	subscription.enumTemplate = &enum_template;
	subscription.sessionKey = config.session_key;

	// win10rs3+
	if (!is_success && _FwpmNetEventSubscribe4)
	{
		status = _FwpmNetEventSubscribe4 (engine_handle, &subscription, &_wfp_logcallback4, NULL, &new_handle);

		is_success = (status == STATUS_SUCCESS);
	}

	// win10rs1+
	if (!is_success && _FwpmNetEventSubscribe3)
	{
		status = _FwpmNetEventSubscribe3 (engine_handle, &subscription, &_wfp_logcallback3, NULL, &new_handle);

		is_success = (status == STATUS_SUCCESS);
	}

	// win10rs1+
	if (!is_success && _FwpmNetEventSubscribe2)
	{
		status = _FwpmNetEventSubscribe2 (engine_handle, &subscription, &_wfp_logcallback2, NULL, &new_handle);

		is_success = (status == STATUS_SUCCESS);
	}

	// win8+
	if (!is_success && _FwpmNetEventSubscribe1)
	{
		status = _FwpmNetEventSubscribe1 (engine_handle, &subscription, &_wfp_logcallback1, NULL, &new_handle);

		is_success = (status == STATUS_SUCCESS);
	}

	// win7+
	if (!is_success)
	{
		status = FwpmNetEventSubscribe0 (engine_handle, &subscription, &_wfp_logcallback0, NULL, &new_handle);

		is_success = (status == STATUS_SUCCESS);
	}

	if (!is_success)
	{
		if (hwnd && status != ERROR_PATH_NOT_FOUND)
		{
			_r_show_errormessage (hwnd, L"Log subscribe failed. Try again later!", status, NULL, ET_WINDOWS);
		}
		else
		{
			_r_log (LOG_LEVEL_WARNING, NULL, L"FwpmNetEventSubscribe", status, NULL);
		}
	}

	_r_toolbar_enablebutton (config.hrebar, IDC_TOOLBAR, IDM_TRAY_ENABLENOTIFICATIONS_CHK, is_success);
	_r_toolbar_enablebutton (config.hrebar, IDC_TOOLBAR, IDM_TRAY_ENABLELOG_CHK, is_success);
	_r_toolbar_enablebutton (config.hrebar, IDC_TOOLBAR, IDM_TRAY_ENABLEUILOG_CHK, is_success);

	current_handle = _InterlockedCompareExchangePointer ((volatile PVOID_PTR)&config.hnetevent, new_handle, NULL);

	if (current_handle)
	{
		if (new_handle)
			FwpmNetEventUnsubscribe0 (engine_handle, new_handle);
	}

	// initialize log file
	if (_r_config_getboolean (L"IsLogEnabled", FALSE, NULL))
		_app_loginit (TRUE);
}

VOID _wfp_logunsubscribe (
	_In_ HANDLE engine_handle
)
{
	HANDLE current_handle;
	ULONG status;

	_app_loginit (FALSE); // destroy log file handle if present

	current_handle = _InterlockedCompareExchangePointer ((volatile PVOID_PTR)&config.hnetevent, NULL, config.hnetevent);

	if (!current_handle)
		return;

	status = FwpmNetEventUnsubscribe0 (engine_handle, current_handle);

	if (status != ERROR_SUCCESS)
		_r_log (LOG_LEVEL_WARNING, NULL, L"FwpmNetEventUnsubscribe", status, NULL);
}

VOID _wfp_logsetoption (
	_In_ HANDLE engine_handle
)
{
	FWP_VALUE0 val;
	UINT32 mask = 0;
	ULONG status;

	// configure dropped packets logging (win8+)
	if (!_r_sys_isosversiongreaterorequal (WINDOWS_8))
		return;

	// add allowed connections monitor
	if (!_r_config_getboolean (L"IsExcludeClassifyAllow", TRUE, NULL))
		mask |= FWPM_NET_EVENT_KEYWORD_CLASSIFY_ALLOW;

	// add inbound multicast and broadcast connections monitor
	if (!_r_config_getboolean (L"IsExcludeInbound", FALSE, NULL))
		mask |= FWPM_NET_EVENT_KEYWORD_INBOUND_MCAST | FWPM_NET_EVENT_KEYWORD_INBOUND_BCAST;

	// add port scanning drop connections monitor (19H1+)
	if (_r_sys_isosversiongreaterorequal (WINDOWS_10_19H1))
	{
		if (!_r_config_getboolean (L"IsExcludePortScanningDrop", FALSE, NULL))
			mask |= FWPM_NET_EVENT_KEYWORD_PORT_SCANNING_DROP;
	}

	// the filter engine will collect wfp network events that match any supplied key words
	RtlZeroMemory (&val, sizeof (FWP_VALUE0));

	val.type = FWP_UINT32;
	val.uint32 = mask;

	status = FwpmEngineSetOption0 (engine_handle, FWPM_ENGINE_NET_EVENT_MATCH_ANY_KEYWORDS, &val);

	if (status != ERROR_SUCCESS)
		_r_log (LOG_LEVEL_WARNING, NULL, L"FwpmEngineSetOption0", status, L"FWPM_ENGINE_NET_EVENT_MATCH_ANY_KEYWORDS");

	// enables the connection monitoring feature and starts logging creation and deletion events (and notifying any subscribers)
	RtlZeroMemory (&val, sizeof (FWP_VALUE0));

	val.type = FWP_UINT32;
	val.uint32 = !_r_config_getboolean (L"IsExcludeIPSecConnections", FALSE, NULL);

	status = FwpmEngineSetOption0 (engine_handle, FWPM_ENGINE_MONITOR_IPSEC_CONNECTIONS, &val);

	if (status != ERROR_SUCCESS)
		_r_log (LOG_LEVEL_WARNING, NULL, L"FwpmEngineSetOption0", status, L"FWPM_ENGINE_MONITOR_IPSEC_CONNECTIONS");
}

static VOID _app_logfreecallback (
	_In_opt_ PITEM_LOG_CALLBACK log
)
{
	if (!log)
		return;

	if (log->app_id)
		_r_mem_free (log->app_id);

	if (log->package_id)
		_r_mem_free (log->package_id);

	if (log->user_id)
		_r_mem_free (log->user_id);

	_r_mem_free (log);
}

static PITEM_LOG_CALLBACK _app_logcopycallback (
	_In_ PITEM_LOG_CALLBACK log
)
{
	PITEM_LOG_CALLBACK log_copy;
	ULONG sid_length;

	log_copy = _r_mem_allocate (sizeof (ITEM_LOG_CALLBACK));

	RtlCopyMemory (log_copy, log, sizeof (ITEM_LOG_CALLBACK));

	log_copy->app_id = NULL;
	log_copy->package_id = NULL;
	log_copy->user_id = NULL;

	if ((log->flags & FWPM_NET_EVENT_FLAG_APP_ID_SET) == FWPM_NET_EVENT_FLAG_APP_ID_SET && log->app_id && log->app_id_length)
	{
		log_copy->app_id = _r_mem_allocate (log->app_id_length + sizeof (WCHAR));
		log_copy->app_id_length = log->app_id_length;

		RtlCopyMemory (log_copy->app_id, log->app_id, log->app_id_length);
	}
	else
	{
		log_copy->flags &= ~(FWPM_NET_EVENT_FLAG_APP_ID_SET);
		log_copy->app_id_length = 0;
	}

	if ((log->flags & FWPM_NET_EVENT_FLAG_PACKAGE_ID_SET) == FWPM_NET_EVENT_FLAG_PACKAGE_ID_SET && log->package_id && RtlValidSid (log->package_id))
	{
		sid_length = RtlLengthSid (log->package_id);

		log_copy->package_id = _r_mem_allocateandcopy (log->package_id, sid_length);
	}
	else
	{
		log_copy->flags &= ~(FWPM_NET_EVENT_FLAG_PACKAGE_ID_SET);
	}

	if ((log->flags & FWPM_NET_EVENT_FLAG_USER_ID_SET) == FWPM_NET_EVENT_FLAG_USER_ID_SET && log->user_id && RtlValidSid (log->user_id))
	{
		sid_length = RtlLengthSid (log->user_id);

		log_copy->user_id = _r_mem_allocateandcopy (log->user_id, sid_length);
	}
	else
	{
		log_copy->flags &= ~(FWPM_NET_EVENT_FLAG_USER_ID_SET);
	}

	return log_copy;
}

static VOID _app_logcallbackprocess (
	_In_ PITEM_LOG_CALLBACK log
)
{
	HANDLE engine_handle;
	PITEM_LOG ptr_log;
	GUID layer_guid;
	PR_STRING filter_name = NULL;
	PR_STRING sid_string = NULL;
	PR_STRING resolved_path;
	PR_STRING layer_name;
	PR_STRING path;
	FWPM_FILTER0 *filter_ptr;
	FWPM_LAYER0 *layer_ptr;
	UINT8 filter_weight = 0;
	BOOLEAN is_myprovider = FALSE;
	NTSTATUS status;

	if (!log->filter_id || !log->layer_id)
		return;

	if (log->is_allow && _r_config_getboolean (L"IsExcludeClassifyAllow", TRUE, NULL))
		return;

	engine_handle = _wfp_getenginehandle ();

	// do not parse when tcp connection has been established, or when non-tcp traffic has been authorized
	if (FwpmLayerGetById0 (engine_handle, log->layer_id, &layer_ptr) != ERROR_SUCCESS || !layer_ptr)
		return;

	if (IsEqualGUID (&layer_ptr->layerKey, &FWPM_LAYER_ALE_FLOW_ESTABLISHED_V4) || IsEqualGUID (&layer_ptr->layerKey, &FWPM_LAYER_ALE_FLOW_ESTABLISHED_V6))
	{
		FwpmFreeMemory0 ((PVOID_PTR)&layer_ptr);

		return;
	}

	if (IsEqualGUID (&layer_ptr->layerKey, &FWPM_LAYER_ALE_AUTH_RECV_ACCEPT_V4) || IsEqualGUID (&layer_ptr->layerKey, &FWPM_LAYER_ALE_AUTH_RECV_ACCEPT_V6))
		log->direction = FWP_DIRECTION_INBOUND; // HACK!!! (issue #581)

	RtlCopyMemory (&layer_guid, &layer_ptr->layerKey, sizeof (GUID));

	FwpmFreeMemory0 ((PVOID_PTR)&layer_ptr);

	// get filter information
	if (FwpmFilterGetById0 (engine_handle, log->filter_id, &filter_ptr) != ERROR_SUCCESS || !filter_ptr)
		return;

	layer_name = _wfp_getlayername (&layer_guid);

	if (!_r_str_isempty (filter_ptr->displayData.description))
	{
		filter_name = _r_obj_createstring (filter_ptr->displayData.description);
	}
	else if (!_r_str_isempty (filter_ptr->displayData.name))
	{
		filter_name = _r_obj_createstring (filter_ptr->displayData.name);
	}

	if (filter_ptr->weight.type == FWP_UINT8)
		filter_weight = filter_ptr->weight.uint8;

	if (filter_ptr->providerKey)
	{
		if (IsEqualGUID (filter_ptr->providerKey, &GUID_WfpProvider))
			is_myprovider = TRUE;
	}

	FwpmFreeMemory0 ((PVOID_PTR)&filter_ptr);

	// allocate log object
	ptr_log = _r_obj_allocate (sizeof (ITEM_LOG), &_app_dereferencelog);

	// copy date and time
	ptr_log->timestamp = _r_unixtime_from_filetime (&log->timestamp);

	// check token for null (not an appcontainer) (HACK!!!)
	if ((log->flags & FWPM_NET_EVENT_FLAG_PACKAGE_ID_SET) == FWPM_NET_EVENT_FLAG_PACKAGE_ID_SET && log->package_id)
	{
		if (RtlEqualSid (log->package_id, &SeNobodySid))
			log->flags &= ~(FWPM_NET_EVENT_FLAG_PACKAGE_ID_SET);
	}

	// get package id (win8+)
	if ((log->flags & FWPM_NET_EVENT_FLAG_PACKAGE_ID_SET) == FWPM_NET_EVENT_FLAG_PACKAGE_ID_SET && log->package_id)
	{
		status = _r_str_fromsid (&sid_string, log->package_id);

		if (NT_SUCCESS (status))
		{
			if (!_app_package_isnotexists (sid_string))
				_r_obj_clearreference ((PVOID_PTR)&sid_string);
		}
	}

	// copy converted nt device path into win32
	if ((log->flags & FWPM_NET_EVENT_FLAG_PACKAGE_ID_SET) == FWPM_NET_EVENT_FLAG_PACKAGE_ID_SET && sid_string)
	{
		_r_obj_movereference ((PVOID_PTR)&ptr_log->path, sid_string);
		sid_string = NULL;

		ptr_log->app_hash = _r_str_gethash (&ptr_log->path->sr, TRUE);
	}
	else if ((log->flags & FWPM_NET_EVENT_FLAG_APP_ID_SET) == FWPM_NET_EVENT_FLAG_APP_ID_SET && log->app_id)
	{
		path = _r_obj_createstring ((LPCWSTR)(log->app_id));

		resolved_path = _r_path_dospathfromnt (&path->sr);

		if (resolved_path)
			_r_obj_movereference ((PVOID_PTR)&path, resolved_path);

		if (path)
		{
			_r_obj_movereference ((PVOID_PTR)&ptr_log->path, path);

			ptr_log->app_hash = _r_str_gethash (&ptr_log->path->sr, TRUE);
		}
	}
	else
	{
		ptr_log->app_hash = 0;
	}

	// get username information
	if ((log->flags & FWPM_NET_EVENT_FLAG_USER_ID_SET) == FWPM_NET_EVENT_FLAG_USER_ID_SET && log->user_id)
		_r_sys_getusername (&ptr_log->username, log->user_id, TRUE);

	// destination
	if ((log->flags & FWPM_NET_EVENT_FLAG_IP_VERSION_SET) == FWPM_NET_EVENT_FLAG_IP_VERSION_SET)
	{
		if (log->version == FWP_IP_VERSION_V4)
		{
			ptr_log->af = AF_INET;

			// local address
			if ((log->flags & FWPM_NET_EVENT_FLAG_LOCAL_ADDR_SET) == FWPM_NET_EVENT_FLAG_LOCAL_ADDR_SET)
				ptr_log->local_addr.S_un.S_addr = _r_byteswap_ulong ((ULONG)log->local_addr4);

			// remote address
			if ((log->flags & FWPM_NET_EVENT_FLAG_REMOTE_ADDR_SET) == FWPM_NET_EVENT_FLAG_REMOTE_ADDR_SET)
				ptr_log->remote_addr.S_un.S_addr = _r_byteswap_ulong ((ULONG)log->remote_addr4);

		}
		else if (log->version == FWP_IP_VERSION_V6)
		{
			ptr_log->af = AF_INET6;

			// remote address
			if ((log->flags & FWPM_NET_EVENT_FLAG_REMOTE_ADDR_SET) == FWPM_NET_EVENT_FLAG_REMOTE_ADDR_SET)
				RtlCopyMemory (ptr_log->remote_addr6.u.Byte, log->remote_addr6.byteArray16, FWP_V6_ADDR_SIZE);

			// local address
			if ((log->flags & FWPM_NET_EVENT_FLAG_LOCAL_ADDR_SET) == FWPM_NET_EVENT_FLAG_LOCAL_ADDR_SET)
				RtlCopyMemory (ptr_log->local_addr6.u.Byte, log->local_addr6.byteArray16, FWP_V6_ADDR_SIZE);
		}
	}

	// ports
	if ((log->flags & FWPM_NET_EVENT_FLAG_LOCAL_PORT_SET) == FWPM_NET_EVENT_FLAG_LOCAL_PORT_SET)
		ptr_log->local_port = log->local_port;

	if ((log->flags & FWPM_NET_EVENT_FLAG_REMOTE_PORT_SET) == FWPM_NET_EVENT_FLAG_REMOTE_PORT_SET)
		ptr_log->remote_port = log->remote_port;

	// protocol
	if ((log->flags & FWPM_NET_EVENT_FLAG_IP_PROTOCOL_SET) == FWPM_NET_EVENT_FLAG_IP_PROTOCOL_SET)
	{
		ptr_log->protocol = log->protocol;

		ptr_log->protocol_str = _r_obj_createstring (_app_db_getprotoname (ptr_log->protocol, ptr_log->af));
	}

	// indicates FWPM_NET_EVENT_TYPE_CLASSIFY_ALLOW state
	ptr_log->is_allow = log->is_allow;

	// indicates whether the packet originated from (or was heading to) the loopback adapter
	ptr_log->is_loopback = log->is_loopback;

	// set filter information
	ptr_log->filter_id = log->filter_id;
	ptr_log->direction = log->direction;

	ptr_log->is_myprovider = is_myprovider;

	ptr_log->filter_name = filter_name;
	ptr_log->layer_name = layer_name;

	ptr_log->is_blocklist = (filter_weight == FW_WEIGHT_RULE_BLOCKLIST);
	ptr_log->is_system = (filter_weight == FW_WEIGHT_HIGHEST) || (filter_weight == FW_WEIGHT_HIGHEST_IMPORTANT);
	ptr_log->is_custom = (filter_weight == FW_WEIGHT_RULE_USER) || (filter_weight == FW_WEIGHT_RULE_USER_BLOCK);

	_app_logthread (ptr_log);

	if (sid_string)
		_r_obj_dereference (sid_string);
}

static VOID NTAPI _app_logcallbackthread (
	_In_opt_ PVOID arglist
)
{
	PITEM_LOG_CALLBACK log;

	log = arglist;

	if (!log)
		return;

	_app_logcallbackprocess (log);

	_app_logfreecallback (log);
}

VOID CALLBACK _wfp_logcallback (
	_In_ PITEM_LOG_CALLBACK log
)
{
	PITEM_LOG_CALLBACK log_copy;

	if (!log->filter_id || !log->layer_id)
		return;

	if (log->is_allow && _r_config_getboolean (L"IsExcludeClassifyAllow", TRUE, NULL))
		return;

	log_copy = _app_logcopycallback (log);

	_r_workqueue_queueitem (&log_queue, &_app_logcallbackthread, log_copy);
}

FORCEINLINE BOOLEAN log_struct_to_f (
	_Out_ PITEM_LOG_CALLBACK log,
	_In_ ULONG version,
	_In_ LPCVOID event_data
)
{
	RtlSecureZeroMemory (log, sizeof (ITEM_LOG_CALLBACK));

	switch (version)
	{
		case WINDOWS_10_RS3:
		{
			const FWPM_NET_EVENT5 *evt = event_data;

			if (evt->type == FWPM_NET_EVENT_TYPE_CLASSIFY_DROP && evt->classifyDrop)
			{
				log->layer_id = evt->classifyDrop->layerId;
				log->filter_id = evt->classifyDrop->filterId;
				log->direction = evt->classifyDrop->msFwpDirection;
				log->is_loopback = !!evt->classifyDrop->isLoopback;
			}
			else if (evt->type == FWPM_NET_EVENT_TYPE_IPSEC_KERNEL_DROP && evt->ipsecDrop)
			{
				log->layer_id = evt->ipsecDrop->layerId;
				log->filter_id = evt->ipsecDrop->filterId;
				log->direction = evt->ipsecDrop->direction;
			}
			else if (evt->type == FWPM_NET_EVENT_TYPE_CLASSIFY_ALLOW && evt->classifyAllow)
			{
				log->layer_id = evt->classifyAllow->layerId;
				log->filter_id = evt->classifyAllow->filterId;
				log->direction = evt->classifyAllow->msFwpDirection;
				log->is_loopback = !!evt->classifyAllow->isLoopback;

				log->is_allow = TRUE;
			}
			else if (evt->type == FWPM_NET_EVENT_TYPE_CLASSIFY_DROP_MAC && evt->classifyDropMac)
			{
				log->layer_id = evt->classifyDropMac->layerId;
				log->filter_id = evt->classifyDropMac->filterId;
				log->direction = evt->classifyDropMac->msFwpDirection;
				log->is_loopback = !!evt->classifyDropMac->isLoopback;
			}
			else
			{
				return FALSE;
			}

			// indicates the direction of the packet transmission and set valid directions
			switch (log->direction)
			{
				case FWP_DIRECTION_IN:
				case FWP_DIRECTION_INBOUND:
				{
					log->direction = FWP_DIRECTION_INBOUND;
					break;
				}

				case FWP_DIRECTION_OUT:
				case FWP_DIRECTION_OUTBOUND:
				{
					log->direction = FWP_DIRECTION_OUTBOUND;
					break;
				}

				default:
				{
					return FALSE;
				}
			}

			log->flags = evt->header.flags;

			RtlCopyMemory (&log->timestamp, &evt->header.timeStamp, sizeof (FILETIME));

			if ((evt->header.flags & FWPM_NET_EVENT_FLAG_APP_ID_SET) == FWPM_NET_EVENT_FLAG_APP_ID_SET)
			{
				log->app_id = evt->header.appId.data;
				log->app_id_length = evt->header.appId.size;
			}

			if ((evt->header.flags & FWPM_NET_EVENT_FLAG_PACKAGE_ID_SET) == FWPM_NET_EVENT_FLAG_PACKAGE_ID_SET)
				log->package_id = evt->header.packageSid;

			if ((evt->header.flags & FWPM_NET_EVENT_FLAG_USER_ID_SET) == FWPM_NET_EVENT_FLAG_USER_ID_SET)
				log->user_id = evt->header.userId;

			if ((evt->header.flags & FWPM_NET_EVENT_FLAG_IP_PROTOCOL_SET) == FWPM_NET_EVENT_FLAG_IP_PROTOCOL_SET)
				log->protocol = evt->header.ipProtocol;

			if ((evt->header.flags & FWPM_NET_EVENT_FLAG_LOCAL_PORT_SET) == FWPM_NET_EVENT_FLAG_LOCAL_PORT_SET)
				log->local_port = evt->header.localPort;

			if ((evt->header.flags & FWPM_NET_EVENT_FLAG_REMOTE_PORT_SET) == FWPM_NET_EVENT_FLAG_REMOTE_PORT_SET)
				log->remote_port = evt->header.remotePort;

			if ((evt->header.flags & FWPM_NET_EVENT_FLAG_IP_VERSION_SET) == FWPM_NET_EVENT_FLAG_IP_VERSION_SET)
			{
				log->version = evt->header.ipVersion;

				if (evt->header.ipVersion == FWP_IP_VERSION_V4)
				{
					if ((evt->header.flags & FWPM_NET_EVENT_FLAG_LOCAL_ADDR_SET) == FWPM_NET_EVENT_FLAG_LOCAL_ADDR_SET)
						log->local_addr4 = evt->header.localAddrV4;

					if ((evt->header.flags & FWPM_NET_EVENT_FLAG_REMOTE_ADDR_SET) == FWPM_NET_EVENT_FLAG_REMOTE_ADDR_SET)
						log->remote_addr4 = evt->header.remoteAddrV4;
				}
				else if (evt->header.ipVersion == FWP_IP_VERSION_V6)
				{
					if ((evt->header.flags & FWPM_NET_EVENT_FLAG_LOCAL_ADDR_SET) == FWPM_NET_EVENT_FLAG_LOCAL_ADDR_SET)
						RtlCopyMemory (&log->local_addr6, &evt->header.localAddrV6, sizeof (FWP_BYTE_ARRAY16));

					if ((evt->header.flags & FWPM_NET_EVENT_FLAG_REMOTE_ADDR_SET) == FWPM_NET_EVENT_FLAG_REMOTE_ADDR_SET)
						RtlCopyMemory (&log->remote_addr6, &evt->header.remoteAddrV6, sizeof (FWP_BYTE_ARRAY16));
				}
			}
			else
			{
				log->version = FWP_IP_VERSION_NONE;
			}

			break;
		}

		case WINDOWS_10_RS1:
		{
			const FWPM_NET_EVENT4 *evt = (const FWPM_NET_EVENT4*)event_data;

			if (evt->type == FWPM_NET_EVENT_TYPE_CLASSIFY_DROP && evt->classifyDrop)
			{
				log->layer_id = evt->classifyDrop->layerId;
				log->filter_id = evt->classifyDrop->filterId;
				log->direction = evt->classifyDrop->msFwpDirection;
				log->is_loopback = !!evt->classifyDrop->isLoopback;
			}
			else if (evt->type == FWPM_NET_EVENT_TYPE_IPSEC_KERNEL_DROP && evt->ipsecDrop)
			{
				log->layer_id = evt->ipsecDrop->layerId;
				log->filter_id = evt->ipsecDrop->filterId;
				log->direction = evt->ipsecDrop->direction;
			}
			else if (evt->type == FWPM_NET_EVENT_TYPE_CLASSIFY_ALLOW && evt->classifyAllow)
			{
				log->layer_id = evt->classifyAllow->layerId;
				log->filter_id = evt->classifyAllow->filterId;
				log->direction = evt->classifyAllow->msFwpDirection;
				log->is_loopback = !!evt->classifyAllow->isLoopback;

				log->is_allow = TRUE;
			}
			else if (evt->type == FWPM_NET_EVENT_TYPE_CLASSIFY_DROP_MAC && evt->classifyDropMac)
			{
				log->layer_id = evt->classifyDropMac->layerId;
				log->filter_id = evt->classifyDropMac->filterId;
				log->direction = evt->classifyDropMac->msFwpDirection;
				log->is_loopback = !!evt->classifyDropMac->isLoopback;
			}
			else
			{
				return FALSE;
			}

			// indicates the direction of the packet transmission and set valid directions
			switch (log->direction)
			{
				case FWP_DIRECTION_IN:
				case FWP_DIRECTION_INBOUND:
				{
					log->direction = FWP_DIRECTION_INBOUND;
					break;
				}

				case FWP_DIRECTION_OUT:
				case FWP_DIRECTION_OUTBOUND:
				{
					log->direction = FWP_DIRECTION_OUTBOUND;
					break;
				}

				default:
				{
					return FALSE;
				}
			}

			log->flags = evt->header.flags;

			RtlCopyMemory (&log->timestamp, &evt->header.timeStamp, sizeof (FILETIME));

			if ((evt->header.flags & FWPM_NET_EVENT_FLAG_APP_ID_SET) == FWPM_NET_EVENT_FLAG_APP_ID_SET)
			{
				log->app_id = evt->header.appId.data;
				log->app_id_length = evt->header.appId.size;
			}

			if ((evt->header.flags & FWPM_NET_EVENT_FLAG_PACKAGE_ID_SET) == FWPM_NET_EVENT_FLAG_PACKAGE_ID_SET)
				log->package_id = evt->header.packageSid;

			if ((evt->header.flags & FWPM_NET_EVENT_FLAG_USER_ID_SET) == FWPM_NET_EVENT_FLAG_USER_ID_SET)
				log->user_id = evt->header.userId;

			if ((evt->header.flags & FWPM_NET_EVENT_FLAG_IP_PROTOCOL_SET) == FWPM_NET_EVENT_FLAG_IP_PROTOCOL_SET)
				log->protocol = evt->header.ipProtocol;

			if ((evt->header.flags & FWPM_NET_EVENT_FLAG_LOCAL_PORT_SET) == FWPM_NET_EVENT_FLAG_LOCAL_PORT_SET)
				log->local_port = evt->header.localPort;

			if ((evt->header.flags & FWPM_NET_EVENT_FLAG_REMOTE_PORT_SET) == FWPM_NET_EVENT_FLAG_REMOTE_PORT_SET)
				log->remote_port = evt->header.remotePort;

			if ((evt->header.flags & FWPM_NET_EVENT_FLAG_IP_VERSION_SET) == FWPM_NET_EVENT_FLAG_IP_VERSION_SET)
			{
				log->version = evt->header.ipVersion;

				if (evt->header.ipVersion == FWP_IP_VERSION_V4)
				{
					if ((evt->header.flags & FWPM_NET_EVENT_FLAG_LOCAL_ADDR_SET) == FWPM_NET_EVENT_FLAG_LOCAL_ADDR_SET)
						log->local_addr4 = evt->header.localAddrV4;

					if ((evt->header.flags & FWPM_NET_EVENT_FLAG_REMOTE_ADDR_SET) == FWPM_NET_EVENT_FLAG_REMOTE_ADDR_SET)
						log->remote_addr4 = evt->header.remoteAddrV4;
				}
				else if (evt->header.ipVersion == FWP_IP_VERSION_V6)
				{
					if ((evt->header.flags & FWPM_NET_EVENT_FLAG_LOCAL_ADDR_SET) == FWPM_NET_EVENT_FLAG_LOCAL_ADDR_SET)
						RtlCopyMemory (&log->local_addr6, &evt->header.localAddrV6, sizeof (FWP_BYTE_ARRAY16));

					if ((evt->header.flags & FWPM_NET_EVENT_FLAG_REMOTE_ADDR_SET) == FWPM_NET_EVENT_FLAG_REMOTE_ADDR_SET)
						RtlCopyMemory (&log->remote_addr6, &evt->header.remoteAddrV6, sizeof (FWP_BYTE_ARRAY16));
				}
			}
			else
			{
				log->version = FWP_IP_VERSION_NONE;
			}

			break;
		}

		case WINDOWS_10_RS2:
		{
			const FWPM_NET_EVENT3 *evt = event_data;

			if (evt->type == FWPM_NET_EVENT_TYPE_CLASSIFY_DROP && evt->classifyDrop)
			{
				log->layer_id = evt->classifyDrop->layerId;
				log->filter_id = evt->classifyDrop->filterId;
				log->direction = evt->classifyDrop->msFwpDirection;
				log->is_loopback = !!evt->classifyDrop->isLoopback;
			}
			else if (evt->type == FWPM_NET_EVENT_TYPE_IPSEC_KERNEL_DROP && evt->ipsecDrop)
			{
				log->layer_id = evt->ipsecDrop->layerId;
				log->filter_id = evt->ipsecDrop->filterId;
				log->direction = evt->ipsecDrop->direction;
			}
			else if (evt->type == FWPM_NET_EVENT_TYPE_CLASSIFY_ALLOW && evt->classifyAllow)
			{
				log->layer_id = evt->classifyAllow->layerId;
				log->filter_id = evt->classifyAllow->filterId;
				log->direction = evt->classifyAllow->msFwpDirection;
				log->is_loopback = !!evt->classifyAllow->isLoopback;

				log->is_allow = TRUE;
			}
			else if (evt->type == FWPM_NET_EVENT_TYPE_CLASSIFY_DROP_MAC && evt->classifyDropMac)
			{
				log->layer_id = evt->classifyDropMac->layerId;
				log->filter_id = evt->classifyDropMac->filterId;
				log->direction = evt->classifyDropMac->msFwpDirection;
				log->is_loopback = !!evt->classifyDropMac->isLoopback;
			}
			else
			{
				return FALSE;
			}

			// indicates the direction of the packet transmission and set valid directions
			switch (log->direction)
			{
				case FWP_DIRECTION_IN:
				case FWP_DIRECTION_INBOUND:
				{
					log->direction = FWP_DIRECTION_INBOUND;
					break;
				}

				case FWP_DIRECTION_OUT:
				case FWP_DIRECTION_OUTBOUND:
				{
					log->direction = FWP_DIRECTION_OUTBOUND;
					break;
				}

				default:
				{
					return FALSE;
				}
			}

			log->flags = evt->header.flags;

			RtlCopyMemory (&log->timestamp, &evt->header.timeStamp, sizeof (FILETIME));

			if ((evt->header.flags & FWPM_NET_EVENT_FLAG_APP_ID_SET) == FWPM_NET_EVENT_FLAG_APP_ID_SET)
			{
				log->app_id = evt->header.appId.data;
				log->app_id_length = evt->header.appId.size;
			}

			if ((evt->header.flags & FWPM_NET_EVENT_FLAG_PACKAGE_ID_SET) == FWPM_NET_EVENT_FLAG_PACKAGE_ID_SET)
				log->package_id = evt->header.packageSid;

			if ((evt->header.flags & FWPM_NET_EVENT_FLAG_USER_ID_SET) == FWPM_NET_EVENT_FLAG_USER_ID_SET)
				log->user_id = evt->header.userId;

			if ((evt->header.flags & FWPM_NET_EVENT_FLAG_IP_PROTOCOL_SET) == FWPM_NET_EVENT_FLAG_IP_PROTOCOL_SET)
				log->protocol = evt->header.ipProtocol;

			if ((evt->header.flags & FWPM_NET_EVENT_FLAG_LOCAL_PORT_SET) == FWPM_NET_EVENT_FLAG_LOCAL_PORT_SET)
				log->local_port = evt->header.localPort;

			if ((evt->header.flags & FWPM_NET_EVENT_FLAG_REMOTE_PORT_SET) == FWPM_NET_EVENT_FLAG_REMOTE_PORT_SET)
				log->remote_port = evt->header.remotePort;

			if ((evt->header.flags & FWPM_NET_EVENT_FLAG_IP_VERSION_SET) == FWPM_NET_EVENT_FLAG_IP_VERSION_SET)
			{
				log->version = evt->header.ipVersion;

				if (evt->header.ipVersion == FWP_IP_VERSION_V4)
				{
					if ((evt->header.flags & FWPM_NET_EVENT_FLAG_LOCAL_ADDR_SET) == FWPM_NET_EVENT_FLAG_LOCAL_ADDR_SET)
						log->local_addr4 = evt->header.localAddrV4;

					if ((evt->header.flags & FWPM_NET_EVENT_FLAG_REMOTE_ADDR_SET) == FWPM_NET_EVENT_FLAG_REMOTE_ADDR_SET)
						log->remote_addr4 = evt->header.remoteAddrV4;
				}
				else if (evt->header.ipVersion == FWP_IP_VERSION_V6)
				{
					if ((evt->header.flags & FWPM_NET_EVENT_FLAG_LOCAL_ADDR_SET) == FWPM_NET_EVENT_FLAG_LOCAL_ADDR_SET)
						RtlCopyMemory (&log->local_addr6, &evt->header.localAddrV6, sizeof (FWP_BYTE_ARRAY16));

					if ((evt->header.flags & FWPM_NET_EVENT_FLAG_REMOTE_ADDR_SET) == FWPM_NET_EVENT_FLAG_REMOTE_ADDR_SET)
						RtlCopyMemory (&log->remote_addr6, &evt->header.remoteAddrV6, sizeof (FWP_BYTE_ARRAY16));
				}
			}
			else
			{
				log->version = FWP_IP_VERSION_NONE;
			}

			break;
		}

		case WINDOWS_8:
		{
			const FWPM_NET_EVENT2 *evt = event_data;

			if (evt->type == FWPM_NET_EVENT_TYPE_CLASSIFY_DROP && evt->classifyDrop)
			{
				log->layer_id = evt->classifyDrop->layerId;
				log->filter_id = evt->classifyDrop->filterId;
				log->direction = evt->classifyDrop->msFwpDirection;
				log->is_loopback = !!evt->classifyDrop->isLoopback;
			}
			else if (evt->type == FWPM_NET_EVENT_TYPE_IPSEC_KERNEL_DROP && evt->ipsecDrop)
			{
				log->layer_id = evt->ipsecDrop->layerId;
				log->filter_id = evt->ipsecDrop->filterId;
				log->direction = evt->ipsecDrop->direction;
			}
			else if (evt->type == FWPM_NET_EVENT_TYPE_CLASSIFY_ALLOW && evt->classifyAllow)
			{
				log->layer_id = evt->classifyAllow->layerId;
				log->filter_id = evt->classifyAllow->filterId;
				log->direction = evt->classifyAllow->msFwpDirection;
				log->is_loopback = !!evt->classifyAllow->isLoopback;

				log->is_allow = TRUE;
			}
			else if (evt->type == FWPM_NET_EVENT_TYPE_CLASSIFY_DROP_MAC && evt->classifyDropMac)
			{
				log->layer_id = evt->classifyDropMac->layerId;
				log->filter_id = evt->classifyDropMac->filterId;
				log->direction = evt->classifyDropMac->msFwpDirection;
				log->is_loopback = !!evt->classifyDropMac->isLoopback;
			}
			else
			{
				return FALSE;
			}

			// indicates the direction of the packet transmission and set valid directions
			switch (log->direction)
			{
				case FWP_DIRECTION_IN:
				case FWP_DIRECTION_INBOUND:
				{
					log->direction = FWP_DIRECTION_INBOUND;
					break;
				}

				case FWP_DIRECTION_OUT:
				case FWP_DIRECTION_OUTBOUND:
				{
					log->direction = FWP_DIRECTION_OUTBOUND;
					break;
				}

				default:
				{
					return FALSE;
				}
			}

			log->flags = evt->header.flags;

			RtlCopyMemory (&log->timestamp, &evt->header.timeStamp, sizeof (FILETIME));

			if ((evt->header.flags & FWPM_NET_EVENT_FLAG_APP_ID_SET) == FWPM_NET_EVENT_FLAG_APP_ID_SET)
			{
				log->app_id = evt->header.appId.data;
				log->app_id_length = evt->header.appId.size;
			}

			if ((evt->header.flags & FWPM_NET_EVENT_FLAG_PACKAGE_ID_SET) == FWPM_NET_EVENT_FLAG_PACKAGE_ID_SET)
				log->package_id = evt->header.packageSid;

			if ((evt->header.flags & FWPM_NET_EVENT_FLAG_USER_ID_SET) == FWPM_NET_EVENT_FLAG_USER_ID_SET)
				log->user_id = evt->header.userId;

			if ((evt->header.flags & FWPM_NET_EVENT_FLAG_IP_PROTOCOL_SET) == FWPM_NET_EVENT_FLAG_IP_PROTOCOL_SET)
				log->protocol = evt->header.ipProtocol;

			if ((evt->header.flags & FWPM_NET_EVENT_FLAG_LOCAL_PORT_SET) == FWPM_NET_EVENT_FLAG_LOCAL_PORT_SET)
				log->local_port = evt->header.localPort;

			if ((evt->header.flags & FWPM_NET_EVENT_FLAG_REMOTE_PORT_SET) == FWPM_NET_EVENT_FLAG_REMOTE_PORT_SET)
				log->remote_port = evt->header.remotePort;

			if ((evt->header.flags & FWPM_NET_EVENT_FLAG_IP_VERSION_SET) == FWPM_NET_EVENT_FLAG_IP_VERSION_SET)
			{
				log->version = evt->header.ipVersion;

				if (evt->header.ipVersion == FWP_IP_VERSION_V4)
				{
					if ((evt->header.flags & FWPM_NET_EVENT_FLAG_LOCAL_ADDR_SET) == FWPM_NET_EVENT_FLAG_LOCAL_ADDR_SET)
						log->local_addr4 = evt->header.localAddrV4;

					if ((evt->header.flags & FWPM_NET_EVENT_FLAG_REMOTE_ADDR_SET) == FWPM_NET_EVENT_FLAG_REMOTE_ADDR_SET)
						log->remote_addr4 = evt->header.remoteAddrV4;
				}
				else if (evt->header.ipVersion == FWP_IP_VERSION_V6)
				{
					if ((evt->header.flags & FWPM_NET_EVENT_FLAG_LOCAL_ADDR_SET) == FWPM_NET_EVENT_FLAG_LOCAL_ADDR_SET)
						RtlCopyMemory (&log->local_addr6, &evt->header.localAddrV6, sizeof (FWP_BYTE_ARRAY16));

					if ((evt->header.flags & FWPM_NET_EVENT_FLAG_REMOTE_ADDR_SET) == FWPM_NET_EVENT_FLAG_REMOTE_ADDR_SET)
						RtlCopyMemory (&log->remote_addr6, &evt->header.remoteAddrV6, sizeof (FWP_BYTE_ARRAY16));
				}
			}
			else
			{
				log->version = FWP_IP_VERSION_NONE;
			}

			break;
		}

		case WINDOWS_7:
		{
			const FWPM_NET_EVENT1 *evt = event_data;

			if (evt->type == FWPM_NET_EVENT_TYPE_CLASSIFY_DROP && evt->classifyDrop)
			{
				log->layer_id = evt->classifyDrop->layerId;
				log->filter_id = evt->classifyDrop->filterId;
				log->direction = evt->classifyDrop->msFwpDirection;
				log->is_loopback = !!evt->classifyDrop->isLoopback;
			}
			else if (evt->type == FWPM_NET_EVENT_TYPE_IPSEC_KERNEL_DROP && evt->ipsecDrop)
			{
				log->layer_id = evt->ipsecDrop->layerId;
				log->filter_id = evt->ipsecDrop->filterId;
				log->direction = evt->ipsecDrop->direction;
			}
			else
			{
				return FALSE;
			}

			// indicates the direction of the packet transmission and set valid directions
			switch (log->direction)
			{
				case FWP_DIRECTION_IN:
				case FWP_DIRECTION_INBOUND:
				{
					log->direction = FWP_DIRECTION_INBOUND;
					break;
				}

				case FWP_DIRECTION_OUT:
				case FWP_DIRECTION_OUTBOUND:
				{
					log->direction = FWP_DIRECTION_OUTBOUND;
					break;
				}

				default:
				{
					return FALSE;
				}
			}

			log->flags = evt->header.flags;

			RtlCopyMemory (&log->timestamp, &evt->header.timeStamp, sizeof (FILETIME));

			if ((evt->header.flags & FWPM_NET_EVENT_FLAG_APP_ID_SET) == FWPM_NET_EVENT_FLAG_APP_ID_SET)
			{
				log->app_id = evt->header.appId.data;
				log->app_id_length = evt->header.appId.size;
			}

			if ((evt->header.flags & FWPM_NET_EVENT_FLAG_USER_ID_SET) == FWPM_NET_EVENT_FLAG_USER_ID_SET)
				log->user_id = evt->header.userId;

			if ((evt->header.flags & FWPM_NET_EVENT_FLAG_IP_PROTOCOL_SET) == FWPM_NET_EVENT_FLAG_IP_PROTOCOL_SET)
				log->protocol = evt->header.ipProtocol;

			if ((evt->header.flags & FWPM_NET_EVENT_FLAG_LOCAL_PORT_SET) == FWPM_NET_EVENT_FLAG_LOCAL_PORT_SET)
				log->local_port = evt->header.localPort;

			if ((evt->header.flags & FWPM_NET_EVENT_FLAG_REMOTE_PORT_SET) == FWPM_NET_EVENT_FLAG_REMOTE_PORT_SET)
				log->remote_port = evt->header.remotePort;

			if ((evt->header.flags & FWPM_NET_EVENT_FLAG_IP_VERSION_SET) == FWPM_NET_EVENT_FLAG_IP_VERSION_SET)
			{
				log->version = evt->header.ipVersion;

				if (evt->header.ipVersion == FWP_IP_VERSION_V4)
				{
					if ((evt->header.flags & FWPM_NET_EVENT_FLAG_LOCAL_ADDR_SET) == FWPM_NET_EVENT_FLAG_LOCAL_ADDR_SET)
						log->local_addr4 = evt->header.localAddrV4;

					if ((evt->header.flags & FWPM_NET_EVENT_FLAG_REMOTE_ADDR_SET) == FWPM_NET_EVENT_FLAG_REMOTE_ADDR_SET)
						log->remote_addr4 = evt->header.remoteAddrV4;
				}
				else if (evt->header.ipVersion == FWP_IP_VERSION_V6)
				{
					if ((evt->header.flags & FWPM_NET_EVENT_FLAG_LOCAL_ADDR_SET) == FWPM_NET_EVENT_FLAG_LOCAL_ADDR_SET)
						RtlCopyMemory (&log->local_addr6, &evt->header.localAddrV6, sizeof (FWP_BYTE_ARRAY16));

					if ((evt->header.flags & FWPM_NET_EVENT_FLAG_REMOTE_ADDR_SET) == FWPM_NET_EVENT_FLAG_REMOTE_ADDR_SET)
						RtlCopyMemory (&log->remote_addr6, &evt->header.remoteAddrV6, sizeof (FWP_BYTE_ARRAY16));
				}
			}
			else
			{
				log->version = FWP_IP_VERSION_NONE;
			}

			break;
		}

		default:
		{
			return FALSE;
		}
	}

	return TRUE;
}

// win7+ callback
VOID CALLBACK _wfp_logcallback0 (
	_Inout_ PVOID context,
	_In_ const FWPM_NET_EVENT1* event_data
)
{
	ITEM_LOG_CALLBACK log;

	if (log_struct_to_f (&log, WINDOWS_7, event_data))
		_wfp_logcallback (&log);
}

// win8+ callback
VOID CALLBACK _wfp_logcallback1 (
	_Inout_ PVOID context,
	_In_ const FWPM_NET_EVENT2* event_data
)
{
	ITEM_LOG_CALLBACK log;

	if (log_struct_to_f (&log, WINDOWS_8, event_data))
		_wfp_logcallback (&log);
}

// win10rs1+ callback
VOID CALLBACK _wfp_logcallback2 (
	_Inout_ PVOID context,
	_In_ const FWPM_NET_EVENT3* event_data
)
{
	ITEM_LOG_CALLBACK log;

	if (log_struct_to_f (&log, WINDOWS_10_RS1, event_data))
		_wfp_logcallback (&log);
}

// win10rs1+ callback
VOID CALLBACK _wfp_logcallback3 (
	_Inout_ PVOID context,
	_In_ const FWPM_NET_EVENT4* event_data
)
{
	ITEM_LOG_CALLBACK log;

	if (log_struct_to_f (&log, WINDOWS_10_RS2, event_data))
		_wfp_logcallback (&log);
}

// win10rs3+ callback
VOID CALLBACK _wfp_logcallback4 (
	_Inout_ PVOID context,
	_In_ const FWPM_NET_EVENT5* event_data
)
{
	ITEM_LOG_CALLBACK log;

	if (log_struct_to_f (&log, WINDOWS_10_RS3, event_data))
		_wfp_logcallback (&log);
}

VOID NTAPI _app_logthread (
	_In_ PVOID arglist
)
{
	PITEM_APP ptr_app = NULL;
	PITEM_LOG ptr_log;
	HWND hwnd;
	BOOLEAN is_notificationenabled;
	BOOLEAN is_exludeblocklist;
	BOOLEAN is_exludestealth;
	BOOLEAN is_loguienabled;
	BOOLEAN is_exludeallow;
	BOOLEAN is_silent = FALSE;
	BOOLEAN is_logenabled;
	BOOLEAN is_notexist;

	hwnd = _r_app_gethwnd ();

	ptr_log = arglist;

	// apps collector
	is_notexist = ptr_log->app_hash && !ptr_log->is_allow && !_app_isappfound (ptr_log->app_hash);

	if (is_notexist)
	{
		ptr_log->app_hash = _app_addapplication (NULL, DATA_UNKNOWN, ptr_log->path, NULL, NULL);

		if (ptr_log->app_hash)
		{
			ptr_app = _app_getappitem (ptr_log->app_hash);

			if (ptr_app)
				PostMessageW (hwnd, WM_LISTVIEW_UPDATED, (WPARAM)ptr_app->type, 0);

			_app_profile_save_deferred ();
		}
	}

	if (!ptr_app)
		ptr_app = _app_getappitem (ptr_log->app_hash);

	is_logenabled = _r_config_getboolean (L"IsLogEnabled", FALSE, NULL);
	is_loguienabled = _r_config_getboolean (L"IsLogUiEnabled", FALSE, NULL);
	is_notificationenabled = _r_config_getboolean (L"IsNotificationsEnabled", TRUE, NULL);

	is_exludeallow = !(ptr_log->is_allow && _r_config_getboolean (L"IsExcludeClassifyAllow", TRUE, NULL));
	is_exludestealth = !(ptr_log->is_system && _r_config_getboolean (L"IsExcludeStealth", TRUE, NULL));

	is_exludeblocklist = !(ptr_log->is_blocklist && _r_config_getboolean (L"IsExcludeBlocklist", TRUE, NULL)) && !(ptr_log->is_custom && _r_config_getboolean (L"IsExcludeCustomRules", TRUE, NULL));

	if ((is_logenabled || is_loguienabled || is_notificationenabled) && is_exludestealth && is_exludeallow)
	{
		// get network string
		ptr_log->remote_addr_str = _app_formataddress (ptr_log->af, ptr_log->protocol, &ptr_log->remote_addr, 0, 0);
		ptr_log->local_addr_str = _app_formataddress (ptr_log->af, ptr_log->protocol, &ptr_log->local_addr, 0, 0);

		// display notification
		if (is_notificationenabled)
		{
			if (ptr_app && !ptr_log->is_allow && is_exludeblocklist)
			{
				if (_app_getappinfo (ptr_app, INFO_IS_SILENT, &is_silent, sizeof (BOOLEAN)))
				{
					if (!is_silent)
						_app_notify_addobject (hwnd, ptr_log, ptr_app);
				}
			}
		}

		// write log to a file
		if (is_logenabled)
			_app_logwrite (ptr_log);

		// only for my own provider
		if (ptr_log->is_myprovider)
		{
			// write log to a ui
			if (is_loguienabled)
				_app_logwrite_ui_async (hwnd, ptr_log); // write to a ui
		}
	}

	if (ptr_app)
		_r_obj_dereference (ptr_app);

	_r_obj_dereference (ptr_log);
}
