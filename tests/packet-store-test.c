#include "packet_store.h"
#include "../vendor/sqlite/sqlite3.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static void sort_host(void *ctx, const char *address, int family, char *out, int size)
{
	(void)ctx;
	(void)family;
	if (strcmp(address, "10.0.0.2") == 0)
		snprintf(out, size, "zeta.example");
	else if (strcmp(address, "10.0.0.10") == 0)
		snprintf(out, size, "alpha.example");
}

static void test_ordering(const char *path)
{
	PH_STORE *store = NULL;
	PH_STORE *reader = NULL;
	PH_PAGE *page = calloc(1, sizeof(*page));
	PH_APPS *apps = calloc(1, sizeof(*apps));
	PH_SORT_KEYS keys = {0};
	keys.hostname = sort_host;
	assert(page && apps);
	char sort_path[4096];
	snprintf(sort_path, sizeof(sort_path), "%s-sort", path);
	remove(sort_path);
	assert(!ph_open(&store, sort_path, 0));

	PH_EVENT event = {.rule = "allow", .direction = 0, .protocol = 6};
	char app_path[48], address[24];
	assert(!ph_begin(store));
	for (int i = 0; i < 510; i++)
	{
		snprintf(app_path, sizeof(app_path), "C:\\%04d\\app-%04d.exe", 509 - i, i);
		snprintf(address, sizeof(address), "10.0.%d.%d", i / 256, i % 256);
		event.identity = event.path = app_path;
		event.remote = address;
		event.remote_port = 510 - i;
		event.timestamp = 1800000000 + i;
		assert(!ph_append(store, &event));
	}
	/* Only this app has two grouped events; latest recency still belongs to another app. */
	event.identity = event.path = "C:\\0509\\app-0000.exe";
	event.remote = "10.0.0.0";
	event.remote_port = 510;
	event.timestamp = 1800000000;
	assert(!ph_append(store, &event));
	assert(!ph_commit(store));
	assert(!ph_open(&reader, sort_path, 1));
	ph_set_sort_keys(reader, &keys);

	assert(!ph_apps(store, 0, "", 0, (PH_ORDER){PH_SORT_APP, 1}, apps));
	assert(apps->count == 500 && apps->more && strstr(apps->items[0].path, "app-0000.exe"));
	assert(!ph_apps(store, 0, "", 500, (PH_ORDER){PH_SORT_APP, 1}, apps));
	assert(apps->count == 10 && !apps->more && strstr(apps->items[0].path, "app-0500.exe"));
	assert(!ph_apps(store, 0, "", 0, (PH_ORDER){PH_SORT_COUNT, 0}, apps));
	assert(apps->items[0].count == 2 && strstr(apps->items[0].path, "app-0000.exe"));

	PH_QUERY query = {.direction = -1, .grouped = 1, .order = {PH_SORT_PORT, 1}};
	assert(!ph_query(reader, &query, page));
	assert(page->count == 500 && page->more && page->rows[0].remote_port == 1);
	query.offset = 500;
	assert(!ph_query(reader, &query, page));
	assert(page->count == 10 && page->rows[0].remote_port == 501);
	query.offset = 0;
	query.order.ascending = 0;
	assert(!ph_query(reader, &query, page));
	assert(page->rows[0].remote_port == 510 && page->rows[0].count == 2);
	query.order = (PH_ORDER){PH_SORT_REMOTE, 1};
	assert(!ph_query(reader, &query, page));
	assert(!strcmp(page->rows[2].remote, "10.0.0.2"));
	query.order = (PH_ORDER){PH_SORT_HOST, 1};
	assert(!ph_query(reader, &query, page));
	assert(!strcmp(page->rows[0].remote, "10.0.0.10"));
	assert(!strcmp(page->rows[1].remote, "10.0.0.2"));
	query.order.ascending = 0;
	assert(!ph_query(reader, &query, page));
	assert(!strcmp(page->rows[0].remote, "10.0.0.2"));
	query.grouped = 0;
	query.order = (PH_ORDER){PH_SORT_COUNT, 1};
	assert(!ph_query(reader, &query, page));
	assert(page->rows[0].id == 1 && page->rows[1].id == 2);
	query.order.ascending = 0;
	assert(!ph_query(reader, &query, page));
	assert(page->rows[0].id == 511 && page->rows[1].id == 510);
	assert(page->total == 511 && page->inbound == 511 && page->outbound == 0);

	/* A change committed by the writer must invalidate the reader's cached totals. */
	event.direction = 1;
	event.remote = "192.0.2.1";
	event.timestamp++;
	assert(!ph_begin(store));
	assert(!ph_append(store, &event));
	assert(!ph_commit(store));
	assert(!ph_query(reader, &query, page));
	assert(page->rows[0].id == 512 && page->total == 512 && page->outbound == 1);
	assert(page->destinations == 511);
	query.direction = 1;
	assert(!ph_query(reader, &query, page));
	assert(page->count == 1 && page->total == 1 && page->inbound == 0 && page->outbound == 1);

	ph_close(reader);
	ph_close(store);
	remove(sort_path);
	free(page);
	free(apps);
}

