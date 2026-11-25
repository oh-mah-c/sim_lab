#pragma once
#include <cstdint>
#include <memory>
#include <string>

// CL-1 Phase 3 — NarrativeHost: asynchronous language layer.
//
// Wraps llama.cpp (vendored under third_party/lang-host, built to
// third_party/llama-build) and a GGUF instruct model (default
// third_party/models/qwen2.5-1.5b-instruct-q4_k_m.gguf) on a dedicated
// std::jthread. The sim thread only calls submit()/poll() — a bounded,
// mutex-protected queue. The worker never touches World state, so
// state_hash() is bit-identical with the host on or off.
//
// Safe-disable contract: if the shared library or the model file is
// missing, start() returns false and every other call is a no-op.
// Nothing here is deterministic — narrative text is presentation only.

namespace simlab {

class NarrativeHost {
public:
    struct Config {
        std::string model_path =
            "third_party/models/qwen2.5-1.5b-instruct-q4_k_m.gguf";
        std::string lib_dir = "third_party/llama-build/bin";
        std::uint32_t max_queue = 64;      // pending prompts (drop-oldest)
        std::uint32_t max_results = 256;   // completed texts kept for poll
        int max_new_tokens = 192;
        float temperature = 0.2f;          // low-temp, near-greedy
        std::uint32_t top_k = 40;
        std::uint32_t seed = 0xC0FFEE;
        std::uint32_t n_ctx = 2048;
    };

    NarrativeHost();
    ~NarrativeHost();
    NarrativeHost(const NarrativeHost&) = delete;
    NarrativeHost& operator=(const NarrativeHost&) = delete;

    // Loads the model + spawns the worker thread. Returns false (and stays
    // inert) when the library or model cannot be loaded.
    bool start();
    bool start(const Config& cfg);
    void stop();
    bool active() const;

    // Enqueue a structured event summary for narration. `key` is the
    // lookup id (CausalEvent::id or EntityId) used by poll().
    void submit(std::uint64_t key, const std::string& summary);

    // Enqueue a full system+user prompt (chronicle chapters, batched
    // narration). Same queue/poll contract as submit().
    void submit_prompt(std::uint64_t key, const std::string& sys,
                       const std::string& user);

    // Fetch a completed narration for `key` (non-blocking). Returns false
    // until the worker finishes it; each key is delivered once.
    bool poll(std::uint64_t key, std::string& out);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace simlab
