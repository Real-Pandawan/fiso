# FISO — Flac ISO — Open Audio Disc Image Format — Specification v1.0

Status: **Draft for v1.0 freeze** · French translation: [FISO_FORMAT_v1.0_FR.md](FISO_FORMAT_v1.0_FR.md) (the English text is authoritative) · License of this document: **CC0 1.0** (public domain dedication)

The key words MUST, MUST NOT, SHOULD, SHOULD NOT and MAY are to be interpreted as described in RFC 2119.

---

## 1. Purpose and scope

A FISO image (`.fiso`, MIME type `application/x-fiso-image`) stores **one physical audio disc** (or an equivalent audio timeline built from files) in a single file:

* the complete audio programme, losslessly compressed with **FLAC**;
* the disc structure (tracks, indexes, pre-gaps, hidden track one audio, ISRC, MCN, flags, original TOC);
* descriptive metadata, including multi-disc album membership;
* embedded pictures (front cover, back cover…);
* integrity data (SHA-256 per chunk, SHA-256 of decoded PCM per segment and for the whole timeline).

FISO is **not** a ZIP archive, not an ISO 9660 image and does not preserve physical sub-channel data (P–W) or the raw Red Book frame layer. What is preserved is the user-audible PCM programme plus the structural information needed to rebuild a Disc-at-Once CD-DA (§9).

Data tracks of mixed-mode / CD-Extra discs are **not stored** in v1.0. Their existence is recorded in the TOC table (§5.6) so that readers can tell the image is not a complete copy of the disc.

**Naming.** “FISO” stands for *Flac ISO*: one file holding a whole disc, like an ISO image, with the audio in FLAC. Despite the name, a FISO image is not an ISO 9660 file system.

## 2. Conventions

