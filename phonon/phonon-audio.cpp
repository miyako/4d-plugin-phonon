#include "phonon-audio.h"

#include <chrono>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <mutex>
#include <thread>
#include <vector>

#if defined(__APPLE__)
#include <AudioToolbox/AudioToolbox.h>
#elif defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <mmsystem.h>
#endif

namespace {

// Each device buffer holds 100 ms at 24 kHz; four of them give 400 ms of queued audio.
constexpr size_t kBufferSamples = 2400;
constexpr int kBufferCount = 4;

// ---------------------------------------------------------------------------------------
// No device: consume samples at the real-time rate, honouring pause.

class NullOut : public AudioOut {
    uint32_t rate_ = 24000;
    std::mutex m_;
    std::condition_variable cv_;
    bool paused_ = false;
    bool aborted_ = false;
    std::atomic<uint64_t> played_{0};

    // Wait `samples` worth of playing time, not counting time spent paused.
    bool consume(size_t samples) {
        using clock = std::chrono::steady_clock;
        auto remaining = std::chrono::duration<double>(double(samples) / rate_);
        std::unique_lock<std::mutex> lock(m_);
        while (remaining.count() > 0) {
            if (aborted_) return false;
            if (paused_) {
                cv_.wait(lock, [this] { return aborted_ || !paused_; });
                continue;
            }
            auto start = clock::now();
            auto step = std::min(remaining, std::chrono::duration<double>(0.02));
            cv_.wait_for(lock, step, [this] { return aborted_ || paused_; });
            auto spent = std::chrono::duration<double>(clock::now() - start);
            if (spent > step) spent = step;
            remaining -= spent;
            played_ += uint64_t(spent.count() * rate_ + 0.5);
        }
        return !aborted_;
    }

public:
    bool open(uint32_t rate, std::string &) override {
        rate_ = rate ? rate : 24000;
        return true;
    }
    bool write(const int16_t *, size_t n, std::string &) override { return consume(n); }
    void drain() override {}
    void pause() override {
        std::lock_guard<std::mutex> lock(m_);
        paused_ = true;
        cv_.notify_all();
    }
    void resume() override {
        std::lock_guard<std::mutex> lock(m_);
        paused_ = false;
        cv_.notify_all();
    }
    void abort() override {
        std::lock_guard<std::mutex> lock(m_);
        aborted_ = true;
        cv_.notify_all();
    }
    uint64_t played() const override { return played_.load(); }
};

#if defined(__APPLE__)

// ---------------------------------------------------------------------------------------
// macOS: an Audio Queue on its own internal thread. The callback hands spent buffers back to
// a free list that write() refills.

class QueueOut : public AudioOut {
    AudioQueueRef queue_ = nullptr;
    std::vector<AudioQueueBufferRef> buffers_;
    std::mutex m_;
    std::condition_variable cv_;
    std::deque<AudioQueueBufferRef> free_;
    bool aborted_ = false;
    bool paused_ = false;
    bool started_ = false;
    std::atomic<uint64_t> played_{0};

    static void callback(void *user, AudioQueueRef, AudioQueueBufferRef buffer) {
        QueueOut *self = static_cast<QueueOut *>(user);
        std::lock_guard<std::mutex> lock(self->m_);
        self->played_ += buffer->mAudioDataByteSize / sizeof(int16_t);
        self->free_.push_back(buffer);
        self->cv_.notify_all();
    }

public:
    ~QueueOut() override {
        if (queue_) {
            AudioQueueStop(queue_, true);
            AudioQueueDispose(queue_, true);  // also frees the buffers
            queue_ = nullptr;
        }
    }

