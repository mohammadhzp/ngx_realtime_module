/**
 * Copyright (c) 2026 mohammadhzp
 * SPDX-License-Identifier: MIT
 * Project: https://github.com/mohammadhzp
 * Date: 6/8/20
 *
 */

#include <ngx_config.h>
#include <ngx_core.h>
#include <ngx_http.h>


#define NGX_HTTP_REALTIME_SYSLOG_MAX_STR                                     \
    NGX_MAX_ERROR_STR + sizeof("<255>Jan 01 00:00:00 ") - 1                  \
    + (NGX_MAXHOSTNAMELEN - 1) + 1 /* space */                               \
    + 32 /* tag */ + 2 /* colon, space */


ngx_module_t  ngx_http_realtime_module;


typedef struct {
    ngx_flag_t            configured;
    ngx_uint_t            failed;
    ngx_int_t             queue_len;
    ngx_queue_t           queue;
    ngx_syslog_peer_t     *peer;
} ngx_http_realtime_syslog_t;


typedef struct {
    ngx_str_t    line;
    ngx_uint_t   log_level;
    ngx_queue_t  queue;
} ngx_http_realtime_syslog_queue_t;


typedef struct {
    ngx_http_complex_value_t    *realtime_format;
    ngx_str_t                    realtime_log_level_name;
    ngx_uint_t                   realtime_log_level;
    ngx_flag_t                   realtime_monitor;
    ngx_flag_t                   realtime_syslog;
    ngx_flag_t                   realtime_error_log;
    ngx_msec_t                   realtime_interval;
    ngx_flag_t                   realtime_error_log_use_cycle;
} ngx_http_realtime_loc_conf_t;


typedef struct {
    ngx_flag_t                     realtime_enabled;
    ngx_flag_t                     realtime_monitor_from_content_phase;
    ngx_uint_t                     realtime_skip_failed_after;
    ngx_uint_t                     realtime_syslog_send_batch_size;
    ngx_msec_t                     realtime_syslog_queue_interval;
    ngx_http_realtime_syslog_t    *syslog;
} ngx_http_realtime_main_conf_t;

typedef struct {
    ngx_event_t    *ev;
    off_t           last_sent;
    time_t          last_sec;
    ngx_uint_t      last_msec;
    ngx_flag_t      completed;
    ngx_flag_t      fresh;
    ngx_flag_t      initialized;

} ngx_http_realtime_ctx_t;


/*
 * Variables
 */


static ngx_int_t ngx_http_realtime_body_bytes_sent_variable(ngx_http_request_t *r, ngx_http_variable_value_t *v, uintptr_t data) {
    ngx_http_realtime_ctx_t        *ctx;
    ngx_http_realtime_loc_conf_t   *loc_conf;
    u_char                         *p;
    off_t                           sent;

    loc_conf = ngx_http_get_module_loc_conf(r, ngx_http_realtime_module);
    if (loc_conf->realtime_monitor != 1) {
        v->not_found = 1;
        return NGX_OK;
    }

    ctx = ngx_http_get_module_ctx(r, ngx_http_realtime_module);

    if (ctx == NULL) {
        v->not_found = 1;
        return NGX_OK;
    }

    if (ctx->fresh == 1) {
        p = ngx_pnalloc(r->pool, NGX_OFF_T_LEN);
        if (p == NULL) {
            return NGX_ERROR;
        }
    } else {
        p = v->data;
    }

    sent = r->connection->sent - ctx->last_sent - r->header_size - r->connection->buffered;
    ctx->last_sent = r->connection->sent;

    sent = ngx_max(sent, 0);

    v->valid = 1;
    v->no_cacheable = 1;
    v->not_found = 0;
    v->len = ngx_sprintf(p, "%O", sent) - p;
    v->data = p;

    return NGX_OK;
}

static ngx_int_t ngx_http_realtime_request_time_elapsed_variable(ngx_http_request_t *r, ngx_http_variable_value_t *v, uintptr_t data) {
    ngx_http_realtime_ctx_t        *ctx;
    ngx_http_realtime_loc_conf_t   *loc_conf;
    u_char                         *p;
    ngx_time_t                     *ts;
    ngx_msec_int_t                  ms;

    loc_conf = ngx_http_get_module_loc_conf(r, ngx_http_realtime_module);
    if (loc_conf->realtime_monitor != 1) {
        v->not_found = 1;
        return NGX_OK;
    }

    ctx = ngx_http_get_module_ctx(r, ngx_http_realtime_module);

    if (ctx == NULL) {
        v->not_found = 1;
        return NGX_OK;
    }

    if (ctx->fresh == 1) {
        p = ngx_pnalloc(r->pool, NGX_TIME_T_LEN + 4);
        if (p == NULL) {
            return NGX_ERROR;
        }
    } else {
        p = v->data;
    }

    ts = ngx_timeofday();
    ms = (ngx_msec_int_t) ((ts->sec - ctx->last_sec) * 1000 + (ts->msec - ctx->last_msec));

    ctx->last_sec = ts->sec;
    ctx->last_msec = ts->msec;

    v->valid = 1;
    v->no_cacheable = 1;
    v->not_found = 0;
    v->len = ngx_sprintf(p, "%T.%03M", (time_t) ms / 1000, ms % 1000) - p;;
    v->data = p;

    return NGX_OK;
}


