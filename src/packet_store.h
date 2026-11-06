/* Durable packet history. UTF-8 at this boundary; no Win32/UI dependencies. */
#pragma once
#include <stdint.h>
#ifdef _WIN32
#define PH_CALL __cdecl
#else
#define PH_CALL
#endif
#define PH_PAGE_SIZE 500
#define PH_TEXT 4096
#define PH_RULE 512
typedef struct PH_STORE PH_STORE;
typedef struct
{
	const char *identity, *path, *rule, *remote, *local, *user, *layer;
	int64_t timestamp, filter;
	int allow, direction, protocol, af, local_port, remote_port;
} PH_EVENT;
typedef struct
{
	int64_t id, count, last, sequence;
	char path[PH_TEXT];
} PH_APP;
typedef struct
{
	int count, more;
	int64_t total;
	PH_APP items[PH_PAGE_SIZE];
} PH_APPS;
typedef struct
{
	int64_t app, offset;
	int allow, direction, grouped;
} PH_QUERY;
typedef struct
{
	int64_t id, app, count, first, last, filter;
	int direction, protocol, af, local_port, remote_port;
	char path[PH_TEXT], remote[64], local[64], rule[PH_RULE], user[256], layer[256];
} PH_ROW;
typedef struct
{
	int count, more;
	int64_t total, inbound, outbound, destinations, oldest, newest;
	PH_ROW rows[PH_PAGE_SIZE];
} PH_PAGE;
int PH_CALL ph_open(PH_STORE **out, const char *path, int readonly);
void PH_CALL ph_close(PH_STORE *s);
const char *PH_CALL ph_error(PH_STORE *s);
int PH_CALL ph_begin(PH_STORE *s);
int PH_CALL ph_begin_cleanup(PH_STORE *s);
int64_t PH_CALL ph_storage_bytes(PH_STORE *s);
int PH_CALL ph_append(PH_STORE *s, const PH_EVENT *e);
int PH_CALL ph_commit(PH_STORE *s);
void PH_CALL ph_rollback(PH_STORE *s);
int PH_CALL ph_apps(PH_STORE *s, int allow, const char *filter, int64_t offset, PH_APPS *out);
int PH_CALL ph_query(PH_STORE *s, const PH_QUERY *q, PH_PAGE *out);
int PH_CALL ph_clear(PH_STORE *s, int64_t app);
int PH_CALL ph_limits(PH_STORE *s, int64_t app, int64_t count, int days, int mib);
int PH_CALL ph_getlimits(PH_STORE *s, int64_t app, int64_t *count, int *days, int *mib);
/* Returns number removed (bounded by budget) or a negative SQLite error. */
int PH_CALL ph_retention(PH_STORE *s, int64_t now, int budget);
int PH_CALL ph_gap(PH_STORE *s, int64_t count, int64_t now, const char *reason);
int64_t PH_CALL ph_lost(PH_STORE *s);
int64_t PH_CALL ph_evicted(PH_STORE *s);
void PH_CALL ph_cancel(PH_STORE *s);