    bool open(uint32_t rate, std::string &error) override {
        AudioStreamBasicDescription f;
        std::memset(&f, 0, sizeof(f));
        f.mSampleRate = rate;
        f.mFormatID = kAudioFormatLinearPCM;
        f.mFormatFlags = kLinearPCMFormatFlagIsSignedInteger | kLinearPCMFormatFlagIsPacked;
        f.mBitsPerChannel = 16;
        f.mChannelsPerFrame = 1;
        f.mBytesPerFrame = 2;
        f.mFramesPerPacket = 1;
        f.mBytesPerPacket = 2;
        // A NULL run loop runs the callback on the queue's own internal thread.
        OSStatus s = AudioQueueNewOutput(&f, callback, this, nullptr, nullptr, 0, &queue_);
        if (s != noErr) {
            queue_ = nullptr;
            error = "audio output: AudioQueueNewOutput failed (" + std::to_string(int(s)) + ")";
            return false;
        }
        for (int i = 0; i < kBufferCount; ++i) {
            AudioQueueBufferRef b = nullptr;
            s = AudioQueueAllocateBuffer(queue_, kBufferSamples * sizeof(int16_t), &b);
            if (s != noErr) {
                error = "audio output: AudioQueueAllocateBuffer failed (" + std::to_string(int(s)) + ")";
                return false;
            }
            buffers_.push_back(b);
            free_.push_back(b);
        }
        return true;
    }

    bool write(const int16_t *pcm, size_t n, std::string &error) override {
        while (n > 0) {
            AudioQueueBufferRef b = nullptr;
            {
                std::unique_lock<std::mutex> lock(m_);
                cv_.wait(lock, [this] { return aborted_ || !free_.empty(); });
                if (aborted_) return false;
                b = free_.front();
                free_.pop_front();
            }
            size_t count = std::min(n, kBufferSamples);
            std::memcpy(b->mAudioData, pcm, count * sizeof(int16_t));
            b->mAudioDataByteSize = UInt32(count * sizeof(int16_t));
            OSStatus s = AudioQueueEnqueueBuffer(queue_, b, 0, nullptr);
            if (s != noErr) {
                error = "audio output: AudioQueueEnqueueBuffer failed (" + std::to_string(int(s)) + ")";
                return false;
            }
            pcm += count;
            n -= count;
            std::unique_lock<std::mutex> lock(m_);
            if (!started_ && !paused_) {
                started_ = true;
                lock.unlock();
                s = AudioQueueStart(queue_, nullptr);
                if (s != noErr) {
                    error = "audio output: AudioQueueStart failed (" + std::to_string(int(s)) + ")";
                    return false;
                }
            }
        }
        return true;
    }

    void drain() override {
        {
            std::unique_lock<std::mutex> lock(m_);
            if (!started_ && !paused_ && free_.size() < buffers_.size()) {
                started_ = true;
                lock.unlock();
                AudioQueueStart(queue_, nullptr);
                lock.lock();
            }
            cv_.wait(lock, [this] { return aborted_ || free_.size() == buffers_.size(); });
        }
        AudioQueueStop(queue_, true);
    }

    void pause() override {
        std::lock_guard<std::mutex> lock(m_);
        paused_ = true;
        if (started_) AudioQueuePause(queue_);
    }

    void resume() override {
        std::lock_guard<std::mutex> lock(m_);
        paused_ = false;
        started_ = true;
        AudioQueueStart(queue_, nullptr);
    }

    void abort() override {
        {
            std::lock_guard<std::mutex> lock(m_);
            aborted_ = true;
            cv_.notify_all();
        }
        if (queue_) AudioQueueStop(queue_, true);
    }

    uint64_t played() const override { return played_.load(); }
};

#elif defined(_WIN32)

// ---------------------------------------------------------------------------------------
// Windows: waveOut with an event callback. The player thread recycles finished headers.

class WaveOut : public AudioOut {
    HWAVEOUT wave_ = nullptr;
    HANDLE event_ = nullptr;
    WAVEHDR headers_[kBufferCount];
    std::vector<int16_t> data_[kBufferCount];
    bool queued_[kBufferCount] = {};
    std::atomic<bool> aborted_{false};
    std::atomic<uint64_t> played_{0};
    std::mutex m_;

    // Reclaim headers the device has finished with. Called with m_ held.
    void reclaim() {
        for (int i = 0; i < kBufferCount; ++i) {
            if (queued_[i] && (headers_[i].dwFlags & WHDR_DONE)) {
                waveOutUnprepareHeader(wave_, &headers_[i], sizeof(WAVEHDR));
                played_ += headers_[i].dwBufferLength / sizeof(int16_t);
                queued_[i] = false;
            }
        }
    }

