#include "packet_store.h"
#include "../vendor/sqlite/sqlite3.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <sys/stat.h>
#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif
struct PH_STORE
{
	sqlite3 *db;
	sqlite3_stmt *app, *insert;
	int readonly;
};
static int sql(PH_STORE *s, const char *text)
{
	return sqlite3_exec(s->db, text, NULL, NULL, NULL);
}
static int prep(PH_STORE *s, sqlite3_stmt **p, const char *text)
{
	return sqlite3_prepare_v2(s->db, text, -1, p, NULL);
}
static void text(sqlite3_stmt *p, int i, const char *s)
{
	sqlite3_bind_text(p, i, s ? s : "", -1, SQLITE_TRANSIENT);
}
static int64_t num(sqlite3_stmt *p, int i)
{
	return sqlite3_column_int64(p, i);
}
static void copycol(sqlite3_stmt *p, int i, char *b, size_t n)
{
	const char *t = (const char *)sqlite3_column_text(p, i);
	if (!t)
		t = "";
	snprintf(b, n, "%s", t);
}
static int scalar(PH_STORE *s, const char *q, int64_t *v)
{
	sqlite3_stmt *p = NULL;
	int rc = prep(s, &p, q);
	if (!rc)
	{
		rc = sqlite3_step(p);
		if (rc == SQLITE_ROW)
		{
			*v = num(p, 0);
			rc = 0;
		}
	}
	sqlite3_finalize(p);
	return rc;
}
static int64_t physical_file_size(const char *path)
{
#ifdef _WIN32
	int n = MultiByteToWideChar(CP_UTF8, 0, path, -1, NULL, 0);
	wchar_t *wide = malloc((size_t)n * sizeof(wchar_t));
	struct _stat64 st;
	int64_t result = 0;
	if (wide)
	{
		MultiByteToWideChar(CP_UTF8, 0, path, -1, wide, n);
		if (!_wstat64(wide, &st))
			result = st.st_size;
		free(wide);
	}
	return result;
#else
	struct stat st;
	return stat(path, &st) ? 0 : st.st_size;
#endif
}
int64_t ph_storage_bytes(PH_STORE *s)
{
	const char *path = sqlite3_db_filename(s->db, "main");
	if (!path)
		return 0;
	int64_t bytes = physical_file_size(path);
	char *side = sqlite3_mprintf("%s-wal", path);
	if (side)
	{
		bytes += physical_file_size(side);
		sqlite3_free(side);
	}
	side = sqlite3_mprintf("%s-shm", path);
	if (side)
	{
		bytes += physical_file_size(side);
		sqlite3_free(side);
	}
	return bytes;
}
static void reclaim_journal(PH_STORE *s)
{
	int64_t cap = 0;
	int days = 0, mib = 1024;
	ph_getlimits(s, 0, &cap, &days, &mib);
	int log = 0, done = 0;
	sqlite3_wal_checkpoint_v2(s->db, NULL, SQLITE_CHECKPOINT_PASSIVE, &log, &done);
	/* TRUNCATE only after passive checkpoint proves no reader pins old frames.
	   The busy handler bounds a race with a newly starting reader. */
	const char *path = sqlite3_db_filename(s->db, "main");
	char *wal = sqlite3_mprintf("%s-wal", path ? path : "");
	int64_t bytes = wal ? physical_file_size(wal) : 0;
	sqlite3_free(wal);
	if (log == done && bytes > (int64_t)mib * 1024 * 1024 / 16)
		sqlite3_wal_checkpoint_v2(s->db, NULL, SQLITE_CHECKPOINT_TRUNCATE, NULL, NULL);
}
const char *ph_error(PH_STORE *s)
{
	return s ? sqlite3_errmsg(s->db) : "History unavailable";
}
int ph_open(PH_STORE **out, const char *path, int ro)
{
	PH_STORE *s = calloc(1, sizeof(*s));
	int rc;
	int64_t version = 0;
	*out = NULL;
	if (!s)
		return SQLITE_NOMEM;
	rc = sqlite3_open_v2(path, &s->db,
						 (ro ? SQLITE_OPEN_READONLY : SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE) |
							 SQLITE_OPEN_FULLMUTEX,
						 NULL);
	if (rc)
		goto fail;
	s->readonly = ro;
	sqlite3_busy_timeout(s->db, 250);
	sqlite3_limit(s->db, SQLITE_LIMIT_LENGTH, 1024 * 1024);
	rc = scalar(s, "PRAGMA user_version", &version);
	if (rc)
		goto fail;
	if (version != 0 && version != 2)
	{
		rc = SQLITE_MISMATCH;
		goto fail;
	}
	if (ro && version != 2)
	{
		rc = SQLITE_MISMATCH;
		goto fail;
	}
	if (!ro && version == 0)
	{
		int64_t tables = 0;
		rc = scalar(s, "SELECT count(*) FROM sqlite_master WHERE type='table' AND name NOT LIKE 'sqlite_%'",
					&tables);
		if (rc || tables)
		{
			if (!rc)
				rc = SQLITE_MISMATCH;
			goto fail;
		}
	}
	if (!ro)
	{
		rc = sql(s, "PRAGMA auto_vacuum=INCREMENTAL; PRAGMA journal_mode=WAL; PRAGMA synchronous=FULL; "
					"PRAGMA wal_autocheckpoint=256; PRAGMA cache_size=-4096; PRAGMA temp_store=FILE; PRAGMA "
					"journal_size_limit=1048576;");
		if (rc)
			goto fail;
		rc = sql(
			s,
			"BEGIN;"
			"CREATE TABLE IF NOT EXISTS apps(id INTEGER PRIMARY KEY AUTOINCREMENT,identity TEXT NOT NULL "
			"UNIQUE,path TEXT NOT NULL,cap INTEGER);"
			"CREATE TABLE IF NOT EXISTS events(id INTEGER PRIMARY KEY AUTOINCREMENT,app INTEGER NOT NULL,t "
			"INTEGER NOT NULL,allow INTEGER NOT NULL,dir INTEGER NOT NULL,proto INTEGER NOT NULL,af INTEGER "
			"NOT NULL,remote TEXT NOT NULL,rport INTEGER NOT NULL,local TEXT NOT NULL,lport INTEGER NOT "
			"NULL,rule TEXT NOT NULL,filter INTEGER NOT NULL,user TEXT NOT NULL,layer TEXT NOT NULL);"
			"CREATE INDEX IF NOT EXISTS event_app_time ON events(app,t DESC,id DESC);"
			"CREATE INDEX IF NOT EXISTS event_view_time ON events(allow,t DESC,id DESC);"
			"CREATE INDEX IF NOT EXISTS event_app_view ON events(app,allow,t DESC,id DESC);"
			"CREATE INDEX IF NOT EXISTS event_time ON events(t,id);"
			"CREATE TABLE IF NOT EXISTS summary(app INTEGER NOT NULL,allow INTEGER NOT NULL,n INTEGER NOT "
			"NULL,last INTEGER NOT NULL,seq INTEGER NOT NULL,PRIMARY KEY(app,allow));"
			"CREATE TABLE IF NOT EXISTS settings(id INTEGER PRIMARY KEY CHECK(id=1),cap INTEGER NOT "
			"NULL,days INTEGER NOT NULL,mib INTEGER NOT NULL);"
			"INSERT OR IGNORE INTO settings VALUES(1,200000,7,1024);"
			"CREATE TABLE IF NOT EXISTS totals(id INTEGER PRIMARY KEY CHECK(id=1),lost INTEGER NOT NULL,age "
			"INTEGER NOT NULL,cap INTEGER NOT NULL,space INTEGER NOT NULL); INSERT OR IGNORE INTO totals "
			"VALUES(1,0,0,0,0);"
			"CREATE TABLE IF NOT EXISTS gaps(id INTEGER PRIMARY KEY,t INTEGER NOT NULL,n INTEGER NOT "
			"NULL,reason TEXT NOT NULL);"
			"CREATE TABLE IF NOT EXISTS dirty(app INTEGER PRIMARY KEY,queued INTEGER NOT NULL);"
			"CREATE TRIGGER IF NOT EXISTS event_add AFTER INSERT ON events BEGIN "
			"INSERT INTO summary VALUES(new.app,new.allow,1,new.t,new.id) ON CONFLICT(app,allow) DO UPDATE "
			"SET n=n+1,seq=CASE WHEN new.t>=last THEN new.id ELSE seq END,last=max(last,new.t);"
			"INSERT OR IGNORE INTO dirty VALUES(new.app,new.id); END;"
			"CREATE TRIGGER IF NOT EXISTS event_remove AFTER DELETE ON events BEGIN UPDATE summary SET n=n-1 "
			"WHERE app=old.app AND allow=old.allow; DELETE FROM summary WHERE app=old.app AND "
			"allow=old.allow AND n=0; END;"
			"PRAGMA user_version=2; COMMIT;");
		if (rc)
			goto fail;
		rc = prep(s, &s->app,
				  "INSERT INTO apps(identity,path) VALUES(?,?) ON CONFLICT(identity) DO UPDATE SET "
				  "path=excluded.path RETURNING id");
		if (rc)
			goto fail;
		rc = prep(
			s, &s->insert,
			"INSERT INTO events(app,t,allow,dir,proto,af,remote,rport,local,lport,rule,filter,user,layer) "
			"VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?)");
		if (rc)
			goto fail;
	}
	*out = s;
	return 0;
fail:
	ph_close(s);
	return rc;
}
void ph_close(PH_STORE *s)
{
	if (!s)
		return;
	sqlite3_finalize(s->app);
	sqlite3_finalize(s->insert);
	sqlite3_close(s->db);
	free(s);
}
int ph_begin(PH_STORE *s)
{
	if (s->readonly)
		return SQLITE_READONLY;
	int64_t cap = 0;
	int days = 0, mib = 1024;
	int rc = ph_getlimits(s, 0, &cap, &days, &mib);
	if (rc)
		return rc;
	if (ph_storage_bytes(s) > (int64_t)mib * 1024 * 1024 * 8 / 10)
	{
		reclaim_journal(s);
		if (ph_storage_bytes(s) > (int64_t)mib * 1024 * 1024 * 9 / 10)
			return SQLITE_FULL;
	}
	return sql(s, "BEGIN IMMEDIATE");
}
int ph_begin_cleanup(PH_STORE *s)
{
	return s->readonly ? SQLITE_READONLY : sql(s, "BEGIN IMMEDIATE");
}

