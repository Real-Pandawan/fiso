/*
 * libfiso — reference implementation of the FISO Open Audio Disc Format v1.0
 * Copyright 2026 Anthony Gentil (Retro-Spare) and contributors
 * SPDX-License-Identifier: Apache-2.0
 *
 * Stable C API. All strings are UTF-8. Every function returning `char*` hands
 * ownership to the caller, who must release it with fiso_free(). Handles are
 * opaque; a handle must not be used from two threads at the same time.
 */
#ifndef FISO_FISO_H
#define FISO_FISO_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#if defined(_WIN32) && defined(FISO_SHARED)
#ifdef FISO_BUILDING
#define FISO_API __declspec(dllexport)
#else
#define FISO_API __declspec(dllimport)
#endif
#elif defined(__GNUC__)
#define FISO_API __attribute__((visibility("default")))
#else
#define FISO_API
#endif

#define FISO_FORMAT_VERSION_MAJOR 1
#define FISO_FORMAT_VERSION_MINOR 0
#define FISO_LIB_VERSION "1.1.0"

/* Error / status codes. Also used as process exit codes by the `fiso` tool. */
typedef enum fiso_status {
    FISO_OK = 0,
    FISO_E_ARG = 1,         /* invalid argument / usage            */
    FISO_E_IO = 2,          /* file system or I/O error            */
    FISO_E_FORMAT = 3,      /* not a valid FISO file / bad input    */
    FISO_E_INTEGRITY = 4,   /* hash or decode mismatch             */
    FISO_E_UNSUPPORTED = 5, /* valid but unsupported feature       */
    FISO_E_EXISTS = 6,      /* output exists and overwrite not set */
    FISO_E_CANCELLED = 7,   /* cancelled by the progress callback  */
    FISO_E_FLAC = 8,        /* libFLAC encoder/decoder failure     */
    FISO_E_INTERNAL = 9
} fiso_status;

typedef struct fiso_image fiso_image;

/* Progress callback: phase is a short ASCII identifier ("encode", "verify",
 * "copy", "extract"...). Return non-zero to cancel the operation. */
typedef int (*fiso_progress_fn)(void *user, const char *phase, uint64_t done, uint64_t total);

FISO_API const char *fiso_version(void);
FISO_API const char *fiso_status_string(int status);
FISO_API void fiso_free(void *p);

/* Last error message of the calling thread (valid until the next call). */
FISO_API const char *fiso_last_error(void);

/* ---- Reading ---------------------------------------------------------- */

FISO_API int fiso_open(const char *path, fiso_image **out);
FISO_API void fiso_close(fiso_image *img);

/* JSON description: format, layout (tracks, indexes, segments, toc),
 * metadata, pictures, sizes, MusicBrainz disc id. */
FISO_API char *fiso_info_json(fiso_image *img);

/* Raw META chunk JSON. */
FISO_API char *fiso_meta_json(fiso_image *img);

typedef struct fiso_format {
    uint32_t sample_rate;
    uint32_t channels;
    uint32_t bits_per_sample;
    uint64_t total_samples; /* sample frames on the timeline */
    int cdda;               /* 1 when the timeline is CD-DA conformant (sector-aligned 44.1 kHz/16/2) */
} fiso_format;

FISO_API int fiso_get_format(fiso_image *img, fiso_format *out);

/* Random access to the decoded timeline. Writes `count` frames of
 * interleaved 32-bit signed samples (right-justified) into `out`.
 * Returns the number of frames written, or a negative fiso_status. */
FISO_API int64_t fiso_read_pcm_s32(fiso_image *img, uint64_t start, uint64_t count, int32_t *out);

/* ---- Verification ----------------------------------------------------- */

/* Full verification report as JSON (see spec §8). `deep`=0 skips decoding. */
FISO_API int fiso_verify(const char *path, int deep, fiso_progress_fn cb, void *user, char **report_json);

/* ---- Creation --------------------------------------------------------- */

/* Creates an image from a JSON plan (see docs/DEVELOPERS.md). */
FISO_API int fiso_create(const char *plan_json, fiso_progress_fn cb, void *user, char **report_json);

/* ---- Editing ---------------------------------------------------------- */

/* Rewrites `path` (or writes `out_path` when not NULL) with new metadata.
 * Audio chunks are copied byte-for-byte. `edit_json`:
 *  {"meta":{...}, "pictures":[{"type":3,"path":"cover.jpg"}],
 *   "remove_picture_types":[3,4], "remove_all_pictures":false} */
FISO_API int fiso_edit(const char *path, const char *edit_json, const char *out_path, fiso_progress_fn cb, void *user);

/* ---- Extraction ------------------------------------------------------- */

/* `options_json`: {"format":"flac|wav|bin-cue|cover|meta", "out_dir":"…",
 *  "tracks":[1,2], "template":"{track:02} - {title}", "overwrite":false,
 *  "include_htoa":false, "allow_conversion":false}. Returns a JSON list of written files.
 * "bin-cue" writes a BIN/CUE that burning software can write as an audio CD: a CD-DA
 * image is written as it is; otherwise track starts are moved to the nearest CD sector
 * and the end is padded, and audio that is not 44.1 kHz/16-bit/stereo is converted only
 * when "allow_conversion" is true (else [needs_conversion]). The image is never modified. */
FISO_API int fiso_extract(const char *path, const char *options_json, fiso_progress_fn cb, void *user, char **result_json);

/* ---- Multi-disc grouping ---------------------------------------------- */

/* `paths_json`: JSON array of image paths. Returns groups as JSON. */
FISO_API int fiso_group(const char *paths_json, char **result_json);

/* ---- CD drives (macOS, Linux and Windows; other platforms return FISO_E_UNSUPPORTED) ---- */

/* Lists optical drives with their capabilities and media state. */
FISO_API int fiso_cd_drives(char **result_json);

/* Reads the TOC of the audio CD in `device` (NULL or "" = first drive with a disc):
 * tracks, ISRC, MCN, CD-Text, MusicBrainz disc id, AccurateRip ids. */
FISO_API int fiso_cd_info(const char *device, char **result_json);

/* Rips the audio CD into a FISO image. `options_json`:
 *  {"device":"dr:0", "output":"Album.fiso", "mode":"secure|fast", "read_offset":6,
 *   "retries":16, "meta":{...}, "pictures":[...], "compression":8, "overwrite":false,
 *   "detect_pregaps":true, "read_htoa":true} */
FISO_API int fiso_cd_rip(const char *options_json, fiso_progress_fn cb, void *user, char **report_json);

FISO_API int fiso_cd_eject(const char *device);

/* AccurateRip checksums (v1 and v2) of every track and the database URL.
 * When `db_path` names a downloaded dBAR file, the result also says which tracks match. */
FISO_API int fiso_accuraterip(const char *image_path, const char *db_path, fiso_progress_fn cb, void *user, char **result_json);

#ifdef __cplusplus
}
#endif
#endif
