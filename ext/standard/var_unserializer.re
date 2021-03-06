/*
  +----------------------------------------------------------------------+
  | PHP Version 5                                                        |
  +----------------------------------------------------------------------+
  | Copyright (c) 1997-2016 The PHP Group                                |
  +----------------------------------------------------------------------+
  | This source file is subject to version 3.01 of the PHP license,      |
  | that is bundled with this package in the file LICENSE, and is        |
  | available through the world-wide-web at the following url:           |
  | http://www.php.net/license/3_01.txt                                  |
  | If you did not receive a copy of the PHP license and are unable to   |
  | obtain it through the world-wide-web, please send a note to          |
  | license@php.net so we can mail you a copy immediately.               |
  +----------------------------------------------------------------------+
  | Author: Sascha Schumann <sascha@schumann.cx>                         |
  +----------------------------------------------------------------------+
*/

/* $Id$ */

#include "php.h"
#include "ext/standard/php_var.h"
#include "php_incomplete_class.h"

/* {{{ reference-handling for unserializer: var_* */
#define VAR_ENTRIES_MAX 1024
#define VAR_ENTRIES_DBG 0

#define VAR_WAKEUP_FLAG 1
#define WITH_WAKEUP_FLAG(zv_ptr) ((zval *) ((zend_uintptr_t) zv_ptr | VAR_WAKEUP_FLAG))
#define WITHOUT_WAKEUP_FLAG(zv_ptr) ((zval *) ((zend_uintptr_t) zv_ptr & ~VAR_WAKEUP_FLAG))
#define HAS_WAKEUP_FLAG(zv_ptr) ((zend_uintptr_t) zv_ptr & VAR_WAKEUP_FLAG)

typedef struct {
	zval *data[VAR_ENTRIES_MAX];
	long used_slots;
	void *next;
} var_entries;

static inline void var_push(php_unserialize_data_t *var_hashx, zval **rval)
{
	var_entries *var_hash = (*var_hashx)->last;
#if VAR_ENTRIES_DBG
	fprintf(stderr, "var_push(%ld): %d\n", var_hash?var_hash->used_slots:-1L, Z_TYPE_PP(rval));
#endif

	if (!var_hash || var_hash->used_slots == VAR_ENTRIES_MAX) {
		var_hash = emalloc(sizeof(var_entries));
		var_hash->used_slots = 0;
		var_hash->next = 0;

		if (!(*var_hashx)->first) {
			(*var_hashx)->first = var_hash;
		} else {
			((var_entries *) (*var_hashx)->last)->next = var_hash;
		}

		(*var_hashx)->last = var_hash;
	}

	var_hash->data[var_hash->used_slots++] = *rval;
}

static inline zval **get_var_push_dtor_slot(php_unserialize_data_t *var_hashx)
{
	var_entries *var_hash;

	if (!var_hashx || !*var_hashx) {
		return NULL;
	}

	var_hash = (*var_hashx)->last_dtor;
#if VAR_ENTRIES_DBG
	fprintf(stderr, "var_push_dtor(%p, %ld): %d\n", *rval, var_hash?var_hash->used_slots:-1L, Z_TYPE_PP(rval));
#endif

	if (!var_hash || var_hash->used_slots == VAR_ENTRIES_MAX) {
		var_hash = emalloc(sizeof(var_entries));
		var_hash->used_slots = 0;
		var_hash->next = 0;

		if (!(*var_hashx)->first_dtor) {
			(*var_hashx)->first_dtor = var_hash;
		} else {
			((var_entries *) (*var_hashx)->last_dtor)->next = var_hash;
		}

		(*var_hashx)->last_dtor = var_hash;
	}

	return &var_hash->data[var_hash->used_slots++];
}

PHPAPI void var_push_dtor(php_unserialize_data_t *var_hashx, zval **rval)
{
	zval **slot = get_var_push_dtor_slot(var_hashx);
	Z_ADDREF_PP(rval);
	*slot = *rval;
}

