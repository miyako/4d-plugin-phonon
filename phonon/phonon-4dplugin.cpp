/*
 phonon: Phonon on-device text-to-speech for 4D.

 Models are loaded once and referenced by a Longint. Each speech runs on two threads of its
 own: one generates audio with the model, the other plays it as it arrives. Plugin commands
 only touch shared state under a lock and never call the 4D runtime from those threads.
*/

#include "phonon-4dplugin.h"
#include "phonon-audio.h"
#include "phonon-core.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <list>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#if defined(__APPLE__)
#include <CoreFoundation/CoreFoundation.h>
#include <dlfcn.h>
#elif defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

#ifndef PHONON_PLUGIN_VERSION
#define PHONON_PLUGIN_VERSION "0.0.0"
#endif

typedef std::u16string ustring;

// ---------------------------------------------------------------------------------------
// Text conversion (UTF-16 <-> UTF-8), independent of the platform's wchar_t.

static std::string to_utf8(const PA_Unichar *s, size_t len) {
    std::string out;
    out.reserve(len);
    for (size_t i = 0; i < len; ++i) {
        uint32_t c = s[i];
        if (c >= 0xD800 && c <= 0xDBFF && i + 1 < len && s[i + 1] >= 0xDC00 && s[i + 1] <= 0xDFFF) {
            c = 0x10000 + ((c - 0xD800) << 10) + (s[i + 1] - 0xDC00);
            ++i;
        } else if (c >= 0xD800 && c <= 0xDFFF) {
            c = 0xFFFD;
        }
        if (c < 0x80) {
            out += char(c);
        } else if (c < 0x800) {
            out += char(0xC0 | (c >> 6));
            out += char(0x80 | (c & 0x3F));
        } else if (c < 0x10000) {
            out += char(0xE0 | (c >> 12));
            out += char(0x80 | ((c >> 6) & 0x3F));
            out += char(0x80 | (c & 0x3F));
        } else {
            out += char(0xF0 | (c >> 18));
            out += char(0x80 | ((c >> 12) & 0x3F));
            out += char(0x80 | ((c >> 6) & 0x3F));
            out += char(0x80 | (c & 0x3F));
        }
    }
    return out;
}

static ustring to_utf16(const std::string &s) {
    ustring out;
    out.reserve(s.size());
    const unsigned char *p = (const unsigned char *)s.data();
    size_t n = s.size(), i = 0;
    while (i < n) {
        uint32_t c = p[i];
        size_t extra = 0;
        if (c < 0x80) extra = 0;
        else if ((c & 0xE0) == 0xC0) { c &= 0x1F; extra = 1; }
        else if ((c & 0xF0) == 0xE0) { c &= 0x0F; extra = 2; }
        else if ((c & 0xF8) == 0xF0) { c &= 0x07; extra = 3; }
        else { out += char16_t(0xFFFD); ++i; continue; }
        if (i + extra >= n + (extra ? 0 : 1) && extra) { out += char16_t(0xFFFD); break; }
        bool ok = true;
        for (size_t k = 1; k <= extra; ++k) {
            if (i + k >= n || (p[i + k] & 0xC0) != 0x80) { ok = false; break; }
            c = (c << 6) | (p[i + k] & 0x3F);
        }
        if (!ok) { out += char16_t(0xFFFD); ++i; continue; }
        i += extra + 1;
        if (c >= 0x10000) {
            c -= 0x10000;
            out += char16_t(0xD800 + (c >> 10));
            out += char16_t(0xDC00 + (c & 0x3FF));
        } else {
            out += char16_t(c);
        }
    }
    return out;
}

static std::string unistring_utf8(PA_Unistring *u) {
    if (!u || !u->fString) return std::string();
    return to_utf8(u->fString, size_t(u->fLength));
}

// ---------------------------------------------------------------------------------------
// 4D object and collection helpers. Values read with PA_GetObjectProperty are borrowed;
// values stored with PA_SetObjectProperty / PA_SetCollectionElement are cleared afterwards,
// releasing the plugin's own reference.

static PA_Unistring make_key(const char *key) {
    ustring k = to_utf16(key);
    return PA_CreateUnistring((PA_Unichar *)k.c_str());
}

static void ob_set_var(PA_ObjectRef o, const char *key, PA_Variable *v) {
    PA_Unistring k = make_key(key);
    PA_SetObjectProperty(o, &k, *v);
    PA_DisposeUnistring(&k);
    PA_ClearVariable(v);
}

static PA_Variable string_variable(const std::string &s) {
    ustring u = to_utf16(s);
    PA_Variable v = PA_CreateVariable(eVK_Undefined);
    PA_Unistring us = PA_CreateUnistring((PA_Unichar *)u.c_str());
    PA_SetStringVariable(&v, &us);
    return v;
}

static void ob_set_s(PA_ObjectRef o, const char *key, const std::string &value) {
    PA_Variable v = string_variable(value);
    ob_set_var(o, key, &v);
}

static void ob_set_n(PA_ObjectRef o, const char *key, double value) {
    PA_Variable v = PA_CreateVariable(eVK_Real);
    PA_SetRealVariable(&v, value);
    ob_set_var(o, key, &v);
}

static void ob_set_b(PA_ObjectRef o, const char *key, bool value) {
    PA_Variable v = PA_CreateVariable(eVK_Boolean);
    PA_SetBooleanVariable(&v, value ? 1 : 0);
    ob_set_var(o, key, &v);
}

