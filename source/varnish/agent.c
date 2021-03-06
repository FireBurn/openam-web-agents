/**
 * The contents of this file are subject to the terms of the Common Development and
 * Distribution License (the License). You may not use this file except in compliance with the
 * License.
 *
 * You can obtain a copy of the License at legal/CDDLv1.0.txt. See the License for the
 * specific language governing permission and limitations under the License.
 *
 * When distributing Covered Software, include this CDDL Header Notice in each file and include
 * the License file at legal/CDDLv1.0.txt. If applicable, add the following below the CDDL
 * Header, with the fields enclosed by brackets [] replaced by your own identifying
 * information: "Portions copyright [year] [name of copyright owner]".
 *
 * Copyright 2015 - 2016 ForgeRock AS.
 */

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <vcl.h>
#include <vrt.h>
#include <vrt_obj.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/resource.h>
#include <vsa.h>
#include <cache/cache.h>
#include <pthread.h>
#include <unistd.h>
#include <errno.h>
#include "version.h"
#include "am.h"
#include "list.h"

#define THREAD_ID                 (void *)(uintptr_t)pthread_self()
#define HTTP_HDR_CLEAR            1
#define HTTP_HDR_SET              0
#define HTTP_HDR_COOKIE           "Cookie"
#define HTTP_HDR_CONTENT_LENGTH   "Content-Length"
#define HTTP_HDR_CONTENT_TYPE     "Content-Type"

#define AM_ADD_HEADER_RESP_SYNTH(r, n, v) am_add_header(r, n, v, AM_DONE, HTTP_HDR_SET, HDR_RESP)
#define AM_ADD_HEADER_RESP_DELIVER(r, n, v) am_add_header(r, n, v, AM_SUCCESS, HTTP_HDR_SET, HDR_RESP)
#define AM_SET_HEADER_REQ(r, n, v) am_add_header(r, n, v, AM_SUCCESS, HTTP_HDR_CLEAR, HDR_REQ)

struct agent_instance {
    unsigned long instance_id;
    char *conf_file;
    int status;
};

struct header {
    int type;
    int unset;
    enum gethdr_e where;
    char *name;
    char *value;
    struct header *next;
};

struct request {
    const struct vrt_ctx *ctx;
    uint32_t xid;
    int status;
    int inauth;
    struct header *headers;
    char *body;
    size_t body_sz;
    struct request *next;
};

static pthread_mutex_t init_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_once_t thread_once = PTHREAD_ONCE_INIT;
static pthread_key_t thread_key;
static volatile int n_init = 0;

char *url_decode(const char *str);
char *load_file(const char *filepath, size_t *data_sz);

int get_extended_stack_enabled() {
    char *env = getenv("AM_EXT_STACK_ENABLED");
    if (ISVALID(env)) {
        char *endp = NULL;
        int v = strtol(env, &endp, 0);
        if (env < endp && *endp == '\0' && 0 < v) {
            return 1;
        }
    }
    return 0;
}

int get_stack_size(int id) {
    char *env = getenv("AM_EXT_STACK_SIZE");
    if (ISVALID(getenv("AM_EXT_STACK_ENABLED")) && ISVALID(env)) {
        char *endp = NULL;
        int v = strtol(env, &endp, 0);
        if (env < endp && *endp == '\0' && 0 < v) {
            return v;
        }
    }
    return 0x40000; /* defaults to 256K */
}

void vmod_init_wp(const struct vrt_ctx *ctx, struct vmod_priv *priv, const char *conf) {
    am_config_t *boot;
    struct agent_instance *settings = (struct agent_instance *) priv->priv;
    pthread_mutex_lock(&init_mutex);
    do {
        if (n_init++ != 1) break;

        if (conf == NULL || access(conf, R_OK) != 0) {
            settings->status = AM_FILE_ERROR;
            fprintf(stderr, "am_vmod_init failed. can't access configuration file (%s)\n", LOGEMPTY(conf));
            break;
        }

        if (cache_param->wthread_stacksize > 0) {
            fprintf(stderr, "am_vmod_init current thread pool stack size limit is %ld bytes\n",
                    cache_param->wthread_stacksize);
        }

        if (settings->status != AM_SUCCESS) {
            fprintf(stderr, "am_vmod_init failed. am_init error (%s)\n", am_strerror(settings->status));
            break;
        }

        settings->instance_id = am_instance_id(conf);
        settings->conf_file = strdup(conf);
        if (settings->conf_file == NULL) {
            fprintf(stderr, "am_vmod_init failed. memory allocation error\n");
            break;
        }

        boot = am_get_config_file(settings->instance_id, conf);
        if (boot == NULL) {
            fprintf(stderr, "am_vmod_init failed. failed to load agent bootstrap configuration file (%s)\n", conf);
            break;
        }

        am_log_register_instance(settings->instance_id, boot->debug_file, boot->debug_level, boot->debug,
                boot->audit_file, boot->audit_level, boot->audit, conf);

        am_config_free(&boot);

        fprintf(stderr, "am_vmod_init %s success\n", MODINFO);
    } while (0);
    pthread_mutex_unlock(&init_mutex);
}

