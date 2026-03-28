#pragma once

#ifndef COBROC_MAIN_H
#define COBROC_MAIN_H

#include "pico/stdlib.h"
#include <cstdint>
#include <cstdio>
#include <array>
#include <algorithm>
#include <cstring>
#include <cstdlib>

#if __has_include("external/ml/coBroc_regression_model.h")
#define COBROC_YDF_STANDALONE_AVAILABLE 1
#include "external/ml/coBroc_regression_model.h"
#else
#define COBROC_YDF_STANDALONE_AVAILABLE 0
#endif

namespace coBroc::ydf {
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
        uint32_t actor = 0;
        uint32_t feedback_penalty = 0;
    };

    struct Prediction {
        float suitability_score = 0.0f;
        bool ok = true;
    };

    namespace detail {
        inline Prediction PredictFallback(const CandidateFeatures& f) {
            float suitability = 0.0f;
            suitability += static_cast<float>(f.legal) * 1.00f;
            suitability += static_cast<float>(f.transition_prev_to_candidate) * 0.03f;
            suitability += static_cast<float>(f.remaining_moves) * 0.01f;
            suitability += static_cast<float>(f.freq_draw) * 0.01f;
            suitability += static_cast<float>(f.freq_if) * 0.01f;
            suitability += static_cast<float>(f.freq_repeat) * 0.01f;
            suitability -= static_cast<float>(f.feedback_penalty) * 0.40f;
            suitability = std::max(0.0f, suitability);
            return Prediction{suitability, true};
        }

#if COBROC_YDF_STANDALONE_AVAILABLE
        inline Prediction PredictStandalone(const CandidateFeatures& f) {
            coBroc_regression_model::Instance in{};
            in.game_id = static_cast<int32_t>(f.game_id);
            in.turn = static_cast<int32_t>(f.turn);
            in.candidate_type = static_cast<int32_t>(f.candidate_type);
            in.candidate_param = static_cast<int32_t>(f.candidate_param);
            in.depth = static_cast<int32_t>(f.depth);
            in.remaining_moves = static_cast<int32_t>(f.remaining_moves);
            in.last_type = static_cast<int32_t>(f.last_type);
            in.freq_move = static_cast<int32_t>(f.freq_move);
            in.freq_draw = static_cast<int32_t>(f.freq_draw);
            in.freq_if = static_cast<int32_t>(f.freq_if);
            in.freq_repeat = static_cast<int32_t>(f.freq_repeat);
            in.freq_end = static_cast<int32_t>(f.freq_end);
            in.transition_prev_to_candidate = static_cast<int32_t>(f.transition_prev_to_candidate);
            in.legal = static_cast<int32_t>(f.legal);
            in.actor = static_cast<int32_t>(f.actor);
            in.feedback_penalty = static_cast<int32_t>(f.feedback_penalty);
            return Prediction{coBroc_regression_model::Predict(in), true};
        }
#endif
    }

    class Model {
    public:
        static const Model& Instance() {
            static Model instance;
            return instance;
        }

        static Prediction Predict(const CandidateFeatures& f) {
#if COBROC_YDF_STANDALONE_AVAILABLE
            return detail::PredictStandalone(f);
#endif
            return detail::PredictFallback(f);
        }

    private:
        Model() = default;
    };

    constexpr bool kModelAvailable = true;
#if COBROC_YDF_STANDALONE_AVAILABLE
    constexpr const char* kBackendName = "standalone";
#else
    constexpr const char* kBackendName = "stub";
#endif
}

#define keyA 15
#define keyB 17
#define keyX 19
#define keyY 21
#define keyUp 2
#define keyDown 18
#define keyLeft 16
#define keyRight 20
#define keyCtrl 3

constexpr uint16_t LCD_REFRESH_DELAY_MS = 50;
int LCD();

#endif // COBROC_MAIN_H
