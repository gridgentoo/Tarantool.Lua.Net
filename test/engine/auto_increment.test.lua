test_run = require('test_run').new()
engine = test_run:get_cfg('engine')

fiber = require('fiber')

--
-- Basic test.
--
s = box.schema.space.create('test', {engine = engine})
_ = s:create_index('pk')
s:auto_increment{}
s:auto_increment{'a'}
s:auto_increment{'b', 'c'}
s:select()

--
-- Deletion of the last element does not restart the sequence.
--
_ = s:delete{3}
s:auto_increment{'d', 'e', 'f'}
s:select()

--
-- Space truncation does not restart the sequence.
--
s:truncate()
s:auto_increment{1}
s:auto_increment{2}
s:select()

--
-- Insert/replace updates the space sequence if the new
-- value is greater than the sequence value.
--
s:truncate()
s:insert{100}
s:auto_increment{}
s:select()
s:replace{1000}
s:auto_increment{}
s:select()
s:insert{50}
s:replace{150}
s:auto_increment{}
s:select()

--
-- Sequence overflow.
--
s:truncate()
s:insert{tonumber64('18446744073709551615')}
s:auto_increment{}
s:auto_increment{}
s:select{}
s:replace{tonumber64('18446744073709551615')}
s:auto_increment{} -- fails as duplicate
s:drop()

--
-- Deletion of the primary key restarts the sequence
-- while deletion of a secondary key does not.
--
s = box.schema.space.create('test', {engine = engine})
_ = s:create_index('pk')
_ = s:create_index('sk', {parts = {2, 'unsigned'}})
s:auto_increment{123}
s.index.sk:drop()
s:auto_increment{456}
s:select()
s.index.pk:drop()
_ = s:create_index('pk')
s:auto_increment{'aaa'}
s:auto_increment{'bbb'}
s:select()
s:drop()

--
-- Attempt to auto increment a space without indexes fails.
--
s = box.schema.space.create('test', {engine = engine})
s:auto_increment{123}

--
-- Attempt to auto increment a space with an unsuitable
-- primary key fails.
--
_ = s:create_index('pk', {parts = {1, 'string'}})
s:auto_increment{}
s.index.pk:drop()
_ = s:create_index('pk', {parts = {1, 'unsigned', 2, 'unsigned'}})
s:auto_increment{}
s.index.pk:drop()
_ = s:create_index('pk', {parts = {2, 'unsigned'}})
s:auto_increment{}
s:drop()

--
-- There is no C or IPROTO API for auto increment operation.
-- The user is supposed to provide 'nil' for the first
-- tuple field in INSERT/REPLACE to trigger auto increment.
-- This is what space.auto_increment Lua function actually
-- does. Check that this API works as well.
--
s = box.schema.space.create('test', {engine = engine})
_ = s:create_index('pk')
s:insert(box.tuple.new(nil))
s:insert{nil, 'a'}
_ = s:delete(2)
s:insert{nil, 'b'}
s:auto_increment{'c'}
s:select()
s:drop()

--
-- Check that we can still have 'nil' for the first tuple value
-- if auto increment is not used.
--
s = box.schema.space.create('test', {engine = engine})
_ = s:create_index('pk', {parts = {2, 'unsigned'}})
s:insert{nil, 1}
s:insert{1, 10}
s:insert{nil, 2}
s:insert{2, 20}
s:select()
s:drop()

--
-- Auto increment is consistent when used concurrently.
--
s = box.schema.space.create('test', {engine = engine})
_ = s:create_index('pk')
c = fiber.channel(10)
test_run:cmd("setopt delimiter ';'")
for i = 1, 10 do
    fiber.create(function()
        for j = 1, 10 do
            s:auto_increment{}
            if j % 3 == 0 then
                fiber.sleep(0)
            end
        end
        c:put(true)
    end)
end
for i = 1, 10 do
    c:get()
end
test_run:cmd("setopt delimiter ''");
s:count()
s.index.pk:min()
s.index.pk:max()
s:drop()

--
-- Auto increment can be used in transaction.
--
s = box.schema.space.create('test', {engine = engine})
_ = s:create_index('pk')
box.begin() s:auto_increment{} s:auto_increment{} box.commit()
s:select()
box.begin() s:auto_increment{} s:auto_increment{} box.rollback()
s:select()
box.begin() s:auto_increment{} s:auto_increment{} box.commit()
s:select()
s:drop()

--
-- Auto increment is persistent.
--
s1 = box.schema.space.create('test1', {engine = engine})
_ = s1:create_index('pk')
s1:auto_increment{}
s1:auto_increment{}
_ = s1:delete(2)
s2 = box.schema.space.create('test2', {engine = engine})
_ = s2:create_index('pk')
s2:auto_increment{}
s2:auto_increment{}
box.snapshot()
_ = s2:delete(2)
s3 = box.schema.space.create('test3', {engine = engine})
_ = s3:create_index('pk')
s3:auto_increment{}
s3:auto_increment{}
_ = s3:delete(2)
test_run:cmd('restart server default')
s1 = box.space.test1
s2 = box.space.test2
s3 = box.space.test3
s1:auto_increment{}
s2:auto_increment{}
s3:auto_increment{}
s1:select()
s2:select()
s3:select()
s1:drop()
s2:drop()
s3:drop()
