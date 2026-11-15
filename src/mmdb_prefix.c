#include "mmdb_prefix.h"
#include <stdlib.h>
#include <string.h>
struct knurl_mmdb_prefix_node {
    struct knurl_mmdb_prefix_node *parent, *child[2], *older, *newer,
        *allocated_next;
    unsigned char key[16], bits, family, leaf;
    char code[3];
};
static unsigned int bit(const unsigned char *key, unsigned int index)
{
    return (key[index / 8] >> (7 - index % 8)) & 1;
}
static int matches(const unsigned char *a, const unsigned char *b,
                   unsigned int bits)
{
    unsigned int bytes = bits / 8, rest = bits % 8;
    return memcmp(a, b, bytes) == 0 &&
           (!rest || ((a[bytes] ^ b[bytes]) & (255u << (8 - rest))) == 0);
}
static unsigned int common(const unsigned char *a, const unsigned char *b,
                           unsigned int maximum)
{
    unsigned int at = 0;
    while (at + 8 <= maximum && a[at / 8] == b[at / 8])
        at += 8;
    while (at < maximum && bit(a, at) == bit(b, at))
        ++at;
    return at;
}
static void newest(struct knurl_mmdb_prefix *cache,
                   struct knurl_mmdb_prefix_node *node)
{
    if (cache->head == node)
        return;
    if (node->newer != NULL)
        node->newer->older = node->older;
    if (node->older != NULL)
        node->older->newer = node->newer;
    if (cache->tail == node)
        cache->tail = node->newer;
    node->newer = NULL;
    node->older = cache->head;
    if (cache->head != NULL)
        cache->head->newer = node;
    else
        cache->tail = node;
    cache->head = node;
}
static void release_node(struct knurl_mmdb_prefix *cache,
                         struct knurl_mmdb_prefix_node *node)
{
    node->child[0] = cache->spare;
    cache->spare = node;
}
static void evict(struct knurl_mmdb_prefix *cache)
{
    struct knurl_mmdb_prefix_node *leaf = cache->tail, *parent, *grand,
                                  *sibling;
    if (leaf == NULL)
        return;
    cache->tail = leaf->newer;
    if (cache->tail != NULL)
        cache->tail->older = NULL;
    else
        cache->head = NULL;
    parent = leaf->parent;
    if (parent == NULL)
        cache->roots[leaf->family == 16] = NULL;
    else {
        sibling = parent->child[parent->child[0] == leaf];
        grand = parent->parent;
        sibling->parent = grand;
        if (grand == NULL)
            cache->roots[leaf->family == 16] = sibling;
        else
            grand->child[grand->child[1] == parent] = sibling;
        release_node(cache, parent);
    }
    release_node(cache, leaf);
}
static struct knurl_mmdb_prefix_node *take(struct knurl_mmdb_prefix *cache)
{
    struct knurl_mmdb_prefix_node *node = NULL, *next;
    if (cache->spare == NULL && sizeof(*node) <= cache->limit - cache->used &&
        sizeof(*node) <= cache->heap->heap_limit - cache->heap->heap_used) {
        node = (struct knurl_mmdb_prefix_node *)malloc(sizeof(*node));
        if (node != NULL) {
            node->allocated_next = cache->allocated;
            cache->allocated = node;
            cache->used += sizeof(*node);
            cache->heap->heap_used += sizeof(*node);
        }
    }
    if (node == NULL) {
        if (cache->spare == NULL)
            evict(cache);
        node = cache->spare;
        if (node == NULL)
            return NULL;
        cache->spare = node->child[0];
    }
    next = node->allocated_next;
    memset(node, 0, sizeof(*node));
    node->allocated_next = next;
    return node;
}
int knurl_mmdb_prefix_get(struct knurl_mmdb_prefix *cache,
                          const knurl_mmdb_address *address,
                          knurl_mmdb_country *out, knurl_status *status)
{
    struct knurl_mmdb_prefix_node *node;
    if (address->size != 4 && address->size != 16)
        return 0;
    node = cache->roots[address->size == 16];
    while (node != NULL && !node->leaf)
        node = node->child[bit(address->bytes, node->bits)];
    if (node == NULL || !matches(address->bytes, node->key, node->bits))
        return 0;
    newest(cache, node);
    memcpy(out->code, node->code, sizeof(out->code));
    *status = node->code[0] ? KNURL_OK : KNURL_EOF;
    return 1;
}
void knurl_mmdb_prefix_put(struct knurl_mmdb_prefix *cache,
                           const knurl_mmdb_address *address, unsigned int bits,
                           const knurl_mmdb_country *country,
                           knurl_status status)
{
    struct knurl_mmdb_prefix_node *leaf, *branch = NULL, *at, *parent;
    unsigned int index = address->size == 16, shared, maximum;
    if ((address->size != 4 && address->size != 16) ||
        bits > address->size * 8 ||
        (status != KNURL_OK && status != KNURL_EOF) ||
        (status == KNURL_OK &&
         (country->code[0] < 'A' || country->code[0] > 'Z' ||
          country->code[1] < 'A' || country->code[1] > 'Z' ||
          country->code[2] != 0)))
        return;
    leaf = take(cache);
    if (leaf == NULL)
        return;
    if (cache->roots[index] != NULL) {
        branch = take(cache);
        if (branch == NULL) {
            release_node(cache, leaf);
            return;
        }
    }
    leaf->bits = (unsigned char)bits;
    leaf->family = (unsigned char)address->size;
    leaf->leaf = 1;
    memcpy(leaf->key, address->bytes, address->size);
    if (bits % 8)
        leaf->key[bits / 8] &= (unsigned char)(255u << (8 - bits % 8));
    memset(leaf->key + (bits + 7) / 8, 0, sizeof(leaf->key) - (bits + 7) / 8);
    if (status == KNURL_OK)
        memcpy(leaf->code, country->code, sizeof(leaf->code));
    at = cache->roots[index];
    if (at == NULL) {
        cache->roots[index] = leaf;
        if (branch != NULL)
            release_node(cache, branch);
        newest(cache, leaf);
        return;
    }
    for (;;) {
        maximum = bits < at->bits ? bits : at->bits;
        shared = common(leaf->key, at->key, maximum);
        if (shared < maximum)
            break;
        if (at->leaf || bits <= at->bits) {
            /* Actual immutable MMDB leaves do not overlap. Ignore duplicate
             * or overlapping insertions rather than broaden an answer. */
            release_node(cache, leaf);
            release_node(cache, branch);
            return;
        }
        at = at->child[bit(leaf->key, at->bits)];
    }
    parent = at->parent;
    branch->bits = (unsigned char)shared;
    branch->family = leaf->family;
    memcpy(branch->key, leaf->key, sizeof(branch->key));
    branch->parent = parent;
    branch->child[bit(leaf->key, shared)] = leaf;
    branch->child[!bit(leaf->key, shared)] = at;
    leaf->parent = at->parent = branch;
    if (parent == NULL)
        cache->roots[index] = branch;
    else
        parent->child[parent->child[1] == at] = branch;
    newest(cache, leaf);
}
void knurl_mmdb_prefix_clear(struct knurl_mmdb_prefix *cache)
{
    struct knurl_mmdb_prefix_node *node, *next;
    for (node = cache->allocated; node != NULL; node = next) {
        next = node->allocated_next;
        cache->heap->heap_used -= sizeof(*node);
        free(node);
    }
    cache->used = 0;
    cache->allocated = cache->spare = cache->head = cache->tail = NULL;
    cache->roots[0] = cache->roots[1] = NULL;
}
