/*
 * Gearman PHP Extension
 *
 * Copyright (C) 2008 James M. Luedke <contact@jamesluedke.com>,
 *                    Eric Day <eday@oddments.org>
 * All rights reserved.
 *
 * Use and distribution licensed under the PHP license.  See
 * the LICENSE file in this directory for full text.
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "php.h"
#include "php_ini.h"
#include "ext/standard/info.h"
#include "php_gearman.h"

#include <libgearman/gearman.h>

/* module version */
#define PHP_GEARMAN_VERSION "0.3"

/* XXX Compatibility Macros
 * If there is a better way to do this someone please let me know.
 * Also which is the prefered method now? ZVAL_ADDREF or Z_ADDREF_P ?
 * -jluedke */
#ifndef Z_ADDREF_P
# define Z_ADDREF_P ZVAL_ADDREF
#endif
#ifndef Z_DELREF_P
# define Z_DELREF_P ZVAL_DELREF
#endif

/* XXX I hate to do this but they changed PHP_ME_MAPPING between versions.
 * in order to make the module compile on versions < 5.2 this is required */
#if (PHP_MAJOR_VERSION == 5) && (PHP_MINOR_VERSION < 2)
#	define __PHP_ME_MAPPING(__name, __func, __arg, __flags) PHP_ME_MAPPING(__name, __func, __arg)
#else
#	define __PHP_ME_MAPPING(__name, __func, __arg, __flags) PHP_ME_MAPPING(__name, __func, __arg, __flags)
#endif

/*
 * Object types and structures.
 */

typedef enum {
	GEARMAN_OBJ_CREATED= (1 << 0)
} gearman_obj_flags_t;

typedef struct {
	zend_object std;
	gearman_obj_flags_t flags;
	gearman_st gearman;
} gearman_obj;

typedef enum {
	GEARMAN_CLIENT_OBJ_CREATED= (1 << 0)
} gearman_client_obj_flags_t;

typedef struct {
	zend_object std;
	gearman_return_t ret;
	gearman_client_obj_flags_t flags;
	gearman_client_st client;
	zval *zclient;
	/* used for keeping track of task interface callbacks */
	zval *zworkload_fn;
	zval *zcreated_fn;
	zval *zdata_fn;
	zval *zwarning_fn;
	zval *zstatus_fn;
	zval *zcomplete_fn;
	zval *zexception_fn;
	zval *zfail_fn;
} gearman_client_obj;

typedef struct _gearman_worker_cb gearman_worker_cb;
struct _gearman_worker_cb {
	zval *zname; /* name associated with callback */
	zval *zcall; /* name of callback */
	zval *zdata; /* data passed to callback via worker */
	gearman_worker_cb *next;
};

typedef enum {
	GEARMAN_WORKER_OBJ_CREATED= (1 << 0)
} gearman_worker_obj_flags_t;

typedef struct {
	zend_object std;
	gearman_return_t ret;
	gearman_worker_obj_flags_t flags;
	gearman_worker_st worker;
	gearman_worker_cb *cb_list;
} gearman_worker_obj;

typedef enum {
	GEARMAN_JOB_OBJ_CREATED= (1 << 0)
} gearman_job_obj_flags_t;

typedef struct {
	zend_object std;
	gearman_return_t ret;
	gearman_job_obj_flags_t flags;
	gearman_job_st *job;
	zval *gearman;
	zval *worker;
	zval *zworkload;
} gearman_job_obj;

typedef enum {
	GEARMAN_TASK_OBJ_CREATED= (1 << 0),
	GEARMAN_TASK_OBJ_DEAD=    (1 << 1)
} gearman_task_obj_flags_t;

typedef struct {
	zend_object std;
	gearman_return_t ret;
	zend_object_value value;
	gearman_task_obj_flags_t flags;
	gearman_task_st *task;
	zval *zgearman;
	zval *zclient;
	gearman_client_st *client;
	zval *zdata;
	zval *zworkload;
	int workload_len;
} gearman_task_obj;

/*
 * Object variables
 */

zend_class_entry *gearman_ce;
static zend_object_handlers gearman_obj_handlers;

zend_class_entry *gearman_client_ce;
static zend_object_handlers gearman_client_obj_handlers;

zend_class_entry *gearman_worker_ce;
static zend_object_handlers gearman_worker_obj_handlers;

zend_class_entry *gearman_job_ce;
static zend_object_handlers gearman_job_obj_handlers;

zend_class_entry *gearman_task_ce;
static zend_object_handlers gearman_task_obj_handlers;

/*
 * Helper macros.
 */

#define GEARMAN_ZPP(__return, __args, ...) { \
  if (zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "O" __args, \
                            __VA_ARGS__) == FAILURE) { \
    __return; \
  } \
  obj= zend_object_store_get_object(zobj TSRMLS_CC); \
}

#define GEARMAN_ZPMP(__return, __args, ...) { \
  if (zend_parse_method_parameters(ZEND_NUM_ARGS() TSRMLS_CC, getThis(), \
                                   "O" __args, __VA_ARGS__) == FAILURE) { \
    __return; \
  } \
  obj= zend_object_store_get_object(zobj TSRMLS_CC); \
}

#define GEARMAN_ZVAL_DONE(__zval) { \
  if ((__zval) != NULL) { \
    if (READY_TO_DESTROY(__zval)) { \
      zval_dtor(__zval); \
      FREE_ZVAL(__zval); \
    } \
    else \
      Z_DELREF_P(__zval); \
  } \
}

/* NOTE: It seems kinda wierd that GEARMAN_WORK_FAIL is a valid
 * return code, however it is required for a worker to pass status
 * back to the client about a failed job, other return codes can
 * be passed back but they will cause a docref Warning. Might
 * want to think of a better solution XXX */
#define PHP_GEARMAN_CLIENT_RET_OK(__ret) ((__ret) == GEARMAN_SUCCESS || \
                                          (__ret) == GEARMAN_PAUSE || \
                                          (__ret) == GEARMAN_IO_WAIT || \
                                          (__ret) == GEARMAN_WORK_STATUS || \
                                          (__ret) == GEARMAN_WORK_DATA || \
                                          (__ret) == GEARMAN_WORK_EXCEPTION || \
                                          (__ret) == GEARMAN_WORK_WARNING || \
                                          (__ret) == GEARMAN_WORK_FAIL)

/* Custom malloc and free calls to avoid excessive buffer copies. */
static void *_php_malloc(size_t size, void *arg) {
	uint8_t *ret;
	ret= emalloc(size+1);
	ret[size]= 0;
	return ret;
}

void _php_free(void *ptr, void *arg) {
	efree(ptr);
}

void _php_task_free(gearman_task_st *task, void *fn_arg) {
	gearman_task_obj *obj= (gearman_task_obj *)fn_arg;
	if (obj->flags & GEARMAN_TASK_OBJ_DEAD)
	{
	  GEARMAN_ZVAL_DONE(obj->zdata)
	  GEARMAN_ZVAL_DONE(obj->zworkload)
	  efree(obj);
	}
	/*
	else 
	  obj->flags&= ~GEARMAN_TASK_OBJ_CREATED;
	  */
}

/*
 * Functions from gearman.h
 */

/* {{{ proto string gearman_version()
   Returns libgearman version */
PHP_FUNCTION(gearman_version) {
	RETURN_STRING((char *)gearman_version(), 1);
}
/* }}} */

#if jluedke_0
PHP_FUNCTION(gearman_create) {
	/* TODO
	gearman= gearman_create(NULL);
	if (gearman == NULL)
	{
	  php_error_docref(NULL TSRMLS_CC, E_WARNING, "Memory allocation failure.");
	  RETURN_NULL();
	}

	ZEND_REGISTER_RESOURCE(return_value, gearman, le_gearman_st);
	*/
}
/* }}} */

PHP_FUNCTION(gearman_clone) {
	/* TODO
	if (zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "r", &zfrom) == FAILURE)
	  RETURN_NULL();

	ZEND_FETCH_RESOURCE(from, gearman_st *, &zfrom, -1, "gearman_st",
	                    le_gearman_st);

	gearman= gearman_clone(NULL, from);
	if (gearman == NULL)
	{
	  php_error_docref(NULL TSRMLS_CC, E_WARNING, "Memory allocation failure.");
	  RETURN_NULL();
	}

	ZEND_REGISTER_RESOURCE(return_value, gearman, le_gearman_st);
	*/
}
/* }}} */

PHP_FUNCTION(gearman_free) {
	/* TODO
	if (zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "r",
	                          &zgearman) == FAILURE)
	{
	  RETURN_FALSE;
	}

	ZEND_FETCH_RESOURCE(gearman, gearman_st *, &zgearman, -1, "gearman_st",
	                    le_gearman_st);

	gearman_free(gearman);
	zend_list_delete(Z_LVAL_P(zgearman));

	RETURN_TRUE;
	*/
}
/* }}} */

PHP_FUNCTION(gearman_error) {
	/* TODO
	if (zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "r",
	                          &zgearman) == FAILURE)
	{
	  RETURN_NULL();
	}

	ZEND_FETCH_RESOURCE(gearman, gearman_st *, &zgearman, -1, "gearman_st",
	                    le_gearman_st);

	RETURN_STRING((char *)gearman_error(gearman), 1);
	*/
}
/* }}} */

PHP_FUNCTION(gearman_errno) {
	/* TODO
	if (zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "r",
	                          &zgearman) == FAILURE)
	{
	  RETURN_NULL();
	}

	ZEND_FETCH_RESOURCE(gearman, gearman_st *, &zgearman, -1, "gearman_st",
	                    le_gearman_st);

	RETURN_LONG(gearman_errno(gearman));
	*/
}
/* }}} */

PHP_FUNCTION(gearman_set_options) {
	/* TODO
	if (zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "rll", &zgearman,
	                          &options, &data) == FAILURE)
	{
	  RETURN_FALSE;
	}

	ZEND_FETCH_RESOURCE(gearman, gearman_st *, &zgearman, -1, "gearman_st",
	                    le_gearman_st);

	gearman_set_options(gearman, options, data);

	RETURN_TRUE;
	*/
}
/* }}} */
#endif

/*
 * Functions from con.h
 */

/*
 * Functions from packet.h
 */

/*
 * Functions from task.h
 */

/* {{{ proto int gearman_task_return_code()
   get last gearman_return_t */
PHP_FUNCTION(gearman_task_return_code)
{
	zval *zobj;
	gearman_task_obj *obj;
	GEARMAN_ZPMP(RETURN_NULL(), "", &zobj, gearman_task_ce)
	RETURN_LONG(obj->ret);
}
/* }}} */

/* {{{ proto object gearman_task_create()
   Returns a task object */
PHP_FUNCTION(gearman_task_create) {
	zval *zobj;
	gearman_obj *obj;
	gearman_task_obj *task;

	GEARMAN_ZPMP(RETURN_NULL(), "", &zobj, gearman_ce)

	Z_TYPE_P(return_value)= IS_OBJECT;
	object_init_ex(return_value, gearman_task_ce);
	task= zend_object_store_get_object(return_value TSRMLS_CC);
	task->zgearman= zobj;
	Z_ADDREF_P(zobj);

	task->task= gearman_task_create(&(obj->gearman), task->task);
	if (task->task == NULL)
	{
		php_error_docref(NULL TSRMLS_CC, E_WARNING, 
		                 "Memory allocation failure.");
		zval_dtor(return_value);
		RETURN_FALSE;
	}

	task->flags|= GEARMAN_TASK_OBJ_CREATED;
}
/* }}} */

/* {{{ proto void gearman_task_free(object task)
   Frees a task object */
PHP_FUNCTION(gearman_task_free) {
	zval *zobj;
	gearman_task_obj *obj;

	GEARMAN_ZPP(RETURN_NULL(), "", &zobj, gearman_task_ce)

	if (obj->flags & GEARMAN_JOB_OBJ_CREATED)
	{
		gearman_task_free(obj->task);
		obj->flags&= ~GEARMAN_TASK_OBJ_CREATED;
	}
}
/* }}} */

/* {{{ proto string gearman_task_fn_arg(object task) 
   Set callback function argument for a task. */
PHP_FUNCTION(gearman_task_fn_arg) {
	zval *zobj;
	gearman_task_obj *obj;

	GEARMAN_ZPP(RETURN_NULL(), "O", &zobj, gearman_task_ce)
	RETURN_STRINGL((char *)obj->zdata->value.str.val, 
	               (long) obj->zdata->value.str.len, 1);
}
/* }}} */

/* {{{ proto string gearman_task_function(object task)
   Returns function name associated with a task. */
PHP_FUNCTION(gearman_task_function) {
	zval *zobj;
	gearman_task_obj *obj;
	GEARMAN_ZPMP(RETURN_NULL(), "", &zobj, gearman_task_ce)
	RETURN_STRING((char *)gearman_task_function(obj->task), 1);
}
/* }}} */

/* {{{ proto string gearman_task_uuid(object task)
   Returns unique identifier for a task. */
PHP_FUNCTION(gearman_task_uuid) {
	zval *zobj;
	gearman_task_obj *obj;
	GEARMAN_ZPMP(RETURN_NULL(), "", &zobj, gearman_task_ce)
	RETURN_STRING((char *)gearman_task_uuid(obj->task), 1);
}
/* }}} */

/* {{{ proto string gearman_task_job_handle(object task)
   Returns job handle for a task. */
PHP_FUNCTION(gearman_task_job_handle) {
	zval *zobj;
	gearman_task_obj *obj;
	GEARMAN_ZPMP(RETURN_NULL(), "", &zobj, gearman_task_ce)
	RETURN_STRING((char *)gearman_task_job_handle(obj->task), 1);
}
/* }}} */

/* {{{ proto bool gearman_task_is_known(object task)
   Get status on whether a task is known or not */
PHP_FUNCTION(gearman_task_is_known) {
	zval *zobj;
	gearman_task_obj *obj;
	GEARMAN_ZPMP(RETURN_NULL(), "", &zobj, gearman_task_ce)
	RETURN_BOOL(gearman_task_is_known(obj->task));
}
/* }}} */


/* {{{ proto bool gearman_task_is_running(object task)
   Get status on whether a task is running or not */
PHP_FUNCTION(gearman_task_is_running) {
	zval *zobj;
	gearman_task_obj *obj;
	GEARMAN_ZPMP(RETURN_NULL(), "", &zobj, gearman_task_ce)
	RETURN_BOOL(gearman_task_is_running(obj->task));
}
/* }}} */


/* {{{ proto int gearman_task_numerator(object task)
   Returns the numerator of percentage complete for a task. */
PHP_FUNCTION(gearman_task_numerator) {
	zval *zobj;
	gearman_task_obj *obj;
	GEARMAN_ZPMP(RETURN_NULL(), "", &zobj, gearman_task_ce)
	RETURN_LONG(gearman_task_numerator(obj->task));
}
/* }}} */


/* {{{ proto int gearman_task_denominator(object task)
   Returns the denominator of percentage complete for a task. */
PHP_FUNCTION(gearman_task_denominator) {
	zval *zobj;
	gearman_task_obj *obj;
	GEARMAN_ZPMP(RETURN_NULL(), "", &zobj, gearman_task_ce)
	RETURN_LONG(gearman_task_denominator(obj->task));
}
/* }}} */


/* {{{ proto string gearman_task_data(object task)
   Get data being returned for a task. */
PHP_FUNCTION(gearman_task_data) {
	zval *zobj;
	gearman_task_obj *obj;
	const uint8_t *data;
	size_t data_len;

	GEARMAN_ZPMP(RETURN_NULL(), "", &zobj, gearman_task_ce)

	data= gearman_task_data(obj->task);
	data_len= gearman_task_data_size(obj->task);
	
	RETURN_STRINGL((char *)data, (long) data_len, 1);
}
/* }}} */


/* {{{ proto int gearman_task_data_size(object task)
   Get data size being returned for a task. */
PHP_FUNCTION(gearman_task_data_size) {
	zval *zobj;
	gearman_task_obj *obj;
	GEARMAN_ZPMP(RETURN_NULL(), "", &zobj, gearman_task_ce)
	RETURN_LONG(gearman_task_data_size(obj->task));
}
/* }}} */


/* {{{ proto array gearman_task_take_data(object task)
   NOT-TESTED Take allocated data from task. Caller is responsible for free()ing the memory. */
PHP_FUNCTION(gearman_task_take_data) {
	zval *zobj;
	gearman_task_obj *obj;
	const uint8_t *data;
	size_t data_len;

	GEARMAN_ZPMP(RETURN_NULL(), "", &zobj, gearman_task_ce)

	/* XXX verify that i am doing this correctly */
	data= gearman_task_take_data(obj->task, &data_len);

	array_init(return_value);
	add_next_index_long(return_value, (long)data_len);
	add_next_index_stringl(return_value, (char *)data, (long)data_len, 1);
}
/* }}} */

/* {{{ proto int gearman_task_send_data(object task, string data)
   NOT-TESTED Send packet data for a task. */
PHP_FUNCTION(gearman_task_send_data) {
	zval *zobj;
	gearman_task_obj *obj;
	const uint8_t *data;
	size_t data_len;

	GEARMAN_ZPMP(RETURN_NULL(), "s", &zobj, gearman_task_ce,
	             data, &data_len)

	/* XXX verify that i am doing this correctly */
	data_len= gearman_task_send_data(obj->task, data, data_len, &obj->ret);
	if (obj->ret != GEARMAN_SUCCESS)
	{
		php_error_docref(NULL TSRMLS_CC, E_WARNING, 
		                 gearman_client_error(obj->client));
		RETURN_FALSE;
	}

	RETURN_LONG(data_len);
}
/* }}} */


/* {{{ proto array gearman_task_recv_data(object task, long buffer_size)
   NOT-TESTED Read work or result data into a buffer for a task. */
PHP_FUNCTION(gearman_task_recv_data) {
	zval *zobj;
	gearman_task_obj *obj;
	char *data_buffer;
	long data_buffer_size;
	size_t data_len;

	GEARMAN_ZPMP(RETURN_NULL(), "l", &zobj, gearman_job_ce, &data_buffer_size)

	data_buffer= (char *) emalloc(data_buffer_size);

	data_len= gearman_task_recv_data(obj->task, data_buffer, data_buffer_size, 
	                                 &obj->ret);
	if (obj->ret != GEARMAN_SUCCESS) {
		php_error_docref(NULL TSRMLS_CC, E_WARNING, 
		                 gearman_client_error(obj->client));
		RETURN_FALSE;
	}

	array_init(return_value);
	add_next_index_long(return_value, (long)data_len);
	add_next_index_stringl(return_value, (char *)data_buffer, 
	                      (long)data_len, 0);
}
/* }}} */

/*
 * Functions from job.h
 */

/* {{{ proto int gearman_job_return_code()
   get last gearman_return_t */
PHP_FUNCTION(gearman_job_return_code)
{
	zval *zobj;
	gearman_job_obj *obj;
	GEARMAN_ZPMP(RETURN_NULL(), "", &zobj, gearman_job_ce)
	RETURN_LONG(obj->ret);
}
/* }}} */

/* {{{ proto void gearman_job_create(object gearman)
   NOT-SUPPORTED Initialize a job structure. */
PHP_FUNCTION(gearman_job_create) {
	zval *zobj;
	gearman_obj *obj;
	gearman_job_obj *job;

	GEARMAN_ZPMP(RETURN_NULL(), "", &zobj, gearman_ce)

	Z_TYPE_P(return_value)= IS_OBJECT;
	object_init_ex(return_value, gearman_job_ce);
	job= zend_object_store_get_object(return_value TSRMLS_CC);
	job->gearman= zobj;
	Z_ADDREF_P(zobj);

	job->job= gearman_job_create(&(obj->gearman), NULL);
	if (job->job == NULL) {
		php_error_docref(NULL TSRMLS_CC, E_WARNING, 
		                 "Memory allocation failure.");
		zval_dtor(return_value);
		RETURN_FALSE;
	}

	job->flags|= GEARMAN_JOB_OBJ_CREATED;
}
/* }}} */

/* {{{ proto void gearman_job_free(object job)
   Free a job object */
PHP_FUNCTION(gearman_job_free) {
	zval *zobj;
	gearman_job_obj *obj;

	GEARMAN_ZPP(RETURN_NULL(), "O", &zobj, gearman_job_ce)

	if (obj->flags & GEARMAN_JOB_OBJ_CREATED) {
		gearman_job_free(obj->job);
		obj->flags&= ~GEARMAN_JOB_OBJ_CREATED;
	}
}
/* }}} */

