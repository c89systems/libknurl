#include "kore.h"
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static void *traversal_malloc(size_t);
static void *traversal_realloc(void *, size_t);
#define kore_malloc traversal_malloc
#define kore_realloc traversal_realloc
#include <json.c>
#undef kore_malloc
#undef kore_realloc
static size_t allocations;
static int measuring;
#define CHECK(t)                                                               \
    do {                                                                       \
        if (!(t)) {                                                            \
            fprintf(stderr, "Kore JSON stack %d: %s\n", __LINE__, #t);         \
            abort();                                                           \
        }                                                                      \
    } while (0)
static void *traversal_malloc(size_t size)
{
    if (measuring)
        ++allocations;
    return kore_malloc(size);
}
static void *traversal_realloc(void *p, size_t size)
{
    if (measuring)
        ++allocations;
    return kore_realloc(p, size);
}
static void destroy(struct kore_json_item *root)
{
    allocations = 0;
    measuring = 1;
    kore_json_item_free(root);
    measuring = 0;
    CHECK(allocations == 0);
}
static void mixed(void)
{
    struct kore_json_item *root, *array, *subtree, *detached;
    struct kore_buf output;
    struct kore_json parsed;
    static const char expected[] =
        "{\"text\":\"a\\\"b\\\\c\\n\",\"number\":1.250000,\"integer\":-7,"
        "\"unsigned\":9,\"literals\":[true,false,null],\"empty\":{},\"array\":["
        "]}";
    root = kore_json_create_item(NULL, NULL, KORE_JSON_TYPE_OBJECT);
    kore_json_create_item(root, "text", KORE_JSON_TYPE_STRING, "a\"b\\c\n");
    kore_json_create_item(root, "number", KORE_JSON_TYPE_NUMBER, 1.25);
    kore_json_create_item(root, "integer", KORE_JSON_TYPE_INTEGER, (int64_t)-7);
    kore_json_create_item(root, "unsigned", KORE_JSON_TYPE_INTEGER_U64,
                          (u_int64_t)9);
    array = kore_json_create_item(root, "literals", KORE_JSON_TYPE_ARRAY);
    kore_json_create_item(array, NULL, KORE_JSON_TYPE_LITERAL, KORE_JSON_TRUE);
    kore_json_create_item(array, NULL, KORE_JSON_TYPE_LITERAL, KORE_JSON_FALSE);
    kore_json_create_item(array, NULL, KORE_JSON_TYPE_LITERAL, KORE_JSON_NULL);
    kore_json_create_item(root, "empty", KORE_JSON_TYPE_OBJECT);
    kore_json_create_item(root, "array", KORE_JSON_TYPE_ARRAY);
    kore_buf_init(&output, sizeof(expected));
    allocations = 0;
    measuring = 1;
    kore_json_item_tobuf(root, &output);
    measuring = 0;
    CHECK(allocations == 0 && output.offset == sizeof(expected) - 1u &&
          !memcmp(output.data, expected, output.offset));
    kore_buf_reset(&output);
    kore_json_item_tobuf(array, &output);
    CHECK(
        output.offset == strlen("\"literals\":[true,false,null]") &&
        !memcmp(output.data, "\"literals\":[true,false,null]", output.offset));
    destroy(root);
    kore_buf_cleanup(&output);
    root = kore_json_create_item(NULL, NULL, KORE_JSON_TYPE_ARRAY);
    subtree = kore_json_create_item(NULL, NULL, KORE_JSON_TYPE_OBJECT);
    kore_json_create_item(subtree, "x", KORE_JSON_TYPE_INTEGER, (int64_t)1);
    kore_json_item_attach(root, subtree);
    detached = kore_json_create_item(root, NULL, KORE_JSON_TYPE_ARRAY);
    kore_buf_init(&output, 32);
    kore_json_item_tobuf(subtree, &output);
    CHECK(output.offset == 7 && !memcmp(output.data, "{\"x\":1}", 7) &&
          subtree->parent == root);
    TAILQ_REMOVE(&root->data.items, subtree, list);
    destroy(subtree);
    CHECK(TAILQ_FIRST(&root->data.items) == detached);
    destroy(root);
    kore_buf_cleanup(&output);
    kore_json_init(&parsed, "{\"a\":[{},[],1]}", 15);
    CHECK(kore_json_parse(&parsed) == KORE_RESULT_OK);
    kore_json_cleanup(&parsed);
    destroy(NULL);
}
static void deep(void)
{
    struct kore_json_item *root, *item;
    struct kore_buf output;
    size_t i, length = 0, depth = 6000;
    char *expected = malloc(depth * 6u + 16u);
    CHECK(expected != NULL);
    root = kore_json_create_item(NULL, NULL, KORE_JSON_TYPE_OBJECT);
    item = root;
    expected[length++] = '{';
    for (i = 0; i < depth; ++i) {
        if (item->type == KORE_JSON_TYPE_OBJECT) {
            memcpy(expected + length, "\"n\":", 4);
            length += 4;
        }
        item = kore_json_create_item(
            item, item->type == KORE_JSON_TYPE_OBJECT ? "n" : NULL,
            i % 2 ? KORE_JSON_TYPE_OBJECT : KORE_JSON_TYPE_ARRAY);
        expected[length++] = i % 2 ? '{' : '[';
    }
    kore_json_create_item(item, "v", KORE_JSON_TYPE_INTEGER, (int64_t)3);
    memcpy(expected + length, "\"v\":3", 5);
    length += 5;
    for (i = depth; i != 0; --i)
        expected[length++] = (i - 1u) % 2 ? '}' : ']';
    expected[length++] = '}';
    kore_buf_init(&output, length);
    allocations = 0;
    measuring = 1;
    kore_json_item_tobuf(root, &output);
    measuring = 0;
    CHECK(allocations <= 10 && output.offset == length &&
          !memcmp(output.data, expected, length));
    for (item = root; item->type == KORE_JSON_TYPE_OBJECT ||
                      item->type == KORE_JSON_TYPE_ARRAY;
         item = TAILQ_FIRST(&item->data.items))
        CHECK(item->parent == NULL);
    destroy(root);
    kore_buf_cleanup(&output);
    free(expected);
    root = kore_json_create_item(NULL, NULL, KORE_JSON_TYPE_ARRAY);
    item = root;
    for (i = 0; i < 65536; ++i)
        item = kore_json_create_item(item, NULL, KORE_JSON_TYPE_ARRAY);
    destroy(root);
}
static void *stack_worker(void *data)
{
#if defined(KNURL_MUSL_STACK_CHECK) || defined(KNURL_DEBUG_TEST_THREADS)
    pthread_attr_t attributes;
    size_t stack_size;
    CHECK(pthread_getattr_np(pthread_self(), &attributes) == 0);
    CHECK(pthread_attr_getstacksize(&attributes, &stack_size) == 0 &&
          stack_size > 0 && stack_size < 256u * 1024u);
#ifdef KNURL_DEBUG_TEST_THREADS
    CHECK(stack_size == 128u * 1024u);
#endif
    CHECK(pthread_attr_destroy(&attributes) == 0);
#endif
    (void)data;
    mixed();
    deep();
    return NULL;
}
int main(void)
{
    pthread_t thread;
    kore_mem_init();
    CHECK(pthread_create(&thread, NULL, stack_worker, NULL) == 0);
    CHECK(pthread_join(thread, NULL) == 0);
    return 0;
}
