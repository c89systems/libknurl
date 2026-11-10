#define _POSIX_C_SOURCE 200809L
#include <knurl/knurl.h>
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
    char path[] = "knurl-source.XXXXXX", buffer[8];
    knurl_source *source = NULL;
    knurl_source *reader = NULL;
    knurl_body_result result;
    knurl_error error;
    size_t count;
    int fd;
    knurl_body_result_init(&result);
    result.kind = KNURL_BODY_FILE;
    result.path = ".";
    CHECK(knurl_body_result_open_reader(&result, &reader, &error) == KNURL_IO &&
          reader == NULL);
    CHECK(knurl_source_file(NULL, &source, &error) == KNURL_INVALID);
    CHECK(knurl_source_file("", &source, &error) == KNURL_INVALID);
    CHECK(knurl_source_file(".", &source, &error) == KNURL_IO);
    fd = mkstemp(path);
    CHECK(fd >= 0 && write(fd, "A\0BC", 4) == 4);
    CHECK(knurl_source_file(path, &source, &error) == KNURL_OK);
    result.path = path;
    result.length.low = 4;
    CHECK(knurl_body_result_open_reader(&result, &reader, &error) == KNURL_OK);
    CHECK(knurl_source_read(reader, buffer, sizeof(buffer), &count, &error) ==
              KNURL_OK &&
          count == 4 && memcmp(buffer, "A\0BC", 4) == 0);
    CHECK(knurl_source_reset(reader, &error) == KNURL_OK);
    CHECK(knurl_source_read(reader, buffer, 1, &count, &error) == KNURL_OK &&
          count == 1 && buffer[0] == 'A');
    CHECK(knurl_source_destroy(reader, &error) == KNURL_OK);
    CHECK(write(fd, "growth", 6) == 6 && unlink(path) == 0);
    CHECK(knurl_source_read(source, buffer, sizeof(buffer), &count, &error) ==
          KNURL_OK);
    CHECK(count == 4 && memcmp(buffer, "A\0BC", 4) == 0);
    CHECK(knurl_source_read(source, buffer, sizeof(buffer), &count, &error) ==
          KNURL_EOF);
    CHECK(knurl_source_reset(source, &error) == KNURL_OK);
    CHECK(knurl_source_read(source, buffer, 1, &count, &error) == KNURL_OK &&
          count == 1 && buffer[0] == 'A');
    CHECK(ftruncate(fd, 0) == 0 && close(fd) == 0);
    CHECK(knurl_source_read(source, buffer, sizeof(buffer), &count, &error) ==
              KNURL_IO &&
          count == 0);
    CHECK(knurl_source_destroy(source, &error) == KNURL_OK);
    CHECK(knurl_source_file(path, &source, &error) == KNURL_IO &&
          source == NULL);
    result.length.low = 0;
    CHECK(knurl_body_result_open_reader(&result, &reader, &error) == KNURL_IO &&
          reader == NULL);
    CHECK(mkfifo(path, 0600) == 0);
    CHECK(knurl_source_file(path, &source, &error) == KNURL_IO &&
          source == NULL);
    CHECK(knurl_body_result_open_reader(&result, &reader, &error) == KNURL_IO &&
          reader == NULL && error.status == KNURL_IO);
    CHECK(unlink(path) == 0);
    fd = open(path, O_WRONLY | O_CREAT | O_EXCL, 0600);
    CHECK(fd >= 0 && close(fd) == 0);
    result.length.low = 1;
    CHECK(knurl_body_result_open_reader(&result, &reader, &error) == KNURL_IO &&
          reader == NULL);
    result.length.low = 0;
    CHECK(knurl_body_result_open_reader(&result, &reader, &error) == KNURL_OK);
    CHECK(knurl_source_read(reader, buffer, sizeof(buffer), &count, &error) ==
              KNURL_EOF &&
          count == 0);
    CHECK(knurl_source_destroy(reader, &error) == KNURL_OK);
    CHECK(knurl_source_file(path, &source, &error) == KNURL_OK);
    CHECK(knurl_source_read(source, buffer, sizeof(buffer), &count, &error) ==
              KNURL_EOF &&
          count == 0);
    CHECK(knurl_source_destroy(source, &error) == KNURL_OK &&
          unlink(path) == 0);
    return 0;
}