/* {{{ proto bool gearman_job_data(object job, string data)
   Send data for a running job. */
PHP_FUNCTION(gearman_job_data) {
	zval *zobj;
	gearman_job_obj *obj;
	char *data;
	int data_len;

	GEARMAN_ZPMP(RETURN_NULL(), "s", &zobj, gearman_job_ce, &data, &data_len)

	obj->ret= gearman_job_data(obj->job, data, data_len);
	if (obj->ret != GEARMAN_SUCCESS && obj->ret != GEARMAN_IO_WAIT) {
		php_error_docref(NULL TSRMLS_CC, E_WARNING, 
		                 gearman_error(obj->job->gearman));
		RETURN_FALSE;
	}

	RETURN_TRUE;
}
/* }}} */

/* {{{ proto bool gearman_job_warning(object job, string warning)
   Send warning for a running job. */
PHP_FUNCTION(gearman_job_warning) {
	zval *zobj;
	gearman_job_obj *obj;
	char *warning= NULL;
	int   warning_len= 0;

	GEARMAN_ZPMP(RETURN_NULL(), "s", &zobj, gearman_job_ce, 
	             &warning, &warning_len)

	obj->ret= gearman_job_warning(obj->job, (void *) warning, 
	                             (size_t) warning_len);
	if (obj->ret != GEARMAN_SUCCESS && obj->ret != GEARMAN_IO_WAIT) {
		php_error_docref(NULL TSRMLS_CC, E_WARNING, 
		                 gearman_error(obj->job->gearman));
		RETURN_FALSE;
	}

	RETURN_TRUE;
}
/* }}} */

/* {{{ proto bool gearman_job_status(object job, int numerator, int denominator)
   Send status information for a running job. */
PHP_FUNCTION(gearman_job_status) {
	zval *zobj;
	gearman_job_obj *obj;
	long numerator;
	long denominator;

	GEARMAN_ZPMP(RETURN_NULL(), "ll", &zobj, gearman_job_ce, &numerator,
	             &denominator)

	obj->ret= gearman_job_status(obj->job, (uint32_t)numerator, 
	                       (uint32_t)denominator);
	if (obj->ret != GEARMAN_SUCCESS && obj->ret != GEARMAN_IO_WAIT) {
		php_error_docref(NULL TSRMLS_CC, E_WARNING, 
		                 gearman_error(obj->job->gearman));
		RETURN_FALSE;
	}

	RETURN_TRUE;
}
/* }}} */

/* {{{ proto bool gearman_job_complete(object job, string result)
   Send result and complete status for a job. */
PHP_FUNCTION(gearman_job_complete) {
	zval *zobj;
	gearman_job_obj *obj;
	char *result;
	int result_len;

	GEARMAN_ZPMP(RETURN_NULL(), "s", &zobj, gearman_job_ce, 
	             &result, &result_len)

	obj->ret= gearman_job_complete(obj->job, result, result_len);
	if (obj->ret != GEARMAN_SUCCESS && obj->ret != GEARMAN_IO_WAIT) {
		php_error_docref(NULL TSRMLS_CC, E_WARNING, 
		                 gearman_error(obj->job->gearman));
		RETURN_FALSE;
	}

	RETURN_TRUE;
}
/* }}} */

/* {{{ proto bool gearman_job_exception(object job, string exception)
   Send exception for a running job. */
PHP_FUNCTION(gearman_job_exception) {
	zval *zobj;
	gearman_job_obj *obj;
	char *exception;
	int exception_len;

	GEARMAN_ZPMP(RETURN_NULL(), "s", &zobj, gearman_job_ce, 
	             &exception, &exception_len)

	obj->ret= gearman_job_exception(obj->job, exception, exception_len);
	if (obj->ret != GEARMAN_SUCCESS && obj->ret != GEARMAN_IO_WAIT) {
		php_error_docref(NULL TSRMLS_CC, E_WARNING, 
		                 gearman_error(obj->job->gearman));
		RETURN_FALSE;
	}

	RETURN_TRUE;
}
/* }}} */

/* {{{ proto bool gearman_job_fail(object job)
   Send fail status for a job. */
PHP_FUNCTION(gearman_job_fail) {
	zval *zobj;
	gearman_job_obj *obj;

	GEARMAN_ZPMP(RETURN_NULL(), "", &zobj, gearman_job_ce)

	obj->ret= gearman_job_fail(obj->job);
	if (obj->ret != GEARMAN_SUCCESS && obj->ret != GEARMAN_IO_WAIT) {
		php_error_docref(NULL TSRMLS_CC, E_WARNING, 
		                 gearman_error(obj->job->gearman));
		RETURN_FALSE;
	}

	RETURN_TRUE;
}
/* }}} */

/* {{{ proto string gearman_job_handle(object job)
   Return job handle. */
PHP_FUNCTION(gearman_job_handle) {
	zval *zobj;
	gearman_job_obj *obj;

	GEARMAN_ZPMP(RETURN_NULL(), "", &zobj, gearman_job_ce)

	RETURN_STRING((char *)gearman_job_handle(obj->job), 1)
}
/* }}} */

/* {{{ proto string gearman_job_unique(object job)
   Get the unique ID associated with a job. */
PHP_FUNCTION(gearman_job_unique) {
	zval *zobj;
	gearman_job_obj *obj;

	GEARMAN_ZPMP(RETURN_NULL(), "", &zobj, gearman_job_ce)

	RETURN_STRING((char *)gearman_job_unique(obj->job), 1)
}
/* }}} */

/* {{{ proto string gearman_job_function_name(object job)
   Return the function name associated with a job. */
PHP_FUNCTION(gearman_job_function_name) {
	zval *zobj;
	gearman_job_obj *obj;

	GEARMAN_ZPMP(RETURN_NULL(), "", &zobj, gearman_job_ce)

	RETURN_STRING((char *)gearman_job_function_name(obj->job), 1)
}
/* }}} */

/* {{{ proto string gearman_job_workload(object job)
   Returns the workload for a job. */
PHP_FUNCTION(gearman_job_workload) {
	zval *zobj;
	gearman_job_obj *obj;
	const uint8_t *workload;
	size_t workload_len;

	GEARMAN_ZPMP(RETURN_NULL(), "", &zobj, gearman_job_ce)

	workload= gearman_job_workload(obj->job);
	workload_len= gearman_job_workload_size(obj->job);

	RETURN_STRINGL((char *)workload, (long) workload_len, 1);
}
/* }}} */

/* {{{ proto int gearman_job_workload_size(object job)
   Returns size of the workload for a job. */
PHP_FUNCTION(gearman_job_workload_size) {
	zval *zobj;
	gearman_job_obj *obj;
	size_t workload_len;

	GEARMAN_ZPMP(RETURN_NULL(), "", &zobj, gearman_job_ce)

	workload_len= gearman_job_workload_size(obj->job);
	
	RETURN_LONG((long) workload_len);
}
/* }}} */

/* {{{ proto bool gearman_job_set_return(int gearman_return_t)
   This function will set a return value of a job */
PHP_FUNCTION(gearman_job_set_return) {
	zval *zobj;
	gearman_job_obj *obj;
	gearman_return_t ret;

	GEARMAN_ZPMP(RETURN_NULL(), "l", &zobj, gearman_job_ce, &ret)
	
	/* make sure its a valid gearman_return_t */
	if (ret < GEARMAN_SUCCESS || ret > GEARMAN_MAX_RETURN) {
		php_error_docref(NULL TSRMLS_CC, E_WARNING, 
		                 "Invalid gearman_return_t: %d", ret);
		RETURN_FALSE;
	}

	obj->ret= ret;
	RETURN_TRUE;
}

/*
 * Functions from client.h
 */

/* {{{ proto int gearman_client_return_code()
   get last gearman_return_t */
PHP_FUNCTION(gearman_client_return_code)
{
	zval *zobj;
	gearman_client_obj *obj;
	GEARMAN_ZPMP(RETURN_NULL(), "", &zobj, gearman_client_ce)
	RETURN_LONG(obj->ret);
}
/* }}} */

/* {{{ proto object gearman_client_create()
   Initialize a client object.  */
PHP_FUNCTION(gearman_client_create) {
	gearman_client_obj *client;

	Z_TYPE_P(return_value)= IS_OBJECT;
	object_init_ex(return_value, gearman_client_ce);
	client= zend_object_store_get_object(return_value TSRMLS_CC);

	if (gearman_client_create(&(client->client)) == NULL) {
		php_error_docref(NULL TSRMLS_CC, E_WARNING, 
		                 "Memory allocation failure.");
		zval_dtor(return_value);
		RETURN_FALSE;
	}

	client->flags|= GEARMAN_CLIENT_OBJ_CREATED;
	gearman_client_set_workload_malloc(&(client->client), _php_malloc, NULL);
	gearman_client_set_workload_free(&(client->client), _php_free, NULL);
	gearman_client_set_task_fn_arg_free(&(client->client), _php_task_free);
	gearman_client_set_data(&(client->client), client);
}
/* }}} */

/* {{{ proto object gearman_client_clone(object client)
   Clone a client object */
PHP_FUNCTION(gearman_client_clone) {
	zval *zobj;
	gearman_client_obj *obj;
	gearman_client_obj *new;

	GEARMAN_ZPMP(RETURN_NULL(), "", &zobj, gearman_client_ce)

	Z_TYPE_P(return_value)= IS_OBJECT;
	object_init_ex(return_value, gearman_client_ce);
	new= zend_object_store_get_object(return_value TSRMLS_CC);

	if (gearman_client_clone(&(new->client), &(obj->client)) == NULL) {
		php_error_docref(NULL TSRMLS_CC, E_WARNING, 
		                 "Memory allocation failure.");
		zval_dtor(return_value);
		RETURN_FALSE;
	}

	new->flags|= GEARMAN_CLIENT_OBJ_CREATED;
}
/* }}} */

/* {{{ proto void gearman_client_free(object client)
   Free resources used by a client object */
PHP_FUNCTION(gearman_client_free) {
	zval *zobj;
	gearman_client_obj *obj;

	GEARMAN_ZPP(RETURN_NULL(), "", &zobj, gearman_client_ce)
	obj= zend_object_store_get_object(zobj TSRMLS_CC);

	if (obj->flags & GEARMAN_CLIENT_OBJ_CREATED) {
		gearman_client_free(&(obj->client));
		obj->flags&= ~GEARMAN_CLIENT_OBJ_CREATED;
	}
}
/* }}} */

/* {{{ proto string gearman_client_error(object client)
   Return an error string for the last error encountered. */
PHP_FUNCTION(gearman_client_error) {
	zval *zobj;
	gearman_client_obj *obj;

	GEARMAN_ZPMP(RETURN_NULL(), "", &zobj, gearman_client_ce)

	RETURN_STRING((char *)gearman_client_error(&(obj->client)), 1)
}
/* }}} */

/* {{{ proto int gearman_client_errno(object client)
   Value of errno in the case of a GEARMAN_ERRNO return value. */
PHP_FUNCTION(gearman_client_errno) {
	zval *zobj;
	gearman_client_obj *obj;

	GEARMAN_ZPMP(RETURN_NULL(), "", &zobj, gearman_client_ce)

	RETURN_LONG(gearman_client_errno(&(obj->client)))
}
/* }}} */

/* {{{ proto void gearman_client_set_options(object client, int option, int data)
   Set options for a client object */
PHP_FUNCTION(gearman_client_set_options) {
	zval *zobj;
	gearman_client_obj *obj;
	long options;
	long data;

	GEARMAN_ZPMP(RETURN_NULL(), "ll", &zobj, gearman_client_ce, 
	             &options, &data)

	gearman_client_set_options(&(obj->client), options, data);
	RETURN_TRUE;
}
/* }}} */

/* {{{ proto bool gearman_client_add_server(object client [, string host [, int port]])
   Add a job server to a client. This goes into a list of servers than can be used to run tasks. No socket I/O happens here, it is just added to a list. */
PHP_FUNCTION(gearman_client_add_server) {
	zval *zobj;
	gearman_client_obj *obj;
	char *host= NULL;
	int host_len= 0;
	long port= 0;

	GEARMAN_ZPMP(RETURN_NULL(), "|sl", &zobj, gearman_client_ce, 
	             &host, &host_len, &port)

	obj->ret= gearman_client_add_server(&(obj->client), host, port);
	if (obj->ret != GEARMAN_SUCCESS) {
		php_error_docref(NULL TSRMLS_CC, E_WARNING,
		                 gearman_client_error(&(obj->client)));
		RETURN_FALSE;
	}

	RETURN_TRUE;
}
/* }}} */

/* TODO
void *_php_client_do()
{
}
*/

/* {{{ proto string gearman_client_do(object client, string function, string workload [, string unique ])
   Run a single task and return an allocated result. */
PHP_FUNCTION(gearman_client_do) {
	zval *zobj;
	gearman_client_obj *obj;
	char *function_name;
	int function_name_len;
	char *workload;
	int workload_len;
	char *unique= NULL;
	int unique_len= 0;
	void *result;
	size_t result_size= 0;

	GEARMAN_ZPMP(RETURN_NULL(), "ss|s", &zobj, gearman_client_ce, 
	            &function_name, &function_name_len, 
	            &workload, &workload_len, &unique, &unique_len)

	result= (char *)gearman_client_do(&(obj->client), function_name, unique,
	                                  workload, (size_t)workload_len,
	                                  &result_size, &(obj)->ret);
	if (! PHP_GEARMAN_CLIENT_RET_OK(obj->ret)) {
		php_error_docref(NULL TSRMLS_CC, E_WARNING,
		                 gearman_client_error(&(obj->client)));
		RETURN_EMPTY_STRING();
	}

	/* NULL results are valid */
	if (! result) {
		RETURN_EMPTY_STRING();
	}

	RETURN_STRINGL((char *)result, (long) result_size, 0);
}
/* }}} */

/* {{{ proto string gearman_client_do_high(object client, string function, string workload [, string unique ])
   Run a high priority task and return an allocated result. */
PHP_FUNCTION(gearman_client_do_high) {
	zval *zobj;
	gearman_client_obj *obj;
	char *function_name;
	int function_name_len;
	char *workload;
	int workload_len;
	char *unique= NULL;
	int unique_len= 0;
	void *result;
	size_t result_size= 0;

	GEARMAN_ZPMP(RETURN_NULL(), "ss|s", &zobj, gearman_client_ce, 
	            &function_name, &function_name_len, 
	            &workload, &workload_len, &unique, &unique_len)

	result= (char *)gearman_client_do_high(&(obj->client), function_name, 
	                                       unique, workload, 
										   (size_t)workload_len,
	                                       &result_size, &(obj)->ret);
	if (! PHP_GEARMAN_CLIENT_RET_OK(obj->ret)) {
		php_error_docref(NULL TSRMLS_CC, E_WARNING,
		                 gearman_client_error(&(obj->client)));
		RETURN_EMPTY_STRING();
	}

	/* NULL results are valid */
	if (! result) {
		RETURN_EMPTY_STRING();
	}

	RETURN_STRINGL((char *)result, (long) result_size, 0);
}
/* }}} */

/* {{{ proto array gearman_client_do_low(object client, string function, string workload [, string unique ])
   Run a low priority task and return an allocated result. */
PHP_FUNCTION(gearman_client_do_low) {
	zval *zobj;
	gearman_client_obj *obj;
	char *function_name;
	int function_name_len;
	char *workload;
	int workload_len;
	char *unique= NULL;
	int unique_len= 0;
	void *result;
	size_t result_size= 0;

	GEARMAN_ZPMP(RETURN_NULL(), "ss|s", &zobj, gearman_client_ce, 
	            &function_name, &function_name_len, 
	            &workload, &workload_len, &unique, &unique_len)

	result= (char *)gearman_client_do_low(&(obj->client), function_name, 
	                                      unique, workload, 
										  (size_t)workload_len,
	                                      &result_size, &obj->ret);
	if (! PHP_GEARMAN_CLIENT_RET_OK(obj->ret)) {
		php_error_docref(NULL TSRMLS_CC, E_WARNING,
		                 gearman_client_error(&(obj->client)));
		RETURN_EMPTY_STRING();
	}

	/* NULL results are valid */
	if (! result) {
		RETURN_EMPTY_STRING();
	}

	RETURN_STRINGL((char *)result, (long) result_size, 0);
}
/* }}} */


/* {{{ proto string gearman_client_do_job_handle(object client)
   Get the job handle for the running task. This should be used between repeated gearman_client_do() and gearman_client_do_high() calls to get information. */
PHP_FUNCTION(gearman_client_do_job_handle) {
	zval *zobj;
	gearman_client_obj *obj;

	GEARMAN_ZPMP(RETURN_NULL(), "", &zobj, gearman_client_ce)

	RETURN_STRING((char *)gearman_client_do_job_handle(&(obj->client)), 1)
}
/* }}} */

/* {{{ proto array gearman_client_do_status(object client)
   Get the status for the running task. This should be used between repeated gearman_client_do() and gearman_client_do_high() calls to get information. */
PHP_FUNCTION(gearman_client_do_status) {
	zval *zobj;
	gearman_client_obj *obj;
	uint32_t numerator;
	uint32_t denominator;

	GEARMAN_ZPMP(RETURN_NULL(), "", &zobj, gearman_client_ce)

	gearman_client_do_status(&(obj->client), &numerator, &denominator);
	
	array_init(return_value);
	add_next_index_long(return_value, (long) numerator);
	add_next_index_long(return_value, (long) denominator);
}
/* }}} */

/* {{{ proto string gearman_client_do_background(object client, string function, string workload [, string unique ])
   Run a task in the background. */
PHP_FUNCTION(gearman_client_do_background) {
	zval *zobj;
	gearman_client_obj *obj;
	char *function_name;
	int function_name_len;
	char *workload;
	int workload_len;
	char *unique= NULL;
	int unique_len= 0;
	char *job_handle;

	GEARMAN_ZPMP(RETURN_NULL(), "ss|s", &zobj, gearman_client_ce, 
	            &function_name, &function_name_len, 
	            &workload, &workload_len, &unique, &unique_len)

	job_handle= emalloc(GEARMAN_JOB_HANDLE_SIZE);

	obj->ret= gearman_client_do_background(&(obj->client), 
	                                 (char *)function_name, 
	                                 (char *)unique, (void *)workload, 
	                                 (size_t)workload_len, job_handle);
	if (! PHP_GEARMAN_CLIENT_RET_OK(obj->ret)) {
		php_error_docref(NULL TSRMLS_CC, E_WARNING,
		                 gearman_client_error(&(obj->client)));
		efree(job_handle);
		RETURN_EMPTY_STRING();
	}

	if (! job_handle) {
		efree(job_handle);
		RETURN_EMPTY_STRING();
	}

	RETURN_STRING(job_handle, 0);
}
/* }}} */

/* {{{ proto string gearman_client_do_high_background(object client, string function, string workload [, string unique ])
   Run a high priority task in the background. */
PHP_FUNCTION(gearman_client_do_high_background) {
	zval *zobj;
	gearman_client_obj *obj;
	char *function_name;
	int function_name_len;
	char *workload;
	int workload_len;
	char *unique= NULL;
	int unique_len= 0;
	char *job_handle;

	GEARMAN_ZPMP(RETURN_NULL(), "ss|s", &zobj, gearman_client_ce, 
	            &function_name, &function_name_len, 
	            &workload, &workload_len, &unique, &unique_len)

	job_handle= emalloc(GEARMAN_JOB_HANDLE_SIZE);

	obj->ret= gearman_client_do_high_background(&(obj->client), 
	                                 (char *)function_name, 
	                                 (char *)unique, (void *)workload, 
	                                 (size_t)workload_len, job_handle);
	if (! PHP_GEARMAN_CLIENT_RET_OK(obj->ret)) {
		php_error_docref(NULL TSRMLS_CC, E_WARNING,
		                 gearman_client_error(&(obj->client)));
		efree(job_handle);
		RETURN_EMPTY_STRING();
	}

	if (! job_handle) {
		efree(job_handle);
		RETURN_EMPTY_STRING();
	}

	RETURN_STRING(job_handle, 0);
}
/* }}} */

/* {{{ proto string gearman_client_do_low_background(object client, string function, string workload [, string unique ])
   Run a low priority task in the background. */
