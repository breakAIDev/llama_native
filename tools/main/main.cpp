#include "arg.h"
#include "common.h"
#include "console.h"
#include "log.h"
#include "sampling.h"
#include "llama.h"
#include "chat.h"

// ==== RAIZE bridge additions ====
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <pthread.h>
#include <deque>
#include <atomic>
#include <mutex>
#include <condition_variable>

#include <cstdio>
#include <cstring>
#include <ctime>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>
#include <thread>

#if defined (__unix__) || (defined (__APPLE__) && defined (__MACH__))
#include <signal.h>
#include <unistd.h>
#elif defined (_WIN32)
#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <signal.h>
#endif

#if defined(_MSC_VER)
#pragma warning(disable: 4244 4267) // possible loss of data
#endif

// ========================= Voice I/O (optional) =========================
#ifdef HAVE_VOICE_IO
#include <queue>
#include <cstdlib>
#include <cstdint>
#include <algorithm>
#include <filesystem>
#include <chrono>
#include <cmath>
#include <functional>

#include <portaudio.h>
#include <vosk_api.h>
#include <espeak-ng/speak_lib.h>

// ---- RAIZE: small helpers for JSON + time + current id ----
static inline std::string now_iso_utc() {
    std::time_t t = std::time(nullptr);
    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", std::gmtime(&t));
    return std::string(buf);
}

// returns a JSON string literal for 'in' (including the surrounding quotes)
static inline std::string json_escape(const std::string& in) {
    std::string out;
    out.reserve(in.size() + 2);
    out.push_back('\"');
    for (unsigned char c : in) {
        switch (c) {
            case '\"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\b': out += "\\b";  break;
            case '\f': out += "\\f";  break;
            case '\n': out += "\\n";  break;
            case '\r': out += "\\r";  break;
            case '\t': out += "\\t";  break;
            default:
                if (c < 0x20) {
                    char u[7];
                    std::snprintf(u, sizeof(u), "\\u%04x", (unsigned)c);
                    out += u;
                } else {
                    out.push_back((char)c);
                }
        }
    }
    out.push_back('\"');
    return out;
}

std::function<void(const std::string&)> on_final;
void set_on_final(std::function<void(const std::string&)> cb) { on_final = std::move(cb); }

// Single global to carry BLE message id across the turn
static thread_local std::string g_ble_current_id;

#ifndef VOSK_DEFAULT_MODEL_DIR
#define VOSK_DEFAULT_MODEL_DIR "/etc/models/vosk-model-en-us-0.22-lgraph.zip"
// #define VOSK_DEFAULT_MODEL_DIR "/etc/models/vosk-model-small-en-us-0.15.zip"
#endif

#ifndef VOSK_DEFAULT_MODEL_PATH
  #ifdef VOSK_DEFAULT_MODEL_DIR
    #define VOSK_DEFAULT_MODEL_PATH VOSK_DEFAULT_MODEL_DIR
  #else
    #define VOSK_DEFAULT_MODEL_PATH "/etc/models/"
  #endif
#endif

struct VoiceIO {
    // ---------- config (overridable via env) ----------
    bool        tts_enabled;
    bool        tts_pause_mic;  // Whether to pause capture while speaking (default on)
    bool        tts_use_aplay_fallback; // If espeak ALSA backend isn't available, optionally fallback via aplay

    std::string tts_voice; // language
    std::string tts_gender; // male or female
    int         tts_pitch;
    int         tts_wpm; // words per minutes
    int         tts_amplitude;

    int         srate;   // recognizer target rate
    int         channels;       // input channels (Vosk likes mono)
    int         framesPer;     // PortAudio frames per buffer
    
    std::string stt_input_device; // voice input device

    // ---------- runtime ----------
    std::atomic<bool> running{false};
    std::thread       th;
    PaStream*         pa_in         = nullptr;
    int               hw_rate       = 0;

    VoskModel*        vmodel        = nullptr;
    VoskRecognizer*   vrec          = nullptr;

    std::queue<std::string> q;
    std::mutex              mu;
    std::condition_variable cv;

    // ---------- tiny helpers ----------
    static inline int16_t s16_round_clamp(float x) {
        x = (x >= 0.0f) ? (x + 0.5f) : (x - 0.5f);
        if (x >  32767.0f) x =  32767.0f;
        if (x < -32768.0f) x = -32768.0f;
        return (int16_t)x;
    }

    struct Resampler {
        int     in_rate  = 0;
        int     out_rate = 0;
        double  pos      = 0.0;
        int16_t last     = 0;

        void reset(int inr, int outr) {
            in_rate  = inr; out_rate = outr;
            pos = 0.0; last = 0;
        }
        void process(const int16_t* in, size_t n_in, std::vector<int16_t>& out) {
            if (n_in == 0) return;
            if (in_rate <= 0 || out_rate <= 0 || in_rate == out_rate) {
                out.insert(out.end(), in, in + n_in);
                last = in[n_in - 1];
                return;
            }
            const double step = (double)in_rate / (double)out_rate;
            double p = pos;
            while (true) {
                size_t idx1 = (size_t)p;
                if (idx1 >= n_in) break;
                const double a  = p - (double)idx1;
                const int16_t s1 = in[idx1];
                const int16_t s0 = (idx1 == 0) ? last : in[idx1 - 1];
                const float sample = (float)((1.0 - a)*(double)s0 + a*(double)s1);
                out.push_back(VoiceIO::s16_round_clamp(sample));
                p += step;
            }
            last = in[n_in - 1];
            pos  = p - (double)n_in;
        }
    } resampler;

    // ---------- misc helpers ----------
    static std::string env_or(const char* name, const char* fallback) {
        const char* v = std::getenv(name);
        return (v && *v) ? std::string(v) : std::string(fallback ? fallback : "");
    }

    static int env_get_int(const char* name, int defv) {
        const char* v = std::getenv(name);
        if (!v || !*v) return defv;
        char* end = nullptr; long x = std::strtol(v, &end, 10);
        return (end == v) ? defv : (int)x;
    }

    static float env_get_float(const char* name, float defv) {
        const char* v = std::getenv(name);
        if (!v || !*v) return defv;
        char* end = nullptr; float x = std::strtof(v, &end);
        return (end == v) ? defv : x;
    }

    static bool ends_with(const std::string& s, const char* suf) {
        size_t n = s.size(), m = std::char_traits<char>::length(suf);
        return n >= m && s.compare(n - m, m, suf) == 0;
    }

    static std::string sh_quote(const std::string& p) {
        std::string out = "'"; for (char c : p) out += (c=='\'') ? "'\"'\"'" : std::string(1,c);
        out += "'"; return out;
    }

    static bool dir_nonempty(const std::filesystem::path& d) {
        namespace fs = std::filesystem;
        if (!fs::exists(d) || !fs::is_directory(d)) return false;
        for (auto it = fs::directory_iterator(d); it != fs::directory_iterator(); ++it) return true;
        return false;
    }

    static std::string jget(const std::string& s, const char* key) {
        const std::string k = std::string("\"") + key + "\"";
        size_t p = s.find(k); if (p == std::string::npos) return "";
        p = s.find(':', p);   if (p == std::string::npos) return "";
        p = s.find('"', p);   if (p == std::string::npos) return "";
        size_t e = s.find('"', p + 1); if (e == std::string::npos) return "";
        return s.substr(p + 1, e - p - 1);
    }

    static std::string unzip_to_cache(const std::string& zipPath) {
        namespace fs = std::filesystem;
        std::string base = zipPath;
        auto slash = base.find_last_of("/\\");
        if (slash != std::string::npos) base = base.substr(slash + 1);
        if (ends_with(base, ".zip"))    base = base.substr(0, base.size() - 4);

        const std::string root   = env_or("VOSK_MODEL_UNZIP_DIR", "/etc/models/vosk");
        const fs::path    outDir = fs::path(root) / base;

        std::error_code ec;
        fs::create_directories(outDir, ec);

        const fs::path ok = outDir / ".unzipped.ok";
        if (!dir_nonempty(outDir) || !fs::exists(ok)) {
            const std::string cmd = "unzip -q -o " + sh_quote(zipPath) + " -d " + sh_quote(root);
            const int rc = std::system(cmd.c_str());
            if (rc != 0) {
                LOG_ERR("[voice] unzip failed (%d) for %s -> %s\n", rc, zipPath.c_str(), root.c_str());
                return {};
            }
            std::ofstream f(ok.string()); f << "from=" << zipPath << "\n";
        }
        return outDir.string();
    }

