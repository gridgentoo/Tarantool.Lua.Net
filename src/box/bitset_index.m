/*
 * Redistribution and use in source and binary forms, with or
 * without modification, are permitted provided that the following
 * conditions are met:
 *
 * 1. Redistributions of source code must retain the above
 *    copyright notice, this list of conditions and the
 *    following disclaimer.
 *
 * 2. Redistributions in binary form must reproduce the above
 *    copyright notice, this list of conditions and the following
 *    disclaimer in the documentation and/or other materials
 *    provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY <COPYRIGHT HOLDER> ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED
 * TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL
 * <COPYRIGHT HOLDER> OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT,
 * INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR
 * BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF
 * LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF
 * THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

#include "bitset_index.h"

#include <string.h>

#include "salloc.h"
#include "tuple.h"
#include "space.h"
#include "exception.h"
#include "pickle.h"
#include <lib/bitset/index.h>

static struct index_traits bitset_index_traits = {
	.allows_partial_key = false,
};

static inline size_t
tuple_to_value(struct tuple *tuple)
{
#if 0
	size_t value = salloc_ptr_to_index(tuple);
	assert(salloc_ptr_from_index(value) == tuple);
	return value;
#endif
	return (size_t) tuple;
}

static inline struct tuple *
value_to_tuple(size_t value)
{
#if 0
	return salloc_ptr_from_index(value);
#endif
	return (struct tuple *) value;
}

struct iterator_wrapper {
	struct iterator base; /* Must be the first member. */
	struct bitset_iterator bitset_it;
};

static struct iterator_wrapper *
iterator_wrapper(struct iterator *it)
{
	return (struct iterator_wrapper *) it;
}

void
iterator_wrapper_free(struct iterator *iterator)
{
	assert(iterator->free == iterator_wrapper_free);
	struct iterator_wrapper *it = iterator_wrapper(iterator);

	bitset_iterator_destroy(&it->bitset_it);
	free(it);
}

struct tuple *
iterator_wrapper_next(struct iterator *iterator)
{
	assert(iterator->free == iterator_wrapper_free);
	struct iterator_wrapper *it = iterator_wrapper(iterator);

	size_t value = bitset_iterator_next(&it->bitset_it);
	if (value == SIZE_MAX)
		return NULL;

	return value_to_tuple(value);
}

@implementation BitsetIndex;

+ (struct index_traits *) traits
{
	return &bitset_index_traits;
}

- (id) init: (struct key_def *) key_def_arg :(struct space *) space_arg
{
	assert (!key_def_arg->is_unique);

	self = [super init: key_def_arg :space_arg];
	assert (self != NULL);

	if (bitset_index_create(&self->index, realloc) != 0)
		panic_syserror("bitset_index_create");

	return self;
}

- (void) free
{
	bitset_index_destroy(&self->index);
	[super free];
}

- (void) beginBuild
{
	tnt_raise(ClientError, :ER_UNSUPPORTED, "BitsetIndex", "beginBuild()");
}

- (void) buildNext: (struct tuple *)tuple
{
	(void) tuple;
	tnt_raise(ClientError, :ER_UNSUPPORTED, "BitsetIndex", "buildNext()");
}

- (void) endBuild
{
	tnt_raise(ClientError, :ER_UNSUPPORTED, "BitsetIndex", "endBuild()");
}

- (void) build: (Index *) pk
{
	assert (!key_def->is_unique);

	struct iterator *it = pk->position;
	struct tuple *tuple;
	[pk initIterator: it :ITER_ALL :NULL :0];

	while ((tuple = it->next(it)))
		[self replace: NULL :tuple :DUP_INSERT];
}

- (size_t) size
{
	return bitset_index_size(&self->index);
}

- (struct tuple *) min
{
	tnt_raise(ClientError, :ER_UNSUPPORTED, "BitsetIndex", "min()");
	return NULL;
}

- (struct tuple *) max
{
	tnt_raise(ClientError, :ER_UNSUPPORTED, "BitsetIndex", "max()");
	return NULL;
}

