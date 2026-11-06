#include "packet_store.h"
#include "../vendor/sqlite/sqlite3.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
int main(int argc, char **argv)
{
	PH_STORE *s = NULL;
	PH_PAGE *p = calloc(1, sizeof(*p));
	PH_APPS *a = calloc(1, sizeof(*a));
	PH_EVENT e = {0};
	PH_QUERY q = {0};
	int i;
	int64_t now = 1800000000;
	assert(argc == 2);
	remove(argv[1]);
	assert(ph_open(&s, argv[1], 0) == 0);
	e.identity = "c:\\one\\same.exe";
	e.path = "C:\\One\\same.exe";
	e.rule = "block test";
	e.remote = "1.2.3.4";
	e.local = "10.0.0.1";
	e.user = "user";
	e.layer = "connect";
	e.protocol = 6;
	e.remote_port = 443;
	e.direction = 1;
	e.timestamp = now;
	assert(!ph_begin(s));
	for (i = 0; i < 10; i++)
	{
		e.timestamp = now + i;
		e.local_port = 40000 + i;
		assert(!ph_append(s, &e));
	}
	e.identity = "c:\\two\\same.exe";
	e.path = "C:\\Two\\same.exe";
	e.timestamp = now + 20;
	assert(!ph_append(s, &e));
	e.timestamp = now - 10;
	assert(!ph_append(s, &e));
	assert(!ph_commit(s));
	assert(!ph_apps(s, 0, "", 0, a));
	assert(a->count == 2);
	assert(a->items[0].count == 2);
	assert(a->items[0].last == now + 20);
	assert(a->items[1].count == 10);
	q.app = a->items[1].id;
	q.direction = -1;
	q.grouped = 1;
	assert(!ph_query(s, &q, p));
	assert(p->count == 1);
	assert(p->rows[0].count == 10);
	assert(p->rows[0].first == now);
	assert(p->rows[0].last == now + 9);
	assert(p->total == 10);
	q.grouped = 0;
	assert(!ph_query(s, &q, p));
	assert(p->count == 10);
	assert(p->rows[0].local_port == 40009);
	ph_close(s);
	s = NULL;
	assert(!ph_open(&s, argv[1], 0));
	assert(!ph_query(s, &q, p));
	assert(p->total == 10);
	assert(!ph_limits(s, q.app, 3, 7, 1024));
	while (ph_retention(s, now + 30, 1024) > 0)
	{
	}
	assert(!ph_query(s, &q, p));
	assert(p->total == 3);
	assert(!ph_apps(s, 0, "two", 0, a));
	assert(a->count == 1 && a->items[0].count == 2);
	assert(!ph_clear(s, q.app));
	assert(!ph_query(s, &q, p));
	assert(p->count == 0);
	/* Clearing must discard the old recency watermark, even for late replay. */
	assert(!ph_begin(s));
	e.identity = "cleared";
	e.path = "cleared.exe";
	e.timestamp = now + 200;
	assert(!ph_append(s, &e));
	e.identity = "middle";
	e.path = "middle.exe";
	e.timestamp = now + 150;
	assert(!ph_append(s, &e));
	assert(!ph_commit(s));
	assert(!ph_apps(s, 0, "cleared", 0, a));
	assert(a->count == 1);
	assert(!ph_clear(s, a->items[0].id));
	assert(!ph_begin(s));
	e.identity = "cleared";
	e.path = "cleared.exe";
	e.timestamp = now + 100;
	assert(!ph_append(s, &e));
	assert(!ph_commit(s));
	assert(!ph_apps(s, 0, "", 0, a));
	assert(!strcmp(a->items[0].path, "middle.exe"));
	/* A repeatedly dirtied low-ID app must not starve another app's retention. */
	assert(!ph_limits(s, 0, 3, 7, 1024));
	assert(!ph_begin(s));
	e.identity = "retention-B";
	e.path = "retention-B";
	for (i = 0; i < 21; i++)
		assert(!ph_append(s, &e));
	assert(!ph_commit(s));
	for (i = 0; i < 30; i++)
	{
		assert(!ph_begin(s));
		e.identity = "c:\\two\\same.exe";
		e.path = "C:\\Two\\same.exe";
		assert(!ph_append(s, &e));
		assert(!ph_commit(s));
		assert(ph_retention(s, now + 300, 1024) >= 0);
	}
	assert(!ph_apps(s, 0, "retention-B", 0, a));
	assert(a->count == 1 && a->items[0].count <= 3);
	assert(!ph_limits(s, 0, 200000, 7, 1024));
	/* Exact repeats retained; different actions/rules never grouped together. */
	assert(!ph_begin(s));
	e.identity = "spam";
	e.path = "spam.exe";
	e.timestamp = now;
	e.local_port = 42;
	for (i = 0; i < 200000; i++)
	{
		e.timestamp = now + i % 1000;
		assert(!ph_append(s, &e));
	}
	assert(!ph_commit(s));
	assert(!ph_apps(s, 0, "spam", 0, a));
	assert(a->count == 1 && a->items[0].count == 200000);
	q.app = a->items[0].id;
	q.grouped = 1;
	assert(!ph_query(s, &q, p));
	assert(p->count == 1 && p->rows[0].count == 200000);
	assert(!ph_begin(s));
	e.allow = 1;
	assert(!ph_append(s, &e));
	e.allow = 0;
	e.rule = "another rule";
	assert(!ph_append(s, &e));
	assert(!ph_commit(s));
	assert(!ph_query(s, &q, p));
	assert(p->count == 2 && p->total == 200001);
	q.grouped = 0;
	q.offset = 500;
	assert(!ph_query(s, &q, p));
	assert(p->count == 500 && p->more);
	/* Grouped results are paged even with hundreds of distinct destinations. */
	char destination[64];
	assert(!ph_begin(s));
	for (i = 0; i < 600; i++)
	{
		snprintf(destination, sizeof(destination), "192.0.%d.%d", i / 256, i % 256);
		e.remote = destination;
		assert(!ph_append(s, &e));
	}
	assert(!ph_commit(s));
	q.grouped = 1;
	q.offset = 0;
	assert(!ph_query(s, &q, p));
	assert(p->count == 500 && p->more);
	q.grouped = 0;
	q.offset = 0;
	assert(!ph_limits(s, q.app, 5, 7, 1024));
	while (ph_retention(s, now + 1000, 1024) > 0)
	{
	}
	assert(!ph_query(s, &q, p));
	assert(p->total <= 5);
	/* Eviction and its accounting must either both commit or both roll back. */
	sqlite3 *inject = NULL;
	assert(sqlite3_open(argv[1], &inject) == SQLITE_OK);
	assert(sqlite3_exec(inject,
						"CREATE TRIGGER fail_counter BEFORE UPDATE ON totals BEGIN SELECT "
						"RAISE(FAIL,'injected accounting failure'); END;",
						NULL, NULL, NULL) == SQLITE_OK);
	assert(!ph_query(s, &q, p));
	int64_t before_failure = p->total;
	assert(before_failure > 0);
	assert(ph_retention(s, now + 8 * 86400, 1024) < 0);
	assert(!ph_query(s, &q, p));
	assert(p->total == before_failure);
	assert(sqlite3_exec(inject, "DROP TRIGGER fail_counter", NULL, NULL, NULL) == SQLITE_OK);
	sqlite3_close(inject);
	while (ph_retention(s, now + 8 * 86400, 1024) > 0)
	{
	}
	assert(!ph_apps(s, 0, "", 0, a));
	assert(a->count == 0);
	assert(!ph_gap(s, 7, now, "overload"));
	assert(ph_lost(s) == 7);
	/* Metadata churn must be reclaimed along with events under the byte budget. */
	assert(!ph_limits(s, 0, 200000, 7, 16));
	assert(!ph_begin(s));
	char identity[4096];
	memset(identity, 'x', 4095);
	identity[4095] = 0;
	e.rule = "";
	e.allow = 0;
	for (i = 0; i < 2200; i++)
	{
		char prefix[32];
		snprintf(prefix, 32, "churn-%08d", i);
		memcpy(identity, prefix, strlen(prefix));
		e.identity = identity;
		e.path = identity;
		assert(!ph_append(s, &e));
	}
	assert(!ph_commit(s));
	assert(!ph_apps(s, 0, "churn", 0, a));
	assert(a->count == 500 && a->more);
	for (i = 0; i < 3000; i++)
		assert(ph_retention(s, now + 1000, 1024) >= 0);
	ph_close(s);
	FILE *file = fopen(argv[1], "rb");
	assert(file);
	fseek(file, 0, SEEK_END);
	assert(ftell(file) < 16 * 1024 * 1024);
	fclose(file);
	assert(!ph_open(&s, argv[1], 1));
	assert(ph_begin(s) != 0);
	ph_close(s);

	/* A held reader must not allow unbounded journal growth; reclaim after release. */
	char walpath[4096];
	snprintf(walpath, sizeof(walpath), "%s-waltest", argv[1]);
	remove(walpath);
	assert(!ph_open(&s, walpath, 0));
	assert(!ph_limits(s, 0, 3, 7, 16));
	char bigrule[4096];
	memset(bigrule, 'r', 4095);
	bigrule[4095] = 0;
	e.identity = "wal-app";
	e.path = "wal-app";
	e.rule = bigrule;
	e.timestamp = now;
	assert(!ph_begin(s));
	assert(!ph_append(s, &e));
	assert(!ph_commit(s));
	sqlite3 *held = NULL;
	assert(sqlite3_open(walpath, &held) == SQLITE_OK);
	assert(sqlite3_exec(held, "BEGIN; SELECT * FROM events", NULL, NULL, NULL) == SQLITE_OK);
	int backpressured = 0;
	for (i = 0; i < 1000; i++)
	{
		int rc = ph_begin(s);
		if (rc == SQLITE_FULL)
		{
			backpressured = 1;
			break;
		}
		assert(!rc);
		assert(!ph_append(s, &e));
		assert(!ph_commit(s));
		assert(ph_retention(s, now, 1024) >= 0);
	}
	assert(backpressured);
	sqlite3_exec(held, "ROLLBACK", NULL, NULL, NULL);
	sqlite3_close(held);
	for (i = 0; i < 20; i++)
		assert(ph_retention(s, now, 1024) >= 0);
	ph_close(s);
	char journalpath[4104];
	snprintf(journalpath, sizeof(journalpath), "%s-wal", walpath);
	file = fopen(journalpath, "rb");
	if (file)
	{
		fseek(file, 0, SEEK_END);
		assert(ftell(file) < 16 * 1024 * 1024);
		fclose(file);
	}
	/* Unknown or old schemas are rejected before modifying existing content. */
	sqlite3 *fixture = NULL;
	char fixture_path[4096];
	snprintf(fixture_path, sizeof(fixture_path), "%s-unsupported", argv[1]);
	remove(fixture_path);
	assert(sqlite3_open(fixture_path, &fixture) == SQLITE_OK);
	assert(sqlite3_exec(
			   fixture,
			   "CREATE TABLE preserve_me(v); INSERT INTO preserve_me VALUES(42); PRAGMA user_version=1;",
			   NULL, NULL, NULL) == SQLITE_OK);
	sqlite3_close(fixture);
	assert(ph_open(&s, fixture_path, 0) == SQLITE_MISMATCH && s == NULL);
	assert(sqlite3_open(fixture_path, &fixture) == SQLITE_OK);
	sqlite3_stmt *st = NULL;
	assert(sqlite3_prepare_v2(fixture, "SELECT v FROM preserve_me", -1, &st, NULL) == SQLITE_OK);
	assert(sqlite3_step(st) == SQLITE_ROW && sqlite3_column_int(st, 0) == 42);
	sqlite3_finalize(st);
	sqlite3_close(fixture);
	remove(fixture_path);
	free(p);
	free(a);
	puts("packet store: persistence, identities, raw/grouped, recency, paging, retention, gaps and 200k "
		 "events passed");
	return 0;
}