PHP_FUNCTION(gearman_client_do_low_background) {
	zval *zobj;
	gearman_client_obj *obj;
	char *function_name;
	int function_name_len;
	char *workload;
	int workload_len;
	char *unique= NULL;
	int unique_len= 0;
	char *job_handle;

	GEARMAN_ZPMP(RETURN_NULL(), "ss|s", &zobj, gearman_client_ce, 
	            &function_name, &function_name_len, 
	            &workload, &workload_len, &unique, &unique_len)

	job_handle= emalloc(GEARMAN_JOB_HANDLE_SIZE);
	obj->ret= gearman_client_do_low_background(&(obj->client), 
	                                     (char *)function_name, 
	                                     (char *)unique, (void *)workload, 
	                                     (size_t)workload_len, job_handle);
	if (! PHP_GEARMAN_CLIENT_RET_OK(obj->ret)) {
		php_error_docref(NULL TSRMLS_CC, E_WARNING,
		                 gearman_client_error(&(obj->client)));
		efree(job_handle);
		RETURN_EMPTY_STRING();
	}

	if (! job_handle) {
		efree(job_handle);
		RETURN_EMPTY_STRING();
	}

	RETURN_STRING(job_handle, 0);
}
/* }}} */

/* {{{ proto array gearman_client_job_status(object client, string job_handle)
   Get the status for a backgound job. */
PHP_FUNCTION(gearman_client_job_status) {
	zval *zobj;
	gearman_client_obj *obj;
	char *job_handle;
	int job_handle_len;
	bool is_known;
	bool is_running;
	uint32_t numerator;
	uint32_t denominator;

	GEARMAN_ZPMP(RETURN_NULL(), "s", &zobj, gearman_client_ce,
	            &job_handle, &job_handle_len)

	obj->ret= gearman_client_job_status(&(obj->client), job_handle, 
	                          &is_known, &is_running,
	                          &numerator, &denominator);
	if (obj->ret != GEARMAN_SUCCESS && obj->ret != GEARMAN_IO_WAIT) {
		php_error_docref(NULL TSRMLS_CC, E_WARNING,
		                 gearman_client_error(&(obj->client)));
	}

	array_init(return_value);
	add_next_index_bool(return_value, is_known);
	add_next_index_bool(return_value, is_running);
	add_next_index_long(return_value, (long) numerator);
	add_next_index_long(return_value, (long) denominator);
}
/* }}} */

/* {{{ proto bool gearman_client_echo(object client, string workload)
   Send data to all job servers to see if they echo it back. */
PHP_FUNCTION(gearman_client_echo) {
	zval *zobj;
	gearman_client_obj *obj;
	char *workload;
	int workload_len;

	GEARMAN_ZPMP(RETURN_NULL(), "s", &zobj, gearman_client_ce, 
	             &workload, &workload_len)

	obj->ret= gearman_client_echo(&(obj->client), workload, 
	                             (size_t)workload_len);
	if (obj->ret != GEARMAN_SUCCESS && obj->ret != GEARMAN_IO_WAIT) {
		php_error_docref(NULL TSRMLS_CC, E_WARNING,
		                 gearman_client_error(&(obj->client)));
		RETURN_FALSE;
	}
	RETURN_TRUE;
}
/* }}} */

/* TODO: clean up the add_task interface, to much copy paste */

/* {{{ proto object gearman_client_add_task(object client, string function, zval workload [, string unique ])
   Add a task to be run in parallel. */
PHP_FUNCTION(gearman_client_add_task) {
	zval *zobj;
	zval *zworkload;
	zval *zdata= NULL;
	gearman_client_obj *obj;
	gearman_task_obj *task;

	char *unique= NULL;
	char *function_name;
	int unique_len= 0;
	int function_name_len= 0;

	GEARMAN_ZPMP(RETURN_NULL(), "sz|zs", &zobj, gearman_client_ce, 
	             &function_name, &function_name_len, &zworkload,
	             &zdata, &unique, &unique_len)

	/* get a task object, and prepare it for return */
	Z_TYPE_P(return_value)= IS_OBJECT;
	object_init_ex(return_value, gearman_task_ce);
	task= zend_object_store_get_object(return_value TSRMLS_CC);

	if (zdata) {
		/* add zdata tothe task object and pass the task object via fn_arg
		 * task->client= zobj; */
		task->zdata= zdata;
		Z_ADDREF_P(zdata);
	}

	/* store our workload and add ref so it wont go away on us */
	task->zworkload= zworkload;
	Z_ADDREF_P(zworkload);

	/* need to store a ref to the client for later access to cb's */
	task->zclient= zobj;
	Z_ADDREF_P(zobj);
	task->client= &obj->client;

	/* add the task */
	task->task= gearman_client_add_task(&(obj->client), task->task, 
	                                    (void *)task, function_name, 
	                                    unique, Z_STRVAL_P(zworkload), 
	                                    (size_t)Z_STRLEN_P(zworkload), 
									    &obj->ret);
	if (obj->ret != GEARMAN_SUCCESS) {
		php_error_docref(NULL TSRMLS_CC, E_WARNING,
		                 gearman_client_error(&(obj->client)));
		RETURN_FALSE;
	}
	task->flags |= GEARMAN_TASK_OBJ_CREATED;
}
/* }}} */

/* {{{ proto object gearman_client_add_task_high(object client, string function, zval workload [, string unique ])
   Add a high priority task to be run in parallel. */
PHP_FUNCTION(gearman_client_add_task_high) {
	zval *zobj;
	zval *zworkload;
	zval *zdata= NULL;
	gearman_client_obj *obj;
	gearman_task_obj *task;

	char *unique= NULL;
	char *function_name;
	int unique_len= 0;
	int function_name_len= 0;

	GEARMAN_ZPMP(RETURN_NULL(), "sz|zs", &zobj, gearman_client_ce, 
	             &function_name, &function_name_len, &zworkload,
	             &zdata, &unique, &unique_len)

	/* get a task object, and prepare it for return */
	Z_TYPE_P(return_value)= IS_OBJECT;
	object_init_ex(return_value, gearman_task_ce);
	task= zend_object_store_get_object(return_value TSRMLS_CC);

	if (zdata) {
		/* add zdata tothe task object and pass the task object via fn_arg
		 * task->client= zobj; */
		task->zdata= zdata;
		Z_ADDREF_P(zdata);
	}

	/* store our workload and add ref so it wont go away on us */
	task->zworkload= zworkload;
	Z_ADDREF_P(zworkload);
	/* need to store a ref to the client for later access to cb's */
	task->zclient= zobj;
	Z_ADDREF_P(zobj);
	task->client= &obj->client;

	/* add the task */
	task->task= gearman_client_add_task_high(&(obj->client), task->task, 
	                                         (void *)task, function_name, 
	                                         unique, Z_STRVAL_P(zworkload),
	                                         (size_t)Z_STRLEN_P(zworkload), 
	                                         &obj->ret);
	if (obj->ret != GEARMAN_SUCCESS) {
		php_error_docref(NULL TSRMLS_CC, E_WARNING,
		                 gearman_client_error(&(obj->client)));
		RETURN_FALSE;
	}
	task->flags |= GEARMAN_TASK_OBJ_CREATED;
}
/* }}} */

/* {{{ proto object gearman_client_add_task_low(object client, string function, zval workload [, string unique ])
   Add a low priority task to be run in parallel. */
PHP_FUNCTION(gearman_client_add_task_low) {
	zval *zobj;
	zval *zworkload;
	zval *zdata= NULL;
	gearman_client_obj *obj;
	gearman_task_obj *task;

	char *unique= NULL;
	char *function_name;
	int unique_len= 0;
	int function_name_len= 0;

	GEARMAN_ZPMP(RETURN_NULL(), "sz|zs", &zobj, gearman_client_ce, 
	             &function_name, &function_name_len, &zworkload,
	             &zdata, &unique, &unique_len)

	/* get a task object, and prepare it for return */
	Z_TYPE_P(return_value)= IS_OBJECT;
	object_init_ex(return_value, gearman_task_ce);
	task= zend_object_store_get_object(return_value TSRMLS_CC);

	if (zdata) {
		/* add zdata tothe task object and pass the task object via fn_arg
		 * task->client= zobj; */
		task->zdata= zdata;
		Z_ADDREF_P(zdata);
	}

	/* store our workload and add ref so it wont go away on us */
	task->zworkload= zworkload;
	Z_ADDREF_P(zworkload);
	/* need to store a ref to the client for later access to cb's */
	task->zclient= zobj;
	Z_ADDREF_P(zobj);
	task->client= &obj->client;

	/* add the task */
	task->task= gearman_client_add_task_low(&(obj->client), task->task, 
	                                         (void *)task, function_name, 
	                                         unique, Z_STRVAL_P(zworkload),
	                                         (size_t)Z_STRLEN_P(zworkload), 
	                                         &obj->ret);
	if (obj->ret != GEARMAN_SUCCESS) {
		php_error_docref(NULL TSRMLS_CC, E_WARNING,
		                 gearman_client_error(&(obj->client)));
		RETURN_FALSE;
	}
	task->flags |= GEARMAN_TASK_OBJ_CREATED;
}
/* }}} */

/* {{{ proto object gearman_client_add_task_background(object client, string function, zval workload [, string unique ])
   Add a background task to be run in parallel. */
PHP_FUNCTION(gearman_client_add_task_background) {
	zval *zobj;
	zval *zworkload;
	zval *zdata= NULL;
	gearman_client_obj *obj;
	gearman_task_obj *task;

	char *unique= NULL;
	char *function_name;
	int unique_len= 0;
	int function_name_len= 0;

	GEARMAN_ZPMP(RETURN_NULL(), "sz|zs", &zobj, gearman_client_ce, 
	             &function_name, &function_name_len, &zworkload,
	             &zdata, &unique, &unique_len)

	/* get a task object, and prepare it for return */
	Z_TYPE_P(return_value)= IS_OBJECT;
	object_init_ex(return_value, gearman_task_ce);
	task= zend_object_store_get_object(return_value TSRMLS_CC);

	if (zdata) {
		/* add zdata tothe task object and pass the task object via fn_arg
		 * task->client= zobj; */
		task->zdata= zdata;
		Z_ADDREF_P(zdata);
	}

	/* store our workload and add ref so it wont go away on us */
	task->zworkload= zworkload;
	Z_ADDREF_P(zworkload);
	/* need to store a ref to the client for later access to cb's */
	task->zclient= zobj;
	Z_ADDREF_P(zobj);
	task->client= &obj->client;

	/* add the task */
	task->task= gearman_client_add_task_background(&(obj->client), task->task, 
	                                           (void *)task, function_name, 
	                                            unique, Z_STRVAL_P(zworkload),
	                                           (size_t)Z_STRLEN_P(zworkload),
	                                           &obj->ret);
	if (obj->ret != GEARMAN_SUCCESS) {
		php_error_docref(NULL TSRMLS_CC, E_WARNING,
		                 gearman_client_error(&(obj->client)));
		RETURN_FALSE;
	}
	task->flags |= GEARMAN_TASK_OBJ_CREATED;
}
/* }}} */

/* {{{ proto object gearman_client_add_task_high_background(object client, string function, zval workload [, string unique ])
   Add a high priority background task to be run in parallel. */
PHP_FUNCTION(gearman_client_add_task_high_background) {
	zval *zobj;
	zval *zworkload;
	zval *zdata= NULL;
	gearman_client_obj *obj;
	gearman_task_obj *task;

	char *unique= NULL;
	char *function_name;
	int unique_len= 0;
	int function_name_len= 0;

	GEARMAN_ZPMP(RETURN_NULL(), "sz|zs", &zobj, gearman_client_ce, 
	             &function_name, &function_name_len, &zworkload,
	             &zdata, &unique, &unique_len)

	/* get a task object, and prepare it for return */
	Z_TYPE_P(return_value)= IS_OBJECT;
	object_init_ex(return_value, gearman_task_ce);
	task= zend_object_store_get_object(return_value TSRMLS_CC);

	if (zdata) {
		/* add zdata tothe task object and pass the task object via fn_arg
		 * task->client= zobj; */
		task->zdata= zdata;
		Z_ADDREF_P(zdata);
	}

	/* store our workload and add ref so it wont go away on us */
	task->zworkload= zworkload;
	Z_ADDREF_P(zworkload);
	/* need to store a ref to the client for later access to cb's */
	task->zclient= zobj;
	Z_ADDREF_P(zobj);
	task->client= &obj->client;

	/* add the task */
	task->task= gearman_client_add_task_high_background(&(obj->client), 
	                                     task->task, (void *)task, 
										 function_name, unique, 
										 Z_STRVAL_P(zworkload),
	                                     (size_t)Z_STRLEN_P(zworkload),
	                                      &obj->ret);
	if (obj->ret != GEARMAN_SUCCESS) {
		php_error_docref(NULL TSRMLS_CC, E_WARNING,
		                 gearman_client_error(&(obj->client)));
		RETURN_FALSE;
	}
	task->flags |= GEARMAN_TASK_OBJ_CREATED;
}
/* }}} */

/* {{{ proto object gearman_client_add_task_low_background(object client, string function, zval workload [, string unique ])
   Add a low priority background task to be run in parallel. */
PHP_FUNCTION(gearman_client_add_task_low_background) {
	zval *zobj;
	zval *zworkload;
	zval *zdata= NULL;
	gearman_client_obj *obj;
	gearman_task_obj *task;

	char *unique= NULL;
	char *function_name;
	int unique_len= 0;
	int function_name_len= 0;

	GEARMAN_ZPMP(RETURN_NULL(), "sz|zs", &zobj, gearman_client_ce, 
	             &function_name, &function_name_len, &zworkload,
	             &zdata, &unique, &unique_len)

	/* get a task object, and prepare it for return */
	Z_TYPE_P(return_value)= IS_OBJECT;
	object_init_ex(return_value, gearman_task_ce);
	task= zend_object_store_get_object(return_value TSRMLS_CC);

	if (zdata) {
		/* add zdata tothe task object and pass the task object via fn_arg
		 * task->client= zobj; */
		task->zdata= zdata;
		Z_ADDREF_P(zdata);
	}

	/* store our workload and add ref so it wont go away on us */
	task->zworkload= zworkload;
	Z_ADDREF_P(zworkload);
	/* need to store a ref to the client for later access to cb's */
	task->zclient= zobj;
	Z_ADDREF_P(zobj);
	task->client= &obj->client;

	/* add the task */

	task->task= 
		gearman_client_add_task_low_background(&(obj->client), task->task, 
	                                          (void *)task, function_name, 
	                                           unique, Z_STRVAL_P(zworkload),
	                                          (size_t)Z_STRLEN_P(zworkload),
	                                           &obj->ret);
	if (obj->ret != GEARMAN_SUCCESS) {
		php_error_docref(NULL TSRMLS_CC, E_WARNING,
		                 gearman_client_error(&(obj->client)));
		RETURN_FALSE;
	}
	task->flags |= GEARMAN_TASK_OBJ_CREATED;
}
/* }}} */

/* this function is used to request status information from the gearmand
 * server. it will then call you pre_defined status callback, passing
 * zdata/fn_arg to it */
/* {{{ proto object gearman_client_add_task_status(object client, string job_handle [, zval data])
   Add task to get the status for a backgound task in parallel. */
PHP_FUNCTION(gearman_client_add_task_status) {
	zval *zobj;
	zval *zdata= NULL;
	gearman_client_obj *obj;
	gearman_task_obj *task;

	char *job_handle;
	int job_handle_len= 0;

	GEARMAN_ZPMP(RETURN_NULL(), "s|z", &zobj, gearman_client_ce, 
	             &job_handle, &job_handle_len, &zdata)

	/* get a task object, and prepare it for return */
	Z_TYPE_P(return_value)= IS_OBJECT;
	object_init_ex(return_value, gearman_task_ce);
	task= zend_object_store_get_object(return_value TSRMLS_CC);

	/* add zdata tothe task object and pass the task object via fn_arg
	 * task->client= zobj; */
	if (zdata) {
		task->zdata= zdata;
		Z_ADDREF_P(zdata);
	}
	/* need to store a ref to the client for later access to cb's */
	task->zclient= zobj;
	Z_ADDREF_P(zobj);
	task->client= &obj->client;

	/* add the task */
	task->task= gearman_client_add_task_status(&(obj->client), task->task, 
	                                    (void *)task, job_handle, &obj->ret);
	if (obj->ret != GEARMAN_SUCCESS) {
		php_error_docref(NULL TSRMLS_CC, E_WARNING,
		                 gearman_client_error(&(obj->client)));
		RETURN_FALSE;
	}
	task->flags |= GEARMAN_TASK_OBJ_CREATED;
}
/* }}} */

/* this function will be used to call our user defined task callbacks */
static gearman_return_t _php_task_cb_fn(gearman_task_obj *task, 
	                                gearman_client_obj *client, zval *zcall) {
	gearman_return_t ret;
	/* cb vars */
	zval *ztask;
	zval **argv[1];
	zval *zret_ptr= NULL;
	bool null_ztask= false;
	gearman_task_obj *new_obj;
	zend_fcall_info fci;
	zend_fcall_info_cache fcic= empty_fcall_info_cache;

	MAKE_STD_ZVAL(ztask)
	if (task->flags & GEARMAN_TASK_OBJ_DEAD) {
		Z_TYPE_P(ztask)= IS_OBJECT;
		object_init_ex(ztask, gearman_task_ce);
		new_obj= zend_object_store_get_object(ztask TSRMLS_CC);
		/* copy over our members */
		new_obj->zclient= client->zclient;
		Z_ADDREF_P(client->zclient);
		new_obj->zdata= task->zdata;
		new_obj->zworkload= task->zworkload;
		new_obj->client= task->client;
		new_obj->task= task->task;
		new_obj->flags|= GEARMAN_TASK_OBJ_CREATED;
		gearman_task_set_fn_arg(new_obj->task, new_obj);
		efree(task);
		task= new_obj;
	} else {
		Z_TYPE_P(ztask)= IS_OBJECT;
		Z_OBJVAL_P(ztask)= task->value;
		null_ztask= true;
	}

	argv[0]= &ztask;
	fci.param_count= 1;
	fci.size= sizeof(fci);
	fci.function_table= EG(function_table);
	fci.function_name= zcall;
	fci.symbol_table= NULL;
	fci.retval_ptr_ptr= &zret_ptr;
	fci.params= argv;
	/* XXX Not sure if there is a better way to do this. 
	 * This struct changed in 5.3 and object_pp is now object_ptr
	 * -jluedke */
#if PHP_VERSION_ID < 50300 /* PHP <= 5.2 */
	fci.object_pp= NULL;
#else
	fci.object_ptr= NULL;
#endif 
	fci.no_separation= 0;

	if (zend_call_function(&fci, &fcic TSRMLS_CC) == FAILURE) {
		php_error_docref(NULL TSRMLS_CC, E_WARNING, 
		                "Could not call the function %s", 
		                Z_STRVAL_P(zcall) ? 
		                Z_STRVAL_P(zcall) : "[undefined]");
	}

	if (null_ztask) {
		Z_TYPE_P(ztask)= IS_NULL;
	}
	GEARMAN_ZVAL_DONE(ztask)

	ret= GEARMAN_SUCCESS;
	if (zret_ptr != NULL && Z_TYPE_P(zret_ptr) != IS_NULL) {
		if (Z_TYPE_P(zret_ptr) != IS_LONG) {
			convert_to_long(zret_ptr);
		}
		ret= Z_LVAL_P(zret_ptr);
	}

	if (zret_ptr != NULL) {
		GEARMAN_ZVAL_DONE(zret_ptr);
	}

	return ret;
}

/* TODO: clean this up a bit, Macro? */
static gearman_return_t _php_task_workload_fn(gearman_task_st *task) {
	gearman_task_obj *task_obj;
	gearman_client_obj *client_obj;
	task_obj= (gearman_task_obj *)gearman_task_fn_arg(task);
	client_obj= (gearman_client_obj *)gearman_client_data(task_obj->client);
	return _php_task_cb_fn(task_obj, client_obj, client_obj->zworkload_fn);
}

static gearman_return_t _php_task_created_fn(gearman_task_st *task) {
	gearman_task_obj *task_obj;
	gearman_client_obj *client_obj;
	task_obj= (gearman_task_obj *)gearman_task_fn_arg(task);
	client_obj= (gearman_client_obj *)gearman_client_data(task_obj->client);
	return _php_task_cb_fn(task_obj, client_obj, client_obj->zcreated_fn);
}