static ngx_int_t ngx_http_realtime_request_completion_variable(ngx_http_request_t *r, ngx_http_variable_value_t *v, uintptr_t data) {
    ngx_http_realtime_ctx_t        *ctx;
    ngx_http_realtime_loc_conf_t   *loc_conf;

    loc_conf = ngx_http_get_module_loc_conf(r, ngx_http_realtime_module);
    if (loc_conf->realtime_monitor != 1) {
        v->not_found = 1;
        return NGX_OK;
    }

    ctx = ngx_http_get_module_ctx(r, ngx_http_realtime_module);

    if (ctx == NULL) {
        v->not_found = 1;
        return NGX_OK;
    }

    if (ctx->completed) {
        v->len = 2;
        v->valid = 1;
        v->no_cacheable = 1;
        v->not_found = 0;
        v->data = (u_char *) "OK";

        return NGX_OK;
    }

    *v = ngx_http_variable_null_value;

    return NGX_OK;
}


static ngx_int_t ngx_http_realtime_is_fresh_variable(ngx_http_request_t *r, ngx_http_variable_value_t *v, uintptr_t data) {
    ngx_http_realtime_ctx_t        *ctx;
    ngx_http_realtime_loc_conf_t   *loc_conf;
    u_char                         *p;

    loc_conf = ngx_http_get_module_loc_conf(r, ngx_http_realtime_module);
    if (loc_conf->realtime_monitor != 1) {
        v->not_found = 1;
        return NGX_OK;
    }

    ctx = ngx_http_get_module_ctx(r, ngx_http_realtime_module);

    if (ctx == NULL) {
        v->not_found = 1;
        return NGX_OK;
    }

    if (ctx->fresh == 1) {
        p = ngx_pnalloc(r->pool, sizeof("YES") - 1);
        if (p == NULL) {
            return NGX_ERROR;
        }
    } else {
        p = v->data;
    }


    v->valid = 1;
    v->no_cacheable = ctx->fresh;
    v->not_found = 0;
    v->len = ngx_sprintf(p, "%s", ctx->fresh == 1 ? "YES" : "NO") - p;
    v->data = p;

    return NGX_OK;
}


static ngx_int_t ngx_http_realtime_interval_variable(ngx_http_request_t *r, ngx_http_variable_value_t *v, uintptr_t data) {
    ngx_http_realtime_ctx_t        *ctx;
    ngx_http_realtime_loc_conf_t   *loc_conf;
    u_char                         *p;

    loc_conf = ngx_http_get_module_loc_conf(r, ngx_http_realtime_module);
    if (loc_conf->realtime_monitor != 1) {
        v->not_found = 1;
        return NGX_OK;
    }

    ctx = ngx_http_get_module_ctx(r, ngx_http_realtime_module);

    if (ctx == NULL) {
        v->not_found = 1;
        return NGX_OK;
    }

    if (ctx->fresh == 1) {
        p = ngx_pnalloc(r->pool, NGX_INT_T_LEN);
        if (p == NULL) {
            return NGX_ERROR;
        }
    } else {
        p = v->data;
    }


    v->valid = 1;
    v->no_cacheable = 0;
    v->not_found = 0;
    v->len = ngx_sprintf(p, "%03M", loc_conf->realtime_interval) - p;
    v->data = p;

    return NGX_OK;
}


static ngx_int_t ngx_http_realtime_queue_size_variable(ngx_http_request_t *r, ngx_http_variable_value_t *v, uintptr_t data) {
    ngx_http_realtime_ctx_t        *ctx;
    ngx_http_realtime_main_conf_t  *main_conf;
    u_char                         *p;

    main_conf = ngx_http_cycle_get_module_main_conf(ngx_cycle, ngx_http_realtime_module);

    if (main_conf->syslog->configured != 1) {
        v->not_found = 1;
        return NGX_OK;
    }

    ctx = ngx_http_get_module_ctx(r, ngx_http_realtime_module);

    if (ctx == NULL) {
        v->not_found = 1;
        return NGX_OK;
    }

    if (ctx->fresh == 1) {
        p = ngx_pnalloc(r->pool, NGX_INT_T_LEN);
        if (p == NULL) {
            return NGX_ERROR;
        }
    } else {
        p = v->data;
    }


    v->valid = 1;
    v->no_cacheable = 0;
    v->not_found = 0;
    v->len = ngx_sprintf(p, "%i", main_conf->syslog->queue_len) - p;
    v->data = p;

    return NGX_OK;
}


