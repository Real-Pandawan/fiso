# Using libfiso

`libfiso` implements FISO v1.0 behind a stable C API declared in [`libfiso/include/fiso/fiso.h`](../libfiso/include/fiso/fiso.h). It has no GUI dependency; its only third-party dependency is libFLAC.

## Conventions

* All strings are UTF-8. Functions returning `char *` give ownership to the caller: release with `fiso_free()`.
* Functions return a `fiso_status` (`FISO_OK` = 0). `fiso_last_error()` returns a human-readable message for the calling thread.
* A `fiso_image *` handle must not be used from two threads at once. Different handles are independent.
* Long operations accept a progress callback; returning non-zero cancels the operation and leaves no partial output file.

## Reading PCM (e.g. adding FISO support to a media player)

```c
#include <fiso/fiso.h>

fiso_image *img;
if (fiso_open("album.fiso", &img) != FISO_OK) { puts(fiso_last_error()); return; }
fiso_format f;
fiso_get_format(img, &f);                 /* 44100 Hz, 2 ch, 16 bit for CD-DA */
int32_t buf[4096 * 8];
uint64_t pos = 0;
while (pos < f.total_samples) {
    int64_t n = fiso_read_pcm_s32(img, pos, 4096, buf);   /* random access, gap-less */
    if (n <= 0) break;
    /* buf holds n interleaved frames, right-justified signed integers */
    pos += (uint64_t)n;
}
char *info = fiso_info_json(img);         /* tracks, indexes, metadata, pictures… */
fiso_free(info);
fiso_close(img);
```

Track boundaries, indexes and pre-gaps are in `fiso_info_json()` (`tracks[].start`, `tracks[].length`, `tracks[].indexes`, positions in samples on the disc timeline).

## Creation plan (`fiso_create`)

```json
{
  "output": "Album - Disc 2.fiso",
  "overwrite": false,
  "compression": 8,
  "flac_passthrough": true,
  "verify": true,
  "source": { "kind": "cue", "path": "disc2.cue" },
  "meta": { "album": { "title": "…", "artist": "…", "group_id": "uuid shared by the discs" },
            "disc": { "number": 2, "total": 3 },
            "tracks": [ { "number": 1, "title": "…" } ] },
  "pictures": [ { "type": 3, "path": "front.jpg" }, { "type": 4, "path": "back.jpg" } ]
}
```

Source kinds:

* `{"kind":"files","files":["01.flac","02.wav"]}` — one track per file, same audio format for all files (never converted silently).
* `{"kind":"fiso","path":"album.fiso"}` — derive a new image from an existing one (metadata and artwork are kept unless the plan replaces them; `provenance.derived_from` is set and a verified-rip state is reset). Used for the CD version of an image (below).
* `{"kind":"cue","path":"album.cue"}` — `BINARY`, `MOTOROLA`, `WAVE` (WAV or FLAC content), several `FILE` entries, `INDEX`, `PREGAP`, `POSTGAP`, `ISRC`, `FLAGS`, `CATALOG`, `TITLE`/`PERFORMER`, `REM GENRE/DATE/DISCNUMBER/TOTALDISCS`. Data tracks are refused.
* `{"kind":"raw","path":"disc.bin","tracks":[{"number":1,"isrc":"…","flags":0,"indexes":[[0,0],[1,58800]]}],"toc":[…],"start_lba":0,"physical":true}` — raw little-endian CD-DA with an explicit layout in samples; this is the entry point for disc rippers.

### Audio CD export (`bin-cue`)

Creation never alters the audio: an image made from FLAC/WAV files whose lengths are not whole CD sectors (588 samples), or from high-resolution files, is an exact archive and is not flagged `CDDA`. FISO Explorer does not burn discs; to burn one, export a BIN/CUE that any burning software can write (CLI: `fiso extract IMAGE --format bin-cue [--convert]`):

```json
{ "format": "bin-cue", "out_dir": "cd/", "allow_conversion": false }
```