static gearman_return_t _php_task_data_fn(gearman_task_st *task) {
	gearman_task_obj *task_obj;
	gearman_client_obj *client_obj;
	task_obj= (gearman_task_obj *)gearman_task_fn_arg(task);
	client_obj= (gearman_client_obj *)gearman_client_data(task_obj->client);
	return _php_task_cb_fn(task_obj, client_obj, client_obj->zdata_fn);
}

static gearman_return_t _php_task_warning_fn(gearman_task_st *task) {
	gearman_task_obj *task_obj;
	gearman_client_obj *client_obj;
	task_obj= (gearman_task_obj *)gearman_task_fn_arg(task);
	client_obj= (gearman_client_obj *)gearman_client_data(task_obj->client);
	return _php_task_cb_fn(task_obj, client_obj, client_obj->zwarning_fn);
}

static gearman_return_t _php_task_status_fn(gearman_task_st *task) {
	gearman_task_obj *task_obj;
	gearman_client_obj *client_obj;
	task_obj= (gearman_task_obj *)gearman_task_fn_arg(task);
	client_obj= (gearman_client_obj *)gearman_client_data(task_obj->client);
	return _php_task_cb_fn(task_obj, client_obj, client_obj->zstatus_fn);
}

static gearman_return_t _php_task_complete_fn(gearman_task_st *task) {
	gearman_task_obj *task_obj;
	gearman_client_obj *client_obj;
	task_obj= (gearman_task_obj *)gearman_task_fn_arg(task);
	client_obj= (gearman_client_obj *)gearman_client_data(task_obj->client);
	return _php_task_cb_fn(task_obj, client_obj, client_obj->zcomplete_fn);
}

static gearman_return_t _php_task_exception_fn(gearman_task_st *task) {
	gearman_task_obj *task_obj;
	gearman_client_obj *client_obj;
	task_obj= (gearman_task_obj *)gearman_task_fn_arg(task);
	client_obj= (gearman_client_obj *)gearman_client_data(task_obj->client);
	return _php_task_cb_fn(task_obj, client_obj, client_obj->zexception_fn);
}

static gearman_return_t _php_task_fail_fn(gearman_task_st *task) {
	gearman_task_obj *task_obj;
	gearman_client_obj *client_obj;
	task_obj= (gearman_task_obj *)gearman_task_fn_arg(task);
	client_obj= (gearman_client_obj *)gearman_client_data(task_obj->client);
	return _php_task_cb_fn(task_obj, client_obj, client_obj->zfail_fn);
}

/* {{{ proto bool gearman_client_set_workload_fn(object client, callback function)
   Callback function when workload data needs to be sent for a task. */
PHP_FUNCTION(gearman_client_set_workload_fn) {
	zval *zobj;
	zval *zworkload_fn;
	gearman_client_obj *obj;
	char *callable= NULL;

	GEARMAN_ZPMP(RETURN_NULL(), "z", &zobj, gearman_client_ce, 
	             &zworkload_fn)

	/* check that the function is callable */
	if (! zend_is_callable(zworkload_fn, 0, &callable)) {
		php_error_docref(NULL TSRMLS_CC, E_WARNING, 
		            "function %s is not callable", callable);
		efree(callable);
		RETURN_FALSE;
	}
	efree(callable);

	/* store the cb in client object */
	obj->zworkload_fn= zworkload_fn;

	/* set the callback for php */
	gearman_client_set_workload_fn(&(obj->client), _php_task_workload_fn);

	RETURN_TRUE;
}
/* }}} */

/* {{{ proto bool gearman_client_set_created_fn(object client, callback function)
   Callback function when workload data needs to be sent for a task. */
PHP_FUNCTION(gearman_client_set_created_fn) {
	zval *zobj;
	zval *zcreated_fn;
	gearman_client_obj *obj;
	char *callable= NULL;

	GEARMAN_ZPMP(RETURN_NULL(), "z", &zobj, gearman_client_ce, 
	             &zcreated_fn)

	/* check that the function is callable */
	if (! zend_is_callable(zcreated_fn, 0, &callable)) {
		php_error_docref(NULL TSRMLS_CC, E_WARNING, 
		            "function %s is not callable", callable);
		efree(callable);
		RETURN_FALSE;
	}
	efree(callable);

	/* store the cb in client object */
	obj->zcreated_fn= zcreated_fn;
	Z_ADDREF_P(zcreated_fn);

	/* set the callback for php */
	gearman_client_set_created_fn(&(obj->client), _php_task_created_fn);

	RETURN_TRUE;
}

/* {{{ proto bool gearman_client_set_data_fn(object client, callback function)
   Callback function when there is a data packet for a task. */
PHP_FUNCTION(gearman_client_set_data_fn) {
	zval *zobj;
	zval *zdata_fn;
	gearman_client_obj *obj;
	char *callable= NULL;

	GEARMAN_ZPMP(RETURN_NULL(), "z", &zobj, gearman_client_ce, 
	             &zdata_fn)

	/* check that the function is callable */
	if (! zend_is_callable(zdata_fn, 0, &callable)) {
		php_error_docref(NULL TSRMLS_CC, E_WARNING, 
		            "function %s is not callable", callable);
		efree(callable);
		RETURN_FALSE;
	}
	efree(callable);

	/* store the cb in client object */
	obj->zdata_fn= zdata_fn;
	Z_ADDREF_P(zdata_fn);

	/* set the callback for php */
	gearman_client_set_data_fn(&(obj->client), _php_task_data_fn);

	RETURN_TRUE;
}
/* }}} */

/* {{{ proto bool gearman_client_set_warning_fn(object client, callback function)
   Callback function when there is a warning packet for a task. */
PHP_FUNCTION(gearman_client_set_warning_fn) {
	zval *zobj;
	zval *zwarning_fn;
	gearman_client_obj *obj;
	char *callable= NULL;

	GEARMAN_ZPMP(RETURN_NULL(), "z", &zobj, gearman_client_ce, 
	             &zwarning_fn)

	/* check that the function is callable */
	if (! zend_is_callable(zwarning_fn, 0, &callable)) {
		php_error_docref(NULL TSRMLS_CC, E_WARNING, 
		            "function %s is not callable", callable);
		efree(callable);
		RETURN_FALSE;
	}
	efree(callable);

	/* store the cb in client object */
	obj->zwarning_fn= zwarning_fn;
	Z_ADDREF_P(zwarning_fn);

	/* set the callback for php */
	gearman_client_set_warning_fn(&(obj->client), _php_task_warning_fn);

	RETURN_TRUE;
}
/* }}} */

/* {{{ proto bool gearman_client_set_status_fn(object client, callback function)
   Callback function when there is a status packet for a task. */
PHP_FUNCTION(gearman_client_set_status_fn) {
	zval *zobj;
	zval *zstatus_fn;
	gearman_client_obj *obj;
	char *callable= NULL;

	GEARMAN_ZPMP(RETURN_NULL(), "z", &zobj, gearman_client_ce, 
	             &zstatus_fn)

	/* check that the function is callable */
	if (! zend_is_callable(zstatus_fn, 0, &callable)) {
		php_error_docref(NULL TSRMLS_CC, E_WARNING, 
		            "function %s is not callable", callable);
		efree(callable);
		RETURN_FALSE;
	}
	efree(callable);

	/* store the cb in client object */
	obj->zstatus_fn= zstatus_fn;
	Z_ADDREF_P(zstatus_fn);

	/* set the callback for php */
	gearman_client_set_status_fn(&(obj->client), _php_task_status_fn);

	RETURN_TRUE;
}
/* }}} */

/* {{{ proto bool gearman_client_set_complete_fn(object client, callback function)
   Callback function when there is a status packet for a task. */
PHP_FUNCTION(gearman_client_set_complete_fn) {
	zval *zobj;
	zval *zcomplete_fn;
	gearman_client_obj *obj;
	char *callable= NULL;

	GEARMAN_ZPMP(RETURN_NULL(), "z", &zobj, gearman_client_ce, 
	             &zcomplete_fn)

	/* check that the function is callable */
	if (! zend_is_callable(zcomplete_fn, 0, &callable)) {
		php_error_docref(NULL TSRMLS_CC, E_WARNING, 
		            "function %s is not callable", callable);
		efree(callable);
		RETURN_FALSE;
	}
	efree(callable);

	/* store the cb in client object */
	obj->zcomplete_fn= zcomplete_fn;
	Z_ADDREF_P(zcomplete_fn);

	/* set the callback for php */
	gearman_client_set_complete_fn(&(obj->client), _php_task_complete_fn);

	RETURN_TRUE;
}
/* }}} */

/* {{{ proto bool gearman_client_set_exception_fn(object client, callback function)
   Callback function when there is a exception packet for a task. */
PHP_FUNCTION(gearman_client_set_exception_fn) {
	zval *zobj;
	zval *zexception_fn;
	gearman_client_obj *obj;
	char *callable= NULL;

	GEARMAN_ZPMP(RETURN_NULL(), "z", &zobj, gearman_client_ce, 
	             &zexception_fn)

	/* check that the function is callable */
	if (! zend_is_callable(zexception_fn, 0, &callable)) {
		php_error_docref(NULL TSRMLS_CC, E_WARNING, 
		            "function %s is not callable", callable);
		efree(callable);
		RETURN_FALSE;
	}
	efree(callable);

	/* store the cb in client object */
	obj->zexception_fn= zexception_fn;
	Z_ADDREF_P(zexception_fn);

	/* set the callback for php */
	gearman_client_set_exception_fn(&(obj->client), _php_task_exception_fn);

	RETURN_TRUE;
}
/* }}} */

/* {{{ proto bool gearman_client_set_fail_fn(object client, callback function)
   Callback function when there is a fail packet for a task. */
PHP_FUNCTION(gearman_client_set_fail_fn) {
	zval *zobj;
	zval *zfail_fn;
	gearman_client_obj *obj;
	char *callable= NULL;

	GEARMAN_ZPMP(RETURN_NULL(), "z", &zobj, gearman_client_ce, 
	             &zfail_fn)

	/* check that the function is callable */
	if (! zend_is_callable(zfail_fn, 0, &callable)) {
		php_error_docref(NULL TSRMLS_CC, E_WARNING, 
		            "function %s is not callable", callable);
		efree(callable);
		RETURN_FALSE;
	}
	efree(callable);

	/* store the cb in client object */
	obj->zfail_fn= zfail_fn;
	Z_ADDREF_P(zfail_fn);

	/* set the callback for php */
	gearman_client_set_fail_fn(&(obj->client), _php_task_fail_fn);

	RETURN_TRUE;
}
/* }}} */

/* {{{ proto void gearman_client_clear_fn(object client)
   Clear all task callback functions. */
PHP_FUNCTION(gearman_client_clear_fn) {
	zval *zobj;
	gearman_client_obj *obj;

	GEARMAN_ZPMP(RETURN_NULL(), "", &zobj, gearman_client_ce)

	gearman_client_clear_fn(&(obj->client));

	GEARMAN_ZVAL_DONE(obj->zworkload_fn)
	obj->zworkload_fn= NULL;
	GEARMAN_ZVAL_DONE(obj->zcreated_fn)
	obj->zcreated_fn= NULL;
	GEARMAN_ZVAL_DONE(obj->zdata_fn)
	obj->zdata_fn= NULL;
	GEARMAN_ZVAL_DONE(obj->zwarning_fn)
	obj->zwarning_fn= NULL;
	GEARMAN_ZVAL_DONE(obj->zstatus_fn)
	obj->zstatus_fn= NULL;
	GEARMAN_ZVAL_DONE(obj->zcomplete_fn)
	obj->zcomplete_fn= NULL;
	GEARMAN_ZVAL_DONE(obj->zexception_fn)
	obj->zexception_fn= NULL;
	GEARMAN_ZVAL_DONE(obj->zfail_fn)
	obj->zfail_fn= NULL;

	RETURN_TRUE;
}
/* }}} */

/* {{{ proto string gearman_client_data(object client)
   Get the application data */
PHP_FUNCTION(gearman_client_data) {
	zval *zobj;
	gearman_client_obj *obj;
	const uint8_t *data;

	GEARMAN_ZPMP(RETURN_NULL(), "", &zobj, gearman_client_ce)

	/* XXX is there no client_data_size? */
	data= gearman_client_data(&(obj->client));
	
	RETURN_STRINGL((char *)data, (long) sizeof(data), 1);
}
/* }}} */

/* {{{ proto bool gearman_client_set_data(object client, string data)
   Set the application data */
PHP_FUNCTION(gearman_client_set_data) {
	zval *zobj;
	gearman_client_obj *obj;

	char *data;
	int data_len= 0;

	GEARMAN_ZPMP(RETURN_NULL(), "s", &zobj, gearman_client_ce, 
	             &data, &data_len)

	gearman_client_set_data(&(obj->client), (void *)data);
	RETURN_TRUE;
}
/* }}} */

/* {{{ proto bool gearman_client_run_tasks(object client)
   Run tasks that have been added in parallel */
PHP_FUNCTION(gearman_client_run_tasks) {
	zval *zobj;
	gearman_client_obj *obj;

	GEARMAN_ZPMP(RETURN_NULL(), "", &zobj, gearman_client_ce)

	obj->zclient= zobj;
	obj->ret= gearman_client_run_tasks(&(obj->client));

	if (! PHP_GEARMAN_CLIENT_RET_OK(obj->ret)) {
		php_error_docref(NULL TSRMLS_CC, E_WARNING,
		                 gearman_client_error(&(obj->client)));
		RETURN_FALSE;
	}

	RETURN_TRUE;
}

/*
 * Functions from worker.h
 */

/* {{{ proto int gearman_worker_return_code()
   get last gearman_return_t */
PHP_FUNCTION(gearman_worker_return_code)
{
	zval *zobj;
	gearman_worker_obj *obj;
	GEARMAN_ZPMP(RETURN_NULL(), "", &zobj, gearman_worker_ce)
	RETURN_LONG(obj->ret);
}
/* }}} */

/* {{{ proto object gearman_worker_create()
   Returns a worker object */
PHP_FUNCTION(gearman_worker_create) {
	gearman_worker_obj *worker;

	Z_TYPE_P(return_value)= IS_OBJECT;
	object_init_ex(return_value, gearman_worker_ce);
	worker= zend_object_store_get_object(return_value TSRMLS_CC);

	if (gearman_worker_create(&(worker->worker)) == NULL) {
		php_error_docref(NULL TSRMLS_CC, E_WARNING, 
		                 "Memory allocation failure.");
		zval_dtor(return_value);
		RETURN_FALSE;
	}

	worker->flags|= GEARMAN_WORKER_OBJ_CREATED;
	gearman_worker_set_workload_malloc(&(worker->worker), _php_malloc, NULL);
	gearman_worker_set_workload_free(&(worker->worker), _php_free, NULL);
}
/* }}} */

/* {{{ proto object gearman_worker_clone(object worker)
   Clone a worker object */
PHP_FUNCTION(gearman_worker_clone) {
	zval *zobj;
	gearman_worker_obj *obj;
	gearman_worker_obj *new;

	GEARMAN_ZPMP(RETURN_NULL(), "", &zobj, gearman_worker_ce)

	Z_TYPE_P(return_value)= IS_OBJECT;
	object_init_ex(return_value, gearman_worker_ce);
	new= zend_object_store_get_object(return_value TSRMLS_CC);

	if (gearman_worker_clone(&(new->worker), &(obj->worker)) == NULL) {
		php_error_docref(NULL TSRMLS_CC, E_WARNING, 
		                 "Memory allocation failure.");
		zval_dtor(return_value);
		RETURN_FALSE;
	}

	new->flags|= GEARMAN_WORKER_OBJ_CREATED;
}
/* }}} */

/* {{{ proto void gearman_worker_free(object worker)
   Free resources used by a worker structure. */
PHP_FUNCTION(gearman_worker_free) {
	zval *zobj;
	gearman_worker_obj *obj;

	GEARMAN_ZPP(RETURN_NULL(), "", &zobj, gearman_worker_ce)
	obj= zend_object_store_get_object(zobj TSRMLS_CC);

	if (obj->flags & GEARMAN_WORKER_OBJ_CREATED) {
		gearman_worker_free(&(obj->worker));
		obj->flags&= ~GEARMAN_WORKER_OBJ_CREATED;
	}
}
/* }}} */

/* {{{ proto string gearman_worker_error(object worker)
   Return an error string for the last error encountered. */
PHP_FUNCTION(gearman_worker_error) {
	zval *zobj;
	gearman_worker_obj *obj;

	GEARMAN_ZPMP(RETURN_NULL(), "", &zobj, gearman_worker_ce)

	RETURN_STRING((char *)gearman_worker_error(&(obj->worker)), 1);
}
/* }}} */

/* {{{ proto int gearman_worker_errno(object worker)
   Value of errno in the case of a GEARMAN_ERRNO return value. */
PHP_FUNCTION(gearman_worker_errno) {
	zval *zobj;
	gearman_worker_obj *obj;

	GEARMAN_ZPMP(RETURN_NULL(), "", &zobj, gearman_worker_ce)

	RETURN_LONG(gearman_worker_errno(&(obj->worker)))
}
/* }}} */

/* {{{ proto void gearman_worker_set_options(object worker, constant option, int value)
   Set options for a worker structure. */
PHP_FUNCTION(gearman_worker_set_options) {
	zval *zobj;
	gearman_worker_obj *obj;
	long options;
	long data;

	GEARMAN_ZPMP(RETURN_NULL(), "ll", &zobj, gearman_worker_ce, 
	             &options, &data)

	gearman_worker_set_options(&(obj->worker), options, data);
	RETURN_TRUE;
}
/* }}} */

/* {{{ proto bool gearman_worker_add_server(object worker [, string host [, int port ]])
   Add a job server to a worker. This goes into a list of servers than can be used to run tasks. No socket I/O happens here, it is just added to a list. */
PHP_FUNCTION(gearman_worker_add_server) {
	zval *zobj;
	gearman_worker_obj *obj;
	char *host= NULL;
	int host_len= 0;
	long port= 0;

	GEARMAN_ZPMP(RETURN_NULL(), "|sl", &zobj, gearman_worker_ce, 
	            &host, &host_len, &port)

	obj->ret= gearman_worker_add_server(&(obj->worker), host, port);
	if (obj->ret != GEARMAN_SUCCESS) {
		php_error_docref(NULL TSRMLS_CC, E_WARNING,
		                 gearman_worker_error(&(obj->worker)));
		RETURN_FALSE;
	}

	RETURN_TRUE;
}
/* }}} */

/* {{{ proto bool gearman_worker_register(object worker, string function [, int timeout ])
   Register function with job servers with an optional timeout. The timeout specifies how many seconds the server will wait before marking a job as failed. If timeout is zero, there is no timeout. */
PHP_FUNCTION(gearman_worker_register) {
	zval *zobj;
	gearman_worker_obj *obj;
	char *function_name;
	int function_name_len;
	int timeout;

	GEARMAN_ZPMP(RETURN_NULL(), "s|l", &zobj, gearman_worker_ce, 
	             &function_name, &function_name_len, &timeout)

	obj->ret= gearman_worker_register(&(obj->worker), function_name, timeout);
	if (obj->ret != GEARMAN_SUCCESS) {
		php_error_docref(NULL TSRMLS_CC, E_WARNING,
		                 gearman_worker_error(&(obj->worker)));
		RETURN_FALSE;
	}

	RETURN_TRUE;
}
/* }}} */

/* {{{ proto bool gearman_worker_unregister(object worker, string function)
    Unregister function with job servers. */
PHP_FUNCTION(gearman_worker_unregister) {
	zval *zobj;
	gearman_worker_obj *obj;
	char *function_name;
	int function_name_len;

	GEARMAN_ZPMP(RETURN_NULL(), "s", &zobj, gearman_worker_ce, 
	             &function_name, &function_name_len)

	obj->ret= gearman_worker_unregister(&(obj->worker), function_name);
	if (obj->ret != GEARMAN_SUCCESS) {
		php_error_docref(NULL TSRMLS_CC, E_WARNING,
		                 gearman_worker_error(&(obj->worker)));
		RETURN_FALSE;
	}

	RETURN_TRUE;
}
/* }}} */

/* {{{ proto bool gearman_worker_unregister_all(object worker)
   Unregister all functions with job servers. */
