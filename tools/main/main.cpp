#include "arg.h"
#include "common.h"
#include "console.h"
#include "log.h"
#include "sampling.h"
#include "llama.h"
#include "chat.h"

#include <cstdio>
#include <cstring>
#include <ctime>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>
#include <thread>
#include <atomic>

#if defined(__unix__) || (defined(__APPLE__) && defined(__MACH__))
#include <signal.h>
#include <unistd.h>
#elif defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <signal.h>
#endif

#if defined(_MSC_VER)
#pragma warning(disable: 4244 4267)
#endif

// ========================= Voice I/O (optional) =========================
#ifdef WITH_VOICE_IO
#include <queue>
#include <mutex>
#include <condition_variable>

#include <portaudio.h>
#include <vosk_api.h>
#include <espeak/speak_lib.h>

#ifndef VOSK_DEFAULT_MODEL_DIR
#define VOSK_DEFAULT_MODEL_DIR "/usr/share/vosk/models/vosk-model-small-en-us-0.15"
#endif

struct VoiceIO {
    // config
    int   srate      = 16000;
    int   channels   = 1;
    int   framesPer  = 512;
    const char* tts_voice = "en-us";
    int   tts_rate_wpm = 170;

    // state
    std::atomic<bool> running{false};
    std::thread       th;
    PaStream*         pa_in = nullptr;
    VoskModel*        vmodel = nullptr;
    VoskRecognizer*   vrec   = nullptr;

    std::queue<std::string> q;
    std::mutex              mu;
    std::condition_variable cv;

    static std::string env_or(const char* name, const char* fallback) {
        const char* v = std::getenv(name);
        return v && *v ? std::string(v) : std::string(fallback);
    }

    static std::string jget(const std::string& s, const char* key) {
        std::string k = std::string("\"") + key + "\"";
        size_t p = s.find(k); if (p == std::string::npos) return "";
        p = s.find(':', p);   if (p == std::string::npos) return "";
        p = s.find('"', p);   if (p == std::string::npos) return "";
        size_t e = s.find('"', p+1); if (e == std::string::npos) return "";
        return s.substr(p+1, e-p-1);
    }

    bool tts_init() {
        int sr = espeak_Initialize(AUDIO_OUTPUT_PLAYBACK, 0, nullptr, 0);
        if (sr <= 0) return false;
        espeak_SetVoiceByName(tts_voice);
        espeak_SetParameter(espeakRATE, tts_rate_wpm, 0);
        return true;
    }

    void tts_say(const std::string& text) {
        if (text.empty()) return;
        espeak_Synth(text.c_str(), text.size()+1, 0, POS_CHARACTER, 0, espeakCHARS_AUTO, nullptr, nullptr);
        espeak_Synchronize();
    }

    void thread_fn() {
        PaStreamParameters in{};
        in.device = Pa_GetDefaultInputDevice();
        if (in.device == paNoDevice) {
            LOG_ERR("[voice] No default input device\n");
            return;
        }
        const PaDeviceInfo* di = Pa_GetDeviceInfo(in.device);
        in.channelCount = channels;
        in.sampleFormat = paInt16;
        in.suggestedLatency = di->defaultLowInputLatency;

        if (Pa_OpenStream(&pa_in, &in, nullptr, srate, framesPer, paNoFlag, nullptr, nullptr) != paNoError) {
            LOG_ERR("[voice] Pa_OpenStream failed\n");
            return;
        }
        Pa_StartStream(pa_in);

        std::vector<int16_t> buf(framesPer);
        while (running) {
            if (Pa_ReadStream(pa_in, buf.data(), buf.size()) != paNoError) continue;
            vosk_recognizer_accept_waveform(vrec, (const char*)buf.data(), buf.size()*sizeof(int16_t));

            // when Vosk finalizes a segment, result() returns a final JSON with "text"
            const char* rj = vosk_recognizer_result(vrec);
            if (rj && rj[0]) {
                std::string js(rj);
                std::string txt = jget(js, "text");
                if (!txt.empty()) {
                    {
                        std::lock_guard<std::mutex> lk(mu);
                        q.push(txt);
                    }
                    cv.notify_one();
                }
            }
        }

        Pa_StopStream(pa_in);
        Pa_CloseStream(pa_in);
        pa_in = nullptr;
    }

