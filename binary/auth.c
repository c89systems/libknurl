#define _POSIX_C_SOURCE 200809L
#include "auth.h"
#include <knurl/auth.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <termios.h>
#include <unistd.h>

static int usage(FILE *out)
{
    fputs("Usage: knurl auth init DIRECTORY\n"
          "       knurl auth prune DIRECTORY\n"
          "       knurl auth user-password DIRECTORY USER  < password.txt\n"
          "       knurl auth user-email DIRECTORY USER EMAIL\n"
          "       knurl auth user-totp DIRECTORY USER  < seed.txt\n"
          "       knurl auth user-enable|user-disable DIRECTORY USER\n"
          "       knurl auth client-issue|client-revoke DIRECTORY CLIENT\n",
          out);
    return out == stdout ? 0 : 2;
}

static void clear(char *text, size_t length)
{
    volatile char *p = text;
    while (length-- != 0)
        *p++ = 0;
}

static volatile sig_atomic_t interrupted;

static void interrupt(int signal_number) { interrupted = signal_number; }

static int read_secret(char *out, size_t capacity, const char *label)
{
    static const int signals[] = {SIGINT, SIGTERM, SIGHUP, SIGQUIT, SIGTSTP};
    struct sigaction handler, previous[sizeof(signals) / sizeof(signals[0])];
    struct termios saved, quiet;
    int ch = 0, tty = isatty(STDIN_FILENO);
    int changed = 0, ok = 1;
    size_t length = 0, count = 0;
    if (tty) {
        if (tcgetattr(STDIN_FILENO, &saved) != 0)
            return 0;
        memset(&handler, 0, sizeof(handler));
        sigemptyset(&handler.sa_mask);
        handler.sa_handler = interrupt;
        for (count = 0; count < sizeof(signals) / sizeof(signals[0]); ++count)
            if (sigaction(signals[count], &handler, &previous[count]) != 0)
                break;
        quiet = saved;
        quiet.c_lflag &= (tcflag_t)~ECHO;
        if (count == sizeof(signals) / sizeof(signals[0]) &&
            tcsetattr(STDIN_FILENO, TCSAFLUSH, &quiet) == 0) {
            changed = 1;
            fprintf(stderr, "%s: ", label);
            fflush(stderr);
        } else {
            ok = 0;
        }
    }
    while (ok && interrupted == 0 && length + 1 < capacity &&
           (ch = fgetc(stdin)) != EOF && ch != '\n' && ch != 0)
        out[length++] = (char)ch;
    if (changed) {
        if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &saved) != 0)
            ok = 0;
        fputc('\n', stderr);
    }
    while (count != 0) {
        --count;
        (void)sigaction(signals[count], &previous[count], NULL);
    }
    if (interrupted != 0) {
        (void)raise((int)interrupted);
        ok = 0;
    }
    out[length] = 0;
    return ok && length != 0 && (ch == '\n' || (ch == EOF && !ferror(stdin)));
}

int knurl_auth_cli(int argc, char **argv)
{
    static const struct {
        const char *name;
        knurl_auth_admin op;
        int args;
    } commands[] = {{"user-password", KNURL_AUTH_USER_PASSWORD, 5},
                    {"user-email", KNURL_AUTH_USER_EMAIL, 6},
                    {"user-totp", KNURL_AUTH_USER_TOTP, 5},
                    {"user-enable", KNURL_AUTH_USER_ENABLE, 5},
                    {"user-disable", KNURL_AUTH_USER_DISABLE, 5},
                    {"client-revoke", KNURL_AUTH_CLIENT_REVOKE, 5},
                    {"client-issue", KNURL_AUTH_CLIENT_SET, 5}};
    knurl_auth_json *json = NULL;
    knurl_error error;
    knurl_status status;
    const char *value = NULL;
    char secret[1026];
    size_t i;
    if (argc == 3 && strcmp(argv[2], "--help") == 0)
        return usage(stdout);
    if (argc == 4 && strcmp(argv[2], "init") == 0) {
        status = knurl_auth_json_init(argv[3], &error);
        goto done;
    }
    if (argc == 4 && strcmp(argv[2], "prune") == 0) {
        status = knurl_auth_json_open(argv[3], &json, &error);
        if (status == KNURL_OK)
            status = knurl_auth_json_prune(json, &error);
        goto done;
    }
    if (argc < 5)
        return usage(stderr);
    for (i = 0; i < sizeof(commands) / sizeof(commands[0]); ++i)
        if (strcmp(argv[2], commands[i].name) == 0)
            break;
    if (i == sizeof(commands) / sizeof(commands[0]) || argc != commands[i].args)
        return usage(stderr);
    status = knurl_auth_json_open(argv[3], &json, &error);
    if (status != KNURL_OK)
        goto done;
    if (commands[i].op == KNURL_AUTH_USER_PASSWORD ||
        commands[i].op == KNURL_AUTH_USER_TOTP) {
        if (!read_secret(secret, sizeof(secret),
                         commands[i].op == KNURL_AUTH_USER_PASSWORD
                             ? "Password"
                             : "Base32 TOTP seed")) {
            fputs("knurl auth: enter a single credential on standard input\n",
                  stderr);
            clear(secret, sizeof(secret));
            knurl_auth_json_close(json);
            return 2;
        }
        value = secret;
    } else if (commands[i].op == KNURL_AUTH_USER_EMAIL) {
        value = argv[5];
    }
    if (commands[i].op == KNURL_AUTH_CLIENT_SET) {
        status = knurl_auth_json_issue(json, argv[4], secret, &error);
        if (status == KNURL_OK &&
            (puts(secret) == EOF || fflush(stdout) != 0)) {
            fputs("knurl auth: credential was issued but could not be printed; "
                  "issue it again\n",
                  stderr);
            clear(secret, sizeof(secret));
            knurl_auth_json_close(json);
            return 1;
        }
    } else {
        status =
            knurl_auth_json_admin(json, commands[i].op, argv[4], value, &error);
    }
done:
    clear(secret, sizeof(secret));
    knurl_auth_json_close(json);
    if (status == KNURL_OK)
        return 0;
    fprintf(stderr, "knurl auth: %s\n", error.message);
    return 1;
}