static void delete_request_list(struct request **list) {
    struct request *t = list != NULL ? *list : NULL;
    if (t != NULL) {
        delete_request_list(&t->next);
        free(t);
        t = NULL;
    }
}

static int am_add_header(struct request *r, const char *name, const char *value,
        int type, int unset, enum gethdr_e where) {
    struct header *h;
    size_t sz;
    if (r == NULL || !ISVALID(name)) return AM_EINVAL;

    h = (struct header *) WS_Alloc(r->ctx->ws, sizeof (struct header));
    if (h == NULL) {
        VSLb(r->ctx->vsl, SLT_VCL_Log, "am_add_header failed to allocate %ld bytes (%p)", sz, THREAD_ID);
        return AM_ENOMEM;
    }

    h->type = type; /* AM_SUCCESS => ok; AM_DONE => done */
    h->unset = unset;
    h->where = where;
    h->value = NULL;
    sz = strlen(name);
    h->name = WS_Alloc(r->ctx->ws, sz + 1);
    if (h->name == NULL) {
        VSLb(r->ctx->vsl, SLT_VCL_Log, "am_add_header failed to allocate %ld bytes (%p)", sz, THREAD_ID);
        return AM_ENOMEM;
    }
    memcpy(h->name, name, sz);
    h->name[sz] = 0;

    /* http_SetHeader requires both h->name as Varnish header and h->value as header + value */
    if (value != NULL) {
        sz = strlen(name + 1) + strlen(value) + 1;
        h->value = WS_Alloc(r->ctx->ws, sz + 1);
        if (h->value == NULL) {
            VSLb(r->ctx->vsl, SLT_VCL_Log, "am_add_header failed to allocate %ld bytes (%p)", sz, THREAD_ID);
            return AM_ENOMEM;
        }
        strcpy(h->value, name + 1);
        strcat(h->value, " ");
        strcat(h->value, value);
        h->value[sz] = 0;
    }

    h->next = NULL;
    AM_LIST_INSERT(r->headers, h);
    return AM_SUCCESS;
}

static struct request *create_request(const struct vrt_ctx *ctx) {
    struct request *req_list, *req_list_entry;
    if (ctx == NULL) return NULL;

    req_list = pthread_getspecific(thread_key);
    if (req_list == NULL) {
        /* request list is not yet available for this worker thread, 
         * create one now and register this request */
        req_list = (struct request *) calloc(1, sizeof (struct request));
        if (req_list == NULL) {
            VSLb(ctx->vsl, SLT_VCL_Log, "am_vmod memory allocation failure (%p)", THREAD_ID);
            return NULL;
        }
        req_list->ctx = ctx;
        req_list->xid = ctx->req->sp->vxid;
        req_list->inauth = 1;
        pthread_setspecific(thread_key, req_list);
        return req_list;
    }

    /* list is there already, register this request */
    req_list_entry = (struct request *) calloc(1, sizeof (struct request));
    if (req_list_entry == NULL) {
        VSLb(ctx->vsl, SLT_VCL_Log, "am_vmod memory allocation failure (%p)", THREAD_ID);
        return NULL;
    }
    req_list_entry->ctx = ctx;
    req_list_entry->xid = ctx->req->sp->vxid;
    req_list_entry->inauth = 1;
    AM_LIST_INSERT(req_list, req_list_entry);
    return req_list_entry;
}

static struct request *get_request(const struct vrt_ctx *ctx) {
    struct request *e, *t, *request_list = pthread_getspecific(thread_key);
    if (request_list == NULL) {
        VSLb(ctx->vsl, SLT_VCL_Log, "am_vmod failed to get request data (%p)", THREAD_ID);
        return NULL;
    }

    AM_LIST_FOR_EACH(request_list, e, t) {
        if (e->xid == ctx->req->sp->vxid) {
            return e;
        }
    }
    VSLb(ctx->vsl, SLT_VCL_Log, "am_vmod failed to locate xid %d in request list (%p)",
            ctx->req->sp->vxid, THREAD_ID);
    return NULL;
}

static char *make_header_key(const char *value) {
    static __thread char header[AM_PATH_SIZE];
    if (!ISVALID(value)) {
        return NULL;
    }
    snprintf(header, sizeof (header), "%c%s:", (unsigned) strlen(value) + 1, value);
    return header;
}

static const char *get_request_header(const struct vrt_ctx *ctx, const char *key) {
    const struct gethdr_s hdr = {HDR_REQ, make_header_key(key)};
    if (!ISVALID(hdr.what)) {
        return NULL;
    }
    return VRT_GetHdr(ctx, &hdr);
}

