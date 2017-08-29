#ifndef TARANTOOL_BOX_FIELD_DEF_H_INCLUDED
#define TARANTOOL_BOX_FIELD_DEF_H_INCLUDED
/*
 * Copyright 2010-2016, Tarantool AUTHORS, please see AUTHORS file.
 *
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

#include "opt_def.h"
#include "schema_def.h"

#if defined(__cplusplus)
extern "C" {
#endif /* defined(__cplusplus) */

/** \cond public */

/*
 * Possible field data types. Can't use STRS/ENUM macros for them,
 * since there is a mismatch between enum name (STRING) and type
 * name literal ("STR"). STR is already used as Objective C type.
 */
enum field_type {
	FIELD_TYPE_ANY = 0,
	FIELD_TYPE_UNSIGNED,
	FIELD_TYPE_STRING,
	FIELD_TYPE_ARRAY,
	FIELD_TYPE_NUMBER,
	FIELD_TYPE_INTEGER,
	FIELD_TYPE_SCALAR,
	FIELD_TYPE_MAP,
	field_type_MAX
};

/** \endcond public */

extern const char *field_type_strs[];

enum field_type
field_type_by_name(const char *name);

struct field_def {
	enum field_type type;
	char *name;
};

struct region;
/**
 * Initialize a field definition. Name is allocated on @a region.
 * Used to allocate temporary field_defs during DDL transaction.
 * Allocated field_defs are copied into tuple_format.fields before
 * end of a transaction, so region memory remains valid. Besides,
 * region memory allows to fast free all temporary field_defs
 * after commit.
 * @param[out] field_def Field definition to init.
 * @param region Region to allocate name.
 * @param field_type Field value type.
 * @param name Field name.
 * @param name_len Length of @a name.
 *
 * @retval Success.
 * @retval Memory error.
 */
int
field_def_create(struct field_def *field, struct region *region,
		 enum field_type type, const char *name, uint32_t name_len);

extern const struct opt_def field_def_reg[];
extern const struct field_def field_def_default;

#if defined(__cplusplus)
} /* extern "C" */

#include "diag.h"

static inline void
field_def_create_xc(struct field_def *field, struct region *region,
		    enum field_type type, const char *name, uint32_t name_len)
{
	if (field_def_create(field, region, type, name, name_len) != 0)
		diag_raise();
}

#endif /* defined(__cplusplus) */

#endif /* TARANTOOL_BOX_FIELD_DEF_H_INCLUDED */