static void test_services(const char *path)
{
	PH_STORE *store = NULL;
	PH_PAGE *page = calloc(1, sizeof(*page));
	char service_path[4096];
	snprintf(service_path, sizeof(service_path), "%s-service", path);
	remove(service_path);
	assert(page && !ph_open(&store, service_path, 0));
	PH_EVENT event = {
		.identity = "svchost", .path = "C:\\Windows\\System32\\svchost.exe",
		.remote = "1.1.1.1", .remote_port = 443, .protocol = 6, .timestamp = 1800000000
	};
	assert(!ph_append(store, &event));
	ph_close(store);

	/* Simulate a saved v2 log, then reopen it through the normal upgrade path. */
	sqlite3 *db = NULL;
	assert(sqlite3_open(service_path, &db) == SQLITE_OK);
	assert(sqlite3_exec(db, "ALTER TABLE events DROP COLUMN service; ALTER TABLE events DROP COLUMN pid;"
		"ALTER TABLE events DROP COLUMN process_started; PRAGMA user_version=2;",
		NULL, NULL, NULL) == SQLITE_OK);
	sqlite3_close(db);
	assert(!ph_open(&store, service_path, 0));
	event.service = "BITS";
	assert(!ph_append(store, &event));
	assert(!ph_append(store, &event));
	event.service = "wuauserv";
	assert(!ph_append(store, &event));
	assert(!ph_set_packet_process(store, ph_inserted_id(store), 1234, event.timestamp - 5, "BITS"));
	assert(!ph_set_packet_process(store, 1, 4321, event.timestamp - 9, NULL));
	ph_close(store);

	assert(!ph_open(&store, service_path, 1));
	PH_QUERY query = {.direction = -1, .grouped = 1, .order = {PH_SORT_SERVICE, 1}};
	assert(!ph_query(store, &query, page));
	assert(page->count == 3 && page->total == 4);
	assert(!strcmp(page->rows[0].service, "BITS") && page->rows[0].count == 2);
	assert(!strcmp(page->rows[1].service, "wuauserv") && page->rows[1].count == 1);
	assert(!page->rows[2].service[0] && page->rows[2].id == 1);
	query.grouped = 0;
	query.order.ascending = 0;
	assert(!ph_query(store, &query, page));
	assert(page->count == 4 && !strcmp(page->rows[0].service, "wuauserv"));
	assert(!strcmp(page->rows[1].service, "BITS") && !page->rows[3].service[0]);
	assert(page->rows[0].pid == 1234 && page->rows[0].process_started == event.timestamp - 5);
	assert(page->rows[3].pid == 4321 && page->rows[3].id == 1);
	ph_close(store);
	remove(service_path);
	free(page);
}

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
	test_services(argv[1]);
	test_ordering(argv[1]);
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
	assert(!ph_apps(s, 0, "", 0, (PH_ORDER){0}, a));
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
	assert(!ph_apps(s, 0, "two", 0, (PH_ORDER){0}, a));
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
	assert(!ph_apps(s, 0, "cleared", 0, (PH_ORDER){0}, a));
	assert(a->count == 1);
	assert(!ph_clear(s, a->items[0].id));
	assert(!ph_begin(s));
	e.identity = "cleared";
	e.path = "cleared.exe";
	e.timestamp = now + 100;
	assert(!ph_append(s, &e));
	assert(!ph_commit(s));
	assert(!ph_apps(s, 0, "", 0, (PH_ORDER){0}, a));
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
	assert(!ph_apps(s, 0, "retention-B", 0, (PH_ORDER){0}, a));
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
	assert(!ph_apps(s, 0, "spam", 0, (PH_ORDER){0}, a));
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
	assert(!ph_apps(s, 0, "", 0, (PH_ORDER){0}, a));
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
	assert(!ph_apps(s, 0, "churn", 0, (PH_ORDER){0}, a));
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
