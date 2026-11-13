# MMDB country readers

The MMDB boundary supplies country lookup for future route ACLs. It does not
implement ACL policy, DNS refresh or forwarded-client identity yet. Both file
readers use the same public provider/receiver contract in `knurl/mmdb.h`.

Construction opens and validates an immutable file synchronously, before serving
requests. Lookup accepts binary IPv4/IPv6, normalizes IPv4-mapped IPv6 to IPv4,
and copies only `country.iso_code` into a three-byte result. Numeric text parsing
never resolves DNS. `OK` means two uppercase ASCII country letters; `EOF` means
no record or no country code. Malformed records and unavailable databases return
errors, never an unknown-country result. Country codes such as XK are accepted;
the reader does not maintain an independent ISO registry.

Providers copy callbacks after successful construction; callers retain the
callback context through its optional close notification. Concurrent lookup
is supported when the supplied callback is thread safe; close requires all
readers to have finished. Receivers must not be copied or modified. File readers
map files read-only and decode selected fields directly. Mapping reserves virtual
address space; pages enter memory on demand and can fault during lookup. No whole
database copy or decoded record cache is built. Replace files by rename and open
a new handle; never truncate or modify an open database in place.

The reader is independent of the webserver. Include only `<knurl/mmdb.h>` and
link the normal static or shared Knurl target. For example, inside an application:

```c
knurl_mmdb *db;
knurl_mmdb_address address;
knurl_mmdb_country country;
knurl_error error;
knurl_status status;

if (knurl_mmdb_native_open("country.mmdb", &db, &error) != KNURL_OK)
    return 1;
status = knurl_mmdb_address_parse("1.2.3.4", &address, &error);
if (status == KNURL_OK)
    status = db->lookup(db, &address, &country, &error);
/* OK: country.code is copied. EOF: unknown. Other statuses: failure. */
db->close(db, NULL);
```

`knurl_mmdb_lookup(db, ...)` and `knurl_mmdb_close(db, ...)` are equivalent free
functions. Select `knurl_mmdb_maxmind_open` for the embedded reference reader.
`knurl_mmdb_new` accepts a host's own lookup/close callbacks through the same
contract; no server, loop or router must be initialized.

The first implementation embeds pristine libmaxminddb 1.14.1 behind the adapter.
Its C99 sources are isolated from Knurl's C89 code and public headers. Its symbols
are private and prefixed; consumers need no libmaxminddb installation, headers or
link flags. Apache-2.0 LICENSE and NOTICE are shipped in the SDK.

The second implementation supplies only MMDB v2 read primitives required for
country selection: metadata, 24/28/32-bit search nodes, IPv4-in-IPv6 traversal,
pointer decoding, map selection and bounded skipping of unselected values.
It has no DOM, writer, localized names API or general value conversion API.
All offsets, lengths and arithmetic are checked against their own section.
Selected pointers cannot point to pointers. Traversal is bounded independently
of the process stack. Corruption outside the selected lookup is not certified
by opening a file: these are readers, not whole-database verification tools.
The native reader permits 64 metadata keys and 64 description languages, with
a shared 65,536-value budget per metadata decode or country lookup. Skipping
is iterative and does not expand unselected pointers. Lookup allocates no heap.
The embedded reader uses its upstream bounds; it requires languages/description
metadata that the native reader accepts as optional, and accepts some duplicate
metadata that the native reader rejects. These validation differences are not
country-result differences for valid databases.

Verification uses generated synthetic databases, including missing/malformed
countries, all node widths, both address families, pointer widths, nested skipped
fields, invalid metadata, truncated files and invalid offsets. Differential tests
compare both backends over the same deterministic corpus and real DB-IP data.
The ordinary suite never downloads data or requires network access.

Benchmarks run each backend in a separate process over identical binary address
corpora and report open time, first pass and repeated lookup time, a result
checksum, virtual mapped bytes and process resident memory. Random and repeated
addresses are measured separately; timing thresholds are not correctness tests.
First-touch results reflect the existing OS file cache and are not claimed to
be cold-disk results. No global cache dropping is permitted. Compare optimized
builds on the same machine; include database identity and memory measurement
limits when reporting results.

After `make build CONFIG=release`, run the reproducible comparison with:

```sh
cmake -DBENCHMARK="$PWD/build/release/knurl_mmdb_benchmark" \
  -DDATABASE="$PWD/build/mmdb/dbip-country-lite-2026-11.mmdb" \
  -DOUTPUT_DIR="$PWD/build/mmdb/performance" -P tests/MMDBPerformance.cmake
build/release/knurl_mmdb_differential build/mmdb/dbip-country-lite-2026-11.mmdb
build/release/knurl_mmdb_memory maxmind build/mmdb/dbip-country-lite-2026-11.mmdb
build/release/knurl_mmdb_memory native build/mmdb/dbip-country-lite-2026-11.mmdb
```

Each timing process reports the median of eight warm passes. The CMake runner
alternates backend order over five trials and requires identical result checksums.
Linux memory probes link the static library with allocator wrappers: live/peak
requested reader heap excludes allocator overhead, file-backed pages and libc's
private allocations. They assert zero allocation during lookup and zero retained
reader heap after close. RSS includes the executable, dependencies, address corpus
and touched mapping pages, so it is not an isolated reader allocation count.
On Linux RSS is current `/proc/self/statm` resident memory; on Darwin it is the
process high-water mark. Both readers map the complete file's virtual extent.

DB-IP data is downloaded only into ignored `build/` storage for local tests and
benchmarks; it is absent from source control and SDKs. Its
[CC BY 4.0 terms](https://db-ip.com/db/download/ip-to-country-lite) require
attribution. Record the download URL, release and SHA-256, and verify the
publisher's digest of the **decompressed** database before use. Benchmark data
attribution: [IP Geolocation by DB-IP](https://db-ip.com).

Format reference: [MaxMind DB v2 specification](https://maxmind.github.io/MaxMind-DB/).