static const char *get_request_header_ex(am_request_t *req, const char *name) {
    struct request *rec;
    if (req == NULL || (rec = (struct request *) req->ctx) == NULL ||
            rec->ctx == NULL || ISINVALID(name))
        return NULL;
    return get_request_header(rec->ctx, name);
}

static am_status_t get_request_url(am_request_t *ar) {
    VCL_IP server_addr;
    struct request *req = (struct request *) ar->ctx;
    if (req == NULL || req->ctx == NULL || req->ctx->ws == NULL) return AM_EINVAL;

    server_addr = VRT_r_server_ip(req->ctx);

    ar->orig_url = WS_Printf(req->ctx->ws, "%s:%d%s",
            NOTNULL(VRT_r_server_identity(req->ctx)), /* varnishd -i option; must be in the form: -i "http://agent.fqdn" */
            server_addr != NULL ? VSA_Port(server_addr) : 80,
            NOTNULL(VRT_r_req_url(req->ctx)));

    if (ar->orig_url == NULL || ar->orig_url[0] == ':') {
        AM_LOG_ERROR(ar->instance_id, "get_request_url(): unable to create request url");
        ar->orig_url = NULL;
        return AM_ERROR;
    }
    return AM_SUCCESS;
}

static am_status_t set_header_in_request(am_request_t *ar, const char *key, const char *value) {
    struct request *req = (struct request *) ar->ctx;
    if (req == NULL || !ISVALID(key)) return AM_EINVAL;
    return AM_SET_HEADER_REQ(req, make_header_key(key), NOTNULL(value));
}

static char *WS_Copy_internal(struct ws *ws, const char *string, size_t size) {
    char *copy = WS_Alloc(ws, size + 1);
    if (copy == NULL) return NULL;
    memcpy(copy, string, size);
    copy[size] = 0;
    return copy;
}

static am_status_t set_cookie(am_request_t *ar, const char *header) {
    struct request *req = (struct request *) ar->ctx;
    am_status_t status = AM_SUCCESS;
    const char *current_cookies;
    char *cookie, *equals, *sep;
    if (req == NULL || ISINVALID(header)) return AM_EINVAL;

    /* add cookie in response headers */
    AM_ADD_HEADER_RESP_DELIVER(req, H_Set_Cookie, header);
    AM_ADD_HEADER_RESP_SYNTH(req, H_Set_Cookie, header);

    /* modify Cookie request header */
    cookie = WS_Copy_internal(req->ctx->ws, header, strlen(header));
    if (cookie == NULL) return AM_ENOMEM;

    equals = strchr(cookie, '=');
    sep = strchr(cookie, ';');
    current_cookies = get_request_header(req->ctx, HTTP_HDR_COOKIE);

    if (sep != NULL && equals != NULL && (sep - equals) > 1) {
        char *new_key = WS_Copy_internal(req->ctx->ws, cookie, (equals - cookie) + 1); /* keep equals sign */
        char *new_value = WS_Copy_internal(req->ctx->ws, cookie, sep - cookie);
        if (new_key == NULL || new_value == NULL) return AM_ENOMEM;
        if (ISINVALID(current_cookies)) {
            /* Cookie request header is not available yet - set it now */
            return set_header_in_request(ar, HTTP_HDR_COOKIE, new_value);
        }
        if (strstr(current_cookies, new_key) == NULL) {
            /* append header value to the existing one */
            char *new_cookie = WS_Alloc(req->ctx->ws, strlen(current_cookies) + strlen(new_value) + 2);
            if (new_cookie == NULL) return AM_ENOMEM;
            strcpy(new_cookie, current_cookies);
            strcat(new_cookie, ";");
            strcat(new_cookie, new_value);
            status = set_header_in_request(ar, HTTP_HDR_COOKIE, new_cookie);
        }
    }
    return status;
}

static am_status_t add_header_in_response(am_request_t *ar, const char *key, const char *value) {
    struct request *req = (struct request *) ar->ctx;
    if (req == NULL || !ISVALID(key)) return AM_EINVAL;
    if (!ISVALID(value)) {
        /* value is empty, agent is setting the cookie in response */
        return set_cookie(ar, key);
    }
    return AM_ADD_HEADER_RESP_DELIVER(req, make_header_key(key), value);
}

static am_status_t set_method(am_request_t *ar) {
    struct request *req = (struct request *) ar->ctx;
    if (req == NULL) return AM_EINVAL;
    http_ForceField(req->ctx->http_req, HTTP_HDR_METHOD, am_method_num_to_str(ar->method));
    return AM_SUCCESS;
}

static void store_custom_response(struct request *r, int status, char *data, size_t data_sz) {
    r->status = status;
    r->body = data;
    r->body_sz = data_sz;
}

