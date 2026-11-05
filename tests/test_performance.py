"""Run production C hot paths with small, deterministic platform shims.

Requires a C compiler on PATH. No Windows SDK needed for these algorithm/lifetime tests.
"""
import re
import os
import shlex
import pathlib
import subprocess
import tempfile
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[1]


def function(path, name):
    source = (ROOT / path).read_text(encoding='utf-8-sig')
    pos = re.search(r'^(?:static |FORCEINLINE )?(?:BOOLEAN|VOID|INT|LONG|NTSTATUS|ULONG|PITEM_APP_INFO)(?: NTAPI| CALLBACK)? ' + re.escape(name) + r' \(', source, re.M).start()
    start = source.rfind('\n', 0, source.rfind('\n', 0, pos)) + 1
    # Start at the return type, including a preceding static qualifier when present.
    start = source.rfind('\n', 0, pos) + 1
    brace = source.index('{', pos)
    level = 1
    end = brace + 1
    while level:
        level += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end]


def run_c(support, functions, main):
    with tempfile.TemporaryDirectory() as tmp:
        src = pathlib.Path(tmp) / 'test.c'
        src.write_text(support + '\n' + '\n'.join(function(*f) for f in functions) + '\n' + main)
        compiled = subprocess.run([os.environ.get('CC', 'cc'), '-std=c11', '-Wall', '-Werror', '-Wno-unused-function', *shlex.split(os.environ.get('CFLAGS', '')), str(src), '-o', tmp + '/test'], capture_output=True, text=True)
        if compiled.returncode:
            raise AssertionError(compiled.stderr)
        result = subprocess.run([tmp + '/test'], capture_output=True, text=True)
        if result.returncode:
            raise AssertionError(result.stdout + result.stderr)


COMMON = r'''
#include <assert.h>
#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#define _In_
#define _In_opt_
#define _Inout_
#define _Out_
#define _Out_opt_
#define NTAPI
#define TRUE 1
#define FALSE 0
#define NULL_PTR NULL
#define VOID void
typedef int BOOLEAN;
typedef int *PBOOLEAN;
typedef uint32_t ULONG;
typedef uintptr_t ULONG_PTR;
typedef int32_t LONG;
typedef int64_t LONG64;
typedef void *PVOID;
typedef void **PVOID_PTR;
'''