    static std::string ensure_model_dir(const std::string& path) {
        namespace fs = std::filesystem;
        if (path.empty()) return {};
        fs::path p(path);
        if (fs::exists(p) && fs::is_directory(p)) return path;
        if (fs::exists(p) && fs::is_regular_file(p) && ends_with(path, ".zip")) return unzip_to_cache(path);
        return {};
    }

    // ---------- PortAudio device ----------
    static std::string to_lower(std::string s) {
        for (char& c : s) c = (char)std::tolower((unsigned char)c);
        return s;
    }

    static void log_pa_devices() {
        const int n = Pa_GetDeviceCount();
        for (int i = 0; i < n; ++i) {
            const PaDeviceInfo* di = Pa_GetDeviceInfo(i);
            const PaHostApiInfo* ai = di ? Pa_GetHostApiInfo(di->hostApi) : nullptr;
            LOG_ERR("[audio] #%d api=%s in=%d out=%d name=%s\n",
                    i, ai ? ai->name : "?", di ? di->maxInputChannels : 0,
                    di ? di->maxOutputChannels : 0, (di && di->name) ? di->name : "?");
        }
    }

    static std::string norm(std::string s) {
        std::string o; o.reserve(s.size());
        for (unsigned char c : s) if (std::isalnum(c)) o.push_back((char)std::tolower(c));
        return o;
    }

    int pick_input_device_from_env() {
        const int n = Pa_GetDeviceCount();

        int first_in = paNoDevice;
        for (int i = 0; i < n; ++i) {
            const PaDeviceInfo* di = Pa_GetDeviceInfo(i);
            if (di && di->maxInputChannels > 0) { first_in = i; break; }
        }

        // numeric index?
        char* end = nullptr; long idx = std::strtol(stt_input_device.c_str(), &end, 10);
        if (end != stt_input_device.c_str() && idx >= 0 && idx < n) {
            const PaDeviceInfo* di = Pa_GetDeviceInfo((int)idx);
            if (di && di->maxInputChannels > 0) return (int)idx;
        }

        // fuzzy substring
        std::string w = norm(stt_input_device);
        for (int i = 0; i < n; ++i) {
            const PaDeviceInfo* di = Pa_GetDeviceInfo(i);
            if (!di || di->maxInputChannels <= 0) continue;
            std::string name = di->name ? di->name : "";
            if (norm(name).find(w) != std::string::npos) return i;
        }

        return first_in;
    }

    bool open_input_stream_choose_rate(int devIndex, int& openedRate) {
        const PaDeviceInfo* di = Pa_GetDeviceInfo(devIndex);
        if (!di) return false;

        PaStreamParameters in{};
        in.device = devIndex;
        in.channelCount = channels;
        in.sampleFormat = paInt16;
        in.suggestedLatency = di->defaultLowInputLatency;

        // Try requested rate first, then safe fallbacks
        std::vector<int> rates;
        rates.push_back(srate);
        if (di->defaultSampleRate > 0) rates.push_back((int)di->defaultSampleRate);
        for (int r : {48000, 44100, 32000, 24000, 16000}) {
            if (std::find(rates.begin(), rates.end(), r) == rates.end()) rates.push_back(r);
        }

        // Do NOT pre-validate with Pa_IsFormatSupported; open directly (plug/asym often fail validation)
        for (int r : rates) {
            PaError err = Pa_OpenStream(&pa_in, &in, nullptr, (double)r, (unsigned long)framesPer, paClipOff, nullptr, nullptr);
            if (err == paNoError) { openedRate = r; return true; }
            LOG_ERR("[voice] Pa_OpenStream(in) @%d Hz failed: %s\n", r, Pa_GetErrorText(err));
        }
        return false;
    }

    // ---------- VAD (simple energy-based, adaptive noise floor) ----------
    struct VAD {
        int   sr            = 16000;  // sample rate of the audio we feed to Vosk
        int   pre_ms        = 300;    // pre-roll to keep before speech start
        float margin_db     = 12.0f;  // how many dB above noise floor counts as speech
        int   start_frames  = 6;      // consecutive “speech” frames to trigger start
        int   hang_frames   = 40;     // consecutive “silence” frames to trigger end
        float noise_db      = -60.0f; // adaptive noise estimate (dB FS)
        float alpha         = 0.05f;  // noise EMA speed

        std::deque<int16_t> preroll;  // ring buffer of pre-roll audio
        bool speaking = false;
        int  above = 0, below = 0;

        void reset(int sr_) {
            sr = sr_;
            preroll.clear();
            speaking = false;
            above = below = 0;
            noise_db = -60.0f;
        }
        int preroll_nsamp() const { return (pre_ms * sr) / 1000; }

        static float level_db(const int16_t* p, int n) {
            if (n <= 0) return -120.0f;
            double s = 0.0;
            for (int i = 0; i < n; ++i) {
                float v = p[i] / 32768.0f;
                s += double(v) * double(v);
            }
            float rms = std::sqrt(s / std::max(1, n));
            return (rms > 1e-6f) ? 20.0f * std::log10(rms) : -120.0f;
        }

        void add_preroll(const int16_t* p, int n) {
            for (int i = 0; i < n; ++i) preroll.push_back(p[i]);
            int keep = preroll_nsamp();
            while ((int)preroll.size() > keep) preroll.pop_front();
        }
        
        void drain_preroll(std::vector<int16_t>& out) {
            out.reserve(out.size() + preroll.size());
            while (!preroll.empty()) { out.push_back(preroll.front()); preroll.pop_front(); }
        }

        // Update VAD with one frame (block) and return current speaking state after update
        bool update(const int16_t* p, int n) {
            const float db = level_db(p, n);

            // Update noise estimate when not clearly “far above” threshold
            if (!speaking || db < noise_db + margin_db * 0.5f) {
                noise_db = (1.0f - alpha) * noise_db + alpha * db;
                if (noise_db > -20.0f) noise_db = -20.0f; // clamp upper bound
            }

            if (db >= noise_db + margin_db) { above++; below = 0; }
            else                            { below++; above = 0; }

            if (!speaking && above >= start_frames) {
                speaking = true; above = below = 0;
            } else if (speaking && below >= hang_frames) {
                speaking = false; above = below = 0;
            }
            return speaking;
        }
    };

    bool vad_enabled = true;
    VAD  vad;

    // ---------- TTS ----------
    bool tts_init() {
        // Force ALSA backend + route to our PCM
        setenv("ESPEAKNG_AUDIO_OUTPUT", "alsa", 1);
        setenv("ESPEAKNG_PLAYBACK_DEVICE", "default", 1);
        
        int sr = espeak_Initialize(AUDIO_OUTPUT_PLAYBACK, 0, nullptr, 0);
        if (sr <= 0) {
            if (tts_use_aplay_fallback) {
                LOG_WRN("[voice] espeak ALSA playback init failed; will use aplay fallback\n");
                return true; // we will pipe to aplay in tts_say()
            }
            return false;
        }

        espeak_SetVoiceByName(tts_voice.c_str());
        espeak_SetParameter(espeakRATE, tts_wpm, 0);
        
        return true;
    }

    void tts_say(const std::string& text) {
        if (!tts_enabled || text.empty()) return;

        bool stopped = false;

        if (tts_pause_mic) {
            std::lock_guard<std::mutex> lk(mu);
            if (pa_in && Pa_IsStreamActive(pa_in) == 1) {
                Pa_StopStream(pa_in);
                stopped = true;
            }
        }

        if (tts_use_aplay_fallback) {
            // No ALSA backend in espeak-ng? Pipe to aplay on the target device.
            // Note: we avoid any shell injection by quoting the user text.
            const std::string cmd =
                "espeak-ng -v " + sh_quote(tts_voice + "+" + tts_gender) +
                " -p " + std::to_string(tts_pitch) +
                " -s " + std::to_string(tts_wpm) +
                " -a " + std::to_string(tts_amplitude) +
                " --stdout " + sh_quote(text) +
                " | aplay -q -D default";
            int rc = std::system(cmd.c_str());
            if (rc != 0) {
                LOG_ERR("[voice] tts fallback via aplay failed rc=%d\n", rc);
            }
        } else {
            // Normal library playback path
            espeak_Synth(text.c_str(), text.size() + 1, 0, POS_CHARACTER, 0, espeakCHARS_AUTO, nullptr, nullptr);
            espeak_Synchronize();
        }

        if (stopped) {
            std::lock_guard<std::mutex> lk(mu);
            if (pa_in && Pa_IsStreamStopped(pa_in) == 1) {
                Pa_StartStream(pa_in);
            }
        }
    }

