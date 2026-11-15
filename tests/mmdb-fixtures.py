"""Generate small owned MMDB inputs. No downloads or test orchestration."""
import ipaddress
import pathlib
import sys


def header(kind, size):
    ext = bytes([kind - 7]) if kind > 7 else b""
    high = 0 if ext else kind << 5
    if size < 29:
        return bytes([high | size]) + ext
    if size < 285:
        return bytes([high | 29]) + ext + bytes([size - 29])
    if size < 65821:
        return bytes([high | 30]) + ext + (size - 285).to_bytes(2, "big")
    return bytes([high | 31]) + ext + (size - 65821).to_bytes(3, "big")


def string(value):
    data = value.encode()
    return header(2, len(data)) + data


def integer(kind, value):
    size = (value.bit_length() + 7) // 8
    return header(kind, size) + value.to_bytes(size, "big")


def mapping(items):
    return header(7, len(items)) + b"".join(string(k) + v for k, v in items)


def pointer(value, width):
    if width == 1:
        return bytes([0x20 | (value >> 8), value & 255])
    if width == 2:
        value -= 2048
        return bytes([0x28 | (value >> 16)]) + (value & 65535).to_bytes(2, "big")
    if width == 3:
        value -= 526336
        return bytes([0x30 | (value >> 24)]) + (value & 0xffffff).to_bytes(3, "big")
    return b"\x38" + value.to_bytes(4, "big")


