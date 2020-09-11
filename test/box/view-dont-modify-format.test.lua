--
-- Make sure we can't modify a view format.
--
box.execute("CREATE TABLE t1 (a INT PRIMARY KEY);")
box.execute("CREATE VIEW v AS SELECT * FROM t1;")
view = box.space._space.index[2]:select('V')[1]:totable()
view_format = view[7]
f = {type = 'string', nullable_action = 'none', name = 'C', is_nullable = true}
table.insert(view_format, f)
view[5] = 3
view[7] = view_format
box.space._space:replace(view)

view = box.space.V
view:format({})

box.execute("DROP VIEW v;")
box.execute("DROP TABLE t1;")