PHP_FUNCTION(gearman_worker_unregister_all) {
	zval *zobj;
	gearman_worker_obj *obj;

	GEARMAN_ZPMP(RETURN_NULL(), "", &zobj, gearman_worker_ce)

	obj->ret= gearman_worker_unregister_all(&(obj->worker));
	if (obj->ret != GEARMAN_SUCCESS) {
		php_error_docref(NULL TSRMLS_CC, E_WARNING,
		                 gearman_worker_error(&(obj->worker)));
		RETURN_FALSE;
	}

	RETURN_TRUE;
}
/* }}} */

/* {{{ proto object gearman_worker_grab_job(obect worker)
   Get a job from one of the job servers. */
PHP_FUNCTION(gearman_worker_grab_job) {
	zval *zobj;
	gearman_worker_obj *obj;
	gearman_job_obj *job;

	GEARMAN_ZPMP(RETURN_NULL(), "", &zobj, gearman_worker_ce)

	Z_TYPE_P(return_value)= IS_OBJECT;
	object_init_ex(return_value, gearman_job_ce);
	job= zend_object_store_get_object(return_value TSRMLS_CC);
	job->worker= zobj;
	Z_ADDREF_P(zobj);

	job->job= gearman_worker_grab_job(&(obj->worker), NULL, &obj->ret);
	if (obj->ret != GEARMAN_SUCCESS && obj->ret != GEARMAN_IO_WAIT) {
		php_error_docref(NULL TSRMLS_CC, E_WARNING,
		                 gearman_worker_error(&(obj->worker)));
		zval_dtor(return_value);
		RETURN_FALSE;
	}

	job->flags|= GEARMAN_JOB_OBJ_CREATED;
}

/* *job is passed in via gearman, need to convert that into a zval that
 * is accessable in the user_defined php callback function */
static void *_php_worker_function_callback(gearman_job_st *job, void *fn_arg,
                                          size_t *result_size,
                                          gearman_return_t *ret_ptr) {
	zval *zjob;
	gearman_job_obj *jobj;
	gearman_worker_cb *worker_cb= (gearman_worker_cb *)fn_arg;
	char *result;

	/* cb vars */
	zval **argv[2];
	zval *zret_ptr= NULL;
	zend_fcall_info fci;
	zend_fcall_info_cache fcic= empty_fcall_info_cache;

	/* first create our job object that will be passed to the callback */
	MAKE_STD_ZVAL(zjob);
	Z_TYPE_P(zjob)= IS_OBJECT;
	object_init_ex(zjob, gearman_job_ce);
	jobj= zend_object_store_get_object(zjob TSRMLS_CC);
	jobj->job= job;

	argv[0]= &zjob;
	if (worker_cb->zdata == NULL) {
		fci.param_count= 1;
	} else {
		argv[1]= &(worker_cb->zdata);
		fci.param_count= 2;
	}
	
	fci.size= sizeof(fci);
	fci.function_table= EG(function_table);
	fci.function_name= worker_cb->zcall;
	fci.symbol_table= NULL;
	fci.retval_ptr_ptr= &zret_ptr;
	fci.params= argv;
	/* XXX Not sure if there is a better way to do this. jluedke */
#if PHP_VERSION_ID < 50300 /* PHP <= 5.2 */
	fci.object_pp= NULL;
#else
	fci.object_ptr= NULL;
#endif
	fci.no_separation= 0;

	jobj->ret= GEARMAN_SUCCESS;
	if (zend_call_function(&fci, &fcic TSRMLS_CC) == FAILURE) {
		php_error_docref(NULL TSRMLS_CC, E_WARNING, 
		                "Could not call the function %s", 
		                worker_cb->zcall->value.str.val ? 
		                worker_cb->zcall->value.str.val : "[undefined]");
		*ret_ptr= GEARMAN_WORK_FAIL;
	}
	*ret_ptr= jobj->ret;

	if (zret_ptr == NULL || Z_TYPE_P(zret_ptr) == IS_NULL) {
		result= NULL;
	} else {
		if (Z_TYPE_P(zret_ptr) != IS_STRING) {
			convert_to_string(zret_ptr);
		}
		result= Z_STRVAL_P(zret_ptr);
		*result_size= Z_STRLEN_P(zret_ptr);
		Z_STRVAL_P(zret_ptr)= NULL;
		Z_TYPE_P(zret_ptr)= IS_NULL;
	}

	if (zret_ptr != NULL) {
		GEARMAN_ZVAL_DONE(zret_ptr);
	}

	GEARMAN_ZVAL_DONE(zjob);

	return result;
}
/* }}} */

/* {{{ proto bool gearman_worker_add_function(object worker, zval function_name, zval callback [, zval data [, int timeout]])
   Register and add callback function for worker. */
PHP_FUNCTION(gearman_worker_add_function) {
	zval *zobj;
	gearman_worker_obj *obj;
	gearman_worker_cb *worker_cb;

	zval *zname;
	zval *zcall;
	zval *zdata= NULL;
	long timeout;

	char *callable= NULL;

	GEARMAN_ZPMP(RETURN_NULL(), "zz|zl", &zobj, gearman_worker_ce,
	             &zname, &zcall, &zdata, &timeout)

	/* check that the function can be called */
	if (!zend_is_callable(zcall, 0, &callable)) {
		php_error_docref(NULL TSRMLS_CC, E_WARNING, 
		                "function %s is not callable", callable);
		efree(callable);
		RETURN_FALSE;
	}
	efree(callable); 

	/* create a new worker cb */
	worker_cb= emalloc(sizeof(gearman_worker_cb));
	memset(worker_cb, 0, sizeof(gearman_worker_cb));

	/* copy over our zcall and zdata */
	worker_cb->zname= zname;
	Z_ADDREF_P(worker_cb->zname);
	worker_cb->zcall= zcall;
	Z_ADDREF_P(worker_cb->zcall);
	if (zdata != NULL) {
		worker_cb->zdata= zdata;
		Z_ADDREF_P(worker_cb->zdata);
	}
	worker_cb->next= obj->cb_list;
	obj->cb_list= worker_cb;

	/* add the function */
	/* NOTE: _php_worker_function_callback is a wrapper that calls
	 * the function defined by gearman_worker_add_function */
	obj->ret= gearman_worker_add_function(&(obj->worker), zname->value.str.val, 
	                                (uint32_t)timeout, 
	                                _php_worker_function_callback,
	                                (void *)worker_cb);
	if (obj->ret != GEARMAN_SUCCESS) {
		php_error_docref(NULL TSRMLS_CC, E_WARNING,
		                 gearman_worker_error(&(obj->worker)));
		RETURN_FALSE;
	}

	RETURN_TRUE;
}
/* }}} */

/* {{{ proto int gearman_worker_work(object worker)
    Wait for a job and call the appropriate callback function when it gets one. */
PHP_FUNCTION(gearman_worker_work) {
	zval *zobj;
	gearman_worker_obj *obj;

	GEARMAN_ZPMP(RETURN_NULL(), "", &zobj, gearman_worker_ce)

	obj->ret= gearman_worker_work(&(obj->worker));
	if (obj->ret != GEARMAN_SUCCESS && obj->ret != GEARMAN_IO_WAIT &&
		obj->ret != GEARMAN_WORK_FAIL) {
		php_error_docref(NULL TSRMLS_CC, E_WARNING,
		                 gearman_worker_error(&(obj->worker)));
		RETURN_FALSE;
	}

	if(obj->ret != GEARMAN_SUCCESS) {
		RETURN_FALSE;
	}

	RETURN_TRUE;
}
/* }}} */

/* {{{ proto bool gearman_worker_echo(object worker, string data)
   Send data to all job servers to see if they echo it back. */
PHP_FUNCTION(gearman_worker_echo) {
	zval *zobj;
	gearman_worker_obj *obj;
	char *workload;
	int workload_len;

	GEARMAN_ZPMP(RETURN_NULL(), "s", &zobj, gearman_worker_ce, 
	             &workload, &workload_len)

	obj->ret= gearman_worker_echo(&(obj->worker), workload, 
	                             (size_t)workload_len);
	if (obj->ret != GEARMAN_SUCCESS && obj->ret != GEARMAN_IO_WAIT) {
		php_error_docref(NULL TSRMLS_CC, E_WARNING,
		                 gearman_worker_error(&(obj->worker)));
		RETURN_FALSE;
	}

	RETURN_TRUE;
}

/*
 * Functions from gearmand.h
 */

#if jluedke_0 /* XXX low level */
/* {{{ proto void gearmand_create()
 */
PHP_FUNCTION(gearmand_create)
{
}
/* }}} */

/* {{{ proto void gearmand_free()
 */
PHP_FUNCTION(gearmand_free)
{
}
/* }}} */

/* {{{ proto void gearmand_set_backlog()
 */
PHP_FUNCTION(gearmand_set_backlog)
{
}
/* }}} */

/* {{{ proto void gearmand_set_verbose()
 */
PHP_FUNCTION(gearmand_set_verbose)
{
}
/* }}} */

/* {{{ proto void gearmand_error()
 */
PHP_FUNCTION(gearmand_error)
{
}
/* }}} */

/* {{{ proto void gearmand_errno()
 */
PHP_FUNCTION(gearmand_errno)
{
}
/* }}} */

/* {{{ proto void gearmand_run()
 */
PHP_FUNCTION(gearmand_run)
{
}
/* }}} */
#endif

/*
 * Methods for gearman_client
 */

PHP_METHOD(gearman_client, __construct) {
	gearman_client_obj *obj;

	obj= zend_object_store_get_object(getThis() TSRMLS_CC);

	if (gearman_client_create(&(obj->client)) == NULL) {
		php_error_docref(NULL TSRMLS_CC, E_WARNING, 
		                 "Memory allocation failure.");
		RETURN_FALSE;
	}

	obj->flags|= GEARMAN_CLIENT_OBJ_CREATED;
	gearman_client_set_workload_malloc(&(obj->client), _php_malloc, NULL);
	gearman_client_set_workload_free(&(obj->client), _php_free, NULL);
	gearman_client_set_task_fn_arg_free(&(obj->client), _php_task_free);
	gearman_client_set_data(&(obj->client), obj);
}

static void gearman_client_obj_free(void *object TSRMLS_DC) {
	gearman_client_obj *client= (gearman_client_obj *)object;

	if (client->flags & GEARMAN_CLIENT_OBJ_CREATED) {
		gearman_client_free(&(client->client));
	}

	GEARMAN_ZVAL_DONE(client->zworkload_fn)
	GEARMAN_ZVAL_DONE(client->zcreated_fn)
	GEARMAN_ZVAL_DONE(client->zdata_fn)
	GEARMAN_ZVAL_DONE(client->zwarning_fn)
	GEARMAN_ZVAL_DONE(client->zstatus_fn)
	GEARMAN_ZVAL_DONE(client->zcomplete_fn)
	GEARMAN_ZVAL_DONE(client->zexception_fn)
	GEARMAN_ZVAL_DONE(client->zfail_fn)

	zend_object_std_dtor(&(client->std) TSRMLS_CC);
	efree(object);
}

static inline zend_object_value
gearman_client_obj_new_ex(zend_class_entry *class_type,
                          gearman_client_obj **gearman_client_ptr TSRMLS_DC) {

	gearman_client_obj *client;
	zend_object_value value;
	zval *tmp;

	client= emalloc(sizeof(gearman_client_obj));
	memset(client, 0, sizeof(gearman_client_obj));

	if (gearman_client_ptr) {
		*gearman_client_ptr= client;
	}

	zend_object_std_init(&(client->std), class_type TSRMLS_CC);
	zend_hash_copy(client->std.properties, 
	              &(class_type->default_properties),
	              (copy_ctor_func_t)zval_add_ref, (void *)(&tmp),
	              sizeof(zval *));

	value.handle= zend_objects_store_put(client,
	             (zend_objects_store_dtor_t)zend_objects_destroy_object,
	             (zend_objects_free_object_storage_t)gearman_client_obj_free,
	                 NULL TSRMLS_CC);

	value.handlers= &gearman_client_obj_handlers;
	return value;
}

static zend_object_value
gearman_client_obj_new(zend_class_entry *class_type TSRMLS_DC) {
	return gearman_client_obj_new_ex(class_type, NULL TSRMLS_CC);
}

/*
 * Methods for gearman_worker
 */

PHP_METHOD(gearman_worker, __construct) {
	gearman_worker_obj *worker;

	worker= zend_object_store_get_object(getThis() TSRMLS_CC);

	if (gearman_worker_create(&(worker->worker)) == NULL) {
		php_error_docref(NULL TSRMLS_CC, E_WARNING, 
		                 "Memory allocation failure.");
		RETURN_FALSE;
	}

	worker->flags|= GEARMAN_WORKER_OBJ_CREATED;
	gearman_worker_set_workload_malloc(&(worker->worker), _php_malloc, NULL);
	gearman_worker_set_workload_free(&(worker->worker), _php_free, NULL);
}

static void gearman_worker_obj_free(void *object TSRMLS_DC) {
	gearman_worker_obj *worker= (gearman_worker_obj *)object;
	gearman_worker_cb *next_cb= NULL;

	if (worker->flags & GEARMAN_CLIENT_OBJ_CREATED) {
		gearman_worker_free(&(worker->worker));
	}

	while (worker->cb_list) {
		next_cb= worker->cb_list->next;
		GEARMAN_ZVAL_DONE(worker->cb_list->zname)
		GEARMAN_ZVAL_DONE(worker->cb_list->zcall)
		GEARMAN_ZVAL_DONE(worker->cb_list->zdata)
		efree(worker->cb_list);
		worker->cb_list= next_cb;
	}

	zend_object_std_dtor(&(worker->std) TSRMLS_CC);
	efree(object);
}

static inline zend_object_value
gearman_worker_obj_new_ex(zend_class_entry *class_type,
                          gearman_worker_obj **gearman_worker_ptr TSRMLS_DC) {
	gearman_worker_obj *worker;
	zend_object_value value;
	zval *tmp;

	worker= emalloc(sizeof(gearman_worker_obj));
	memset(worker, 0, sizeof(gearman_worker_obj));

	if (gearman_worker_ptr) {
		*gearman_worker_ptr= worker;
	}

	zend_object_std_init(&(worker->std), class_type TSRMLS_CC);
	zend_hash_copy(worker->std.properties, 
	              &(class_type->default_properties),
	              (copy_ctor_func_t)zval_add_ref, (void *)(&tmp),
	              sizeof(zval *));

	value.handle= zend_objects_store_put(worker,
	             (zend_objects_store_dtor_t)zend_objects_destroy_object,
	             (zend_objects_free_object_storage_t)gearman_worker_obj_free,
	              NULL TSRMLS_CC);

	value.handlers= &gearman_worker_obj_handlers;
	return value;
}

static zend_object_value
gearman_worker_obj_new(zend_class_entry *class_type TSRMLS_DC) {
	return gearman_worker_obj_new_ex(class_type, NULL TSRMLS_CC);
}

/*
 * Methods Job object
 */

PHP_METHOD(gearman_job, __construct) {
	zval *zobj;
	gearman_job_obj *obj;
	zval *zgearman;
	gearman_obj *gearman;

	GEARMAN_ZPMP(RETURN_NULL(), "O", &zobj, gearman_job_ce, 
	             &zgearman, gearman_ce)
	gearman= zend_object_store_get_object(zgearman TSRMLS_CC);

	obj->job= gearman_job_create(&(gearman->gearman), NULL);
	if (obj->job == NULL) {
		php_error_docref(NULL TSRMLS_CC, E_WARNING, 
		                 "Memory allocation failure.");
		RETURN_FALSE;
	}

	obj->flags|= GEARMAN_JOB_OBJ_CREATED;
}

static void gearman_job_obj_free(void *object TSRMLS_DC) {
	gearman_job_obj *job= (gearman_job_obj *)object;

	if (job->flags & GEARMAN_JOB_OBJ_CREATED) {
		gearman_job_free(job->job);
	}

	GEARMAN_ZVAL_DONE(job->gearman)
	GEARMAN_ZVAL_DONE(job->worker)

	/*
	if (job->zworkload != NULL)
	{
	  Z_TYPE_P(job->zworkload)= IS_NULL;
	  GEARMAN_ZVAL_DONE(job->zworkload);
	}
	*/
	zend_object_std_dtor(&(job->std) TSRMLS_CC);
	efree(object);
}

static inline zend_object_value
gearman_job_obj_new_ex(zend_class_entry *class_type,
                          gearman_job_obj **gearman_job_ptr TSRMLS_DC) {
	gearman_job_obj *job;
	zend_object_value value;
	zval *tmp;

	job= emalloc(sizeof(gearman_job_obj));
	memset(job, 0, sizeof(gearman_job_obj));

	if (gearman_job_ptr) {
		*gearman_job_ptr= job;
	}

	zend_object_std_init(&(job->std), class_type TSRMLS_CC);
	zend_hash_copy(job->std.properties, 
	              &(class_type->default_properties),
	              (copy_ctor_func_t)zval_add_ref, (void *)(&tmp),
	              sizeof(zval *));

	value.handle= zend_objects_store_put(job,
	                 (zend_objects_store_dtor_t)zend_objects_destroy_object,
	                 (zend_objects_free_object_storage_t)gearman_job_obj_free,
	                 NULL TSRMLS_CC);

	value.handlers= &gearman_job_obj_handlers;

	return value;
}

static zend_object_value
gearman_job_obj_new(zend_class_entry *class_type TSRMLS_DC) {
	return gearman_job_obj_new_ex(class_type, NULL TSRMLS_CC);
}

/*
 * Methods Task object
 */

PHP_METHOD(gearman_task, __construct) {
	zval *zobj;
	zval *zgearman;
	gearman_task_obj *obj;
	gearman_obj *gearman;

	GEARMAN_ZPMP(RETURN_NULL(), "O", &zobj, gearman_task_ce, &zgearman, 
	             gearman_ce)
	gearman= zend_object_store_get_object(zgearman TSRMLS_CC);
	obj->zgearman= zgearman;
	Z_ADDREF_P(zgearman);

	obj->task= gearman_task_create(&(gearman->gearman), obj->task);
	if (obj->task == NULL) {
		php_error_docref(NULL TSRMLS_CC, E_WARNING, 
		                 "Memory allocation failure.");
		RETURN_FALSE;
	}
	obj->flags|= GEARMAN_TASK_OBJ_CREATED;
}

static void gearman_task_obj_free(void *object TSRMLS_DC) {
	gearman_task_obj *task= (gearman_task_obj *)object;

	/* We don't call gearman_task_free here since the
	 * task object can still use them internally */
	/* XXX if (! (task->flags & GEARMAN_TASK_OBJ_DEAD)) */
	{
		GEARMAN_ZVAL_DONE(task->zgearman)
		GEARMAN_ZVAL_DONE(task->zclient)
	}
	zend_object_std_dtor(&(task->std) TSRMLS_CC);

	if (task->flags & GEARMAN_TASK_OBJ_CREATED) {
		task->flags |= GEARMAN_TASK_OBJ_DEAD;
	} else {
		GEARMAN_ZVAL_DONE(task->zworkload)
		GEARMAN_ZVAL_DONE(task->zdata)
		efree(object);
	}
}

static inline zend_object_value
gearman_task_obj_new_ex(zend_class_entry *class_type,
                        gearman_task_obj **gearman_task_ptr TSRMLS_DC) {
	gearman_task_obj *task;
	zval *tmp;

	task= emalloc(sizeof(gearman_task_obj));
	memset(task, 0, sizeof(gearman_task_obj));

	if (gearman_task_ptr) {
		*gearman_task_ptr= task;
	}

	zend_object_std_init(&(task->std), class_type TSRMLS_CC);
	zend_hash_copy(task->std.properties, 
	              &(class_type->default_properties),
	              (copy_ctor_func_t)zval_add_ref, (void *)(&tmp),
	              sizeof(zval *));

	task->value.handle= zend_objects_store_put(task,
	                (zend_objects_store_dtor_t)zend_objects_destroy_object,
	                (zend_objects_free_object_storage_t)gearman_task_obj_free,
	                 NULL TSRMLS_CC);

	task->value.handlers= &gearman_task_obj_handlers;

	return task->value;
}

static zend_object_value
gearman_task_obj_new(zend_class_entry *class_type TSRMLS_DC) {
	return gearman_task_obj_new_ex(class_type, NULL TSRMLS_CC);
}

