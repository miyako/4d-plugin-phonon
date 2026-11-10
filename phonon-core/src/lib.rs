//! A small static C interface to the Phonon CPU engine (`ptts::synth::Synth`), for the 4D
//! plugin. It is modelled on `ptts-ffi`'s CPU engine, but:
//!
//! - it runs the CPU engine on every platform, so macOS and Windows load the same checkpoint
//!   folder (the layout of a Hugging Face model repo);
//! - per-utterance settings (voice, language, temperature, seed) are passed with each call as
//!   JSON instead of being kept on the handle, so concurrent callers cannot disturb each other;
//! - one model generates one utterance at a time (the engine refuses overlapping generations),
//!   so each handle serializes `phonon_speak` behind a mutex.
//!
//! Every entry point catches panics: unwinding across the C boundary would abort the host.
//! Strings returned to the caller are allocated here and released with `phonon_string_free`.

use ptts::checkpoint::read_config;
use ptts::preprocess::Normalize;
use ptts::synth::{DeviceKind, Quant, SpeechOptions, Synth, SynthBuilder};
use serde::Deserialize;
use std::collections::BTreeMap;
use std::ffi::{CStr, CString, c_char, c_void};
use std::panic::{AssertUnwindSafe, catch_unwind};
use std::path::{Path, PathBuf};
use std::sync::Mutex;

/// Weight files tried in order, with the format each loads as.
const WEIGHTS: [(&str, Quant); 2] =
    [("model.q8.gguf", Quant::Q80), ("model.safetensors", Quant::F32)];

pub type PhononPcmFn = extern "C" fn(*const f32, usize, *mut c_void) -> bool;

#[derive(Deserialize, Default)]
#[serde(default, rename_all = "camelCase")]
struct LoadOptions {
    language: Option<String>,
    voice: Option<String>,
    temperature: Option<f32>,
    seed: Option<u64>,
    cfg_coef: Option<f32>,
    eos_threshold: Option<f32>,
    max_tokens_per_chunk: Option<usize>,
    quantize_mimi: Option<bool>,
    conditions: BTreeMap<String, serde_json::Value>,
    voices: Vec<VoiceFile>,
}

#[derive(Deserialize)]
struct VoiceFile {
    name: String,
    path: String,
}

#[derive(Deserialize, Default)]
#[serde(default, rename_all = "camelCase")]
struct SpeakOptions {
    voice: Option<String>,
    language: Option<String>,
    temperature: Option<f32>,
    seed: Option<u64>,
    cfg_coef: Option<f32>,
    max_tokens_per_chunk: Option<usize>,
}

struct Inner {
    synth: Synth,
    voices: Vec<String>,
}

pub struct PhononModel {
    inner: Mutex<Inner>,
    /// Model facts that do not change, kept outside the lock so that `phonon_info` never waits
    /// behind a generation.
    info: Mutex<serde_json::Value>,
    language: String,
    default_voice: Option<String>,
    sample_rate: u32,
}

fn c_string(s: &str) -> *mut c_char {
    CString::new(s.replace('\0', " ")).expect("NULs were replaced").into_raw()
}

fn set_error(error: *mut *mut c_char, message: &str) {
    if !error.is_null() {
        unsafe { *error = c_string(message) };
    }
}

fn guarded<T>(f: impl FnOnce() -> Result<T, String>) -> Result<T, String> {
    catch_unwind(AssertUnwindSafe(f)).unwrap_or_else(|p| {
        let msg = p
            .downcast_ref::<&str>()
            .map(|s| s.to_string())
            .or_else(|| p.downcast_ref::<String>().cloned())
            .unwrap_or_else(|| "unknown".into());
        Err(format!("internal error: {msg}"))
    })
}

unsafe fn str_arg(p: *const c_char) -> String {
    if p.is_null() { String::new() } else { unsafe { CStr::from_ptr(p) }.to_string_lossy().into_owned() }
}

fn parse<T: Default + for<'de> Deserialize<'de>>(json: &str) -> Result<T, String> {
    if json.trim().is_empty() {
        return Ok(T::default());
    }
    serde_json::from_str(json).map_err(|e| format!("invalid options: {e}"))
}

fn parse_normalize(lang: &str) -> Result<Normalize, String> {
    lang.parse::<Normalize>().map_err(|e| format!("language {lang:?}: {e}"))
}

fn check_temperature(t: f32) -> Result<(), String> {
    if !t.is_finite() || t < 0.0 {
        return Err(format!("temperature must be finite and >= 0, got {t}"));
    }
    Ok(())
}

fn condition_value(v: &serde_json::Value) -> String {
    match v {
        serde_json::Value::String(s) => s.clone(),
        other => other.to_string(),
    }
}

