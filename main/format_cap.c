/*
 * Asterisk -- An open source telephony toolkit.
 *
 * Copyright (C) 2010, Digium, Inc.
 *
 * David Vossel <dvossel@digium.com>
 *
 * See http://www.asterisk.org for more information about
 * the Asterisk project. Please do not directly contact
 * any of the maintainers of this project for assistance;
 * the project provides a web site, mailing lists and IRC
 * channels for your use.
 *
 * This program is free software, distributed under the terms of
 * the GNU General Public License Version 2. See the LICENSE file
 * at the top of the source tree.
 */

/*!
 * \file
 * \brief Format Capability API
 *
 * \author David Vossel <dvossel@digium.com>
 */

/*** MODULEINFO
	<support_level>core</support_level>
 ***/

#include "asterisk.h"

ASTERISK_FILE_VERSION(__FILE__, "$Revision: 400356 $");

#include "asterisk/_private.h"
#include "asterisk/format.h"
#include "asterisk/format_cap.h"
#include "asterisk/frame.h"
#include "asterisk/astobj2.h"
#include "asterisk/utils.h"

#define FORMAT_STR_BUFSIZE 256

struct ast_format_cap {
	/* The capabilities structure is just an ao2 container of ast_formats */
	struct ao2_container *formats;
	struct ao2_iterator it;
	/*! TRUE if the formats container created without a lock. */
	struct ast_flags flags;
	unsigned int string_cache_valid;
	char format_strs[0];
};

/*! format exists within capabilities structure if it is identical to
 * another format, or if the format is a proper subset of another format. */
static int cmp_cb(void *obj, void *arg, int flags)
{
	struct ast_format *format1 = arg;
	struct ast_format *format2 = obj;
	enum ast_format_cmp_res res = ast_format_cmp(format1, format2);

	return ((res == AST_FORMAT_CMP_EQUAL) ||
			(res == AST_FORMAT_CMP_SUBSET)) ?
				CMP_MATCH | CMP_STOP :
				0;
}

static int hash_cb(const void *obj, const int flags)
{
	const struct ast_format *format = obj;
	return format->id;
}

struct ast_format_cap *ast_format_cap_alloc(enum ast_format_cap_flags flags)
{
	struct ast_format_cap *cap;
	unsigned int alloc_size;
	
	alloc_size = sizeof(*cap);
	if (flags & AST_FORMAT_CAP_FLAG_CACHE_STRINGS) {
		alloc_size += FORMAT_STR_BUFSIZE;
	}

	cap = ast_calloc(1, alloc_size);
	if (!cap) {
		return NULL;
	}

	ast_set_flag(&cap->flags, flags);
	cap->formats = ao2_container_alloc_options(
		(flags & AST_FORMAT_CAP_FLAG_NOLOCK) ?
		AO2_ALLOC_OPT_LOCK_NOLOCK :
		AO2_ALLOC_OPT_LOCK_MUTEX,
		11, hash_cb, cmp_cb);
	if (!cap->formats) {
		ast_free(cap);
		return NULL;
	}

	return cap;
}

void *ast_format_cap_destroy(struct ast_format_cap *cap)
{
	if (!cap) {
		return NULL;
	}
	ao2_ref(cap->formats, -1);
	ast_free(cap);
	return NULL;
}

static void format_cap_add(struct ast_format_cap *cap, const struct ast_format *format)
{
	struct ast_format *fnew;

	if (!format || !format->id) {
		return;
	}
	if (!(fnew = ao2_alloc(sizeof(struct ast_format), NULL))) {
		return;
	}
	ast_format_copy(fnew, format);
	ao2_link(cap->formats, fnew);

	ao2_ref(fnew, -1);
}

void ast_format_cap_add(struct ast_format_cap *cap, const struct ast_format *format)
{
	format_cap_add(cap, format);
	cap->string_cache_valid = 0;
}

void ast_format_cap_add_all_by_type(struct ast_format_cap *cap, enum ast_format_type type)
{
	int x;
	size_t f_len = 0;
	const struct ast_format_list *f_list = ast_format_list_get(&f_len);

	for (x = 0; x < f_len; x++) {
		if (AST_FORMAT_GET_TYPE(f_list[x].format.id) == type) {
			format_cap_add(cap, &f_list[x].format);
		}
	}
	cap->string_cache_valid = 0;
	ast_format_list_destroy(f_list);
}