/* Function list. */
zend_function_entry gearman_functions[] = {
	/* Functions from gearman.h */
	PHP_FE(gearman_version, NULL)
#if jluedke_0
	PHP_FE(gearman_create, NULL)
	PHP_FE(gearman_clone, NULL)
	PHP_FE(gearman_free, NULL)
	PHP_FE(gearman_error, NULL)
	PHP_FE(gearman_errno, NULL)
	PHP_FE(gearman_set_options, NULL)
#endif

	/* Functions from con.h */

	/* Functions from packet.h */

	/* Functions from task.h */
	PHP_FE(gearman_task_return_code, NULL)
	PHP_FE(gearman_task_create, NULL)
	PHP_FE(gearman_task_free, NULL)
	PHP_FE(gearman_task_fn_arg, NULL)
	PHP_FE(gearman_task_function, NULL)
	PHP_FE(gearman_task_uuid, NULL)
	PHP_FE(gearman_task_job_handle, NULL)
	PHP_FE(gearman_task_is_known, NULL)
	PHP_FE(gearman_task_is_running, NULL)
	PHP_FE(gearman_task_numerator, NULL)
	PHP_FE(gearman_task_denominator, NULL)
	PHP_FE(gearman_task_data, NULL)
	PHP_FE(gearman_task_data_size, NULL)
	PHP_FE(gearman_task_take_data, NULL)
	PHP_FE(gearman_task_send_data, NULL)
	PHP_FE(gearman_task_recv_data, NULL)

	/* Functions from job.h */
	PHP_FE(gearman_job_return_code, NULL)
	PHP_FE(gearman_job_create, NULL)
	PHP_FE(gearman_job_free, NULL)
	PHP_FE(gearman_job_data, NULL)
	PHP_FE(gearman_job_warning, NULL)
	PHP_FE(gearman_job_status, NULL)
	PHP_FE(gearman_job_complete, NULL)
	PHP_FE(gearman_job_exception, NULL)
	PHP_FE(gearman_job_fail, NULL)
	PHP_FE(gearman_job_handle, NULL)
	PHP_FE(gearman_job_unique, NULL)
	PHP_FE(gearman_job_function_name, NULL)
	PHP_FE(gearman_job_workload, NULL)
	PHP_FE(gearman_job_workload_size, NULL)

	/* Functions from client.h */
	PHP_FE(gearman_client_return_code, NULL)
	PHP_FE(gearman_client_create, NULL)
	PHP_FE(gearman_client_clone, NULL)
	PHP_FE(gearman_client_free, NULL)
	PHP_FE(gearman_client_error, NULL)
	PHP_FE(gearman_client_errno, NULL)
	PHP_FE(gearman_client_set_options, NULL)
	PHP_FE(gearman_client_add_server, NULL)
	PHP_FE(gearman_client_do, NULL)
	PHP_FE(gearman_client_do_high, NULL)
	PHP_FE(gearman_client_do_low, NULL)
	PHP_FE(gearman_client_do_job_handle, NULL)
	PHP_FE(gearman_client_do_status, NULL)
	PHP_FE(gearman_client_do_background, NULL)
	PHP_FE(gearman_client_do_high_background, NULL)
	PHP_FE(gearman_client_do_low_background, NULL)
	PHP_FE(gearman_client_job_status, NULL)
	PHP_FE(gearman_client_echo, NULL)
	PHP_FE(gearman_client_add_task, NULL)
	PHP_FE(gearman_client_add_task_high, NULL)
	PHP_FE(gearman_client_add_task_low, NULL)
	PHP_FE(gearman_client_add_task_background, NULL)
	PHP_FE(gearman_client_add_task_high_background, NULL)
	PHP_FE(gearman_client_add_task_low_background, NULL)
	PHP_FE(gearman_client_add_task_status, NULL)
	PHP_FE(gearman_client_set_workload_fn, NULL)
	PHP_FE(gearman_client_set_created_fn, NULL)
	PHP_FE(gearman_client_set_data_fn, NULL)
	PHP_FE(gearman_client_set_warning_fn, NULL)
	PHP_FE(gearman_client_set_status_fn, NULL)
	PHP_FE(gearman_client_set_complete_fn, NULL)
	PHP_FE(gearman_client_set_exception_fn, NULL)
	PHP_FE(gearman_client_set_fail_fn, NULL)
	PHP_FE(gearman_client_clear_fn, NULL)
	PHP_FE(gearman_client_data, NULL)
	PHP_FE(gearman_client_set_data, NULL)
	PHP_FE(gearman_client_run_tasks, NULL)

	/* Functions from worker.h */
	PHP_FE(gearman_worker_return_code, NULL)
	PHP_FE(gearman_worker_create, NULL)
	PHP_FE(gearman_worker_clone, NULL)
	PHP_FE(gearman_worker_free, NULL)
	PHP_FE(gearman_worker_error, NULL)
	PHP_FE(gearman_worker_errno, NULL)
	PHP_FE(gearman_worker_set_options, NULL)
	PHP_FE(gearman_worker_add_server, NULL)
	PHP_FE(gearman_worker_register, NULL)
	PHP_FE(gearman_worker_unregister, NULL)
	PHP_FE(gearman_worker_unregister_all, NULL)
	PHP_FE(gearman_worker_grab_job, NULL)
	PHP_FE(gearman_worker_add_function, NULL)
	PHP_FE(gearman_worker_work, NULL)
	PHP_FE(gearman_worker_echo, NULL)

	/* Functions from gearmand.h */
#if jluedke_0
	PHP_FE(gearmand_create, NULL)
	PHP_FE(gearmand_free, NULL)
	PHP_FE(gearmand_set_backlog, NULL)
	PHP_FE(gearmand_set_verbose, NULL)
	PHP_FE(gearmand_error, NULL)
	PHP_FE(gearmand_errno, NULL)
	PHP_FE(gearmand_run, NULL)
#endif

	{NULL, NULL, NULL} /* Must be the last line in gearman_functions[] */
};

zend_function_entry gearman_methods[]= {
/* Still need to finish this
	PHP_ME(gearman, __construct, NULL, ZEND_ACC_CTOR | ZEND_ACC_PUBLIC)
	__PHP_ME_MAPPING(clone, gearman_clone, NULL, 0)
	__PHP_ME_MAPPING(error, gearman_error, NULL, 0)
	__PHP_ME_MAPPING(errno, gearman_errno, NULL, 0)
	__PHP_ME_MAPPING(set_options, gearman_set_options, NULL, 0)
	__PHP_ME_MAPPING(job_create, gearman_job_create, NULL, 0)
*/
	{NULL, NULL, NULL}
};

zend_function_entry gearman_client_methods[]= {
	PHP_ME(gearman_client, __construct, NULL, ZEND_ACC_CTOR | ZEND_ACC_PUBLIC)
	__PHP_ME_MAPPING(return_code, gearman_client_return_code, NULL, 0)
	__PHP_ME_MAPPING(clone, gearman_client_clone, NULL, 0)
	__PHP_ME_MAPPING(error, gearman_client_error, NULL, 0)
	__PHP_ME_MAPPING(errno, gearman_client_errno, NULL, 0)
	__PHP_ME_MAPPING(set_options, gearman_client_set_options, NULL, 0)
	__PHP_ME_MAPPING(add_server, gearman_client_add_server, NULL, 0)
	__PHP_ME_MAPPING(do, gearman_client_do, NULL, 0)
	__PHP_ME_MAPPING(do_high, gearman_client_do_high, NULL, 0)
	__PHP_ME_MAPPING(do_low, gearman_client_do_low, NULL, 0)
	__PHP_ME_MAPPING(do_job_handle, gearman_client_do_job_handle, NULL, 0)
	__PHP_ME_MAPPING(do_status, gearman_client_do_status, NULL, 0)
	__PHP_ME_MAPPING(do_background, gearman_client_do_background, NULL, 0)
	__PHP_ME_MAPPING(do_high_background, gearman_client_do_high_background, 
	                 NULL, 0)
	__PHP_ME_MAPPING(do_low_background, gearman_client_do_low_background, 
	                 NULL, 0)
	__PHP_ME_MAPPING(job_status, gearman_client_job_status, NULL, 0)
	__PHP_ME_MAPPING(echo, gearman_client_echo, NULL, 0)
	__PHP_ME_MAPPING(add_task, gearman_client_add_task, NULL, 0)
	__PHP_ME_MAPPING(add_task_high, gearman_client_add_task_high, NULL, 0)
	__PHP_ME_MAPPING(add_task_low, gearman_client_add_task_low, NULL, 0)
	__PHP_ME_MAPPING(add_task_background, gearman_client_add_task_background, 
	                 NULL, 0)
	__PHP_ME_MAPPING(add_task_high_background,
	               gearman_client_add_task_high_background, NULL, 0)
	__PHP_ME_MAPPING(add_task_low_background,
	               gearman_client_add_task_low_background, NULL, 0)
	__PHP_ME_MAPPING(add_task_status, gearman_client_add_task_status, NULL, 0)
	__PHP_ME_MAPPING(set_workload_fn, gearman_client_set_workload_fn, NULL, 0)
	__PHP_ME_MAPPING(set_created_fn, gearman_client_set_created_fn, NULL, 0)
	__PHP_ME_MAPPING(set_data_fn, gearman_client_set_data_fn, NULL, 0)
	__PHP_ME_MAPPING(set_warning_fn, gearman_client_set_warning_fn, NULL, 0)
	__PHP_ME_MAPPING(set_status_fn, gearman_client_set_status_fn, NULL, 0)
	__PHP_ME_MAPPING(set_complete_fn, gearman_client_set_complete_fn, NULL, 0)
	__PHP_ME_MAPPING(set_exception_fn, gearman_client_set_exception_fn, NULL, 0)
	__PHP_ME_MAPPING(set_fail_fn, gearman_client_set_fail_fn, NULL, 0)
	__PHP_ME_MAPPING(clear_fn, gearman_client_clear_fn, NULL, 0)
	__PHP_ME_MAPPING(data, gearman_client_data, NULL, 0)
	__PHP_ME_MAPPING(set_data, gearman_client_set_data, NULL, 0)
	__PHP_ME_MAPPING(run_tasks, gearman_client_run_tasks, NULL, 0)
	{NULL, NULL, NULL}
};

zend_function_entry gearman_worker_methods[]= {
	PHP_ME(gearman_worker, __construct, NULL, ZEND_ACC_CTOR | ZEND_ACC_PUBLIC)
	__PHP_ME_MAPPING(return_code, gearman_worker_return_code, NULL, 0)
	__PHP_ME_MAPPING(clone, gearman_worker_clone, NULL, 0)
	__PHP_ME_MAPPING(error, gearman_worker_error, NULL, 0)
	__PHP_ME_MAPPING(errno, gearman_worker_errno, NULL, 0)
	__PHP_ME_MAPPING(set_options, gearman_worker_set_options, NULL, 0)
	__PHP_ME_MAPPING(add_server, gearman_worker_add_server, NULL, 0)
	__PHP_ME_MAPPING(add_function, gearman_worker_add_function, NULL, 0)
	__PHP_ME_MAPPING(work, gearman_worker_work, NULL, 0)
	{NULL, NULL, NULL}
};

zend_function_entry gearman_job_methods[]= {
	PHP_ME(gearman_job, __construct, NULL, ZEND_ACC_CTOR | ZEND_ACC_PUBLIC)
	__PHP_ME_MAPPING(return_code, gearman_job_return_code, NULL, 0)
	__PHP_ME_MAPPING(workload, gearman_job_workload, NULL, 0)
	__PHP_ME_MAPPING(workload_size, gearman_job_workload_size, NULL, 0)
	__PHP_ME_MAPPING(warning, gearman_job_warning, NULL, 0)
	__PHP_ME_MAPPING(status, gearman_job_status, NULL, 0)
	__PHP_ME_MAPPING(handle, gearman_job_handle, NULL, 0)
	__PHP_ME_MAPPING(unique, gearman_job_unique, NULL, 0)
	__PHP_ME_MAPPING(data, gearman_job_data, NULL, 0)
	__PHP_ME_MAPPING(complete, gearman_job_complete, NULL, 0)
	__PHP_ME_MAPPING(exception, gearman_job_exception, NULL, 0)
	__PHP_ME_MAPPING(fail, gearman_job_fail, NULL, 0)
	__PHP_ME_MAPPING(function_name, gearman_job_function_name, NULL, 0)
	__PHP_ME_MAPPING(set_return, gearman_job_set_return, NULL, 0)
	{NULL, NULL, NULL}
};

zend_function_entry gearman_task_methods[]= {
	PHP_ME(gearman_task, __construct, NULL, ZEND_ACC_CTOR | ZEND_ACC_PUBLIC)
	__PHP_ME_MAPPING(return_code, gearman_task_return_code, NULL, 0)
	__PHP_ME_MAPPING(create, gearman_task_create, NULL, 0)
	__PHP_ME_MAPPING(free, gearman_task_free, NULL, 0)
	__PHP_ME_MAPPING(fn_arg, gearman_task_fn_arg, NULL, 0)
	__PHP_ME_MAPPING(function, gearman_task_function, NULL, 0)
	__PHP_ME_MAPPING(uuid, gearman_task_uuid, NULL, 0)
	__PHP_ME_MAPPING(job_handle, gearman_task_job_handle, NULL, 0)
	__PHP_ME_MAPPING(is_known, gearman_task_is_known, NULL, 0)
	__PHP_ME_MAPPING(is_running, gearman_task_is_running, NULL, 0)
	__PHP_ME_MAPPING(task_numerator, gearman_task_numerator, NULL, 0)
	__PHP_ME_MAPPING(task_denominator, gearman_task_denominator, NULL, 0)
	__PHP_ME_MAPPING(data, gearman_task_data, NULL, 0)
	__PHP_ME_MAPPING(data_size, gearman_task_data_size, NULL, 0)
	__PHP_ME_MAPPING(take_data, gearman_task_take_data, NULL, 0)
	__PHP_ME_MAPPING(send_data, gearman_task_send_data, NULL, 0)
	__PHP_ME_MAPPING(recv_data, gearman_task_recv_data, NULL, 0)
	{NULL, NULL, NULL}
};