    // ---------- capture thread ----------
    void thread_fn() {
        int dev = pick_input_device_from_env();
        if (dev == paNoDevice) {
            LOG_ERR("[voice] No input device found. Listing devices:\n");
            log_pa_devices();
            return;
        }
        if (!open_input_stream_choose_rate(dev, hw_rate)) {
            const PaDeviceInfo* di = Pa_GetDeviceInfo(dev);
            LOG_ERR("[voice] input open failed for device: %s\n", di && di->name ? di->name : "?");
            log_pa_devices();
            return;
        }
        if (Pa_StartStream(pa_in) != paNoError) {
            LOG_ERR("[voice] Pa_StartStream failed\n");
            Pa_CloseStream(pa_in); pa_in = nullptr;
            return;
        }

        const bool need_resample = hw_rate != srate;
        if (need_resample) {
            resampler.reset(hw_rate, srate);
        }
        vad.reset(srate);

        std::vector<int16_t> inbuf((size_t)framesPer);
        std::vector<int16_t> rsbuf; rsbuf.reserve(framesPer * 2);
        std::vector<int16_t> tmp;

        bool was_speaking = false;

        while (running) {
            PaError err = Pa_ReadStream(pa_in, inbuf.data(), (unsigned long)inbuf.size());
            if (err == paInputOverflowed) { Pa_Sleep(1);  continue; }
            if (err != paNoError)         { Pa_Sleep(10); continue; }

            // Choose the buffer we’ll analyze/forward (16 kHz mono expected by Vosk)
            const int16_t* data = nullptr;
            int            ns   = 0;

            if (need_resample) {
                rsbuf.clear();
                resampler.process(inbuf.data(), inbuf.size(), rsbuf);
                if (rsbuf.empty()) continue;
                data = rsbuf.data();
                ns   = (int)rsbuf.size();
            } else {
                data = inbuf.data();
                ns   = (int)inbuf.size();
            }

            if (!vad_enabled) {
                // Old behavior: always stream; push final results when available
                (void)vosk_recognizer_accept_waveform(vrec, (const char*)data, ns * (int)sizeof(int16_t));
                if (const char* rj = vosk_recognizer_result(vrec); rj && rj[0]) {
                    std::string js(rj);
                    std::string txt = jget(js, "text");
                    if (!txt.empty()) {
                        { std::lock_guard<std::mutex> lk(mu); q.push(txt); }
                        cv.notify_one();
                    }
                }
                continue;
            }

            // VAD gating: only stream while speaking, collect one final utterance at end
            vad.add_preroll(data, ns);
            bool speaking_now = vad.update(data, ns);

            if (!was_speaking && speaking_now) {
                // Start-of-speech: feed preroll first so we don’t clip initial words
                tmp.clear();
                vad.drain_preroll(tmp);
                if (!tmp.empty()) {
                    (void)vosk_recognizer_accept_waveform(vrec, (const char*)tmp.data(), (int)tmp.size() * (int)sizeof(int16_t));
                }
                (void)vosk_recognizer_accept_waveform(vrec, (const char*)data, ns * (int)sizeof(int16_t));
            } else if (speaking_now) {
                // In speech: keep feeding
                (void)vosk_recognizer_accept_waveform(vrec, (const char*)data, ns * (int)sizeof(int16_t));
            } else if (was_speaking && !speaking_now) {
                // End-of-speech: force finalize and emit a single full utterance
                if (const char* fj = vosk_recognizer_final_result(vrec); fj && fj[0]) {
                    std::string js(fj);
                    std::string txt = jget(js, "text");
                    if (!txt.empty()) {
                        { std::lock_guard<std::mutex> lk(mu); q.push(txt); }
                        cv.notify_one();
                        if (on_final) on_final(txt);
                    }
                } else {
                    // Fallback: try normal result if final is empty
                    if (const char* rj = vosk_recognizer_result(vrec); rj && rj[0]) {
                        std::string js(rj);
                        std::string txt = jget(js, "text");
                        if (!txt.empty()) {
                            { std::lock_guard<std::mutex> lk(mu); q.push(txt); }
                            cv.notify_one();
                            if (on_final) on_final(txt);
                        }
                    }
                }
                // Prepare recognizer for the next utterance
                vosk_recognizer_reset(vrec);
            }

            was_speaking = speaking_now;
        }

        if (pa_in) {
            Pa_StopStream(pa_in);
            Pa_CloseStream(pa_in);
            pa_in = nullptr;
        }
    }

    // ---------- lifecycle ----------
    bool init() {
        tts_enabled = env_get_int("ESPEAK", 1) != 0;
        tts_pause_mic = env_get_int("ESPEAK_PAUSE", 1) != 0;
        tts_use_aplay_fallback = env_get_int("ESPEAK_APLAY_FALLBACK", 1) != 0;

        tts_voice = env_or("ESPEAK_VOICE", "en-us");
        tts_gender = env_or("ESPEAK_GENDER", "m1");
        tts_pitch = env_get_int("ESPEAK_PITCH", 58);
        tts_wpm = env_get_int("ESPEAK_WPM", 170);
        tts_amplitude = env_get_int("ESPEAK_AMPLITUDE", 175);

        srate     = env_get_int("VOICE_RATE",     48000);
        channels  = env_get_int("VOICE_CHANNELS", 1);
        framesPer = env_get_int("VOICE_FRAMES",   1024);

        stt_input_device = env_or("VOICE_IN_DEV", "SNIPER");
        
        vad_enabled       = env_get_int  ("VOICE_VAD", 1) != 0;
        vad.margin_db     = env_get_float("VOICE_VAD_MARGIN_DB", 12.0f);
        vad.start_frames  = env_get_int  ("VOICE_VAD_START_FRAMES", 6);
        vad.hang_frames   = env_get_int  ("VOICE_VAD_HANG_FRAMES", 40);
        vad.pre_ms        = env_get_int  ("VOICE_VAD_PRE_MS", 300);
        
        std::string model_path = env_or("VOSK_MODEL", VOSK_DEFAULT_MODEL_PATH);
        if (model_path.empty()) model_path = env_or("VOSK_MODEL_DIR", "");
        if (model_path.empty()) model_path = env_or("VOSK_MODEL_ZIP", "");
        std::string model_dir = ensure_model_dir(model_path);
        if (model_dir.empty()) {
            LOG_ERR("[voice] cannot prepare vosk model from path: %s\n", model_path.c_str());
            return false;
        }

        vosk_set_log_level(-1);
        vmodel = vosk_model_new(model_dir.c_str());
        if (!vmodel) { LOG_ERR("[voice] cannot load vosk model at %s\n", model_dir.c_str()); return false; }
        vrec = vosk_recognizer_new(vmodel, (float)srate);
        if (!vrec)  { LOG_ERR("[voice] cannot create recognizer\n"); return false; }

        if (Pa_Initialize() != paNoError) {
            LOG_ERR("[voice] PortAudio init failed\n");
            return false;
        }

        if (tts_enabled) {
            if (!tts_init()) {
                LOG_ERR("[voice] eSpeak NG init failed; disabling TTS\n");
                tts_enabled = false;
            } else {
                LOG_INF("[voice] TTS ready on device: default\n");
            }
        }

        running = true;
        th = std::thread(&VoiceIO::thread_fn, this);
        return true;
    }

    void shutdown() {
        running = false;
        if (th.joinable()) th.join();
        if (vrec)   { vosk_recognizer_free(vrec); vrec = nullptr; }
        if (vmodel) { vosk_model_free(vmodel);     vmodel = nullptr; }
        Pa_Terminate();
    }

    // New: non-blocking pop with timeout for dual-source input
    bool try_wait_utt_for(std::string& out, int timeout_ms) {
        std::unique_lock<std::mutex> lk(mu);
        if (cv.wait_for(lk, std::chrono::milliseconds(timeout_ms), [&]{ return !q.empty(); })) {
            out = std::move(q.front()); q.pop();
            return true;
        }
        return false;
    }
};

static VoiceIO g_voice;

// ===== RAIZE Unix-socket bridge for external app text =====
struct ExtInbox {
    std::string in_path  = "/run/raize_llm_in.sock";   // Python -> C++
    std::string out_path = "/run/raize_llm_out.sock";  // C++ -> Python
    int         in_srv_fd = -1;
    std::thread in_th;
    std::mutex  mu;
    std::deque<std::pair<std::string,std::string>> q; // (id,text)

    std::mutex  out_mu;

    static int make_server(const std::string& path) {
        ::unlink(path.c_str());
        int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
        if (fd < 0) return -1;
        struct sockaddr_un addr{};
        addr.sun_family = AF_UNIX;
        std::snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", path.c_str());
        if (::bind(fd, (struct sockaddr*)&addr, sizeof(addr)) != 0) { ::close(fd); return -1; }
        ::chmod(path.c_str(), 0666);
        if (::listen(fd, 4) != 0) { ::close(fd); return -1; }
        return fd;
    }