static void ob_set_c(PA_ObjectRef o, const char *key, PA_CollectionRef value) {
    PA_Variable v = PA_CreateVariable(eVK_Collection);
    PA_SetCollectionVariable(&v, value);
    ob_set_var(o, key, &v);
}

static bool ob_get_var(PA_ObjectRef o, const char *key, PA_Variable &out) {
    if (!o) return false;
    PA_Unistring k = make_key(key);
    bool found = PA_HasObjectProperty(o, &k);
    if (found) out = PA_GetObjectProperty(o, &k);
    PA_DisposeUnistring(&k);
    return found;
}

static bool ob_get_s(PA_ObjectRef o, const char *key, std::string &out) {
    PA_Variable v;
    if (!ob_get_var(o, key, v) || PA_GetVariableKind(v) != eVK_Unistring) return false;
    PA_Unistring u = PA_GetStringVariable(v);
    out = unistring_utf8(&u);
    return true;
}

static bool ob_get_n(PA_ObjectRef o, const char *key, double &out) {
    PA_Variable v;
    if (!ob_get_var(o, key, v)) return false;
    switch (PA_GetVariableKind(v)) {
        case eVK_Real: out = PA_GetRealVariable(v); return true;
        case eVK_Longint: out = PA_GetLongintVariable(v); return true;
        default: return false;
    }
}

static bool ob_get_b(PA_ObjectRef o, const char *key, bool &out) {
    PA_Variable v;
    if (!ob_get_var(o, key, v) || PA_GetVariableKind(v) != eVK_Boolean) return false;
    out = PA_GetBooleanVariable(v) != 0;
    return true;
}

static void c_append(PA_CollectionRef c, PA_Variable *v) {
    PA_SetCollectionElement(c, PA_GetCollectionLength(c), *v);
    PA_ClearVariable(v);
}

static void c_append_n(PA_CollectionRef c, double value) {
    PA_Variable v = PA_CreateVariable(eVK_Real);
    PA_SetRealVariable(&v, value);
    c_append(c, &v);
}

static void c_append_o(PA_CollectionRef c, PA_ObjectRef o) {
    PA_Variable v = PA_CreateVariable(eVK_Object);
    PA_SetObjectVariable(&v, o);
    c_append(c, &v);
}

// JSON Stringify (command 1217) on an object the caller keeps.
static std::string ob_to_json(PA_ObjectRef o) {
    if (!o) return std::string();
    PA_Variable p[1];
    std::memset(p, 0, sizeof(p));
    PA_SetObjectVariable(&p[0], o);
    PA_Variable r = PA_ExecuteCommandByID(1217, p, 1);
    std::string json;
    if (PA_GetVariableKind(r) == eVK_Unistring) {
        PA_Unistring u = PA_GetStringVariable(r);
        json = unistring_utf8(&u);
    }
    PA_ClearVariable(&r);
    return json;
}

// JSON Parse (command 1218) into a new object, with zeroed parameters: PA_JsonParse does not
// zero them for eVK_Object in every SDK revision. The runtime takes the string.
static PA_ObjectRef json_to_ob(const std::string &json) {
    ustring u = to_utf16(json);
    PA_Unistring us = PA_CreateUnistring((PA_Unichar *)u.c_str());
    PA_Variable p[2];
    std::memset(p, 0, sizeof(p));
    PA_SetStringVariable(&p[0], &us);
    PA_SetLongintVariable(&p[1], eVK_Object);
    PA_Variable r = PA_ExecuteCommandByID(1218, p, 2);
    PA_ObjectRef o = PA_GetObjectVariable(r);
    return o ? o : PA_CreateObject();
}

static void return_text(PA_PluginParameters params, const std::string &s) {
    ustring u = to_utf16(s);
    PA_ReturnString(params, (PA_Unichar *)u.c_str());
}

// ---------------------------------------------------------------------------------------
// Paths: 4D may hand over a POSIX path or, on macOS, an HFS path ("Macintosh HD:Users:...").

static std::string native_path(const std::string &path) {
#if defined(__APPLE__)
    if (!path.empty() && path[0] != '/' && path.find(':') != std::string::npos) {
        std::string result = path;
        CFStringRef s = CFStringCreateWithCString(kCFAllocatorDefault, path.c_str(), kCFStringEncodingUTF8);
        if (s) {
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
            CFURLRef url = CFURLCreateWithFileSystemPath(kCFAllocatorDefault, s, kCFURLHFSPathStyle, false);
#pragma clang diagnostic pop
            if (url) {
                CFStringRef posix = CFURLCopyFileSystemPath(url, kCFURLPOSIXPathStyle);
                if (posix) {
                    char buf[4096];
                    if (CFStringGetCString(posix, buf, sizeof(buf), kCFStringEncodingUTF8)) result = buf;
                    CFRelease(posix);
                }
                CFRelease(url);
            }
            CFRelease(s);
        }
        return result;
    }
#endif
    return path;
}