PHP_MINIT_FUNCTION(gearman) {
	zend_class_entry ce;

	/* gearman */
	/* Still need to finish this
	INIT_CLASS_ENTRY(ce, "gearman", gearman_methods);
	ce.create_object= gearman_obj_new;
	gearman_ce= zend_register_internal_class_ex(&ce, NULL, NULL TSRMLS_CC);
	*/
	memcpy(&gearman_obj_handlers, zend_get_std_object_handlers(),
	       sizeof(zend_object_handlers));
	gearman_obj_handlers.clone_obj= NULL; /* we have our own clone method */

	/* gearman_client */
	INIT_CLASS_ENTRY(ce, "gearman_client", gearman_client_methods);
	ce.create_object= gearman_client_obj_new;
	gearman_client_ce= zend_register_internal_class_ex(&ce, NULL, 
	                                                   NULL TSRMLS_CC);
	memcpy(&gearman_client_obj_handlers, zend_get_std_object_handlers(),
	       sizeof(zend_object_handlers));
	gearman_client_obj_handlers.clone_obj= NULL; /* we have our own clone method */

	/* gearman_worker */
	INIT_CLASS_ENTRY(ce, "gearman_worker", gearman_worker_methods);
	ce.create_object= gearman_worker_obj_new;
	gearman_worker_ce= zend_register_internal_class_ex(&ce, NULL, 
	                                                   NULL TSRMLS_CC);
	memcpy(&gearman_worker_obj_handlers, zend_get_std_object_handlers(),
	       sizeof(zend_object_handlers));
	gearman_worker_obj_handlers.clone_obj= NULL; /* we have our own clone method */

	/* gearman_job */
	INIT_CLASS_ENTRY(ce, "gearman_job", gearman_job_methods);
	ce.create_object= gearman_job_obj_new;
	gearman_job_ce= zend_register_internal_class_ex(&ce, NULL, NULL TSRMLS_CC);
	memcpy(&gearman_job_obj_handlers, zend_get_std_object_handlers(),
	       sizeof(zend_object_handlers));
	gearman_job_obj_handlers.clone_obj= NULL; /* we have our own clone method */

	/* gearman_task */
	INIT_CLASS_ENTRY(ce, "gearman_task", gearman_task_methods);
	ce.create_object= gearman_task_obj_new;
	gearman_task_ce= zend_register_internal_class_ex(&ce, NULL, NULL TSRMLS_CC);
	memcpy(&gearman_task_obj_handlers, zend_get_std_object_handlers(),
	       sizeof(zend_object_handlers));
	gearman_task_obj_handlers.clone_obj= NULL; /* we have our own clone method */


  /* These are automatically generated from gearman_constants.h using
     const_gen.sh. Do not remove the CONST_GEN_* comments, this is how the
     script locates the correct location to replace. */

  /* CONST_GEN_START */
  REGISTER_STRING_CONSTANT("GEARMAN_DEFAULT_TCP_HOST",
                         GEARMAN_DEFAULT_TCP_HOST,
                         CONST_CS | CONST_PERSISTENT);
  REGISTER_LONG_CONSTANT("GEARMAN_DEFAULT_TCP_PORT",
                         GEARMAN_DEFAULT_TCP_PORT,
                         CONST_CS | CONST_PERSISTENT);
  REGISTER_LONG_CONSTANT("GEARMAN_DEFAULT_SOCKET_TIMEOUT",
                         GEARMAN_DEFAULT_SOCKET_TIMEOUT,
                         CONST_CS | CONST_PERSISTENT);
  REGISTER_LONG_CONSTANT("GEARMAN_DEFAULT_SOCKET_SEND_SIZE",
                         GEARMAN_DEFAULT_SOCKET_SEND_SIZE,
                         CONST_CS | CONST_PERSISTENT);
  REGISTER_LONG_CONSTANT("GEARMAN_DEFAULT_SOCKET_RECV_SIZE",
                         GEARMAN_DEFAULT_SOCKET_RECV_SIZE,
                         CONST_CS | CONST_PERSISTENT);
  REGISTER_LONG_CONSTANT("GEARMAN_DEFAULT_BACKLOG",
                         GEARMAN_DEFAULT_BACKLOG,
                         CONST_CS | CONST_PERSISTENT);
  REGISTER_LONG_CONSTANT("GEARMAN_DEFAULT_MAX_QUEUE_SIZE",
                         GEARMAN_DEFAULT_MAX_QUEUE_SIZE,
                         CONST_CS | CONST_PERSISTENT);
  REGISTER_LONG_CONSTANT("GEARMAN_MAX_ERROR_SIZE",
                         GEARMAN_MAX_ERROR_SIZE,
                         CONST_CS | CONST_PERSISTENT);
  REGISTER_LONG_CONSTANT("GEARMAN_PACKET_HEADER_SIZE",
                         GEARMAN_PACKET_HEADER_SIZE,
                         CONST_CS | CONST_PERSISTENT);
  REGISTER_LONG_CONSTANT("GEARMAN_JOB_HANDLE_SIZE",
                         GEARMAN_JOB_HANDLE_SIZE,
                         CONST_CS | CONST_PERSISTENT);
  REGISTER_LONG_CONSTANT("GEARMAN_OPTION_SIZE",
                         GEARMAN_OPTION_SIZE,
                         CONST_CS | CONST_PERSISTENT);
  REGISTER_LONG_CONSTANT("GEARMAN_UNIQUE_SIZE",
                         GEARMAN_UNIQUE_SIZE,
                         CONST_CS | CONST_PERSISTENT);
  REGISTER_LONG_CONSTANT("GEARMAN_MAX_COMMAND_ARGS",
                         GEARMAN_MAX_COMMAND_ARGS,
                         CONST_CS | CONST_PERSISTENT);
  REGISTER_LONG_CONSTANT("GEARMAN_ARGS_BUFFER_SIZE",
                         GEARMAN_ARGS_BUFFER_SIZE,
                         CONST_CS | CONST_PERSISTENT);
  REGISTER_LONG_CONSTANT("GEARMAN_SEND_BUFFER_SIZE",
                         GEARMAN_SEND_BUFFER_SIZE,
                         CONST_CS | CONST_PERSISTENT);
  REGISTER_LONG_CONSTANT("GEARMAN_RECV_BUFFER_SIZE",
                         GEARMAN_RECV_BUFFER_SIZE,
                         CONST_CS | CONST_PERSISTENT);
  REGISTER_LONG_CONSTANT("GEARMAN_SERVER_CON_ID_SIZE",
                         GEARMAN_SERVER_CON_ID_SIZE,
                         CONST_CS | CONST_PERSISTENT);
  REGISTER_LONG_CONSTANT("GEARMAN_JOB_HASH_SIZE",
                         GEARMAN_JOB_HASH_SIZE,
                         CONST_CS | CONST_PERSISTENT);
  REGISTER_LONG_CONSTANT("GEARMAN_MAX_FREE_SERVER_CON",
                         GEARMAN_MAX_FREE_SERVER_CON,
                         CONST_CS | CONST_PERSISTENT);
  REGISTER_LONG_CONSTANT("GEARMAN_MAX_FREE_SERVER_PACKET",
                         GEARMAN_MAX_FREE_SERVER_PACKET,
                         CONST_CS | CONST_PERSISTENT);
  REGISTER_LONG_CONSTANT("GEARMAN_MAX_FREE_SERVER_JOB",
                         GEARMAN_MAX_FREE_SERVER_JOB,
                         CONST_CS | CONST_PERSISTENT);
  REGISTER_LONG_CONSTANT("GEARMAN_MAX_FREE_SERVER_CLIENT",
                         GEARMAN_MAX_FREE_SERVER_CLIENT,
                         CONST_CS | CONST_PERSISTENT);
  REGISTER_LONG_CONSTANT("GEARMAN_MAX_FREE_SERVER_WORKER",
                         GEARMAN_MAX_FREE_SERVER_WORKER,
                         CONST_CS | CONST_PERSISTENT);
  REGISTER_LONG_CONSTANT("GEARMAN_TEXT_RESPONSE_SIZE",
                         GEARMAN_TEXT_RESPONSE_SIZE,
                         CONST_CS | CONST_PERSISTENT);
  REGISTER_LONG_CONSTANT("GEARMAN_WORKER_WAIT_TIMEOUT",
                         GEARMAN_WORKER_WAIT_TIMEOUT,
                         CONST_CS | CONST_PERSISTENT);
  REGISTER_LONG_CONSTANT("GEARMAN_PIPE_BUFFER_SIZE",
                         GEARMAN_PIPE_BUFFER_SIZE,
                         CONST_CS | CONST_PERSISTENT);
  REGISTER_LONG_CONSTANT("GEARMAN_SUCCESS",
                         GEARMAN_SUCCESS,
                         CONST_CS | CONST_PERSISTENT);
  REGISTER_LONG_CONSTANT("GEARMAN_IO_WAIT",
                         GEARMAN_IO_WAIT,
                         CONST_CS | CONST_PERSISTENT);
  REGISTER_LONG_CONSTANT("GEARMAN_SHUTDOWN",
                         GEARMAN_SHUTDOWN,
                         CONST_CS | CONST_PERSISTENT);
  REGISTER_LONG_CONSTANT("GEARMAN_SHUTDOWN_GRACEFUL",
                         GEARMAN_SHUTDOWN_GRACEFUL,
                         CONST_CS | CONST_PERSISTENT);
  REGISTER_LONG_CONSTANT("GEARMAN_ERRNO",
                         GEARMAN_ERRNO,
                         CONST_CS | CONST_PERSISTENT);
  REGISTER_LONG_CONSTANT("GEARMAN_EVENT",
                         GEARMAN_EVENT,
                         CONST_CS | CONST_PERSISTENT);
  REGISTER_LONG_CONSTANT("GEARMAN_TOO_MANY_ARGS",
                         GEARMAN_TOO_MANY_ARGS,
                         CONST_CS | CONST_PERSISTENT);
  REGISTER_LONG_CONSTANT("GEARMAN_NO_ACTIVE_FDS",
                         GEARMAN_NO_ACTIVE_FDS,
                         CONST_CS | CONST_PERSISTENT);
  REGISTER_LONG_CONSTANT("GEARMAN_INVALID_MAGIC",
                         GEARMAN_INVALID_MAGIC,
                         CONST_CS | CONST_PERSISTENT);
  REGISTER_LONG_CONSTANT("GEARMAN_INVALID_COMMAND",
                         GEARMAN_INVALID_COMMAND,
                         CONST_CS | CONST_PERSISTENT);
  REGISTER_LONG_CONSTANT("GEARMAN_INVALID_PACKET",
                         GEARMAN_INVALID_PACKET,
                         CONST_CS | CONST_PERSISTENT);
  REGISTER_LONG_CONSTANT("GEARMAN_UNEXPECTED_PACKET",
                         GEARMAN_UNEXPECTED_PACKET,
                         CONST_CS | CONST_PERSISTENT);
  REGISTER_LONG_CONSTANT("GEARMAN_GETADDRINFO",
                         GEARMAN_GETADDRINFO,
                         CONST_CS | CONST_PERSISTENT);
  REGISTER_LONG_CONSTANT("GEARMAN_NO_SERVERS",
                         GEARMAN_NO_SERVERS,
                         CONST_CS | CONST_PERSISTENT);
  REGISTER_LONG_CONSTANT("GEARMAN_LOST_CONNECTION",
                         GEARMAN_LOST_CONNECTION,
                         CONST_CS | CONST_PERSISTENT);
  REGISTER_LONG_CONSTANT("GEARMAN_MEMORY_ALLOCATION_FAILURE",
                         GEARMAN_MEMORY_ALLOCATION_FAILURE,
                         CONST_CS | CONST_PERSISTENT);
  REGISTER_LONG_CONSTANT("GEARMAN_JOB_EXISTS",
                         GEARMAN_JOB_EXISTS,
                         CONST_CS | CONST_PERSISTENT);
  REGISTER_LONG_CONSTANT("GEARMAN_JOB_QUEUE_FULL",
                         GEARMAN_JOB_QUEUE_FULL,
                         CONST_CS | CONST_PERSISTENT);
  REGISTER_LONG_CONSTANT("GEARMAN_SERVER_ERROR",
                         GEARMAN_SERVER_ERROR,
                         CONST_CS | CONST_PERSISTENT);
  REGISTER_LONG_CONSTANT("GEARMAN_WORK_ERROR",
                         GEARMAN_WORK_ERROR,
                         CONST_CS | CONST_PERSISTENT);
  REGISTER_LONG_CONSTANT("GEARMAN_WORK_DATA",
                         GEARMAN_WORK_DATA,
                         CONST_CS | CONST_PERSISTENT);
  REGISTER_LONG_CONSTANT("GEARMAN_WORK_WARNING",
                         GEARMAN_WORK_WARNING,
                         CONST_CS | CONST_PERSISTENT);
  REGISTER_LONG_CONSTANT("GEARMAN_WORK_STATUS",
                         GEARMAN_WORK_STATUS,
                         CONST_CS | CONST_PERSISTENT);
  REGISTER_LONG_CONSTANT("GEARMAN_WORK_EXCEPTION",
                         GEARMAN_WORK_EXCEPTION,
                         CONST_CS | CONST_PERSISTENT);
  REGISTER_LONG_CONSTANT("GEARMAN_WORK_FAIL",
                         GEARMAN_WORK_FAIL,
                         CONST_CS | CONST_PERSISTENT);
  REGISTER_LONG_CONSTANT("GEARMAN_NOT_CONNECTED",
                         GEARMAN_NOT_CONNECTED,
                         CONST_CS | CONST_PERSISTENT);
  REGISTER_LONG_CONSTANT("GEARMAN_COULD_NOT_CONNECT",
                         GEARMAN_COULD_NOT_CONNECT,
                         CONST_CS | CONST_PERSISTENT);
  REGISTER_LONG_CONSTANT("GEARMAN_SEND_IN_PROGRESS",
                         GEARMAN_SEND_IN_PROGRESS,
                         CONST_CS | CONST_PERSISTENT);
  REGISTER_LONG_CONSTANT("GEARMAN_RECV_IN_PROGRESS",
                         GEARMAN_RECV_IN_PROGRESS,
                         CONST_CS | CONST_PERSISTENT);
  REGISTER_LONG_CONSTANT("GEARMAN_NOT_FLUSHING",
                         GEARMAN_NOT_FLUSHING,
                         CONST_CS | CONST_PERSISTENT);
  REGISTER_LONG_CONSTANT("GEARMAN_DATA_TOO_LARGE",
                         GEARMAN_DATA_TOO_LARGE,
                         CONST_CS | CONST_PERSISTENT);
  REGISTER_LONG_CONSTANT("GEARMAN_INVALID_FUNCTION_NAME",
                         GEARMAN_INVALID_FUNCTION_NAME,
                         CONST_CS | CONST_PERSISTENT);
  REGISTER_LONG_CONSTANT("GEARMAN_INVALID_WORKER_FUNCTION",
                         GEARMAN_INVALID_WORKER_FUNCTION,
                         CONST_CS | CONST_PERSISTENT);
  REGISTER_LONG_CONSTANT("GEARMAN_NO_REGISTERED_FUNCTIONS",
                         GEARMAN_NO_REGISTERED_FUNCTIONS,
                         CONST_CS | CONST_PERSISTENT);
  REGISTER_LONG_CONSTANT("GEARMAN_NO_JOBS",
                         GEARMAN_NO_JOBS,
                         CONST_CS | CONST_PERSISTENT);
  REGISTER_LONG_CONSTANT("GEARMAN_ECHO_DATA_CORRUPTION",
                         GEARMAN_ECHO_DATA_CORRUPTION,
                         CONST_CS | CONST_PERSISTENT);
  REGISTER_LONG_CONSTANT("GEARMAN_NEED_WORKLOAD_FN",
                         GEARMAN_NEED_WORKLOAD_FN,
                         CONST_CS | CONST_PERSISTENT);
  REGISTER_LONG_CONSTANT("GEARMAN_PAUSE",
                         GEARMAN_PAUSE,
                         CONST_CS | CONST_PERSISTENT);
  REGISTER_LONG_CONSTANT("GEARMAN_UNKNOWN_STATE",
                         GEARMAN_UNKNOWN_STATE,
                         CONST_CS | CONST_PERSISTENT);
  REGISTER_LONG_CONSTANT("GEARMAN_PTHREAD",
                         GEARMAN_PTHREAD,
                         CONST_CS | CONST_PERSISTENT);
  REGISTER_LONG_CONSTANT("GEARMAN_PIPE_EOF",
                         GEARMAN_PIPE_EOF,
                         CONST_CS | CONST_PERSISTENT);
  REGISTER_LONG_CONSTANT("GEARMAN_MAX_RETURN",
                         GEARMAN_MAX_RETURN,
                         CONST_CS | CONST_PERSISTENT);
  REGISTER_LONG_CONSTANT("GEARMAN_ALLOCATED",
                         GEARMAN_ALLOCATED,
                         CONST_CS | CONST_PERSISTENT);
  REGISTER_LONG_CONSTANT("GEARMAN_NON_BLOCKING",
                         GEARMAN_NON_BLOCKING,
                         CONST_CS | CONST_PERSISTENT);
  REGISTER_LONG_CONSTANT("GEARMAN_DONT_TRACK_PACKETS",
                         GEARMAN_DONT_TRACK_PACKETS,
                         CONST_CS | CONST_PERSISTENT);
  REGISTER_LONG_CONSTANT("GEARMAN_CON_ALLOCATED",
                         GEARMAN_CON_ALLOCATED,
                         CONST_CS | CONST_PERSISTENT);
  REGISTER_LONG_CONSTANT("GEARMAN_CON_READY",
                         GEARMAN_CON_READY,
                         CONST_CS | CONST_PERSISTENT);
  REGISTER_LONG_CONSTANT("GEARMAN_CON_PACKET_IN_USE",
                         GEARMAN_CON_PACKET_IN_USE,
                         CONST_CS | CONST_PERSISTENT);
  REGISTER_LONG_CONSTANT("GEARMAN_CON_EXTERNAL_FD",
                         GEARMAN_CON_EXTERNAL_FD,
                         CONST_CS | CONST_PERSISTENT);
  REGISTER_LONG_CONSTANT("GEARMAN_CON_STATE_ADDRINFO",
                         GEARMAN_CON_STATE_ADDRINFO,
                         CONST_CS | CONST_PERSISTENT);
  REGISTER_LONG_CONSTANT("GEARMAN_CON_STATE_CONNECT",
                         GEARMAN_CON_STATE_CONNECT,
                         CONST_CS | CONST_PERSISTENT);
  REGISTER_LONG_CONSTANT("GEARMAN_CON_STATE_CONNECTING",
                         GEARMAN_CON_STATE_CONNECTING,
                         CONST_CS | CONST_PERSISTENT);
  REGISTER_LONG_CONSTANT("GEARMAN_CON_STATE_CONNECTED",
                         GEARMAN_CON_STATE_CONNECTED,
                         CONST_CS | CONST_PERSISTENT);
  REGISTER_LONG_CONSTANT("GEARMAN_CON_SEND_STATE_NONE",
                         GEARMAN_CON_SEND_STATE_NONE,
                         CONST_CS | CONST_PERSISTENT);
  REGISTER_LONG_CONSTANT("GEARMAN_CON_SEND_STATE_PRE_FLUSH",
                         GEARMAN_CON_SEND_STATE_PRE_FLUSH,
                         CONST_CS | CONST_PERSISTENT);
  REGISTER_LONG_CONSTANT("GEARMAN_CON_SEND_STATE_FORCE_FLUSH",
                         GEARMAN_CON_SEND_STATE_FORCE_FLUSH,
                         CONST_CS | CONST_PERSISTENT);
  REGISTER_LONG_CONSTANT("GEARMAN_CON_SEND_STATE_FLUSH",
                         GEARMAN_CON_SEND_STATE_FLUSH,
                         CONST_CS | CONST_PERSISTENT);
  REGISTER_LONG_CONSTANT("GEARMAN_CON_SEND_STATE_FLUSH_DATA",
                         GEARMAN_CON_SEND_STATE_FLUSH_DATA,
                         CONST_CS | CONST_PERSISTENT);
  REGISTER_LONG_CONSTANT("GEARMAN_CON_RECV_STATE_NONE",
                         GEARMAN_CON_RECV_STATE_NONE,
                         CONST_CS | CONST_PERSISTENT);
  REGISTER_LONG_CONSTANT("GEARMAN_CON_RECV_STATE_READ",
                         GEARMAN_CON_RECV_STATE_READ,
                         CONST_CS | CONST_PERSISTENT);
  REGISTER_LONG_CONSTANT("GEARMAN_CON_RECV_STATE_READ_DATA",
                         GEARMAN_CON_RECV_STATE_READ_DATA,
                         CONST_CS | CONST_PERSISTENT);
  REGISTER_LONG_CONSTANT("GEARMAN_PACKET_ALLOCATED",
                         GEARMAN_PACKET_ALLOCATED,
                         CONST_CS | CONST_PERSISTENT);
  REGISTER_LONG_CONSTANT("GEARMAN_PACKET_COMPLETE",
                         GEARMAN_PACKET_COMPLETE,
                         CONST_CS | CONST_PERSISTENT);
  REGISTER_LONG_CONSTANT("GEARMAN_PACKET_FREE_DATA",
                         GEARMAN_PACKET_FREE_DATA,
                         CONST_CS | CONST_PERSISTENT);
  REGISTER_LONG_CONSTANT("GEARMAN_MAGIC_TEXT",
                         GEARMAN_MAGIC_TEXT,
                         CONST_CS | CONST_PERSISTENT);
  REGISTER_LONG_CONSTANT("GEARMAN_MAGIC_REQUEST",
                         GEARMAN_MAGIC_REQUEST,
                         CONST_CS | CONST_PERSISTENT);
  REGISTER_LONG_CONSTANT("GEARMAN_MAGIC_RESPONSE",
                         GEARMAN_MAGIC_RESPONSE,
                         CONST_CS | CONST_PERSISTENT);
  REGISTER_LONG_CONSTANT("GEARMAN_COMMAND_TEXT",
                         GEARMAN_COMMAND_TEXT,
                         CONST_CS | CONST_PERSISTENT);
  REGISTER_LONG_CONSTANT("GEARMAN_COMMAND_CAN_DO",
                         GEARMAN_COMMAND_CAN_DO,
                         CONST_CS | CONST_PERSISTENT);
  REGISTER_LONG_CONSTANT("GEARMAN_COMMAND_CANT_DO",
                         GEARMAN_COMMAND_CANT_DO,
                         CONST_CS | CONST_PERSISTENT);
  REGISTER_LONG_CONSTANT("GEARMAN_COMMAND_RESET_ABILITIES",
                         GEARMAN_COMMAND_RESET_ABILITIES,
                         CONST_CS | CONST_PERSISTENT);
  REGISTER_LONG_CONSTANT("GEARMAN_COMMAND_PRE_SLEEP",
                         GEARMAN_COMMAND_PRE_SLEEP,
                         CONST_CS | CONST_PERSISTENT);
  REGISTER_LONG_CONSTANT("GEARMAN_COMMAND_UNUSED",
                         GEARMAN_COMMAND_UNUSED,
                         CONST_CS | CONST_PERSISTENT);
  REGISTER_LONG_CONSTANT("GEARMAN_COMMAND_NOOP",
                         GEARMAN_COMMAND_NOOP,
                         CONST_CS | CONST_PERSISTENT);
  REGISTER_LONG_CONSTANT("GEARMAN_COMMAND_SUBMIT_JOB",
                         GEARMAN_COMMAND_SUBMIT_JOB,
                         CONST_CS | CONST_PERSISTENT);
  REGISTER_LONG_CONSTANT("GEARMAN_COMMAND_JOB_CREATED",
                         GEARMAN_COMMAND_JOB_CREATED,
                         CONST_CS | CONST_PERSISTENT);
  REGISTER_LONG_CONSTANT("GEARMAN_COMMAND_GRAB_JOB",
                         GEARMAN_COMMAND_GRAB_JOB,
                         CONST_CS | CONST_PERSISTENT);
  REGISTER_LONG_CONSTANT("GEARMAN_COMMAND_NO_JOB",
                         GEARMAN_COMMAND_NO_JOB,
                         CONST_CS | CONST_PERSISTENT);
  REGISTER_LONG_CONSTANT("GEARMAN_COMMAND_JOB_ASSIGN",
                         GEARMAN_COMMAND_JOB_ASSIGN,
                         CONST_CS | CONST_PERSISTENT);
  REGISTER_LONG_CONSTANT("GEARMAN_COMMAND_WORK_STATUS",
                         GEARMAN_COMMAND_WORK_STATUS,
                         CONST_CS | CONST_PERSISTENT);
  REGISTER_LONG_CONSTANT("GEARMAN_COMMAND_WORK_COMPLETE",
                         GEARMAN_COMMAND_WORK_COMPLETE,
                         CONST_CS | CONST_PERSISTENT);
  REGISTER_LONG_CONSTANT("GEARMAN_COMMAND_WORK_FAIL",
                         GEARMAN_COMMAND_WORK_FAIL,
                         CONST_CS | CONST_PERSISTENT);
  REGISTER_LONG_CONSTANT("GEARMAN_COMMAND_GET_STATUS",
                         GEARMAN_COMMAND_GET_STATUS,
                         CONST_CS | CONST_PERSISTENT);
  REGISTER_LONG_CONSTANT("GEARMAN_COMMAND_ECHO_REQ",
                         GEARMAN_COMMAND_ECHO_REQ,
                         CONST_CS | CONST_PERSISTENT);
  REGISTER_LONG_CONSTANT("GEARMAN_COMMAND_ECHO_RES",
                         GEARMAN_COMMAND_ECHO_RES,
                         CONST_CS | CONST_PERSISTENT);
  REGISTER_LONG_CONSTANT("GEARMAN_COMMAND_SUBMIT_JOB_BG",
                         GEARMAN_COMMAND_SUBMIT_JOB_BG,
                         CONST_CS | CONST_PERSISTENT);
  REGISTER_LONG_CONSTANT("GEARMAN_COMMAND_ERROR",
                         GEARMAN_COMMAND_ERROR,
                         CONST_CS | CONST_PERSISTENT);
  REGISTER_LONG_CONSTANT("GEARMAN_COMMAND_STATUS_RES",
                         GEARMAN_COMMAND_STATUS_RES,
                         CONST_CS | CONST_PERSISTENT);
  REGISTER_LONG_CONSTANT("GEARMAN_COMMAND_SUBMIT_JOB_HIGH",
                         GEARMAN_COMMAND_SUBMIT_JOB_HIGH,
                         CONST_CS | CONST_PERSISTENT);
  REGISTER_LONG_CONSTANT("GEARMAN_COMMAND_SET_CLIENT_ID",
                         GEARMAN_COMMAND_SET_CLIENT_ID,
                         CONST_CS | CONST_PERSISTENT);
  REGISTER_LONG_CONSTANT("GEARMAN_COMMAND_CAN_DO_TIMEOUT",
                         GEARMAN_COMMAND_CAN_DO_TIMEOUT,
                         CONST_CS | CONST_PERSISTENT);
  REGISTER_LONG_CONSTANT("GEARMAN_COMMAND_ALL_YOURS",
                         GEARMAN_COMMAND_ALL_YOURS,
                         CONST_CS | CONST_PERSISTENT);
  REGISTER_LONG_CONSTANT("GEARMAN_COMMAND_WORK_EXCEPTION",
                         GEARMAN_COMMAND_WORK_EXCEPTION,
                         CONST_CS | CONST_PERSISTENT);
  REGISTER_LONG_CONSTANT("GEARMAN_COMMAND_OPTION_REQ",
                         GEARMAN_COMMAND_OPTION_REQ,
                         CONST_CS | CONST_PERSISTENT);
  REGISTER_LONG_CONSTANT("GEARMAN_COMMAND_OPTION_RES",
                         GEARMAN_COMMAND_OPTION_RES,
                         CONST_CS | CONST_PERSISTENT);
  REGISTER_LONG_CONSTANT("GEARMAN_COMMAND_WORK_DATA",
                         GEARMAN_COMMAND_WORK_DATA,
                         CONST_CS | CONST_PERSISTENT);
  REGISTER_LONG_CONSTANT("GEARMAN_COMMAND_WORK_WARNING",
                         GEARMAN_COMMAND_WORK_WARNING,
                         CONST_CS | CONST_PERSISTENT);
  REGISTER_LONG_CONSTANT("GEARMAN_COMMAND_GRAB_JOB_UNIQ",
                         GEARMAN_COMMAND_GRAB_JOB_UNIQ,
                         CONST_CS | CONST_PERSISTENT);
  REGISTER_LONG_CONSTANT("GEARMAN_COMMAND_JOB_ASSIGN_UNIQ",
                         GEARMAN_COMMAND_JOB_ASSIGN_UNIQ,
                         CONST_CS | CONST_PERSISTENT);
  REGISTER_LONG_CONSTANT("GEARMAN_COMMAND_SUBMIT_JOB_HIGH_BG",
                         GEARMAN_COMMAND_SUBMIT_JOB_HIGH_BG,
                         CONST_CS | CONST_PERSISTENT);
  REGISTER_LONG_CONSTANT("GEARMAN_COMMAND_SUBMIT_JOB_LOW",
                         GEARMAN_COMMAND_SUBMIT_JOB_LOW,
                         CONST_CS | CONST_PERSISTENT);
  REGISTER_LONG_CONSTANT("GEARMAN_COMMAND_SUBMIT_JOB_LOW_BG",
                         GEARMAN_COMMAND_SUBMIT_JOB_LOW_BG,
                         CONST_CS | CONST_PERSISTENT);
  REGISTER_LONG_CONSTANT("GEARMAN_COMMAND_SUBMIT_JOB_SCHED",
                         GEARMAN_COMMAND_SUBMIT_JOB_SCHED,
                         CONST_CS | CONST_PERSISTENT);
  REGISTER_LONG_CONSTANT("GEARMAN_COMMAND_SUBMIT_JOB_EPOCH",
                         GEARMAN_COMMAND_SUBMIT_JOB_EPOCH,
                         CONST_CS | CONST_PERSISTENT);
  REGISTER_LONG_CONSTANT("GEARMAN_COMMAND_MAX",
                         GEARMAN_COMMAND_MAX,
                         CONST_CS | CONST_PERSISTENT);
  REGISTER_LONG_CONSTANT("GEARMAN_TASK_ALLOCATED",
                         GEARMAN_TASK_ALLOCATED,
                         CONST_CS | CONST_PERSISTENT);
  REGISTER_LONG_CONSTANT("GEARMAN_TASK_SEND_IN_USE",
                         GEARMAN_TASK_SEND_IN_USE,
                         CONST_CS | CONST_PERSISTENT);
  REGISTER_LONG_CONSTANT("GEARMAN_TASK_STATE_NEW",
                         GEARMAN_TASK_STATE_NEW,
                         CONST_CS | CONST_PERSISTENT);
  REGISTER_LONG_CONSTANT("GEARMAN_TASK_STATE_SUBMIT",
                         GEARMAN_TASK_STATE_SUBMIT,
                         CONST_CS | CONST_PERSISTENT);
  REGISTER_LONG_CONSTANT("GEARMAN_TASK_STATE_WORKLOAD",
                         GEARMAN_TASK_STATE_WORKLOAD,
                         CONST_CS | CONST_PERSISTENT);
  REGISTER_LONG_CONSTANT("GEARMAN_TASK_STATE_WORK",
                         GEARMAN_TASK_STATE_WORK,
                         CONST_CS | CONST_PERSISTENT);
  REGISTER_LONG_CONSTANT("GEARMAN_TASK_STATE_CREATED",
                         GEARMAN_TASK_STATE_CREATED,
                         CONST_CS | CONST_PERSISTENT);
  REGISTER_LONG_CONSTANT("GEARMAN_TASK_STATE_DATA",
                         GEARMAN_TASK_STATE_DATA,
                         CONST_CS | CONST_PERSISTENT);
  REGISTER_LONG_CONSTANT("GEARMAN_TASK_STATE_WARNING",
                         GEARMAN_TASK_STATE_WARNING,
                         CONST_CS | CONST_PERSISTENT);
  REGISTER_LONG_CONSTANT("GEARMAN_TASK_STATE_STATUS",
                         GEARMAN_TASK_STATE_STATUS,
                         CONST_CS | CONST_PERSISTENT);
  REGISTER_LONG_CONSTANT("GEARMAN_TASK_STATE_COMPLETE",
                         GEARMAN_TASK_STATE_COMPLETE,
                         CONST_CS | CONST_PERSISTENT);
  REGISTER_LONG_CONSTANT("GEARMAN_TASK_STATE_EXCEPTION",
                         GEARMAN_TASK_STATE_EXCEPTION,
                         CONST_CS | CONST_PERSISTENT);
  REGISTER_LONG_CONSTANT("GEARMAN_TASK_STATE_FAIL",
                         GEARMAN_TASK_STATE_FAIL,
                         CONST_CS | CONST_PERSISTENT);
  REGISTER_LONG_CONSTANT("GEARMAN_TASK_STATE_FINISHED",
                         GEARMAN_TASK_STATE_FINISHED,
                         CONST_CS | CONST_PERSISTENT);
  REGISTER_LONG_CONSTANT("GEARMAN_JOB_ALLOCATED",
                         GEARMAN_JOB_ALLOCATED,
                         CONST_CS | CONST_PERSISTENT);
  REGISTER_LONG_CONSTANT("GEARMAN_JOB_ASSIGNED_IN_USE",
                         GEARMAN_JOB_ASSIGNED_IN_USE,
                         CONST_CS | CONST_PERSISTENT);
  REGISTER_LONG_CONSTANT("GEARMAN_JOB_WORK_IN_USE",
                         GEARMAN_JOB_WORK_IN_USE,
                         CONST_CS | CONST_PERSISTENT);
  REGISTER_LONG_CONSTANT("GEARMAN_JOB_PRIORITY_HIGH",
                         GEARMAN_JOB_PRIORITY_HIGH,
                         CONST_CS | CONST_PERSISTENT);
  REGISTER_LONG_CONSTANT("GEARMAN_JOB_PRIORITY_NORMAL",
                         GEARMAN_JOB_PRIORITY_NORMAL,
                         CONST_CS | CONST_PERSISTENT);
  REGISTER_LONG_CONSTANT("GEARMAN_JOB_PRIORITY_LOW",
                         GEARMAN_JOB_PRIORITY_LOW,
                         CONST_CS | CONST_PERSISTENT);
  REGISTER_LONG_CONSTANT("GEARMAN_JOB_PRIORITY_MAX",
                         GEARMAN_JOB_PRIORITY_MAX,
                         CONST_CS | CONST_PERSISTENT);
  REGISTER_LONG_CONSTANT("GEARMAN_CLIENT_ALLOCATED",
                         GEARMAN_CLIENT_ALLOCATED,
                         CONST_CS | CONST_PERSISTENT);
  REGISTER_LONG_CONSTANT("GEARMAN_CLIENT_NON_BLOCKING",
                         GEARMAN_CLIENT_NON_BLOCKING,
                         CONST_CS | CONST_PERSISTENT);
  REGISTER_LONG_CONSTANT("GEARMAN_CLIENT_TASK_IN_USE",
                         GEARMAN_CLIENT_TASK_IN_USE,
                         CONST_CS | CONST_PERSISTENT);
  REGISTER_LONG_CONSTANT("GEARMAN_CLIENT_UNBUFFERED_RESULT",
                         GEARMAN_CLIENT_UNBUFFERED_RESULT,
                         CONST_CS | CONST_PERSISTENT);
  REGISTER_LONG_CONSTANT("GEARMAN_CLIENT_NO_NEW",
                         GEARMAN_CLIENT_NO_NEW,
                         CONST_CS | CONST_PERSISTENT);
  REGISTER_LONG_CONSTANT("GEARMAN_CLIENT_STATE_IDLE",
                         GEARMAN_CLIENT_STATE_IDLE,
                         CONST_CS | CONST_PERSISTENT);
  REGISTER_LONG_CONSTANT("GEARMAN_CLIENT_STATE_NEW",
                         GEARMAN_CLIENT_STATE_NEW,
                         CONST_CS | CONST_PERSISTENT);
  REGISTER_LONG_CONSTANT("GEARMAN_CLIENT_STATE_SUBMIT",
                         GEARMAN_CLIENT_STATE_SUBMIT,
                         CONST_CS | CONST_PERSISTENT);
  REGISTER_LONG_CONSTANT("GEARMAN_CLIENT_STATE_PACKET",
                         GEARMAN_CLIENT_STATE_PACKET,
                         CONST_CS | CONST_PERSISTENT);
  REGISTER_LONG_CONSTANT("GEARMAN_WORKER_ALLOCATED",
                         GEARMAN_WORKER_ALLOCATED,
                         CONST_CS | CONST_PERSISTENT);
  REGISTER_LONG_CONSTANT("GEARMAN_WORKER_NON_BLOCKING",
                         GEARMAN_WORKER_NON_BLOCKING,
                         CONST_CS | CONST_PERSISTENT);
  REGISTER_LONG_CONSTANT("GEARMAN_WORKER_PACKET_INIT",
                         GEARMAN_WORKER_PACKET_INIT,
                         CONST_CS | CONST_PERSISTENT);
  REGISTER_LONG_CONSTANT("GEARMAN_WORKER_GRAB_JOB_IN_USE",
                         GEARMAN_WORKER_GRAB_JOB_IN_USE,
                         CONST_CS | CONST_PERSISTENT);
  REGISTER_LONG_CONSTANT("GEARMAN_WORKER_PRE_SLEEP_IN_USE",
                         GEARMAN_WORKER_PRE_SLEEP_IN_USE,
                         CONST_CS | CONST_PERSISTENT);
  REGISTER_LONG_CONSTANT("GEARMAN_WORKER_WORK_JOB_IN_USE",
                         GEARMAN_WORKER_WORK_JOB_IN_USE,
                         CONST_CS | CONST_PERSISTENT);
  REGISTER_LONG_CONSTANT("GEARMAN_WORKER_CHANGE",
                         GEARMAN_WORKER_CHANGE,
                         CONST_CS | CONST_PERSISTENT);
  REGISTER_LONG_CONSTANT("GEARMAN_WORKER_GRAB_UNIQ",
                         GEARMAN_WORKER_GRAB_UNIQ,
                         CONST_CS | CONST_PERSISTENT);
  REGISTER_LONG_CONSTANT("GEARMAN_WORKER_STATE_START",
                         GEARMAN_WORKER_STATE_START,
                         CONST_CS | CONST_PERSISTENT);
  REGISTER_LONG_CONSTANT("GEARMAN_WORKER_STATE_FUNCTION_SEND",
                         GEARMAN_WORKER_STATE_FUNCTION_SEND,
                         CONST_CS | CONST_PERSISTENT);
  REGISTER_LONG_CONSTANT("GEARMAN_WORKER_STATE_CONNECT",
                         GEARMAN_WORKER_STATE_CONNECT,
                         CONST_CS | CONST_PERSISTENT);
  REGISTER_LONG_CONSTANT("GEARMAN_WORKER_STATE_GRAB_JOB_SEND",
                         GEARMAN_WORKER_STATE_GRAB_JOB_SEND,
                         CONST_CS | CONST_PERSISTENT);
  REGISTER_LONG_CONSTANT("GEARMAN_WORKER_STATE_GRAB_JOB_RECV",
                         GEARMAN_WORKER_STATE_GRAB_JOB_RECV,
                         CONST_CS | CONST_PERSISTENT);
  REGISTER_LONG_CONSTANT("GEARMAN_WORKER_STATE_PRE_SLEEP",
                         GEARMAN_WORKER_STATE_PRE_SLEEP,
                         CONST_CS | CONST_PERSISTENT);
  REGISTER_LONG_CONSTANT("GEARMAN_WORKER_FUNCTION_PACKET_IN_USE",
                         GEARMAN_WORKER_FUNCTION_PACKET_IN_USE,
                         CONST_CS | CONST_PERSISTENT);
  REGISTER_LONG_CONSTANT("GEARMAN_WORKER_FUNCTION_CHANGE",
                         GEARMAN_WORKER_FUNCTION_CHANGE,
                         CONST_CS | CONST_PERSISTENT);
  REGISTER_LONG_CONSTANT("GEARMAN_WORKER_FUNCTION_REMOVE",
                         GEARMAN_WORKER_FUNCTION_REMOVE,
                         CONST_CS | CONST_PERSISTENT);
  REGISTER_LONG_CONSTANT("GEARMAN_WORKER_WORK_STATE_GRAB_JOB",
                         GEARMAN_WORKER_WORK_STATE_GRAB_JOB,
                         CONST_CS | CONST_PERSISTENT);
  REGISTER_LONG_CONSTANT("GEARMAN_WORKER_WORK_STATE_FUNCTION",
                         GEARMAN_WORKER_WORK_STATE_FUNCTION,
                         CONST_CS | CONST_PERSISTENT);
  REGISTER_LONG_CONSTANT("GEARMAN_WORKER_WORK_STATE_COMPLETE",
                         GEARMAN_WORKER_WORK_STATE_COMPLETE,
                         CONST_CS | CONST_PERSISTENT);
  REGISTER_LONG_CONSTANT("GEARMAN_WORKER_WORK_STATE_FAIL",
                         GEARMAN_WORKER_WORK_STATE_FAIL,
                         CONST_CS | CONST_PERSISTENT);
  REGISTER_LONG_CONSTANT("GEARMAN_SERVER_ALLOCATED",
                         GEARMAN_SERVER_ALLOCATED,
                         CONST_CS | CONST_PERSISTENT);
  REGISTER_LONG_CONSTANT("GEARMAN_SERVER_PROC_THREAD",
                         GEARMAN_SERVER_PROC_THREAD,
                         CONST_CS | CONST_PERSISTENT);
  REGISTER_LONG_CONSTANT("GEARMAN_SERVER_THREAD_ALLOCATED",
                         GEARMAN_SERVER_THREAD_ALLOCATED,
                         CONST_CS | CONST_PERSISTENT);
  REGISTER_LONG_CONSTANT("GEARMAN_SERVER_CON_SLEEPING",
                         GEARMAN_SERVER_CON_SLEEPING,
                         CONST_CS | CONST_PERSISTENT);
  REGISTER_LONG_CONSTANT("GEARMAN_SERVER_CON_EXCEPTIONS",
                         GEARMAN_SERVER_CON_EXCEPTIONS,
                         CONST_CS | CONST_PERSISTENT);
  REGISTER_LONG_CONSTANT("GEARMAN_SERVER_CON_DEAD",
                         GEARMAN_SERVER_CON_DEAD,
                         CONST_CS | CONST_PERSISTENT);
  REGISTER_LONG_CONSTANT("GEARMAN_SERVER_FUNCTION_ALLOCATED",
                         GEARMAN_SERVER_FUNCTION_ALLOCATED,
                         CONST_CS | CONST_PERSISTENT);
  REGISTER_LONG_CONSTANT("GEARMAN_SERVER_CLIENT_ALLOCATED",
                         GEARMAN_SERVER_CLIENT_ALLOCATED,
                         CONST_CS | CONST_PERSISTENT);
  REGISTER_LONG_CONSTANT("GEARMAN_SERVER_WORKER_ALLOCATED",
                         GEARMAN_SERVER_WORKER_ALLOCATED,
                         CONST_CS | CONST_PERSISTENT);
  REGISTER_LONG_CONSTANT("GEARMAN_SERVER_JOB_ALLOCATED",
                         GEARMAN_SERVER_JOB_ALLOCATED,
                         CONST_CS | CONST_PERSISTENT);
  REGISTER_LONG_CONSTANT("GEARMAND_LISTEN_EVENT",
                         GEARMAND_LISTEN_EVENT,
                         CONST_CS | CONST_PERSISTENT);
  REGISTER_LONG_CONSTANT("GEARMAND_WAKEUP_EVENT",
                         GEARMAND_WAKEUP_EVENT,
                         CONST_CS | CONST_PERSISTENT);
  REGISTER_LONG_CONSTANT("GEARMAND_WAKEUP_PAUSE",
                         GEARMAND_WAKEUP_PAUSE,
                         CONST_CS | CONST_PERSISTENT);
  REGISTER_LONG_CONSTANT("GEARMAND_WAKEUP_SHUTDOWN",
                         GEARMAND_WAKEUP_SHUTDOWN,
                         CONST_CS | CONST_PERSISTENT);
  REGISTER_LONG_CONSTANT("GEARMAND_WAKEUP_SHUTDOWN_GRACEFUL",
                         GEARMAND_WAKEUP_SHUTDOWN_GRACEFUL,
                         CONST_CS | CONST_PERSISTENT);
  REGISTER_LONG_CONSTANT("GEARMAND_WAKEUP_CON",
                         GEARMAND_WAKEUP_CON,
                         CONST_CS | CONST_PERSISTENT);
  REGISTER_LONG_CONSTANT("GEARMAND_WAKEUP_RUN",
                         GEARMAND_WAKEUP_RUN,
                         CONST_CS | CONST_PERSISTENT);
  REGISTER_LONG_CONSTANT("GEARMAND_THREAD_WAKEUP_EVENT",
                         GEARMAND_THREAD_WAKEUP_EVENT,
                         CONST_CS | CONST_PERSISTENT);
  REGISTER_LONG_CONSTANT("GEARMAND_THREAD_LOCK",
                         GEARMAND_THREAD_LOCK,
                         CONST_CS | CONST_PERSISTENT);
  /* CONST_GEN_STOP */

	return SUCCESS;
}