    bool init() {
        // model path via env VOSK_MODEL or default macro
        std::string model_dir = env_or("VOSK_MODEL", VOSK_DEFAULT_MODEL_DIR);
        std::string tts_v = env_or("ESPEAK_VOICE", tts_voice);
        if (!tts_v.empty()) tts_voice = tts_v.c_str();

        vosk_set_log_level(-1);
        vmodel = vosk_model_new(model_dir.c_str());
        if (!vmodel) {
            LOG_ERR("[voice] cannot load vosk model at %s\n", model_dir.c_str());
            return false;
        }
        vrec = vosk_recognizer_new(vmodel, (float)srate);
        if (!vrec) {
            LOG_ERR("[voice] cannot create recognizer\n");
            return false;
        }

        if (Pa_Initialize() != paNoError) {
            LOG_ERR("[voice] PortAudio init failed\n");
            return false;
        }
        if (!tts_init()) {
            LOG_ERR("[voice] eSpeak NG init failed\n");
            return false;
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

    std::string wait_utt() {
        std::unique_lock<std::mutex> lk(mu);
        cv.wait(lk, [&]{ return !q.empty(); });
        std::string s = std::move(q.front());
        q.pop();
        return s;
    }
};

static VoiceIO g_voice;
#endif // WITH_VOICE_IO
// =======================================================================

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

#if defined(__unix__) || (defined(__APPLE__) && defined(__MACH__)) || defined(_WIN32)
static void sigint_handler(int signo) {
    if (signo == SIGINT) {
        if (!is_interacting && g_params->interactive) {
            is_interacting  = true;
            need_insert_eot = true;
        } else {
            console::cleanup();
            LOG("\n");
            common_perf_print(*g_ctx, *g_smpl);
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

    console::init(params.simple_io, params.use_color);
    atexit([]() { console::cleanup(); });

    if (params.embedding) {
        LOG_ERR("************\n");
        LOG_ERR("%s: please use the 'embedding' tool for embedding calculations\n", __func__);
        LOG_ERR("************\n\n");
        return 0;
    }

    if (params.n_ctx != 0 && params.n_ctx < 8) {
        LOG_WRN("%s: minimum context size is 8; using 8.\n", __func__);
        params.n_ctx = 8;
    }

    LOG_INF("%s: llama backend init\n", __func__);
    llama_backend_init();
    llama_numa_init(params.numa);

    llama_model * model = nullptr;
    llama_context * ctx = nullptr;
    common_sampler * smpl = nullptr;

    g_model = &model;
    g_ctx   = &ctx;
    g_smpl  = &smpl;

    // Load model/context
    LOG_INF("%s: load the model and apply lora adapter, if any\n", __func__);
    common_init_result llama_init = common_init_from_params(params);
    model = llama_init.model.get();
    ctx   = llama_init.context.get();
    if (model == NULL) {
        LOG_ERR("%s: error: unable to load model\n", __func__);
        return 1;
    }

    const llama_vocab * vocab = llama_model_get_vocab(model);
    auto chat_templates = common_chat_templates_init(model, params.chat_template);

    // Threadpools
    LOG_INF("%s: llama threadpool init, n_threads = %d\n", __func__, (int) params.cpuparams.n_threads);
    auto * cpu_dev = ggml_backend_dev_by_type(GGML_BACKEND_DEVICE_TYPE_CPU);
    if (!cpu_dev) {
        LOG_ERR("%s: no CPU backend found\n", __func__);
        return 1;
    }
    auto * reg = ggml_backend_dev_backend_reg(cpu_dev);
    auto * ggml_threadpool_new_fn  = (decltype(ggml_threadpool_new)  *) ggml_backend_reg_get_proc_address(reg, "ggml_threadpool_new");
    auto * ggml_threadpool_free_fn = (decltype(ggml_threadpool_free) *) ggml_backend_reg_get_proc_address(reg, "ggml_threadpool_free");

    struct ggml_threadpool_params tpp_batch = ggml_threadpool_params_from_cpu_params(params.cpuparams_batch);
    struct ggml_threadpool_params tpp       = ggml_threadpool_params_from_cpu_params(params.cpuparams);

    set_process_priority(params.cpuparams.priority);

    struct ggml_threadpool * threadpool_batch = NULL;
    if (!ggml_threadpool_params_match(&tpp, &tpp_batch)) {
        threadpool_batch = ggml_threadpool_new_fn(&tpp_batch);
        if (!threadpool_batch) {
            LOG_ERR("%s: batch threadpool create failed : n_threads %d\n", __func__, tpp_batch.n_threads);
            return 1;
        }
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

    // Conversation mode auto
    const bool has_chat_template = common_chat_templates_was_explicit(chat_templates.get());
    if (params.conversation_mode == COMMON_CONVERSATION_MODE_AUTO) {
        params.conversation_mode = has_chat_template ? COMMON_CONVERSATION_MODE_ENABLED : COMMON_CONVERSATION_MODE_DISABLED;
        if (has_chat_template) {
            LOG_INF("%s: chat template is available, enabling conversation mode (disable with -no-cnv)\n", __func__);
        }
    }
    if (params.conversation_mode && !has_chat_template) {
        LOG_WRN("%s: chat template not available/supported; outputs may be suboptimal\n", __func__);
    }

    // System info
    LOG_INF("\n%s\n\n", common_params_get_system_info(params).c_str());

    // Session cache
    std::string path_session = params.path_prompt_cache;
    std::vector<llama_token> session_tokens;
    if (!path_session.empty()) {
        LOG_INF("%s: attempting to load session from '%s'\n", __func__, path_session.c_str());
        if (!file_exists(path_session)) {
            LOG_INF("%s: session file does not exist, will create.\n", __func__);
        } else if (file_is_empty(path_session)) {
            LOG_INF("%s: session file is empty, new session will be initialized.\n", __func__);
        } else {
            session_tokens.resize(n_ctx);
            size_t n_token_count_out = 0;
            if (!llama_state_load_file(ctx, path_session.c_str(), session_tokens.data(), session_tokens.capacity(), &n_token_count_out)) {
                LOG_ERR("%s: failed to load session file '%s'\n", __func__, path_session.c_str());
                return 1;
            }
            session_tokens.resize(n_token_count_out);
            LOG_INF("%s: loaded session with %d tokens\n", __func__, (int) session_tokens.size());
        }
    }

    const bool add_bos = llama_vocab_get_add_bos(vocab) && !params.use_jinja;
    if (!llama_model_has_encoder(model)) {
        GGML_ASSERT(!llama_vocab_get_add_eos(vocab));
    }

    std::vector<common_chat_msg> chat_msgs;
    auto chat_add_and_format = [&chat_msgs, &chat_templates](const std::string & role, const std::string & content) {
        common_chat_msg new_msg;
        new_msg.role = role;
        new_msg.content = content;
        auto formatted = common_chat_format_single(chat_templates.get(), chat_msgs, new_msg, role == "user", g_params->use_jinja);
        chat_msgs.push_back(new_msg);
        return formatted;
    };

    std::string prompt;
    std::vector<llama_token> embd_inp;
    bool waiting_for_first_input = false;

    // Build initial prompt / conversation header
    {
        if (params.conversation_mode && params.enable_chat_template) {
            if (!params.system_prompt.empty()) chat_add_and_format("system", params.system_prompt);
            if (!params.prompt.empty()) {
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
            prompt = params.prompt;
        }

        if (params.interactive_first || !prompt.empty() || session_tokens.empty()) {
            embd_inp = common_tokenize(ctx, prompt, true, true);
        } else {
            embd_inp = session_tokens;
        }
    }

    if (!waiting_for_first_input && embd_inp.empty()) {
        if (add_bos) {
            embd_inp.push_back(llama_vocab_bos(vocab));
        } else {
            LOG_ERR("input is empty\n");
            return -1;
        }
    }

    if ((int) embd_inp.size() > n_ctx - 4) {
        LOG_ERR("%s: prompt too long (%d tokens, max %d)\n", __func__, (int) embd_inp.size(), n_ctx - 4);
        return 1;
    }

    if (params.n_keep < 0 || params.n_keep > (int) embd_inp.size()) {
        params.n_keep = (int)embd_inp.size();
    } else {
        params.n_keep += add_bos;
    }

    if (params.conversation_mode) {
        if (params.single_turn && !params.prompt.empty()) {
            params.interactive = false;
            params.interactive_first = false;
        } else {
            params.interactive_first = true;
        }
    }
    if (params.interactive_first) params.interactive = true;

    // Ctrl+C handler
#if defined(__unix__) || (defined(__APPLE__) && defined(__MACH__)) || defined(_WIN32)
    {
        struct sigaction sigint_action;
        sigint_action.sa_handler = sigint_handler;
        sigemptyset(&sigint_action.sa_mask);
        sigint_action.sa_flags = 0;
        sigaction(SIGINT, &sigint_action, NULL);
    }
#endif

    // Init voice I/O (optional)
#ifdef WITH_VOICE_IO
    if (!g_voice.init()) {
        LOG_ERR("[voice] init failed; continuing with keyboard input\n");
    } else {
        g_voice.tts_say("Hello, I'm ready. Please speak.");
    }
#endif

    if (params.interactive) {
        LOG_INF("%s: interactive mode on.\n", __func__);
        if (!params.antiprompt.empty()) {
            for (const auto & ap : params.antiprompt) LOG_INF("Reverse prompt: '%s'\n", ap.c_str());
        }
    }

    smpl = common_sampler_init(model, sparams);
    if (!smpl) {
        LOG_ERR("%s: failed to initialize sampling\n", __func__);
        return 1;
    }

    LOG_INF("sampler seed: %u\n",     common_sampler_get_seed(smpl));
    LOG_INF("sampler params: \n%s\n", sparams.print().c_str());
    LOG_INF("sampler chain: %s\n",    common_sampler_print(smpl).c_str());
    LOG_INF("generate: n_ctx = %d, n_batch = %d, n_predict = %d, n_keep = %d\n", n_ctx, params.n_batch, params.n_predict, params.n_keep);

    int n_past             = 0;
    int n_remain           = params.n_predict;
    int n_consumed         = 0;

    std::vector<int>   input_tokens;  g_input_tokens  = &input_tokens;
    std::vector<int>   output_tokens; g_output_tokens = &output_tokens;
    std::ostringstream output_ss;     g_output_ss     = &output_ss;
    std::ostringstream assistant_ss;

    console::set_display(console::prompt);
    bool input_echo = true;
    bool display    = params.display_prompt;
    std::vector<llama_token> embd;

    // antiprompts that are single tokens
    std::vector<llama_token> antiprompt_token;
    for (const std::string & ap : params.antiprompt) {
        auto ids = ::common_tokenize(ctx, ap, false, true);
        if (ids.size() == 1) antiprompt_token.push_back(ids[0]);
    }

    if (llama_model_has_encoder(model)) {
        int enc_input_size = embd_inp.size();
        llama_token * enc_input_buf = embd_inp.data();
        if (llama_encode(ctx, llama_batch_get_one(enc_input_buf, enc_input_size))) {
            LOG_ERR("%s : failed to eval\n", __func__);
            return 1;
        }
        llama_token decoder_start_token_id = llama_model_decoder_start_token(model);
        if (decoder_start_token_id == LLAMA_TOKEN_NULL) decoder_start_token_id = llama_vocab_bos(vocab);
        embd_inp.clear();
        embd_inp.push_back(decoder_start_token_id);
    }

    bool is_antiprompt = false;

    while ((n_remain != 0 && !is_antiprompt) || params.interactive) {
        if (!embd.empty()) {
            if (n_past + (int) embd.size() >= n_ctx) {
                if (!params.ctx_shift || params.n_predict == -2) break;

                const int n_left    = n_past - params.n_keep;
                const int n_discard = n_left/2;
                llama_kv_self_seq_rm (ctx, 0, params.n_keep            , params.n_keep + n_discard);
                llama_kv_self_seq_add(ctx, 0, params.n_keep + n_discard, n_past, -n_discard);
                n_past -= n_discard;
                LOG_DBG("context shift: n_past=%d\n", n_past);
            }

            // evaluate in batches
            for (int i = 0; i < (int) embd.size(); i += params.n_batch) {
                int n_eval = std::min<int>(params.n_batch, (int)embd.size() - i);
                if (llama_decode(ctx, llama_batch_get_one(&embd[i], n_eval))) {
                    LOG_ERR("%s : failed to eval\n", __func__);
                    return 1;
                }
                n_past += n_eval;
            }
        }

        embd.clear();

        if ((int) embd_inp.size() <= n_consumed && !is_interacting) {
            // sample a token
            const llama_token id = common_sampler_sample(smpl, ctx, -1);
            common_sampler_accept(smpl, id, /* accept_grammar= */ true);
            embd.push_back(id);
            input_echo = true;
            --n_remain;
        } else {
            while ((int) embd_inp.size() > n_consumed) {
                embd.push_back(embd_inp[n_consumed]);
                common_sampler_accept(smpl, embd_inp[n_consumed], /* accept_grammar= */ false);
                ++n_consumed;
                if ((int) embd.size() >= params.n_batch) break;
            }
        }

        // display tokens
        if (input_echo && display) {
            for (auto id : embd) {
                const std::string token_str = common_token_to_piece(ctx, id, params.special);
                LOG("%s", token_str.c_str());
                if (embd.size() > 1) {
                    input_tokens.push_back(id);
                } else {
                    output_tokens.push_back(id);
                    output_ss << token_str;
                }
            }
        }

        if (input_echo && (int) embd_inp.size() == n_consumed) {
            console::set_display(console::reset);
            display = true;
        }

        // reverse prompt detection
        if ((int) embd_inp.size() <= n_consumed) {
            if (!params.antiprompt.empty()) {
                const int n_prev = 32;
                const std::string last_output = common_sampler_prev_str(smpl, ctx, n_prev);
                is_antiprompt = false;

                for (std::string ap : params.antiprompt) {
                    size_t extra_padding = params.interactive ? 0 : 2;
                    size_t search_start_pos = last_output.length() > ap.length() + extra_padding
                            ? last_output.length() - (ap.length() + extra_padding)
                            : 0;
                    if (last_output.find(ap, search_start_pos) != std::string::npos) {
                        if (params.interactive) is_interacting = true;
                        is_antiprompt = true;
                        break;
                    }
                }

                llama_token last_token = common_sampler_last(smpl);
                for (auto t : antiprompt_token) {
                    if (t == last_token) {
                        if (params.interactive) is_interacting = true;
                        is_antiprompt = true;
                        break;
                    }
                }
            }

            // EOG handling in interactive mode
            if (!waiting_for_first_input && llama_vocab_is_eog(vocab, common_sampler_last(smpl))) {
                if (params.interactive) {
                    if (!params.antiprompt.empty()) {
                        const auto first_ap = common_tokenize(ctx, params.antiprompt.front(), false, true);
                        embd_inp.insert(embd_inp.end(), first_ap.begin(), first_ap.end());
                        is_antiprompt = true;
                    }
                    if (params.enable_chat_template) {
                        // finalize assistant turn
                        // accumulate last token
                        // (we also build assistant_ss below as tokens stream)
                    }
#ifdef WITH_VOICE_IO
                    g_voice.tts_say(assistant_ss.str());
#endif
                    is_interacting = true;
                    LOG("\n");
                }
            }

            // accumulate assistant response text (for TTS)
            if (params.conversation_mode && !waiting_for_first_input) {
                const auto id = common_sampler_last(smpl);
                assistant_ss << common_token_to_piece(ctx, id, false);
            }

            // wait for user input if interacting
            if ((n_past > 0 || waiting_for_first_input) && is_interacting) {
                if (params.conversation_mode) LOG("\n> ");

                if (params.input_prefix_bos) {
                    embd_inp.push_back(llama_vocab_bos(vocab));
                }

                std::string buffer;

#ifdef WITH_VOICE_IO
                // block until we get a final utterance from Vosk
                buffer = g_voice.wait_utt();
                LOG_INF("[mic] %s\n", buffer.c_str());
#else
                // keyboard path
                console::set_display(console::user_input);
                display = params.display_prompt;
                std::string line;
                bool another_line = true;
                do {
                    another_line = console::readline(line, params.multiline_input);
                    buffer += line;
                } while (another_line);
                console::set_display(console::reset);
                display = true;
                if (buffer.empty()) { LOG("EOF by user\n"); break; }
                if (!buffer.empty() && buffer.back() == '\n') buffer.pop_back();
#endif

                if (!buffer.empty()) {
                    const size_t original_size = embd_inp.size();

                    if (params.escape) string_process_escapes(buffer);

                    bool format_chat = params.conversation_mode && params.enable_chat_template;
                    std::string user_inp = format_chat ? chat_add_and_format("user", std::move(buffer))
                                                       : std::move(buffer);

                    // if user stopped generation mid-way, close the assistant turn for template
                    if (need_insert_eot && format_chat) {
                        llama_token eot = llama_vocab_eot(vocab);
                        embd_inp.push_back(eot == LLAMA_TOKEN_NULL ? llama_vocab_eos(vocab) : eot);
                        need_insert_eot = false;
                    }

                    const auto line_pfx = common_tokenize(ctx, params.input_prefix, false, true);
                    const auto line_inp = common_tokenize(ctx, user_inp,            false, format_chat);
                    const auto line_sfx = common_tokenize(ctx, params.input_suffix, false, true);

                    embd_inp.insert(embd_inp.end(), line_pfx.begin(), line_pfx.end());
                    embd_inp.insert(embd_inp.end(), line_inp.begin(), line_inp.end());
                    embd_inp.insert(embd_inp.end(), line_sfx.begin(), line_sfx.end());

                    for (size_t i = original_size; i < embd_inp.size(); ++i) {
                        const llama_token token = embd_inp[i];
                        output_tokens.push_back(token);
                        output_ss << common_token_to_piece(ctx, token);
                    }

                    // reset assistant turn text
                    assistant_ss.str("");
                    assistant_ss.clear();

                    n_remain -= line_inp.size();
                }

                input_echo = false;
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

        // end if single-shot and reached EOG
        if (!embd.empty() && llama_vocab_is_eog(vocab, embd.back()) && !params.interactive) {
            LOG(" [end of text]\n");
            break;
        }

        // In interactive mode, when we hit max tokens budget, return to user
        if (params.interactive && n_remain <= 0 && params.n_predict >= 0) {
            n_remain = params.n_predict;
            is_interacting = true;
#ifdef WITH_VOICE_IO
            // speak what we have so far in this turn
            g_voice.tts_say(assistant_ss.str());
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

#ifdef WITH_VOICE_IO
    g_voice.shutdown();
#endif

    llama_backend_free();

    ggml_threadpool_free_fn(threadpool);
    ggml_threadpool_free_fn(threadpool_batch);

    return 0;
}