void ast_format_cap_add_all(struct ast_format_cap *cap)
{
	int x;
	size_t f_len = 0;
	const struct ast_format_list *f_list = ast_format_list_get(&f_len);

	for (x = 0; x < f_len; x++) {
		format_cap_add(cap, &f_list[x].format);
	}
	cap->string_cache_valid = 0;
	ast_format_list_destroy(f_list);
}

static int append_cb(void *obj, void *arg, int flag)
{
	struct ast_format_cap *result = (struct ast_format_cap *) arg;
	struct ast_format *format = (struct ast_format *) obj;

	if (!ast_format_cap_iscompatible(result, format)) {
		format_cap_add(result, format);
	}

	return 0;
}

void ast_format_cap_append(struct ast_format_cap *dst, const struct ast_format_cap *src)
{
	ao2_callback(src->formats, OBJ_NODATA, append_cb, dst);
	dst->string_cache_valid = 0;
}

static int copy_cb(void *obj, void *arg, int flag)
{
	struct ast_format_cap *result = (struct ast_format_cap *) arg;
	struct ast_format *format = (struct ast_format *) obj;

	format_cap_add(result, format);
	return 0;
}

static void format_cap_remove_all(struct ast_format_cap *cap)
{
	ao2_callback(cap->formats, OBJ_NODATA | OBJ_MULTIPLE | OBJ_UNLINK, NULL, NULL);
}

void ast_format_cap_copy(struct ast_format_cap *dst, const struct ast_format_cap *src)
{
	format_cap_remove_all(dst);
	ao2_callback(src->formats, OBJ_NODATA, copy_cb, dst);
	dst->string_cache_valid = 0;
}

struct ast_format_cap *ast_format_cap_dup(const struct ast_format_cap *cap)
{
	struct ast_format_cap *dst;

	dst = ast_format_cap_alloc(ast_test_flag(&cap->flags, AST_FLAGS_ALL));
	if (!dst) {
		return NULL;
	}
	ao2_callback(cap->formats, OBJ_NODATA, copy_cb, dst);
	dst->string_cache_valid = 0;
	return dst;
}

int ast_format_cap_is_empty(const struct ast_format_cap *cap)
{
	if (!cap) {
		return 1;
	}
	return ao2_container_count(cap->formats) == 0 ? 1 : 0;
}

static int find_exact_cb(void *obj, void *arg, int flag)
{
	struct ast_format *format1 = (struct ast_format *) arg;
	struct ast_format *format2 = (struct ast_format *) obj;

	return (ast_format_cmp(format1, format2) == AST_FORMAT_CMP_EQUAL) ? CMP_MATCH : 0;
}

int ast_format_cap_remove(struct ast_format_cap *cap, struct ast_format *format)
{
	struct ast_format *fremove;

	fremove = ao2_callback(cap->formats, OBJ_POINTER | OBJ_UNLINK, find_exact_cb, format);
	if (fremove) {
		ao2_ref(fremove, -1);
		cap->string_cache_valid = 0;
		return 0;
	}

	return -1;
}

struct multiple_by_id_data {
	struct ast_format *format;
	int match_found;
};

static int multiple_by_id_cb(void *obj, void *arg, int flag)
{
	struct multiple_by_id_data *data = arg;
	struct ast_format *format = obj;
	int res;

	res = (format->id == data->format->id) ? CMP_MATCH : 0;
	if (res) {
		data->match_found = 1;
	}

	return res;
}

int ast_format_cap_remove_byid(struct ast_format_cap *cap, enum ast_format_id id)
{
	struct ast_format format = {
		.id = id,
	};
	struct multiple_by_id_data data = {
		.format = &format,
		.match_found = 0,
	};

	ao2_callback(cap->formats,
		OBJ_NODATA | OBJ_MULTIPLE | OBJ_UNLINK,
		multiple_by_id_cb,
		&data);

	/* match_found will be set if at least one item was removed */
	if (data.match_found) {
		cap->string_cache_valid = 0;
		return 0;
	}

	return -1;
}