static ngx_http_variable_t  ngx_http_realtime_vars[] = {

        { ngx_string("realtime_body_bytes_sent"), NULL,
          ngx_http_realtime_body_bytes_sent_variable, 0, NGX_HTTP_VAR_CHANGEABLE|NGX_HTTP_VAR_NOCACHEABLE, 0 },

        { ngx_string("realtime_time_elapsed"), NULL,
          ngx_http_realtime_request_time_elapsed_variable, 0, NGX_HTTP_VAR_CHANGEABLE|NGX_HTTP_VAR_NOCACHEABLE, 0 },

        { ngx_string("realtime_request_completion"), NULL,
          ngx_http_realtime_request_completion_variable, 0, NGX_HTTP_VAR_CHANGEABLE|NGX_HTTP_VAR_NOCACHEABLE, 0 },

        { ngx_string("realtime_is_fresh"), NULL,
          ngx_http_realtime_is_fresh_variable, 0, NGX_HTTP_VAR_CHANGEABLE|NGX_HTTP_VAR_NOCACHEABLE, 0 },

        { ngx_string("realtime_queue_size"), NULL,
          ngx_http_realtime_queue_size_variable, 0, NGX_HTTP_VAR_CHANGEABLE|NGX_HTTP_VAR_NOCACHEABLE, 0 },

        { ngx_string("realtime_interval"), NULL,
          ngx_http_realtime_interval_variable, 0, 0, 0 },

        ngx_http_null_variable
};


/*
 * Functions
 */


static ngx_int_t ngx_http_realtime_send_to_syslog(ngx_syslog_peer_t *peer, ngx_uint_t level, u_char *buf, size_t len) {
    u_char             *p, msg[NGX_HTTP_REALTIME_SYSLOG_MAX_STR];
    ngx_uint_t          head_len;
    ssize_t             n;
    ssize_t             size;

    if (peer->busy) {
        return 2;
    }

    peer->busy = 1;
    if (peer->severity > 5) {
        peer->severity = level - 1;
    }

    p = ngx_syslog_add_header(peer, msg);
    head_len = p - msg;

    if (len > NGX_HTTP_REALTIME_SYSLOG_MAX_STR - head_len) {
        len = NGX_HTTP_REALTIME_SYSLOG_MAX_STR - head_len;
    }

    p = ngx_snprintf(p, len, "%s", buf);
    size = p - msg;

    n = ngx_syslog_send(peer, msg, size);

    peer->busy = 0;

    if (n == size) {
        return 1;
    }

    return 0;
}

static void ngx_http_realtime_process(ngx_event_t *ev) {
    ngx_str_t                           log_line;
    ngx_http_realtime_syslog_queue_t    *syslog_queue;
    ngx_http_request_t                  *r;
    ngx_connection_t                    *c;
    ngx_http_realtime_ctx_t             *ctx;
    ngx_http_realtime_main_conf_t       *main_conf;
    ngx_http_realtime_loc_conf_t        *loc_conf;

    r = ev->data;

    if (r == NULL) {
        return;
    }


    if (r->pool == NULL) {
        return;
    }

    ctx = ngx_http_get_module_ctx(r, ngx_http_realtime_module);

    if (ctx == NULL) {
        return;
    }

    if (ctx->initialized != 1) {
        return;
    }

    c = r->connection;
    main_conf = ngx_http_cycle_get_module_main_conf(ngx_cycle, ngx_http_realtime_module);
    loc_conf = ngx_http_get_module_loc_conf(r, ngx_http_realtime_module);

    if (main_conf->realtime_enabled != 1 || loc_conf->realtime_monitor != 1) {
        if (r->internal != 1) {
            ngx_log_error(NGX_LOG_ALERT, c->log, 0, "Realtime log bug: in processor and not internally redirected while config are disabled for location");
        }
        return;
    }

    if (loc_conf->realtime_format == NULL) {
        if (r->internal != 1) {
            ngx_log_error(NGX_LOG_ALERT, c->log, 0, "Realtime log bug: format is null in processor and it's not internally redirected");
        }
        return;
    }

    if (ngx_http_complex_value(r, loc_conf->realtime_format, &log_line) != NGX_OK) {
        return;
    }

    ctx->fresh = 0;

    if (loc_conf->realtime_error_log) {
        ngx_log_error(
                loc_conf->realtime_log_level,
                loc_conf->realtime_error_log_use_cycle == 1 ? ngx_cycle->log : c->log,
                0,
                "%V",
                &log_line
        );
    }

    if (loc_conf->realtime_syslog && main_conf->syslog->configured == 1) {
        syslog_queue = ngx_palloc(ngx_cycle->pool, sizeof(ngx_http_realtime_syslog_queue_t));

        if (syslog_queue == NULL) {
            ngx_log_error(NGX_LOG_WARN, c->log, 0, "Cannot create memory for syslog element data");

        } else {
            syslog_queue->line.data = ngx_pnalloc(ngx_cycle->pool, log_line.len);

            if (syslog_queue->line.data != NULL) {
                syslog_queue->line.len = log_line.len;
                syslog_queue->log_level = loc_conf->realtime_log_level;
                ngx_memcpy(syslog_queue->line.data, log_line.data, log_line.len);
                ++main_conf->syslog->queue_len;
                ngx_queue_insert_tail(&main_conf->syslog->queue, &syslog_queue->queue);
            } else {
                ngx_pfree(ngx_cycle->pool, syslog_queue);
                ngx_log_error(NGX_LOG_WARN, c->log, 0, "Cannot add to syslog queue");
            }
        }
    }

    if (ctx->completed != 1 && !ev->timer_set) {
        ngx_add_timer(ev, loc_conf->realtime_interval);
    }
}