    int free_slot() {
        for (int i = 0; i < kBufferCount; ++i)
            if (!queued_[i]) return i;
        return -1;
    }

    bool busy() {
        for (int i = 0; i < kBufferCount; ++i)
            if (queued_[i]) return true;
        return false;
    }

public:
    WaveOut() { std::memset(headers_, 0, sizeof(headers_)); }

    ~WaveOut() override {
        if (wave_) {
            waveOutReset(wave_);
            {
                std::lock_guard<std::mutex> lock(m_);
                for (int i = 0; i < kBufferCount; ++i) {
                    if (queued_[i]) {
                        waveOutUnprepareHeader(wave_, &headers_[i], sizeof(WAVEHDR));
                        queued_[i] = false;
                    }
                }
            }
            waveOutClose(wave_);
            wave_ = nullptr;
        }
        if (event_) CloseHandle(event_);
    }

    bool open(uint32_t rate, std::string &error) override {
        event_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (!event_) {
            error = "audio output: CreateEvent failed";
            return false;
        }
        WAVEFORMATEX f;
        std::memset(&f, 0, sizeof(f));
        f.wFormatTag = WAVE_FORMAT_PCM;
        f.nChannels = 1;
        f.nSamplesPerSec = rate;
        f.wBitsPerSample = 16;
        f.nBlockAlign = 2;
        f.nAvgBytesPerSec = rate * 2;
        MMRESULT r = waveOutOpen(&wave_, WAVE_MAPPER, &f, (DWORD_PTR)event_, 0, CALLBACK_EVENT);
        if (r != MMSYSERR_NOERROR) {
            wave_ = nullptr;
            error = "audio output: waveOutOpen failed (" + std::to_string(int(r)) + ")";
            return false;
        }
        return true;
    }

    bool write(const int16_t *pcm, size_t n, std::string &error) override {
        while (n > 0) {
            int slot = -1;
            while (!aborted_) {
                {
                    std::lock_guard<std::mutex> lock(m_);
                    reclaim();
                    slot = free_slot();
                }
                if (slot >= 0) break;
                WaitForSingleObject(event_, 50);
            }
            if (aborted_) return false;
            size_t count = std::min(n, kBufferSamples);
            std::lock_guard<std::mutex> lock(m_);
            data_[slot].assign(pcm, pcm + count);
            WAVEHDR &h = headers_[slot];
            std::memset(&h, 0, sizeof(h));
            h.lpData = reinterpret_cast<LPSTR>(data_[slot].data());
            h.dwBufferLength = DWORD(count * sizeof(int16_t));
            MMRESULT r = waveOutPrepareHeader(wave_, &h, sizeof(WAVEHDR));
            if (r == MMSYSERR_NOERROR) r = waveOutWrite(wave_, &h, sizeof(WAVEHDR));
            if (r != MMSYSERR_NOERROR) {
                error = "audio output: waveOutWrite failed (" + std::to_string(int(r)) + ")";
                return false;
            }
            queued_[slot] = true;
            pcm += count;
            n -= count;
        }
        return true;
    }

    void drain() override {
        while (!aborted_) {
            {
                std::lock_guard<std::mutex> lock(m_);
                reclaim();
                if (!busy()) return;
            }
            WaitForSingleObject(event_, 50);
        }
    }

    void pause() override {
        if (wave_) waveOutPause(wave_);
    }
    void resume() override {
        if (wave_) waveOutRestart(wave_);
    }
    void abort() override {
        aborted_ = true;
        if (wave_) waveOutReset(wave_);
        if (event_) SetEvent(event_);
    }
    uint64_t played() const override { return played_.load(); }
};

#endif

}  // namespace

std::unique_ptr<AudioOut> audio_create(const std::string &output) {
    if (output == "none") return std::unique_ptr<AudioOut>(new NullOut());
#if defined(__APPLE__)
    return std::unique_ptr<AudioOut>(new QueueOut());
#elif defined(_WIN32)
    return std::unique_ptr<AudioOut>(new WaveOut());
#else
    return std::unique_ptr<AudioOut>(new NullOut());
#endif
}