static int multiple_by_type_cb(void *obj, void *arg, int flag)
{
	int *type = arg;
	struct ast_format *format = obj;
	return ((AST_FORMAT_GET_TYPE(format->id)) == *type) ? CMP_MATCH : 0;
}

void ast_format_cap_remove_bytype(struct ast_format_cap *cap, enum ast_format_type type)
{
	ao2_callback(cap->formats,
		OBJ_UNLINK | OBJ_NODATA | OBJ_MULTIPLE,
		multiple_by_type_cb,
		&type);
	cap->string_cache_valid = 0;
}

void ast_format_cap_remove_all(struct ast_format_cap *cap)
{
	format_cap_remove_all(cap);
	cap->string_cache_valid = 0;
}

void ast_format_cap_set(struct ast_format_cap *cap, struct ast_format *format)
{
	format_cap_remove_all(cap);
	format_cap_add(cap, format);
	cap->string_cache_valid = 0;
}

int ast_format_cap_get_compatible_format(const struct ast_format_cap *cap, const struct ast_format *format, struct ast_format *result)
{
	struct ast_format *f;
	struct ast_format_cap *tmp_cap = (struct ast_format_cap *) cap;

	f = ao2_find(tmp_cap->formats, format, OBJ_POINTER);
	if (f) {
		ast_format_copy(result, f);
		ao2_ref(f, -1);
		return 1;
	}
	ast_format_clear(result);
	return 0;
}

int ast_format_cap_iscompatible(const struct ast_format_cap *cap, const struct ast_format *format)
{
	struct ast_format *f;
	struct ast_format_cap *tmp_cap = (struct ast_format_cap *) cap;

	if (!tmp_cap) {
		return 0;
	}

	f = ao2_find(tmp_cap->formats, format, OBJ_POINTER);
	if (f) {
		ao2_ref(f, -1);
		return 1;
	}

	return 0;
}

struct byid_data {
	struct ast_format *result;
	enum ast_format_id id;
};
static int find_best_byid_cb(void *obj, void *arg, int flag)
{
	struct ast_format *format = obj;
	struct byid_data *data = arg;

	if (data->id != format->id) {
		return 0;
	}
	if (!data->result->id || (ast_format_rate(data->result) < ast_format_rate(format))) {
		ast_format_copy(data->result, format);
	}
	return 0;
}

int ast_format_cap_best_byid(const struct ast_format_cap *cap, enum ast_format_id id, struct ast_format *result)
{
	struct byid_data data;
	data.result = result;
	data.id = id;

	ast_format_clear(result);
	ao2_callback(cap->formats,
		OBJ_MULTIPLE | OBJ_NODATA,
		find_best_byid_cb,
		&data);
	return result->id ? 1 : 0;
}

/*! \internal
 * \brief this struct is just used for the ast_format_cap_joint function so we can provide
 * both a format and a result ast_format_cap structure as arguments to the find_joint_cb
 * ao2 callback function.
 */
struct find_joint_data {
	/*! format to compare to for joint capabilities */
	struct ast_format *format;
	/*! if joint formmat exists with above format, add it to the result container */
	struct ast_format_cap *joint_cap;
	int joint_found;
};

static int find_joint_cb(void *obj, void *arg, int flag)
{
	struct ast_format *format = obj;
	struct find_joint_data *data = arg;

	struct ast_format tmp = { 0, };
	if (!ast_format_joint(format, data->format, &tmp)) {
		if (data->joint_cap) {
			ast_format_cap_add(data->joint_cap, &tmp);
		}
		data->joint_found++;
	}

	return 0;
}

int ast_format_cap_has_joint(const struct ast_format_cap *cap1, const struct ast_format_cap *cap2)
{
	struct ao2_iterator it;
	struct ast_format *tmp;
	struct find_joint_data data = {
		.joint_found = 0,
		.joint_cap = NULL,
	};

	it = ao2_iterator_init(cap1->formats, 0);
	while ((tmp = ao2_iterator_next(&it))) {
		data.format = tmp;
		ao2_callback(cap2->formats,
			OBJ_MULTIPLE | OBJ_NODATA,
			find_joint_cb,
			&data);
		ao2_ref(tmp, -1);
	}
	ao2_iterator_destroy(&it);

	return data.joint_found ? 1 : 0;
}