    void start() {
        in_srv_fd = make_server(in_path);
        in_th = std::thread([&]{ this->accept_loop(); });
    }

    void stop() {
        if (in_srv_fd >= 0) ::close(in_srv_fd);
        if (in_th.joinable()) in_th.join();
    }

    void accept_loop() {
        while (true) {
            int cfd = ::accept(in_srv_fd, nullptr, nullptr);
            if (cfd < 0) { std::this_thread::sleep_for(std::chrono::milliseconds(100)); continue; }
            std::string buf;
            char tmp[1024];
            while (true) {
                ssize_t n = ::read(cfd, tmp, sizeof(tmp));
                if (n <= 0) break;
                buf.append(tmp, tmp+n);
                size_t p;
                while ((p = buf.find('\n')) != std::string::npos) {
                    std::string line = buf.substr(0, p);
                    buf.erase(0, p+1);
                    // Expect minimal JSON: {"id":"...", "text":"..."}
                    auto id = VoiceIO::jget(line, "id");
                    auto tx = VoiceIO::jget(line, "text");
                    if (!tx.empty()) {
                        std::lock_guard<std::mutex> lk(mu);
                        q.emplace_back(id, tx);
                    }
                }
            }
            ::close(cfd);
        }
    }

    bool pop(std::string& id, std::string& text) {
        std::lock_guard<std::mutex> lk(mu);
        if (q.empty()) return false;
        id = q.front().first; text = q.front().second; q.pop_front();
        return true;
    }

    void send_event(const std::string& json_line) {
        // connect out_path and send a line
        int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
        if (fd < 0) return;
        struct sockaddr_un addr{};
        addr.sun_family = AF_UNIX;
        std::snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", out_path.c_str());
        if (::connect(fd, (struct sockaddr*)&addr, sizeof(addr)) == 0) {
            (void)::write(fd, json_line.c_str(), json_line.size());
            (void)::write(fd, "\n", 1);
        }
        ::close(fd);
    }
} g_ext;

#endif // HAVE_VOICE_IO

// ----------------------- Globals from original CLI ----------------------
static llama_context           ** g_ctx;
static llama_model             ** g_model;
static common_sampler          ** g_smpl;
static common_params            * g_params;
static std::vector<llama_token> * g_input_tokens;
static std::ostringstream       * g_output_ss;
static std::vector<llama_token> * g_output_tokens;
static bool is_interacting  = false;
static bool need_insert_eot = false;

static void print_usage(int argc, char ** argv) {
    (void) argc;

    LOG("\nexample usage:\n");
    LOG("\n  text generation:     %s -m your_model.gguf -p \"I believe the meaning of life is\" -n 128 -no-cnv\n", argv[0]);
    LOG("\n  chat (conversation): %s -m your_model.gguf -sys \"You are a helpful assistant\"\n", argv[0]);
    LOG("\n");
}

static bool file_exists(const std::string & path) {
    std::ifstream f(path.c_str());
    return f.good();
}

static bool file_is_empty(const std::string & path) {
    std::ifstream f;
    f.exceptions(std::ifstream::failbit | std::ifstream::badbit);
    f.open(path.c_str(), std::ios::in | std::ios::binary | std::ios::ate);
    return f.tellg() == 0;
}

#if defined (__unix__) || (defined (__APPLE__) && defined (__MACH__)) || defined (_WIN32)
static void sigint_handler(int signo) {
    if (signo == SIGINT) {
        if (!is_interacting && g_params->interactive) {
            is_interacting  = true;
            need_insert_eot = true;
        } else {
            console::cleanup();
            LOG("\n");
            common_perf_print(*g_ctx, *g_smpl);

            // make sure all logs are flushed
            LOG("Interrupted by user\n");
            common_log_pause(common_log_main());

            _exit(130);
        }
    }
}
#endif