static void ngx_http_realtime_queue_consumer_handler(ngx_event_t *ev) {
    ngx_http_realtime_syslog_queue_t    *info;
    ngx_queue_t                         *q;
    ngx_http_realtime_main_conf_t       *main_conf;
    ngx_int_t                            syslog_result;
    ngx_uint_t                           current_cycle_sent_number;

    current_cycle_sent_number = 0;

    main_conf = ngx_http_cycle_get_module_main_conf(ngx_cycle, ngx_http_realtime_module);

    while (!ngx_queue_empty(&main_conf->syslog->queue)) {
        q = ngx_queue_head(&main_conf->syslog->queue);
        info = ngx_queue_data(q, ngx_http_realtime_syslog_queue_t, queue);

        if (info == NULL) {
            ngx_log_error(NGX_LOG_ALERT, ngx_cycle->log, 0, "Realtime syslog queue data is NULL in consumer");
            continue;
        }

        syslog_result = ngx_http_realtime_send_to_syslog(
                main_conf->syslog->peer,
                info->log_level,
                info->line.data, info->line.len
        );

        if (syslog_result == 0) {
            main_conf->syslog->failed += 1;
        } else {
            main_conf->syslog->failed = 0;
        }

        if (main_conf->syslog->failed == 0 || main_conf->syslog->failed > main_conf->realtime_skip_failed_after) {
            if (main_conf->syslog->failed > main_conf->realtime_skip_failed_after) {
                ngx_log_error(NGX_LOG_ALERT, ngx_cycle->log, 0, "Realtime syslog send() failed multiple times");
            }

            --main_conf->syslog->queue_len;
            ngx_queue_remove(q);
            ngx_pfree(ngx_cycle->pool, info->line.data);
            ngx_pfree(ngx_cycle->pool, info);
        }


        ++current_cycle_sent_number;

        if (!ngx_exiting && ev != NULL) {
            if (current_cycle_sent_number >= main_conf->realtime_syslog_send_batch_size) {
                break;
            }
        }
    }

    if (ngx_exiting || ev == NULL) {
        return;
    }

    ngx_add_timer(ev, main_conf->realtime_syslog_queue_interval);
}


/*
 * Handlers
 */


static ngx_int_t ngx_http_realtime_configure_handler(ngx_http_request_t *r) {
    ngx_http_realtime_loc_conf_t    *loc_conf;
    ngx_connection_t                *c;
    ngx_event_t                     *ev;
    ngx_http_realtime_ctx_t         *ctx;

    loc_conf = ngx_http_get_module_loc_conf(r, ngx_http_realtime_module);
    c = r->connection;

    if (loc_conf->realtime_monitor != 1) {
        return NGX_DECLINED;
    }

    if (loc_conf->realtime_format == NULL) {
        return NGX_DECLINED;
    }

    ctx = ngx_http_get_module_ctx(r, ngx_http_realtime_module);
    if (ctx != NULL) {
        return NGX_DECLINED;
    }

    ctx = ngx_pcalloc(r->pool, sizeof(ngx_http_realtime_ctx_t));

    if (ctx == NULL) {
        return NGX_ERROR;
    }

    ev = ngx_palloc(r->pool, sizeof(ngx_event_t));
    if (ev == NULL) {
        return NGX_ERROR;
    }

    ev->data = r;
    ev->handler = ngx_http_realtime_process;
    ev->log = c->log;
    ev->cancelable = 1;

    ctx->last_sent = 0;
    ctx->last_sec  = r->start_sec;
    ctx->last_msec = r->start_msec;
    ctx->completed = 0;
    ctx->fresh = 1;

    if (!ev->timer_set) {
        ngx_add_timer(ev, loc_conf->realtime_interval);
        ctx->initialized = 1;
    }

    ctx->ev = ev;
    ngx_http_set_ctx(r, ctx, ngx_http_realtime_module);

    return NGX_DECLINED;
}


