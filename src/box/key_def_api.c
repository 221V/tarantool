/*
 * Copyright 2010-2020, Tarantool AUTHORS, please see AUTHORS file.
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
#include "key_def_api.h"
#include "key_def.h"
#include "small/region.h"
#include "coll_id_cache.h"
#include "tuple_format.h"
#include "field_def.h"
#include "coll_id_cache.h"
#include "fiber.h"
#include "tuple.h"
#include <msgpuck.h>

/* {{{ Helpers */

static int
key_def_set_internal_part(struct key_part_def *internal_part,
			  box_key_part_def_t *part)
{
	*internal_part = key_part_def_default;

	/* Set internal_part->fieldno. */
	internal_part->fieldno = part->fieldno;

	/* Set internal_part->type. */
	if (part->field_type == NULL) {
		diag_set(IllegalParams, "Field type is mandatory");
		return -1;
	}
	size_t type_len = strlen(part->field_type);
	internal_part->type = field_type_by_name(part->field_type, type_len);
	if (internal_part->type == field_type_MAX) {
		diag_set(IllegalParams, "Unknown field type: \"%s\"",
			 part->field_type);
		return -1;
	}

	/* Set internal_part->is_nullable. */
	internal_part->is_nullable =
		(part->flags & BOX_KEY_PART_DEF_IS_NULLABLE_MASK) ==
		BOX_KEY_PART_DEF_IS_NULLABLE_MASK;

	/* Set internal_part->coll_id. */
	if (part->collation != NULL) {
		size_t collation_len = strlen(part->collation);
		struct coll_id *coll_id = coll_by_name(part->collation,
						       collation_len);
		if (coll_id == NULL) {
			diag_set(IllegalParams, "Unknown collation: \"%s\"",
				 part->collation);
			return -1;
		}
		internal_part->coll_id = coll_id->id;
	}

	return 0;
}

/* }}} Helpers */

/* {{{ API functions implementations */

void
box_key_part_def_create(box_key_part_def_t *part)
{
	memset(part, 0, sizeof(*part));
}

box_key_def_t *
box_key_def_new(uint32_t *fields, uint32_t *types, uint32_t part_count)
{
	size_t sz = key_def_sizeof(part_count);
	struct key_def *key_def = calloc(1, sz);
	if (key_def == NULL) {
		diag_set(OutOfMemory, sz, "malloc", "struct key_def");
		return NULL;
	}

	key_def->part_count = part_count;
	key_def->unique_part_count = part_count;

	for (uint32_t item = 0; item < part_count; ++item) {
		key_def_set_part_175(key_def, item, fields[item],
				     (enum field_type)types[item]);
	}
	key_def_set_cmp(key_def);
	return key_def;
}

box_key_def_t *
box_key_def_new_ex(box_key_part_def_t *parts, uint32_t part_count)
{
	struct region *region = &fiber()->gc;
	size_t region_svp = region_used(region);
	size_t internal_parts_size;
	struct key_part_def *internal_parts =
		region_alloc_array(region, typeof(internal_parts[0]),
				   part_count, &internal_parts_size);
	if (parts == NULL) {
		diag_set(OutOfMemory, internal_parts_size, "region_alloc_array",
			 "parts");
		return NULL;
	}
	if (part_count == 0) {
		diag_set(IllegalParams, "At least one key part is required");
		return NULL;
	}

	/*
	 * It is possible to implement a function similar to
	 * key_def_new() and eliminate <box_key_part_def_t> ->
	 * <struct key_part_def> copying. However this would lead
	 * to code duplication and would complicate maintanence,
	 * so it worth to do so only if key_def creation will
	 * appear on a hot path in some meaningful use case.
	 */
	uint32_t min_field_count = 0;
	for (uint32_t i = 0; i < part_count; ++i) {
		if (key_def_set_internal_part(&internal_parts[i],
					      &parts[i]) != 0) {
			region_truncate(region, region_svp);
			return NULL;
		}
		bool is_nullable =
			(parts[i].flags & BOX_KEY_PART_DEF_IS_NULLABLE_MASK) ==
			BOX_KEY_PART_DEF_IS_NULLABLE_MASK;
		if (!is_nullable && parts[i].fieldno > min_field_count)
			min_field_count = parts[i].fieldno;
	}

	struct key_def *key_def = key_def_new(internal_parts, part_count);
	region_truncate(region, region_svp);
	if (key_def == NULL)
		return NULL;

	/*
	 * Update key_def->has_optional_parts and function
	 * pointers.
	 *
	 * FIXME: It seems, this call should be part of
	 * key_def_new(), because otherwise a callee function may
	 * obtain an incorrect key_def. However I don't know any
	 * case that would prove this guess.
	 */
	key_def_update_optionality(key_def, min_field_count);

	return key_def;
}