* All integers are **little-endian**, unsigned unless written `i32` (two's complement).
* Offsets are absolute byte offsets from the start of the file.
* `sample` means one **sample frame** (one sample per channel). For CD-DA: 44 100 Hz, 2 channels, 16 bits.
* CD-DA units: 1 sector = 1/75 s = **588 samples** = **2 352 bytes** of PCM.
* LBA: logical block address as defined by Red Book, LBA 0 = MSF 00:02:00.
* Canonical PCM byte form (used for every hash): interleaved channels, signed integers, little-endian, `ceil(bits/8)` bytes per sample. For CD-DA this is exactly the byte layout of a raw audio `.bin` file.
* SHA-256 is FIPS 180-4. CRC-32 is CRC-32/ISO-HDLC (polynomial 0x04C11DB7 reflected, init 0xFFFFFFFF, xorout 0xFFFFFFFF — the zlib `crc32`).
* Strings in binary structures are fixed-size ASCII fields padded with NUL bytes.

## 3. File layout

```
+---------------------------+  offset 0
| File header (96 bytes)    |
+---------------------------+  offset 96 (8-byte aligned)
| Chunk data ...            |  each chunk starts on an 8-byte boundary,
| (any order)               |  gaps are filled with zero bytes
+---------------------------+  table_offset (8-byte aligned)
| Chunk table               |  chunk_count × 64 bytes
+---------------------------+  file_size
```

The chunk table is at the end so that a writer can stream audio without knowing chunk sizes in advance.

### 3.1 File header (96 bytes)

| Offset | Size | Field | Value / rule |
|---:|---:|---|---|
| 0 | 8 | `magic` | `89 46 49 53 4F 0D 0A 0A` (`\x89FISO\r\n\n`) |
| 8 | 2 | `version_major` | `1` |
| 10 | 2 | `version_minor` | `0` |
| 12 | 4 | `header_size` | `96` |
| 16 | 4 | `file_flags` | `0` (reserved, MUST be 0 in v1.0) |
| 20 | 4 | `chunk_count` | 1 … 65 536 |
| 24 | 8 | `table_offset` | multiple of 8, ≥ 96 |
| 32 | 8 | `file_size` | MUST equal the real file size |
| 40 | 32 | `table_sha256` | SHA-256 of the whole chunk table |
| 72 | 20 | reserved | zero |
| 92 | 4 | `header_crc32` | CRC-32 of bytes 0 … 91 |

The magic number follows the PNG design: the non-ASCII first byte detects 7-bit transfers, `\r\n` detects CRLF→LF conversion and the final lone `\n` detects LF→CRLF conversion.

`table_offset + chunk_count × 64` MUST equal `file_size`.

### 3.2 Versioning

* A reader MUST refuse a file whose `version_major` it does not implement.
* A reader implementing `1.x` MUST accept any `1.y`, ignoring non-critical chunks and reserved fields it does not understand.
* Minor versions MAY add chunk types and append fields to the *end* of JSON objects; they MUST NOT change the meaning or size of existing binary fields.

### 3.3 Chunk table entry (64 bytes)

| Offset | Size | Field | Rule |
|---:|---:|---|---|
| 0 | 4 | `type` | four ASCII characters, e.g. `FLAC` |
| 4 | 4 | `flags` | bit 0 `CRITICAL`; other bits 0 |
| 8 | 8 | `offset` | multiple of 8, ≥ `header_size` |
| 16 | 8 | `length` | bytes |
| 24 | 32 | `sha256` | SHA-256 of the chunk data |
| 56 | 4 | `chunk_version` | version of the chunk payload (1 for all v1.0 types) |
| 60 | 4 | reserved | zero |

Rules:

* `offset + length ≤ table_offset`; chunks MUST NOT overlap.
* A reader encountering an unknown `type` with `CRITICAL` set MUST refuse to open the file. Unknown non-critical chunks MUST be ignored and SHOULD be preserved byte-for-byte when rewriting.
* The order of entries is significant only for `FLAC` chunks, which are referenced by index from `LAYT`.

### 3.4 Chunk types defined in v1.0

| Type | Critical | Count | Content |
|---|---|---|---|
| `LAYT` | yes | exactly 1 | disc layout, binary (§5) |
| `META` | yes | exactly 1 | descriptive metadata, JSON UTF-8 (§6) |
| `FLAC` | yes | ≥ 1 | one audio segment as a standalone FLAC stream (§4) |
| `PICT` | no | 0 … n | embedded picture (§7) |
| `CDTX` | no | 0 … 1 | raw CD-Text packs as read from the disc (18-byte packs, CRC included) |
| `RLOG` | no | 0 … 1 | rip / creation log, UTF-8 text, informational only |

## 4. Audio: segments and the timeline

### 4.1 Timeline

The image represents one continuous PCM **timeline** of `total_samples` samples. Sample 0 is at LBA `timeline_start_lba` of the original disc (normally 0, i.e. the start of track 1 INDEX 00 or INDEX 01). The lead-in and the mandatory 150-sector pre-gap before LBA 0 are not part of the timeline.

### 4.2 Segmentation rule

The timeline is cut into **segments**, each stored as one `FLAC` chunk. In v1.0 the cut points are fixed:

* every track's **INDEX 01** starts a new segment;
* if track 1 INDEX 01 is after timeline sample 0, the audio before it forms a **HTOA segment** (Hidden Track One Audio, kind 0).

Consequently a track segment runs from its INDEX 01 to the next track's INDEX 01 (or the end of the timeline), and **the pre-gap (INDEX 00 region) of track N+1 is stored at the end of track N's segment**. Silent pre-gaps cost almost nothing after FLAC compression. This rule keeps the timeline gap-less and makes every segment directly exportable as a FLAC file without re-encoding.

### 4.3 FLAC chunk content

* A complete FLAC stream: `fLaC` marker, `STREAMINFO` first, optionally `SEEKTABLE`, `PADDING` and a vendor-only `VORBIS_COMMENT`, then audio frames.
* Writers MUST NOT put tags in a segment: no `PICTURE` block, and a `VORBIS_COMMENT` block, if any, holds only the encoder vendor string and zero comments (libFLAC always writes one). Metadata lives in `META`/`PICT`, so editing it never touches audio. Readers MUST ignore `VORBIS_COMMENT`, `PICTURE` and `APPLICATION` blocks in segments.
* `STREAMINFO` sample rate, channels and bits per sample MUST equal the `LAYT` values; `total samples` MUST equal the segment length and MUST be non-zero; the `MD5` field MUST be set (non-zero).
* Writers SHOULD encode with the equivalent of `flac -8` and libFLAC verify mode.

### 4.4 CD-DA conformance

`LAYT.flags` bit 0 (`CDDA`) MUST be set only if: rate 44 100 Hz, 2 channels, 16 bits, and every segment start/length, track start/length and index position is a multiple of 588. Only images with this flag set MAY be written to a CD. Writers MUST NOT resample or requantise silently to obtain it.

## 5. `LAYT` chunk (binary)

Layout: header (128 bytes), then `track_count` track records, `index_count` index records, `segment_count` segment records, `toc_entry_count` TOC records, in that order with no padding. `length` of the chunk MUST equal `128 + 48·T + 16·I + 64·S + 16·C`.

### 5.1 Header (128 bytes)

| Offset | Size | Field | Rule |
|---:|---:|---|---|
| 0 | 2 | `layt_version` | 1 |
| 2 | 2 | `flags` | bit 0 `CDDA`, bit 1 `HTOA`, bit 2 `PHYSICAL_SOURCE` (ripped from a disc), bit 3 `HAS_TOC`, bit 4 `SYNTHESIZED_SILENCE` (some audio is digital silence generated from a CUE `PREGAP`/`POSTGAP`, not read from a source) |
| 4 | 4 | `sample_rate` | 1 … 655 350 |
| 8 | 1 | `channels` | 1 … 8 |
| 9 | 1 | `bits_per_sample` | 4 … 32 |
| 10 | 2 | reserved | 0 |
| 12 | 2 | `track_count` | 1 … 99 |
| 14 | 2 | `index_count` | ≥ `track_count` |
| 16 | 2 | `segment_count` | `track_count` or `track_count + 1` (HTOA) |
| 18 | 2 | `toc_entry_count` | 0 … 101 |
| 20 | 1 | `first_track_number` | 1 … 99 |
| 21 | 1 | `last_track_number` | ≥ first |
| 22 | 2 | reserved | 0 |
| 24 | 4 | `timeline_start_lba` (`i32`) | LBA of sample 0 |
| 28 | 4 | reserved | 0 |
| 32 | 8 | `total_samples` | > 0 |
| 40 | 32 | `timeline_pcm_sha256` | SHA-256 of canonical PCM of the whole timeline |
| 72 | 16 | `mcn` | Media Catalog Number (13 ASCII digits) or all NUL |
| 88 | 40 | reserved | 0 |

### 5.2 Track record (48 bytes)

| Offset | Size | Field | Rule |
|---:|---:|---|---|
| 0 | 1 | `number` | 1 … 99, strictly increasing by 1 |
| 1 | 1 | `flags` | bit 0 pre-emphasis, bit 1 digital copy permitted, bit 2 four-channel |
| 2 | 2 | `first_index` | index of this track's first record in the index table |
| 4 | 2 | `index_count` | ≥ 1 |
| 6 | 2 | reserved | 0 |
| 8 | 8 | `start_sample` | position of INDEX 01 |
| 16 | 8 | `length_samples` | up to next track INDEX 01 / timeline end |
| 24 | 16 | `isrc` | 12 ASCII chars or all NUL |
| 40 | 8 | reserved | 0 |

### 5.3 Index record (16 bytes)

| Offset | Size | Field |
|---:|---:|---|
| 0 | 1 | `track_number` |
| 1 | 1 | `index_number` (0 … 99) |
| 2 | 6 | reserved (0) |
| 8 | 8 | `sample_position` (timeline) |

Rules: records are grouped per track in track order and sorted by `index_number`; positions are non-decreasing across the whole table; every track has INDEX 01 at `start_sample`; INDEX 00 of track N (if any) lies in `[start of track N-1, start_sample of N)`; INDEX ≥ 2 of track N lies inside track N.

A track's **pre-gap length** is `start_sample − position(INDEX 00)`, or 0 when INDEX 00 is absent. The pre-gap of track 1 is the HTOA region when present.

### 5.4 Segment record (64 bytes)

| Offset | Size | Field | Rule |
|---:|---:|---|---|
| 0 | 4 | `chunk_index` | index in the chunk table of a `FLAC` chunk |
| 4 | 1 | `kind` | 0 = HTOA, 1 = track |
| 5 | 1 | `track_number` | 0 for HTOA |
| 6 | 2 | reserved | 0 |
| 8 | 8 | `start_sample` | |
| 16 | 8 | `length_samples` | > 0 |
| 24 | 32 | `pcm_sha256` | SHA-256 of the segment's canonical PCM |
| 56 | 8 | reserved | 0 |

Segments are in timeline order, contiguous, and cover `[0, total_samples)` exactly. A track segment's start/length MUST equal its track record. Each `FLAC` chunk is referenced exactly once.

### 5.5 Timeline hash

`timeline_pcm_sha256` is computed over the canonical PCM of all segments concatenated in order (= the BIN file of §9.2).

### 5.6 TOC record (16 bytes) — informational

| Offset | Size | Field |
|---:|---:|---|
| 0 | 1 | `track_number` (1 … 99, `0xAA` = lead-out) |
| 1 | 1 | `control` (Q-channel control nibble) |
| 2 | 1 | `adr` |
| 3 | 1 | `session` (1-based) |
| 4 | 4 | `start_lba` (`i32`) |
| 8 | 1 | `stored` (1 if the track's audio is in this image, 0 for data tracks) |
| 9 | 7 | reserved |

When `HAS_TOC` is set the table reproduces the TOC as read from the physical disc, including data tracks and the lead-out, and allows the MusicBrainz Disc ID to be recomputed exactly. For images built from files, writers MAY synthesise a TOC (`HAS_TOC` set, `PHYSICAL_SOURCE` clear).

## 6. `META` chunk (JSON)

UTF-8 JSON, no BOM, ≤ 16 MiB. Formal schema: `spec/schemas/fiso-meta-1.schema.json`. The top-level member `"schema"` MUST be `"fiso.meta/1"`.

```json
{
  "schema": "fiso.meta/1",
  "album": {
    "title": "Example", "artist": "Example Artist", "date": "1999-04-12",
    "genre": "Rock", "label": "Label", "catalog_number": "CAT-01",
    "country": "FR", "barcode": "0123456789012",
    "group_id": "8b1c…-uuid",
    "musicbrainz": { "release_id": "…", "release_group_id": "…",
                     "album_artist_id": "…", "disc_id": "…" }
  },
  "disc": { "number": 2, "total": 3, "subtitle": "Live" },
  "tracks": [ { "number": 1, "title": "Intro", "artist": "Example Artist",
                "musicbrainz": { "recording_id": "…", "track_id": "…" } } ],
  "cdtext": { "album_title": "…", "album_performer": "…" },
  "tags": { "COMMENT": "free key/value pairs (Vorbis-comment names)" },
  "provenance": {
    "source": "cd | files | cue", "creator": "FISO Explorer 1.0.0",
    "created": "2026-11-08T00:00:00Z",
    "rip": { "mode": "secure | fast", "drive": "…", "read_offset": 6, "c2": true,
             "retried_sectors": 0, "unrecovered_errors": 0,
             "verified": false, "verification": "" },
    "derived_from": "original image file name, when this image was derived from another FISO image",
    "cd_mastering": { "alignment": "shift", "moved_boundaries": 2, "max_shift_ms": 3.9,
                      "end_padding_samples": 547, "converted_from": "96.0 kHz / 24 bit / stereo" }
  }
}
```

Required members: `schema`, `album.title`, `disc.number`, `disc.total` (1 ≤ number ≤ total ≤ 999), `tracks` (one object per LAYT track, matched by `number`). Everything else is optional; an absent member means *unknown*, never an empty value. Readers MUST NOT fail on unknown members.

A FISO image is an archive: writers MUST store the source audio unchanged when creating an image from files, a CUE sheet or a disc. A writer MAY offer, as a separate and explicit operation, to derive a *CD version* of an existing image (a new file; the original is never modified) so that it can be burned as CD-DA (flag `CDDA`). Such an image carries `provenance.derived_from` and `provenance.cd_mastering`, which records what was changed: track starts moved to the nearest sector boundary (`shift`; no audio added or removed between tracks), silence appended after the last track (`end_padding_samples`), and, only with the user's consent, a format conversion (`converted_from`). Its presence tells readers that the image is a CD version and not the archive.

Mapping to common tags: `ALBUM`=album.title, `ALBUMARTIST`=album.artist, `DISCNUMBER`=disc.number, `DISCTOTAL`=disc.total, `DISCSUBTITLE`=disc.subtitle, `ALBUM_GROUP_ID`=album.group_id, `MUSICBRAINZ_ALBUMID`=album.musicbrainz.release_id.

### 6.1 Multi-disc albums

One image = one physical disc. Images of the same album are grouped by, in order of precedence:

1. equal `album.musicbrainz.release_id`;
2. equal `album.group_id` (a UUID shared by the images of one album when no MusicBrainz release exists);
3. otherwise readers MAY propose a heuristic grouping on (`album.title`, `album.artist`, `disc.total`) but MUST present it as a guess.

## 7. `PICT` chunk (binary)

| Field | Size |
|---|---|
| `picture_type` (ID3v2 APIC / FLAC code: 3 = front cover, 4 = back cover, 0 = other…) | 4 |
| `mime_length` | 4 |
| `mime` (ASCII; v1.0 writers use `image/jpeg` or `image/png`) | `mime_length` |
| `description_length` | 4 |
| `description` (UTF-8) | `description_length` |
| `width`, `height`, `color_depth`, `colors_used` | 4 × 4 |
| `data_length` | 4 |
| `data` | `data_length` |

The chunk length MUST equal the sum of the fields. Maximum 64 MiB. Picture bytes are untrusted data: readers MUST decode them with a sandboxed or hardened image decoder and never execute them.

### 7.1 `CDTX` chunk

The CD-Text packs exactly as returned by the drive (READ TOC format 0101b) without the 4-byte response header: a sequence of 18-byte packs (pack type, track number, sequence number, block/character position, 12 text bytes, CRC-16). Length MUST be a multiple of 18, at most 1 MiB. Readers MAY decode it; the authoritative descriptive metadata is always `META`. A writer burning a disc SHOULD reproduce these packs unchanged when the first track number is 1, otherwise build CD-Text from `META`.

## 8. Integrity levels

A conformant verifier reports three **independent** states:

| State | Meaning | Checks |
|---|---|---|
| **Valid file** | structure conforms | magic, version, header CRC, sizes/offsets, table SHA-256, every chunk SHA-256, LAYT/META/PICT rules |
| **Intact audio** | decoded audio equals what was stored | every FLAC stream decodes without error, STREAMINFO MD5 matches, segment and timeline PCM SHA-256 match |
| **Verified rip** | the source audio was checked against an external reference | only when `provenance.rip.verified` is true **and** names the method in `provenance.rip.verification`; never inferred |

## 9. Rebuilding a CD-DA

### 9.1 Disc-at-Once

Write the timeline unchanged in DAO mode. Track N starts at its INDEX 01; INDEX 00 and other indexes are written as sub-channel index marks. No silence is inserted between tracks. ISRC, MCN, pre-emphasis and copy flags come from `LAYT`; CD-Text from `CDTX` if present, else from `META` (`cdtext` then album/tracks titles).

Recommended mapping for software that burns a FISO image directly (burn engines that model a track as *pre-gap + data*). The reference implementation does not burn; it exports a CD-DA BIN/CUE instead (extraction format `bin-cue`):

* track 1: pre-gap = 150 sectors of digital silence followed by the HTOA region `[0, start of track 1)`; data = `[start of track 1, next boundary)`;
* track N > 1: pre-gap = `[INDEX 00, INDEX 01)` (empty when INDEX 00 is absent, i.e. gap-less); data = `[INDEX 01, next boundary)`;
* the *next boundary* is INDEX 00 of the next track if present, else its INDEX 01, else the end of the timeline;
* INDEX 02 … 99 are written as index points relative to INDEX 01.

On the resulting disc, LBA *x* holds timeline sector *x*. A writer SHOULD offer to read the disc back and compare it with the timeline, searching the combined read/write offset.

### 9.2 BIN/CUE export

The BIN file is the canonical PCM of the full timeline. The CUE sheet uses one `FILE … BINARY` and, for every track, `TRACK nn AUDIO`, optional `FLAGS`/`ISRC`, `INDEX 00` (if present), `INDEX 01` and further indexes, positions converted to `mm:ss:ff` from the start of the BIN. When the timeline does not start at LBA 0 or the image is not `CDDA`, exporters MUST warn that an exact disc cannot be rebuilt.

## 10. Writer requirements

* Write to a temporary file in the destination directory, `fsync`, then atomically rename; never modify an existing image in place.
* Metadata edits MUST copy `FLAC` chunks byte-for-byte.
* Write the header last.

## 11. Reader requirements and limits

A reader MUST validate before any allocation: `header_size == 96`, `chunk_count ≤ 65 536`, all offsets/lengths inside the file without integer overflow, `LAYT` length consistent with its counts, `META` ≤ 16 MiB, `PICT` ≤ 64 MiB, track/segment/index rules of §5. Any violation makes the file invalid.

## 12. Media type and extension

Extension `.fiso`. Suggested MIME type `application/x-fiso-image`. Uniform Type Identifier (macOS) `fr.retrospare.fiso-image`, conforming to `public.data`. freedesktop.org magic: string `\x89FISO\r\n\n` at offset 0.

File managers and thumbnailers SHOULD display the front cover (`PICT` type 3, else the first `PICT`) as the icon of a `.fiso` file. The reference implementation ships a Quick Look thumbnail extension (macOS) and a `fiso thumbnail IMAGE OUT` command for freedesktop.org thumbnailers.

## 13. Change history

* 1.0 (draft) — initial version; `CDTX` content (§7.1), burning map (§9.1), icon recommendation (§12).