static ngx_int_t ngx_http_realtime_finalize_handler(ngx_http_request_t *r) {
    ngx_http_realtime_loc_conf_t    *loc_conf;
    ngx_http_realtime_ctx_t         *ctx;

    loc_conf = ngx_http_get_module_loc_conf(r, ngx_http_realtime_module);

    if (loc_conf->realtime_monitor != 1) {
        return NGX_DECLINED;
    }

    ctx = ngx_http_get_module_ctx(r, ngx_http_realtime_module);

    if (ctx == NULL) {
        return NGX_DECLINED;
    }

    if (ctx->initialized != 1) {
        return NGX_DECLINED;
    }

    if (ctx->ev->timer_set) {
        ngx_del_timer(ctx->ev);
    }

    ctx->completed = 1;
    ngx_http_realtime_process(ctx->ev);

    return NGX_DECLINED;
}


/*
 * Configuration
 */


static ngx_int_t ngx_http_realtime_process_init(ngx_cycle_t *cycle) {
    ngx_http_realtime_main_conf_t    *main_conf;
    ngx_event_t                      *ev;

    main_conf = ngx_http_cycle_get_module_main_conf(cycle, ngx_http_realtime_module);

    if (main_conf->syslog == NULL) {
        return NGX_OK;
    }

    if (main_conf->syslog->configured != 1) {
        return NGX_OK;
    }

    ev = ngx_pcalloc(cycle->pool, sizeof(ngx_event_t));
    if (ev == NULL) {
        return NGX_ERROR;
    }

    ev->data = cycle;
    ev->handler = ngx_http_realtime_queue_consumer_handler;
    ev->log = cycle->log;
    ev->cancelable = 1;

    ngx_add_timer(ev, main_conf->realtime_syslog_queue_interval);

    return NGX_OK;
}


static void ngx_http_realtime_process_exit(ngx_cycle_t *cycle) {
    ngx_http_realtime_main_conf_t    *main_conf;

    main_conf = ngx_http_cycle_get_module_main_conf(cycle, ngx_http_realtime_module);

    if (main_conf->realtime_enabled != 1) {
        return;
    }

    if (main_conf->syslog == NULL) {
        return;
    }

    if (main_conf->syslog->configured != 1) {
        return;
    }

    ngx_http_realtime_queue_consumer_handler(NULL);
}


static ngx_int_t ngx_http_realtime_attach_conf(ngx_conf_t *cf) {
    ngx_http_realtime_main_conf_t    *main_conf;

    main_conf = ngx_http_conf_get_module_main_conf(cf, ngx_http_realtime_module);
    if (main_conf->realtime_enabled != 1) {
        ngx_conf_log_error(NGX_LOG_INFO, cf, 0, "Realtime disabled so no attach");
        return NGX_OK;
    }

    ngx_http_handler_pt *h;
    ngx_http_core_main_conf_t *cm_cf;

    cm_cf = ngx_http_conf_get_module_main_conf(cf, ngx_http_core_module);

    h = ngx_array_push(&cm_cf->phases[
            main_conf->realtime_monitor_from_content_phase ? NGX_HTTP_CONTENT_PHASE: NGX_HTTP_PRECONTENT_PHASE
    ].handlers);

    if (h == NULL) {
        ngx_conf_log_error(NGX_LOG_EMERG, cf, 0, "Could not attach realtime pre content handler.");
        return NGX_ERROR;
    }
    *h = ngx_http_realtime_configure_handler;

    h = ngx_array_push(&cm_cf->phases[NGX_HTTP_LOG_PHASE].handlers);
    if (h == NULL) {
        ngx_conf_log_error(NGX_LOG_EMERG, cf, 0, "Could not attach realtime log handler");
        return NGX_ERROR;
    }
    *h = ngx_http_realtime_finalize_handler;

    return NGX_OK;
}


static ngx_int_t ngx_hunter_realtime_pre_conf(ngx_conf_t *cf) {
    ngx_http_variable_t  *var, *v;

    for (v = ngx_http_realtime_vars; v->name.len; v++) {
        var = ngx_http_add_variable(cf, &v->name, v->flags);
        if (var == NULL) {
            return NGX_ERROR;
        }

        var->get_handler = v->get_handler;
        var->data = v->data;
    }

    return NGX_OK;
}