static int am_status_value(am_status_t v) {
    switch (v) {
        case AM_SUCCESS:
        case AM_PDP_DONE:
        case AM_DONE:
        case AM_NOT_HANDLING:
            return 200;
        case AM_NOT_FOUND:
            return 404;
        case AM_INTERNAL_REDIRECT:
        case AM_REDIRECT:
            return 302;
        case AM_FORBIDDEN:
            return 403;
        case AM_BAD_REQUEST:
            return 400;
        case AM_NOT_IMPLEMENTED:
            return 501;
        default:
            return 500;
    }
}

static am_status_t set_custom_response(am_request_t *ar, const char *text, const char *cont_type) {
    struct request *req = (struct request *) ar->ctx;
    am_status_t status = AM_ERROR;
    if (req == NULL || !ISVALID(text)) {
        return AM_EINVAL;
    }

    status = ar->is_json_url ? AM_JSON_RESPONSE : ar->status;

    switch (status) {
        case AM_JSON_RESPONSE:
        {
            AM_ADD_HEADER_RESP_SYNTH(req, H_Content_Type, "application/json");
            req->status = am_status_value(ar->status);
            switch (ar->status) {
                case AM_PDP_DONE:
                {
                    char *temp = NULL;
                    if (ISVALID(ar->post_data_fn)) {
                        size_t data_sz = ar->post_data_sz;
                        char *data = load_file(ar->post_data_fn, &data_sz);
                        unlink(ar->post_data_fn);
                        temp = base64_encode(data, &data_sz);
                        am_free(data);
                    }
                    req->body_sz = am_asprintf(&req->body, AM_JSON_TEMPLATE_LOCATION_DATA,
                            am_strerror(ar->status), ar->post_data_url, cont_type,
                            NOTNULL(temp), am_status_value(ar->status));
                    am_free(temp);
                }
                    break;
                case AM_REDIRECT:
                case AM_INTERNAL_REDIRECT:
                    req->body_sz = am_asprintf(&req->body, AM_JSON_TEMPLATE_LOCATION,
                            am_strerror(ar->status), text, am_status_value(ar->status));
                    if (is_http_status(ar->conf->json_url_response_code)) {
                        req->status = ar->conf->json_url_response_code;
                    } else {
                        if (ar->conf->json_url_response_code != 0) {
                            AM_LOG_WARNING(ar->instance_id, "set_custom_response(): response status code %d is not valid, sending HTTP_FORBIDDEN",
                                    ar->conf->json_url_response_code);
                        }
                        req->status = am_status_value(AM_FORBIDDEN);
                    }
                    break;
                default:
                {
                    char *payload = am_json_escape(text, NULL);
                    am_asprintf(&req->body, AM_JSON_TEMPLATE_DATA,
                            am_strerror(ar->status), ISVALID(payload) ? payload : "\"\"", am_status_value(ar->status));
                    am_free(payload);
                    break;
                }
            }
            break;
        }
        case AM_PDP_DONE:
        {
            /* special handler for x-www-form-urlencoded POST data */
            if (strcasecmp(cont_type, "application/x-www-form-urlencoded") == 0) {
                char *pair, *a, *eq, *last = NULL;
                char *form = NULL;
                int form_sz;

                form_sz = am_asprintf(&form, "<html><head></head><body onload=\"document.postform.submit()\">"
                        "<form name=\"postform\" method=\"POST\" action=\"%s\">", ar->post_data_url);
                if (form == NULL) {
                    AM_LOG_ERROR(ar->instance_id, "set_custom_response(): memory allocation error");
                    ar->status = AM_ENOMEM;
                    break;
                }

                if (ISVALID(ar->post_data_fn)) {
                    a = load_file(ar->post_data_fn, NULL);
                    unlink(ar->post_data_fn);
                    if (a == NULL) {
                        AM_LOG_ERROR(ar->instance_id,
                                "set_custom_response(): unable to open post preservation file %s", ar->post_data_fn);
                        ar->status = AM_FILE_ERROR;
                        break;
                    }
                    for (pair = strtok_r(a, "&", &last); pair;
                            pair = strtok_r(NULL, "&", &last)) {
                        char *values = url_decode(pair);
                        if (values != NULL) {
                            eq = strchr(values, '=');
                            if (eq) {
                                *eq++ = 0;
                                form_sz = am_asprintf(&form,
                                        "%s<input type=\"hidden\" name=\"%s\" value=\"%s\"/>",
                                        form, values, eq);
                            } else {
                                form_sz = am_asprintf(&form,
                                        "%s<input type=\"hidden\" name=\"%s\" value=\"\"/>",
                                        form, values);
                            }
                            free(values);
                        }
                    }
                    free(a);
                }
                form_sz = am_asprintf(&form, "%s</form></body></html>", form);
                if (form == NULL) {
                    AM_LOG_ERROR(ar->instance_id, "set_custom_response(): memory allocation error");
                    ar->status = AM_ENOMEM;
                    break;
                }

                AM_ADD_HEADER_RESP_SYNTH(req, H_Content_Type, "text/html");
                AM_ADD_HEADER_RESP_SYNTH(req, H_Content_Length, VRT_INT_string(req->ctx, form_sz));
                store_custom_response(req, am_status_value(ar->status), form, form_sz);
                break;
            }

            /* all other content types are replied directly in synt response */
            AM_ADD_HEADER_RESP_SYNTH(req, H_Content_Type, cont_type);
            AM_ADD_HEADER_RESP_SYNTH(req, H_Content_Length, VRT_INT_string(req->ctx, ar->post_data_sz));
            if (ISVALID(ar->post_data_fn)) {
                char *data = load_file(ar->post_data_fn, NULL);
                unlink(ar->post_data_fn);
                if (data == NULL) {
                    AM_LOG_ERROR(ar->instance_id,
                            "set_custom_response(): unable to open post preservation file %s", ar->post_data_fn);
                    ar->status = AM_FILE_ERROR;
                    break;
                }
                store_custom_response(req, am_status_value(ar->status), data, ar->post_data_sz);
            }
            break;
        }
        case AM_INTERNAL_REDIRECT:
        case AM_REDIRECT:
        {
            AM_ADD_HEADER_RESP_SYNTH(req, H_Location, text);
            store_custom_response(req, am_status_value(status), NULL, 0);
            break;
        }
        default:
        {
            if (ISVALID(cont_type)) {
                AM_ADD_HEADER_RESP_SYNTH(req, H_Content_Type, cont_type);
            }
            AM_ADD_HEADER_RESP_SYNTH(req, H_Content_Length, VRT_INT_string(req->ctx, strlen(text)));
            store_custom_response(req, am_status_value(status), strdup(text), strlen(text));
            break;
        }
    }
    AM_LOG_DEBUG(ar->instance_id, "set_custom_response(): status: %s (exit: %s)",
            am_strerror(status), am_strerror(ar->status));
    return AM_SUCCESS;
}

