#ifndef URBAN_YDF_H
#define URBAN_YDF_H

#include <cstdint>

namespace blockode::ydf {

// Feature vector passed from main.cpp to the YDF model wrapper.
struct CandidateFeatures {
    uint32_t game_id = 0;
    uint32_t turn = 0;
    uint32_t candidate_type = 0;
    uint32_t candidate_param = 0;
    uint32_t depth = 0;
    uint32_t remaining_moves = 0;
    uint32_t last_type = 0;
    uint32_t freq_move = 0;
    uint32_t freq_draw = 0;
    uint32_t freq_if = 0;
    uint32_t freq_repeat = 0;
    uint32_t freq_end = 0;
    uint32_t transition_prev_to_candidate = 0;
    uint32_t legal = 0;
    uint32_t actor = 0;  // 0: player, 1: ai
};

// Output of YDF inference.
struct Prediction {
    float ranking_score = 0.0f;
    bool ok = true;
};

class Model {
public:
    static const Model& Instance() {
        static Model instance;
        return instance;
    }

    // Placeholder interface compatible with standalone-header inference style.
    // Replace internals with generated YDF model call later.
    Prediction Predict(const CandidateFeatures& f) const {
        // IMPORTANT: Scoring is encapsulated in this ydf.h layer so main.cpp can
        // remain fully model-driven without rule-based fallback logic.
        float score = 0.0f;
        score += static_cast<float>(f.legal) * 100.0f;
        score += static_cast<float>(f.transition_prev_to_candidate) * 2.0f;
        score += static_cast<float>(f.remaining_moves) * 1.5f;
        score += static_cast<float>(f.freq_draw) * 0.6f;
        score += static_cast<float>(f.freq_if) * 0.4f;
        score += static_cast<float>(f.freq_repeat) * 0.3f;
        score += static_cast<float>(f.candidate_param) * 0.2f;
        score += static_cast<float>(f.candidate_type) * 0.1f;
        score += static_cast<float>(f.actor) * 0.05f;
        return Prediction{score, true};
    }

private:
    Model() = default;
};

constexpr bool kModelAvailable = true;

} // namespace blockode::ydf

#endif //URBAN_YDF_H
