# phonon

English | [日本語](README.ja.md)

Offline text-to-speech for 4D with [Phonon](https://gradium.ai/phonon), Gradium's on-device speech models. Load one or more models, then speak text in the background: each speech runs on threads of its own and can be paused, resumed, stopped and followed while it plays. Speech can also be returned as a WAV `Blob` or saved to a file. The Phonon engine ([`ptts`](https://crates.io/crates/ptts) 0.7.0) is linked into the plugin; nothing is sent over the network.

## Requirements

- 4D v21.1 or later (compatible with the `compatibilityVersion` in `.4DProject`)
- macOS 11 or later (Apple silicon or Intel), or Windows x64 with an AVX2 processor (2013 or later)
- A Phonon model folder, such as [Phonon 31m](https://huggingface.co/gradium/phonon-31m-en) (English, Apache 2.0, about 65 MB). See [Models](#models)

## Installation

Download the latest release from the [Releases](../../releases) page.

### macOS & Windows (single download)

1. Download the `.zip` from the release
2. Extract to get the `phonon.bundle` folder
3. Copy the `.bundle` into your 4D application's **Plugins** folder (or your database's **Plugins** folder)
4. Restart 4D

### macOS only (notarized DMG)

1. Download the `.dmg` from the release
2. Mount it and copy the `.bundle` into your **Plugins** folder
3. Restart 4D

## Models

The plugin loads a folder laid out like the model's Hugging Face repository. The same folder works on macOS and Windows. For Phonon 31m, download these four files into one folder:

```bash
hf download gradium/phonon-31m-en config.json tokenizer.json model.q8.gguf default-voice.safetensors --local-dir phonon-31m-en
```

The Phonon 40m models (English, French, German, Spanish, Portuguese and multilingual) are CC BY-NC 4.0 and gated: accept their terms on Hugging Face first. More voices are in the [voice catalog](https://huggingface.co/gradium/phonon-voices). The model cards forbid impersonating a real person or cloning a voice without consent.

## Commands

The full reference, with every option and status property, is in [docs/phonon-reference.md](docs/phonon-reference.md).

| Command | Returns | Purpose |
|---|---|---|
| `Phonon Load model(path; options)` | Longint | Load a model folder; `0` on failure |
| `Phonon Load models(list)` | Collection | Load several model folders in parallel |
| `Phonon UNLOAD MODEL(model)` | — | Release a model (speeches using it finish first) |
| `Phonon UNLOAD ALL` | — | Release every model |
| `Phonon Get models` | Collection | Describe every loaded model |
| `Phonon Get model info(model)` | Object | Voices, languages, sample rate of a model |
| `Phonon ADD VOICE(model; name; path)` | — | Add a voice file to a model |
| `Phonon Play(model; text; options)` | Longint | Speak in the background; returns a speech reference |
| `Phonon Play multiple(list; options)` | Collection | Speak several texts, at once or in sequence |
| `Phonon PAUSE(speech)` | — | Pause a speech |
| `Phonon RESUME(speech)` | — | Resume a speech |
| `Phonon STOP(speech)` | — | Stop a speech and release it |
| `Phonon PAUSE ALL` | — | Pause every speech |
| `Phonon RESUME ALL` | — | Resume every speech |
| `Phonon STOP ALL` | — | Stop and release every speech |
| `Phonon SET VOLUME(speech; volume)` | — | Change the volume while playing |
| `Phonon Get status(speech)` | Object | State, position, duration, error of a speech |
| `Phonon Get statuses(speeches)` | Collection | Status of several or all speeches |
| `Phonon Synthesize(model; text; options)` | Blob | Speech as WAV data (16-bit, mono, 24 kHz) |
| `Phonon Get last error` | Text | Why the last command of this process failed |
| `Phonon SET THREADS(count)` | — | CPU threads; set before the first model is loaded |
| `Phonon Get info` | Object | Plugin and engine versions, counts |

Every command is thread-safe. No command raises a 4D error: failures return `0` or an empty value and leave a message for `Phonon Get last error`, and failures during playback put the speech in the `error` state.

When 4D quits or closes the project, the plugin stops every speech, waits for its threads and frees every model.

**Example:**

```4d
var $folder : 4D.Folder
$folder:=Folder(Get 4D folder(Current resources folder)+"phonon-31m-en"; fk platform path)

var $model : Integer
$model:=Phonon Load model($folder.path; {language: "en"})
If ($model=0)
	ALERT(Phonon Get last error)
	return 
End if 

var $speech : Integer
$speech:=Phonon Play($model; "Your report is ready."; {voice: "default"})

var $status : Object
Repeat 
	DELAY PROCESS(Current process; 10)
	$status:=Phonon Get status($speech)
Until ($status.state=Phonon state finished) || ($status.state=Phonon state error)
Phonon STOP($speech)

var $wav : Blob
$wav:=Phonon Synthesize($model; "Saved for later."; {})
```

## Constants

| Constant | Type | Value | Description |
|---|---|---|---|
| `Phonon state pending` | Text | `pending` | Not playing yet: waiting for the model, the previous speech or the first audio |
| `Phonon state playing` | Text | `playing` | Playing |
| `Phonon state paused` | Text | `paused` | Paused |
| `Phonon state finished` | Text | `finished` | Played to the end |
| `Phonon state error` | Text | `error` | Failed; the status's `error` says why |
| `Phonon state unknown` | Text | `unknown` | No speech with that reference (released or never existed) |
| `Phonon output default` | Text | `default` | Play on the default output device |
| `Phonon output none` | Text | `none` | Play silently in real time, without a device |

## Building from Source

### Prerequisites

- CMake 3.20+
- Xcode (macOS) or Visual Studio 2022 (Windows)
- [Rust](https://rustup.rs) (stable). CMake runs `cargo` to build the engine from [`phonon-core`](phonon-core) and adds the Rust targets it needs with `rustup`

### Clone

```bash
git clone --recurse-submodules https://github.com/miyako/4d-plugin-phonon.git
cd 4d-plugin-phonon
```

### Build (macOS)

```bash
cd phonon
mkdir -p cmake-build && cd cmake-build
cmake .. -DCMAKE_BUILD_TYPE=Release -DCMAKE_OSX_ARCHITECTURES="arm64;x86_64"
cmake --build .
```

### Build (Windows)

```pwsh
cd phonon
mkdir cmake-build; cd cmake-build
cmake .. -A x64
cmake --build . --config Release
```

### Run Tests

Requires [tool4d](https://developer.4d.com/docs/Admin/cli/) (free, no license needed). The first run downloads Phonon 31m (about 65 MB) into `phonon-test/models`:

```bash
/path/to/tool4d --dataless --startup-method=test_all --project=$(pwd)/phonon-test/Project/phonon.4DProject
```

## CI/CD

This project uses the canonical generated three-workflow native-plugin pipeline:

| Workflow | Trigger | Purpose |
|---|---|---|
| `test.yml` | Push / PR / manual / reusable | Build, test and validate same-revision artifacts |
| `bump-version.yml` | Manual, default branch only | Bump `VERSION` and commit; no tag |
| `release.yml` | Manual, default branch only; no inputs | Reuse tests, sign, notarize, verify, draft and publish `vVERSION` |

### Required Secrets (for `release.yml` only)

Configure these in **Settings → Secrets and variables → Actions**:

| Secret | Description |
|---|---|
| `APPLE_DEVELOPER_ID_CERTIFICATE` | Base64-encoded Developer ID Application `.p12` including its private key |
| `APPLE_DEVELOPER_ID_CERTIFICATE_PASSWORD` | Password for the `.p12` export |
| `KEYCHAIN_PASSWORD` | Independent password for the CI runner's temporary keychain |
| `NOTARYTOOL_APPLE_ID` | Apple ID email for notarization |
| `NOTARYTOOL_TEAM_ID` | Apple Developer Team ID (10-char alphanumeric) |
| `NOTARYTOOL_PASSWORD` | Apple app-specific password, not the account password |

## License

[MIT](LICENSE). The plugin links Rust crates statically, including the Phonon engine (MIT OR Apache-2.0); their licenses are in [THIRD-PARTY-NOTICES](THIRD-PARTY-NOTICES/phonon-core.txt). After changing `phonon-core/Cargo.lock`, regenerate that file with `python3 phonon-core/third_party_notices.py`. Phonon models have their own licenses (see [Models](#models)).