static FILE *open_write(const std::string &path) {
#if defined(_WIN32)
    int n = MultiByteToWideChar(CP_UTF8, 0, path.c_str(), -1, nullptr, 0);
    if (n <= 0) return nullptr;
    std::wstring w(size_t(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, path.c_str(), -1, &w[0], n);
    return _wfopen(w.c_str(), L"wb");
#else
    return std::fopen(path.c_str(), "wb");
#endif
}

// ---------------------------------------------------------------------------------------
// Audio helpers.

static int16_t to_int16(float s, double volume) {
    float v = float(s * volume);
    if (!(v == v)) v = 0.0f;  // NaN
    if (v > 1.0f) v = 1.0f;
    if (v < -1.0f) v = -1.0f;
    return int16_t(std::lround(v * 32767.0f));
}

static void put_u32(std::vector<uint8_t> &b, uint32_t v) {
    for (int i = 0; i < 4; ++i) b.push_back(uint8_t(v >> (8 * i)));
}

static void put_u16(std::vector<uint8_t> &b, uint16_t v) {
    b.push_back(uint8_t(v));
    b.push_back(uint8_t(v >> 8));
}

// 16-bit PCM mono WAV.
static std::vector<uint8_t> make_wav(const std::vector<float> &samples, uint32_t rate, double volume) {
    std::vector<uint8_t> b;
    uint32_t data = uint32_t(samples.size() * 2);
    b.reserve(44 + data);
    b.insert(b.end(), {'R', 'I', 'F', 'F'});
    put_u32(b, 36 + data);
    b.insert(b.end(), {'W', 'A', 'V', 'E', 'f', 'm', 't', ' '});
    put_u32(b, 16);
    put_u16(b, 1);  // PCM
    put_u16(b, 1);  // mono
    put_u32(b, rate);
    put_u32(b, rate * 2);
    put_u16(b, 2);
    put_u16(b, 16);
    b.insert(b.end(), {'d', 'a', 't', 'a'});
    put_u32(b, data);
    for (float s : samples) {
        uint16_t v = uint16_t(to_int16(s, volume));
        put_u16(b, v);
    }
    return b;
}

static bool write_file(const std::string &path, const std::vector<uint8_t> &bytes, std::string &error) {
    FILE *f = open_write(path);
    if (!f) {
        error = "cannot open file for writing: " + path;
        return false;
    }
    bool ok = std::fwrite(bytes.data(), 1, bytes.size(), f) == bytes.size();
    ok = (std::fclose(f) == 0) && ok;
    if (!ok) error = "cannot write file: " + path;
    return ok;
}

static std::string take_error(char *e) {
    std::string s = e ? e : "unknown error";
    if (e) phonon_string_free(e);
    return s;
}

// ---------------------------------------------------------------------------------------
// Registry.

struct Model {
    PA_long32 ref = 0;
    std::string path;
    PhononModel *handle = nullptr;
    uint32_t rate = 24000;
    ~Model() {
        if (handle) phonon_free(handle);
    }
};

struct Speech {
    PA_long32 ref = 0;
    PA_long32 afterRef = 0;
    std::shared_ptr<Model> model;
    std::weak_ptr<Speech> after;
    std::string text, speakJson, output = "default", file;
    double prebuffer = 0.2;
    std::atomic<double> volume{1.0};

    std::mutex m;
    std::condition_variable cv;
    std::vector<float> samples;  // everything generated so far
    size_t readPos = 0;          // next sample to play
    uint64_t generated = 0;
    bool generationDone = false;
    bool started = false;  // first audio handed to the output
    bool paused = false;
    bool finished = false;
    bool failed = false;
    std::string error;
    double ttfaMs = -1;
    std::chrono::steady_clock::time_point created = std::chrono::steady_clock::now();
    std::unique_ptr<AudioOut> out;

    std::atomic<bool> stop{false};
    std::atomic<bool> removed{false};
    std::atomic<int> running{0};
    std::thread generator, player;

    bool terminal() {
        std::lock_guard<std::mutex> lock(m);
        return finished;
    }
};

static std::mutex gMutex;
static std::map<PA_long32, std::shared_ptr<Model>> gModels;
static std::map<PA_long32, std::shared_ptr<Speech>> gSpeeches;
static std::list<std::shared_ptr<Speech>> gGraveyard;  // stopped, threads still finishing
static PA_long32 gNextModel = 1;
static PA_long32 gNextSpeech = 1;

static std::mutex gErrorMutex;
static std::map<PA_long32, std::string> gErrors;  // by 4D process number

static void set_error(const std::string &e) {
    PA_long32 p = PA_GetCurrentProcessNumber();
    std::lock_guard<std::mutex> lock(gErrorMutex);
    if (e.empty()) gErrors.erase(p);
    else gErrors[p] = e;
}

static void clear_error() { set_error(std::string()); }

static std::shared_ptr<Model> find_model(PA_long32 ref) {
    std::lock_guard<std::mutex> lock(gMutex);
    auto it = gModels.find(ref);
    return it == gModels.end() ? nullptr : it->second;
}

static std::shared_ptr<Speech> find_speech(PA_long32 ref) {
    std::lock_guard<std::mutex> lock(gMutex);
    auto it = gSpeeches.find(ref);
    return it == gSpeeches.end() ? nullptr : it->second;
}

// Join the threads of stopped speeches that have finished. Called under gMutex.
static void reap_locked() {
    for (auto it = gGraveyard.begin(); it != gGraveyard.end();) {
        if ((*it)->running.load() == 0) {
            if ((*it)->generator.joinable()) (*it)->generator.join();
            if ((*it)->player.joinable()) (*it)->player.join();
            it = gGraveyard.erase(it);
        } else {
            ++it;
        }
    }
}

static void signal_stop(const std::shared_ptr<Speech> &s) {
    s->stop = true;
    s->removed = true;
    std::lock_guard<std::mutex> lock(s->m);
    if (s->out) s->out->abort();
    s->cv.notify_all();
}

// ---------------------------------------------------------------------------------------
// Speech threads.

static bool on_pcm(const float *pcm, size_t n, void *user) {
    Speech *s = static_cast<Speech *>(user);
    if (s->stop) return false;
    std::lock_guard<std::mutex> lock(s->m);
    if (s->ttfaMs < 0) {
        s->ttfaMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - s->created).count();
    }
    s->samples.insert(s->samples.end(), pcm, pcm + n);
    s->generated += n;
    s->cv.notify_all();
    return !s->stop;
}

