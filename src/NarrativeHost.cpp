// CL-1 Phase 3 — NarrativeHost implementation.
// llama.cpp is dlopen'ed at runtime: no link-time dependency, and a
// missing library/model degrades to a safe no-op. The worker thread owns
// all llama state; the sim thread only touches two mutexed containers.
// This file NEVER reads or writes World state — state_hash() cannot see it.
#include "simlab/NarrativeHost.hpp"

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <deque>
#include <dlfcn.h>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "llama.h"

namespace simlab {
namespace {

// ---- resolved llama C API (dlopen) -----------------------------------------
struct LlamaApi {
    void* h = nullptr;
    bool ok = false;

    decltype(&llama_backend_init) backend_init = nullptr;
    decltype(&llama_backend_free) backend_free = nullptr;
    decltype(&llama_model_load_from_file) model_load = nullptr;
    decltype(&llama_model_free) model_free = nullptr;
    decltype(&llama_init_from_model) ctx_init = nullptr;
    decltype(&llama_free) ctx_free = nullptr;
    decltype(&llama_model_get_vocab) get_vocab = nullptr;
    decltype(&llama_vocab_n_tokens) vocab_n_tokens = nullptr;
    decltype(&llama_vocab_is_eog) vocab_is_eog = nullptr;
    decltype(&llama_tokenize) tokenize = nullptr;
    decltype(&llama_detokenize) detokenize = nullptr;
    decltype(&llama_get_memory) get_memory = nullptr;
    decltype(&llama_memory_seq_rm) seq_rm = nullptr;
    decltype(&llama_decode) decode = nullptr;
    decltype(&llama_get_logits) get_logits = nullptr;
    decltype(&llama_sampler_chain_init) chain_init = nullptr;
    decltype(&llama_sampler_chain_add) chain_add = nullptr;
    decltype(&llama_sampler_init_temp) samp_temp = nullptr;
    decltype(&llama_sampler_init_top_k) samp_top_k = nullptr;
    decltype(&llama_sampler_init_dist) samp_dist = nullptr;
    decltype(&llama_sampler_sample) samp_sample = nullptr;
    decltype(&llama_sampler_accept) samp_accept = nullptr;
    decltype(&llama_sampler_free) samp_free = nullptr;
    decltype(&llama_set_n_threads) set_threads = nullptr;
    decltype(&llama_model_default_params) model_params = nullptr;
    decltype(&llama_context_default_params) ctx_params = nullptr;
    decltype(&llama_batch_ext_init) bext_init = nullptr;
    decltype(&llama_batch_ext_free) bext_free = nullptr;
    decltype(&llama_batch_ext_clear) bext_clear = nullptr;
    decltype(&llama_batch_ext_add_token) bext_add = nullptr;
    decltype(&llama_batch_ext_set_pos) bext_pos = nullptr;
    decltype(&llama_batch_ext_set_output_logits) bext_logits = nullptr;
    decltype(&llama_process) process = nullptr;

    template <typename T> bool sym(T& dst, const char* n) {
        dst = reinterpret_cast<T>(dlsym(h, n));
        return dst != nullptr;
    }

    bool load(const std::string& dir) {
        static const char* kNames[] = {
            "libllama.so", "libllama.so.1", "libllama.so.0",
        };
        for (const char* n : kNames) {
            std::string p = dir.empty() ? n : dir + "/" + n;
            h = dlopen(p.c_str(), RTLD_NOW | RTLD_LOCAL);
            if (h) break;
        }
        if (!h) {   // fall back to the loader search path
            for (const char* n : kNames) {
                h = dlopen(n, RTLD_NOW | RTLD_LOCAL);
                if (h) break;
            }
        }
        if (!h) return false;
        ok =
            sym(backend_init, "llama_backend_init") &&
            sym(backend_free, "llama_backend_free") &&
            sym(model_load, "llama_model_load_from_file") &&
            sym(model_free, "llama_model_free") &&
            sym(ctx_init, "llama_init_from_model") &&
            sym(ctx_free, "llama_free") &&
            sym(get_vocab, "llama_model_get_vocab") &&
            sym(vocab_n_tokens, "llama_vocab_n_tokens") &&
            sym(vocab_is_eog, "llama_vocab_is_eog") &&
            sym(tokenize, "llama_tokenize") &&
            sym(detokenize, "llama_detokenize") &&
            sym(get_memory, "llama_get_memory") &&
            sym(seq_rm, "llama_memory_seq_rm") &&
            sym(decode, "llama_decode") &&
            sym(get_logits, "llama_get_logits") &&
            sym(chain_init, "llama_sampler_chain_init") &&
            sym(chain_add, "llama_sampler_chain_add") &&
            sym(samp_temp, "llama_sampler_init_temp") &&
            sym(samp_top_k, "llama_sampler_init_top_k") &&
            sym(samp_dist, "llama_sampler_init_dist") &&
            sym(samp_sample, "llama_sampler_sample") &&
            sym(samp_accept, "llama_sampler_accept") &&
            sym(samp_free, "llama_sampler_free") &&
            sym(set_threads, "llama_set_n_threads") &&
            sym(model_params, "llama_model_default_params") &&
            sym(ctx_params, "llama_context_default_params") &&
            sym(bext_init, "llama_batch_ext_init") &&
            sym(bext_free, "llama_batch_ext_free") &&
            sym(bext_clear, "llama_batch_ext_clear") &&
            sym(bext_add, "llama_batch_ext_add_token") &&
            sym(bext_pos, "llama_batch_ext_set_pos") &&
            sym(bext_logits, "llama_batch_ext_set_output_logits") &&
            sym(process, "llama_process");
        if (!ok) { dlclose(h); h = nullptr; }
        return ok;
    }
    ~LlamaApi() { if (h) dlclose(h); }
};

struct Job { std::uint64_t key; std::string sys; std::string user;
             int max_new = 0; };

} // namespace

struct NarrativeHost::Impl {
    Config cfg;
    LlamaApi api;

