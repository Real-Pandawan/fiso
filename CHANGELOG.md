# Changelog

All notable changes to this project are documented in this file. Versions follow [Semantic Versioning](https://semver.org/).

## 1.2.0

* Tabs: each tab is an independent session (home, image, creation, batch). A creation keeps running in its tab while you work in others; the tab shows its progress and a mark when it is done.
* At most 4 images are created (or CDs ripped) at the same time, across all tabs; the others wait for a free slot and can be cancelled while waiting.
* Batch creation: add one or more folders of albums and FISO Explorer finds every album inside, at any depth. Tags and artwork of the files (embedded picture or cover.jpg, folder.jpg…) come first; MusicBrainz completes what they do not say, matching artist, album and number of tracks. When the match is not certain, the image is created from the tags and marked "to check". Images are named "Artist - Album.fiso" in the chosen folder and existing images are never replaced.
* New menu items: New Tab (Ctrl/Cmd+T), Batch Creation (Ctrl/Cmd+Shift+N), Close Tab (Ctrl/Cmd+Shift+W).

* FISO Explorer uses the full window: the left column is gone. A top bar holds the logo (back to the home screen) on the left and Settings and Help on the right.
* Open images are listed on the home screen.
* Settings and Help close with a cross (or Escape) and return to the home screen.
* Settings show the installed version of FISO Explorer and of its engine.

## 1.1.1

* macOS: the front cover now fills the whole file icon in the Finder on Retina displays (it was drawn at half size in a corner).

## 1.1.0

* Creation: optional re-encoding of FLAC sources ("Re-encode FLAC files" in FISO Explorer, `--reencode` in the `fiso` tool, `"flac_passthrough": false` in the API). Every track is then encoded with the same encoder and settings, whatever the origin of the files.
* Images record how their audio frames were produced in `provenance.encoding` (encoder, compression level, tracks copied from FLAC sources).

## 1.0.0

First public release.

### Format

* FISO v1.0 specification (English and French, CC0): container with signature, header CRC and chunk table; `LAYT` disc layout (tracks, indexes, pre-gaps, hidden track one audio, ISRC, MCN, flags, TOC); FLAC audio segments cut at INDEX 01; `META` metadata (`fiso.meta/1`, JSON schema); `PICT` artwork; `CDTX` raw CD-Text; `RLOG` rip log; SHA-256 per chunk.
* Three integrity levels: valid file, intact audio, verified rip.
* Multi-disc albums through a shared album identifier.

### Library and command-line tool

* `libfiso`, a C++20 library with a stable C API (`fiso.h`): create (FLAC/WAV files, CUE sheets, raw CD layout), read, random-access PCM, verify, edit metadata without touching the audio, extract, group discs, MusicBrainz disc ID.
* FLAC files are stored without re-encoding when their frames can be kept as they are.
* `fiso` command-line tool built on the public API, with machine-readable progress.
* Audio CD export (`bin-cue`): a BIN/CUE that any burning software can write. Track starts that do not fall on CD sectors move to the nearest sector without adding gaps; audio that is not 44.1 kHz / 16-bit / stereo is converted only on request; the image itself is never modified.
* CD ripping on macOS, Windows and Linux: secure and fast modes, C2 error pointers, read-offset correction, pre-gap detection through sub-channel Q, hidden track one audio, ISRC, MCN, CD-Text. Linux drives are read with MMC commands through SG_IO, Windows drives through the CD-ROM class driver (no administrator rights needed).
* AccurateRip v1/v2 checksums, database lookup and drive offsets; "verified rip" is recorded only when every track matches.

### FISO Explorer

* Desktop application for macOS (Apple Silicon and Intel), Windows and Linux.
* Create, open, play (gap-less), verify, edit metadata, export tracks, artwork and metadata, convert to a burnable audio CD image.
* MusicBrainz lookup with release covers and track counts; artwork from the Cover Art Archive.
* Front cover shown as the icon of `.fiso` files in the macOS Finder (Quick Look extension); optional freedesktop thumbnailer on Linux.
* Interface in English, French, Spanish, German, Portuguese, Italian, Dutch and Japanese; light and dark themes.

### Quality

* Conformance test suite (237 checks, including simulated CD drives at the reader and MMC command levels) run on macOS, Windows and Linux.
* Freely redistributable sample images in `test-files/`.
