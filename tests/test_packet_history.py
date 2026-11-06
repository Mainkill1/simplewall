#!/usr/bin/env python3
"""Build/run the production portable store against the real bundled SQLite."""
import os, pathlib, shlex, subprocess, tempfile, time
root=pathlib.Path(__file__).resolve().parents[1]
with tempfile.TemporaryDirectory() as temp:
    exe=pathlib.Path(temp)/'history-test'
    subprocess.run(['cc','-std=c11','-O1','-g',*shlex.split(os.getenv('CFLAGS','')),'-DSQLITE_OMIT_LOAD_EXTENSION','-Isrc','-Ivendor/sqlite','tests/packet-store-test.c','src/packet_store.c','vendor/sqlite/sqlite3.c','-lpthread','-lm','-o',str(exe)],cwd=root,check=True)
    started=time.monotonic()
    subprocess.run([str(exe),str(pathlib.Path(temp)/'history.db')],check=True)
    print(f'Elapsed: {time.monotonic()-started:.3f}s')