void
box_key_def_delete(box_key_def_t *key_def)
{
	key_def_delete(key_def);
}

box_key_part_def_t *
box_key_def_dump_parts(const box_key_def_t *key_def, uint32_t *part_count_ptr)
{
	struct region *region = &fiber()->gc;
	size_t region_svp = region_used(region);
	size_t size;
	box_key_part_def_t *parts = region_alloc_array(
		region, typeof(parts[0]), key_def->part_count, &size);
	if (parts == NULL) {
		diag_set(OutOfMemory, size, "region_alloc_array", "parts");
		return NULL;
	}

	for (uint32_t i = 0; i < key_def->part_count; i++) {
		const struct key_part *part = &key_def->parts[i];
		box_key_part_def_t *part_def = &parts[i];
		box_key_part_def_create(part_def);

		/* Set part->{fieldno,flags,field_type}. */
		part_def->fieldno = part->fieldno;
		part_def->flags = 0;
		if (part->is_nullable)
			part_def->flags |= BOX_KEY_PART_DEF_IS_NULLABLE_MASK;
		assert(part->type >= 0 && part->type < field_type_MAX);
		part_def->field_type = field_type_strs[part->type];

		/* Set part->collation. */
		if (part->coll_id != COLL_NONE) {
			struct coll_id *coll_id = coll_by_id(part->coll_id);
			/*
			 * A collation may be removed after
			 * key_def creation.
			 */
			if (coll_id == NULL) {
				diag_set(CollationError,
					 "key_def holds dead collation id %d",
					 part->coll_id);
				region_truncate(region, region_svp);
				return NULL;
			}
			part_def->collation = coll_id->name;
		}
	}

	if (part_count_ptr != NULL)
		*part_count_ptr = key_def->part_count;

	return parts;
}

int
box_tuple_validate_key_parts(box_key_def_t *key_def, box_tuple_t *tuple)
{
	for (uint32_t idx = 0; idx < key_def->part_count; ++idx) {
		struct key_part *part = &key_def->parts[idx];
		const char *field = tuple_field_by_part(tuple, part);
		if (field == NULL) {
			if (part->is_nullable)
				continue;
			diag_set(ClientError, ER_NO_SUCH_FIELD,
				 part->fieldno + TUPLE_INDEX_BASE);
			return -1;
		}
		enum mp_type mp_type = mp_typeof(*field);
		if (key_mp_type_validate(part->type, mp_type, ER_FIELD_TYPE,
					 idx, part->is_nullable) != 0)
			return -1;
	}
	return 0;
}

int
box_tuple_compare(const box_tuple_t *tuple_a, const box_tuple_t *tuple_b,
		  box_key_def_t *key_def)
{
	return tuple_compare(tuple_a, tuple_b, key_def);
}

int
box_tuple_compare_with_key(const box_tuple_t *tuple_a, const char *key_b,
			   box_key_def_t *key_def)
{
	uint32_t part_count = mp_decode_array(&key_b);
	return tuple_compare_with_key(tuple_a, key_b, part_count, key_def);

}

/* }}} API functions implementations */