    llama_model* model = nullptr;
    llama_context* ctx = nullptr;
    llama_batch_ext* batch = nullptr;
    const llama_vocab* vocab = nullptr;
    int32_t n_vocab = 0;

    std::mutex mu;
    std::condition_variable cv;
    std::deque<Job> queue;
    std::unordered_map<std::uint64_t, std::string> done;
    bool stopping = false;
    std::jthread worker;
    std::atomic<bool> ready{false};
    bool backend_up = false;

    bool init() {
        if (!api.load(cfg.lib_dir)) return false;
        if (!backend_up) { api.backend_init(); backend_up = true; }
        model = api.model_load(cfg.model_path.c_str(), api.model_params());
        if (!model) { if (backend_up) { api.backend_free(); backend_up = false; } return false; }
        llama_context_params cp = api.ctx_params();
        cp.n_ctx = cfg.n_ctx;
        cp.n_seq_max = 1;
        // Prompt + generation must fit one batch: chapter prompts run
        // well past 512 tokens — size the batch to the context window or
        // bext_add() overflows and generate() silently drops the job.
        cp.n_batch = cfg.n_ctx;
        cp.n_ubatch = cfg.n_ctx;
        ctx = api.ctx_init(model, cp);
        if (!ctx) { api.model_free(model); model = nullptr;
                    if (backend_up) { api.backend_free(); backend_up = false; }
                    return false; }
        api.set_threads(ctx, 2, 4);
        batch = api.bext_init(ctx);
        if (!batch) { api.ctx_free(ctx); ctx = nullptr; api.model_free(model);
                      model = nullptr;
                      if (backend_up) { api.backend_free(); backend_up = false; }
                      return false; }
        vocab = api.get_vocab(model);
        n_vocab = vocab ? static_cast<int32_t>(api.vocab_n_tokens(vocab)) : 0;
        ready.store(true);
        return true;
    }

    void shutdown() {
        ready.store(false);
        {
            std::lock_guard<std::mutex> lk(mu);
            stopping = true;
        }
        cv.notify_all();
        if (worker.joinable()) worker.join();
        if (batch) { api.bext_free(batch); batch = nullptr; }
        if (ctx) { api.ctx_free(ctx); ctx = nullptr; }
        if (model) { api.model_free(model); model = nullptr; }
        if (backend_up) { api.backend_free(); backend_up = false; }
    }

    // Qwen-instruct one-liner chronicler (live feed callouts).
    static const char* kOnelinerSys() {
        return "You are the chronicler of an artificial ecosystem. Write "
               "one vivid two-sentence account of the event. Plain prose "
               "only, no preamble.";
    }

    // Build the Qwen-instruct chat prompt (chat template, no jinja dep).
    static std::string prompt_for(const std::string& sys,
                                  const std::string& user) {
        return std::string("<|im_start|>system\n") + sys +
               "<|im_end|>\n<|im_start|>user\n" + user +
               "<|im_end|>\n<|im_start|>assistant\n";
    }