PHP_MSHUTDOWN_FUNCTION(gearman) {
	return SUCCESS;
}

PHP_RINIT_FUNCTION(gearman) {
	return SUCCESS;
}

PHP_RSHUTDOWN_FUNCTION(gearman) {
	return SUCCESS;
}

PHP_MINFO_FUNCTION(gearman) {
	char port_str[6];

	php_info_print_table_start();
	php_info_print_table_header(2, "gearman support", "enabled");
	php_info_print_table_row(2, "extension version", PHP_GEARMAN_VERSION);
	php_info_print_table_row(2, "libgearman version", gearman_version());
	php_info_print_table_row(2, "Default TCP Host", GEARMAN_DEFAULT_TCP_HOST);
	snprintf(port_str, 6, "%u", GEARMAN_DEFAULT_TCP_PORT);
	php_info_print_table_row(2, "Default TCP Port", port_str);
	php_info_print_table_end();
}

/* Module config struct. */
zend_module_entry gearman_module_entry = {
#if ZEND_MODULE_API_NO >= 20010901
	STANDARD_MODULE_HEADER,
#endif
	"gearman",
	gearman_functions,
	PHP_MINIT(gearman),
	PHP_MSHUTDOWN(gearman),
	PHP_RINIT(gearman),
	PHP_RSHUTDOWN(gearman),
	PHP_MINFO(gearman),
#if ZEND_MODULE_API_NO >= 20010901
	"0.1",
#endif
	STANDARD_MODULE_PROPERTIES
};

#ifdef COMPILE_DL_GEARMAN
ZEND_GET_MODULE(gearman)
#endif