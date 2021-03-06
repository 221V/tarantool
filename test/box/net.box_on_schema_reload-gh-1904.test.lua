fiber = require 'fiber'
test_run = require('test_run').new()

LISTEN = require('uri').parse(box.cfg.listen)
space = box.schema.space.create('net_box_test_space')
index = space:create_index('primary', { type = 'tree' })

box.schema.user.create('netbox', { password  = 'test' })
box.schema.user.grant('netbox', 'read,write', 'space', 'net_box_test_space')
box.schema.user.grant('netbox', 'execute', 'universe')

net = require('net.box')

-- gh-1904 net.box hangs in :close() if a fiber was cancelled
-- while blocked in :_wait_state() in :_request()
options = {user = 'netbox', password = 'badpass', wait_connected = false, reconnect_after = 0.01}
c = net:new(box.cfg.listen, options)
f = fiber.create(function() c:call("") end)
fiber.sleep(0.01)
f:cancel(); c:close()

box.schema.user.grant('guest', 'read', 'space', '_schema')

-- check for on_schema_reload callback
test_run:cmd("setopt delimiter ';'")
do
    local a = 0
    function osr_cb()
        a = a + 1
    end
    local con = net.new(box.cfg.listen, {
        wait_connected = false
    })
    con:on_schema_reload(osr_cb)
    con:wait_connected()
    con.space._schema:select{}
    box.schema.space.create('misisipi')
    box.space.misisipi:drop()
    con.space._schema:select{}
    con:close()
    con = nil

    return a
end;
do
    local a = 0
    function osr_cb()
        a = a + 1
    end
    local con = net.new(box.cfg.listen, {
        wait_connected = true
    })
    con:on_schema_reload(osr_cb)
    con.space._schema:select{}
    box.schema.space.create('misisipi')
    box.space.misisipi:drop()
    con.space._schema:select{}
    con:close()
    con = nil

    return a
end;
test_run:cmd("setopt delimiter ''");

box.schema.user.revoke('guest', 'read', 'space', '_schema')