/*
 * Read up to len bytes, returning pipelined data first.
 */
static ssize_t v1f_read_internal(const struct vrt_ctx *ctx, struct http_conn *htc, void *d, ssize_t len) {
    ssize_t i;
    unsigned char *p = d;
    size_t l = 0;

    if (htc->pipeline_b) {
        l = htc->pipeline_e - htc->pipeline_b;
        if (l > len) {
            l = len;
        }
        memcpy(p, htc->pipeline_b, l);
        p += l;
        len -= l;
        htc->pipeline_b += l;
        if (htc->pipeline_b == htc->pipeline_e) {
            htc->pipeline_b = htc->pipeline_e = NULL;
        }
    }
    if (len == 0) {
        return l;
    }
    i = read(htc->fd, p, len);
    if (i < 0) {
        VSLb(ctx->vsl, SLT_FetchError, "%s", strerror(errno));
        return i;
    }
    return i + l;
}

static am_status_t get_request_body(am_request_t *ar) {
    static const char *thisfunc = "get_request_body():";
    struct request *req = (struct request *) ar->ctx;
    size_t content_length, wrote, total_read = 0, tmp_sz = 0;
    const char *content_length_s;
    char *body = NULL, *tmp_body, *tmp, *file_name = NULL;
    int bytes_read;
    char *buf;
    FILE *fd = NULL;
    am_bool_t to_file = AM_FALSE, tmp_writes = AM_TRUE;

    if (req == NULL || req->ctx == NULL) return AM_EINVAL;

    content_length_s = get_request_header(req->ctx, HTTP_HDR_CONTENT_LENGTH);
    if (ISINVALID(content_length_s)) {
        AM_LOG_WARNING(ar->instance_id, "%s missing Content-Length header", thisfunc);
        return AM_NOT_FOUND;
    }

    errno = 0;
    content_length = strtoul(content_length_s, NULL, 10);
    if (content_length == 0) {
        if (errno == ERANGE) {
            AM_LOG_WARNING(ar->instance_id, "%s invalid Content-Length header value %s",
                    thisfunc, content_length_s);
            return AM_NOT_FOUND;
        }

        ar->post_data = ar->post_data_fn = NULL;
        ar->post_data_sz = 0;
        return AM_SUCCESS;
    }

#define REQ_DATA_BUFF_SZ 1024
    buf = WS_Alloc(req->ctx->ws, REQ_DATA_BUFF_SZ + 1);
    if (buf == NULL) {
        AM_LOG_ERROR(ar->instance_id, "%s memory allocation failure", thisfunc);
        return AM_ENOMEM;
    }

#define TEMP_BUFFER_SZ  (REQ_DATA_BUFF_SZ * 2)     
    tmp = WS_Alloc(req->ctx->ws, TEMP_BUFFER_SZ);
    if (tmp == NULL) {
        AM_LOG_ERROR(ar->instance_id, "%s memory allocation failure", thisfunc);
        return AM_ENOMEM;
    }

    while (content_length) {
        bytes_read = v1f_read_internal(req->ctx, req->ctx->req->htc, buf,
                content_length > REQ_DATA_BUFF_SZ ? REQ_DATA_BUFF_SZ : content_length);
        if (bytes_read <= 0) {
            AM_LOG_ERROR(ar->instance_id, "%s v1f_read failure", thisfunc);
            return AM_ERROR;
        }

        content_length -= bytes_read;

        if (tmp_writes && (tmp_sz + bytes_read) <= TEMP_BUFFER_SZ) {
            /* stream initial data into the temp buffer */
            memcpy(tmp + tmp_sz, buf, bytes_read);
            tmp_sz += bytes_read;
        }

        AM_LOG_DEBUG(ar->instance_id, "%s read: %ld, temp: %ld bytes",
                thisfunc, bytes_read, tmp_sz);

        if (tmp_writes) {
            /* try to analyze temp buffer data - see if we can spot our key */
            if (tmp_sz > 5) {
                /* we've got enough data - check if that's 
                 * LARES POST or should it be stored into a file */
                tmp_writes = AM_FALSE;
                to_file = memcmp(tmp, "LARES=", 6) != 0;
            } else {
                /* too little was read in */
                continue;
            }
        }

        AM_LOG_DEBUG(ar->instance_id, "%s storing into: %s, temp writes: %s",
                thisfunc, to_file ? "file" : "memory", !tmp_writes ? "done" : "working");

        if (!tmp_writes) {
            /* no more temp buffer screening - write data directly into a file (heap buffer) */
            if (to_file) {

                if (fd == NULL) {
                    char key[37];

                    if (ISINVALID(ar->conf->pdp_dir)) {
                        AM_LOG_ERROR(ar->instance_id, "%s invalid POST preservation configuration",
                                thisfunc);
                        return AM_EINVAL;
                    }

                    uuid(key, sizeof (key));
                    file_name = WS_Printf(req->ctx->ws, "%s/%s", ar->conf->pdp_dir, key);
                    if (file_name == NULL) {
                        return AM_ENOMEM;
                    }

                    fd = fopen(file_name, "a");
                    if (fd == NULL) {
                        AM_LOG_ERROR(ar->instance_id, "%s unable to open POST preservation file: %s (%d)",
                                thisfunc, file_name, errno);
                        return AM_FILE_ERROR;
                    }
                }

                if (tmp_sz > 0) {
                    /* write down whatever we have stored in the temp buffer */
                    wrote = fwrite(tmp, 1, tmp_sz, fd);
                    total_read += tmp_sz;
                    tmp_sz = 0;
                } else {
                    wrote = fwrite(buf, 1, bytes_read, fd);
                    total_read += bytes_read;
                }
                if (ferror(fd)) {
                    fclose(fd);
                    unlink(file_name);
                    AM_LOG_ERROR(ar->instance_id, "%s unable to write to POST preservation file: %s",
                            thisfunc, file_name);
                    return AM_FILE_ERROR;
                }

            } else {

                /* process in-memory data */
                tmp_body = realloc(body, total_read + (tmp_sz > 0 ? tmp_sz : bytes_read) + 1);
                if (tmp_body == NULL) {
                    am_free(body);
                    AM_LOG_ERROR(ar->instance_id, "%s memory allocation failure", thisfunc);
                    return AM_ENOMEM;
                }
                body = tmp_body;

                if (tmp_sz > 0) {
                    /* write down whatever we have stored in the temp buffer */
                    memcpy(body + total_read, tmp, tmp_sz);
                    total_read += tmp_sz;
                    tmp_sz = 0;
                } else {
                    memcpy(body + total_read, buf, bytes_read);
                    total_read += bytes_read;
                }
                body[total_read] = '\0';
            }
        }
    }

    if (fd != NULL) {
        fclose(fd);
    }

    ar->post_data = body;
    ar->post_data_fn = ISVALID(file_name) ? strdup(file_name) : NULL;
    ar->post_data_sz = total_read;
    AM_LOG_DEBUG(ar->instance_id, "%s processed %ld bytes\n%s",
            thisfunc, total_read, ISVALID(body) ? body : LOGEMPTY(file_name));
    return AM_SUCCESS;
}

