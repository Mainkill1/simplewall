#pragma once
#include "packet_store.h"
/* IDs are deliberately outside the legacy list-control range. */
#define PH_APPLIST 6100
#define PH_ROWS 6101
#define PH_BLOCKED 6102
#define PH_ALLOWED 6103
#define PH_FILTER 6104
#define PH_GROUP 6105
#define PH_DIRECTION 6106
#define PH_CLEAR 6107
#define PH_ALLOW 6108
#define PH_PREV 6109
#define PH_NEXT 6110
#define PH_LIVE 6111
#define PH_LIMIT 6112
#define PH_APPPREV 6113
#define PH_APPNEXT 6114
#define PH_HEADER 6115
#define PH_STATS 6116
#define PH_STATUS 6117
#define PH_LIMITCOUNT 6118
#define PH_LIMITDAYS 6119
#define PH_LIMITMIB 6120
#define PH_SAVEGLOBAL 6121
#define PH_SAVEAPP 6122
#define PH_LIMITPANEL 6123
#define PH_AUDIT 6124
#define PH_AUDIT_HELP 6125
#define PH_AUDIT_TIME 6126
#define PH_RESULT_READY (WM_APP + 161)
#define PH_PROCESS_READY (WM_APP + 162)
#define PH_AUDIT_BATCH 128
#define PH_AUDIT_ALWAYS ((ULONG)-1)
#define PH_AUDIT_MAX_MINUTES 1440
typedef struct
{
	LONG generation;
	BOOLEAN apps_only;
	PH_QUERY query;
	PH_ORDER app_order;
	int64_t app_offset;
	WCHAR filter[256];
} PH_REQUEST;
typedef struct
{
	PH_REQUEST request;
	PH_APPS apps;
	PH_PAGE page;
	int error;
	int64_t lost, evicted, cap, bytes;
	int days, mib;
	char message[256];
} PH_RESULT;
VOID _app_packet_init(HWND hwnd);
VOID _app_packet_shutdown(VOID);
BOOLEAN _app_packet_capture_reserve(SIZE_T bytes);
VOID _app_packet_capture_release(SIZE_T bytes);
VOID _app_packet_submit(PITEM_LOG log);
VOID _app_packet_reloadwatches(VOID);
BOOLEAN _app_packet_haswatches(VOID);
BOOLEAN _app_packet_iswatched(LPCWSTR path);
BOOLEAN _app_packet_issvchost(LPCWSTR path);
PR_STRING _app_packet_process_info(const PH_ROW *row, LPCWSTR path);
PR_STRING _app_packet_bits_job_info(const PH_ROW *row);
DWORD _app_packet_query_blocked_audit(PBOOLEAN enabled);
DWORD _app_packet_set_audit_timer(ULONG minutes);
void _app_packet_restore_audit_timer(void);
ULONG _app_packet_update_audit_timer(PULONG minutes);
void _app_packet_stop_audit_timer(void);
BOOLEAN _app_packet_audit_enabled(void);
void _app_packet_resolve_audit(PH_ROW *rows, size_t count);
BOOLEAN _app_packet_setwatch(LPCWSTR path, BOOLEAN enabled);
VOID _app_packet_lost(VOID);
LONG64 _app_packet_pending_loss(VOID);
VOID _app_packet_request(PH_REQUEST *request, BOOLEAN cancel);
PH_RESULT *_app_packet_take(VOID);
BOOLEAN _app_packet_busy(VOID);
LONG _app_packet_revision(VOID);
LONG _app_packet_audit_revision(VOID);
VOID _app_packet_command(int type, int64_t app, int64_t cap, int days, int mib);
VOID _app_packet_error(WCHAR *buffer, SIZE_T count);
VOID _app_packet_hostname(const char *address, int af, WCHAR *out, int count);
VOID _app_packet_displayname(LPCWSTR path, WCHAR *out, int count);
LONG _app_packet_dns_revision(VOID);
VOID _app_packet_view_init(HWND hwnd);
VOID _app_packet_view_refresh(VOID);
VOID _app_packet_view_clear(VOID);
BOOLEAN _app_packet_view_command(INT id);
