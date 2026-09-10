"""Driver for dv_decode.test -- server-free unit coverage of the deletion-vector blob decoder
(UCDeletionVectorData::FromBlob, src/uc_puffin.cpp) via the uc_read_deletion_vector table function.

The duckdb-iceberg #1204 suite only exercised the live happy path; this pins the pure decode: a
valid blob's positions AND every FromBlob reject-guard (min-size, magic, bad count, bad/OOB bitmap
payload, checksum, trailing bytes). No UC server, no network -- just crafted blob files this driver
builds in a temp dir and feeds to the body via ${...} paths.

The puffin `deletion-vector-v1` byte layout (see src/uc_puffin.cpp:66):
    vector_size(4, BE) | magic(4) | n_bitmaps(8, LE) | {key(4, LE) | portable-roaring}* | CRC32(4, BE)
vector_size = blob_length - 4 (covers everything after its own field); CRC32 (standard zlib crc, the
same 0xEDB88320 table FromBlob's CRC32 builds) is over magic..last-bitmap, stored big-endian.
"""

import struct
import zlib

from ducktest import run_paired

MAGIC = bytes([0xD1, 0xD3, 0x39, 0x64])
SERIAL_COOKIE_NO_RUNCONTAINER = 12346  # CRoaring "portable" no-run cookie


def _portable_roaring(values):
    """CRoaring 'portable' serialization of a uint32 set as array containers (all a small deletion
    vector needs). Byte-identical to CRoaring's own writer for array containers -- cross-checked
    against pyroaring during development; the C++ side decodes it via roaring::Roaring::readSafe."""
    buckets = {}
    for v in values:
        buckets.setdefault((v >> 16) & 0xFFFF, []).append(v & 0xFFFF)
    keys = sorted(buckets)
    out = struct.pack("<I", SERIAL_COOKIE_NO_RUNCONTAINER) + struct.pack("<I", len(keys))
    for k in keys:  # descriptive headers: key, cardinality-1
        out += struct.pack("<H", k) + struct.pack("<H", len(set(buckets[k])) - 1)
    off = 8 + 8 * len(keys)  # offset header: one uint32 per container -> data start
    for k in keys:
        out += struct.pack("<I", off)
        off += 2 * len(set(buckets[k]))
    for k in keys:  # array container data: sorted low-16 values
        for lo in sorted(set(buckets[k])):
            out += struct.pack("<H", lo)
    return out


def _blob(bitmaps, *, crc=None, count=None):
    """Assemble a full deletion-vector blob. `bitmaps` is a list of (key, [positions]); `crc`/`count`
    override the (otherwise correct) checksum / n_bitmaps field to craft corrupt inputs."""
    body = MAGIC + struct.pack("<q", len(bitmaps) if count is None else count)
    for key, positions in bitmaps:
        body += struct.pack("<i", key) + _portable_roaring(positions)
    checksum = zlib.crc32(body) & 0xFFFFFFFF if crc is None else crc
    return struct.pack(">I", len(body) + 4) + body + struct.pack(">I", checksum)


def test_dv_decode(request, tmp_path):
    fixtures = {}

    def put(name, data):
        p = tmp_path / f"{name}.dv"
        p.write_bytes(data)
        fixtures[name] = str(p)
        fixtures[f"{name}_SIZE"] = str(len(data))

    # -- valid --------------------------------------------------------------
    put("DV_EMPTY", _blob([]))  # 0 bitmaps -> 0 positions
    put("DV_POS", _blob([(0, [0, 1, 5, 100])]))  # one container
    put("DV_MULTI", _blob([(0, [0, 1, 70000, 131072, 131073])]))  # multiple 16-bit containers
    # valid blob at a non-zero offset inside a larger file (content-offset path)
    pad = b"\x00" * 13
    off_blob = _blob([(0, [7, 42])])
    put("DV_OFFSET_FILE", pad + off_blob)
    fixtures["DV_OFFSET"] = str(len(pad))
    fixtures["DV_OFFSET_BLOBSIZE"] = str(len(off_blob))

    # -- reject guards (src/uc_puffin.cpp:68) --------------------------------
    put("DV_SMALL", b"\x00" * 10)  # < 20 -> too small
    put("DV_MAGIC", struct.pack(">I", 16) + b"XXXX" + struct.pack("<q", 0) + b"\x00\x00\x00\x00")  # bad magic
    put("DV_BADCOUNT", _blob([], count=-1))  # negative n_bitmaps
    # n_bitmaps=1, valid key, junk where a portable-roaring bitmap should be -> deserialize_size == 0
    bad_body = MAGIC + struct.pack("<q", 1) + struct.pack("<i", 0) + b"\xAB\xCD"
    put(
        "DV_BADPAYLOAD",
        struct.pack(">I", len(bad_body) + 4) + bad_body + struct.pack(">I", zlib.crc32(bad_body) & 0xFFFFFFFF),
    )
    put("DV_CRC", _blob([], crc=0xDEADBEEF))  # good bytes, wrong checksum
    put("DV_TRAILING", _blob([]) + b"\xFF")  # extra byte after checksum

    # OOB: array header claims 4 values but the buffer holds only 1 -> readSafe/deserialize_size reject
    oob_roaring = struct.pack("<I", SERIAL_COOKIE_NO_RUNCONTAINER) + struct.pack("<I", 1)
    oob_roaring += struct.pack("<H", 0) + struct.pack("<H", 3)  # cardinality-1 = 3 (claims 4 values)
    oob_roaring += struct.pack("<I", 16) + struct.pack("<H", 0)  # offset + only ONE value present
    oob_body = MAGIC + struct.pack("<q", 1) + struct.pack("<i", 0) + oob_roaring
    oob = struct.pack(">I", len(oob_body) + 4) + oob_body + struct.pack(">I", zlib.crc32(oob_body) & 0xFFFFFFFF)
    put("DV_OOB", oob)

    run_paired(request, env=fixtures)
