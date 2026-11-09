# Contributing

Thank you for your interest in FISO. Bug reports, questions and pull requests are welcome.

## Reporting a problem

Open an issue and include:

* the version of FISO Explorer (shown in its *About* window) and your system (macOS / Windows / Linux, version, Intel or Apple Silicon);
* what you did, what you expected and what happened;
* for CD ripping: the drive model and the rip report;
* if possible, the output of `fiso verify IMAGE --json`.

Please do not attach copyrighted music.

## Development

See [docs/BUILDING.md](docs/BUILDING.md) to build the engine and the application, and [docs/DEVELOPERS.md](docs/DEVELOPERS.md) for the C API.

Before sending a pull request:

* `ctest --output-on-failure` passes (in the CMake build folder);
* `npm run lint` passes (in `fiso-explorer/`): it runs ESLint, checks the formatting and checks that every translation has the same keys as `src/renderer/i18n/en.js`;
* code is formatted: `npm run format` for JavaScript, `clang-format -i` for C++ (settings in `.clang-format`);
* every bug fix comes with a test;
* user-visible text goes through the translation files — add the English string first, then the other languages (an English placeholder is better than a missing key).

## Code layout

| Path | Role |
|---|---|
| `libfiso/include/fiso/fiso.h` | Public C API — keep it backward compatible |
| `libfiso/src/` | Format implementation: `layout` (disc structure), `image` (reading), `writer` / `create` (writing), `ops` (verify, edit, extract), `cd_*` (CD drives and AccurateRip), `capi.cpp` (C API) |
| `fiso-cli/main.cpp` | The `fiso` tool; uses only the public API |
| `fiso-explorer/src/main.js` | Electron main process: windows, menus, files, runs `fiso` |
| `fiso-explorer/src/library.js` | Batch creation: finds album folders, reads FLAC tags and artwork, completes them with MusicBrainz |
| `fiso-explorer/src/renderer/` | User interface (plain JavaScript, no framework) |
| `fiso-explorer/native/thumbnail/` | macOS Quick Look extension (Finder icons) |
| `packaging/` | Installer scripts |

## Rules

* The specification in `spec/` is the reference. A change in libfiso that affects files must be reflected in the specification; an incompatible change requires a new major format version.
* FISO never alters audio silently: any conversion must be requested explicitly and reported.
* Test material must be synthetic or explicitly licensed for redistribution.

## License

Code contributions are accepted under the Apache License 2.0. Contributions to `spec/` are dedicated to the public domain under CC0 1.0.
