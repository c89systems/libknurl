#include "auth_internal.h"
#include <openssl/crypto.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

struct auth_endpoint {
    knurl_auth *auth;
    knurl_auth_endpoint kind;
};

static int digit(unsigned char ch)
{
    if (ch >= '0' && ch <= '9')
        return ch - '0';
    ch = (unsigned char)(ch | 32);
    return ch >= 'a' && ch <= 'f' ? ch - 'a' + 10 : -1;
}

static int decode(const char *p, const char *end, char *out, size_t capacity)
{
    size_t length = 0;
    while (p < end) {
        int ch = (unsigned char)*p++;
        if (ch == '+') {
            ch = ' ';
        } else if (ch == '%') {
            int a, b;
            if (end - p < 2 || (a = digit(p[0])) < 0 || (b = digit(p[1])) < 0)
                return 0;
            p += 2;
            ch = a * 16 + b;
        }
        if (ch == 0 || length + 1 >= capacity)
            return 0;
        out[length++] = (char)ch;
    }
    out[length] = 0;
    return length != 0;
}

static int fields(knurl_bytes input, struct knurl_auth_job *job)
{
    const char *p = input.data, *limit, *end, *separator;
    char key[16];
    unsigned int required = KNURL_AUTH_FIELD_USER, optional = 0, seen = 0, bit;
    size_t capacity;
    char *target;
    if (input.length == 0)
        return 0;
    if (job->endpoint == KNURL_AUTH_LOGIN) {
        if (job->auth->config.login != KNURL_AUTH_LOGIN_EMAIL)
            required |= KNURL_AUTH_FIELD_PASSWORD;
        if (job->auth->config.login == KNURL_AUTH_LOGIN_EMAIL)
            required |= KNURL_AUTH_FIELD_CODE;
        if (job->auth->config.login == KNURL_AUTH_LOGIN_TOTP)
            optional |= KNURL_AUTH_FIELD_CODE;
    }
    if (job->endpoint == KNURL_AUTH_WORKFLOW) {
        optional = KNURL_AUTH_FIELD_USER | KNURL_AUTH_FIELD_PASSWORD |
                   KNURL_AUTH_FIELD_CODE | KNURL_AUTH_FIELD_WORKFLOW;
        required = 0;
    }
    limit = p + input.length;
    while (p < limit) {
        end = memchr(p, '&', (size_t)(limit - p));
        if (end == NULL)
            end = limit;
        separator = memchr(p, '=', (size_t)(end - p));
        if (separator == NULL || !decode(p, separator, key, sizeof(key)))
            return 0;
        if (strcmp(key, "username") == 0) {
            bit = KNURL_AUTH_FIELD_USER;
            target = job->subject;
            capacity = sizeof(job->subject);
        } else if (strcmp(key, "password") == 0) {
            bit = KNURL_AUTH_FIELD_PASSWORD;
            target = job->credential;
            capacity = sizeof(job->credential);
        } else if (strcmp(key, "code") == 0) {
            bit = KNURL_AUTH_FIELD_CODE;
            target = job->code;
            capacity = sizeof(job->code);
        } else if (strcmp(key, "workflow") == 0) {
            bit = KNURL_AUTH_FIELD_WORKFLOW;
            target = job->workflow;
            capacity = sizeof(job->workflow);
        } else {
            return 0;
        }
        if (!((required | optional) & bit) || (seen & bit) ||
            !decode(separator + 1, end, target, capacity))
            return 0;
        seen |= bit;
        p = end < limit ? end + 1 : end;
    }
    job->fields = seen;
    if ((seen & KNURL_AUTH_FIELD_CODE) &&
        (strlen(job->code) != 6 || strspn(job->code, "0123456789") != 6))
        return 0;
    if (job->endpoint == KNURL_AUTH_WORKFLOW) {
        if (seen & KNURL_AUTH_FIELD_WORKFLOW)
            return (seen == (KNURL_AUTH_FIELD_WORKFLOW |
                             KNURL_AUTH_FIELD_PASSWORD) ||
                    seen ==
                        (KNURL_AUTH_FIELD_WORKFLOW | KNURL_AUTH_FIELD_CODE)) &&
                   strspn(job->workflow, "0123456789abcdef") == 64;
        required = KNURL_AUTH_FIELD_USER;
        if (!(job->auth->config.factors & KNURL_AUTH_FACTOR_EMAIL))
            required |= KNURL_AUTH_FIELD_PASSWORD;
        return seen == required && knurl_auth_name(job->subject);
    }
    return (seen & required) == required && knurl_auth_name(job->subject);
}

