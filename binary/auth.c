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
    fputs(
        "Usage: knurl -a auth init DIRECTORY\n"
        "       knurl -a auth prune DIRECTORY\n"
        "       knurl -a auth user-password DIRECTORY USER  < password.txt\n"
        "       knurl -a auth user-email DIRECTORY USER EMAIL\n"
        "       knurl -a auth user-totp DIRECTORY USER [--rotate] [--users-id "
        "ID]\n"
        "       knurl -a auth user-enable|user-disable DIRECTORY USER "
        "[--users-id ID]\n"
        "       knurl -a auth client-issue|client-revoke DIRECTORY POLICY "
        "CLIENT\n"
        "       knurl -a auth -h | --help\n",
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
    } commands[] = {{"user-password", KNURL_AUTH_USER_PASSWORD, 3},
                    {"user-email", KNURL_AUTH_USER_EMAIL, 4},
                    {"user-enable", KNURL_AUTH_USER_ENABLE, 3},
                    {"user-disable", KNURL_AUTH_USER_DISABLE, 3}};
    knurl_auth_json *json = NULL;
    knurl_error error;
    knurl_status status;
    const char *value = NULL, *users_id = NULL;
    char secret[1026];
    size_t i;
    int enroll, issue, revoke;
    if (argc == 1 &&
        (strcmp(argv[0], "-h") == 0 || strcmp(argv[0], "--help") == 0))
        return usage(stdout);
    if (argc == 2 && strcmp(argv[0], "init") == 0) {
        status = knurl_auth_json_init(argv[1], &error);
        goto done;
    }
    if (argc == 2 && strcmp(argv[0], "prune") == 0) {
        status = knurl_auth_json_open(argv[1], &json, &error);
        if (status == KNURL_OK)
            status = knurl_auth_json_prune(json, &error);
        goto done;
    }
    if (argc < 3)
        return usage(stderr);
    if (argc >= 5 && strcmp(argv[argc - 2], "--users-id") == 0) {
        users_id = argv[argc - 1];
        argc -= 2;
    }
    enroll = strcmp(argv[0], "user-totp") == 0;
    issue = strcmp(argv[0], "client-issue") == 0;
    revoke = strcmp(argv[0], "client-revoke") == 0;
    for (i = 0; i < sizeof(commands) / sizeof(commands[0]); ++i)
        if (strcmp(argv[0], commands[i].name) == 0)
            break;
    if ((users_id != NULL && !enroll && strcmp(argv[0], "user-enable") != 0 &&
         strcmp(argv[0], "user-disable") != 0) ||
        (enroll && argc != 3 && (argc != 4 || strcmp(argv[3], "--rotate"))) ||
        ((issue || revoke) && argc != 4) ||
        (!enroll && !issue && !revoke &&
         (i == sizeof(commands) / sizeof(commands[0]) ||
          argc != commands[i].args)))
        return usage(stderr);
    status = knurl_auth_json_open(argv[1], &json, &error);
    if (status != KNURL_OK)
        goto done;
    if (enroll || issue || revoke) {
        if (enroll)
            status = knurl_auth_json_enroll_totp(json, users_id, argv[2],
                                                 argc == 4, secret, &error);
        else if (issue)
            status =
                knurl_auth_json_issue(json, argv[2], argv[3], secret, &error);
        else
            status = knurl_auth_json_revoke(json, argv[2], argv[3], &error);
        if (!revoke && status == KNURL_OK &&
            (puts(secret) == EOF || fflush(stdout) != 0)) {
            fputs(enroll ? "knurl auth: enrollment was stored but could not be "
                           "printed; run with --rotate\n"
                         : "knurl auth: credential was issued but could not be "
                           "printed; issue it again\n",
                  stderr);
            clear(secret, sizeof(secret));
            knurl_auth_json_close(json);
            return 1;
        }
        goto done;
    }
    if (commands[i].op == KNURL_AUTH_USER_PASSWORD) {
        if (!read_secret(secret, sizeof(secret), "Password")) {
            fputs("knurl auth: enter a single credential on standard input\n",
                  stderr);
            clear(secret, sizeof(secret));
            knurl_auth_json_close(json);
            return 2;
        }
        value = secret;
    } else if (commands[i].op == KNURL_AUTH_USER_EMAIL) {
        value = argv[3];
    }
    status = knurl_auth_json_admin(json, commands[i].op, users_id, argv[2],
                                   value, &error);
done:
    clear(secret, sizeof(secret));
    knurl_auth_json_close(json);
    if (status == KNURL_OK)
        return 0;
    fprintf(stderr, "knurl auth: %s\n", error.message);
    return 1;
}