PHPAPI void var_push_dtor_no_addref(php_unserialize_data_t *var_hashx, zval **rval)
{
	var_entries *var_hash;

    if (!var_hashx || !*var_hashx) {
        return;
    }

    var_hash = (*var_hashx)->last_dtor;
#if VAR_ENTRIES_DBG
	fprintf(stderr, "var_push_dtor_no_addref(%p, %ld): %d (%d)\n", *rval, var_hash?var_hash->used_slots:-1L, Z_TYPE_PP(rval), Z_REFCOUNT_PP(rval));
#endif

	if (!var_hash || var_hash->used_slots == VAR_ENTRIES_MAX) {
		var_hash = emalloc(sizeof(var_entries));
		var_hash->used_slots = 0;
		var_hash->next = 0;

		if (!(*var_hashx)->first_dtor) {
			(*var_hashx)->first_dtor = var_hash;
		} else {
			((var_entries *) (*var_hashx)->last_dtor)->next = var_hash;
		}

		(*var_hashx)->last_dtor = var_hash;
	}

	var_hash->data[var_hash->used_slots++] = *rval;
}

PHPAPI void var_replace(php_unserialize_data_t *var_hashx, zval *ozval, zval **nzval)
{
	long i;
	var_entries *var_hash = (*var_hashx)->first;
#if VAR_ENTRIES_DBG
	fprintf(stderr, "var_replace(%ld): %d\n", var_hash?var_hash->used_slots:-1L, Z_TYPE_PP(nzval));
#endif

	while (var_hash) {
		for (i = 0; i < var_hash->used_slots; i++) {
			if (var_hash->data[i] == ozval) {
				var_hash->data[i] = *nzval;
				/* do not break here */
			}
		}
		var_hash = var_hash->next;
	}
}

static int var_access(php_unserialize_data_t *var_hashx, long id, zval ***store)
{
	var_entries *var_hash = (*var_hashx)->first;
#if VAR_ENTRIES_DBG
	fprintf(stderr, "var_access(%ld): %ld\n", var_hash?var_hash->used_slots:-1L, id);
#endif

	while (id >= VAR_ENTRIES_MAX && var_hash && var_hash->used_slots == VAR_ENTRIES_MAX) {
		var_hash = var_hash->next;
		id -= VAR_ENTRIES_MAX;
	}

	if (!var_hash) return !SUCCESS;

	if (id < 0 || id >= var_hash->used_slots) return !SUCCESS;

	*store = &var_hash->data[id];

	return SUCCESS;
}

PHPAPI void var_destroy(php_unserialize_data_t *var_hashx)
{
	void *next;
	long i;
	var_entries *var_hash = (*var_hashx)->first;
	zend_bool wakeup_failed = 0;
	TSRMLS_FETCH();

#if VAR_ENTRIES_DBG
	fprintf(stderr, "var_destroy(%ld)\n", var_hash?var_hash->used_slots:-1L);
#endif

	while (var_hash) {
		next = var_hash->next;
		efree(var_hash);
		var_hash = next;
	}

	var_hash = (*var_hashx)->first_dtor;

	while (var_hash) {
		for (i = 0; i < var_hash->used_slots; i++) {
			zval *zv = var_hash->data[i];
#if VAR_ENTRIES_DBG
    fprintf(stderr, "var_destroy dtor(%p, %ld)\n", var_hash->data[i], Z_REFCOUNT_P(var_hash->data[i]));
#endif

			if (HAS_WAKEUP_FLAG(zv)) {
				zv = WITHOUT_WAKEUP_FLAG(zv);
				if (!wakeup_failed) {
					zval *retval_ptr = NULL;
					zval wakeup_name;
					INIT_PZVAL(&wakeup_name);
					ZVAL_STRINGL(&wakeup_name, "__wakeup", sizeof("__wakeup") - 1, 0);

					BG(serialize_lock)++;
					if (call_user_function_ex(CG(function_table), &zv, &wakeup_name, &retval_ptr, 0, 0, 1, NULL TSRMLS_CC) == FAILURE || retval_ptr == NULL) {
						wakeup_failed = 1;
						zend_object_store_ctor_failed(zv TSRMLS_CC);
					}
					BG(serialize_lock)--;

					if (retval_ptr) {
						zval_ptr_dtor(&retval_ptr);
					}
				} else {
					zend_object_store_ctor_failed(zv TSRMLS_CC);
				}
			}

			zval_ptr_dtor(&zv);
		}
		next = var_hash->next;
		efree(var_hash);
		var_hash = next;
	}
}

