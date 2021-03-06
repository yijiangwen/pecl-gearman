/*
 * Gearman PHP Extension
 *
 * Copyright (C) 2008 James M. Luedke <contact@jamesluedke.com>,
 *			Eric Day <eday@oddments.org>
 * All rights reserved.
 *
 * Use and distribution licensed under the PHP license.  See
 * the LICENSE file in this directory for full text.
 */

#include "php_gearman_task.h"
#include "php_gearman_client.h"

inline gearman_client_obj *gearman_client_fetch_object(zend_object *obj) {
       return (gearman_client_obj *)((char*)(obj) - XtOffsetOf(gearman_client_obj, std));
}

static void gearman_client_ctor(INTERNAL_FUNCTION_PARAMETERS) {
        gearman_client_obj *client;

        if (zend_parse_parameters_none() == FAILURE) {
                return;
        }

        client = Z_GEARMAN_CLIENT_P(return_value);

        if (gearman_client_create(&(client->client)) == NULL) {
                GEARMAN_EXCEPTION("Memory allocation failure", 0);
        }    

        client->flags |= GEARMAN_CLIENT_OBJ_CREATED;
        gearman_client_add_options(&(client->client), GEARMAN_CLIENT_FREE_TASKS);
        gearman_client_set_workload_malloc_fn(&(client->client), _php_malloc, NULL);
        gearman_client_set_workload_free_fn(&(client->client), _php_free, NULL);
        gearman_client_set_task_context_free_fn(&(client->client), _php_task_free);
}

/* {{{ proto object gearman_client_create()
   Returns a GearmanClient object */
PHP_FUNCTION(gearman_client_create) {
        if (object_init_ex(return_value, gearman_client_ce) != SUCCESS) {
                php_error_docref(NULL, E_WARNING, "Object creation failure.");
                RETURN_FALSE;
        }    

        gearman_client_ctor(INTERNAL_FUNCTION_PARAM_PASSTHRU);
}

inline zend_object *gearman_client_obj_new(zend_class_entry *ce) {
	gearman_client_obj *intern = ecalloc(1,
		sizeof(gearman_client_obj) +
		zend_object_properties_size(ce));

	zend_object_std_init(&(intern->std), ce);
	object_properties_init(&intern->std, ce);
	array_init(&intern->task_list);
	intern->created_tasks = 0;

	intern->std.handlers = &gearman_client_obj_handlers;
	return &intern->std;
}

/* {{{ proto object GearmanClient::__construct()
   Returns a GearmanClient object */
PHP_METHOD(GearmanClient, __construct)
{
        return_value = getThis();
        gearman_client_ctor(INTERNAL_FUNCTION_PARAM_PASSTHRU);

}
/* }}} */

/* {{{ proto object GearmanClient::__destruct()
   cleans up GearmanClient object */
PHP_METHOD(GearmanClient, __destruct)
{
        char *context = NULL;
        gearman_client_obj *intern = Z_GEARMAN_CLIENT_P(getThis());
        if (!intern) {
                return;
        }

        context = gearman_client_context(&(intern->client));
        efree(context);

        if (intern->flags & GEARMAN_CLIENT_OBJ_CREATED) {
                gearman_client_free(&intern->client);
        }

        // Clear Callbacks
        zval_dtor(&intern->zworkload_fn);
        zval_dtor(&intern->zcreated_fn);
        zval_dtor(&intern->zdata_fn);
        zval_dtor(&intern->zwarning_fn);
        zval_dtor(&intern->zstatus_fn);
        zval_dtor(&intern->zcomplete_fn);
        zval_dtor(&intern->zexception_fn);
        zval_dtor(&intern->zfail_fn);

        zval_dtor(&intern->task_list);

        zend_object_std_dtor(&intern->std);
}

/* {{{ proto int gearman_client_return_code()
   get last gearman_return_t */
PHP_FUNCTION(gearman_client_return_code)
{
        gearman_client_obj *obj;
        zval *zobj;

        if (zend_parse_method_parameters(ZEND_NUM_ARGS(), getThis(), "O", &zobj, gearman_client_ce) == FAILURE) {
                RETURN_NULL();
        }    
        obj = Z_GEARMAN_CLIENT_P(zobj);

        RETURN_LONG(obj->ret);
}
/* }}} */

/* {{{ proto string gearman_client_error()
   Return an error string for the last error encountered. */
PHP_FUNCTION(gearman_client_error) {
        char *error = NULL;
        gearman_client_obj *obj;
        zval *zobj;

        if (zend_parse_method_parameters(ZEND_NUM_ARGS(), getThis(), "O", &zobj, gearman_client_ce) == FAILURE) {
                RETURN_NULL();
        }    
        obj = Z_GEARMAN_CLIENT_P(zobj);

        error = (char *)gearman_client_error(&(obj->client));
        if (error) {
                RETURN_STRING(error)
        }    
        RETURN_FALSE;
}
/* }}} */

/* {{{ proto int gearman_client_get_errno()
   Value of errno in the case of a GEARMAN_ERRNO return value. */
PHP_FUNCTION(gearman_client_get_errno) {
        gearman_client_obj *obj;
        zval *zobj;

        if (zend_parse_method_parameters(ZEND_NUM_ARGS(), getThis(), "O", &zobj, gearman_client_ce) == FAILURE) {
                RETURN_NULL();
        }    
        obj = Z_GEARMAN_CLIENT_P(zobj);

        RETURN_LONG(gearman_client_errno(&(obj->client)))
}
/* }}} */

/* {{{ proto int gearman_client_options()
   Get options for a client structure. */