static void generate(std::shared_ptr<Speech> s) {
    uint64_t n = 0;
    char *err = nullptr;
    bool ok = phonon_speak(s->model->handle, s->text.c_str(), s->speakJson.c_str(), on_pcm, s.get(), &n, &err);
    std::string error = ok ? std::string() : take_error(err);
    if (ok && !s->stop && !s->file.empty()) {
        std::vector<float> copy;
        {
            std::lock_guard<std::mutex> lock(s->m);
            copy = s->samples;
        }
        write_file(s->file, make_wav(copy, s->model->rate, s->volume.load()), error);
    }
    {
        std::lock_guard<std::mutex> lock(s->m);
        s->generationDone = true;
        if (!error.empty() && !s->stop) {
            s->failed = true;
            s->error = error;
        }
        s->cv.notify_all();
    }
    s->running--;
}

static void play(std::shared_ptr<Speech> s) {
    // Wait for the speech this one follows.
    while (!s->stop) {
        std::shared_ptr<Speech> before = s->after.lock();
        if (!before || before->removed || before->terminal()) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }

    std::unique_ptr<AudioOut> out = audio_create(s->output);
    std::string error;
    bool ok = !s->stop && out->open(s->model->rate, error);
    {
        std::lock_guard<std::mutex> lock(s->m);
        if (ok) {
            if (s->paused) out->pause();
            s->out = std::move(out);
        } else if (!s->stop) {
            s->failed = true;
            s->error = error;
        }
    }
    if (!ok) s->stop = true;

    const size_t prebuffer = size_t(std::max(0.0, s->prebuffer) * s->model->rate);
    const size_t chunk = s->model->rate / 10;
    std::vector<int16_t> pcm;
    while (ok && !s->stop) {
        {
            std::unique_lock<std::mutex> lock(s->m);
            s->cv.wait(lock, [&] {
                size_t avail = s->samples.size() - s->readPos;
                if (s->stop || s->generationDone) return true;
                return s->started ? avail > 0 : avail >= prebuffer;
            });
            if (s->stop) break;
            size_t avail = s->samples.size() - s->readPos;
            if (avail == 0) break;  // generation done and everything queued
            size_t count = std::min(avail, chunk);
            double volume = s->volume.load();
            pcm.resize(count);
            for (size_t i = 0; i < count; ++i) pcm[i] = to_int16(s->samples[s->readPos + i], volume);
            s->readPos += count;
            s->started = true;
        }
        if (!s->out->write(pcm.data(), pcm.size(), error)) {
            if (!s->stop) {
                std::lock_guard<std::mutex> lock(s->m);
                s->failed = true;
                s->error = error;
            }
            s->stop = true;
            break;
        }
    }
    if (ok && !s->stop) s->out->drain();

    {
        std::unique_lock<std::mutex> lock(s->m);
        // Generation may still be running after a playback error or a stop: end it, and wait
        // for it to report how it ended, so that an error is not lost.
        if (s->failed) s->stop = true;
        s->cv.wait(lock, [&] { return s->generationDone; });
        s->finished = true;
        s->samples.clear();
        s->samples.shrink_to_fit();
        s->out.reset();
    }
    s->running--;
}

// ---------------------------------------------------------------------------------------
// Status objects.

static PA_ObjectRef status_object(const std::shared_ptr<Speech> &s) {
    PA_ObjectRef o = PA_CreateObject();
    std::string state, error;
    double position = 0, duration = 0, ttfa = -1;
    bool complete = false;
    {
        std::lock_guard<std::mutex> lock(s->m);
        uint64_t played = s->out ? s->out->played() : 0;
        if (s->finished && !s->failed) played = s->generated;
        double rate = double(s->model->rate);
        position = double(std::min<uint64_t>(played, s->generated)) / rate;
        duration = double(s->generated) / rate;
        complete = s->generationDone;
        ttfa = s->ttfaMs;
        error = s->error;
        if (s->failed) state = "error";
        else if (s->finished) state = "finished";
        else if (s->paused) state = "paused";
        else if (s->started) state = "playing";
        else state = "pending";
    }
    ob_set_n(o, "ref", s->ref);
    ob_set_n(o, "model", s->model->ref);
    ob_set_s(o, "state", state);
    ob_set_n(o, "position", position);
    ob_set_n(o, "duration", duration);
    ob_set_b(o, "complete", complete);
    ob_set_n(o, "volume", s->volume.load());
    ob_set_s(o, "output", s->output);
    if (s->afterRef) ob_set_n(o, "after", s->afterRef);
    if (ttfa >= 0) ob_set_n(o, "ttfa", ttfa);
    if (!s->file.empty()) ob_set_s(o, "file", s->file);
    if (!error.empty()) ob_set_s(o, "error", error);
    return o;
}

static PA_ObjectRef model_object(const std::shared_ptr<Model> &m) {
    char *json = phonon_info(m->handle);
    PA_ObjectRef o = json_to_ob(json ? json : "{}");
    if (json) phonon_string_free(json);
    ob_set_n(o, "ref", m->ref);
    ob_set_s(o, "path", m->path);
    return o;
}