static int enroll_fields(knurl_bytes body, struct knurl_auth_job *job)
{
    const char *data = body.data;
    if (body.length == 0)
        return 1;
    return body.length == 11 && memcmp(data, "code=", 5) == 0 &&
           decode(data + 5, data + 11, job->code, sizeof(job->code)) &&
           strspn(job->code, "0123456789") == 6;
}

static int same_origin(knurl_request *request)
{
    knurl_bytes origin, site;
    const char *authority = request->impl->authority.data;
    if (knurl_request_header_first(request, "sec-fetch-site", &site, NULL) ==
            KNURL_OK &&
        strcmp(site.data, "same-origin") && strcmp(site.data, "none"))
        return 0;
    if (knurl_request_header_first(request, "origin", &origin, NULL) !=
        KNURL_OK)
        return 1;
    return origin.length == request->impl->authority.length + 8 &&
           strncmp(origin.data, "https://", 8) == 0 &&
           knurl_text_equal_case((const char *)origin.data + 8, authority);
}

static knurl_status body_end(knurl_request *request, void *userdata,
                             knurl_error *error)
{
    const struct auth_endpoint *endpoint = userdata;
    struct knurl_auth_job *job;
    knurl_status status;
    knurl_bytes body;
    char email_workflow[65];
    int valid = 1;
    job = calloc(1, sizeof(*job));
    if (job == NULL)
        return knurl_fail(error, KNURL_NOMEM, "auth_route",
                          "allocating request");
    job->auth = endpoint->auth;
    job->request = request;
    job->endpoint = endpoint->kind;
    if (job->endpoint == KNURL_AUTH_LOGOUT) {
        if (!knurl_auth_cookie(request, endpoint->auth, job->cookie))
            job->cookie[0] = 0;
    } else if (knurl_request_body_bytes(request, &body, error) == KNURL_OK) {
        valid = job->endpoint == KNURL_AUTH_TOTP_ENROLL_ENDPOINT
                    ? enroll_fields(body, job)
                    : fields(body, job);
        if (body.length != 0)
            OPENSSL_cleanse((void *)body.data, body.length);
    } else {
        valid = 0;
    }
    if (!valid) {
        knurl_auth_job_free(job);
        return knurl_auth_reply(request, 400, NULL, NULL, error);
    }
    if (job->endpoint == KNURL_AUTH_TOTP_ENROLL_ENDPOINT &&
        !knurl_auth_cookie(request, endpoint->auth, job->cookie)) {
        knurl_auth_job_free(job);
        return knurl_auth_reply(request, 401, NULL, NULL, error);
    }
    email_workflow[0] = 0;
    if (job->endpoint == KNURL_AUTH_WORKFLOW) {
        if (!knurl_auth_digest(NULL, "workflow", "route",
                               request->impl->raw_path.data,
                               job->path_digest) ||
            !knurl_auth_random_hex(job->next_workflow, 32)) {
            knurl_auth_job_free(job);
            return knurl_auth_reply(request, 503, NULL, NULL, error);
        }
        if (job->fields == KNURL_AUTH_FIELD_USER)
            memcpy(email_workflow, job->next_workflow, sizeof(email_workflow));
    }
    status = knurl_auth_start(job, error);
    if (status != KNURL_OK)
        return knurl_auth_reply(request, 503, NULL, NULL, error);
    if (endpoint->kind == KNURL_AUTH_EMAIL_START)
        return knurl_auth_reply(request, 202, NULL, NULL, error);
    if (email_workflow[0] != 0) {
        status = knurl_auth_workflow_pending(request, email_workflow,
                                             KNURL_AUTH_FACTOR_EMAIL, error);
        OPENSSL_cleanse(email_workflow, sizeof(email_workflow));
        return status;
    }
    return KNURL_OK;
}