    // Generate one narration; returns false on any llama error.
    bool generate(const std::string& sys, const std::string& user,
                  int max_new, std::string& out) {
        const std::string prompt = prompt_for(sys, user);
        std::vector<llama_token> toks(static_cast<std::size_t>(prompt.size()) + 64);
        const int nt = api.tokenize(vocab, prompt.data(),
                                    static_cast<int32_t>(prompt.size()),
                                    toks.data(),
                                    static_cast<int32_t>(toks.size()),
                                    /*add_special=*/true,
                                    /*parse_special=*/true);
        if (nt <= 0) return false;
        toks.resize(static_cast<std::size_t>(nt));

        api.seq_rm(api.get_memory(ctx), 0, -1, -1);  // fresh prompt KV

        // prompt: all tokens, logits only on the last
        api.bext_clear(batch);
        for (int i = 0; i < nt; ++i) {
            const int32_t idx = api.bext_add(batch, 0, toks[i]);
            if (idx < 0) return false;
            const llama_pos p = static_cast<llama_pos>(i);
            api.bext_pos(batch, idx, &p);
        }
        api.bext_logits(batch, nt - 1, true);
        if (api.process(ctx, LLAMA_PROCESS_TYPE_DECODE, batch) != 0)
            return false;

        llama_sampler* smpl = api.chain_init({});
        api.chain_add(smpl, api.samp_temp(cfg.temperature));
        api.chain_add(smpl, api.samp_top_k(static_cast<int32_t>(cfg.top_k)));
        api.chain_add(smpl, api.samp_dist(cfg.seed));

        std::vector<llama_token> gen;
        int budget = max_new > 0 ? max_new : cfg.max_new_tokens;
        // never overflow the KV window: shrink the budget to what fits
        budget = std::min<int>(budget,
            std::max<int>(8, cfg.n_ctx - nt - 8));
        gen.reserve(static_cast<std::size_t>(budget));
        llama_token tok = LLAMA_TOKEN_NULL;
        for (int i = 0; i < budget; ++i) {
            tok = api.samp_sample(smpl, ctx, -1);
            if (tok == LLAMA_TOKEN_NULL) break;
            if (vocab && api.vocab_is_eog(vocab, tok)) break;
            gen.push_back(tok);
            api.bext_clear(batch);
            const int32_t idx = api.bext_add(batch, 0, tok);
            if (idx < 0) break;
            const llama_pos p = static_cast<llama_pos>(nt + i);
            api.bext_pos(batch, idx, &p);
            api.bext_logits(batch, idx, true);
            if (api.process(ctx, LLAMA_PROCESS_TYPE_DECODE, batch) != 0)
                break;
            api.samp_accept(smpl, tok);
        }
        api.samp_free(smpl);

        if (gen.empty()) return false;
        std::vector<char> buf(gen.size() * 8 + 1, 0);
        const int nd = api.detokenize(vocab, gen.data(),
                                      static_cast<int32_t>(gen.size()),
                                      buf.data(),
                                      static_cast<int32_t>(buf.size()),
                                      /*add_special=*/false,
                                      /*parse_special=*/true);
        if (nd <= 0) return false;
        out.assign(buf.data(), static_cast<std::size_t>(nd));
        return true;
    }

    void run() {
        while (true) {
            Job job;
            {
                std::unique_lock<std::mutex> lk(mu);
                cv.wait(lk, [&] { return stopping || !queue.empty(); });
                if (stopping && queue.empty()) return;
                if (queue.empty()) continue;
                job = std::move(queue.front());
                queue.pop_front();
            }
            if (!ready.load()) continue;
            std::string text;
            const std::string sys = job.sys.empty() ? kOnelinerSys() : job.sys;
            if (generate(sys, job.user, job.max_new, text)) {
                std::lock_guard<std::mutex> lk(mu);
                if (done.size() < cfg.max_results) done[job.key] = std::move(text);
            }
        }
    }
};

NarrativeHost::NarrativeHost() : impl_(std::make_unique<Impl>()) {}
NarrativeHost::~NarrativeHost() { stop(); }

bool NarrativeHost::start() { return start(Config{}); }

bool NarrativeHost::start(const Config& cfg) {
    stop();
    impl_->cfg = cfg;
    if (!impl_->init()) return false;
    impl_->worker = std::jthread([this] { impl_->run(); });
    return true;
}

void NarrativeHost::stop() {
    if (impl_->worker.joinable() || impl_->model || impl_->ctx) impl_->shutdown();
}

bool NarrativeHost::active() const { return impl_->ready.load(); }

void NarrativeHost::submit(std::uint64_t key, const std::string& summary) {
    submit_prompt(key, "", summary);
}

void NarrativeHost::submit_prompt(std::uint64_t key, const std::string& sys,
                                  const std::string& user, int max_new) {
    if (!active()) return;
    std::lock_guard<std::mutex> lk(impl_->mu);
    if (impl_->queue.size() >= impl_->cfg.max_queue) impl_->queue.pop_front();
    impl_->queue.push_back({key, sys, user, max_new});
    impl_->cv.notify_one();
}

bool NarrativeHost::poll(std::uint64_t key, std::string& out) {
    std::lock_guard<std::mutex> lk(impl_->mu);
    auto it = impl_->done.find(key);
    if (it == impl_->done.end()) return false;
    out = std::move(it->second);
    impl_->done.erase(it);
    return true;
}

} // namespace simlab
