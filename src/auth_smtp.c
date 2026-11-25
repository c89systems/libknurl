#include "auth_internal.h"
#include <curl/curl.h>
#include <knurl/tls.h>
#include <openssl/crypto.h>
#include <stdio.h>
#include <string.h>

int knurl_auth_email_address(const char *address)
{
    static const char local[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789._+-";
    static const char domain[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789.-";
    size_t length;
    if (address == NULL || strlen(address) > 254)
        return 0;
    length = strspn(address, local);
    return length > 0 && address[length] == '@' && address[length + 1] != 0 &&
           address[length + 1 + strspn(address + length + 1, domain)] == 0;
}

struct mail {
    const char *data;
    size_t length;
};

static size_t read_mail(char *out, size_t size, size_t count, void *userdata)
{
    struct mail *mail = userdata;
    size_t length = size * count;
    if (length > mail->length)
        length = mail->length;
    memcpy(out, mail->data, length);
    mail->data += length;
    mail->length -= length;
    return length;
}

knurl_status knurl_auth_smtp_send(const char *recipient, const char *code,
                                  void *userdata, knurl_error *error)
{
    const knurl_auth_smtp *config = userdata;
    struct curl_slist *recipients;
    struct mail mail;
    char address[258], sender[258], message[768];
    const char *ca;
    CURL *curl;
    CURLcode status = CURLE_FAILED_INIT;
    if (!knurl_auth_email_address(recipient) ||
        !knurl_auth_email_address(config->from))
        return knurl_fail(error, KNURL_INVALID, "auth_email",
                          "invalid SMTP envelope address");
    curl = curl_easy_init();
    snprintf(address, sizeof(address), "<%s>", recipient);
    snprintf(sender, sizeof(sender), "<%s>", config->from);
    recipients = curl_slist_append(NULL, address);
    if (curl == NULL || recipients == NULL)
        goto done;
    snprintf(
        message, sizeof(message),
        "From: %s\r\nTo: %s\r\nSubject: Sign-in code\r\n"
        "MIME-Version: 1.0\r\nContent-Type: text/plain; charset=us-ascii\r\n"
        "\r\nYour sign-in code is %s. It expires in 10 minutes.\r\n",
        sender, address, code);
    mail.data = message;
    mail.length = strlen(message);
    ca = config->ca_file != NULL ? config->ca_file : knurl_tls_system_ca_file();
#define SET(option, value)                                                     \
    do {                                                                       \
        status = curl_easy_setopt(curl, option, value);                        \
        if (status != CURLE_OK)                                                \
            goto done;                                                         \
    } while (0)
    SET(CURLOPT_URL, config->url);
    SET(CURLOPT_PROTOCOLS_STR, "smtp,smtps");
    SET(CURLOPT_PROXY, "");
    SET(CURLOPT_NOSIGNAL, 1L);
    SET(CURLOPT_TIMEOUT_MS, config->timeout_ms ? config->timeout_ms : 10000L);
    SET(CURLOPT_USE_SSL, (long)CURLUSESSL_ALL);
    SET(CURLOPT_SSL_VERIFYPEER, 1L);
    SET(CURLOPT_SSL_VERIFYHOST, 2L);
    if (ca != NULL)
        SET(CURLOPT_CAINFO, ca);
    if (config->username != NULL)
        SET(CURLOPT_USERNAME, config->username);
    if (config->password != NULL)
        SET(CURLOPT_PASSWORD, config->password);
    SET(CURLOPT_MAIL_FROM, sender);
    SET(CURLOPT_MAIL_RCPT, recipients);
    SET(CURLOPT_UPLOAD, 1L);
    SET(CURLOPT_INFILESIZE_LARGE, (curl_off_t)mail.length);
    SET(CURLOPT_READFUNCTION, read_mail);
    SET(CURLOPT_READDATA, &mail);
#undef SET
    status = curl_easy_perform(curl);
done:
    curl_slist_free_all(recipients);
    curl_easy_cleanup(curl);
    OPENSSL_cleanse(message, sizeof(message));
    return status == CURLE_OK ? KNURL_OK
                              : knurl_fail(error, KNURL_BACKEND, "auth_email",
                                           curl_easy_strerror(status));
}
