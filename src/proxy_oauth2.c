#include "auth_internal.h"
#include "proxy_runtime_internal.h"
#include <knurl/dispatch.h>
#include <knurl/reply.h>
#include <openssl/crypto.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

struct token_job {
    knurl_request *request;
    knurl_oauth2_client *client;
    knurl_job *job;
    uint64_t generation;
    char authorization[7 + 16385];
};

static knurl_status acquire(knurl_job *task, knurl_bytes input, void *userdata,
                            knurl_error *error)
{
    struct token_job *job = userdata;
    (void)input;
    if (knurl_job_cancelled(task))
        return KNURL_CANCELLED;
    memcpy(job->authorization, "Bearer ", 7);
    return knurl_oauth2_client_get(job->client, job->authorization + 7,
                                   sizeof(job->authorization) - 7,
                                   &job->generation, error);
}

static void finish(struct token_job *job, knurl_status status,
                   const knurl_error *error)
{
    knurl_request *request = job->request;
    knurl_reply *reply = NULL;
    if (request == NULL)
        return;
    job->request = NULL;
    if (status == KNURL_OK) {
        request->impl->proxy_authorization =
            knurl_copy_text(job->authorization);
        if (request->impl->proxy_authorization == NULL)
            status = KNURL_NOMEM;
        else
            request->impl->proxy_token_generation = job->generation;
    } else if (status != KNURL_TIMEOUT &&
               knurl_reply_new(&reply, NULL) == KNURL_OK &&
               knurl_reply_status(reply, 502, NULL) == KNURL_OK) {
        status = KNURL_OK;
    }
    (void)knurl_request_hook_resume(request, status, reply, error, NULL);
    if (reply != NULL)
        (void)knurl_reply_destroy(reply, NULL);
}

static void completed(knurl_job *task, const knurl_job_outcome *result,
                      void *userdata)
{
    (void)task;
    finish(userdata, result->cause, &result->error);
}

static void interrupted(knurl_job *task, knurl_status status, void *userdata)
{
    (void)task;
    finish(userdata, status, NULL);
}

static void retired(const knurl_job_outcome *result, void *userdata)
{
    struct token_job *job = userdata;
    (void)result;
    OPENSSL_cleanse(job->authorization, sizeof(job->authorization));
    knurl_oauth2_client_close(job->client);
    free(job);
}

static void cancel(void *userdata)
{
    struct token_job *job = userdata;
    job->request = NULL;
    (void)knurl_job_cancel(job->job, NULL);
}

knurl_status knurl_oauth2_proxy_admit(knurl_request *request,
                                      const knurl_reply *headers, int code,
                                      void *userdata, knurl_error *error)
{
    knurl_oauth2_client *client = userdata;
    struct token_job *job;
    knurl_executor *executor;
    knurl_job_config config;
    knurl_status status =
        knurl_auth_executor(request, KNURL_AUTH_PROXY, &executor, error);
    (void)headers;
    (void)code;
    if (status != KNURL_OK)
        return status;
    job = calloc(1, sizeof(*job));
    if (job == NULL)
        return knurl_fail(error, KNURL_NOMEM, "proxy_oauth2",
                          "allocating token job");
    job->request = request;
    job->client = client;
    knurl_job_config_init(&config);
    config.work = acquire;
    config.completed = completed;
    config.interrupted = interrupted;
    config.retired = retired;
    config.userdata = job;
    config.serial_key = client;
    config.timeout_ms = 12000;
    status = knurl_executor_submit(executor, &config, &job->job, error);
    if (status != KNURL_OK) {
        free(job);
        if (status == KNURL_AGAIN)
            return knurl_fail(error, KNURL_LIMIT, "proxy_oauth2",
                              "native token worker queue is full or busy");
        return status;
    }
    knurl_oauth2_client_retain(client);
    status = knurl_request_hook_wait(request, cancel, job, error);
    if (status != KNURL_OK)
        cancel(job);
    return status == KNURL_OK ? KNURL_AGAIN : status;
}

static const char *space(const char *start, const char *end)
{
    while (start < end && (*start == ' ' || *start == '\t'))
        ++start;
    return start;
}

static const char *word(const char *start, const char *end)
{
    while (start < end && *start != ' ' && *start != '\t' && *start != '=')
        ++start;
    return start;
}

static int invalid_token(const char *value)
{
    const char *start, *end, *name_end, *argument;
    int bearer = 0, quoted;
    for (start = value; *start != 0; start = *end ? end + 1 : end) {
        quoted = 0;
        for (end = start; *end != 0; ++end) {
            if (*end == '"')
                quoted = !quoted;
            else if (quoted && *end == '\\' && end[1] != 0)
                ++end;
            else if (!quoted && *end == ',')
                break;
        }
        start = space(start, end);
        name_end = word(start, end);
        argument = space(name_end, end);
        /* A bare name starts a new challenge. A name followed by '=' is a
         * parameter of the current challenge, including on later segments. */
        if (argument == end || *argument != '=') {
            bearer =
                name_end - start == 6 && strncasecmp(start, "Bearer", 6) == 0;
            start = argument;
            name_end = word(start, end);
            argument = space(name_end, end);
        }
        if (bearer && name_end - start == 5 &&
            strncasecmp(start, "error", 5) == 0 && argument < end &&
            *argument == '=') {
            const char *last = end;
            argument = space(argument + 1, end);
            while (last > argument && (last[-1] == ' ' || last[-1] == '\t'))
                --last;
            if (last - argument == 15 && *argument == '"' && last[-1] == '"') {
                ++argument;
                --last;
            }
            if (last - argument == 13 &&
                memcmp(argument, "invalid_token", 13) == 0)
                return 1;
        }
    }
    return 0;
}

void knurl_proxy_oauth2_response(knurl_request *request,
                                 knurl_oauth2_client *client, unsigned status,
                                 const knurl_proxy_headers *headers)
{
    size_t i;
    if (client == NULL || status != 401 || request == NULL ||
        request->impl->proxy_token_generation == 0)
        return;
    for (i = 0; i < headers->count; ++i) {
        if (strcasecmp(headers->fields[i].name, "WWW-Authenticate") == 0 &&
            invalid_token(headers->fields[i].value)) {
            knurl_oauth2_client_reject(client,
                                       request->impl->proxy_token_generation);
            return;
        }
    }
}
