#pragma once
// Patron layer (SDL-free, viewer side): the PLAYER's own knowledge of the
// world — never World Truth. It reads the World (const) and the causal ledger
// incrementally and keeps:
//   * a Field Journal / Rosetta lexicon per tongue: every word the player has
//     heard (speech bubbles of `communicate` events in view) or seen carved
//     (inscriptions), with the player's guessed meaning + grammar tag. A guess
//     is CONFIRMED [ok] only when it matches the word's real concept
//     (concept_key + chronicle of the coined semantic_concept) AND the player
//     witnessed the word in context at least once.
//   * emergent milestones: the first completed occurrence of every cultural
//     verb by a sapient (data-driven: any verb, any law), and the first entity
//     of each culture kind (word, tongue, tool, shelter, inscription, debt).
//   * Resonance: the patron's power budget, earned when the watched tribe
//     lives through strong moments (firsts, births, carvings, rites, treaties)
//     and slowly with time.
//   * pins (legends), free notes.
// Saved as a small text sidecar next to the world save.
#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <vector>
#include "simlab/Types.hpp"

namespace simlab { class World; }

namespace simlab::render {

class FogOfWar;

enum class WordTag : int { None = 0, Noun, Verb, Danger, Sacred, Count };
const char* word_tag_name(WordTag t);

struct JournalWord {
    std::string guess;                 // player's meaning ("" = not guessed)
    WordTag tag = WordTag::None;
    int heard = 0, carved = 0;         // times witnessed spoken / seen carved
    std::uint64_t first_tick = 0;
    std::string truth;                 // concept_key it really names (hidden)
};

struct Milestone {
    std::string key, title;
    std::uint64_t tick = 0;
    Vec2 pos;
    EntityId who = kNoEntity;
    std::int64_t lang = 0;
};

struct SpeechBubble {
    EntityId speaker = kNoEntity;
    std::int64_t lang = 0;
    std::uint64_t tick = 0;
    std::vector<std::string> words;    // rendered phonemes
};

struct PatronJournal {
    std::map<std::int64_t, std::map<std::string, JournalWord>> lexicon;   // tongue -> word
    std::vector<Milestone> milestones;
    std::set<std::string> milestone_keys;       // world firsts + per-tribe firsts
    std::set<EntityId> pins;
    std::string notes;
    std::int64_t resonance = 40;                // 0..resonance_max
    std::int64_t resonance_max = 100;
    std::uint64_t res_tick = 0;                 // last time-regen tick
    EventId scanned = 0;                        // ledger cursor (next id to read)
    std::int64_t tribe = 0;                     // the patron's own people (language_id)
    std::vector<SpeechBubble> bubbles;          // transient (not saved), newest last

    bool save(const std::string& path) const;
    bool load(const std::string& path);
    bool spend(std::int64_t cost) { if (resonance < cost) return false; resonance -= cost; return true; }
};

// Rendered spoken form of a lexical_entry (same codec the codex exports).
std::string word_of_entry(const World& w, EntityId entry);
// Human-readable truth behind a concept key (key + its chronicle, material
// ids resolved to names) — used only to CONFIRM a player's guess.
std::string concept_gloss(const World& w, const std::string& concept_key);
bool guess_confirmed(const World& w, const JournalWord& jw);
// Player-side reading of a word: "guess [ok]" / "guess [?]" / "" unknown.
std::string journal_reading(const World& w, const PatronJournal& j,
                            std::int64_t lang, const std::string& word);
// Words of an inscription in its carver's tongue (concept keys -> words).
std::vector<std::string> inscription_words(const World& w, EntityId stone);

// Incremental scan of new ledger events. `fog` (optional) limits what the
// patron hears/sees to Visible cells; nullptr = everything.
// Returns the number of new milestones.
std::size_t patron_scan(const World& w, PatronJournal& j, const FogOfWar* fog);

}  // namespace simlab::render