- (struct iterator *) allocIterator
{
	struct iterator_wrapper *it = malloc(sizeof(struct iterator_wrapper));
	if (!it)
		return NULL;

	memset(it, 0, sizeof(struct iterator_wrapper));
	it->base.next = iterator_wrapper_next;
	it->base.free = iterator_wrapper_free;

	bitset_iterator_create(&it->bitset_it, realloc);

	return (struct iterator *) it;
}

- (struct tuple *) findByKey: (void *) key :(int) part_count
{
	(void) key;
	(void) part_count;
	tnt_raise(ClientError, :ER_UNSUPPORTED, "BitsetIndex", "findByKey()");
	return NULL;
}

- (struct tuple *) findByTuple: (struct tuple *) tuple
{
	(void) tuple;
	tnt_raise(ClientError, :ER_UNSUPPORTED, "BitsetIndex", "findByTuple()");
	return NULL;
}

- (struct tuple *) replace: (struct tuple *) old_tuple
	: (struct tuple *) new_tuple
	: (u32) flags
{
	assert(!key_def->is_unique);
	assert(old_tuple != NULL || new_tuple != NULL);
	(void) flags;

	struct tuple *ret = NULL;

	if (old_tuple != NULL) {
		size_t value = tuple_to_value(old_tuple);
#if defined(DEBUG)
		say_debug("BitsetIndex: remove value = %zu (%p)",
			  value, old_tuple);
#endif /* defined(DEBUG) */
		if (bitset_index_contains_value(&self->index, value)) {
			ret = old_tuple;

			assert (old_tuple != new_tuple);
			bitset_index_remove_value(&self->index, value);
		}
	}

	if (new_tuple != NULL) {
		const void *field = tuple_field(new_tuple,
						key_def->parts[0].fieldno);
		assert (field != NULL);
		size_t bitset_key_size = (size_t) load_varint32(&field);
		const void *bitset_key = field;

		size_t value = tuple_to_value(new_tuple);
#if defined(DEBUG)
		say_debug("BitsetIndex: insert value = %zu (%p)",
			  value, new_tuple);
#endif /* defined(DEBUG) */
		if (bitset_index_insert(&self->index, bitset_key,
					bitset_key_size, value) < 0) {
			tnt_raise(ClientError, :ER_MEMORY_ISSUE, 0,
				  "BitsetIndex", "insert");
		}
	}

	return ret;
}

- (void) initIterator: (struct iterator *) iterator:(enum iterator_type) type
      :(const void *) key :(int) part_count
{
	assert(iterator->free == iterator_wrapper_free);
	struct iterator_wrapper *it = iterator_wrapper(iterator);

	const void *bitset_key = NULL;
	size_t bitset_key_size = 0;

	if (type != ITER_ALL) {
		check_key_parts(key_def, part_count,
				bitset_index_traits.allows_partial_key);
		const void *key2 = key;
		bitset_key_size = (size_t) load_varint32(&key2);
		bitset_key = key2;
	}

	struct bitset_expr expr;
	bitset_expr_create(&expr, realloc);
	@try {
		int rc = 0;
		switch (type) {
		case ITER_ALL:
			rc = bitset_index_expr_all(&expr);
			break;
		case ITER_EQ:
			rc = bitset_index_expr_equals(&expr, bitset_key,
						      bitset_key_size);
			break;
		case ITER_BITS_ALL_SET:
			rc = bitset_index_expr_all_set(&expr, bitset_key,
						       bitset_key_size);
			break;
		case ITER_BITS_ALL_NOT_SET:
			rc = bitset_index_expr_all_not_set(&expr, bitset_key,
							   bitset_key_size);
			break;
		case ITER_BITS_ANY_SET:
			rc = bitset_index_expr_any_set(&expr, bitset_key,
						       bitset_key_size);
			break;
		default:
			tnt_raise(ClientError, :ER_UNSUPPORTED,
				  "BitsetIndex", "requested iterator type");
		}

		if (rc != 0) {
			tnt_raise(ClientError, :ER_MEMORY_ISSUE,
				  0, "BitsetIndex", "iterator expression");
		}

		if (bitset_index_init_iterator(&self->index, &it->bitset_it,
					       &expr) != 0) {
			tnt_raise(ClientError, :ER_MEMORY_ISSUE,
				  0, "BitsetIndex", "iterator state");
		}
	} @finally {
		bitset_expr_destroy(&expr);
	}
}

@end
