#!/usr/bin/env tarantool

require('suite')

box.cfg {
    listen            = os.getenv("LISTEN"),
    slab_alloc_arena  = 0.1,
    pid_file          = "tarantool.pid",
    rows_per_wal      = 500000,
}

require('console').listen(os.getenv('ADMIN'))