fn load(dir: &Path, opts: LoadOptions) -> Result<PhononModel, String> {
    let language = opts.language.clone().unwrap_or_else(|| "en".into());
    parse_normalize(&language)?;
    if !dir.is_dir() {
        return Err(format!("model folder not found: {}", dir.display()));
    }
    let cfg = read_config(dir.join("config.json")).map_err(|e| format!("config.json: {e}"))?;
    let (weights, quant): (PathBuf, Quant) = WEIGHTS
        .iter()
        .map(|&(name, q)| (dir.join(name), q))
        .find(|(p, _)| p.is_file())
        .ok_or(format!(
            "no weights in {}; expected one of {}",
            dir.display(),
            WEIGHTS.map(|(name, _)| name).join(", ")
        ))?;
    let mut builder = SynthBuilder::new(cfg, weights, Normalize::OFF)
        .tokenizer_file(dir.join("tokenizer.json"))
        .device(DeviceKind::Cpu)
        .quant(quant);
    if let Some(t) = opts.temperature {
        check_temperature(t)?;
        builder = builder.temperature(t);
    }
    if let Some(s) = opts.seed {
        builder = builder.seed(s);
    }
    if let Some(c) = opts.cfg_coef {
        builder = builder.cfg_coef(c);
    }
    if let Some(e) = opts.eos_threshold {
        builder = builder.eos_threshold(e);
    }
    if let Some(m) = opts.max_tokens_per_chunk {
        builder = builder.max_tokens_per_chunk(m);
    }
    if let Some(q) = opts.quantize_mimi {
        builder = builder.quantize_mimi(q);
    }
    for (name, value) in &opts.conditions {
        builder = builder.condition(name.clone(), condition_value(value));
    }
    for (name, path) in ptts::loader::checkpoint_voices(dir) {
        builder = builder.add_voice(name, path);
    }
    for v in &opts.voices {
        if !Path::new(&v.path).is_file() {
            return Err(format!("voice file not found: {}", v.path));
        }
        builder = builder.add_voice(v.name.clone(), v.path.clone());
    }
    if let Some(v) = &opts.voice {
        builder = builder.voice(v.clone());
    }
    let synth = builder.build().map_err(|e| e.to_string())?;
    let voices = synth.voices();
    if let Some(v) = &opts.voice {
        if !voices.contains(v) {
            return Err(format!("no voice named {v:?}"));
        }
    }
    let default_voice = opts.voice.clone().or_else(|| synth.default_voice());
    let sample_rate = synth.sample_rate();
    let languages = synth.languages().to_vec();
    let info = serde_json::json!({
        "voices": voices,
        "languages": languages,
        "language": language,
        "voice": default_voice,
        "sampleRate": sample_rate,
        "weights": if quant == Quant::Q80 { "q8_0" } else { "f32" },
        "device": synth.device_name(),
    });
    Ok(PhononModel {
        inner: Mutex::new(Inner { synth, voices }),
        info: Mutex::new(info),
        language,
        default_voice,
        sample_rate,
    })
}

/// Load the checkpoint folder `dir`. `options_json` may be NULL or empty. On failure returns
/// NULL and, when `error` is not NULL, stores a message to release with `phonon_string_free`.
///
/// # Safety
/// `dir` and `options_json` must be NULL or NUL-terminated UTF-8.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn phonon_load(
    dir: *const c_char,
    options_json: *const c_char,
    error: *mut *mut c_char,
) -> *mut PhononModel {
    let dir = unsafe { str_arg(dir) };
    let json = unsafe { str_arg(options_json) };
    match guarded(|| load(Path::new(&dir), parse(&json)?)) {
        Ok(m) => Box::into_raw(Box::new(m)),
        Err(e) => {
            set_error(error, &e);
            std::ptr::null_mut()
        }
    }
}

/// The model's facts as JSON: voices, languages, language, voice, sampleRate, weights, device.
///
/// # Safety
/// `m` must come from `phonon_load`.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn phonon_info(m: *const PhononModel) -> *mut c_char {
    let m = unsafe { &*m };
    let info = m.info.lock().map(|i| i.to_string()).unwrap_or_else(|_| "{}".into());
    c_string(&info)
}

/// The output sample rate in Hz (24000 for the published models).
///
/// # Safety
/// `m` must come from `phonon_load`.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn phonon_sample_rate(m: *const PhononModel) -> u32 {
    unsafe { (*m).sample_rate }
}

