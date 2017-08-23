env = require('test_run')
test_run = env.new()

-- gh-2025 box.savepoint

box.savepoint()
box.rollback_to_savepoint()

box.begin() box.savepoint()
box.rollback()

box.begin() box.rollback_to_savepoint()
box.rollback()

engine = test_run:get_cfg('engine')

-- Test many savepoints on each statement.
s = box.schema.space.create('test', {engine = engine})
p = s:create_index('pk')
test_run:cmd("setopt delimiter ';'")
box.begin()
s:replace{1}
box.savepoint()
s:replace{2}
box.savepoint()
s:replace{3}
box.savepoint()
s:replace{4}
s1 = s:select{}
box.rollback_to_savepoint()
s2 = s:select{}
box.rollback_to_savepoint()
s3 = s:select{}
box.rollback_to_savepoint()
s4 = s:select{}
box.commit();
test_run:cmd("setopt delimiter ''");
s1
s2
s3
s4
s:truncate()

-- Test rollback to savepoint on the current statement.
test_run:cmd("setopt delimiter ';'")
box.begin()
s:replace{1}
s:replace{2}
box.savepoint()
box.rollback_to_savepoint()
box.commit()
test_run:cmd("setopt delimiter ''");
s:select{}
s:truncate()

-- Test rollback to savepoint after multiple statements.
test_run:cmd("setopt delimiter ';'")
box.begin()
s:replace{1}
box.savepoint()
s:replace{2}
s:replace{3}
s:replace{4}
box.rollback_to_savepoint()
box.commit()
test_run:cmd("setopt delimiter ''");
s:select{}
s:truncate()

-- Test rollback to savepoint after failed statement.
test_run:cmd("setopt delimiter ';'")
box.begin()
s:replace{1}
box.savepoint()
s:replace{3}
pcall(s.replace, s, {'kek'})
s:replace{4}
box.rollback_to_savepoint()
box.commit()
test_run:cmd("setopt delimiter ''");
s:select{}
s:truncate()

-- Test rollback to savepoint inside the trigger.
s1 = nil
s2 = nil
s3 = nil
s4 = nil
test_run:cmd("setopt delimiter ';'")
function on_replace(old, new)
	if new[1] > 10 then return end
	s1 = s:select{}
	box.savepoint()
	s:replace{100}
	box.rollback_to_savepoint()
	s2 = s:select{}
end;
_ = s:on_replace(on_replace);
box.begin()
s:replace{1}
s3 = s1
s4 = s2
s:replace{2}
box.commit()
test_run:cmd("setopt delimiter ''");
s4
s3
s2
s1
s:select{}
s:drop()

-- Test rollback to savepoint, created in trigger,
-- from main tx stream.
s = box.schema.space.create('test', {engine = engine})
p = s:create_index('pk')
test_run:cmd("setopt delimiter ';'")
function on_replace2(old, new)
	if new[1] ~= 1 then return end
	box.savepoint()
	s:replace{100}
end;
_ = s:on_replace(on_replace2);
box.begin()
s:replace{1}
s1 = s:select{}
s:replace{2}
s:replace{3}
s2 = s:select{}
box.rollback_to_savepoint()
s3 = s:select{}
s:replace{4}
s4 = s:select{}
box.commit()
test_run:cmd("setopt delimiter ''");
s1
s2
s3
s4
s:drop()

-- Test incorrect savepoints usage inside a transaction.
s = box.schema.space.create('test', {engine = engine})
p = s:create_index('pk')

test_run:cmd("setopt delimiter ';'")
box.begin()
ok1 = pcall(box.savepoint)
s:replace{1}
ok2 = pcall(box.rollback_to_savepoint)
s:replace{2}
s:replace{3}
ok3 = pcall(box.rollback_to_savepoint)
s:replace{4}
box.savepoint()
box.rollback_to_savepoint()
ok4 = pcall(box.rollback_to_savepoint)
s1 = s:select{}
box.commit()
test_run:cmd("setopt delimiter ''");
ok1
ok2
ok3
ok4
s1
s:truncate()

-- Test unused savepoints.
test_run:cmd("setopt delimiter ';'")
box.begin()
s:replace{1}
box.savepoint()
s:replace{2}
pcall(s.replace, s, {'3'})
s:replace{4}
box.commit()
test_run:cmd("setopt delimiter ''");
s:select{}
s:truncate()

-- Test many savepoints sequentially.
test_run:cmd("setopt delimiter ';'")
box.begin()
s:replace{1}
box.savepoint()
box.savepoint()
box.savepoint()
s:replace{2}
box.rollback_to_savepoint()
box.rollback_to_savepoint()
box.rollback_to_savepoint()
s:replace{3}
box.commit()
test_run:cmd("setopt delimiter ''");
s:select{}

s:drop()