static am_status_t set_request_body(am_request_t *ar) {
    static const char *thisfunc = "set_request_body():";
    struct request *req = (struct request *) ar->ctx;
    if (req == NULL) return AM_EINVAL;

    if (ISVALID(ar->post_data_fn) && ar->post_data_sz > 0) {
        AM_ADD_HEADER_RESP_SYNTH(req, H_Content_Length, VRT_INT_string(req->ctx, ar->post_data_sz));
        AM_LOG_DEBUG(ar->instance_id, "%s preserved %d bytes", thisfunc,
                ar->post_data_sz);
    }
    return AM_SUCCESS;
}

unsigned int vmod_authenticate_wp(const struct vrt_ctx *ctx, struct vmod_priv *priv) {
    unsigned int result = 0;
    int status;
    am_request_t am_request;
    am_config_t *boot = NULL;
    VCL_IP client_addr = VRT_r_client_ip(ctx);
    struct agent_instance *settings = (struct agent_instance *) priv->priv;
    struct request *req = create_request(ctx);

    if (settings == NULL || req == NULL) {
        VSLb(ctx->vsl, SLT_VCL_Error, "am_vmod failed to allocate memory for agent instance data structures");
        return result;
    }

    if (settings->status != AM_SUCCESS) {
        VSLb(ctx->vsl, SLT_VCL_Error, "am_vmod failed to initialize agent instance, configuration: %s, error: %s",
                settings->conf_file, am_strerror(settings->status));
        AM_LOG_ERROR(settings->instance_id, "vmod_authenticate(): failed to initialize agent instance, error: %s",
                am_strerror(settings->status));
        return result;
    }

    status = am_get_agent_config(settings->instance_id, settings->conf_file, &boot);
    if (boot == NULL || status != AM_SUCCESS) {
        VSLb(ctx->vsl, SLT_VCL_Error, "am_vmod failed to get agent configuration instance, configuration: %s, error: %s",
                settings->conf_file, am_strerror(status));
        AM_LOG_ERROR(settings->instance_id, "vmod_authenticate(): failed to get agent configuration instance, error: %s",
                am_strerror(status));
        return result;
    }

    memset(&am_request, 0, sizeof (am_request_t));
    am_request.conf = boot;
    am_request.status = AM_ERROR;
    am_request.instance_id = settings->instance_id;
    am_request.ctx = req;
    am_request.method = am_method_str_to_num(VRT_r_req_method(ctx));
    am_request.content_type = get_request_header(ctx, HTTP_HDR_CONTENT_TYPE);
    am_request.cookies = get_request_header(ctx, HTTP_HDR_COOKIE);

    if (ISVALID(am_request.conf->client_ip_header)) {
        am_request.client_ip = (char *) get_request_header(ctx, am_request.conf->client_ip_header);
    }
    if (!ISVALID(am_request.client_ip) && client_addr != NULL) {
        am_request.client_ip = (char *) VRT_IP_string(ctx, client_addr);
    }
    if (ISVALID(am_request.conf->client_hostname_header)) {
        am_request.client_host = (char *) get_request_header(ctx, am_request.conf->client_hostname_header);
    }

    am_request.am_get_request_url_f = get_request_url;
    am_request.am_get_post_data_f = get_request_body;
    am_request.am_set_post_data_f = set_request_body;
    am_request.am_set_user_f = NULL; /* not supported in Varnish */
    am_request.am_set_header_in_request_f = set_header_in_request;
    am_request.am_add_header_in_response_f = add_header_in_response;
    am_request.am_set_cookie_f = set_cookie;
    am_request.am_set_custom_response_f = set_custom_response;
    am_request.am_set_method_f = set_method;
    am_request.am_get_request_header_f = get_request_header_ex;

    am_process_request(&am_request);

    req->status = am_status_value(am_request.status);
    result = am_request.status == AM_SUCCESS;

    AM_LOG_DEBUG(settings->instance_id, "vmod_authenticate(): exit status: %s (%d)",
            am_strerror(am_request.status), am_request.status);

    am_config_free(&am_request.conf);
    am_request_free(&am_request);

    return result;
}