/// Speak `text`, handing mono float PCM in [-1, 1] to `cb` as it is generated, on the calling
/// thread. `cb` returns false to stop early, which is not an error. Blocks while another
/// utterance of the same model is being generated. `samples` receives the number of samples
/// delivered.
///
/// # Safety
/// `m` must come from `phonon_load`; strings must be NULL or NUL-terminated UTF-8; `cb` must
/// stay valid for the call.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn phonon_speak(
    m: *mut PhononModel,
    text: *const c_char,
    options_json: *const c_char,
    cb: PhononPcmFn,
    user: *mut c_void,
    samples: *mut u64,
    error: *mut *mut c_char,
) -> bool {
    let m = unsafe { &*m };
    let text = unsafe { str_arg(text) };
    let json = unsafe { str_arg(options_json) };
    let r = guarded(|| {
        let o: SpeakOptions = parse(&json)?;
        let normalize = parse_normalize(o.language.as_deref().unwrap_or(&m.language))?;
        if let Some(t) = o.temperature {
            check_temperature(t)?;
        }
        let inner = m.inner.lock().map_err(|_| "model lock poisoned".to_string())?;
        let voice = o.voice.or_else(|| m.default_voice.clone());
        if let Some(v) = &voice {
            if !inner.voices.contains(v) {
                return Err(format!("no voice named {v:?}"));
            }
        }
        let opts = SpeechOptions {
            voice,
            temperature: o.temperature,
            seed: o.seed,
            cfg_coef: o.cfg_coef,
            max_tokens_per_chunk: o.max_tokens_per_chunk,
            ..Default::default()
        };
        let text = normalize.apply(&text);
        let stream = inner.synth.stream_with(&text, &opts).map_err(|e| e.to_string())?;
        let mut delivered: u64 = 0;
        // Dropping the stream on an early return stops generation.
        for pcm in stream {
            let pcm = pcm.map_err(|e| e.to_string())?;
            if pcm.is_empty() {
                continue;
            }
            delivered += pcm.len() as u64;
            if !cb(pcm.as_ptr(), pcm.len(), user) {
                break;
            }
        }
        Ok(delivered)
    });
    match r {
        Ok(n) => {
            if !samples.is_null() {
                unsafe { *samples = n };
            }
            true
        }
        Err(e) => {
            set_error(error, &e);
            false
        }
    }
}

/// Add (or replace) a voice from a precomputed voice file, such as one from the Phonon voice
/// catalog or Gradium Studio. Waits for any generation of this model to finish.
///
/// # Safety
/// `m` must come from `phonon_load`; strings must be NUL-terminated UTF-8.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn phonon_add_voice(
    m: *mut PhononModel,
    name: *const c_char,
    path: *const c_char,
    error: *mut *mut c_char,
) -> bool {
    let m = unsafe { &*m };
    let name = unsafe { str_arg(name) };
    let path = unsafe { str_arg(path) };
    let r = guarded(|| {
        if name.is_empty() {
            return Err("voice name is empty".into());
        }
        if !Path::new(&path).is_file() {
            return Err(format!("voice file not found: {path}"));
        }
        let mut inner = m.inner.lock().map_err(|_| "model lock poisoned".to_string())?;
        inner.synth.add_voice_file(&name, Path::new(&path)).map_err(|e| e.to_string())?;
        inner.voices = inner.synth.voices();
        if let Ok(mut info) = m.info.lock() {
            info["voices"] = serde_json::json!(inner.voices);
        }
        Ok(())
    });
    match r {
        Ok(()) => true,
        Err(e) => {
            set_error(error, &e);
            false
        }
    }
}

/// Set how many CPU threads generation may use (0 = one per core). Applies to the next
/// generation; the thread pool used by the tokenizer is sized once, at its first use.
#[unsafe(no_mangle)]
pub extern "C" fn phonon_set_threads(n: usize) {
    let _ = catch_unwind(|| {
        if n == 0 {
            xn::set_num_threads(xn::get_num_cpus());
        } else {
            xn::set_num_threads(n);
        }
    });
}

/// How many CPU threads generation uses.
#[unsafe(no_mangle)]
pub extern "C" fn phonon_get_threads() -> usize {
    catch_unwind(xn::get_num_threads).unwrap_or(0)
}

/// The `ptts` engine version this library was built with.
#[unsafe(no_mangle)]
pub extern "C" fn phonon_engine_version() -> *const c_char {
    static VERSION: &[u8] = b"0.7.0\0";
    VERSION.as_ptr() as *const c_char
}

/// # Safety
/// `m` must be NULL or come from `phonon_load`, and must not be used afterwards.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn phonon_free(m: *mut PhononModel) {
    if !m.is_null() {
        let _ = catch_unwind(AssertUnwindSafe(|| drop(unsafe { Box::from_raw(m) })));
    }
}

/// # Safety
/// `s` must be NULL or a string this library returned, not yet released.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn phonon_string_free(s: *mut c_char) {
    if !s.is_null() {
        drop(unsafe { CString::from_raw(s) });
    }
}
