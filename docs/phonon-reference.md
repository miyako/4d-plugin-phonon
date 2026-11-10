# phonon command reference

The `phonon` plugin speaks text with [Phonon](https://gradium.ai/phonon), Gradium's on-device text-to-speech models, entirely offline. It runs the Phonon CPU engine ([`ptts`](https://crates.io/crates/ptts) 0.7.0, statically linked) inside 4D. You load a model folder once and get a Longint reference; each `Phonon Play` then generates and plays speech on two threads of its own and returns a speech reference that you pause, resume, stop and poll. `Phonon Synthesize` returns the speech as a WAV `Blob` instead of playing it.

| Command | Returns | Purpose |
|---|---|---|
| [Phonon Load model](#phonon-load-model) | Longint | Load a model folder |
| [Phonon Load models](#phonon-load-models) | Collection | Load several model folders in parallel |
| [Phonon UNLOAD MODEL](#phonon-unload-model) | — | Release a model |
| [Phonon UNLOAD ALL](#phonon-unload-all) | — | Release every model |
| [Phonon Get models](#phonon-get-models) | Collection | Describe every loaded model |
| [Phonon Get model info](#phonon-get-model-info) | Object | Describe one model: voices, languages, sample rate |
| [Phonon ADD VOICE](#phonon-add-voice) | — | Add a voice file to a loaded model |
| [Phonon Play](#phonon-play) | Longint | Speak text in the background |
| [Phonon Play multiple](#phonon-play-multiple) | Collection | Speak several texts, at once or one after another |
| [Phonon PAUSE](#phonon-pause) | — | Pause a speech |
| [Phonon RESUME](#phonon-resume) | — | Resume a speech |
| [Phonon STOP](#phonon-stop) | — | Stop a speech and release it |
| [Phonon PAUSE ALL](#phonon-pause-all) | — | Pause every speech |
| [Phonon RESUME ALL](#phonon-resume-all) | — | Resume every speech |
| [Phonon STOP ALL](#phonon-stop-all) | — | Stop and release every speech |
| [Phonon SET VOLUME](#phonon-set-volume) | — | Change the volume of a speech while it plays |
| [Phonon Get status](#phonon-get-status) | Object | State and progress of one speech |
| [Phonon Get statuses](#phonon-get-statuses) | Collection | State and progress of several or all speeches |
| [Phonon Synthesize](#phonon-synthesize) | Blob | Speech as WAV data, without playing it |
| [Phonon Get last error](#phonon-get-last-error) | Text | Why the last command of this process failed |
| [Phonon SET THREADS](#phonon-set-threads) | — | CPU threads the engine uses |
| [Phonon Get info](#phonon-get-info) | Object | Plugin and engine versions, counts |

**Platforms:** macOS 11 or later (universal: Apple silicon and Intel), Windows x64.

---

## Requirements & platform notes

- **A model folder.** The plugin never downloads anything. Pass a folder laid out like the model's Hugging Face repository: `config.json`, `tokenizer.json`, the weights (`model.q8.gguf`, or `model.safetensors`; the GGUF file is used when both are there), and voices as `voices/<name>.safetensors`, `embeddings/<name>.safetensors` or `default-voice.safetensors` (listed as the voice `default`). The same folder works on macOS and Windows. For [Phonon 31m](https://huggingface.co/gradium/phonon-31m-en) (English, Apache 2.0) these four files are enough: `config.json`, `tokenizer.json`, `model.q8.gguf`, `default-voice.safetensors` (about 65 MB).
- **Model licenses differ.** Phonon 31m is Apache 2.0. The Phonon 40m models (English, French, German, Spanish, Portuguese, and a multilingual one) are CC BY-NC 4.0 and gated on Hugging Face. The model cards forbid impersonating a real person or cloning a voice without the speaker's consent. Check the card of the model you ship.
- **Audio format.** Phonon models produce 24 kHz mono. Playback and `Phonon Synthesize` use 16-bit PCM.
- **Output device.** **On macOS**, playback goes to the default output device through an Audio Queue (AudioToolbox). **On Windows**, it goes to the default device through `waveOut` (the wave mapper). There is no device selection. A machine without an output device does not fail the `Phonon Play` call: the speech ends in the `error` state with a message starting `audio output:`. Pass `output: "none"` to play silently in real time (for tests, or to drive your own timing).
- **Windows CPU.** The Windows engine is built for the `x86-64-v3` baseline (AVX2, Intel Haswell / AMD Excavator, 2013 or later), as Phonon's own release builds are. It is not built for older processors.
- **Thread safety.** Every command is thread-safe and can run in a preemptive process. `Phonon Play` returns at once; `Phonon Load model`, `Phonon Load models` and `Phonon Synthesize` block the calling process while they work, so call them from a worker if the user interface must stay responsive.
- **One generation per model at a time.** The engine generates one utterance per model at a time. Speeches on the same model queue for generation (they stay `pending`); generation runs about 20 times faster than real time on a recent CPU, so playback is rarely held up. To generate in parallel, load the model more than once.
- **Errors are not 4D errors.** No command raises a 4D error. A command that fails returns `0`, an empty object, collection or `Blob`, and leaves a message for [`Phonon Get last error`](#phonon-get-last-error). Failures during generation or playback show in the speech's status instead (`state` = `"error"`, `error` = the message).
- **Cleanup.** When 4D quits or closes the project, the plugin stops every speech, waits for its threads and frees every model. The engine's own pool of idle worker threads lives until the process ends, so the plugin keeps its code loaded until then.

---

## Phonon Load model

### Syntax

```4d
$model:=Phonon Load model($path; $options)
```

| Parameter | Type | Description |
|---|---|---|
| `$path` | Text | The model folder. A POSIX path (`Folder.path` of a folder made with `fk platform path`, or `File.path`), a Windows path, or **on macOS** a system (HFS) path such as `Folder.platformPath` |
| `$options` | Object | Optional settings, see below. Pass `{}` or omit for the defaults |
| Result | Longint | Model reference, or `0` on failure |

| Property | Type | Description |
|---|---|---|
| `language` | Text | How numbers and symbols in the text are read out. `en` (default), `fr`, `de`, `es`, `pt` normalize numbers and symbols; `ar`, `hi`, `ko`, `pl`, `sv`, `fi`, `no`, `it`, `nl`, `ja`, `zh`, `da`, `tr` normalize symbols only; `none` reads the text as written. A speech can override it |
| `voice` | Text | Default voice for this model. Without it, the checkpoint's own default, then `default`, then the first voice by name |
| `temperature` | Real | Sampling temperature, `0` or more. Higher varies more, `0` removes the randomness. Default: the engine's, `0.3` |
| `seed` | Real | Default sampling seed |
| `conditions` | Object | The model's conditioners by name. `duration_delta` sets the speaking rate: negative is faster (`-0.3`), positive slower (`0.3`), `0` the default. Small values work best |
| `voices` | Collection | Extra voice files: objects `{name: Text; path: Text}`, `path` a POSIX or Windows path |
| `cfgCoef` | Real | Classifier-free guidance coefficient. `1` disables it |
| `eosThreshold` | Real | End-of-speech threshold. Lower lets the model run longer before it ends an utterance |
| `maxTokensPerChunk` | Real | Longest chunk of text generated at once |
| `quantizeMimi` | Boolean | Quantize the audio decoder too |

### Description

Loads the weights, tokenizer and voices of the folder and returns a reference to pass to the other commands. Loading is the slow step: load once and keep the reference for the life of the application.

The path is used as given; it is converted only when, **on macOS**, it does not start with `/` and contains `:` (a system path). Paths inside `voices` are not converted.

`0` is returned when the folder, `config.json` or the weights are missing, when the language code or the `voice` is unknown, or when the files cannot be read. [`Phonon Get last error`](#phonon-get-last-error) then says why.

### Example

From the plugin's own test method (`test_load_model.4dm`):

```4d
var $model : Integer
$model:=Phonon Load model($path; {language: "en"})
ASSERT($model>0; Phonon Get last error)
ASSERT(Phonon Get last error="")

var $info : Object
$info:=Phonon Get model info($model)
ASSERT($info.ref=$model)
ASSERT($info.path=$path)
ASSERT($info.sampleRate=24000)
ASSERT($info.language="en")
ASSERT($info.voices.indexOf("default")#-1)
```

A slower voice, with a voice file from the voice catalog:

```4d
var $folder : 4D.Folder
$folder:=Folder(Get 4D folder(Current resources folder)+"phonon-31m-en"; fk platform path)
$model:=Phonon Load model($folder.path; {conditions: {duration_delta: 0.3}; voices: [{name: "Zoey"; path: $folder.file("Zoey.safetensors").path}]; voice: "Zoey"})
If ($model=0)
	ALERT(Phonon Get last error)
End if 
```

---

## Phonon Load models

### Syntax

```4d
$models:=Phonon Load models($list)
```

| Parameter | Type | Description |
|---|---|---|
| `$list` | Collection | Model folders: each element a path (Text), or an object with `path` and any [`Phonon Load model`](#phonon-load-model) option |
| Result | Collection | One model reference per element, in the same order; `0` where loading failed |

### Description

Loads every element on a thread of its own and waits for all of them. When some fail, [`Phonon Get last error`](#phonon-get-last-error) returns one line per failure, `#index: message`, with the 0-based index of the element.

### Example

From the plugin's own test method (`test_load_models.4dm`):

```4d
var $refs : Collection
$refs:=Phonon Load models([$path; {path: $path; language: "en"; temperature: 0.5}; $path+"no-such-folder"])
ASSERT($refs.length=3)
ASSERT($refs[0]>0)
ASSERT($refs[1]>0)
ASSERT($refs[0]#$refs[1])
ASSERT($refs[2]=0)
ASSERT(Phonon Get last error="#2: @")
```

---

## Phonon UNLOAD MODEL

### Syntax

```4d
Phonon UNLOAD MODEL($model)
```

| Parameter | Type | Description |
|---|---|---|
| `$model` | Longint | Model reference |

### Description

Removes the model from the plugin. Speeches still using it keep it until they end, so unloading never cuts a speech short; its memory is released after the last of them. An unknown reference sets [`Phonon Get last error`](#phonon-get-last-error).

### Example

```4d
Phonon UNLOAD MODEL($model)
```

---

## Phonon UNLOAD ALL

### Syntax

```4d
Phonon UNLOAD ALL
```

### Description

[`Phonon UNLOAD MODEL`](#phonon-unload-model) for every loaded model.

### Example

From the plugin's own test method (`test_play_multiple.4dm`):

```4d
Phonon UNLOAD ALL
$models:=Phonon Get models
ASSERT($models.length=0)
```

---

## Phonon Get models

### Syntax

```4d
$models:=Phonon Get models
```

| Parameter | Type | Description |
|---|---|---|
| Result | Collection | One [model info object](#phonon-get-model-info) per loaded model |

### Example

```4d
var $models : Collection
$models:=Phonon Get models
ASSERT($models.query("ref = :1"; $model).length=1)
```

---

## Phonon Get model info

### Syntax

```4d
$info:=Phonon Get model info($model)
```

| Parameter | Type | Description |
|---|---|---|
| `$model` | Longint | Model reference |
| Result | Object | Model info, or an empty object for an unknown reference |

| Property | Type | Description |
|---|---|---|
| `ref` | Real | Model reference |
| `path` | Text | The path the model was loaded from, as given |
| `voices` | Collection | Voice names (Text) |
| `languages` | Collection | Languages the model was trained for, from its configuration |
| `language` | Text | Default language code for normalizing text |
| `voice` | Text | Default voice |
| `sampleRate` | Real | Output sample rate in Hz (`24000`) |
| `weights` | Text | `"q8_0"` (GGUF) or `"f32"` (safetensors) |
| `device` | Text | Where the engine runs (the CPU) |

### Example

```4d
var $info : Object
$info:=Phonon Get model info($model)
For each ($voice; $info.voices)
	// list the voices in a pop-up menu...
End for each 
```

---

## Phonon ADD VOICE

### Syntax

```4d
Phonon ADD VOICE($model; $name; $path)
```

| Parameter | Type | Description |
|---|---|---|
| `$model` | Longint | Model reference |
| `$name` | Text | Voice name; an existing voice of that name is replaced |
| `$path` | Text | Voice file (`.safetensors`), from the [Phonon voice catalog](https://huggingface.co/gradium/phonon-voices) or Gradium Studio. **On macOS** a system path is accepted too |

### Description

Adds a precomputed voice to a loaded model. Waits for any generation in progress on that model. Check [`Phonon Get last error`](#phonon-get-last-error): it is empty on success. A voice belongs to a model architecture; use voices made for the model you loaded.

### Example

From the plugin's own test method (`test_load_model.4dm`):

```4d
Phonon ADD VOICE($model; "copy"; $path+"default-voice.safetensors")
ASSERT(Phonon Get last error=""; Phonon Get last error)
ASSERT(Phonon Get model info($model).voices.indexOf("copy")#-1)
```

---

## Phonon Play

### Syntax

```4d
$speech:=Phonon Play($model; $text; $options)
```

| Parameter | Type | Description |
|---|---|---|
| `$model` | Longint | Model reference |
| `$text` | Text | What to say |
| `$options` | Object | Optional settings, see below |
| Result | Longint | Speech reference, or `0` when the speech could not be started |

| Property | Type | Description |
|---|---|---|
| `voice` | Text | Voice for this speech. Default: the model's |
| `language` | Text | Language code for this speech. Default: the model's |
| `temperature` | Real | Sampling temperature for this speech |
| `seed` | Real | Sampling seed for this speech |
| `cfgCoef` | Real | Classifier-free guidance coefficient for this speech |
| `maxTokensPerChunk` | Real | Longest chunk of text generated at once |
| `volume` | Real | `0` to `10`; `1` (default) is unchanged, above `1` amplifies and may clip |
| `output` | Text | `"default"` (default): the default output device. `"none"`: no device, played silently in real time |
| `paused` | Boolean | Start paused; [`Phonon RESUME`](#phonon-resume) starts playback |
| `after` | Real | A speech reference: start playing only when that speech has finished, failed or been stopped |
| `file` | Text | Also save the whole speech to this WAV file when generation completes |
| `prebuffer` | Real | Seconds of audio to generate before playback starts, `0` to `60`. Default `0.2` |

### Description

Starts two threads and returns at once. One generates the audio with the model; the other plays it as it arrives, after `prebuffer` seconds are ready (or generation has finished). Follow the speech with [`Phonon Get status`](#phonon-get-status).

`0` is returned, with a message for [`Phonon Get last error`](#phonon-get-last-error), only for an unknown model reference or an invalid `volume`, `output` or `prebuffer`. Everything else is reported in the speech's status: an unknown `voice`, a bad `language` or `temperature`, a missing output device or a `file` that cannot be written ends the speech in the `error` state with an `error` message.

A finished speech stays listed, with its final status, until you release it with [`Phonon STOP`](#phonon-stop) or [`Phonon STOP ALL`](#phonon-stop-all). Its audio is freed as soon as it finishes; what stays is a few numbers.

`file` is written with the volume the speech has when generation completes. `seed` gives the same take each time only approximately when the engine uses several threads; see [`Phonon SET THREADS`](#phonon-set-threads).

### Example

From the plugin's own test method (`test_play.4dm`):

```4d
var $speech : Integer
$speech:=Phonon Play($model; "One, two, three."; {output: Phonon output none})
ASSERT($speech>0; Phonon Get last error)

var $status : Object
$status:=Phonon Get status($speech)
ASSERT($status.ref=$speech)
ASSERT($status.model=$model)
ASSERT($status.output="none")
ASSERT([Phonon state pending; Phonon state playing].indexOf($status.state)#-1)

$status:=phonon_test_wait($speech; 60)
ASSERT($status.state=Phonon state finished; JSON Stringify($status))
ASSERT($status.complete)
ASSERT($status.duration>0.5)
ASSERT($status.position=$status.duration)
```

Speak through the speakers and release the speech once it has finished, from a worker:

```4d
$speech:=Phonon Play($model; "Your report is ready."; {voice: "default"; volume: 0.8})
Repeat 
	DELAY PROCESS(Current process; 10)
	$status:=Phonon Get status($speech)
Until ($status.state=Phonon state finished) || ($status.state=Phonon state error)
If ($status.state=Phonon state error)
	// $status.error says why
End if 
Phonon STOP($speech)
```

---

## Phonon Play multiple

### Syntax

```4d
$speeches:=Phonon Play multiple($list; $options)
```

| Parameter | Type | Description |
|---|---|---|
| `$list` | Collection | What to say: each element a Text, or an object with `text`, optionally `model`, and any [`Phonon Play`](#phonon-play) option |
| `$options` | Object | Shared settings: `model` (the model for elements that name none), `sequential` (Boolean), and any `Phonon Play` option |
| Result | Collection | One speech reference per element, in the same order; `0` where a speech could not be started |

### Description

Calls [`Phonon Play`](#phonon-play) for each element. An element's own properties win over the shared ones.

With `sequential: True`, each speech plays after the one before it (its `after` is the previous speech that started). Generation still runs ahead, so the next speech is usually ready when the previous one ends. Without it, all speeches play at the same time and the system mixes them.

Failures leave one line per element in [`Phonon Get last error`](#phonon-get-last-error), `#index: message`.

### Example

From the plugin's own test method (`test_play_multiple.4dm`):

```4d
var $refs : Collection
$refs:=Phonon Play multiple(["First sentence."; {text: "Second sentence."; volume: 0.8}; {text: "Third, from the other model."; model: $models[1]}]; {model: $models[0]; sequential: True; output: Phonon output none})
ASSERT($refs.length=3)
ASSERT($refs.indexOf(0)=-1; Phonon Get last error)

var $statuses : Collection
$statuses:=Phonon Get statuses($refs)
ASSERT($statuses[1].after=$refs[0])
ASSERT($statuses[2].after=$refs[1])
```

Read a document paragraph by paragraph:

```4d
$refs:=Phonon Play multiple(Split string($document; "\r"; sk ignore empty strings); {model: $model; sequential: True})
```

---

## Phonon PAUSE

### Syntax

```4d
Phonon PAUSE($speech)
```

| Parameter | Type | Description |
|---|---|---|
| `$speech` | Longint | Speech reference |

### Description

Pauses playback; the state becomes `paused` and `position` stops. Generation carries on in the background. Pausing a speech that has not started yet keeps it from starting until [`Phonon RESUME`](#phonon-resume). Pausing a finished speech does nothing. An unknown reference sets [`Phonon Get last error`](#phonon-get-last-error).

### Example

From the plugin's own test method (`test_pause.4dm`):

```4d
Phonon PAUSE($speech)
ASSERT(Phonon Get status($speech).state=Phonon state paused)
```

---

## Phonon RESUME

### Syntax

```4d
Phonon RESUME($speech)
```

| Parameter | Type | Description |
|---|---|---|
| `$speech` | Longint | Speech reference |

### Description

Resumes a paused speech, or starts one played with `paused: True`.

### Example

From the plugin's own test method (`test_pause.4dm`):

```4d
Phonon RESUME($speech)
ASSERT(Phonon Get status($speech).state=Phonon state playing)
```

---

## Phonon STOP

### Syntax

```4d
Phonon STOP($speech)
```

| Parameter | Type | Description |
|---|---|---|
| `$speech` | Longint | Speech reference |

### Description

Stops playback and generation at once and releases the speech: its reference becomes unknown ([`Phonon Get status`](#phonon-get-status) returns `state` = `"unknown"`). Use it on finished speeches too, to release them. Speeches waiting for this one (`after`) start. Returns without waiting for the speech's threads; the plugin joins them later.

### Example

From the plugin's own test method (`test_play.4dm`):

```4d
Phonon STOP($speech)
ASSERT(Phonon Get last error="")
ASSERT(Phonon Get status($speech).state=Phonon state unknown)
```

---

## Phonon PAUSE ALL

### Syntax

```4d
Phonon PAUSE ALL
```

### Description

[`Phonon PAUSE`](#phonon-pause) for every speech.

### Example

```4d
Phonon PAUSE ALL
```

---

## Phonon RESUME ALL

### Syntax

```4d
Phonon RESUME ALL
```

### Description

[`Phonon RESUME`](#phonon-resume) for every speech, including those started paused.

### Example

```4d
Phonon RESUME ALL
```

---

## Phonon STOP ALL

### Syntax

```4d
Phonon STOP ALL
```

### Description

[`Phonon STOP`](#phonon-stop) for every speech, finished or not.

### Example

From the plugin's own test method (`test_play_multiple.4dm`):

```4d
Phonon STOP ALL
$statuses:=Phonon Get statuses
ASSERT($statuses.length=0)
```

---

## Phonon SET VOLUME

### Syntax

```4d
Phonon SET VOLUME($speech; $volume)
```

| Parameter | Type | Description |
|---|---|---|
| `$speech` | Longint | Speech reference |
| `$volume` | Real | `0` to `10`; `1` is unchanged |

### Description

Changes the volume of the audio not yet handed to the device. Up to about 0.4 seconds already queued play at the old volume. A value out of range sets [`Phonon Get last error`](#phonon-get-last-error) and changes nothing.

### Example

From the plugin's own test method (`test_pause.4dm`):

```4d
Phonon SET VOLUME($speech; 0.5)
ASSERT(Phonon Get status($speech).volume=0.5)
```

---

## Phonon Get status

### Syntax

```4d
$status:=Phonon Get status($speech)
```

| Parameter | Type | Description |
|---|---|---|
| `$speech` | Longint | Speech reference |
| Result | Object | Status, see below |

| Property | Type | Description |
|---|---|---|
| `ref` | Real | Speech reference |
| `model` | Real | Model reference |
| `state` | Text | `pending` (waiting for the model, for `after` or for the prebuffer), `playing`, `paused`, `finished`, `error`; `unknown` for a reference that does not exist (then only `ref` and `state` are set) |
| `position` | Real | Seconds played. **On a device**, counted in 0.1 s steps as buffers finish |
| `duration` | Real | Seconds generated so far; the full length once `complete` is true |
| `complete` | Boolean | Generation has ended |
| `volume` | Real | Current volume |
| `output` | Text | `default` or `none` |
| `after` | Real | The speech this one waits for, if any |
| `ttfa` | Real | Milliseconds from `Phonon Play` to the first audio generated, once there is some |
| `file` | Text | The `file` option, if given |
| `error` | Text | Why the speech failed, in the `error` state |

### Example

From the plugin's own test method (`phonon_test_wait.4dm`):

```4d
Repeat 
	$status:=Phonon Get status($ref)
	If ($status.state=Phonon state finished) || ($status.state=Phonon state error) || ($status.state=Phonon state unknown)
		return $status
	End if 
	DELAY PROCESS(Current process; 3)
Until (Milliseconds>$deadline)
```

---

## Phonon Get statuses

### Syntax

```4d
$statuses:=Phonon Get statuses($speeches)
```

| Parameter | Type | Description |
|---|---|---|
| `$speeches` | Collection | Optional speech references. Omit for every speech |
| Result | Collection | One [status object](#phonon-get-status) per existing speech; unknown references are left out |

### Example

```4d
// speeches still talking
var $statuses; $active : Collection
$statuses:=Phonon Get statuses
$active:=$statuses.query("state in :1"; [Phonon state pending; Phonon state playing; Phonon state paused])
```

---

## Phonon Synthesize

### Syntax

```4d
$wav:=Phonon Synthesize($model; $text; $options)
```

| Parameter | Type | Description |
|---|---|---|
| `$model` | Longint | Model reference |
| `$text` | Text | What to say |
| `$options` | Object | Optional: `voice`, `language`, `temperature`, `seed`, `cfgCoef`, `maxTokensPerChunk` and `volume`, as for [`Phonon Play`](#phonon-play) |
| Result | Blob | A WAV file: 16-bit PCM, mono, 24 kHz. Empty on failure |

### Description

Generates the whole speech on the calling process and returns it; nothing is played. It waits for any generation already running on the same model. Use it to save speech, serve it over HTTP or attach it to a record.

### Example

From the plugin's own test method (`test_synthesize.4dm`):

```4d
var $wav : Blob
$wav:=Phonon Synthesize($model; "Hello from Phonon."; {})
ASSERT(Phonon Get last error=""; Phonon Get last error)
ASSERT(BLOB size($wav)>44)
var $offset : Integer
ASSERT(BLOB to text($wav; UTF8 text without length; $offset; 4)="RIFF")
```

Save it:

```4d
$wav:=Phonon Synthesize($model; "Welcome."; {voice: "default"})
If (BLOB size($wav)>0)
	File("/RESOURCES/welcome.wav").setContent($wav)
End if 
```

---

## Phonon Get last error

### Syntax

```4d
$error:=Phonon Get last error
```

| Parameter | Type | Description |
|---|---|---|
| Result | Text | The message left by the last failing command of the current process, or `""` |

### Description

Each 4D process has its own message. Commands that can fail clear it when they succeed and set it when they fail, so read it right after the command you are checking. `Phonon Get models`, `Phonon Get info` and `Phonon Get last error` itself leave it unchanged.

### Example

```4d
$model:=Phonon Load model($path; {})
If ($model=0)
	ALERT(Phonon Get last error)
End if 
```

---

## Phonon SET THREADS

### Syntax

```4d
Phonon SET THREADS($count)
```

| Parameter | Type | Description |
|---|---|---|
| `$count` | Longint | CPU threads, `1` to `1024`; `0` for one per core (the default) |

### Description

The engine creates its pool of worker threads when the first model is loaded, and the pool never grows after that. Call `Phonon SET THREADS` **before** loading the first model; later calls only change how the work is split. One stream of speech rarely uses all cores; measure on your target machines.

With one thread, the same `seed` gives the same audio bit for bit. With several, the order in which threads add up results varies, so takes differ slightly.

`Phonon SET THREADS` also sets the `RAYON_NUM_THREADS` environment variable of the 4D process.

### Example

```4d
// On Startup, before any Phonon Load model
Phonon SET THREADS(2)
```

---

## Phonon Get info

### Syntax

```4d
$info:=Phonon Get info
```

| Property | Type | Description |
|---|---|---|
| `version` | Text | Plugin version |
| `engine` | Text | Phonon engine (`ptts`) version |
| `threads` | Real | CPU threads set for the engine |
| `models` | Real | Loaded models |
| `speeches` | Real | Speeches not yet released |

### Example

From the plugin's own test method (`test_info.4dm`):

```4d
var $info : Object
$info:=Phonon Get info
ASSERT($info.version#"")
ASSERT($info.engine="0.7.0")
ASSERT($info.threads>0)
```

---

## Error handling & troubleshooting

- **`0`, empty object or empty `Blob` returned.** The command failed; [`Phonon Get last error`](#phonon-get-last-error) says why. No 4D error is raised.
- **`Phonon Play` returned a reference but nothing is heard.** Read the speech's status: an unknown voice, a bad language or temperature, or a missing output device end the speech in the `error` state with the reason in `error`.
- **`audio output:` errors.** No usable output device (a server or CI machine, a remote session without audio). Use `output: "none"`, or `Phonon Synthesize` to get the audio as data.
- **Model folder not found on macOS.** Pass a POSIX path or a system path. `Folder(fk database folder).path` is a 4D filesystem path (`/PACKAGE/...`), not a POSIX one: build the folder from its platform path first, `Folder($folder.platformPath; fk platform path).path`.
- **Speeches stay `pending`.** Speeches on one model generate one at a time, and `after` waits for its speech. Load the model twice to generate in parallel.
- **Speeches pile up in `Phonon Get statuses`.** Finished speeches stay until released. Call `Phonon STOP` once you have read the final status, or `Phonon STOP ALL`.
- **The same seed gives slightly different audio.** Expected with several threads; see [`Phonon SET THREADS`](#phonon-set-threads).
- **The plugin does not load on an old Windows PC.** The Windows build requires AVX2 (`x86-64-v3`).

---

## Quick reference

```4d
Phonon SET THREADS(0)  // optional, before the first load
$model:=Phonon Load model($folder.path; {language: "en"; conditions: {duration_delta: 0}})
$voices:=Phonon Get model info($model).voices

$speech:=Phonon Play($model; "Hello."; {voice: "default"; volume: 1})
Phonon PAUSE($speech)
Phonon RESUME($speech)
Phonon SET VOLUME($speech; 0.5)
$status:=Phonon Get status($speech)  // state, position, duration, error
Phonon STOP($speech)

$refs:=Phonon Play multiple(["One."; "Two."]; {model: $model; sequential: True})
Phonon STOP ALL

$wav:=Phonon Synthesize($model; "Saved."; {})
Phonon UNLOAD MODEL($model)
```