PHP_FUNCTION(gearman_client_options) {
        gearman_client_obj *obj;
        zval *zobj;

        if (zend_parse_method_parameters(ZEND_NUM_ARGS(), getThis(), "O", &zobj, gearman_client_ce) == FAILURE) {
                RETURN_NULL();
        }    
        obj = Z_GEARMAN_CLIENT_P(zobj);

        RETURN_LONG(gearman_client_options(&(obj->client)))
}
/* }}} */

/* {{{ proto void gearman_client_set_options(constant option)
   Set options for a client structure. */
PHP_FUNCTION(gearman_client_set_options) {
        zend_long options;

        gearman_client_obj *obj;
        zval *zobj;

        if (zend_parse_method_parameters(ZEND_NUM_ARGS(), getThis(), "Ol", &zobj, gearman_client_ce, &options) == FAILURE) {
                RETURN_FALSE;
        }    
        obj = Z_GEARMAN_CLIENT_P(zobj);

        gearman_client_set_options(&(obj->client), options);
        RETURN_TRUE;
}
/* }}} */

/* {{{ proto void GearmanClient::addOptions(constant option)
   Set options for a client structure. */
PHP_FUNCTION(gearman_client_add_options) {
        zend_long options;

        gearman_client_obj *obj;
        zval *zobj;

        if (zend_parse_method_parameters(ZEND_NUM_ARGS(), getThis(), "Ol", &zobj, gearman_client_ce, &options) == FAILURE) {
                RETURN_FALSE;
        }
        obj = Z_GEARMAN_CLIENT_P(zobj);

        gearman_client_add_options(&(obj->client), options);
        RETURN_TRUE;
}
/* }}} */

/* {{{ proto void GearmanClient::removeOptions(constant option)
   Set options for a client structure. */
PHP_FUNCTION(gearman_client_remove_options) {
        zend_long options;

        gearman_client_obj *obj;
        zval *zobj;

        if (zend_parse_method_parameters(ZEND_NUM_ARGS(), getThis(), "Ol", &zobj, gearman_client_ce, &options) == FAILURE) {
                RETURN_FALSE;
        }
        obj = Z_GEARMAN_CLIENT_P(zobj);

        gearman_client_remove_options(&(obj->client), options);
        RETURN_TRUE;
}
/* }}} */

PHP_FUNCTION(gearman_client_timeout) {
        gearman_client_obj *obj;
        zval *zobj;

        if (zend_parse_method_parameters(ZEND_NUM_ARGS(), getThis(), "O", &zobj, gearman_client_ce) == FAILURE) {
                RETURN_NULL();
        }    
        obj = Z_GEARMAN_CLIENT_P(zobj);

        RETURN_LONG(gearman_client_timeout(&(obj->client)))
}
/* }}} */

/* {{{ proto void gearman_client_set_timeout(object, constant timeout)
   Set timeout for a client structure. */
PHP_FUNCTION(gearman_client_set_timeout) {
        zend_long timeout;

        gearman_client_obj *obj;
        zval *zobj;

        if (zend_parse_method_parameters(ZEND_NUM_ARGS(), getThis(), "Ol", &zobj, gearman_client_ce, &timeout) == FAILURE) {
                RETURN_FALSE;
        }    
        obj = Z_GEARMAN_CLIENT_P(zobj);

        gearman_client_set_timeout(&(obj->client), timeout);
        RETURN_TRUE;
}
/* }}} */

/* {{{ proto bool gearman_client_add_server(object client [, string host [, int port]])
   Add a job server to a client. This goes into a list of servers than can be used to run tasks. No socket I/O happens here, it is just added to a list. */
PHP_FUNCTION(gearman_client_add_server) {
        char *host = NULL;
        size_t host_len = 0;
        zend_long port = 0;

        gearman_client_obj *obj;
        zval *zobj;

        if (zend_parse_method_parameters(ZEND_NUM_ARGS(), getThis(), "O|sl", &zobj, gearman_client_ce, &host, &host_len, &port) == FAILURE) {
                RETURN_FALSE;
        }            
        obj = Z_GEARMAN_CLIENT_P(zobj);

        obj->ret = gearman_client_add_server(&(obj->client), host, port);
        if (obj->ret != GEARMAN_SUCCESS) {
                php_error_docref(NULL, E_WARNING, "%s",
                                                 gearman_client_error(&(obj->client)));
                RETURN_FALSE;                         
        }            

        if (!gearman_client_set_server_option(&(obj->client), "exceptions", (sizeof("exceptions") - 1))) {
                GEARMAN_EXCEPTION("Failed to set exception option", 0);
        }            

        RETURN_TRUE;
}
/* }}} */

/* {{{ proto bool gearman_client_add_servers(object client [, string servers])
   Add a list of job servers to a client. This goes into a list of servers that can be used to run tasks. No socket I/O happens here, it is just added to a list. */
PHP_FUNCTION(gearman_client_add_servers) {
        char *servers = NULL;
        size_t servers_len = 0;

        gearman_client_obj *obj;
        zval *zobj;

        if (zend_parse_method_parameters(ZEND_NUM_ARGS(), getThis(), "O|s", &zobj, gearman_client_ce, &servers, &servers_len) == FAILURE) {
                RETURN_FALSE;
        }
        obj = Z_GEARMAN_CLIENT_P(zobj);

        obj->ret = gearman_client_add_servers(&(obj->client), servers);
        if (obj->ret != GEARMAN_SUCCESS) {
                php_error_docref(NULL, E_WARNING, "%s",
                                                 gearman_client_error(&(obj->client)));
                RETURN_FALSE;
        }

        if (!gearman_client_set_server_option(&(obj->client), "exceptions", (sizeof("exceptions") - 1))) {
                GEARMAN_EXCEPTION("Failed to set exception option", 0);
        }

        RETURN_TRUE;
}
/* }}} */