static struct http *get_sess_http(const struct vrt_ctx *ctx, enum gethdr_e where) {
    if (ctx == NULL) return NULL;
    switch (where) {
        case HDR_REQ:
            return ctx->http_req;
        case HDR_BEREQ:
            return ctx->http_bereq;
        case HDR_BERESP:
            return ctx->http_beresp;
        case HDR_RESP:
            return ctx->http_resp;
        default:
            return NULL;
    }
}

void vmod_cleanup_wp(const struct vrt_ctx *ctx, struct vmod_priv *priv) {
    static const char *thisfunc = "vmod_cleanup():";
    struct request *request_list = pthread_getspecific(thread_key);
    VSLb(ctx->vsl, SLT_Debug, "%s xid: %d", thisfunc, ctx->req->sp->vxid);
    delete_request_list(&request_list);
}

void vmod_request_cleanup_wp(const struct vrt_ctx *ctx, struct vmod_priv *priv) {
    static const char *thisfunc = "vmod_request_cleanup():";
    struct request *e, *t, *tmp;
    struct request *request_list = pthread_getspecific(thread_key);

    if (request_list == NULL) return;

    VSLb(ctx->vsl, SLT_Debug, "%s xid: %d", thisfunc, ctx->req->sp->vxid);

    AM_LIST_FOR_EACH(request_list, e, t) {
        if (e->xid == ctx->req->sp->vxid) {
            VSLb(ctx->vsl, SLT_Debug, "%s removing request %d (%p)",
                    thisfunc, e->xid, THREAD_ID);
            am_free(e->body);
            /* remove request from the list */
            if (request_list == e) {
                request_list = e->next;
                pthread_setspecific(thread_key, request_list);
            } else {
                tmp = request_list;
                while (tmp->next && tmp->next != e) {
                    tmp = tmp->next;
                }
                if (tmp->next) {
                    tmp->next = e->next;
                }
            }
            free(e);
            e = NULL;
            break;
        }
    }
}