int ast_format_cap_identical(const struct ast_format_cap *cap1, const struct ast_format_cap *cap2)
{
	struct ao2_iterator it;
	struct ast_format *tmp;

	if (ao2_container_count(cap1->formats) != ao2_container_count(cap2->formats)) {
		return 0; /* if they are not the same size, they are not identical */
	}

	it = ao2_iterator_init(cap1->formats, 0);
	while ((tmp = ao2_iterator_next(&it))) {
		if (!ast_format_cap_iscompatible(cap2, tmp)) {
			ao2_ref(tmp, -1);
			ao2_iterator_destroy(&it);
			return 0;
		}
		ao2_ref(tmp, -1);
	}
	ao2_iterator_destroy(&it);

	return 1;
}

struct ast_format_cap *ast_format_cap_joint(const struct ast_format_cap *cap1, const struct ast_format_cap *cap2)
{
	struct ao2_iterator it;
	struct ast_format_cap *result = ast_format_cap_alloc(AST_FORMAT_CAP_FLAG_NOLOCK);
	struct ast_format *tmp;
	struct find_joint_data data = {
		.joint_found = 0,
		.joint_cap = result,
	};
	if (!result) {
		return NULL;
	}

	it = ao2_iterator_init(cap1->formats, 0);
	while ((tmp = ao2_iterator_next(&it))) {
		data.format = tmp;
		ao2_callback(cap2->formats,
			OBJ_MULTIPLE | OBJ_NODATA,
			find_joint_cb,
			&data);
		ao2_ref(tmp, -1);
	}
	ao2_iterator_destroy(&it);

	if (ao2_container_count(result->formats)) {
		return result;
	}

	result = ast_format_cap_destroy(result);
	return NULL;
}

static int joint_copy_helper(const struct ast_format_cap *cap1, const struct ast_format_cap *cap2, struct ast_format_cap *result, int append)
{
	struct ao2_iterator it;
	struct ast_format *tmp;
	struct find_joint_data data = {
		.joint_cap = result,
		.joint_found = 0,
	};
	if (!append) {
		format_cap_remove_all(result);
	}
	it = ao2_iterator_init(cap1->formats, 0);
	while ((tmp = ao2_iterator_next(&it))) {
		data.format = tmp;
		ao2_callback(cap2->formats,
			OBJ_MULTIPLE | OBJ_NODATA,
			find_joint_cb,
			&data);
		ao2_ref(tmp, -1);
	}
	ao2_iterator_destroy(&it);

	result->string_cache_valid = 0;

	return ao2_container_count(result->formats) ? 1 : 0;
}

int ast_format_cap_joint_append(const struct ast_format_cap *cap1, const struct ast_format_cap *cap2, struct ast_format_cap *result)
{
	return joint_copy_helper(cap1, cap2, result, 1);
}

int ast_format_cap_joint_copy(const struct ast_format_cap *cap1, const struct ast_format_cap *cap2, struct ast_format_cap *result)
{
	return joint_copy_helper(cap1, cap2, result, 0);
}

struct ast_format_cap *ast_format_cap_get_type(const struct ast_format_cap *cap, enum ast_format_type ftype)
{
	struct ao2_iterator it;
	struct ast_format_cap *result = ast_format_cap_alloc(AST_FORMAT_CAP_FLAG_NOLOCK);
	struct ast_format *tmp;

	if (!result) {
		return NULL;
	}

	/* for each format in cap1, see if that format is
	 * compatible with cap2. If so copy it to the result */
	it = ao2_iterator_init(cap->formats, 0);
	while ((tmp = ao2_iterator_next(&it))) {
		if (AST_FORMAT_GET_TYPE(tmp->id) == ftype) {
			/* copy format */
			ast_format_cap_add(result, tmp);
		}
		ao2_ref(tmp, -1);
	}
	ao2_iterator_destroy(&it);