// ---------------------------------------------------------------------------------------
// Loading.

struct LoadRequest {
    std::string path, json;
    std::shared_ptr<Model> model;
    std::string error;
};

static void pin_module();

static void load_one(LoadRequest &r) {
    if (r.path.empty()) {
        r.error = "model path is empty";
        return;
    }
    pin_module();
    char *err = nullptr;
    PhononModel *h = phonon_load(r.path.c_str(), r.json.c_str(), &err);
    if (!h) {
        r.error = take_error(err);
        return;
    }
    auto m = std::make_shared<Model>();
    m->handle = h;
    m->path = r.path;
    m->rate = phonon_sample_rate(h);
    if (!m->rate) m->rate = 24000;
    r.model = m;
}

static PA_long32 register_model(const std::shared_ptr<Model> &m) {
    std::lock_guard<std::mutex> lock(gMutex);
    m->ref = gNextModel++;
    gModels[m->ref] = m;
    return m->ref;
}

static void Phonon_Load_model(PA_PluginParameters params) {
    LoadRequest r;
    r.path = native_path(unistring_utf8(PA_GetStringParameter(params, 1)));
    r.json = ob_to_json(PA_GetObjectParameter(params, 2));
    load_one(r);
    PA_long32 ref = 0;
    if (r.model) {
        ref = register_model(r.model);
        clear_error();
    } else {
        set_error(r.error);
    }
    PA_ReturnLong(params, ref);
}

static void Phonon_Load_models(PA_PluginParameters params) {
    PA_CollectionRef in = PA_GetCollectionParameter(params, 1);
    std::vector<LoadRequest> requests;
    if (in) {
        PA_long32 n = PA_GetCollectionLength(in);
        for (PA_long32 i = 0; i < n; ++i) {
            LoadRequest r;
            PA_Variable v = PA_GetCollectionElement(in, i);
            if (PA_GetVariableKind(v) == eVK_Unistring) {
                PA_Unistring u = PA_GetStringVariable(v);
                r.path = unistring_utf8(&u);
            } else if (PA_GetVariableKind(v) == eVK_Object) {
                PA_ObjectRef o = PA_GetObjectVariable(v);
                ob_get_s(o, "path", r.path);
                r.json = ob_to_json(o);
            }
            r.path = native_path(r.path);
            requests.push_back(r);
        }
    }
    // Load in parallel: each model loads independently.
    std::vector<std::thread> threads;
    for (auto &r : requests) threads.emplace_back(load_one, std::ref(r));
    for (auto &t : threads) t.join();

    PA_CollectionRef out = PA_CreateCollection();
    std::string errors;
    for (size_t i = 0; i < requests.size(); ++i) {
        PA_long32 ref = 0;
        if (requests[i].model) {
            ref = register_model(requests[i].model);
        } else {
            if (!errors.empty()) errors += "\n";
            errors += "#" + std::to_string(i) + ": " + requests[i].error;
        }
        c_append_n(out, ref);
    }
    set_error(errors);
    PA_ReturnCollection(params, out);
}

static void Phonon_UNLOAD_MODEL(PA_PluginParameters params) {
    PA_long32 ref = PA_GetLongParameter(params, 1);
    std::shared_ptr<Model> m;
    {
        std::lock_guard<std::mutex> lock(gMutex);
        auto it = gModels.find(ref);
        if (it != gModels.end()) {
            m = it->second;
            gModels.erase(it);
        }
    }
    // Speeches using the model keep it alive until they end.
    if (m) clear_error();
    else set_error("no model with ref " + std::to_string(ref));
}

static void Phonon_UNLOAD_ALL(PA_PluginParameters params) {
    std::lock_guard<std::mutex> lock(gMutex);
    gModels.clear();
    clear_error();
}

static void Phonon_Get_models(PA_PluginParameters params) {
    std::vector<std::shared_ptr<Model>> models;
    {
        std::lock_guard<std::mutex> lock(gMutex);
        for (auto &kv : gModels) models.push_back(kv.second);
    }
    PA_CollectionRef out = PA_CreateCollection();
    for (auto &m : models) {
        PA_ObjectRef o = model_object(m);
        c_append_o(out, o);
    }
    PA_ReturnCollection(params, out);
}

static void Phonon_Get_model_info(PA_PluginParameters params) {
    PA_long32 ref = PA_GetLongParameter(params, 1);
    std::shared_ptr<Model> m = find_model(ref);
    if (!m) {
        set_error("no model with ref " + std::to_string(ref));
        PA_ReturnObject(params, PA_CreateObject());
        return;
    }
    clear_error();
    PA_ReturnObject(params, model_object(m));
}

static void Phonon_ADD_VOICE(PA_PluginParameters params) {
    PA_long32 ref = PA_GetLongParameter(params, 1);
    std::string name = unistring_utf8(PA_GetStringParameter(params, 2));
    std::string path = native_path(unistring_utf8(PA_GetStringParameter(params, 3)));
    std::shared_ptr<Model> m = find_model(ref);
    if (!m) {
        set_error("no model with ref " + std::to_string(ref));
        return;
    }
    char *err = nullptr;
    if (phonon_add_voice(m->handle, name.c_str(), path.c_str(), &err)) clear_error();
    else set_error(take_error(err));
}

// ---------------------------------------------------------------------------------------
// Playing.

