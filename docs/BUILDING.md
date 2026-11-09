# Building

## Requirements

| | macOS | Windows | Linux |
|---|---|---|---|
| Compiler | Xcode Command Line Tools (`xcode-select --install`) | Visual Studio 2022 or Build Tools, workload *Desktop development with C++* | GCC 11 or later |
| CMake | 3.20 or later (`brew install cmake`) | 3.20 or later (installed with Visual Studio) | 3.20 or later |
| Node.js | 20 or later (`brew install node`) | 20 or later | 20 or later |
| git | yes | yes | yes |

The first build downloads libFLAC 1.4.3 from https://github.com/xiph/flac and links it statically, so the `fiso` tool has no runtime dependency.

## Engine, command-line tool and tests

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release --parallel
cd build && ctest -C Release --output-on-failure
```

CMake options:

| Option | Default | Effect |
|---|---|---|
| `FISO_USE_SYSTEM_FLAC` | `OFF` | Link the system libFLAC (found with pkg-config) instead of building it |
| `FISO_BUILD_SHARED` | `OFF` | Build `libfiso` as a shared library |
| `FISO_BUILD_TESTS` | `ON` | Build the test suite |

## Application (development)

```sh
cmake --build build --config Release
cd fiso-explorer
npm install
npm start          # uses ../build/fiso-cli/fiso
npm run lint       # ESLint and translation check
```

`FISO_BIN=/path/to/fiso npm start` uses another build of the engine.

## Installers

Each script builds the engine, runs the tests, then packages the application. The result is written to `fiso-explorer/dist/`.

| Target | Run on | Command | Result |
|---|---|---|---|
| macOS Apple Silicon | a Mac | `./packaging/macos/build-mac.sh arm64` | `.dmg` |
| macOS Intel | a Mac | `./packaging/macos/build-mac.sh x64` | `.dmg` |
| Windows x64 | Windows | `powershell -ExecutionPolicy Bypass -File packaging\windows\build-windows.ps1` | `.exe` installer |
| Linux x64 | Linux | `./packaging/linux/build-linux.sh` | `.AppImage` |

Any Mac can build both macOS targets (the other architecture is cross-compiled); the tests run only for the native architecture. The release workflow builds both on Intel runners.

### Continuous integration and releases

* `.github/workflows/ci.yml` builds the engine and runs the tests on macOS, Windows and Linux for every push and pull request, and checks the application code and translations.
* `.github/workflows/release.yml` runs the four installer scripts on GitHub-hosted machines when a release is published, and attaches the installers to the release. It can also be started by hand from the *Actions* tab; the installers are then available as artifacts of the run.

### Code signing

The installers are not signed with an Apple Developer ID or a Windows code-signing certificate. The macOS app is signed *ad hoc* (required on Apple Silicon), so macOS asks for confirmation on first launch (*System Settings › Privacy & Security › Open Anyway*), and Windows SmartScreen shows a warning (*More info › Run anyway*). Signing and notarization can be added to the scripts later without other changes.

## Linux desktop integration (optional)

The AppImage runs without installation. To show the front cover as the icon of `.fiso` files in file managers that use freedesktop thumbnailers (GNOME Files, Nemo, Caja…), install the `fiso` command-line tool in your `PATH`, then:

```sh
sudo cp packaging/linux/fiso-mime.xml /usr/share/mime/packages/fiso.xml
sudo update-mime-database /usr/share/mime
sudo cp packaging/linux/fiso.thumbnailer /usr/share/thumbnailers/
```