	if (ao2_container_count(result->formats)) {
		return result;
	}
	result = ast_format_cap_destroy(result);

	return NULL;
}


int ast_format_cap_has_type(const struct ast_format_cap *cap, enum ast_format_type type)
{
	struct ao2_iterator it;
	struct ast_format *tmp;

	it = ao2_iterator_init(cap->formats, 0);
	while ((tmp = ao2_iterator_next(&it))) {
		if (AST_FORMAT_GET_TYPE(tmp->id) == type) {
			ao2_ref(tmp, -1);
			ao2_iterator_destroy(&it);
			return 1;
		}
		ao2_ref(tmp, -1);
	}
	ao2_iterator_destroy(&it);

	return 0;
}

void ast_format_cap_iter_start(struct ast_format_cap *cap)
{
	/* We can unconditionally lock even if the container has no lock. */
	ao2_lock(cap->formats);
	cap->it = ao2_iterator_init(cap->formats, AO2_ITERATOR_DONTLOCK);
}

void ast_format_cap_iter_end(struct ast_format_cap *cap)
{
	ao2_iterator_destroy(&cap->it);
	/* We can unconditionally unlock even if the container has no lock. */
	ao2_unlock(cap->formats);
}

int ast_format_cap_iter_next(struct ast_format_cap *cap, struct ast_format *format)
{
	struct ast_format *tmp = ao2_iterator_next(&cap->it);

	if (!tmp) {
		return -1;
	}
	ast_format_copy(format, tmp);
	ao2_ref(tmp, -1);

	return 0;
}

static char *getformatname_multiple(char *buf, size_t size, struct ast_format_cap *cap)
{
	int x;
	unsigned len;
	char *start, *end = buf;
	struct ast_format tmp_fmt;
	size_t f_len;
	const struct ast_format_list *f_list = ast_format_list_get(&f_len);

	if (!size) {
		f_list = ast_format_list_destroy(f_list);
		return buf;
	}
	snprintf(end, size, "(");
	len = strlen(end);
	end += len;
	size -= len;
	start = end;
	for (x = 0; x < f_len; x++) {
		ast_format_copy(&tmp_fmt, &f_list[x].format);
		if (ast_format_cap_iscompatible(cap, &tmp_fmt)) {
			snprintf(end, size, "%s|", f_list[x].name);
			len = strlen(end);
			end += len;
			size -= len;
		}
	}
	if (start == end) {
		ast_copy_string(start, "nothing)", size);
	} else if (size > 1) {
		*(end - 1) = ')';
	}
	f_list = ast_format_list_destroy(f_list);
	return buf;
}

char *ast_getformatname_multiple(char *buf, size_t size, struct ast_format_cap *cap)
{
	if (ast_test_flag(&cap->flags, AST_FORMAT_CAP_FLAG_CACHE_STRINGS)) {
		if (!cap->string_cache_valid) {
			getformatname_multiple(cap->format_strs, FORMAT_STR_BUFSIZE, cap);
			cap->string_cache_valid = 1;
		}
		ast_copy_string(buf, cap->format_strs, size);
		return buf;
	}

	return getformatname_multiple(buf, size, cap);
}

uint64_t ast_format_cap_to_old_bitfield(const struct ast_format_cap *cap)
{
	uint64_t res = 0;
	struct ao2_iterator it;
	struct ast_format *tmp;

	it = ao2_iterator_init(cap->formats, 0);
	while ((tmp = ao2_iterator_next(&it))) {
		res |= ast_format_to_old_bitfield(tmp);
		ao2_ref(tmp, -1);
	}
	ao2_iterator_destroy(&it);
	return res;
}

void ast_format_cap_from_old_bitfield(struct ast_format_cap *dst, uint64_t src)
{
	uint64_t tmp = 0;
	int x;
	struct ast_format tmp_format = { 0, };

	format_cap_remove_all(dst);
	for (x = 0; x < 64; x++) {
		tmp = (1ULL << x);
		if (tmp & src) {
			format_cap_add(dst, ast_format_from_old_bitfield(&tmp_format, tmp));
		}
	}
	dst->string_cache_valid = 0;
}
