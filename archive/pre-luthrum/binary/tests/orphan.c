/* Test-only PID 1 launcher; it is never installed or packaged. */
#define _POSIX_C_SOURCE 200809L
#include <sys/wait.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>
int main(void)
{
    pid_t child, orphan;
    int status;
    FILE *file;
    struct timespec delay;
    if (getpid() != 1)
        return 1;
    child = fork();
    if (child < 0)
        return 1;
    if (child == 0) {
        orphan = fork();
        if (orphan < 0)
            _exit(1);
        if (orphan == 0) {
            delay.tv_sec = 0;
            delay.tv_nsec = 500000000;
            (void)nanosleep(&delay, NULL);
            _exit(0);
        }
        file = fopen("/tmp/orphan.pid", "w");
        if (file == NULL)
            _exit(1);
        if (fprintf(file, "%ld\n", (long)orphan) < 0 || fclose(file) != 0)
            _exit(1);
        _exit(0);
    }
    if (waitpid(child, &status, 0) != child || !WIFEXITED(status) ||
        WEXITSTATUS(status))
        return 1;
    execl("/bin/knurl", "knurl", "/server.lua", (char *)NULL);
    return 1;
}
