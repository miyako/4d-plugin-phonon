#ifndef PHONON_AUDIO_H
#define PHONON_AUDIO_H

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

// One playback stream of 16-bit mono PCM. All calls come from the speech's player thread,
// except pause/resume/abort, which may come from any thread.
class AudioOut {
public:
    virtual ~AudioOut() {}
    // Open the stream at `rate` Hz. False on failure, with `error` set.
    virtual bool open(uint32_t rate, std::string &error) = 0;
    // Queue samples, blocking while all buffers are in use (for example while paused).
    // Returns false once abort() was called or on a device error (then `error` is set).
    virtual bool write(const int16_t *pcm, size_t n, std::string &error) = 0;
    // Block until everything queued has played, or abort() was called.
    virtual void drain() = 0;
    virtual void pause() = 0;
    virtual void resume() = 0;
    // Stop at once and make any blocked write()/drain() return.
    virtual void abort() = 0;
    // Samples played so far (to the granularity of one buffer on real devices).
    virtual uint64_t played() const = 0;
};

// "default": the system's default output device. "none": no device; playback is paced in
// real time against the clock, so status, pause and resume behave as with a device.
std::unique_ptr<AudioOut> audio_create(const std::string &output);

#endif