static void *ngx_http_realtime_create_main_conf(ngx_conf_t *cf) {
    ngx_http_realtime_main_conf_t    *conf;

    conf = ngx_pcalloc(cf->pool, sizeof(ngx_http_realtime_main_conf_t));
    if (conf == NULL) {
        ngx_conf_log_error(NGX_LOG_EMERG, cf, 0, "unable to allocate memory to create realtime main config");
        return NGX_CONF_ERROR;
    }

    conf->realtime_enabled = NGX_CONF_UNSET;
    conf->realtime_monitor_from_content_phase = NGX_CONF_UNSET;
    conf->realtime_skip_failed_after = NGX_CONF_UNSET_UINT;
    conf->realtime_syslog_send_batch_size = NGX_CONF_UNSET_UINT;
    conf->realtime_syslog_queue_interval = NGX_CONF_UNSET_MSEC;

    conf->syslog = ngx_pcalloc(cf->pool, sizeof(ngx_http_realtime_syslog_t));
    if (conf->syslog == NULL) {
        return NGX_CONF_ERROR;
    }

    conf->syslog->configured = 0;

    return conf;
}


static char *ngx_http_realtime_init_main_conf(ngx_conf_t *cf, void *configuration) {
    ngx_http_realtime_main_conf_t    *conf = (ngx_http_realtime_main_conf_t*) configuration;

    ngx_conf_init_value(conf->realtime_enabled, 0);
    ngx_conf_init_value(conf->realtime_monitor_from_content_phase, 0);
    ngx_conf_init_uint_value(conf->realtime_skip_failed_after, 10);
    ngx_conf_init_uint_value(conf->realtime_syslog_send_batch_size, 5);
    ngx_conf_init_msec_value(conf->realtime_syslog_queue_interval, 300);

    if (conf->realtime_enabled != 1) {
        ngx_conf_log_error(NGX_LOG_INFO, cf, 0, "Realtime disabled");
        return NGX_CONF_OK;
    }

    if (conf->realtime_skip_failed_after < 1) {
        conf->realtime_skip_failed_after = 1;
    }

    if (conf->realtime_syslog_send_batch_size < 1) {
        conf->realtime_syslog_send_batch_size = 1;
    }

    if (conf->realtime_syslog_queue_interval < 10) {
        conf->realtime_syslog_queue_interval = 10;
    }

    return NGX_CONF_OK;
}


static void *ngx_http_realtime_create_loc_conf(ngx_conf_t *cf) {
    ngx_http_realtime_loc_conf_t    *conf;

    conf = ngx_pcalloc(cf->pool, sizeof(ngx_http_realtime_loc_conf_t));
    if (conf == NULL) {
        ngx_conf_log_error(NGX_LOG_EMERG, cf, 0, "unable to allocate memory to create realtime loc config");
        return NGX_CONF_ERROR;
    }

    conf->realtime_monitor = NGX_CONF_UNSET;
    conf->realtime_error_log = NGX_CONF_UNSET;
    conf->realtime_syslog = NGX_CONF_UNSET;
    conf->realtime_interval = NGX_CONF_UNSET_MSEC;
    conf->realtime_error_log_use_cycle = NGX_CONF_UNSET;

    return conf;
}