// Options read here; the rest of the object goes to the engine as JSON.
static bool start_speech(PA_long32 modelRef, const std::string &text, PA_ObjectRef options,
                         PA_long32 defaultAfter, PA_long32 &ref, std::string &error) {
    ref = 0;
    std::shared_ptr<Model> m = find_model(modelRef);
    if (!m) {
        error = "no model with ref " + std::to_string(modelRef);
        return false;
    }
    auto s = std::make_shared<Speech>();
    s->model = m;
    s->text = text;
    s->speakJson = ob_to_json(options);
    double d;
    bool b;
    std::string str;
    if (ob_get_n(options, "volume", d)) {
        if (!(d >= 0 && d <= 10)) {
            error = "volume must be between 0 and 10";
            return false;
        }
        s->volume = d;
    }
    if (ob_get_s(options, "output", str)) {
        if (str != "default" && str != "none") {
            error = "output must be \"default\" or \"none\"";
            return false;
        }
        s->output = str;
    }
    if (ob_get_n(options, "prebuffer", d)) {
        if (!(d >= 0 && d <= 60)) {
            error = "prebuffer must be between 0 and 60 seconds";
            return false;
        }
        s->prebuffer = d;
    }
    if (ob_get_b(options, "paused", b)) s->paused = b;
    if (ob_get_s(options, "file", str)) s->file = native_path(str);
    s->afterRef = defaultAfter;
    if (ob_get_n(options, "after", d)) s->afterRef = PA_long32(d);

    std::lock_guard<std::mutex> lock(gMutex);
    reap_locked();
    if (s->afterRef) {
        auto it = gSpeeches.find(s->afterRef);
        if (it != gSpeeches.end()) s->after = it->second;
    }
    s->ref = gNextSpeech++;
    s->running = 2;
    try {
        s->generator = std::thread(generate, s);
    } catch (...) {
        error = "cannot start a thread";
        return false;
    }
    try {
        s->player = std::thread(play, s);
    } catch (...) {
        s->running--;
        signal_stop(s);
        gGraveyard.push_back(s);
        error = "cannot start a thread";
        return false;
    }
    gSpeeches[s->ref] = s;
    ref = s->ref;
    return true;
}

static void Phonon_Play(PA_PluginParameters params) {
    PA_long32 model = PA_GetLongParameter(params, 1);
    std::string text = unistring_utf8(PA_GetStringParameter(params, 2));
    PA_ObjectRef options = PA_GetObjectParameter(params, 3);
    PA_long32 ref = 0;
    std::string error;
    if (start_speech(model, text, options, 0, ref, error)) clear_error();
    else set_error(error);
    PA_ReturnLong(params, ref);
}

static void Phonon_Play_multiple(PA_PluginParameters params) {
    PA_CollectionRef in = PA_GetCollectionParameter(params, 1);
    PA_ObjectRef common = PA_GetObjectParameter(params, 2);
    double d;
    bool sequential = false;
    PA_long32 defaultModel = 0;
    if (ob_get_n(common, "model", d)) defaultModel = PA_long32(d);
    ob_get_b(common, "sequential", sequential);

    PA_CollectionRef out = PA_CreateCollection();
    std::string errors;
    PA_long32 previous = 0;
    PA_long32 n = in ? PA_GetCollectionLength(in) : 0;
    for (PA_long32 i = 0; i < n; ++i) {
        PA_Variable v = PA_GetCollectionElement(in, i);
        std::string text;
        PA_long32 model = defaultModel;
        PA_ObjectRef options = common;
        PA_ObjectRef merged = nullptr;
        if (PA_GetVariableKind(v) == eVK_Unistring) {
            PA_Unistring u = PA_GetStringVariable(v);
            text = unistring_utf8(&u);
        } else if (PA_GetVariableKind(v) == eVK_Object) {
            PA_ObjectRef item = PA_GetObjectVariable(v);
            ob_get_s(item, "text", text);
            if (ob_get_n(item, "model", d)) model = PA_long32(d);
            // The item's own settings win over the shared ones.
            std::string a = ob_to_json(common), b = ob_to_json(item);
            if (a.size() > 2 && b.size() > 2) merged = json_to_ob(a.substr(0, a.size() - 1) + "," + b.substr(1));
            else if (b.size() > 2) merged = json_to_ob(b);
            if (merged) options = merged;
        }
        PA_long32 ref = 0;
        std::string error;
        if (!start_speech(model, text, options, sequential ? previous : 0, ref, error)) {
            if (!errors.empty()) errors += "\n";
            errors += "#" + std::to_string(i) + ": " + error;
        } else {
            previous = ref;
        }
        if (merged) PA_DisposeObject(merged);
        c_append_n(out, ref);
    }
    set_error(errors);
    PA_ReturnCollection(params, out);
}

static void set_paused(const std::shared_ptr<Speech> &s, bool paused) {
    std::lock_guard<std::mutex> lock(s->m);
    if (s->finished || s->paused == paused) return;
    s->paused = paused;
    if (s->out) {
        if (paused) s->out->pause();
        else s->out->resume();
    }
}

static void Phonon_PAUSE(PA_PluginParameters params) {
    PA_long32 ref = PA_GetLongParameter(params, 1);
    auto s = find_speech(ref);
    if (!s) return set_error("no speech with ref " + std::to_string(ref));
    set_paused(s, true);
    clear_error();
}