int ph_commit(PH_STORE *s)
{
	return sql(s, "COMMIT");
}
void ph_rollback(PH_STORE *s)
{
	sql(s, "ROLLBACK");
}
void ph_cancel(PH_STORE *s)
{
	if (s)
		sqlite3_interrupt(s->db);
}
int ph_append(PH_STORE *s, const PH_EVENT *e)
{
	sqlite3_stmt *p = s->app;
	int rc;
	int64_t id;
	if (!p)
		return SQLITE_READONLY;
	sqlite3_reset(p);
	sqlite3_clear_bindings(p);
	text(p, 1, e->identity);
	text(p, 2, e->path);
	rc = sqlite3_step(p);
	if (rc != SQLITE_ROW)
		return rc;
	id = num(p, 0);
	sqlite3_reset(p);
	p = s->insert;
	sqlite3_reset(p);
	sqlite3_clear_bindings(p);
	sqlite3_bind_int64(p, 1, id);
	sqlite3_bind_int64(p, 2, e->timestamp);
	sqlite3_bind_int(p, 3, !!e->allow);
	sqlite3_bind_int(p, 4, e->direction);
	sqlite3_bind_int(p, 5, e->protocol);
	sqlite3_bind_int(p, 6, e->af);
	text(p, 7, e->remote);
	sqlite3_bind_int(p, 8, e->remote_port);
	text(p, 9, e->local);
	sqlite3_bind_int(p, 10, e->local_port);
	text(p, 11, e->rule);
	sqlite3_bind_int64(p, 12, e->filter);
	text(p, 13, e->user);
	text(p, 14, e->layer);
	rc = sqlite3_step(p);
	sqlite3_reset(p);
	return rc == SQLITE_DONE ? 0 : rc;
}
int ph_apps(PH_STORE *s, int allow, const char *filter, int64_t offset, PH_APPS *out)
{
	sqlite3_stmt *p = NULL;
	int rc;
	memset(out, 0, sizeof(*out));
	rc = prep(s, &p,
			  "SELECT a.id,a.path,v.n,v.last,v.seq,count(*) OVER() FROM apps a JOIN summary v ON v.app=a.id "
			  "WHERE v.allow=? AND v.n>0 AND instr(lower(a.path),lower(?))>0 ORDER BY v.last DESC,v.seq "
			  "DESC,a.id LIMIT 501 OFFSET ?");
	if (rc)
		return rc;
	sqlite3_bind_int(p, 1, allow);
	text(p, 2, filter);
	sqlite3_bind_int64(p, 3, offset);
	while ((rc = sqlite3_step(p)) == SQLITE_ROW)
	{
		PH_APP *a;
		if (out->count == PH_PAGE_SIZE)
		{
			out->more = 1;
			break;
		}
		a = &out->items[out->count++];
		a->id = num(p, 0);
		copycol(p, 1, a->path, sizeof(a->path));
		a->count = num(p, 2);
		a->last = num(p, 3);
		a->sequence = num(p, 4);
		out->total = num(p, 5);
	}
	sqlite3_finalize(p);
	return rc == SQLITE_DONE || rc == SQLITE_ROW ? 0 : rc;
}
static void querybind(sqlite3_stmt *p, const PH_QUERY *q)
{
	sqlite3_bind_int64(p, 1, q->app);
	sqlite3_bind_int(p, 2, q->allow);
	sqlite3_bind_int(p, 3, q->direction);
	sqlite3_bind_int64(p, 4, q->offset);
}
int ph_query(PH_STORE *s, const PH_QUERY *q, PH_PAGE *out)
{
	sqlite3_stmt *p = NULL;
	int rc;
	char query[2400];
	const char *where =
		q->app
			? " FROM events e JOIN apps a ON a.id=e.app WHERE e.app=?1 AND e.allow=?2 AND (?3=-1 OR "
			  "e.dir=?3) "
			: " FROM events e JOIN apps a ON a.id=e.app WHERE ?1=0 AND e.allow=?2 AND (?3=-1 OR e.dir=?3) ";
	memset(out, 0, sizeof(*out));
	rc = sql(s, "BEGIN");
	if (rc)
		return rc;
	snprintf(query, sizeof(query),
			 "SELECT count(*),coalesce(sum(dir=0),0),coalesce(sum(dir=1),0),count(DISTINCT "
			 "remote||':'||rport||':'||proto),coalesce(min(t),0),coalesce(max(t),0)%s",
			 where);
	rc = prep(s, &p, query);
	if (rc)
		goto done;
	querybind(p, q);
	rc = sqlite3_step(p);
	if (rc != SQLITE_ROW)
		goto done;
	out->total = num(p, 0);
	out->inbound = num(p, 1);
	out->outbound = num(p, 2);
	out->destinations = num(p, 3);
	out->oldest = num(p, 4);
	out->newest = num(p, 5);
	sqlite3_finalize(p);
	p = NULL;
	snprintf(query, sizeof(query),
			 "SELECT "
			 "max(e.id),e.app,%s,a.path,e.remote,e.rport,e.local,%s,e.rule,e.filter,e.user,e.layer,e.dir,e."
			 "proto,e.af%s%s ORDER BY %s DESC,max(e.id) DESC LIMIT 501 OFFSET ?4",
			 q->grouped ? "count(*),min(e.t),max(e.t)" : "1,e.t,e.t", q->grouped ? "0" : "e.lport", where,
			 q->grouped
				 ? " GROUP BY e.app,e.af,e.remote,e.rport,e.local,e.proto,e.dir,e.rule,e.filter,e.allow"
				 : " GROUP BY e.id",
			 q->grouped ? "max(e.t)" : "e.t");
	rc = prep(s, &p, query);
	if (rc)
		goto done;
	querybind(p, q);
	while ((rc = sqlite3_step(p)) == SQLITE_ROW)
	{
		PH_ROW *r;
		if (out->count == PH_PAGE_SIZE)
		{
			out->more = 1;
			break;
		}
		r = &out->rows[out->count++];
		r->id = num(p, 0);
		r->app = num(p, 1);
		r->count = num(p, 2);
		r->first = num(p, 3);
		r->last = num(p, 4);
		copycol(p, 5, r->path, sizeof(r->path));
		copycol(p, 6, r->remote, sizeof(r->remote));
		r->remote_port = (int)num(p, 7);
		copycol(p, 8, r->local, sizeof(r->local));
		r->local_port = (int)num(p, 9);
		copycol(p, 10, r->rule, sizeof(r->rule));
		r->filter = num(p, 11);
		copycol(p, 12, r->user, sizeof(r->user));
		copycol(p, 13, r->layer, sizeof(r->layer));
		r->direction = (int)num(p, 14);
		r->protocol = (int)num(p, 15);
		r->af = (int)num(p, 16);
	}
	if (rc == SQLITE_DONE || rc == SQLITE_ROW)
		rc = 0;
done:
	sqlite3_finalize(p);
	sql(s, "ROLLBACK");
	return rc;
}
int ph_clear(PH_STORE *s, int64_t app)
{
	sqlite3_stmt *p = NULL;
	int rc = prep(s, &p, "DELETE FROM events WHERE (?=0 OR app=?)");
	if (rc)
		return rc;
	sqlite3_bind_int64(p, 1, app);
	sqlite3_bind_int64(p, 2, app);
	rc = sqlite3_step(p);
	sqlite3_finalize(p);
	return rc == SQLITE_DONE ? 0 : rc;
}
int ph_getlimits(PH_STORE *s, int64_t app, int64_t *count, int *days, int *mib)
{
	sqlite3_stmt *p = NULL;
	int rc = prep(s, &p, "SELECT coalesce((SELECT cap FROM apps WHERE id=?),cap),days,mib FROM settings");
	if (rc)
		return rc;
	sqlite3_bind_int64(p, 1, app);
	rc = sqlite3_step(p);
	if (rc == SQLITE_ROW)
	{
		*count = num(p, 0);
		*days = (int)num(p, 1);
		*mib = (int)num(p, 2);
		rc = 0;
	}
	sqlite3_finalize(p);
	return rc;
}
int ph_limits(PH_STORE *s, int64_t app, int64_t count, int days, int mib)
{
	sqlite3_stmt *p = NULL;
	int rc;
	if (count < 1 || days < 1 || mib < 16)
		return SQLITE_RANGE;
	rc = prep(s, &p, app ? "UPDATE apps SET cap=? WHERE id=?" : "UPDATE settings SET cap=?");
	if (rc)
		return rc;
	sqlite3_bind_int64(p, 1, count);
	if (app)
		sqlite3_bind_int64(p, 2, app);
	rc = sqlite3_step(p);
	sqlite3_finalize(p);
	if (rc != SQLITE_DONE)
		return rc;
	rc = prep(s, &p, "UPDATE settings SET days=?,mib=?");
	if (rc)
		return rc;
	sqlite3_bind_int(p, 1, days);
	sqlite3_bind_int(p, 2, mib);
	rc = sqlite3_step(p);
	sqlite3_finalize(p);
	if (rc == SQLITE_DONE)
		rc = sql(s, "INSERT OR IGNORE INTO dirty SELECT id,0 FROM apps");
	return rc;
}
static int remove_sql(PH_STORE *s, const char *q, int64_t x, int64_t y, int budget)
{
	sqlite3_stmt *p = NULL;
	int rc = prep(s, &p, q), n = 0;
	if (rc)
		return -rc;
	sqlite3_bind_int64(p, 1, x);
	sqlite3_bind_int64(p, 2, y);
	sqlite3_bind_int(p, 3, budget);
	rc = sqlite3_step(p);
	n = sqlite3_changes(s->db);
	sqlite3_finalize(p);
	return rc == SQLITE_DONE ? n : -rc;
}
static int retention_step(PH_STORE *s, int64_t now, int budget)
{
	sqlite3_stmt *p = NULL;
	int64_t cap = 200000, app = 0, pages = 0, size = 0, freepages = 0;
	int days = 7, mib = 1024, rc, n;
	rc = ph_getlimits(s, 0, &cap, &days, &mib);
	if (rc)
		return -rc;
	n = remove_sql(s,
				   "DELETE FROM events WHERE id IN(SELECT id FROM events WHERE t<?1 ORDER BY t,id LIMIT ?3)",
				   now - (int64_t)days * 86400, 0, budget);
	if (n > 0)
	{
		char b[128];
		snprintf(b, sizeof(b), "UPDATE totals SET age=age+%d", n);
		rc = sql(s, b);
		if (rc)
			return -rc;
	}
	if (n)
		return n;
	rc = prep(s, &p,
			  "SELECT d.app,coalesce(a.cap,s.cap) FROM dirty d JOIN apps a ON a.id=d.app CROSS JOIN settings "
			  "s ORDER BY d.queued,d.app LIMIT 1");
	if (rc)
		return -rc;
	rc = sqlite3_step(p);
	if (rc == SQLITE_ROW)
	{
		app = num(p, 0);
		cap = num(p, 1);
	}
	sqlite3_finalize(p);
	if (app)
	{
		n = remove_sql(s,
					   "DELETE FROM events WHERE id IN(SELECT id FROM events WHERE app=?1 ORDER BY t DESC,id "
					   "DESC LIMIT ?3 OFFSET ?2)",
					   app, cap, budget);
		if (n > 0)
		{
			rc = remove_sql(s,
							"UPDATE dirty SET queued=(SELECT coalesce(max(id),0)+1 FROM events) WHERE app=?1",
							app, 0, 0);
			if (rc < 0)
				return rc;
			char b[128];
			snprintf(b, sizeof(b), "UPDATE totals SET cap=cap+%d", n);
			rc = sql(s, b);
			if (rc)
				return -rc;
		}
		if (n)
			return n;
		rc = remove_sql(s, "DELETE FROM dirty WHERE app=?1", app, 0, 0);
		if (rc < 0)
			return rc;
	}
	scalar(s, "PRAGMA page_count", &pages);
	scalar(s, "PRAGMA page_size", &size);
	scalar(s, "PRAGMA freelist_count", &freepages);
	/* Leave 20% headroom for WAL/index/temp growth. Reclaim free pages incrementally. */
	if ((pages - freepages) * size > (int64_t)mib * 1024 * 1024 * 8 / 10)
	{
		/* Orphan identities/index entries can dominate storage after app churn. */
		int cleaned = remove_sql(s,
								 "DELETE FROM apps WHERE id IN(SELECT a.id FROM apps a WHERE a.cap IS NULL "
								 "AND NOT EXISTS(SELECT 1 FROM events e WHERE e.app=a.id) LIMIT ?3)",
								 0, 0, budget);
		if (cleaned > 0)
		{
			rc = sql(s, "DELETE FROM dirty WHERE NOT EXISTS(SELECT 1 FROM apps a WHERE a.id=dirty.app)");
			if (rc)
				return -rc;
			rc = sql(s, "DELETE FROM summary WHERE n<=0");
			if (rc)
				return -rc;
			rc = sql(s, "PRAGMA incremental_vacuum(256)");
			if (rc)
				return -rc;
			return cleaned;
		}
		n = remove_sql(s, "DELETE FROM events WHERE id IN(SELECT id FROM events ORDER BY t,id LIMIT ?3)", 0,
					   0, budget);
		if (n > 0)
		{
			char b[128];
			snprintf(b, sizeof(b), "UPDATE totals SET space=space+%d", n);
			rc = sql(s, b);
			if (rc)
				return -rc;
		}
		if (n)
			return n;
	}
	if (freepages)
	{
		rc = sql(s, "PRAGMA incremental_vacuum(256)");
		if (rc)
			return -rc;
	}
	return app || freepages ? 1 : 0;
}
int ph_retention(PH_STORE *s, int64_t now, int budget)
{
	int rc = ph_begin_cleanup(s);
	if (rc)
		return -rc;
	int n = retention_step(s, now, budget);
	if (n < 0)
	{
		ph_rollback(s);
		return n;
	}
	rc = ph_commit(s);
	if (rc)
	{
		ph_rollback(s);
		return -rc;
	}
	reclaim_journal(s);
	return n;
}
int ph_gap(PH_STORE *s, int64_t count, int64_t now, const char *reason)
{
	sqlite3_stmt *p = NULL;
	int rc = ph_begin(s);
	if (rc)
		return rc;
	rc = prep(s, &p, "INSERT INTO gaps(t,n,reason) VALUES(?,?,?)");
	if (rc)
		goto done;
	sqlite3_bind_int64(p, 1, now);
	sqlite3_bind_int64(p, 2, count);
	text(p, 3, reason);
	rc = sqlite3_step(p);
	sqlite3_finalize(p);
	p = NULL;
	if (rc != SQLITE_DONE)
		goto done;
	rc = prep(s, &p, "UPDATE totals SET lost=lost+?");
	if (rc)
		goto done;
	sqlite3_bind_int64(p, 1, count);
	rc = sqlite3_step(p);
	sqlite3_finalize(p);
	p = NULL;
	if (rc != SQLITE_DONE)
		goto done;
	rc = sql(s, "DELETE FROM gaps WHERE id NOT IN(SELECT id FROM gaps ORDER BY id DESC LIMIT 1024)");
done:
	sqlite3_finalize(p);
	if (!rc)
		rc = ph_commit(s);
	if (rc)
		ph_rollback(s);
	return rc;
}
int64_t ph_lost(PH_STORE *s)
{
	int64_t n = 0;
	scalar(s, "SELECT lost FROM totals", &n);
	return n;
}
int64_t ph_evicted(PH_STORE *s)
{
	int64_t n = 0;
	scalar(s, "SELECT age+cap+space FROM totals", &n);
	return n;
}
