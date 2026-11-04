#define _POSIX_C_SOURCE 200809L
#include <rotmos/rotmos.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(x)                                                               \
    do {                                                                       \
        if (!(x)) {                                                            \
            fprintf(stderr, "source file line %d: %s\n", __LINE__, #x);        \
            return 1;                                                          \
        }                                                                      \
    } while (0)
int main(void)
{
    char path[] = "rotmos-source.XXXXXX", buffer[8];
    rotmos_source *source = NULL;
    rotmos_error error;
    size_t count;
    int fd;
    CHECK(rotmos_source_file(NULL, &source, &error) == ROTMOS_INVALID);
    CHECK(rotmos_source_file("", &source, &error) == ROTMOS_INVALID);
    CHECK(rotmos_source_file(".", &source, &error) == ROTMOS_IO);
    fd = mkstemp(path);
    CHECK(fd >= 0 && write(fd, "A\0BC", 4) == 4);
    CHECK(rotmos_source_file(path, &source, &error) == ROTMOS_OK);
    CHECK(write(fd, "growth", 6) == 6 && unlink(path) == 0);
    CHECK(rotmos_source_read(source, buffer, sizeof(buffer), &count, &error) ==
          ROTMOS_OK);
    CHECK(count == 4 && memcmp(buffer, "A\0BC", 4) == 0);
    CHECK(rotmos_source_read(source, buffer, sizeof(buffer), &count, &error) ==
          ROTMOS_EOF);
    CHECK(rotmos_source_reset(source, &error) == ROTMOS_OK);
    CHECK(rotmos_source_read(source, buffer, 1, &count, &error) == ROTMOS_OK &&
          count == 1 && buffer[0] == 'A');
    CHECK(ftruncate(fd, 0) == 0 && close(fd) == 0);
    CHECK(rotmos_source_read(source, buffer, sizeof(buffer), &count, &error) ==
              ROTMOS_IO &&
          count == 0);
    CHECK(rotmos_source_destroy(source, &error) == ROTMOS_OK);
    CHECK(rotmos_source_file(path, &source, &error) == ROTMOS_IO &&
          source == NULL);
    CHECK(mkfifo(path, 0600) == 0);
    CHECK(rotmos_source_file(path, &source, &error) == ROTMOS_IO &&
          source == NULL);
    CHECK(unlink(path) == 0);
    fd = open(path, O_WRONLY | O_CREAT | O_EXCL, 0600);
    CHECK(fd >= 0 && close(fd) == 0);
    CHECK(rotmos_source_file(path, &source, &error) == ROTMOS_OK);
    CHECK(rotmos_source_read(source, buffer, sizeof(buffer), &count, &error) ==
              ROTMOS_EOF &&
          count == 0);
    CHECK(rotmos_source_destroy(source, &error) == ROTMOS_OK &&
          unlink(path) == 0);
    return 0;
}