/* }}} */

static char *unserialize_str(const unsigned char **p, size_t *len, size_t maxlen)
{
	size_t i, j;
	char *str = safe_emalloc(*len, 1, 1);
	unsigned char *end = *(unsigned char **)p+maxlen;

	if (end < *p) {
		efree(str);
		return NULL;
	}

	for (i = 0; i < *len; i++) {
		if (*p >= end) {
			efree(str);
			return NULL;
		}
		if (**p != '\\') {
			str[i] = (char)**p;
		} else {
			unsigned char ch = 0;

			for (j = 0; j < 2; j++) {
				(*p)++;
				if (**p >= '0' && **p <= '9') {
					ch = (ch << 4) + (**p -'0');
				} else if (**p >= 'a' && **p <= 'f') {
					ch = (ch << 4) + (**p -'a'+10);
				} else if (**p >= 'A' && **p <= 'F') {
					ch = (ch << 4) + (**p -'A'+10);
				} else {
					efree(str);
					return NULL;
				}
			}
			str[i] = (char)ch;
		}
		(*p)++;
	}
	str[i] = 0;
	*len = i;
	return str;
}

#define YYFILL(n) do { } while (0)
#define YYCTYPE unsigned char
#define YYCURSOR cursor
#define YYLIMIT limit
#define YYMARKER marker


/*!re2c
uiv = [+]? [0-9]+;
iv = [+-]? [0-9]+;
nv = [+-]? ([0-9]* "." [0-9]+|[0-9]+ "." [0-9]*);
nvexp = (iv | nv) [eE] [+-]? iv;
any = [\000-\377];
object = [OC];
*/



static inline long parse_iv2(const unsigned char *p, const unsigned char **q)
{
	char cursor;
	long result = 0;
	int neg = 0;

	switch (*p) {
		case '-':
			neg++;
			/* fall-through */
		case '+':
			p++;
	}

	while (1) {
		cursor = (char)*p;
		if (cursor >= '0' && cursor <= '9') {
			result = result * 10 + (size_t)(cursor - (unsigned char)'0');
		} else {
			break;
		}
		p++;
	}
	if (q) *q = p;
	if (neg) return -result;
	return result;
}

static inline long parse_iv(const unsigned char *p)
{
	return parse_iv2(p, NULL);
}

/* no need to check for length - re2c already did */
static inline size_t parse_uiv(const unsigned char *p)
{
	unsigned char cursor;
	size_t result = 0;

	if (*p == '+') {
		p++;
	}

	while (1) {
		cursor = *p;
		if (cursor >= '0' && cursor <= '9') {
			result = result * 10 + (size_t)(cursor - (unsigned char)'0');
		} else {
			break;
		}
		p++;
	}
	return result;
}

#define UNSERIALIZE_PARAMETER zval **rval, const unsigned char **p, const unsigned char *max, php_unserialize_data_t *var_hash TSRMLS_DC
#define UNSERIALIZE_PASSTHRU rval, p, max, var_hash TSRMLS_CC