int main(int argc, char ** argv) {
    common_params params;
    g_params = &params;
    if (!common_params_parse(argc, argv, params, LLAMA_EXAMPLE_MAIN, print_usage)) {
        return 1;
    }

    common_init();

    auto & sparams = params.sampling;

    // save choice to use color for later
    // (note for later: this is a slightly awkward choice)
    console::init(params.simple_io, params.use_color);
    atexit([]() { console::cleanup(); });

    if (params.embedding) {
        LOG_ERR("************\n");
        LOG_ERR("%s: please use the 'embedding' tool for embedding calculations\n", __func__);
        LOG_ERR("************\n\n");

        return 0;
    }

    if (params.n_ctx != 0 && params.n_ctx < 8) {
        LOG_WRN("%s: warning: minimum context size is 8, using minimum size.\n", __func__);
        params.n_ctx = 8;
    }

    if (params.rope_freq_base != 0.0) {
        LOG_WRN("%s: warning: changing RoPE frequency base to %g.\n", __func__, params.rope_freq_base);
    }

    if (params.rope_freq_scale != 0.0) {
        LOG_WRN("%s: warning: scaling RoPE frequency by %g.\n", __func__, params.rope_freq_scale);
    }

    LOG_INF("%s: llama backend init\n", __func__);

    llama_backend_init();
    llama_numa_init(params.numa);

    llama_model * model = nullptr;
    llama_context * ctx = nullptr;
    common_sampler * smpl = nullptr;

    g_model = &model;
    g_ctx = &ctx;
    g_smpl = &smpl;

    std::vector<common_chat_msg> chat_msgs;

    // load the model and apply lora adapter, if any
    LOG_INF("%s: load the model and apply lora adapter, if any\n", __func__);
    common_init_result llama_init = common_init_from_params(params);

    model = llama_init.model.get();
    ctx = llama_init.context.get();

    if (model == NULL) {
        LOG_ERR("%s: error: unable to load model\n", __func__);
        return 1;
    }

    const llama_vocab * vocab = llama_model_get_vocab(model);
    auto chat_templates = common_chat_templates_init(model, params.chat_template);

    LOG_INF("%s: llama threadpool init, n_threads = %d\n", __func__, (int) params.cpuparams.n_threads);

    auto * cpu_dev = ggml_backend_dev_by_type(GGML_BACKEND_DEVICE_TYPE_CPU);
    if (!cpu_dev) {
        LOG_ERR("%s: no CPU backend found\n", __func__);
        return 1;
    }
    auto * reg = ggml_backend_dev_backend_reg(cpu_dev);
    auto * ggml_threadpool_new_fn = (decltype(ggml_threadpool_new) *) ggml_backend_reg_get_proc_address(reg, "ggml_threadpool_new");
    auto * ggml_threadpool_free_fn = (decltype(ggml_threadpool_free) *) ggml_backend_reg_get_proc_address(reg, "ggml_threadpool_free");

    struct ggml_threadpool_params tpp_batch =
            ggml_threadpool_params_from_cpu_params(params.cpuparams_batch);
    struct ggml_threadpool_params tpp =
            ggml_threadpool_params_from_cpu_params(params.cpuparams);

    set_process_priority(params.cpuparams.priority);

    struct ggml_threadpool * threadpool_batch = NULL;
    if (!ggml_threadpool_params_match(&tpp, &tpp_batch)) {
        threadpool_batch = ggml_threadpool_new_fn(&tpp_batch);
        if (!threadpool_batch) {
            LOG_ERR("%s: batch threadpool create failed : n_threads %d\n", __func__, tpp_batch.n_threads);
            return 1;
        }

        // Start the non-batch threadpool in the paused state
        tpp.paused = true;
    }

    struct ggml_threadpool * threadpool = ggml_threadpool_new_fn(&tpp);
    if (!threadpool) {
        LOG_ERR("%s: threadpool create failed : n_threads %d\n", __func__, tpp.n_threads);
        return 1;
    }

    llama_attach_threadpool(ctx, threadpool, threadpool_batch);

    const int n_ctx_train = llama_model_n_ctx_train(model);
    const int n_ctx = llama_n_ctx(ctx);

    if (n_ctx > n_ctx_train) {
        LOG_WRN("%s: model was trained on only %d context tokens (%d specified)\n", __func__, n_ctx_train, n_ctx);
    }

    // auto enable conversation mode if chat template is available
    const bool has_chat_template = common_chat_templates_was_explicit(chat_templates.get());
    if (params.conversation_mode == COMMON_CONVERSATION_MODE_AUTO) {
        if (has_chat_template) {
            LOG_INF("%s: chat template is available, enabling conversation mode (disable it with -no-cnv)\n", __func__);
            params.conversation_mode = COMMON_CONVERSATION_MODE_ENABLED;
        } else {
            params.conversation_mode = COMMON_CONVERSATION_MODE_DISABLED;
        }
    }

    // in case user force-activate conversation mode (via -cnv) without proper chat template, we show a warning
    if (params.conversation_mode && !has_chat_template) {
        LOG_WRN("%s: chat template is not available or is not supported. This may cause the model to output suboptimal responses\n", __func__);
    }

    // print chat template example in conversation mode
    if (params.conversation_mode) {
        if (params.enable_chat_template) {
            if (!params.prompt.empty() && params.system_prompt.empty()) {
                LOG_WRN("*** User-specified prompt will pre-start conversation, did you mean to set --system-prompt (-sys) instead?\n");
            }

            LOG_INF("%s: chat template example:\n%s\n", __func__, common_chat_format_example(chat_templates.get(), params.use_jinja).c_str());
        } else {
            LOG_INF("%s: in-suffix/prefix is specified, chat template will be disabled\n", __func__);
        }
    }

    // print system information
    {
        LOG_INF("\n");
        LOG_INF("%s\n", common_params_get_system_info(params).c_str());
        LOG_INF("\n");
    }

    std::string path_session = params.path_prompt_cache;
    std::vector<llama_token> session_tokens;

    if (!path_session.empty()) {
        LOG_INF("%s: attempting to load saved session from '%s'\n", __func__, path_session.c_str());
        if (!file_exists(path_session)) {
            LOG_INF("%s: session file does not exist, will create.\n", __func__);
        } else if (file_is_empty(path_session)) {
            LOG_INF("%s: The session file is empty. A new session will be initialized.\n", __func__);
        } else {
            // The file exists and is not empty
            session_tokens.resize(n_ctx);
            size_t n_token_count_out = 0;
            if (!llama_state_load_file(ctx, path_session.c_str(), session_tokens.data(), session_tokens.capacity(), &n_token_count_out)) {
                LOG_ERR("%s: failed to load session file '%s'\n", __func__, path_session.c_str());
                return 1;
            }
            session_tokens.resize(n_token_count_out);
            LOG_INF("%s: loaded a session with prompt size of %d tokens\n", __func__, (int)session_tokens.size());
        }
    }

    const bool add_bos = llama_vocab_get_add_bos(vocab) && !params.use_jinja;
    if (!llama_model_has_encoder(model)) {
        GGML_ASSERT(!llama_vocab_get_add_eos(vocab));
    }

    LOG_DBG("n_ctx: %d, add_bos: %d\n", n_ctx, add_bos);

    std::vector<llama_token> embd_inp;

    bool waiting_for_first_input = false;
    auto chat_add_and_format = [&chat_msgs, &chat_templates](const std::string & role, const std::string & content) {
        common_chat_msg new_msg;
        new_msg.role = role;
        new_msg.content = content;
        auto formatted = common_chat_format_single(chat_templates.get(), chat_msgs, new_msg, role == "user", g_params->use_jinja);
        chat_msgs.push_back(new_msg);
        LOG_DBG("formatted: '%s'\n", formatted.c_str());
        return formatted;
    };

    std::string prompt;
    {
        if (params.conversation_mode && params.enable_chat_template) {
            if (!params.system_prompt.empty()) {
                // format the system prompt (will use template default if empty)
                chat_add_and_format("system", params.system_prompt);
            }

            if (!params.prompt.empty()) {
                // format and append the user prompt
                chat_add_and_format("user", params.prompt);
            } else {
                waiting_for_first_input = true;
            }

            if (!params.system_prompt.empty() || !params.prompt.empty()) {
                common_chat_templates_inputs inputs;
                inputs.messages = chat_msgs;
                inputs.add_generation_prompt = !params.prompt.empty();

                prompt = common_chat_templates_apply(chat_templates.get(), inputs).prompt;
            }
        } else {
            // otherwise use the prompt as is
            prompt = params.prompt;
        }

        if (params.interactive_first || !prompt.empty() || session_tokens.empty()) {
            LOG_DBG("tokenize the prompt\n");
            embd_inp = common_tokenize(ctx, prompt, true, true);
        } else {
            LOG_DBG("use session tokens\n");
            embd_inp = session_tokens;
        }

        LOG_DBG("prompt: \"%s\"\n", prompt.c_str());
        LOG_DBG("tokens: %s\n", string_from(ctx, embd_inp).c_str());
    }

    // Should not run without any tokens
    if (!waiting_for_first_input && embd_inp.empty()) {
        if (add_bos) {
            embd_inp.push_back(llama_vocab_bos(vocab));
            LOG_WRN("embd_inp was considered empty and bos was added: %s\n", string_from(ctx, embd_inp).c_str());
        } else {
            LOG_ERR("input is empty\n");
            return -1;
        }
    }

    // Tokenize negative prompt
    if ((int) embd_inp.size() > n_ctx - 4) {
        LOG_ERR("%s: prompt is too long (%d tokens, max %d)\n", __func__, (int) embd_inp.size(), n_ctx - 4);
        return 1;
    }

    // debug message about similarity of saved session, if applicable
    size_t n_matching_session_tokens = 0;
    if (!session_tokens.empty()) {
        for (llama_token id : session_tokens) {
            if (n_matching_session_tokens >= embd_inp.size() || id != embd_inp[n_matching_session_tokens]) {
                break;
            }
            n_matching_session_tokens++;
        }
        if (params.prompt.empty() && n_matching_session_tokens == embd_inp.size()) {
            LOG_INF("%s: using full prompt from session file\n", __func__);
        } else if (n_matching_session_tokens >= embd_inp.size()) {
            LOG_INF("%s: session file has exact match for prompt!\n", __func__);
        } else if (n_matching_session_tokens < (embd_inp.size() / 2)) {
            LOG_WRN("%s: session file has low similarity to prompt (%zu / %zu tokens); will mostly be reevaluated\n",
                    __func__, n_matching_session_tokens, embd_inp.size());
        } else {
            LOG_INF("%s: session file matches %zu / %zu tokens of prompt\n",
                    __func__, n_matching_session_tokens, embd_inp.size());
        }

        // remove any "future" tokens that we might have inherited from the previous session
        llama_kv_self_seq_rm(ctx, -1, n_matching_session_tokens, -1);
    }

    LOG_DBG("recalculate the cached logits (check): embd_inp.size() %zu, n_matching_session_tokens %zu, embd_inp.size() %zu, session_tokens.size() %zu\n",
         embd_inp.size(), n_matching_session_tokens, embd_inp.size(), session_tokens.size());

    // if we will use the cache for the full prompt without reaching the end of the cache, force
    // reevaluation of the last token to recalculate the cached logits
    if (!embd_inp.empty() && n_matching_session_tokens == embd_inp.size() && session_tokens.size() > embd_inp.size()) {
        LOG_DBG("recalculate the cached logits (do): session_tokens.resize( %zu )\n", embd_inp.size() - 1);

        session_tokens.resize(embd_inp.size() - 1);
    }

    // number of tokens to keep when resetting context
    if (params.n_keep < 0 || params.n_keep > (int) embd_inp.size()) {
        params.n_keep = (int)embd_inp.size();
    } else {
        params.n_keep += add_bos; // always keep the BOS token
    }

    if (params.conversation_mode) {
        if (params.single_turn && !params.prompt.empty()) {
            params.interactive = false;
            params.interactive_first = false;
        } else {
            params.interactive_first = true;
        }
    }

    // enable interactive mode if interactive start is specified
    if (params.interactive_first) {
        params.interactive = true;
    }

    if (params.verbose_prompt) {
        LOG_INF("%s: prompt: '%s'\n", __func__, params.prompt.c_str());
        LOG_INF("%s: number of tokens in prompt = %zu\n", __func__, embd_inp.size());
        for (int i = 0; i < (int) embd_inp.size(); i++) {
            LOG_INF("%6d -> '%s'\n", embd_inp[i], common_token_to_piece(ctx, embd_inp[i]).c_str());
        }

        if (params.n_keep > add_bos) {
            LOG_INF("%s: static prompt based on n_keep: '", __func__);
            for (int i = 0; i < params.n_keep; i++) {
                LOG_CNT("%s", common_token_to_piece(ctx, embd_inp[i]).c_str());
            }
            LOG_CNT("'\n");
        }
        LOG_INF("\n");
    }

    // ctrl+C handling
    {
#if defined (__unix__) || (defined (__APPLE__) && defined (__MACH__))
        struct sigaction sigint_action;
        sigint_action.sa_handler = sigint_handler;
        sigemptyset (&sigint_action.sa_mask);
        sigint_action.sa_flags = 0;
        sigaction(SIGINT, &sigint_action, NULL);
#elif defined (_WIN32)
        auto console_ctrl_handler = +[](DWORD ctrl_type) -> BOOL {
            return (ctrl_type == CTRL_C_EVENT) ? (sigint_handler(SIGINT), true) : false;
        };
        SetConsoleCtrlHandler(reinterpret_cast<PHANDLER_ROUTINE>(console_ctrl_handler), true);
#endif
    }
	
    // Init voice I/O (optional)
#ifdef HAVE_VOICE_IO
    std::mutex voice_cb_mu;
    std::deque<std::string> voice_ready;

    if (!g_voice.init()) {
        LOG_ERR("[voice] init failed; continuing with keyboard input\n");
    } else {
        g_voice.tts_say("Hello, I'm ready. Please speak.");
        
        g_voice.set_on_final([&](const std::string& txt){
            std::lock_guard<std::mutex> lk(voice_cb_mu);
            voice_ready.push_back(txt);
        });
    }
#endif

    if (params.interactive) {
        LOG_INF("%s: interactive mode on.\n", __func__);

// #ifdef HAVE_VOICE_IO
//         // Start RAIZE external inbox
//         g_ext.start();
//         // Tell BLE bridge we're alive
//         g_ext.send_event(std::string("{\"type\":\"status\",\"up\":true,\"ts\":\"") + now_iso_utc() + "\"}");
// #endif

        if (!params.antiprompt.empty()) {
            for (const auto & antiprompt : params.antiprompt) {
                LOG_INF("Reverse prompt: '%s'\n", antiprompt.c_str());
                if (params.verbose_prompt) {
                    auto tmp = common_tokenize(ctx, antiprompt, false, true);
                    for (int i = 0; i < (int) tmp.size(); i++) {
                        LOG_INF("%6d -> '%s'\n", tmp[i], common_token_to_piece(ctx, tmp[i]).c_str());
                    }
                }
            }
        }

        if (params.input_prefix_bos) {
            LOG_INF("Input prefix with BOS\n");
        }

        if (!params.input_prefix.empty()) {
            LOG_INF("Input prefix: '%s'\n", params.input_prefix.c_str());
            if (params.verbose_prompt) {
                auto tmp = common_tokenize(ctx, params.input_prefix, true, true);
                for (int i = 0; i < (int) tmp.size(); i++) {
                    LOG_INF("%6d -> '%s'\n", tmp[i], common_token_to_piece(ctx, tmp[i]).c_str());
                }
            }
        }

        if (!params.input_suffix.empty()) {
            LOG_INF("Input suffix: '%s'\n", params.input_suffix.c_str());
            if (params.verbose_prompt) {
                auto tmp = common_tokenize(ctx, params.input_suffix, false, true);
                for (int i = 0; i < (int) tmp.size(); i++) {
                    LOG_INF("%6d -> '%s'\n", tmp[i], common_token_to_piece(ctx, tmp[i]).c_str());
                }
            }
        }
    }

    smpl = common_sampler_init(model, sparams);
    if (!smpl) {
        LOG_ERR("%s: failed to initialize sampling subsystem\n", __func__);
        return 1;
    }

    LOG_INF("sampler seed: %u\n",     common_sampler_get_seed(smpl));
    LOG_INF("sampler params: \n%s\n", sparams.print().c_str());
    LOG_INF("sampler chain: %s\n",    common_sampler_print(smpl).c_str());

    LOG_INF("generate: n_ctx = %d, n_batch = %d, n_predict = %d, n_keep = %d\n", n_ctx, params.n_batch, params.n_predict, params.n_keep);

    // group-attention state
    // number of grouped KV tokens so far (used only if params.grp_attn_n > 1)
    int ga_i = 0;

    const int ga_n = params.grp_attn_n;
    const int ga_w = params.grp_attn_w;

    if (ga_n != 1) {
        GGML_ASSERT(ga_n > 0                    && "grp_attn_n must be positive");                     // NOLINT
        GGML_ASSERT(ga_w % ga_n == 0            && "grp_attn_w must be a multiple of grp_attn_n");     // NOLINT
      //GGML_ASSERT(n_ctx_train % ga_w == 0     && "n_ctx_train must be a multiple of grp_attn_w");    // NOLINT
      //GGML_ASSERT(n_ctx >= n_ctx_train * ga_n && "n_ctx must be at least n_ctx_train * grp_attn_n"); // NOLINT
        LOG_INF("self-extend: n_ctx_train = %d, grp_attn_n = %d, grp_attn_w = %d\n", n_ctx_train, ga_n, ga_w);
    }
    LOG_INF("\n");

    if (params.interactive) {
        const char * control_message;
        if (params.multiline_input) {
            control_message = " - To return control to the AI, end your input with '\\'.\n"
                              " - To return control without starting a new line, end your input with '/'.\n";
        } else {
            control_message = " - Press Return to return control to the AI.\n"
                              " - To return control without starting a new line, end your input with '/'.\n"
                              " - If you want to submit another line, end your input with '\\'.\n";
        }
        LOG_INF("== Running in interactive mode. ==\n");
#if defined (__unix__) || (defined (__APPLE__) && defined (__MACH__)) || defined (_WIN32)
        LOG_INF(       " - Press Ctrl+C to interject at any time.\n");
#endif
        LOG_INF(       "%s", control_message);
        if (params.conversation_mode && params.enable_chat_template && params.system_prompt.empty()) {
            LOG_INF(   " - Not using system message. To change it, set a different value via -sys PROMPT\n");
        }
        LOG_INF("\n");

        is_interacting = params.interactive_first;
    }

    bool is_antiprompt        = false;
    bool input_echo           = true;
    bool display              = true;
    bool need_to_save_session = !path_session.empty() && n_matching_session_tokens < embd_inp.size();

    int n_past             = 0;
    int n_remain           = params.n_predict;
    int n_consumed         = 0;
    int n_session_consumed = 0;

    std::vector<int>   input_tokens;  g_input_tokens  = &input_tokens;
    std::vector<int>   output_tokens; g_output_tokens = &output_tokens;
    std::ostringstream output_ss;     g_output_ss     = &output_ss;
    std::ostringstream assistant_ss; // for storing current assistant message, used in conversation mode

    // the first thing we will do is to output the prompt, so set color accordingly
    console::set_display(console::prompt);
    display = params.display_prompt;

    std::vector<llama_token> embd;

    // single-token antiprompts
    std::vector<llama_token> antiprompt_token;

    for (const std::string & antiprompt : params.antiprompt) {
        auto ids = ::common_tokenize(ctx, antiprompt, false, true);
        if (ids.size() == 1) {
            antiprompt_token.push_back(ids[0]);
        }
    }

    if (llama_model_has_encoder(model)) {
        int enc_input_size = embd_inp.size();
        llama_token * enc_input_buf = embd_inp.data();

        if (llama_encode(ctx, llama_batch_get_one(enc_input_buf, enc_input_size))) {
            LOG_ERR("%s : failed to eval\n", __func__);
            return 1;
        }

        llama_token decoder_start_token_id = llama_model_decoder_start_token(model);
        if (decoder_start_token_id == LLAMA_TOKEN_NULL) {
            decoder_start_token_id = llama_vocab_bos(vocab);
        }

        embd_inp.clear();
        embd_inp.push_back(decoder_start_token_id);
    }

    while ((n_remain != 0 && !is_antiprompt) || params.interactive) {
        // predict
        if (!embd.empty()) {
            // Note: (n_ctx - 4) here is to match the logic for commandline prompt handling via
            // --prompt or --file which uses the same value.
            int max_embd_size = n_ctx - 4;

            // Ensure the input doesn't exceed the context size by truncating embd if necessary.
            if ((int) embd.size() > max_embd_size) {
                const int skipped_tokens = (int) embd.size() - max_embd_size;
                embd.resize(max_embd_size);

                console::set_display(console::error);
                LOG_WRN("<<input too long: skipped %d token%s>>", skipped_tokens, skipped_tokens != 1 ? "s" : "");
                console::set_display(console::reset);
            }

            if (ga_n == 1) {
                // infinite text generation via context shifting
                // if we run out of context:
                // - take the n_keep first tokens from the original prompt (via n_past)
                // - take half of the last (n_ctx - n_keep) tokens and recompute the logits in batches

                if (n_past + (int) embd.size() >= n_ctx) {
                    if (!params.ctx_shift){
                        LOG_DBG("\n\n%s: context full and context shift is disabled => stopping\n", __func__);
                        break;
                    }

                    if (params.n_predict == -2) {
                        LOG_DBG("\n\n%s: context full and n_predict == -%d => stopping\n", __func__, params.n_predict);
                        break;
                    }

                    const int n_left    = n_past - params.n_keep;
                    const int n_discard = n_left/2;

                    LOG_DBG("context full, swapping: n_past = %d, n_left = %d, n_ctx = %d, n_keep = %d, n_discard = %d\n",
                            n_past, n_left, n_ctx, params.n_keep, n_discard);

                    llama_kv_self_seq_rm (ctx, 0, params.n_keep            , params.n_keep + n_discard);
                    llama_kv_self_seq_add(ctx, 0, params.n_keep + n_discard, n_past, -n_discard);

                    n_past -= n_discard;

                    LOG_DBG("after swap: n_past = %d\n", n_past);

                    LOG_DBG("embd: %s\n", string_from(ctx, embd).c_str());

                    LOG_DBG("clear session path\n");
                    path_session.clear();
                }
            } else {
                // context extension via Self-Extend
                while (n_past >= ga_i + ga_w) {
                    const int ib = (ga_n*ga_i)/ga_w;
                    const int bd = (ga_w/ga_n)*(ga_n - 1);
                    const int dd = (ga_w/ga_n) - ib*bd - ga_w;

                    LOG_DBG("\n");
                    LOG_DBG("shift: [%6d, %6d] + %6d -> [%6d, %6d]\n", ga_i, n_past, ib*bd, ga_i + ib*bd, n_past + ib*bd);
                    LOG_DBG("div:   [%6d, %6d] / %6d -> [%6d, %6d]\n", ga_i + ib*bd, ga_i + ib*bd + ga_w, ga_n, (ga_i + ib*bd)/ga_n, (ga_i + ib*bd + ga_w)/ga_n);
                    LOG_DBG("shift: [%6d, %6d] + %6d -> [%6d, %6d]\n", ga_i + ib*bd + ga_w, n_past + ib*bd, dd, ga_i + ib*bd + ga_w + dd, n_past + ib*bd + dd);

                    llama_kv_self_seq_add(ctx, 0, ga_i,                n_past,              ib*bd);
                    llama_kv_self_seq_div(ctx, 0, ga_i + ib*bd,        ga_i + ib*bd + ga_w, ga_n);
                    llama_kv_self_seq_add(ctx, 0, ga_i + ib*bd + ga_w, n_past + ib*bd,      dd);

                    n_past -= bd;

                    ga_i += ga_w/ga_n;

                    LOG_DBG("\nn_past_old = %d, n_past = %d, ga_i = %d\n\n", n_past + bd, n_past, ga_i);
                }
            }

            // try to reuse a matching prefix from the loaded session instead of re-eval (via n_past)
            if (n_session_consumed < (int) session_tokens.size()) {
                size_t i = 0;
                for ( ; i < embd.size(); i++) {
                    if (embd[i] != session_tokens[n_session_consumed]) {
                        session_tokens.resize(n_session_consumed);
                        break;
                    }

                    n_past++;
                    n_session_consumed++;

                    if (n_session_consumed >= (int) session_tokens.size()) {
                        ++i;
                        break;
                    }
                }
                if (i > 0) {
                    embd.erase(embd.begin(), embd.begin() + i);
                }
            }

            for (int i = 0; i < (int) embd.size(); i += params.n_batch) {
                int n_eval = (int) embd.size() - i;
                if (n_eval > params.n_batch) {
                    n_eval = params.n_batch;
                }

                LOG_DBG("eval: %s\n", string_from(ctx, embd).c_str());

                if (llama_decode(ctx, llama_batch_get_one(&embd[i], n_eval))) {
                    LOG_ERR("%s : failed to eval\n", __func__);
                    return 1;
                }

                n_past += n_eval;

                LOG_DBG("n_past = %d\n", n_past);
                // Display total tokens alongside total time
                if (params.n_print > 0 && n_past % params.n_print == 0) {
                    LOG_DBG("\n\033[31mTokens consumed so far = %d / %d \033[0m\n", n_past, n_ctx);
                }
            }

            if (!embd.empty() && !path_session.empty()) {
                session_tokens.insert(session_tokens.end(), embd.begin(), embd.end());
                n_session_consumed = session_tokens.size();
            }
        }

        embd.clear();

        if ((int) embd_inp.size() <= n_consumed && !is_interacting) {
            // optionally save the session on first sample (for faster prompt loading next time)
            if (!path_session.empty() && need_to_save_session && !params.prompt_cache_ro) {
                need_to_save_session = false;
                llama_state_save_file(ctx, path_session.c_str(), session_tokens.data(), session_tokens.size());

                LOG_DBG("saved session to %s\n", path_session.c_str());
            }

            const llama_token id = common_sampler_sample(smpl, ctx, -1);

            common_sampler_accept(smpl, id, /* accept_grammar= */ true);

            // LOG_DBG("last: %s\n", string_from(ctx, smpl->prev.to_vector()).c_str());

            embd.push_back(id);

            // echo this to console
            input_echo = true;

            // decrement remaining sampling budget
            --n_remain;

            LOG_DBG("n_remain: %d\n", n_remain);
        } else {
            // some user input remains from prompt or interaction, forward it to processing
            LOG_DBG("embd_inp.size(): %d, n_consumed: %d\n", (int) embd_inp.size(), n_consumed);
            while ((int) embd_inp.size() > n_consumed) {
                embd.push_back(embd_inp[n_consumed]);

                // push the prompt in the sampling context in order to apply repetition penalties later
                // for the prompt, we don't apply grammar rules
                common_sampler_accept(smpl, embd_inp[n_consumed], /* accept_grammar= */ false);

                ++n_consumed;
                if ((int) embd.size() >= params.n_batch) {
                    break;
                }
            }
        }

        // display text
        if (input_echo && display) {
            for (auto id : embd) {
                const std::string token_str = common_token_to_piece(ctx, id, params.special);

                // Console/Stream Output
                LOG("%s", token_str.c_str());

                // Record Displayed Tokens To Log
                // Note: Generated tokens are created one by one hence this check
                if (embd.size() > 1) {
                    // Incoming Requested Tokens
                    input_tokens.push_back(id);
                } else {
                    // Outgoing Generated Tokens
                    output_tokens.push_back(id);
                    output_ss << token_str;
                }
            }
        }

        // reset color to default if there is no pending user input
        if (input_echo && (int) embd_inp.size() == n_consumed) {
            console::set_display(console::reset);
            display = true;
        }

        // if not currently processing queued inputs;
        if ((int) embd_inp.size() <= n_consumed) {
            // check for reverse prompt in the last n_prev tokens
            if (!params.antiprompt.empty()) {
                const int n_prev = 32;
                const std::string last_output = common_sampler_prev_str(smpl, ctx, n_prev);

                is_antiprompt = false;
                // Check if each of the reverse prompts appears at the end of the output.
                // If we're not running interactively, the reverse prompt might be tokenized with some following characters
                // so we'll compensate for that by widening the search window a bit.
                for (std::string & antiprompt : params.antiprompt) {
                    size_t extra_padding = params.interactive ? 0 : 2;
                    size_t search_start_pos = last_output.length() > static_cast<size_t>(antiprompt.length() + extra_padding)
                        ? last_output.length() - static_cast<size_t>(antiprompt.length() + extra_padding)
                        : 0;

                    if (last_output.find(antiprompt, search_start_pos) != std::string::npos) {
                        if (params.interactive) {
                            is_interacting = true;
                        }
                        is_antiprompt = true;
                        break;
                    }
                }

                // check for reverse prompt using special tokens
                llama_token last_token = common_sampler_last(smpl);
                for (auto token : antiprompt_token) {
                    if (token == last_token) {
                        if (params.interactive) {
                            is_interacting = true;
                        }
                        is_antiprompt = true;
                        break;
                    }
                }

                if (is_antiprompt) {
                    LOG_DBG("found antiprompt: %s\n", last_output.c_str());
                }
            }

            // deal with end of generation tokens in interactive mode
            if (!waiting_for_first_input && llama_vocab_is_eog(vocab, common_sampler_last(smpl))) {
                LOG_DBG("found an EOG token\n");

                if (params.interactive) {
                    if (!params.antiprompt.empty()) {
                        // tokenize and inject first reverse prompt
                        const auto first_antiprompt = common_tokenize(ctx, params.antiprompt.front(), false, true);
                        embd_inp.insert(embd_inp.end(), first_antiprompt.begin(), first_antiprompt.end());
                        is_antiprompt = true;
                    }

                    if (params.enable_chat_template) {
                        chat_add_and_format("assistant", assistant_ss.str());
                        // Send final event
                        std::string content = assistant_ss.str();
#ifdef HAVE_VOICE_IO
                        g_voice.tts_say(content);
                        // // Pull id if set in this scope (best-effort); else empty
                        // /* using global g_ble_current_id */
                        // std::ostringstream ev;
                        // // Use UTC time
                        // std::time_t t = std::time(nullptr);
                        // char buf[32]; std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", std::gmtime(&t));
                        // ev << "{\"type\":\"final\",\"id\":\"" << g_ble_current_id
                        //    << "\",\"content\":" << json_escape(content)
                        //    << ",\"ts\":\"" << buf << "\"}";
                        // g_ext.send_event(ev.str());
                        // g_ble_current_id.clear();
#endif
                    }
                    is_interacting = true;
                    LOG("\n");
                }
            }

            // if current token is not EOG, we add it to current assistant message
            if (params.conversation_mode && !waiting_for_first_input) {
                const auto id = common_sampler_last(smpl);
                assistant_ss << common_token_to_piece(ctx, id, false);

                if (!prompt.empty()) {
                    prompt.clear();
                    is_interacting = false;
                }
            }

            if ((n_past > 0 || waiting_for_first_input) && is_interacting) {
                LOG_DBG("waiting for user input\n");

                if (params.conversation_mode) {
                    LOG("\n> ");
                }

                // When a full assistant message is spoken, emit a JSON event for BLE
                // Hooking two places:
                // 1) When EOG is detected and assistant_ss is set (below)
                // 2) When token budget is hit (interactive loop continuation)

                if (params.input_prefix_bos) {
                    LOG_DBG("adding input prefix BOS token\n");
                    embd_inp.push_back(llama_vocab_bos(vocab));
                }

                std::string buffer;
#ifdef HAVE_VOICE_IO
                // Prefer external inbox; if none arrives within 50ms, poll voice with timeout.
                {
                    // std::string ext_id, ext_text;
                    // if (g_ext.pop(ext_id, ext_text)) {
                    //     buffer = ext_text;
                    //     // Store current id in a static so we can mirror it back on final
                    //     /* using global g_ble_current_id */
                    //     g_ble_current_id = ext_id;
                    //     // Inform BLE we accepted the request
                    //     if (!g_ble_current_id.empty()) {
                    //         g_ext.send_event(std::string("{\"type\":\"ack\",\"id\":\"")+g_ble_current_id+"\"}");
                    //     }
                    // } else {
                        // 1) Instant grab if a final utterance was just delivered
                        {
                            std::lock_guard<std::mutex> lk(voice_cb_mu);
                            if (!voice_ready.empty()) {
                                buffer = std::move(voice_ready.front());
                                voice_ready.pop_front();
                            }
                        }
                        if (buffer.empty()) {
                            std::string voice;
                            if (g_voice.try_wait_utt_for(voice, 50)) {
                                buffer = std::move(voice);
                            }
                        }
                    // }
                }
                if (!buffer.empty()) {
                    LOG_INF("%s", buffer.c_str());
                    // Ack to BLE if it had an id (since id is only known in g_ext.pop(), we can't retrieve it here; optional)
                }
#else
                if (!params.input_prefix.empty() && !params.conversation_mode) {
                    LOG_DBG("appending input prefix: '%s'\n", params.input_prefix.c_str());
                    LOG("%s", params.input_prefix.c_str());
                }

                // color user input only
                console::set_display(console::user_input);
                display = params.display_prompt;

                std::string line;
                bool another_line = true;
                do {
                    another_line = console::readline(line, params.multiline_input);
                    buffer += line;
                } while (another_line);

                // done taking input, reset color
                console::set_display(console::reset);
                display = true;

                if (buffer.empty()) { // Ctrl+D on empty line exits
                    LOG("EOF by user\n");
                    break;
                }

                if (buffer.back() == '\n') {
                    buffer.pop_back();
                }
#endif
                if (buffer.empty()) { // Enter key on empty line lets the user pass control back
                    LOG_DBG("empty line, passing control back\n");
                } else {
                    // Add tokens to embd only if the input buffer is non-empty
                    // append input suffix if any
                    buffer += "/no_think\n";

                    if (!params.input_suffix.empty() && !params.conversation_mode) {
                        LOG_DBG("appending input suffix: '%s'\n", params.input_suffix.c_str());
                        LOG("%s", params.input_suffix.c_str());
                    }

                    LOG_DBG("buffer: '%s'\n", buffer.c_str());

                    const size_t original_size = embd_inp.size();

                    if (params.escape) {
                        string_process_escapes(buffer);
                    }

                    bool format_chat = params.conversation_mode && params.enable_chat_template;
                    std::string user_inp = format_chat
                        ? chat_add_and_format("user", std::move(buffer))
                        : std::move(buffer);
                    // TODO: one inconvenient of current chat template implementation is that we can't distinguish between user input and special tokens (prefix/postfix)
                    const auto line_pfx = common_tokenize(ctx, params.input_prefix, false, true);
                    const auto line_inp = common_tokenize(ctx, user_inp,            false, format_chat);
                    const auto line_sfx = common_tokenize(ctx, params.input_suffix, false, true);

                    LOG_DBG("input tokens: %s\n", string_from(ctx, line_inp).c_str());

                    // if user stop generation mid-way, we must add EOT to finish model's last response
                    if (need_insert_eot && format_chat) {
                        llama_token eot = llama_vocab_eot(vocab);
                        embd_inp.push_back(eot == LLAMA_TOKEN_NULL ? llama_vocab_eos(vocab) : eot);
                        need_insert_eot = false;
                    }

                    embd_inp.insert(embd_inp.end(), line_pfx.begin(), line_pfx.end());
                    embd_inp.insert(embd_inp.end(), line_inp.begin(), line_inp.end());
                    embd_inp.insert(embd_inp.end(), line_sfx.begin(), line_sfx.end());

                    for (size_t i = original_size; i < embd_inp.size(); ++i) {
                        const llama_token token = embd_inp[i];
                        output_tokens.push_back(token);
                        output_ss << common_token_to_piece(ctx, token);
                    }

                    // reset assistant message
                    assistant_ss.str("");

                    n_remain -= line_inp.size();
                    LOG_DBG("n_remain: %d\n", n_remain);
                }

                input_echo = false; // do not echo this again
            }

            if (n_past > 0 || waiting_for_first_input) {
                if (is_interacting) {
                    common_sampler_reset(smpl);
                }
                is_interacting = false;

                if (waiting_for_first_input && params.single_turn) {
                    params.interactive = false;
                    params.interactive_first = false;
                }
                waiting_for_first_input = false;
            }
        }

        // end of generation
        if (!embd.empty() && llama_vocab_is_eog(vocab, embd.back()) && !(params.interactive)) {
            LOG(" [end of text]\n");
            break;
        }

        // In interactive mode, respect the maximum number of tokens and drop back to user input when reached.
        // We skip this logic when n_predict == -1 (infinite) or -2 (stop at context size).
        if (params.interactive && n_remain <= 0 && params.n_predict >= 0) {
            n_remain = params.n_predict;
            is_interacting = true;
#ifdef HAVE_VOICE_IO
            // speak what we have so far in this turn
            g_voice.tts_say(assistant_ss.str());
            // // Emit partial/final as needed (here treat as final chunk for simplicity)
            // std::string content = assistant_ss.str();
            // /* using global g_ble_current_id */
            // std::time_t t = std::time(nullptr);
            // char buf[32]; std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", std::gmtime(&t));
            // std::ostringstream ev;
            // ev << "{\"type\":\"final\",\"id\":\"" << g_ble_current_id
            //    << "\",\"content\":" << json_escape(content)
            //    << ",\"ts\":\"" << buf << "\"}";
            // g_ext.send_event(ev.str());
            // g_ble_current_id.clear();
#endif
        }
    }

    if (!path_session.empty() && params.prompt_cache_all && !params.prompt_cache_ro) {
        LOG("\n%s: saving final output to session file '%s'\n", __func__, path_session.c_str());
        llama_state_save_file(ctx, path_session.c_str(), session_tokens.data(), session_tokens.size());
    }

    LOG("\n\n");
    common_perf_print(ctx, smpl);

    common_sampler_free(smpl);

#ifdef HAVE_VOICE_IO
    g_voice.shutdown();
#endif

    llama_backend_free();

    ggml_threadpool_free_fn(threadpool);
    ggml_threadpool_free_fn(threadpool_batch);

    return 0;
}