static void Phonon_RESUME(PA_PluginParameters params) {
    PA_long32 ref = PA_GetLongParameter(params, 1);
    auto s = find_speech(ref);
    if (!s) return set_error("no speech with ref " + std::to_string(ref));
    set_paused(s, false);
    clear_error();
}

static void all_speeches(std::vector<std::shared_ptr<Speech>> &out) {
    std::lock_guard<std::mutex> lock(gMutex);
    for (auto &kv : gSpeeches) out.push_back(kv.second);
}

static void Phonon_PAUSE_ALL(PA_PluginParameters params) {
    std::vector<std::shared_ptr<Speech>> list;
    all_speeches(list);
    for (auto &s : list) set_paused(s, true);
    clear_error();
}

static void Phonon_RESUME_ALL(PA_PluginParameters params) {
    std::vector<std::shared_ptr<Speech>> list;
    all_speeches(list);
    for (auto &s : list) set_paused(s, false);
    clear_error();
}

static void Phonon_STOP(PA_PluginParameters params) {
    PA_long32 ref = PA_GetLongParameter(params, 1);
    std::lock_guard<std::mutex> lock(gMutex);
    auto it = gSpeeches.find(ref);
    if (it == gSpeeches.end()) {
        set_error("no speech with ref " + std::to_string(ref));
        return;
    }
    auto s = it->second;
    gSpeeches.erase(it);
    signal_stop(s);
    gGraveyard.push_back(s);
    reap_locked();
    clear_error();
}

static void Phonon_STOP_ALL(PA_PluginParameters params) {
    std::lock_guard<std::mutex> lock(gMutex);
    for (auto &kv : gSpeeches) {
        signal_stop(kv.second);
        gGraveyard.push_back(kv.second);
    }
    gSpeeches.clear();
    reap_locked();
    clear_error();
}

static void Phonon_Get_status(PA_PluginParameters params) {
    PA_long32 ref = PA_GetLongParameter(params, 1);
    auto s = find_speech(ref);
    if (!s) {
        set_error("no speech with ref " + std::to_string(ref));
        PA_ObjectRef o = PA_CreateObject();
        ob_set_n(o, "ref", ref);
        ob_set_s(o, "state", "unknown");
        PA_ReturnObject(params, o);
        return;
    }
    clear_error();
    PA_ReturnObject(params, status_object(s));
}

static void Phonon_Get_statuses(PA_PluginParameters params) {
    PA_CollectionRef in = PA_GetCollectionParameter(params, 1);
    std::vector<std::shared_ptr<Speech>> list;
    if (in) {
        PA_long32 n = PA_GetCollectionLength(in);
        for (PA_long32 i = 0; i < n; ++i) {
            PA_Variable v = PA_GetCollectionElement(in, i);
            PA_long32 ref = 0;
            if (PA_GetVariableKind(v) == eVK_Real) ref = PA_long32(PA_GetRealVariable(v));
            else if (PA_GetVariableKind(v) == eVK_Longint) ref = PA_GetLongintVariable(v);
            auto s = find_speech(ref);
            if (s) list.push_back(s);
        }
    } else {
        all_speeches(list);
    }
    PA_CollectionRef out = PA_CreateCollection();
    for (auto &s : list) c_append_o(out, status_object(s));
    clear_error();
    PA_ReturnCollection(params, out);
}

static void Phonon_SET_VOLUME(PA_PluginParameters params) {
    PA_long32 ref = PA_GetLongParameter(params, 1);
    double volume = PA_GetDoubleParameter(params, 2);
    auto s = find_speech(ref);
    if (!s) return set_error("no speech with ref " + std::to_string(ref));
    if (!(volume >= 0 && volume <= 10)) return set_error("volume must be between 0 and 10");
    s->volume = volume;
    clear_error();
}

// ---------------------------------------------------------------------------------------
// Synthesis to a WAV BLOB, on the calling thread.

static bool collect_pcm(const float *pcm, size_t n, void *user) {
    static_cast<std::vector<float> *>(user)->insert(static_cast<std::vector<float> *>(user)->end(), pcm, pcm + n);
    return true;
}

static void Phonon_Synthesize(PA_PluginParameters params) {
    PA_long32 ref = PA_GetLongParameter(params, 1);
    std::string text = unistring_utf8(PA_GetStringParameter(params, 2));
    PA_ObjectRef options = PA_GetObjectParameter(params, 3);
    std::shared_ptr<Model> m = find_model(ref);
    if (!m) {
        set_error("no model with ref " + std::to_string(ref));
        PA_ReturnBlob(params, nullptr, 0);
        return;
    }
    double volume = 1.0, d;
    if (ob_get_n(options, "volume", d)) {
        if (!(d >= 0 && d <= 10)) {
            set_error("volume must be between 0 and 10");
            PA_ReturnBlob(params, nullptr, 0);
            return;
        }
        volume = d;
    }
    std::vector<float> samples;
    uint64_t n = 0;
    char *err = nullptr;
    if (!phonon_speak(m->handle, text.c_str(), ob_to_json(options).c_str(), collect_pcm, &samples, &n, &err)) {
        set_error(take_error(err));
        PA_ReturnBlob(params, nullptr, 0);
        return;
    }
    std::vector<uint8_t> wav = make_wav(samples, m->rate, volume);
    if (wav.size() > 0x7FFFFFFF) {
        set_error("audio too long for a BLOB");
        PA_ReturnBlob(params, nullptr, 0);
        return;
    }
    clear_error();
    PA_ReturnBlob(params, wav.data(), PA_long32(wav.size()));
}