static inline int process_nested_data(UNSERIALIZE_PARAMETER, HashTable *ht, long elements, int objprops)
{
	while (elements-- > 0) {
		zval *key, *data, **old_data;

		ALLOC_INIT_ZVAL(key);

		if (!php_var_unserialize(&key, p, max, NULL TSRMLS_CC)) {
            var_push_dtor_no_addref(var_hash, &key);
			return 0;
		}

		if (Z_TYPE_P(key) != IS_LONG && Z_TYPE_P(key) != IS_STRING) {
            var_push_dtor_no_addref(var_hash, &key);
			return 0;
		}

		ALLOC_INIT_ZVAL(data);

		if (!php_var_unserialize(&data, p, max, var_hash TSRMLS_CC)) {
            var_push_dtor_no_addref(var_hash, &key);
            var_push_dtor_no_addref(var_hash, &data);
			return 0;
		}

		if (!objprops) {
			switch (Z_TYPE_P(key)) {
			case IS_LONG:
				if (zend_hash_index_find(ht, Z_LVAL_P(key), (void **)&old_data)==SUCCESS) {
					var_push_dtor(var_hash, old_data);
				}
				zend_hash_index_update(ht, Z_LVAL_P(key), &data, sizeof(data), NULL);
				break;
			case IS_STRING:
				if (zend_symtable_find(ht, Z_STRVAL_P(key), Z_STRLEN_P(key) + 1, (void **)&old_data)==SUCCESS) {
					var_push_dtor(var_hash, old_data);
				}
				zend_symtable_update(ht, Z_STRVAL_P(key), Z_STRLEN_P(key) + 1, &data, sizeof(data), NULL);
				break;
			}
		} else {
			/* object properties should include no integers */
			convert_to_string(key);
			if (zend_hash_find(ht, Z_STRVAL_P(key), Z_STRLEN_P(key) + 1, (void **)&old_data)==SUCCESS) {
				var_push_dtor(var_hash, old_data);
			}
			zend_hash_update(ht, Z_STRVAL_P(key), Z_STRLEN_P(key) + 1, &data,
					sizeof data, NULL);
		}
		var_push_dtor(var_hash, &data);
        var_push_dtor_no_addref(var_hash, &key);

		if (elements && *(*p-1) != ';' && *(*p-1) != '}') {
			(*p)--;
			return 0;
		}
	}

	return 1;
}

static inline int finish_nested_data(UNSERIALIZE_PARAMETER)
{
	if (*p >= max || **p != '}') {
		return 0;
	}

	(*p)++;
	return 1;
}

static inline int object_custom(UNSERIALIZE_PARAMETER, zend_class_entry *ce)
{
	long datalen;

	datalen = parse_iv2((*p) + 2, p);

	(*p) += 2;

	if (datalen < 0 || (max - (*p)) <= datalen) {
		zend_error(E_WARNING, "Insufficient data for unserializing - %ld required, %ld present", datalen, (long)(max - (*p)));
		return 0;
	}

	if (ce->unserialize == NULL) {
		zend_error(E_WARNING, "Class %s has no unserializer", ce->name);
		object_init_ex(*rval, ce);
	} else if (ce->unserialize(rval, ce, (const unsigned char*)*p, datalen, (zend_unserialize_data *)var_hash TSRMLS_CC) != SUCCESS) {
		return 0;
	}

	(*p) += datalen;

	return finish_nested_data(UNSERIALIZE_PASSTHRU);
}

static inline long object_common1(UNSERIALIZE_PARAMETER, zend_class_entry *ce)
{
	long elements;

	if( *p >= max - 2) {
		zend_error(E_WARNING, "Bad unserialize data");
		return -1;
	}

	elements = parse_iv2((*p) + 2, p);

	(*p) += 2;

	if (ce->serialize == NULL) {
		object_init_ex(*rval, ce);
	} else {
		/* If this class implements Serializable, it should not land here but in object_custom(). The passed string
		obviously doesn't descend from the regular serializer. */
		zend_error(E_WARNING, "Erroneous data format for unserializing '%s'", ce->name);
		return -1;
	}

	return elements;
}

#ifdef PHP_WIN32
# pragma optimize("", off)
#endif
static inline int object_common2(UNSERIALIZE_PARAMETER, long elements)
{
	if (Z_TYPE_PP(rval) != IS_OBJECT) {
		return 0;
	}

	if (!process_nested_data(UNSERIALIZE_PASSTHRU, Z_OBJPROP_PP(rval), elements, 1)) {
		/* We've got partially constructed object on our hands here. Wipe it. */
		if (Z_TYPE_PP(rval) == IS_OBJECT) {
			zend_hash_clean(Z_OBJPROP_PP(rval));
			zend_object_store_ctor_failed(*rval TSRMLS_CC);
		}
	    ZVAL_NULL(*rval);
		return 0;
	}

    if (Z_TYPE_PP(rval) != IS_OBJECT) {
        return 0;
    }

	if (Z_OBJCE_PP(rval) != PHP_IC_ENTRY &&
		zend_hash_exists(&Z_OBJCE_PP(rval)->function_table, "__wakeup", sizeof("__wakeup"))
	) {
		/* Store object for delayed __wakeup call. Remove references. */
		zval **slot = get_var_push_dtor_slot(var_hash);
		zval *zv = *rval;
		Z_ADDREF_P(zv);
		if (PZVAL_IS_REF(zv)) {
			SEPARATE_ZVAL(&zv);
		}
		*slot = WITH_WAKEUP_FLAG(zv);
	}

	return finish_nested_data(UNSERIALIZE_PASSTHRU);

}
#ifdef PHP_WIN32
# pragma optimize("", on)
#endif