class PerformanceTests(unittest.TestCase):
    def test_rule_membership_reuses_index_and_invalidates(self):
        support = COMMON + r'''
#define DATA_RULE_USER 1
#define PR_QUEUED_LOCK_INIT 0
typedef int R_QUEUED_LOCK;
typedef struct { ULONG_PTR key; ULONG value; } ENTRY;
typedef struct { ENTRY entries[256]; size_t count; } TABLE, *PR_HASHTABLE;
typedef struct { int type; int is_enabled; PR_HASHTABLE apps; } ITEM_RULE, *PITEM_RULE;
static PITEM_RULE rules_list[4];
static R_QUEUED_LOCK lock_rules;
static size_t rule_visits;
static volatile LONG rule_cache_generation = 1;
static size_t _r_obj_getlistsize(PITEM_RULE *list) { (void)list; return 4; }
static PITEM_RULE _r_obj_getlistitem(PITEM_RULE *list, size_t i) { ++rule_visits; return list[i]; }
static PR_HASHTABLE _r_obj_createhashtable(size_t size, size_t count, void *cb) { (void)size; (void)count; (void)cb; return calloc(1,sizeof(TABLE)); }
static void _r_obj_clearhashtable(PR_HASHTABLE t) { t->count=0; }
static void *_r_obj_findhashtable(PR_HASHTABLE t, ULONG_PTR key) { for(size_t i=0;i<t->count;i++) if(t->entries[i].key==key) return &t->entries[i].value; return NULL; }
static void *_r_obj_addhashtableitem(PR_HASHTABLE t, ULONG_PTR key, void *value) { ULONG *p=_r_obj_findhashtable(t,key); if(!p) { t->entries[t->count].key=key; p=&t->entries[t->count++].value; } *p=value?*(ULONG*)value:0; return p; }
static int _r_obj_enumhashtable(PR_HASHTABLE t, PVOID_PTR entry, ULONG *key, ULONG_PTR *i) { if(*i>=t->count) return 0; if(entry) *entry=&t->entries[*i].value; if(key) *key=t->entries[*i].key; ++*i; return 1; }
#define _r_queuedlock_acquireshared(p) ((void)(p))
#define _r_queuedlock_releaseshared(p) ((void)(p))
#define _r_queuedlock_acquireexclusive(p) ((void)(p))
#define _r_queuedlock_releaseexclusive(p) ((void)(p))
#define InterlockedIncrement(p) (++*(p))
#define InterlockedCompareExchange(p,v,c) (*(p))
'''
        source = (ROOT / 'src/profile.c').read_text()
        # Keep the baseline runnable before the invalidation API is implemented.
        functions = []
        if 'VOID _app_rulecache_invalidate (' in source:
            functions += [('src/profile.c', '_app_rulecache_invalidate')]
        else:
            support += '\nstatic void _app_rulecache_invalidate(void) {}\n'
        functions += [('src/profile.c', '_app_isapphaverule')]
        run_c(support, functions, r'''
int main(void) {
 TABLE a={0},b={0}; ITEM_RULE enabled={1,1,&a}, disabled={1,0,&b};
 _r_obj_addhashtableitem(&a,42,NULL); _r_obj_addhashtableitem(&b,43,NULL);
 rules_list[0]=&enabled; rules_list[1]=NULL; rules_list[2]=&disabled;
 _app_rulecache_invalidate();
 for(int i=0;i<1000;i++) {
  assert(_app_isapphaverule(42,FALSE)); assert(!_app_isapphaverule(43,FALSE));
  assert(_app_isapphaverule(43,TRUE)); assert(!_app_isapphaverule(99,TRUE));
 }
 if(rule_visits>8) { fprintf(stderr,"Repeated membership queries visited %zu rules; expected one index build\n",rule_visits); return 1; }
 enabled.is_enabled=0; _app_rulecache_invalidate();
 assert(!_app_isapphaverule(42,FALSE)); assert(_app_isapphaverule(42,TRUE));
 a.count=0; _app_rulecache_invalidate(); assert(!_app_isapphaverule(42,TRUE));
 rules_list[2]=NULL; _app_rulecache_invalidate(); assert(!_app_isapphaverule(43,TRUE));
 return 0;
}
''')

    def test_search_unchanged_hidden_row_does_not_update_control(self):
        support = COMMON + r'''
typedef void *HWND;
typedef int INT;
typedef struct { const wchar_t *buffer; } R_STRINGREF, *PR_STRINGREF;
typedef struct { R_STRINGREF sr; } STRING, *PR_STRING;
typedef struct { int is_hidden; ULONG_PTR id_code; } ITEM_LISTVIEW_CONTEXT, *PITEM_LISTVIEW_CONTEXT;
typedef struct { PR_STRING real_path,comment; } ITEM_APP, *PITEM_APP;
typedef struct { PR_STRING path,local_addr_str,local_host_str,remote_addr_str,remote_host_str,protocol_str; } NETWORK,*PITEM_NETWORK;
typedef struct { PR_STRING name,rule_remote,rule_local,protocol_str,comment; } RULE,*PITEM_RULE;
typedef struct { PR_STRING path,filter_name,layer_name,username,local_addr_str,local_host_str,remote_addr_str,remote_host_str,protocol_str; } LOG,*PITEM_LOG;
static ITEM_APP app;
static int updates;
#define _app_getappitem(id) (&app)
#define _app_getrulebyid(id) ((PITEM_RULE)NULL)
#define _app_network_getitem(id) ((PITEM_NETWORK)NULL)
#define _app_getlogitem(id) ((PITEM_LOG)NULL)
#define _InterlockedCompareExchangePointer(p,a,b) (*(p))
#define _r_obj_dereference(p) ((void)(p))
#define _r_listview_setitem(...) (++updates)
#define I_IMAGECALLBACK 0
#define I_GROUPIDCALLBACK 0
#define I_DEFAULT 0
#define IDC_APPS_PROFILE 1
#define IDC_APPS_SERVICE 2
#define IDC_APPS_UWP 3
#define IDC_RULE_APPS_ID 4
#define IDC_RULES_BLOCKLIST 5
#define IDC_RULES_SYSTEM 6
#define IDC_RULES_CUSTOM 7
#define IDC_APP_RULES_ID 8
#define IDC_NETWORK 9
#define IDC_LOG 10
'''
        # Exercise the real helper with stable no-match state; integration checks
        # below cover the final visibility comparison in the caller.
        support += r'''
#define SIZE_MAX ((size_t)-1)
static int _r_obj_isstringempty(const void *p) { const R_STRINGREF *s=p; return !s || !s->buffer || !*s->buffer; }
static size_t _r_str_findstring(PR_STRINGREF s, PR_STRINGREF q, int ignore) { (void)ignore; return wcsstr(s->buffer,q->buffer)?0:SIZE_MAX; }
'''
        support = '#include <wchar.h>\n' + support.replace('#define SIZE_MAX ((size_t)-1)\n','')
        run_c(support, [('src/search.c','_app_search_isstringfound'), ('src/search.c','_app_search_applyfilteritem')], r'''
int main(void) { R_STRINGREF s={L"app.exe"},q={L"other"}; ITEM_LISTVIEW_CONTEXT c={TRUE,0}; BOOLEAN changed=FALSE;
 assert(!_app_search_isstringfound(&s,&q,&c,&changed)); assert(c.is_hidden); assert(!changed);
 STRING path={s},query={q}; app.real_path=&path;
 changed=_app_search_applyfilteritem(NULL,IDC_APPS_PROFILE,0,&c,&query);
 if(changed || updates) { fprintf(stderr,"An unchanged hidden row triggered an update\n"); return 1; }
 query.sr.buffer=L"app"; app.comment=NULL;
 assert(_app_search_applyfilteritem(NULL,IDC_APPS_PROFILE,0,&c,&query)); assert(!c.is_hidden);
 assert(_app_search_applyfilteritem(NULL,IDC_APPS_PROFILE,0,&c,NULL)==FALSE);
 q.buffer=L"other"; c.is_hidden=TRUE; changed=FALSE;
 q.buffer=L"app"; assert(_app_search_isstringfound(&s,&q,&c,&changed)); assert(!c.is_hidden); assert(changed); return 0; }
''')

    def test_metadata_worker_releases_reference_after_open_failure(self):
        support = COMMON + r'''
typedef void *HANDLE;
typedef void *HWND;
typedef int NTSTATUS;
typedef struct { wchar_t *buffer; int sr; } STRING, *PR_STRING;
typedef struct { BOOLEAN is_loaded; volatile LONG load_state; volatile LONG64 retry_after; ULONG app_hash; PR_STRING path; int listview_id; } ITEM_APP_INFO,*PITEM_APP_INFO;
#define NT_SUCCESS(s) ((s)>=0)
#define STATUS_OBJECT_NAME_NOT_FOUND -1
#define STATUS_OBJECT_PATH_NOT_FOUND -2
#define STATUS_ACCESS_DENIED -3
#define GENERIC_READ 0
#define FILE_SHARE_READ 0
#define FILE_SHARE_DELETE 0
#define FILE_SHARE_WRITE 0
#define LOG_LEVEL_ERROR 0
static int releases;
static HWND _r_app_gethwnd(void) {return NULL;}
static NTSTATUS _r_fs_openfile(HANDLE *h,void *s,int a,int b,int c,int d) {(void)h;(void)s;(void)a;(void)b;(void)c;(void)d;return -1;}
#define _r_log(...) ((void)0)
#define _app_getfileicon(...) ((void)0)
#define _app_getfilesignatureinfo(...) ((void)0)
#define _app_getfileversioninfo(...) ((void)0)
#define _app_getfilehashinfo(...) ((void)0)
#define _r_config_getboolean(...) 0
#define _r_wnd_isvisible(...) 0
#define _r_listview_redraw(...) ((void)0)
#define _app_listview_requestredraw(...) ((void)0)
#define _r_obj_dereference(p) (++releases)
#define NtClose(h) ((void)(h))
#define InterlockedExchange(p,v) (*(p)=(v))
#define _InterlockedExchange64(p,v) (*(p)=(v))
#define _r_sys_gettickcount64() 1000
'''
        run_c(support, [('src/helper.c','_app_queue_fileinformation')], r'''
int main(void) { STRING path={L"missing.exe",0}; ITEM_APP_INFO info={0}; info.path=&path; info.load_state=1;
 _app_queue_fileinformation(&info); if(releases!=1) { fprintf(stderr,"Failed metadata load released %d references, expected 1\n",releases); return 1; } return 0; }
''')

    def test_save_request_targets_main_window(self):
        support = COMMON + r'''
typedef uintptr_t HWND;
static struct {volatile LONG save_pending;} config;
static HWND posted;
#define RM_PROFILE_SAVE 1
#define _r_app_gethwnd() 42
#define _app_profile_save(hwnd) ((void)(hwnd))
#define InterlockedExchange(p,v) (*(p)=(v))
#define PostMessageW(hwnd,msg,w,l) (posted=(hwnd))
'''
        run_c(support, [('src/profile.c','_app_profile_requestsave')], r'''
int main(void) { _app_profile_requestsave(99); assert(config.save_pending==1);
 if(posted!=42) {fprintf(stderr,"Save request went to notification window instead of main window\n");return 1;} return 0; }
''')

    def test_sort_preserves_checkbox_priority_and_numeric_order(self):
        support = COMMON + r'''
#include <wchar.h>
#define CALLBACK
typedef intptr_t LPARAM;
typedef int INT;
typedef struct {wchar_t *buffer;} STRING,*PR_STRING;
typedef struct {ULONG sort_index;} ITEM_LISTVIEW_CONTEXT,*PITEM_LISTVIEW_CONTEXT;
typedef struct {PR_STRING text;LONG64 value;BOOLEAN has_value,is_checked;} APP_SORT_KEY,*PAPP_SORT_KEY;
typedef struct {PAPP_SORT_KEY keys;BOOLEAN is_descend;} APP_SORT_CONTEXT,*PAPP_SORT_CONTEXT;
#define _r_str_compare_logical(a,b) wcscmp(a,b)
'''
        run_c(support, [('src/listview.c','_app_listview_compare_callback')], r'''
int main(void) {STRING a={L"a"},b={L"b"}; APP_SORT_KEY keys[2]={{&a,10,TRUE,TRUE},{&b,2,TRUE,FALSE}};
 APP_SORT_CONTEXT sort={keys,FALSE};ITEM_LISTVIEW_CONTEXT one={0},two={1};
 assert(_app_listview_compare_callback((LPARAM)&one,(LPARAM)&two,(LPARAM)&sort)<0);
 sort.is_descend=TRUE;assert(_app_listview_compare_callback((LPARAM)&one,(LPARAM)&two,(LPARAM)&sort)<0);
 keys[0].is_checked=FALSE;sort.is_descend=FALSE;
 assert(_app_listview_compare_callback((LPARAM)&one,(LPARAM)&two,(LPARAM)&sort)>0);
 sort.is_descend=TRUE;assert(_app_listview_compare_callback((LPARAM)&one,(LPARAM)&two,(LPARAM)&sort)<0);
 keys[0].value=keys[1].value;sort.is_descend=FALSE;
 assert(_app_listview_compare_callback((LPARAM)&one,(LPARAM)&two,(LPARAM)&sort)<0);
 keys[0].has_value=FALSE;assert(_app_listview_compare_callback((LPARAM)&one,(LPARAM)&two,(LPARAM)&sort)<0);
 assert(_app_listview_compare_callback((LPARAM)&one,(LPARAM)&one,(LPARAM)&sort)==0);return 0;}
''')

    def test_ui_existence_never_checks_disk_and_deduplicates_pending_work(self):
        support = COMMON + r'''
typedef uint64_t ULONG64;
#define DATA_APP_REGULAR 1
typedef struct {BOOLEAN is_undeletable,is_enabled,is_haveerrors;int type;volatile LONG exists_state,exists_pending;volatile LONG64 exists_checked;} ITEM_APP,*PITEM_APP;
static unsigned queued,disk_checks;static ULONG64 now=1000;
static int file_queue;
static BOOLEAN _app_isappexists(PITEM_APP a){(void)a;++disk_checks;return TRUE;}
static void _app_queue_exists(void *p){(void)p;}
#define _r_obj_reference(p) (p)
#define _r_workqueue_queueitem(q,f,p) (++queued)
#define _r_sys_gettickcount64() now
#define _InterlockedCompareExchange64(p,v,c) (*(p))
static LONG cas(volatile LONG *p,LONG v,LONG c){LONG old=*p;if(old==c)*p=v;return old;}
#define InterlockedCompareExchange cas
'''
        # Touch stubs so -Wall can still diagnose errors inside production code.
        support += '\n'
        run_c(support, [('src/profile.c','_app_isappexists_ui')], r'''
int main(void){ITEM_APP app={0};(void)file_queue;(void)_app_queue_exists;app.type=DATA_APP_REGULAR;
 for(int i=0;i<10000;i++){assert(_app_isappexists_ui(&app));}assert(queued==1);assert(disk_checks==0);
 app.exists_checked=now;app.exists_pending=0;app.exists_state=2;assert(!_app_isappexists_ui(&app));assert(queued==1);
 now+=30001;assert(!_app_isappexists_ui(&app));assert(queued==2);
 app.is_undeletable=TRUE;assert(_app_isappexists_ui(&app));assert(disk_checks==0);return 0;}
''')

    def test_profile_wait_includes_a_dequeued_unfinished_write(self):
        support = COMMON + r'''
#define INFINITE 0xffffffff
#define WAIT_FAILED -1
#define Sleep(ms) ((void)(ms))
static volatile LONG profile_writes_pending=1;
static void *profile_write_event;
static int waits;
#define InterlockedCompareExchange(p,v,c) (*(p))
#define WaitForSingleObject(e,timeout) ((void)(e),(void)(timeout),++waits,profile_writes_pending=0)
'''
        run_c(support, [('src/profile.c','_app_profile_waitwrites')], r'''
int main(void){_app_profile_waitwrites();assert(waits==1);_app_profile_waitwrites();assert(waits==1);return 0;}
''')


    def test_metadata_requests_queue_once_and_retry_failures_after_backoff(self):
        support = COMMON + r'''
typedef uint64_t ULONG64;
typedef int ENUM_TYPE_DATA;
typedef int INT;
typedef struct {wchar_t *buffer;} STRING,*PR_STRING;
typedef struct {PR_STRING path;ULONG app_hash;ENUM_TYPE_DATA type;INT listview_id;BOOLEAN is_loaded;volatile LONG load_state;volatile LONG64 retry_after;} ITEM_APP_INFO,*PITEM_APP_INFO;
static ITEM_APP_INFO *cached;
static int references,queued;
static ULONG64 now=1000;
static int lock_cache_information,cache_information,file_queue;
static void _app_queue_fileinformation(void *p){(void)p;}
static void _app_dereferenceappinfo(void *p){(void)p;}
static PITEM_APP_INFO lookup(void){if(cached)references++;return cached;}
static void *reference(void *p){if(p==cached)references++;return p;}
#define _r_obj_isstringempty(p) (!(p))
#define _app_isappvalidbinary(p) TRUE
#define _r_queuedlock_acquireexclusive(p) ((void)(p))
#define _r_queuedlock_releaseexclusive(p) ((void)(p))
#define _r_obj_findhashtablepointer(t,h) ((void)(t),(void)(h),lookup())
#define _r_obj_allocate(size,cb) (references=1,cached=calloc(1,size))
#define _r_obj_reference reference
#define _r_obj_addhashtablepointer(t,h,p) ((void)(t),(void)(h),(void)(p))
#define _r_workqueue_queueitem(q,f,p) ((void)(q),(void)(f),(void)(p),++queued)
#define _r_obj_dereference(p) ((void)(p),--references)
#define _r_sys_gettickcount64() now
#define _InterlockedCompareExchange64(p,v,c) (*(p))
static LONG cas(volatile LONG *p,LONG v,LONG c){LONG old=*p;if(old==c)*p=v;return old;}
#define InterlockedCompareExchange cas
'''
        run_c(support, [('src/helper.c','_app_getfileinformation')], r'''
int main(void){STRING path={L"app.exe"};(void)_app_dereferenceappinfo;
 for(int i=0;i<1000;i++){_app_getfileinformation(&path,42,1,106);}
 assert(queued==1);assert(references==2); /* table + worker */
 cached->load_state=2;cached->is_loaded=TRUE;--references; /* worker completed */
 for(int i=0;i<1000;i++){_app_getfileinformation(&path,42,1,106);}
 assert(queued==1);assert(references==1);
 cached->is_loaded=FALSE;cached->retry_after=31000;
 _app_getfileinformation(&path,42,1,106);assert(queued==1);assert(references==1);
 now=31001;_app_getfileinformation(&path,42,1,106);assert(queued==2);assert(references==2);
 _app_getfileinformation(&path,42,1,106);assert(queued==2);assert(references==2);free(cached);return 0;}
''')

    def test_failed_snapshot_write_does_not_replace_profile_and_signals_completion(self):
        support = COMMON + r'''
typedef void *HANDLE;
typedef uintptr_t HWND;
typedef int NTSTATUS;
typedef struct {wchar_t *buffer;int sr;} STRING,*PR_STRING;
typedef struct {void *buffer;size_t length;} BYTE,*PR_BYTE;
typedef struct {PR_BYTE bytes;PR_STRING path;HWND hwnd;LONG64 timestamp;} APP_PROFILE_SNAPSHOT,*PAPP_PROFILE_SNAPSHOT;
#define NT_SUCCESS(s) ((s)>=0)
#define FILE_OVERWRITE_IF 0
#define GENERIC_WRITE 0
#define FILE_SHARE_READ 0
#define FILE_ATTRIBUTE_NORMAL 0
#define FILE_ATTRIBUTE_READONLY 1
#define _r_fs_getattributes(path,out) (*(out)=0,0)
#define _r_fs_setattributes(...) ((void)0)
#define LOG_LEVEL_ERROR 0
#define RM_PROFILE_SAVE_ERROR 1
static int lock_profile,renames,releases,freed,events,write_fails,posted;
static volatile LONG profile_writes_pending;
static void *profile_write_event;
static STRING temporary_name={L"profile.xml.pending",0};
#define _r_obj_concatstrings(...) (&temporary_name)
#define _r_queuedlock_acquireexclusive(p) ((void)(p))
#define _r_queuedlock_releaseexclusive(p) ((void)(p))
static int create(HANDLE *file){*file=(HANDLE)1;return 0;}
#define _r_fs_createfile(out,...) create(out)
#define _r_fs_writefile(...) (write_fails?-5:0)
#define NtClose(h) ((void)(h))
#define _r_fs_movefile(...) (++renames,0)
#define _r_config_getboolean(...) FALSE
#define _r_config_getlong64(...) 0
#define _r_config_setlong64(...) ((void)0)
#define _r_fs_isexists(...) TRUE
#define _r_fs_copyfile(...) 0
#define _r_log(...) ((void)0)
#define PostMessageW(...) (++posted)
#define _r_obj_dereference(p) ((void)(p),++releases)
#define _r_mem_free(p) ((void)(p),++freed)
#define InterlockedDecrement(p) (--*(p))
#define SetEvent(e) ((void)(e),assert(profile_writes_pending==0),++events)
'''
        run_c(support, [('src/profile.c','_app_profile_write_snapshot')], r'''
int main(void){STRING path={L"profile.xml",0};BYTE bytes={NULL,100};APP_PROFILE_SNAPSHOT s={&bytes,&path,42,1000};
 profile_writes_pending=1;write_fails=TRUE;_app_profile_write_snapshot(&s);
 assert(renames==0);assert(posted==1);assert(releases==3);assert(freed==1);assert(events==1);
 profile_writes_pending=1;write_fails=FALSE;_app_profile_write_snapshot(&s);
 assert(renames==1);assert(posted==1);assert(events==2);return 0;}
''')


    def test_version_query_adapter_matches_output_first_application_call(self):
        adapter = (ROOT / 'tools/public-sdk/routine-compat.h').read_text()
        mapping = '\n'.join(line for line in adapter.splitlines()
                            if line.startswith('#define _r_res_queryversion('))
        helper = function('src/helper.c', '_app_getfileversioninfo')
        call = re.search(r'_r_res_queryversion \(.*\)\)', helper).group(0)[:-1]
        support = COMMON + r'''
static char resource[] = "version resource";
static int version_info = 1234;
static BOOLEAN _r_res_queryversion(const void *block, PVOID_PTR output)
{
    if (block != resource || output == (PVOID_PTR)resource)
        return FALSE;
    *output = &version_info;
    return TRUE;
}
''' + mapping
        run_c(support, [], r'''
int main(void) {
 void *ver_info=NULL;
 struct {void *buffer;} ver_block={resource};
 if (!''' + call + r''' || ver_info != &version_info) {
  fprintf(stderr,"Version query adapter passed output pointer as resource data\n"); return 1;
 }
 return 0;
}
''')


    def test_sid_adapter_matches_output_first_application_call(self):
        adapter = (ROOT / 'tools/public-sdk/routine-compat.h').read_text()
        mapping = '\n'.join(line for line in adapter.splitlines()
                            if line.startswith('#define _r_str_fromsid('))
        support = COMMON + r'''
typedef void *PSID;
static char sid[] = "SID";
static char string[] = "S-1-5-18";
static int _r_str_fromsid(void *input, void **output)
{
 if (input != sid || output == (void**)sid) return -1;
 *output=string; return 0;
}
''' + mapping
        run_c(support, [], r'''
int main(void) {
 void *text=NULL;
 if (_r_str_fromsid(&text,(void*)sid) != 0 || text != string) {
  fprintf(stderr,"SID adapter passed output pointer as SID data\n"); return 1;
 }
 return 0;
}
''')


    @unittest.skipUnless((ROOT.parent / 'routine/src/routine.c').is_file(), 'requires public routine SDK checkout')
    def test_toolbar_localization_preserves_existing_icon(self):
        adapter = (ROOT / 'tools/public-sdk/routine-compat.h').read_text()
        mapping = '\n'.join(line for line in adapter.splitlines()
                            if line.startswith('#define _r_toolbar_setbutton('))
        support = COMMON + r'''
#include <wchar.h>
typedef void *HWND;
typedef int INT;
typedef uintptr_t UINT_PTR;
typedef wchar_t *LPWSTR;
typedef unsigned char BYTE;
typedef intptr_t LPARAM;
typedef struct {unsigned cbSize,dwMask;LPWSTR pszText;BYTE fsStyle,fsState;int iImage;} TBBUTTONINFOW;
#define I_DEFAULT -8
#define I_IMAGENONE -2
#define TBIF_TEXT 1
#define TBIF_STYLE 2
#define TBIF_STATE 4
#define TBIF_IMAGE 8
#define TB_SETBUTTONINFO 0
static int image=8;
static int _r_wnd_sendmessage(HWND hwnd,int ctrl,int msg,UINT_PTR id,LPARAM data) {
 (void)hwnd;(void)ctrl;(void)msg;(void)id;
 TBBUTTONINFOW *info=(TBBUTTONINFOW*)data;
 if(info->dwMask&TBIF_IMAGE) image=info->iImage;
 return 1;
}
'''
        run_c(support, [('../routine/src/routine.c','_r_toolbar_setbutton')], mapping + r'''
int main(void) {
 _r_toolbar_setbutton(NULL,0,1,L"Refresh",1,0,I_DEFAULT);
 if(image!=8){fprintf(stderr,"Updating toolbar text replaced its icon with index %d\n",image);return 1;}
 _r_toolbar_setbutton(NULL,0,1,NULL,0,0,0);assert(image==0);
 _r_toolbar_setbutton(NULL,0,1,NULL,0,0,1);assert(image==1);return 0;
}
''')


    def test_toolbar_toggle_tracks_checked_state_without_latching_pressed(self):
        support = COMMON + r'''
#define FORCEINLINE static inline
typedef void *HWND;
typedef int INT;
typedef unsigned UINT;
#define TB_CHECKBUTTON 1
#define TB_PRESSBUTTON 2
#define MAKELPARAM(a,b) (a)
static unsigned state=4; /* enabled */
static int _r_wnd_sendmessage(HWND hwnd,int ctrl,int msg,unsigned id,int value) {
 (void)hwnd;(void)ctrl;(void)id;
 unsigned mask=msg==TB_CHECKBUTTON?1:2;
 if(value)state|=mask;else state&=~mask;
 return TRUE;
}
'''
        path = 'src/controls.c'
        name = '_app_toolbar_setchecked'
        if name + ' (' not in (ROOT / path).read_text():
            path, name = '../routine/src/routine.h', '_r_toolbar_pressbutton'
        run_c(support, [(path,name)], r'''
int main(void) {
 ''' + name + r'''(NULL,0,42,TRUE);
 if((state&3)!=1){fprintf(stderr,"Toolbar latched pressed state instead of checked state\n");return 1;}
 for(int i=0;i<10;i++) {
  state^=1; /* automatic BTNS_CHECK click */
  ''' + name + r'''(NULL,0,42,FALSE);assert(state==4);
  state^=1;
  ''' + name + r'''(NULL,0,42,TRUE);assert(state==5);
 }
 return 0;
}
''')

    def test_refresh_queues_once_without_reloading_profile_or_firewall(self):
        support = COMMON + r'''
typedef void *HWND;
static struct {volatile LONG refresh_pending;} config;
static int refresh_queue,queued,reloads,rebuilds;
static void _app_queue_refresh(void *p){(void)p;}
#define _r_workqueue_queueitem(q,f,p) ((void)(q),(void)(f),(void)(p),++queued)
#define _wfp_isfiltersapplying() FALSE
#define _app_profile_load(...) (++reloads)
#define _app_changefilters(...) (++rebuilds)
static LONG cas(volatile LONG *p,LONG value,LONG expected){LONG old=*p;if(old==expected)*p=value;return old;}
#define InterlockedCompareExchange cas
'''
        name = '_app_command_refresh'
        if name + ' (' in (ROOT / 'src/messages.c').read_text():
            funcs=[('src/messages.c',name)]
        else:
            source=(ROOT / 'src/main.c').read_text()
            body=source.split('case IDM_REFRESH:',1)[1].split('case IDM_LOADONSTARTUP_CHK:',1)[0]
            body=body[body.index('{')+1:body.rfind('}')].replace('break;', 'return;')
            support += '\nstatic void _app_command_refresh(HWND hwnd){(void)hwnd;'+body+'}\n'
            funcs=[]
        run_c(support, funcs, r'''
int main(void) {
 (void)refresh_queue;(void)_app_queue_refresh;(void)cas;(void)config;
 for(int i=0;i<100;i++){_app_command_refresh(NULL);}
 if(reloads||rebuilds){fprintf(stderr,"Refresh reloaded the profile/rebuilt firewall filters\n");return 1;}
 assert(queued==1);return 0;
}
''')


    def test_refresh_checks_paths_outside_apps_lock_and_retains_model_objects(self):
        support = COMMON + r'''
typedef void *HWND;
typedef int INT;
typedef struct {int refs;volatile LONG exists_pending;void *real_path;ULONG app_hash;INT type;} ITEM_APP,*PITEM_APP;
typedef struct {PITEM_APP items[3];int count;} LIST,*PR_LIST;
static ITEM_APP entries[3];
static LIST snapshot;
static int lock_apps,apps_table,checked,metadata,completed;
#define RM_REFRESH_COMPLETE 1
static void *_r_obj_reference(void *p){++((PITEM_APP)p)->refs;return p;}
static void _r_obj_dereference(void *p){
 if(p==&snapshot){for(int i=0;i<snapshot.count;i++){--snapshot.items[i]->refs;}}
 else {--((PITEM_APP)p)->refs;}
}
static PR_LIST _r_obj_createlist(int initial,void(*cleanup)(void*)){
 (void)initial;(void)cleanup;snapshot.count=0;return &snapshot;
}
#define _r_queuedlock_acquireshared(p) (++*(p))
#define _r_queuedlock_releaseshared(p) (--*(p))
static BOOLEAN _r_obj_enumhashtablepointer(int table,PVOID_PTR p,void *hash,ULONG_PTR *cursor){
 (void)table;(void)hash;assert(lock_apps==1);
 if(*cursor>=3){return FALSE;}*p=&entries[(*cursor)++];return TRUE;
}
static void _r_obj_addlistitem(PR_LIST list,void *p,void *index){(void)index;list->items[list->count++]=p;}
static ULONG_PTR _r_obj_getlistsize(PR_LIST list){return list->count;}
static void *_r_obj_getlistitem(PR_LIST list,ULONG_PTR i){return list->items[i];}
static LONG cas(volatile LONG *p,LONG value,LONG expected){LONG old=*p;if(old==expected)*p=value;return old;}
#define InterlockedCompareExchange cas
static void _app_queue_exists(void *p){
 PITEM_APP app=p;assert(lock_apps==0);assert(app->exists_pending==1);++checked;app->exists_pending=0;_r_obj_dereference(app);
}
static int _app_listview_getbytype(int type){return type;}
static void _app_getfileinformation(void *path,ULONG hash,int type,int list){(void)path;(void)hash;(void)type;(void)list;assert(lock_apps==0);++metadata;}
static void PostMessageW(HWND hwnd,int msg,int w,int l){(void)hwnd;(void)msg;(void)w;(void)l;++completed;assert(lock_apps==0);}
'''
        run_c(support,[('src/profile.c','_app_queue_refresh')],r'''
int main(void){for(int i=0;i<3;i++){entries[i].refs=1;}entries[1].exists_pending=1;
 _app_queue_refresh(NULL);
 assert(checked==2);assert(metadata==3);assert(completed==1);assert(lock_apps==0);
 for(int i=0;i<3;i++){assert(entries[i].refs==1);}return 0;
}
''')


    @unittest.skipUnless((ROOT.parent / 'routine/src/routine.c').is_file(), 'requires public routine SDK checkout')
    def test_packet_log_append_uses_valid_index_with_public_sdk(self):
        adapter=(ROOT/'tools/public-sdk/routine-compat.h').read_text()
        mapping='\n'.join(line for line in adapter.splitlines() if line.startswith('#define _r_listview_additem('))
        support=COMMON+r'''
typedef void *HWND;
typedef int INT;
typedef wchar_t *LPWSTR;
typedef intptr_t LPARAM;
typedef struct {unsigned mask;int iItem,iSubItem;LPWSTR pszText;int iImage,iGroupId;LPARAM lParam;} LVITEMW;
typedef struct {LONG log_id;} ITEM_LOG,*PITEM_LOG;
static struct {LONG log_id;} config;
static int rows;
#define IDC_LOG 113
#define INT_ERROR -1
#define I_DEFAULT -8
#define LPSTR_TEXTCALLBACK ((LPWSTR)-1)
#define I_IMAGECALLBACK -1
#define LVIF_TEXT 1
#define LVIF_IMAGE 2
#define LVIF_GROUPID 4
#define LVIF_PARAM 8
#define LVM_INSERTITEM 1
#define _InterlockedIncrement(p) (++*(p))
#define _InterlockedCompareExchange(p,v,c) (*(p))
#define _app_listview_createcontext(hash) ((LPARAM)(hash))
static int _r_listview_getitemcount(HWND hwnd,int id){(void)hwnd;(void)id;return rows;}
static int _r_wnd_sendmessage(HWND hwnd,int id,int msg,int w,LPARAM data){
 (void)hwnd;(void)id;(void)msg;(void)w;LVITEMW *item=(LVITEMW*)data;
 if(item->iItem<0)return -1;
 ++rows;return item->iItem;
}
'''
        support += '\n'+function('../routine/src/routine.c','_r_listview_additem')+'\n'+mapping
        run_c(support,[('src/listview.c','_app_listview_addlogitem')],r'''
int main(void){ITEM_LOG log={0};(void)_r_listview_getitemcount;
 _app_listview_addlogitem(NULL,&log,42);
 _app_listview_addlogitem(NULL,&log,43);
 if(rows!=2){fprintf(stderr,"Packet records exist but negative insert index produced %d rows\n",rows);return 1;}
 assert(config.log_id==2);return 0;}
''')


if __name__ == '__main__':
    unittest.main(verbosity=2)
