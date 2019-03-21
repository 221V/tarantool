test_run = require('test_run').new()
replica_set = require('fast_replica')

--
-- gh-3893: Replication failure: relay may report that an xlog
-- is corrupted if it it currently being written to.
--
s = box.schema.space.create('test')
_ = s:create_index('primary')

-- Deploy a replica.
box.schema.user.grant('guest', 'replication')
replica_set.create(test_run, 'wal_rw_stress')
test_run:cmd("start server wal_rw_stress")

-- Setup replica => master channel.
box.cfg{replication = test_run:cmd("eval wal_rw_stress 'return box.cfg.listen'")}

-- Disable master => wal_rw_stress channel.
test_run:cmd("switch wal_rw_stress")
replication = box.cfg.replication
box.cfg{replication = {}}
test_run:cmd("switch default")

-- Write some xlogs on the master.
test_run:cmd("setopt delimiter ';'")
for i = 1, 100 do
    box.begin()
    for j = 1, 100 do
        s:replace{1, require('digest').urandom(1000)}
    end
    box.commit()
end;
test_run:cmd("setopt delimiter ''");

-- Enable master => wal_rw_stress channel and wait for the replica to catch up.
-- The relay handling wal_rw_stress => master channel on the replica will read
-- an xlog while the applier is writing to it. Although applier and relay
-- are running in different threads, there shouldn't be any rw errors.
test_run:cmd("switch wal_rw_stress")
box.cfg{replication = replication}
box.info.replication[1].downstream.status ~= 'stopped' or box.info
test_run:cmd("switch default")

-- Cleanup.
box.cfg{replication = {}}
replica_set.drop(test_run, 'wal_rw_stress')
test_run:cleanup_cluster()
box.schema.user.revoke('guest', 'replication')
s:drop()