PHPAPI int php_var_unserialize(UNSERIALIZE_PARAMETER)
{
	const unsigned char *cursor, *limit, *marker, *start;
	zval **rval_ref;

	limit = max;
	cursor = *p;

	if (YYCURSOR >= YYLIMIT) {
		return 0;
	}

	if (var_hash && cursor[0] != 'R') {
		var_push(var_hash, rval);
	}

	start = cursor;



/*!re2c

"R:" iv ";"		{
	long id;

 	*p = YYCURSOR;
	if (!var_hash) return 0;

	id = parse_iv(start + 2) - 1;
	if (id == -1 || var_access(var_hash, id, &rval_ref) != SUCCESS) {
		return 0;
	}

	if (*rval != NULL) {
		var_push_dtor_no_addref(var_hash, rval);
	}
	*rval = *rval_ref;
	Z_ADDREF_PP(rval);
	Z_SET_ISREF_PP(rval);

	return 1;
}

"r:" iv ";"		{
	long id;

 	*p = YYCURSOR;
	if (!var_hash) return 0;

	id = parse_iv(start + 2) - 1;
	if (id == -1 || var_access(var_hash, id, &rval_ref) != SUCCESS) {
		return 0;
	}

	if (*rval == *rval_ref) return 0;

	if (*rval != NULL) {
		var_push_dtor_no_addref(var_hash, rval);
	}
	*rval = *rval_ref;
	Z_ADDREF_PP(rval);
	Z_UNSET_ISREF_PP(rval);

	return 1;
}

"N;"	{
	*p = YYCURSOR;
	INIT_PZVAL(*rval);
	ZVAL_NULL(*rval);
	return 1;
}

"b:" [01] ";"	{
	*p = YYCURSOR;
	INIT_PZVAL(*rval);
	ZVAL_BOOL(*rval, parse_iv(start + 2));
	return 1;
}

"i:" iv ";"	{
#if SIZEOF_LONG == 4
	int digits = YYCURSOR - start - 3;

	if (start[2] == '-' || start[2] == '+') {
		digits--;
	}

	/* Use double for large long values that were serialized on a 64-bit system */
	if (digits >= MAX_LENGTH_OF_LONG - 1) {
		if (digits == MAX_LENGTH_OF_LONG - 1) {
			int cmp = strncmp(YYCURSOR - MAX_LENGTH_OF_LONG, long_min_digits, MAX_LENGTH_OF_LONG - 1);

			if (!(cmp < 0 || (cmp == 0 && start[2] == '-'))) {
				goto use_double;
			}
		} else {
			goto use_double;
		}
	}
#endif
	*p = YYCURSOR;
	INIT_PZVAL(*rval);
	ZVAL_LONG(*rval, parse_iv(start + 2));
	return 1;
}

"d:" ("NAN" | "-"? "INF") ";"	{
	*p = YYCURSOR;
	INIT_PZVAL(*rval);

	if (!strncmp(start + 2, "NAN", 3)) {
		ZVAL_DOUBLE(*rval, php_get_nan());
	} else if (!strncmp(start + 2, "INF", 3)) {
		ZVAL_DOUBLE(*rval, php_get_inf());
	} else if (!strncmp(start + 2, "-INF", 4)) {
		ZVAL_DOUBLE(*rval, -php_get_inf());
	}

	return 1;
}

