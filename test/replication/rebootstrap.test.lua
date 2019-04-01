test_run = require('test_run').new()

SERVERS = {'rebootstrap1', 'rebootstrap2'}

test_run:create_cluster(SERVERS, "replication", {args="0.1"})
test_run:wait_fullmesh(SERVERS)

--
-- gh-3422: If quorum can't be formed, because some replicas are
-- re-bootstrapping, box.cfg{} must wait for bootstrap to complete
-- instead of stopping synchronization and leaving the instance
-- in 'orphan' mode.
--
test_run:cmd('stop server rebootstrap1')
test_run:cmd('restart server rebootstrap2 with cleanup=True, wait=False, wait_load=False, args="0.1 2.0"')
test_run:cmd('start server rebootstrap1 with args="0.1"')
test_run:cmd('switch rebootstrap1')
test_run:wait_cond(function() return box.info.status == 'running' end) or box.info.status

test_run:cmd('switch default')
test_run:drop_cluster(SERVERS)