def database(width=24, version=6, data=None, target=0, metadata=None, node_transform=None, separator=None, networks=None):
    if data is None:
        data = mapping([("country", mapping([("iso_code", string("US"))]))])
    nodes = [[None, None]]
    if networks is None:
        networks = [("1.2.3.0/24", target), ("128.0.0.0/1", target)]
        if version == 6:
            networks.append(("2001:db8::/32", target))
    for text, offset in networks:
        network = ipaddress.ip_network(text)
        bits = network.prefixlen + (96 if version == 6 and network.version == 4 else 0)
        address = int(network.network_address)
        node = 0
        for shift in range((128 if version == 6 else 32) - 1,
                           (128 if version == 6 else 32) - bits - 1, -1):
            bit = (address >> shift) & 1
            if shift == (128 if version == 6 else 32) - bits:
                nodes[node][bit] = (offset,)
            else:
                if nodes[node][bit] is None:
                    nodes[node][bit] = len(nodes)
                    nodes.append([None, None])
                node = nodes[node][bit]
    count = len(nodes)
    tree = bytearray()
    for pair in nodes:
        a, b = [count if v is None else count + 16 + v[0]
                if isinstance(v, tuple) else v for v in pair]
        if node_transform is not None:
            a, b = node_transform(count, a, b)
        if width == 28:
            tree.extend((a & 0xffffff).to_bytes(3, "big"))
            tree.append(((a >> 24) << 4) | (b >> 24))
            tree.extend((b & 0xffffff).to_bytes(3, "big"))
        else:
            tree.extend(a.to_bytes(width // 8, "big"))
            tree.extend(b.to_bytes(width // 8, "big"))
    meta = [("node_count", integer(6, count)),
            ("record_size", integer(5, width)),
            ("ip_version", integer(5, version)),
            ("database_type", string("Knurl-Test")),
            ("binary_format_major_version", integer(5, 2)),
            ("binary_format_minor_version", integer(5, 0)),
            ("build_epoch", integer(9, 1)),
            ("languages", header(11, 1) + string("en")),
            ("description", mapping([("en", string("Knurl fixtures"))]))]
    if metadata is not None:
        meta = metadata(meta)
    return bytes(tree) + (bytes(16) if separator is None else separator) + data + b"\xab\xcd\xefMaxMind.com" + mapping(meta)


root = pathlib.Path(sys.argv[1])
root.mkdir(parents=True, exist_ok=True)


def write(name, **kwargs):
    (root / (name + ".mmdb")).write_bytes(database(**kwargs))


for width in (24, 28, 32):
    for version in (4, 6):
        write(f"valid-{width}-{version}", width=width, version=version)
for width in (1, 2, 3, 4):
    offset = [0, 100, 2048, 526336, 23][width]
    prefix = mapping([("country", pointer(offset, width))])
    data = prefix + bytes(offset - len(prefix)) + mapping([("iso_code", string("SE"))])
    write(f"pointer-{width}", data=data)
for name, value in [("missing", mapping([])), ("missing-code", mapping([("country", mapping([]))])),
                    ("bad-code", mapping([("country", mapping([("iso_code", string("USA"))]))])),
                    ("bad-type", mapping([("country", string("US"))])),
                    ("bad-case", mapping([("country", mapping([("iso_code", string("us"))]))]))]:
    write(name, data=value)
for size in (28, 29, 284, 285, 65820, 65821):
    data = mapping([("unused", header(11, 2) + string("x" * size) +
                    mapping([("nested", header(14, 1))])),
                    ("country", mapping([("iso_code", string("DE"))]))])
    write(f"skip-{size}", data=data)
write("bad-pointer", data=b"\x38\xff\xff\xff\xff")
write("pointer-cycle", data=b"\x20\x00")
write("bad-metadata", metadata=lambda m: [(k, integer(5, 3)) if k == "ip_version" else (k, v) for k, v in m])
write("duplicate-metadata", metadata=lambda m: m + [m[0]])
write("bad-version", metadata=lambda m: [(k, integer(5, 3)) if k == "binary_format_major_version" else (k, v) for k, v in m])
write("missing-metadata", metadata=lambda m: m[1:])
(root / "empty.mmdb").write_bytes(b"")
(root / "truncated.mmdb").write_bytes(database()[:-2])
country = mapping([("country", mapping([("iso_code", string("US"))]))])
for width in (28, 32):
    write(f"high-{width}", width=width, version=4,
          data=bytes(16777216) + country, target=16777216)
write("root-pointer", data=pointer(100, 1) + bytes(98) + country)
keyed = header(7, 1) + pointer(60, 1) + header(7, 1) + pointer(80, 1) + string("US")
keyed += bytes(60 - len(keyed)) + string("country")
keyed += bytes(80 - len(keyed)) + string("iso_code")
write("key-pointer", data=keyed)
for name, value in [("null-root", string("US")),
                    ("bad-integer", mapping([("unused", header(5, 3) + bytes(3)), ("country", mapping([]))])),
                    ("bad-double", mapping([("unused", header(3, 7) + bytes(7)), ("country", mapping([]))])),
                    ("bad-boolean", mapping([("unused", header(14, 2)), ("country", mapping([]))])),
                    ("bad-size-29", b"\x5d"), ("bad-size-30", b"\x5e\xff"),
                    ("bad-size-31", b"\x5f\xff\xff"),
                    ("huge-array", mapping([("unused", header(11, 16777215)), ("country", mapping([]))]))]:
    write(name, data=value)
for width in (1, 2, 3, 4):
    write(f"short-pointer-{width}", data=bytes([0x20 + (width - 1) * 8]) + bytes(width - 1))
write("bad-tree-data", node_transform=lambda n, a, b: (n + 1, n + 1))
write("tree-cycle", node_transform=lambda n, a, b: (0, 0))
write("bad-tree-offset", node_transform=lambda n, a, b: (0xffffff, 0xffffff))
write("bad-separator", separator=b"x" * 16)
write("huge-node-count", metadata=lambda m: [(k, integer(6, 0xffffffff)) if k == "node_count" else (k, v) for k, v in m])
write("bad-record-width", metadata=lambda m: [(k, integer(5, 36)) if k == "record_size" else (k, v) for k, v in m])
write("duplicate-description", metadata=lambda m: [(k, mapping([("en", string("a")), ("en", string("b"))])) if k == "description" else (k, v) for k, v in m])
write("optional-metadata", metadata=lambda m: [(k, v) for k, v in m if k not in ("description", "languages")])
write("many-metadata", metadata=lambda m: m + [(f"extra-{i}", string("x")) for i in range(65)])
mixed = (header(11, 10) + header(3, 8) + bytes(8) + header(4, 2) + b"\xff\xff" +
         integer(5, 65535) + integer(6, 0xffffffff) + header(8, 4) + b"\xff" * 4 +
         header(9, 8) + bytes(8) + header(10, 16) + bytes(16) + header(14, 0) +
         header(15, 4) + bytes(4) + mapping([]))
for name, value in [("skip-scalars", mixed),
                    ("skip-depth", header(11, 1) * 256 + string("leaf"))]:
    write(name, data=mapping([("unused", value),
                             ("country", mapping([("iso_code", string("DE"))]))]))
data = bytearray(128 * 4096 + 256)
networks = []
for index in range(128):
    record = mapping([("country", mapping([("iso_code", string("SE" if index % 2 else "US"))]))])
    offset = index * 4096
    data[offset:offset + len(record)] = record
    networks.append((f"100.0.{index}.0/24", offset))
write("cache", version=4, data=bytes(data), networks=networks)
changed = bytearray(data)
for index in range(128):
    record = mapping([("country", mapping([("iso_code", string("US" if index % 2 else "SE"))]))])
    changed[index * 4096:index * 4096 + len(record)] = record
write("cache-new", version=4, data=bytes(changed), networks=networks)
us = mapping([("country", mapping([("iso_code", string("US"))]))])
se = mapping([("country", mapping([("iso_code", string("SE"))]))])
data = us + bytes(64 - len(us)) + se
write("prefix-4", version=4, data=data, networks=[("1.2.3.4/32", 0), ("1.2.3.5/32", 64), ("2.3.4.0/25", 0)])
write("prefix-6", data=data, networks=[("2001:db8::1/128", 0), ("2001:db8::2/128", 64), ("2001:db8:1::/127", 0)])
empty = database(version=4, data=b"")
base = empty.index(b"\xab\xcd\xefMaxMind.com")
for name, in_record in [("boundary-control", 0), ("boundary-code", country.index(b"US"))]:
    padding = (4095 - base - in_record) % 4096
    write(name, version=4, target=padding, data=bytes(padding) + country)