static char *ngx_http_realtime_merge_loc_conf(ngx_conf_t *cf, void *parent_configuration, void *configuration) {
    ngx_http_realtime_loc_conf_t     *parent;
    ngx_http_realtime_loc_conf_t     *conf;
    ngx_http_realtime_main_conf_t    *main_conf;

    parent = parent_configuration;
    conf = configuration;
    main_conf = ngx_http_conf_get_module_main_conf(cf, ngx_http_realtime_module);

    ngx_conf_merge_value(conf->realtime_monitor, parent->realtime_monitor, 0);
    ngx_conf_merge_value(conf->realtime_error_log, parent->realtime_error_log, 0);
    ngx_conf_merge_value(conf->realtime_syslog, parent->realtime_syslog, main_conf->syslog->configured);
    ngx_conf_merge_value(conf->realtime_error_log_use_cycle, parent->realtime_error_log_use_cycle, 0);

    ngx_conf_merge_msec_value(conf->realtime_interval, parent->realtime_interval, 5000);
    ngx_conf_merge_str_value(conf->realtime_log_level_name, parent->realtime_log_level_name, "alert");

    if (conf->realtime_interval < 100) {
        ngx_conf_log_error(NGX_LOG_EMERG, cf, 0, "Interval cannot be less than 0.1s");
        return NGX_CONF_ERROR;
    }

    if (conf->realtime_format == NULL) {
        conf->realtime_format = parent->realtime_format;
    }

    if (ngx_memcmp(conf->realtime_log_level_name.data, (u_char *) "emerg", conf->realtime_log_level_name.len) == 0) {
        conf->realtime_log_level = NGX_LOG_EMERG;

    } else if (ngx_memcmp(conf->realtime_log_level_name.data, (u_char *) "alert", conf->realtime_log_level_name.len) == 0) {
        conf->realtime_log_level = NGX_LOG_ALERT;

    } else if (ngx_memcmp(conf->realtime_log_level_name.data, (u_char *) "crit", conf->realtime_log_level_name.len) == 0) {
        conf->realtime_log_level = NGX_LOG_CRIT;

    } else if (ngx_memcmp(conf->realtime_log_level_name.data, (u_char *) "error", conf->realtime_log_level_name.len) == 0) {
        conf->realtime_log_level = NGX_LOG_ERR;

    } else if (ngx_memcmp(conf->realtime_log_level_name.data, (u_char *) "warn", conf->realtime_log_level_name.len) == 0) {
        conf->realtime_log_level = NGX_LOG_WARN;

    } else if (ngx_memcmp(conf->realtime_log_level_name.data, (u_char *) "notice", conf->realtime_log_level_name.len) == 0) {
        conf->realtime_log_level = NGX_LOG_NOTICE;

    } else if (ngx_memcmp(conf->realtime_log_level_name.data, (u_char *) "info", conf->realtime_log_level_name.len) == 0) {
        conf->realtime_log_level = NGX_LOG_INFO;

    } else if (ngx_memcmp(conf->realtime_log_level_name.data, (u_char *) "debug", conf->realtime_log_level_name.len) == 0) {
        conf->realtime_log_level = NGX_LOG_DEBUG;

    } else {
        ngx_str_set(&conf->realtime_log_level_name, "alert");
        conf->realtime_log_level = NGX_LOG_ALERT;
    }

    if (main_conf->syslog->configured != 1) {
        conf->realtime_syslog = 0;
    }

    if (conf->realtime_format == NULL || conf->realtime_monitor != 1 || (conf->realtime_error_log != 1 && conf->realtime_syslog != 1)) {
        conf->realtime_monitor = 0;
        conf->realtime_error_log = 0;
        conf->realtime_syslog = 0;
    }

    return NGX_CONF_OK;
}


static char * ngx_http_realtime_setup_syslog(ngx_conf_t *cf, ngx_command_t *cmd, void *conf) {
    ngx_http_realtime_main_conf_t    *main_conf;

    main_conf = ngx_http_conf_get_module_main_conf(cf, ngx_http_realtime_module);
    if (main_conf->realtime_enabled != 1) {
        return NGX_CONF_OK;
    }

    main_conf->syslog->peer = ngx_pcalloc(cf->pool, sizeof(ngx_syslog_peer_t));

    if (main_conf->syslog->peer == NULL) {
        return NGX_CONF_ERROR;
    }

    if (ngx_syslog_process_conf(cf, main_conf->syslog->peer) != NGX_CONF_OK) {
        return NGX_CONF_ERROR;
    }

    ngx_queue_init(&main_conf->syslog->queue);

    main_conf->syslog->configured = 1;
    main_conf->syslog->failed = 0;
    main_conf->syslog->queue_len = 0;

    return NGX_CONF_OK;
}

/*
 * End
 */


/*
 * Routines
 */