// ---------------------------------------------------------------------------------------
// Miscellaneous.

static void Phonon_Get_last_error(PA_PluginParameters params) {
    std::string e;
    {
        PA_long32 p = PA_GetCurrentProcessNumber();
        std::lock_guard<std::mutex> lock(gErrorMutex);
        auto it = gErrors.find(p);
        if (it != gErrors.end()) e = it->second;
    }
    return_text(params, e);
}

static void Phonon_SET_THREADS(PA_PluginParameters params) {
    PA_long32 n = PA_GetLongParameter(params, 1);
    if (n < 0 || n > 1024) return set_error("threads must be between 0 and 1024");
    phonon_set_threads(size_t(n));
    clear_error();
}

static void Phonon_Get_info(PA_PluginParameters params) {
    PA_ObjectRef o = PA_CreateObject();
    size_t models, speeches;
    {
        std::lock_guard<std::mutex> lock(gMutex);
        reap_locked();
        models = gModels.size();
        speeches = gSpeeches.size();
    }
    ob_set_s(o, "version", PHONON_PLUGIN_VERSION);
    ob_set_s(o, "engine", phonon_engine_version());
    ob_set_n(o, "threads", double(phonon_get_threads()));
    ob_set_n(o, "models", double(models));
    ob_set_n(o, "speeches", double(speeches));
    PA_ReturnObject(params, o);
}

// ---------------------------------------------------------------------------------------
// Cleanup: when 4D quits or closes the project, stop every speech, wait for its threads and
// free every model, so no thread outlives the plugin.

static void cleanup() {
    std::list<std::shared_ptr<Speech>> all;
    {
        std::lock_guard<std::mutex> lock(gMutex);
        for (auto &kv : gSpeeches) all.push_back(kv.second);
        gSpeeches.clear();
        all.splice(all.end(), gGraveyard);
    }
    for (auto &s : all) signal_stop(s);
    for (auto &s : all) {
        if (s->generator.joinable()) s->generator.join();
        if (s->player.joinable()) s->player.join();
    }
    all.clear();
    {
        std::lock_guard<std::mutex> lock(gMutex);
        gModels.clear();
    }
    std::lock_guard<std::mutex> lock(gErrorMutex);
    gErrors.clear();
}

// The engine's tensor library runs a pool of worker threads that live for the life of the
// process; they cannot be stopped. Keep the plugin's code mapped after 4D releases the
// plugin (for example when it closes the project), so those idle threads never run unmapped
// code. Everything else is released by cleanup().
static void pin_module() {
    static std::once_flag once;
    std::call_once(once, [] {
#if defined(__APPLE__)
        Dl_info info;
        if (dladdr((const void *)&PluginMain, &info) && info.dli_fname) {
            // Returns the image already loaded, now marked never to be unloaded. The extra
            // reference is kept on purpose.
            dlopen(info.dli_fname, RTLD_LAZY | RTLD_NOLOAD | RTLD_NODELETE);
        }
#elif defined(_WIN32)
        HMODULE module = nullptr;
        GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
                           (LPCWSTR)&PluginMain, &module);
#endif
    });
}

static void on_close_process() {
    PA_long32 p = PA_GetCurrentProcessNumber();
    std::lock_guard<std::mutex> lock(gErrorMutex);
    gErrors.erase(p);
}

void PluginMain(PA_long32 selector, PA_PluginParameters params) {
    try {
        switch (selector) {
            case kInitPlugin:
            case kServerInitPlugin:
                pin_module();
                break;
            case kDeinitPlugin:
            case kServerDeinitPlugin:
                cleanup();
                break;
            case kCloseProcess:
                on_close_process();
                break;
            case 1: Phonon_Load_model(params); break;
            case 2: Phonon_Load_models(params); break;
            case 3: Phonon_UNLOAD_MODEL(params); break;
            case 4: Phonon_Get_models(params); break;
            case 5: Phonon_Get_model_info(params); break;
            case 6: Phonon_ADD_VOICE(params); break;
            case 7: Phonon_Play(params); break;
            case 8: Phonon_Play_multiple(params); break;
            case 9: Phonon_PAUSE(params); break;
            case 10: Phonon_RESUME(params); break;
            case 11: Phonon_STOP(params); break;
            case 12: Phonon_Get_status(params); break;
            case 13: Phonon_Get_statuses(params); break;
            case 14: Phonon_PAUSE_ALL(params); break;
            case 15: Phonon_RESUME_ALL(params); break;
            case 16: Phonon_STOP_ALL(params); break;
            case 17: Phonon_SET_VOLUME(params); break;
            case 18: Phonon_Synthesize(params); break;
            case 19: Phonon_Get_last_error(params); break;
            case 20: Phonon_SET_THREADS(params); break;
            case 21: Phonon_Get_info(params); break;
            case 22: Phonon_UNLOAD_ALL(params); break;
        }
    } catch (...) {
        // A command that failed half way still owes 4D its result.
        try {
            set_error("internal error");
            switch (selector) {
                case 1: case 7: PA_ReturnLong(params, 0); break;
                case 2: case 4: case 8: case 13: PA_ReturnCollection(params, PA_CreateCollection()); break;
                case 5: case 12: case 21: PA_ReturnObject(params, PA_CreateObject()); break;
                case 18: PA_ReturnBlob(params, nullptr, 0); break;
                case 19: return_text(params, "internal error"); break;
            }
        } catch (...) {
        }
    }
}
