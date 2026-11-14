# MMDB country reader

Knurl's standalone C89 MMDB reader supplies country lookup for [route ACLs](acl.md).
The public provider, free functions and receiver shell in `knurl/mmdb.h` work
without initializing a server, router or loop. Knurl is the sole implementation;
there is no embedded or external libmaxminddb dependency.

Files are read with bounded `pread` operations, never `mmap`. A lazy LRU cache
retains only 4 KiB file blocks actually accessed, including search-tree nodes
and encoded record bytes. A second LRU cache retains exact resolved network
prefixes with copied country codes or valid unknown results. Separate IPv4/IPv6
compressed prefix trees serve hits without database traversal, decoding or file
reads. IPv4-mapped IPv6 normalizes before caching. Errors are never cached;
invalid country codes are rejected before insertion.

`knurl_mmdb_file_config.max_heap_bytes`
limits all reader-owned requested heap bytes, including its provider, file state,
both cache indexes and bookkeeping. Zero selects 512 KiB; the minimum is 8 KiB.
Allocator bookkeeping, bounded stack storage and the OS's shared file cache are
outside this heap limit. The reader does not reserve or allocate the complete
configured limit at open. After fixed state, one quarter of the budget is reserved
for file blocks and three quarters for prefix results, with room guaranteed for
at least one file block. These internal quotas share the total ceiling. Each
layer allocates lazily and reuses its least recently used entries at its bound.
File blocks use a hash index; prefix nodes are recycled without steady eviction
allocation. Optional growth failures reuse existing storage or skip result
caching; failure to allocate the first file block returns NOMEM.

Opening synchronously validates metadata and pins a regular file descriptor.
Lookup may issue synchronous file reads on a cache miss. Concurrent calls on
one handle are safe and serialize access to its cache; separate handles operate
independently and each has its own heap limit. Close requires all readers to
have finished. Keep the opened file immutable: replace database paths by atomic
rename and open a new handle, rather than modifying an opened inode in place.

Lookup accepts binary IPv4/IPv6 and normalizes IPv4-mapped IPv6 to IPv4. Numeric
text parsing never resolves DNS. OK returns a copied two-letter uppercase
`country.iso_code`; EOF means no record or missing country code. Malformed
records return BACKEND, file read failures return IO, and allocation failures
return NOMEM. Errors never become unknown-country results. Country codes such
as XK are accepted without maintaining an independent ISO registry.

```c
knurl_mmdb_file_config config;
knurl_mmdb *db;
knurl_mmdb_address address;
knurl_mmdb_country country;
knurl_error error;
knurl_status status;

knurl_mmdb_file_config_init(&config);
config.max_heap_bytes = 512u * 1024u;
if (knurl_mmdb_open("country.mmdb", &config, &db, &error) != KNURL_OK)
    return 1;
status = knurl_mmdb_address_parse("1.2.3.4", &address, &error);
if (status == KNURL_OK)
    status = db->lookup(db, &address, &country, &error);
/* OK: country.code is copied. EOF: unknown. Other statuses: failure. */
db->close(db, NULL);
```

A NULL file config selects defaults. `knurl_mmdb_lookup(db, ...)` and
`knurl_mmdb_close(db, ...)` are equivalent free functions. `knurl_mmdb_new`
copies host lookup/close callbacks under the same country contract. Retain
callback context until its optional close notification. Do not copy receivers.

The reader implements the MMDB v2 primitives required for country selection:
metadata, 24/28/32-bit search nodes, IPv4-in-IPv6 traversal, data pointers,
map selection and bounded skipping of unselected values. There is no DOM, writer
or general value conversion API. All offsets and lengths are checked against
their own section. Selected pointers cannot point to pointers. Skipping is
iterative and does not expand unselected pointer targets. A shared budget of
65,536 decoded values bounds metadata decoding or a lookup; metadata and
description maps each permit 64 keys. Opening does not certify unselected data.

Each reader owns its cache generation; opening a replacement database constructs
fresh caches. Country facts remain independent of route admission decisions.

Synthetic tests cover node and pointer widths, address/subnet boundaries,
missing and malformed records, cache growth and eviction, concurrent reads and
deep skipped fields. A deterministic corpus checks results against an independent
prefix oracle. Optional local real-data tests can compare every result against
a captured three-byte-per-result oracle; no database or oracle is shipped.
Linux allocator/I/O probes prove lazy growth, the configured heap bound, bounded
read requests, eviction reuse and cleanup. Forced read failures after file-block
eviction verify that another address in a cached prefix still returns its country
or valid unknown result without I/O. Exact-address and partial-byte prefix
boundaries, independent database generations and failed cache growth are covered.

After `make build CONFIG=release`, measure cache budgets with:

```sh
cmake -DBENCHMARK="$PWD/build/release/knurl_mmdb_benchmark" \
  -DPROBE="$PWD/build/release/knurl_mmdb_benchmark_probe" \
  -DDATABASE="$PWD/build/mmdb/dbip-country-lite-2026-11.mmdb" \
  '-DHEAP_LIMITS=65536;131072;262144;524288;1048576;2097152;4194304;8388608' \
  -DOUTPUT_DIR="$PWD/build/mmdb-cache/performance" -P tests/MMDBPerformance.cmake
build/release/knurl_mmdb_memory build/release/tests/mmdb-fixtures/cache.mmdb 65536
```

Timing tools use the same deterministic address corpus for each heap limit.
The default mixed corpus contains 65,536 addresses, half IPv4 and half IPv6;
the hot profile repeats its first 16 addresses. Each process runs one first pass
and eight warm passes; the timing is their median, followed by five independent
trials with reversed heap-limit order on alternate trials. The runner accepts
`ROUNDS`, `ADDRESS_COUNT` and `PATTERNS` (a list of `random` and/or `hot`) for
controlled follow-up experiments. For example, use `-DADDRESS_COUNT=1048576
-DPATTERNS=random -DROUNDS=3` to check a broader mixed-address working set.
They report open time, first pass, median warm lookup time, result checksum,
database bytes and process RSS. Checksums must agree across heap limits.
On Linux the optional `PROBE` target runs the identical corpus separately,
tracking reader-owned requested heap and pread calls through linker wrappers.
Its output goes to `heap.txt`; its instrumented timings are not performance
results. `comparison.txt` contains timings from the uninstrumented consumer.
Use `knurl_mmdb_benchmark_static` to retain a baseline executable independent of
later shared-library changes. Both consumers and the probe use the same corpus
and must produce identical result checksums. The probe also reports prefix hits
and misses, checks the heap limit and zero retained reader heap after close.
The address array and measurement machinery are excluded from reader heap accounting.
Peak heap is distinct from the configured limit: a hot working set can stop
growing well below a large limit. Compare against a saved file-block-only
baseline at the same total heap limit when measuring the prefix-cache benefit.
Existing OS cache state is retained; results are not cold-disk measurements.
Earlier mmap/reference-backend measurements describe the superseded memory
model and must not be presented as the bounded reader's performance.

The November 2026 DB-IP Country Lite input used during development is 8,515,063
bytes (about 8.12 MiB), or 4,205,904 bytes compressed. Downloads and captured
oracles remain in ignored build storage. Record release, URL and SHA-256, and
verify the publisher's digest of the decompressed database. No ordinary test
downloads anything or needs network access. DB-IP's
[CC BY 4.0 terms](https://db-ip.com/db/download/ip-to-country-lite) require
attribution: [IP Geolocation by DB-IP](https://db-ip.com).

Format reference: [MaxMind DB v2 specification](https://maxmind.github.io/MaxMind-DB/).
