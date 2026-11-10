#include "kore.h"
#include "xml-stack-fixture.h"
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
static void check(int condition)
{
    if (!condition)
        abort();
}
static double median(double *values, size_t count)
{
    size_t i, j;
    double value;
    for (i = 1; i < count; ++i) {
        value = values[i];
        for (j = i; j && values[j - 1] > value; --j)
            values[j] = values[j - 1];
        values[j] = value;
    }
    return values[count / 2u];
}
static void xml_benchmark(size_t depth)
{
    struct xml_stack_fixture f;
    knurl_error error;
    double values[5], seconds;
    clock_t start;
    size_t sample, count, i;
    check(xml_stack_fixture_new(&f, depth) == 0);
    for (sample = 0; sample < 5; ++sample) {
        count = 0;
        start = clock();
        do {
            for (i = 0; i < 32; ++i)
                check(xml_stack_parse(&f, depth + 2u, &error) == KNURL_OK &&
                      !strcmp(f.record.text, "payload"));
            count += 32;
            seconds = (double)(clock() - start) / CLOCKS_PER_SEC;
        } while (seconds < 0.05);
        values[sample] = seconds * 1e9 / (double)count;
    }
    printf("xml-%lu %.3f\n", (unsigned long)depth, median(values, 5));
    xml_stack_fixture_destroy(&f);
}
static struct kore_json_item *tree(size_t depth, int wide)
{
    struct kore_json_item *root, *item;
    size_t i;
    root = kore_json_create_item(NULL, NULL, KORE_JSON_TYPE_OBJECT);
    item = root;
    for (i = 0; i < depth; ++i) {
        if (!wide)
            item = kore_json_create_item(item, "n", KORE_JSON_TYPE_OBJECT);
        kore_json_create_item(item, "v", KORE_JSON_TYPE_INTEGER, (int64_t)i);
    }
    return root;
}
static void kore_benchmark(size_t depth, int wide, int release)
{
    struct kore_json_item *root, *batch[32];
    struct kore_buf output;
    double values[5], seconds;
    clock_t start, ticks;
    size_t sample, count, i;
    root = tree(depth, wide);
    kore_buf_init(&output, depth * 32u + 64u);
    for (sample = 0; sample < 5; ++sample) {
        count = 0;
        ticks = 0;
        do {
            if (release)
                for (i = 0; i < 32; ++i)
                    batch[i] = tree(depth, wide);
            start = clock();
            for (i = 0; i < 32; ++i) {
                if (release)
                    kore_json_item_free(batch[i]);
                else {
                    kore_buf_reset(&output);
                    kore_json_item_tobuf(root, &output);
                }
            }
            ticks += clock() - start;
            count += 32;
            seconds = (double)ticks / CLOCKS_PER_SEC;
        } while (seconds < 0.05);
        values[sample] = seconds * 1e9 / (double)count;
    }
    printf("kore-%s-%s-%lu %.3f\n", release ? "free" : "write",
           wide ? "wide" : "deep", (unsigned long)depth, median(values, 5));
    kore_json_item_free(root);
    kore_buf_cleanup(&output);
}
static int compare(const char *before, const char *after)
{
    struct sample {
        char name[64];
        double old[3], current[3];
    } samples[8];
    FILE *old = fopen(before, "r"), *current = fopen(after, "r");
    char name[64], candidate[64];
    size_t i, round;
    double previous, next, ratio;
    int failed = 0;
    check(old != NULL && current != NULL);
    for (round = 0; round < 3; ++round)
        for (i = 0; i < 8; ++i) {
            check(fscanf(old, "%63s %lf", name, &previous) == 2 &&
                  fscanf(current, "%63s %lf", candidate, &next) == 2 &&
                  !strcmp(name, candidate) && previous > 0 && next > 0);
            if (round == 0)
                strcpy(samples[i].name, name);
            check(!strcmp(samples[i].name, name));
            samples[i].old[round] = previous;
            samples[i].current[round] = next;
        }
    check(fscanf(old, "%63s", name) == EOF &&
          fscanf(current, "%63s", name) == EOF);
    fclose(old);
    fclose(current);
    for (i = 0; i < 8; ++i) {
        previous = median(samples[i].old, 3);
        next = median(samples[i].current, 3);
        ratio = next / previous;
        printf("%s baseline_ns=%.3f candidate_ns=%.3f ratio=%.4f\n",
               samples[i].name, previous, next, ratio);
        if (!(ratio <= 1.05)) {
            fprintf(stderr, "%s exceeds the 5%% CPU-time regression budget\n",
                    samples[i].name);
            failed = 1;
        }
    }
    return failed;
}
int main(int argc, char **argv)
{
    if (argc == 4 && !strcmp(argv[1], "--compare"))
        return compare(argv[2], argv[3]);
    check(argc == 1);
    kore_mem_init();
    xml_benchmark(0);
    xml_benchmark(3);
    xml_benchmark(128);
    kore_benchmark(8, 1, 0);
    kore_benchmark(128, 1, 0);
    kore_benchmark(128, 0, 0);
    kore_benchmark(128, 1, 1);
    kore_benchmark(128, 0, 1);
    return 0;
}