static ngx_command_t  ngx_http_realtime_commands[] = {

        { ngx_string("realtime_enabled"),
          NGX_HTTP_MAIN_CONF|NGX_CONF_FLAG,
          ngx_conf_set_flag_slot,
          NGX_HTTP_MAIN_CONF_OFFSET,
          offsetof(ngx_http_realtime_main_conf_t, realtime_enabled),
          NULL },

        { ngx_string("realtime_monitor_from_content_phase"),
          NGX_HTTP_MAIN_CONF|NGX_CONF_FLAG,
          ngx_conf_set_flag_slot,
          NGX_HTTP_MAIN_CONF_OFFSET,
          offsetof(ngx_http_realtime_main_conf_t, realtime_monitor_from_content_phase),
          NULL },

        { ngx_string("realtime_syslog_info"),
          NGX_HTTP_MAIN_CONF|NGX_CONF_1MORE,
          ngx_http_realtime_setup_syslog,
          NGX_HTTP_MAIN_CONF_OFFSET,
          0,
          NULL },

        { ngx_string("realtime_skip_failed_after"),
          NGX_HTTP_MAIN_CONF|NGX_CONF_TAKE1,
          ngx_conf_set_num_slot,
          NGX_HTTP_MAIN_CONF_OFFSET,
          offsetof(ngx_http_realtime_main_conf_t, realtime_skip_failed_after),
          NULL },

        { ngx_string("realtime_syslog_send_batch_size"),
          NGX_HTTP_MAIN_CONF|NGX_CONF_TAKE1,
          ngx_conf_set_num_slot,
          NGX_HTTP_MAIN_CONF_OFFSET,
          offsetof(ngx_http_realtime_main_conf_t, realtime_syslog_send_batch_size),
          NULL },

        { ngx_string("realtime_syslog_queue_interval"),
          NGX_HTTP_MAIN_CONF|NGX_CONF_TAKE1,
          ngx_conf_set_msec_slot,
          NGX_HTTP_MAIN_CONF_OFFSET,
          offsetof(ngx_http_realtime_main_conf_t, realtime_syslog_queue_interval),
          NULL },

        { ngx_string("realtime_format"),
          NGX_HTTP_MAIN_CONF|NGX_HTTP_SRV_CONF|NGX_HTTP_LOC_CONF|NGX_CONF_TAKE1,
          ngx_http_set_complex_value_slot,
          NGX_HTTP_LOC_CONF_OFFSET,
          offsetof(ngx_http_realtime_loc_conf_t, realtime_format),
          NULL },

        { ngx_string("realtime_log_level"),
          NGX_HTTP_MAIN_CONF|NGX_HTTP_SRV_CONF|NGX_HTTP_LOC_CONF|NGX_CONF_TAKE1,
          ngx_conf_set_str_slot,
          NGX_HTTP_LOC_CONF_OFFSET,
          offsetof(ngx_http_realtime_loc_conf_t, realtime_log_level_name),  // Offset is correct btw
          NULL },

        { ngx_string("realtime_monitor"),
          NGX_HTTP_MAIN_CONF|NGX_HTTP_SRV_CONF|NGX_HTTP_LOC_CONF|NGX_CONF_FLAG,
          ngx_conf_set_flag_slot,
          NGX_HTTP_LOC_CONF_OFFSET,
          offsetof(ngx_http_realtime_loc_conf_t, realtime_monitor),
          NULL },

        { ngx_string("realtime_error_log"),
          NGX_HTTP_MAIN_CONF|NGX_HTTP_SRV_CONF|NGX_HTTP_LOC_CONF|NGX_CONF_FLAG,
          ngx_conf_set_flag_slot,
          NGX_HTTP_LOC_CONF_OFFSET,
          offsetof(ngx_http_realtime_loc_conf_t, realtime_error_log),
          NULL },

        { ngx_string("realtime_syslog"),
          NGX_HTTP_MAIN_CONF|NGX_HTTP_SRV_CONF|NGX_HTTP_LOC_CONF|NGX_CONF_FLAG,
          ngx_conf_set_flag_slot,
          NGX_HTTP_LOC_CONF_OFFSET,
          offsetof(ngx_http_realtime_loc_conf_t, realtime_syslog),
          NULL },

        { ngx_string("realtime_interval"),
          NGX_HTTP_MAIN_CONF|NGX_HTTP_SRV_CONF|NGX_HTTP_LOC_CONF|NGX_CONF_TAKE1,
          ngx_conf_set_msec_slot,
          NGX_HTTP_LOC_CONF_OFFSET,
          offsetof(ngx_http_realtime_loc_conf_t, realtime_interval),
          NULL },

        { ngx_string("realtime_error_log_use_cycle"),
          NGX_HTTP_MAIN_CONF|NGX_HTTP_SRV_CONF|NGX_HTTP_LOC_CONF|NGX_CONF_FLAG,
          ngx_conf_set_flag_slot,
          NGX_HTTP_LOC_CONF_OFFSET,
          offsetof(ngx_http_realtime_loc_conf_t, realtime_error_log_use_cycle),
          NULL },

        ngx_null_command
};


static ngx_http_module_t ngx_http_realtime_module_ctx = {
        ngx_hunter_realtime_pre_conf,          /* pre-configuration */
        ngx_http_realtime_attach_conf,         /* post-configuration */

        ngx_http_realtime_create_main_conf,    /* create main configuration */
        ngx_http_realtime_init_main_conf,      /* init main configuration */

        NULL,                                  /* create server configuration */
        NULL,                                  /* merge server configuration */

        ngx_http_realtime_create_loc_conf,     /* create location configuration */
        ngx_http_realtime_merge_loc_conf       /* merge location configuration */
};


ngx_module_t  ngx_http_realtime_module = {
        NGX_MODULE_V1,
        &ngx_http_realtime_module_ctx,    /* module context */
        ngx_http_realtime_commands,       /* module directives */
        NGX_HTTP_MODULE,                  /* module type */
        NULL,                             /* init master */
        NULL,                             /* init module */
        ngx_http_realtime_process_init,   /* init process */
        NULL,                             /* init thread */
        NULL,                             /* exit thread */
        ngx_http_realtime_process_exit,   /* exit process */
        NULL,                             /* exit master */
        NGX_MODULE_V1_PADDING
};

/*
 * End
 */