"d:" (iv | nv | nvexp) ";"	{
#if SIZEOF_LONG == 4
use_double:
#endif
	*p = YYCURSOR;
	INIT_PZVAL(*rval);
	ZVAL_DOUBLE(*rval, zend_strtod((const char *)start + 2, NULL));
	return 1;
}

"s:" uiv ":" ["] 	{
	size_t len, maxlen;
	char *str;

	len = parse_uiv(start + 2);
	maxlen = max - YYCURSOR;
	if (maxlen < len) {
		*p = start + 2;
		return 0;
	}

	str = (char*)YYCURSOR;

	YYCURSOR += len;

	if (*(YYCURSOR) != '"') {
		*p = YYCURSOR;
		return 0;
	}

	if (*(YYCURSOR + 1) != ';') {
		*p = YYCURSOR + 1;
		return 0;
	}

	YYCURSOR += 2;
	*p = YYCURSOR;

	INIT_PZVAL(*rval);
	ZVAL_STRINGL(*rval, str, len, 1);
	return 1;
}

"S:" uiv ":" ["] 	{
	size_t len, maxlen;
	char *str;

	len = parse_uiv(start + 2);
	maxlen = max - YYCURSOR;
	if (maxlen < len) {
		*p = start + 2;
		return 0;
	}

	if ((str = unserialize_str(&YYCURSOR, &len, maxlen)) == NULL) {
		return 0;
	}

	if (*(YYCURSOR) != '"') {
		efree(str);
		*p = YYCURSOR;
		return 0;
	}

	if (*(YYCURSOR + 1) != ';') {
		efree(str);
		*p = YYCURSOR + 1;
		return 0;
	}

	YYCURSOR += 2;
	*p = YYCURSOR;

	INIT_PZVAL(*rval);
	ZVAL_STRINGL(*rval, str, len, 0);
	return 1;
}

"a:" uiv ":" "{" {
	long elements = parse_iv(start + 2);
	/* use iv() not uiv() in order to check data range */
	*p = YYCURSOR;
    if (!var_hash) return 0;

	if (elements < 0) {
		return 0;
	}

	INIT_PZVAL(*rval);

	array_init_size(*rval, elements);

	if (!process_nested_data(UNSERIALIZE_PASSTHRU, Z_ARRVAL_PP(rval), elements, 0)) {
		return 0;
	}

	return finish_nested_data(UNSERIALIZE_PASSTHRU);
}

"o:" iv ":" ["] {
	long elements;
    if (!var_hash) return 0;

	INIT_PZVAL(*rval);

	elements = object_common1(UNSERIALIZE_PASSTHRU, ZEND_STANDARD_CLASS_DEF_PTR);
	if (elements < 0) {
		return 0;
	}
	return object_common2(UNSERIALIZE_PASSTHRU, elements);
}