void vmod_done_wp(const struct vrt_ctx *ctx, struct vmod_priv *priv) {
    static const char *thisfunc = "vmod_done():";
    int status;
    struct http *hp;
    struct header *h, *t;
    struct agent_instance *settings = (struct agent_instance *) priv->priv;
    struct request *req = get_request(ctx);

    if (settings == NULL || req == NULL) {
        http_PutResponse(ctx->http_resp, "HTTP/1.1", am_status_value(AM_ERROR), NULL); /* fatal */
        return;
    }

    if (req->inauth) {
        status = req->status;
        if (status < 100 || status > 999) {
            status = 503;
        }

        VSLb(ctx->vsl, SLT_Debug, "%s xid: %d (%d)", thisfunc, ctx->req->sp->vxid, status);

        http_PutResponse(ctx->http_resp, "HTTP/1.1", status, NULL);
        AM_LOG_DEBUG(settings->instance_id, "%s setting response status %d",
                thisfunc, status);

        AM_LIST_FOR_EACH(req->headers, h, t) {

            if (h->type != AM_DONE || !ISVALID(h->value)) continue;

            hp = get_sess_http(ctx, h->where);
            if (hp == NULL) continue;

            AM_LOG_DEBUG(settings->instance_id, "%s setting response header \"%s\"",
                    thisfunc, h->value);

            http_SetHeader(hp, h->value);
        }

        if (ISVALID(req->body)) {
            struct vsb *vsb;
            CAST_OBJ_NOTNULL(vsb, ctx->specific, VSB_MAGIC);
            VSB_bcat(vsb, req->body, req->body_sz);
        }
    }

    vmod_request_cleanup_wp(ctx, priv);
}

void vmod_ok_wp(const struct vrt_ctx *ctx, struct vmod_priv *priv) {
    static const char *thisfunc = "vmod_ok():";
    int status;
    struct http *hp;
    struct header *h, *t;
    struct request *req = get_request(ctx);

    VSLb(ctx->vsl, SLT_Debug, "%s xid: %d", thisfunc, ctx->req->sp->vxid);

    if (req && req->inauth) {
        status = req->status;
        if (status < 100 || status > 999) {
            status = 503;
        }
        if (status == 200 && ctx->http_resp->status != 200
                && ctx->http_resp->status != 800) {
            /* pass backend response status to the caller */
            status = ctx->http_resp->status;
        }

        http_PutResponse(ctx->http_resp, "HTTP/1.1", status, NULL);

        AM_LIST_FOR_EACH(req->headers, h, t) {
            if (h->type != AM_SUCCESS) continue;

            hp = get_sess_http(ctx, h->where);
            if (hp == NULL) continue;

            if (h->unset || h->value == NULL) {
                http_Unset(hp, h->name);
                if (!h->unset) continue;
            }

            http_SetHeader(hp, h->value);
        }
    }

    vmod_request_cleanup_wp(ctx, priv);
}

static void cleanup_key(void *value) {
    free(value);
    pthread_setspecific(thread_key, NULL);
}

static void make_key() {
    pthread_key_create(&thread_key, cleanup_key);
}

static void init_cleanup(void *priv) {
    struct agent_instance *settings;
    pthread_mutex_lock(&init_mutex);
    settings = (struct agent_instance *) priv;
    if (settings && --n_init == 0) {
        am_shutdown_worker();
        am_shutdown(AM_DEFAULT_AGENT_ID);
        pthread_key_delete(thread_key);
        am_free(settings->conf_file);
        free(settings);
    }
    pthread_mutex_unlock(&init_mutex);
}

int event_function_wp(const struct vrt_ctx *ctx, struct vmod_priv *priv, enum vcl_event_e e) {
    struct agent_instance *settings;
    pthread_once(&thread_once, make_key);
    if (e == VCL_EVENT_LOAD) {
        settings = calloc(1, sizeof (struct agent_instance));
        AN(settings);
        settings->status = AM_ERROR;
        if (n_init++ == 0) {
            settings->status = am_init(AM_DEFAULT_AGENT_ID);
            /* am_init_worker(AM_DEFAULT_AGENT_ID); */
        }
        priv->priv = settings;
        priv->free = init_cleanup;
    }
    return 0;
}
