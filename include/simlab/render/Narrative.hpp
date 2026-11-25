#pragma once
// Emergent Narrative Chronicle (Phase 15, viewer-side, SDL-free).
//
// The "soul" layer: turns REAL causal-ledger events into natural-language
// chronicle lines for the Live Feed, floating callouts and inspector life
// stories. 100% data-driven: every sentence pattern lives in
// data/narrative_templates.txt — this file only resolves {slots} against
// World Truth (entity names, wielded tools, wounded anatomy, coined words)
// and the player's Rosetta journal for quoted speech.
//
// Read-only like Patron: scan() takes const World&, mutates only the
// caller-owned ChronicleFeed (viewer memory, never World Truth, never
// hashed, never saved with the world).
#include <cstdint>
#include <deque>
#include <map>
#include <set>
#include <string>
#include <vector>
#include "simlab/Types.hpp"
#include "simlab/Causal.hpp"
#include "simlab/render/Snapshot.hpp"   // ViewRect

namespace simlab { class World; struct Entity; }
namespace simlab::render { struct PatronJournal; }

namespace simlab::render {

// category bits for the feed filters
enum NarrCat : std::uint8_t {
    kCatCombat     = 1u << 0,
    kCatSurvival   = 1u << 1,
    kCatDiscovery  = 1u << 2,   // craft formulas, coined words, verified truths
    kCatSociety    = 1u << 3,   // law, trade, ritual, treaty, birth, theft
    kCatOther      = 1u << 4,
    kCatPinned     = 1u << 5,   // involves a player-pinned legend
};
const char* narr_cat_name(std::uint8_t cat);

struct NarrativeLine {
    std::uint64_t seq   = 0;          // monotonic id (callout dedupe)
    std::uint64_t tick  = 0;
    std::string   text;              // composed sentence (slots resolved)
    Vec2          pos{};             // where it happened (camera jump target)
    EntityId      who   = kNoEntity; // primary actor (click -> select)
    EntityId      about = kNoEntity; // secondary participant (target/victim)
    std::uint8_t  cat   = kCatOther;
    bool          callout = false;   // also float above `pos` for a few seconds
    bool          llm   = false;     // text came from NarrativeHost (async Qwen),
                                     // not the deterministic template composer
};

struct ChronicleFeed {
    EventId scanned = 0;                       // ledger cursor (next id to read)
    bool    primed  = false;                   // first scan: skip backlog
    std::uint64_t seq_next = 1;                // next NarrativeLine.seq
    std::deque<NarrativeLine> lines;           // oldest -> newest, capped
    std::set<std::string> seen_verified;       // "<mind>:<key>" dedupe
    std::map<std::uint64_t, std::uint64_t> last_tell;  // actor+rule -> tick (spam throttle)
    std::uint64_t next_mind_scan = 0;          // tick gate for discovery scan
};

class NarrativeComposer {
public:
    // Parse data/narrative_templates.txt. Format (fields split by '|',
    // '#' comments):
    //   key | category | callout | template
    //   key      = verb ("strike"), qualified ("strike:wound" — picked first
    //              when the event carries that qualifier), "spawn:<kind>"
    //              ("spawn:inscription"), or "*" fallback. Multiple lines
    //              per key = variants chosen by hash(tick, key).
    //   category = combat|survival|discovery|society|other ('|' ORs bits)
    //   callout  = 1 floats the line above the actor on the map
    //   template = text with {slots}; see Narrative.cpp for the slot table.
    // Missing/unreadable file -> a small built-in fallback set (still works).
    bool load(const std::string& path);

    // Incrementally narrate new ledger events + newly-verified discoveries.
    // `near` (optional) restricts event narration to that map rect (the camera
    // view); events involving a pinned entity are always narrated.
    // `journal` (optional) enables Rosetta glosses on quoted words.
    // Appends to f.lines (cap kMaxLines) and f.callouts via return? — callouts
    // are flagged on the lines; the viewer drains them.
    void scan(const World& w, ChronicleFeed& f, const ViewRect* near,
              const std::set<EntityId>& pins, const PatronJournal* journal) const;

    // "#42 human" / "#7 berry_bush (wild_berries)" style short descriptor.
    static std::string describe(const World& w, EntityId id);
    // Short human descriptor for minds/entities without ids context.
    static std::string describe_kind(const World& w, const std::string& kind_name,
                                     MaterialId mat);

    std::size_t rule_count() const { return rules_.size(); }

private:
    struct Rule {
        std::string key;
        std::uint8_t cat = kCatOther;
        bool callout = false;
        std::string text;
    };
    std::vector<Rule> rules_;
    std::map<std::string, std::vector<std::size_t>> by_key_;   // key -> rule idx

    // variant pick: hash(tick, key) % count — deterministic per replay
    const Rule* pick(const std::string& key, std::uint64_t tick,
                     bool star_fallback) const;
    const Rule* pick2(const std::string& qualified, const std::string& base,
                      std::uint64_t tick) const;   // qualified -> base -> "*"
    void narrate_event(const World& w, ChronicleFeed& f, const CausalEvent& ev,
                       const ViewRect* near, const std::set<EntityId>& pins,
                       const PatronJournal* journal) const;
    void scan_minds(const World& w, ChronicleFeed& f, const ViewRect* near,
                    const std::set<EntityId>& pins) const;
    // compose + append one line; star_fallback=false restricts to exact keys
    // (spawn:* lines must never degrade to the "*" catch-all)
    void emit(const World& w, ChronicleFeed& f, const std::string& key,
              const std::string& base_key, const CausalEvent& ev,
              EntityId actor, EntityId about, const Entity* spawned, Vec2 pos,
              const PatronJournal* journal, bool pinned, bool star_fallback) const;
};

}  // namespace simlab::render