object ":" uiv ":" ["]	{
	size_t len, len2, len3, maxlen;
	long elements;
	char *class_name;
	zend_class_entry *ce;
	zend_class_entry **pce;
	int incomplete_class = 0;

	int custom_object = 0;

	zval *user_func;
	zval *retval_ptr;
	zval **args[1];
	zval *arg_func_name;

    if (!var_hash) return 0;
	if (*start == 'C') {
		custom_object = 1;
	}

	INIT_PZVAL(*rval);
	len2 = len = parse_uiv(start + 2);
	maxlen = max - YYCURSOR;
	if (maxlen < len || len == 0) {
		*p = start + 2;
		return 0;
	}

	class_name = (char*)YYCURSOR;

	YYCURSOR += len;

	if (*(YYCURSOR) != '"') {
		*p = YYCURSOR;
		return 0;
	}
	if (*(YYCURSOR+1) != ':') {
		*p = YYCURSOR+1;
		return 0;
	}

	len3 = strspn(class_name, "0123456789_abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ\177\200\201\202\203\204\205\206\207\210\211\212\213\214\215\216\217\220\221\222\223\224\225\226\227\230\231\232\233\234\235\236\237\240\241\242\243\244\245\246\247\250\251\252\253\254\255\256\257\260\261\262\263\264\265\266\267\270\271\272\273\274\275\276\277\300\301\302\303\304\305\306\307\310\311\312\313\314\315\316\317\320\321\322\323\324\325\326\327\330\331\332\333\334\335\336\337\340\341\342\343\344\345\346\347\350\351\352\353\354\355\356\357\360\361\362\363\364\365\366\367\370\371\372\373\374\375\376\377\\");
	if (len3 != len)
	{
		*p = YYCURSOR + len3 - len;
		return 0;
	}

	class_name = estrndup(class_name, len);

	do {
		/* Try to find class directly */
		BG(serialize_lock)++;
		if (zend_lookup_class(class_name, len2, &pce TSRMLS_CC) == SUCCESS) {
			BG(serialize_lock)--;
			if (EG(exception)) {
				efree(class_name);
				return 0;
			}
			ce = *pce;
			break;
		}
		BG(serialize_lock)--;

		if (EG(exception)) {
			efree(class_name);
			return 0;
		}

		/* Check for unserialize callback */
		if ((PG(unserialize_callback_func) == NULL) || (PG(unserialize_callback_func)[0] == '\0')) {
			incomplete_class = 1;
			ce = PHP_IC_ENTRY;
			break;
		}

		/* Call unserialize callback */
		MAKE_STD_ZVAL(user_func);
		ZVAL_STRING(user_func, PG(unserialize_callback_func), 1);
		args[0] = &arg_func_name;
		MAKE_STD_ZVAL(arg_func_name);
		ZVAL_STRING(arg_func_name, class_name, 1);
		BG(serialize_lock)++;
		if (call_user_function_ex(CG(function_table), NULL, user_func, &retval_ptr, 1, args, 0, NULL TSRMLS_CC) != SUCCESS) {
			BG(serialize_lock)--;
			if (EG(exception)) {
				efree(class_name);
				zval_ptr_dtor(&user_func);
				zval_ptr_dtor(&arg_func_name);
				return 0;
			}
			php_error_docref(NULL TSRMLS_CC, E_WARNING, "defined (%s) but not found", user_func->value.str.val);
			incomplete_class = 1;
			ce = PHP_IC_ENTRY;
			zval_ptr_dtor(&user_func);
			zval_ptr_dtor(&arg_func_name);
			break;
		}
		BG(serialize_lock)--;
		if (retval_ptr) {
			zval_ptr_dtor(&retval_ptr);
		}
		if (EG(exception)) {
			efree(class_name);
			zval_ptr_dtor(&user_func);
			zval_ptr_dtor(&arg_func_name);
			return 0;
		}

		/* The callback function may have defined the class */
		BG(serialize_lock)++;
		if (zend_lookup_class(class_name, len2, &pce TSRMLS_CC) == SUCCESS) {
			ce = *pce;
		} else {
			php_error_docref(NULL TSRMLS_CC, E_WARNING, "Function %s() hasn't defined the class it was called for", user_func->value.str.val);
			incomplete_class = 1;
			ce = PHP_IC_ENTRY;
		}
		BG(serialize_lock)--;

		zval_ptr_dtor(&user_func);
		zval_ptr_dtor(&arg_func_name);
		break;
	} while (1);

	*p = YYCURSOR;

	if (custom_object) {
		int ret;

		ret = object_custom(UNSERIALIZE_PASSTHRU, ce);

		if (ret && incomplete_class) {
			php_store_class_name(*rval, class_name, len2);
		}
		efree(class_name);
		return ret;
	}

	elements = object_common1(UNSERIALIZE_PASSTHRU, ce);

	if (elements < 0) {
	   efree(class_name);
	   return 0;
	}

	if (incomplete_class) {
		php_store_class_name(*rval, class_name, len2);
	}
	efree(class_name);

	return object_common2(UNSERIALIZE_PASSTHRU, elements);
}

"}" {
	/* this is the case where we have less data than planned */
	php_error_docref(NULL TSRMLS_CC, E_NOTICE, "Unexpected end of serialized data");
	return 0; /* not sure if it should be 0 or 1 here? */
}

any	{ return 0; }

*/

	return 0;
}