static knurl_status headers(knurl_request *request, knurl_request_plan *plan,
                            void *userdata, knurl_error *error)
{
    static const char form[] = "application/x-www-form-urlencoded";
    const struct auth_endpoint *endpoint = userdata;
    knurl_bytes type;
    const char *value;
    int status = 0;
    if (!request->impl->tls || !same_origin(request))
        status = 403;
    else if (endpoint->kind != KNURL_AUTH_LOGOUT) {
        if (knurl_request_header_first(request, "content-type", &type, NULL) !=
            KNURL_OK)
            status = 415;
        else {
            value = type.data;
            if (strncasecmp(value, form, sizeof(form) - 1) != 0 ||
                (value[sizeof(form) - 1] != 0 &&
                 value[sizeof(form) - 1] != ';'))
                status = 415;
        }
    }
    if (status != 0) {
        plan->disposition = KNURL_REQUEST_REPLY;
        return knurl_auth_reply(request, status, NULL, NULL, error);
    }
    plan->on_body_end = body_end;
    plan->userdata = userdata;
    return KNURL_OK;
}

static void release(void *userdata)
{
    struct auth_endpoint *endpoint = userdata;
    knurl_auth_close(endpoint->auth);
    free(endpoint);
}

knurl_status knurl_auth_route(knurl_router *router, const char *path,
                              knurl_auth *auth, knurl_auth_endpoint kind,
                              knurl_route **out, knurl_error *error)
{
    struct auth_endpoint *endpoint;
    knurl_route_config config;
    knurl_status status;
    if (out != NULL)
        *out = NULL;
    if (router == NULL || path == NULL || auth == NULL || out == NULL ||
        kind < KNURL_AUTH_LOGIN || kind > KNURL_AUTH_WORKFLOW ||
        (kind != KNURL_AUTH_WORKFLOW &&
         auth->config.method != KNURL_AUTH_SESSION) ||
        (kind == KNURL_AUTH_WORKFLOW && auth->config.factors == 0) ||
        (kind == KNURL_AUTH_LOGIN && auth->config.factors != 0) ||
        (kind == KNURL_AUTH_EMAIL_START &&
         auth->config.login != KNURL_AUTH_LOGIN_EMAIL))
        return knurl_fail(
            error, KNURL_INVALID, "auth_route",
            "valid browser policy, endpoint and path are required");
    endpoint = malloc(sizeof(*endpoint));
    if (endpoint == NULL)
        return knurl_fail(error, KNURL_NOMEM, "auth_route",
                          "allocating endpoint");
    endpoint->auth = auth;
    endpoint->kind = kind;
    knurl_route_config_init(&config);
    config.method = KNURL_HTTP_POST;
    config.path = path;
    config.userdata = endpoint;
    config.on_headers = headers;
    if (kind != KNURL_AUTH_LOGOUT) {
        knurl_body_policy_init_buffered(&config.body);
        config.body.max_bytes.low = 4096;
        config.body.memory_buffer_limit_bytes = 4096;
        config.body.disk_spool_disabled = 1;
    }
    status = knurl_router_route(router, &config, out, error);
    if (status == KNURL_OK) {
        knurl_auth_retain(auth);
        (*out)->impl->release_userdata = release;
    } else {
        free(endpoint);
    }
    return status;
}