* A CD-DA image (e.g. ripped from a CD) is written as it is, sample for sample.
* Otherwise each track start moves to the nearest sector boundary (at most 294 samples, 6.7 ms); no audio is added or removed between tracks, so gap-less albums stay gap-less. Silence completes the last sector.
* Audio that is not 44.1 kHz / 16-bit / stereo is refused with `[needs_conversion]` unless `"allow_conversion": true`: windowed-sinc resampling, TPDF dither to 16 bits, mono copied to both channels. More than two channels is refused with `[multichannel]` (no downmix).

Changes are reported in `warnings` (`[aligned]`, `[end_padding]`, `[converted]`) and in `cd_mastering`; the FISO image is never modified. Internally the export derives a temporary CD version with `fiso_create` (`{"source":{"kind":"fiso","path":…},"cd_compatible":true}`), which applications may also use to keep a CD version as a FISO image (`provenance.cd_mastering`); `cd_compatible` with any other source kind is refused with `[cd_version_from_fiso]`.

`flac_passthrough` (default `true`) stores the frames of FLAC sources as they are when possible. Set it to `false` to decode them and encode every track again with the same encoder and settings (`compression`), whatever their origin; the audio is unchanged, only the compressed data is. The image records how its frames were produced in `provenance.encoding` (`encoder`, `compression`, and `copied_tracks` for the tracks whose frames were kept from the sources).

The returned report contains `pcm_bytes`, `fiso_bytes`, `ratio`, `cdda`, `flac_passthrough_tracks`, `encoding`, `warnings` and the full `verify` report.

## Editing, extraction, grouping

See the comments in `fiso.h` for `fiso_edit`, `fiso_extract` and `fiso_group`. The `fiso` command-line tool ([`fiso-cli/main.cpp`](../fiso-cli/main.cpp)) is a complete example that uses nothing but the public API.

## CD drives

`fiso_cd_drives`, `fiso_cd_info`, `fiso_cd_rip`, `fiso_cd_eject` and `fiso_accuraterip` are declared in `fiso.h`. The CD logic (secure reading, offset correction, pre-gap detection, AccurateRip) is portable C++ in `libfiso/src/cd_core.cpp`; a platform supplies a `cd::Reader`, drive listing and eject:

| File | Platform | How the drive is read |
|---|---|---|
| `cd_macos.cpp` | macOS | IOCDMedia ioctls; DiscRecording (loaded at run time) for drive listing and eject |
| `cd_linux.cpp` | Linux | MMC commands (READ TOC, READ CD, READ SUB-CHANNEL) sent through SG_IO to `/dev/srN` |
| `cd_windows.cpp` | Windows | CD-ROM class driver ioctls (`IOCTL_CDROM_RAW_READ`, `IOCTL_CDROM_READ_TOC_EX`, `IOCTL_CDROM_READ_Q_CHANNEL`); no administrator rights |
| `cd_none.cpp` | others | returns `FISO_E_UNSUPPORTED` |

`cd_mmc.cpp` implements `cd::Reader` on top of any `cd::MmcTransport` (one method that sends a command descriptor block). The tests drive it with a simulated MMC drive, so the command encoding and decoding is covered without hardware.

Engine error messages start with a stable identifier in brackets, e.g. `[no_media]`, `[read_errors]`, `[needs_conversion]`, so that applications can translate them.

## `fiso` exit codes

| Code | Meaning |
|---:|---|
| 0 | success |
| 1 | invalid arguments / usage |
| 2 | I/O error |
| 3 | invalid FISO file or invalid input |
| 4 | integrity failure (hash or decode mismatch) |
| 5 | unsupported (valid but not handled, e.g. data track, future major version) |
| 6 | output exists and `--overwrite` was not given |
| 7 | cancelled (SIGINT/SIGTERM) — no partial file is left |
| 8 | FLAC codec error |
| 9 | internal error |

With `--progress-json`, progress and errors are written to stderr as one JSON object per line: `{"progress":{"phase":"encode","done":…,"total":…}}`, `{"error":{"code":…,"status":"…","message":"…"}}`.
