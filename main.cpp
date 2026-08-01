#include "main.h"

#include <random>

extern "C" {
#include "Infrared.h"
#include "LCD_1in3.h"
#include "DEV_Config.h"
#include "lvgl.h"
#include <stdio.h>
}

#include <array>
#include <algorithm>
#include <cstdio>
#include <cstdlib>

#include "pico/multicore.h"
#include "pico/util/queue.h"

using namespace coBroc::core;

// ── Hardware abstraction ────────────────────────────────────────────────

namespace hardware {
    bool keyPressed(uint8_t key) {
        return DEV_Digital_Read(key) == 0;
    }

    void initInfraredPins() {
        SET_Infrared_PIN(keyA);
        SET_Infrared_PIN(keyB);
        SET_Infrared_PIN(keyX);
        SET_Infrared_PIN(keyY);
        SET_Infrared_PIN(keyLeft);
        SET_Infrared_PIN(keyRight);
        SET_Infrared_PIN(keyUp);
        SET_Infrared_PIN(keyDown);
        SET_Infrared_PIN(keyCtrl);
    }
}

// ── Pico-specific YDF worker (multicore + queues) ──────────────────────

namespace {
    constexpr uint8_t YDF_WORKER_BATCH_CAPACITY = AI_MAX_PREDICT_CANDIDATES;

    enum class YdfWorkerCommand : uint8_t {
        PredictBatch = 1,
        Shutdown = 2,
    };

    struct YdfWorkerRequest {
        YdfWorkerCommand command = YdfWorkerCommand::PredictBatch;
        uint8_t batch_count = 0;
        std::array<coBroc::ydf::CandidateFeatures, YDF_WORKER_BATCH_CAPACITY> features{};
    };

    struct YdfWorkerResponse {
        uint8_t batch_count = 0;
        std::array<coBroc::ydf::Prediction, YDF_WORKER_BATCH_CAPACITY> predictions{};
    };

    queue_t g_ydf_request_queue{};
    queue_t g_ydf_response_queue{};
    bool g_ydf_worker_running = false;

    void ydfWorkerCore1Main() {
        while (true) {
            YdfWorkerRequest req{};
            queue_remove_blocking(&g_ydf_request_queue, &req);
            if (req.command == YdfWorkerCommand::Shutdown) break;

            YdfWorkerResponse res{};
            res.batch_count = req.batch_count;
            for (uint8_t i = 0; i < req.batch_count; i++) {
                res.predictions[i] = coBroc::ydf::Model::Predict(req.features[i]);
            }
            queue_add_blocking(&g_ydf_response_queue, &res);
        }
    }

    void startYdfWorker() {
        if (g_ydf_worker_running) return;
        queue_init(&g_ydf_request_queue, sizeof(YdfWorkerRequest), 1);
        queue_init(&g_ydf_response_queue, sizeof(YdfWorkerResponse), 1);
        multicore_reset_core1();
        multicore_launch_core1(ydfWorkerCore1Main);
        g_ydf_worker_running = true;
    }

    void stopYdfWorker() {
        if (!g_ydf_worker_running) return;
        YdfWorkerRequest shutdown_req{};
        shutdown_req.command = YdfWorkerCommand::Shutdown;
        queue_add_blocking(&g_ydf_request_queue, &shutdown_req);
        multicore_reset_core1();
        queue_free(&g_ydf_request_queue);
        queue_free(&g_ydf_response_queue);
        g_ydf_worker_running = false;
    }

    // Pico batch predictor: delegate to core1 worker if available, else sync
    void picoBatchPredictor(const ProgramState& s,
                             std::array<AICandidate, AI_CANDIDATE_SLOTS>& candidates,
                             const std::array<uint8_t, AI_CANDIDATE_SLOTS>& indices,
                             uint8_t index_count) {
        if (!g_ydf_worker_running || index_count == 0) {
            predictCandidatesSync(s, candidates, indices, index_count);
            return;
        }

        YdfWorkerRequest req{};
        req.command = YdfWorkerCommand::PredictBatch;
        req.batch_count = index_count;
        for (uint8_t i = 0; i < index_count; i++) {
            req.features[i] = makeYdfFeatures(s, candidates[indices[i]]);
        }
        queue_add_blocking(&g_ydf_request_queue, &req);

        YdfWorkerResponse res{};
        queue_remove_blocking(&g_ydf_response_queue, &res);
        const uint8_t out_count = std::min(req.batch_count, res.batch_count);
        for (uint8_t i = 0; i < out_count; i++) {
            candidates[indices[i]].suitability = res.predictions[i].ok ? res.predictions[i].suitability_score : -1.0f;
        }
        for (uint8_t i = out_count; i < req.batch_count; i++) {
            candidates[indices[i]].suitability = -1.0f;
        }
    }

    // ── Input handlers ──────────────────────────────────────────────────

    bool handlePlayerInput(ProgramState& s) {
        if (hardware::keyPressed(keyB)) {
            cycleBlockType(s);
            sleep_ms(140);
            return true;
        }
        if (hardware::keyPressed(keyX)) {
            cycleParam(s);
            sleep_ms(140);
            return true;
        }
        if (hardware::keyPressed(keyA)) {
            const BlockType t = s.selected_block;
            uint8_t param = blockHasParam(t) ? s.selected_param : 0;
            if (blockHasParam(t)) {
                const uint8_t minp = minParamForBlock(t);
                const uint8_t maxp = maxParamForBlock(t);
                if (param < minp || param > maxp) param = minp;
            }
            if (addStepToProgram(s, t, param, false)) {
                // 盤面満杯、または END で最後のスコープを閉じてプログラム完成
                // (トップレベルでの DRAW などは完成ではない)
                const bool completed = (t == BlockType::End && s.syntax_depth == 0);
                s.turn = (s.move_count >= MAX_MOVES || completed)
                             ? TurnState::SelectInputColor
                             : TurnState::AITurn;
            }
            sleep_ms(160);
            return true;
        }
        if (hardware::keyPressed(keyY)) {
            s.turn = TurnState::SelectInputColor;
            sleep_ms(180);
            return true;
        }
        return false;
    }

    bool handleColorSelectInput(ProgramState& s) {
        if (hardware::keyPressed(keyX)) {
            s.run_input_color = s.run_input_color >= COLOR_PARAM_MAX ? COLOR_PARAM_MIN : static_cast<uint8_t>(s.run_input_color + 1);
            sleep_ms(140);
            return true;
        }
        if (hardware::keyPressed(keyY)) {
            s.turn = TurnState::RunProgram;
            sleep_ms(180);
            return true;
        }
        if (hardware::keyPressed(keyA)) {
            s.turn = TurnState::PlayerTurn;
            sleep_ms(180);
            return true;
        }
        return false;
    }

    // ── LVGL UI ─────────────────────────────────────────────────────────

    namespace ui {
        constexpr uint16_t LVGL_DRAW_BUFFER_LINES = 24;
        constexpr size_t LVGL_DRAW_BUFFER_PIXEL_COUNT = static_cast<size_t>(LCD_WIDTH) * LVGL_DRAW_BUFFER_LINES;
        constexpr size_t LCD_TX_LINE_BUFFER_SIZE = static_cast<size_t>(LCD_WIDTH) * sizeof(lv_color_t);

        struct LvglUiContext {
            std::array<lv_color_t, LVGL_DRAW_BUFFER_PIXEL_COUNT> lvgl_draw_pixels{};
            std::array<uint8_t, LCD_TX_LINE_BUFFER_SIZE> lcd_tx_line_buffer{};
            std::array<lv_point_t, (MAX_MOVES + 2) * 8> flow_line_points{};
            size_t flow_line_point_count = 0;
            lv_disp_draw_buf_t lvgl_draw_buf{};
            lv_disp_drv_t lvgl_disp_drv{};
        };

        LvglUiContext g_lvgl_ui{};

        constexpr lv_coord_t UI_MARGIN = 6;
        constexpr lv_coord_t UI_CARD_WIDTH = static_cast<lv_coord_t>(LCD_WIDTH - UI_MARGIN * 2);
        constexpr lv_coord_t UI_HEADER_HEIGHT = 58;
        constexpr lv_coord_t UI_LIST_Y = static_cast<lv_coord_t>(UI_MARGIN + UI_HEADER_HEIGHT + 4);
        constexpr lv_coord_t UI_LIST_HEIGHT = static_cast<lv_coord_t>(LCD_HEIGHT - UI_LIST_Y - UI_MARGIN);
        constexpr lv_coord_t FLOW_NODE_HEIGHT = 34;
        constexpr lv_coord_t FLOW_NODE_WIDTH = static_cast<lv_coord_t>(UI_CARD_WIDTH - 28);
        constexpr lv_coord_t FLOW_NODE_X = static_cast<lv_coord_t>((UI_CARD_WIDTH - FLOW_NODE_WIDTH) / 2);
        constexpr lv_coord_t FLOW_NODE_GAP = 12;
        constexpr lv_coord_t FLOW_TOP_PADDING = 8;

        lv_color_t lvColorFromRgb565Fast(uint16_t rgb565) {
            const uint8_t r = static_cast<uint8_t>(((rgb565 >> 11) & 0x1F) * 255 / 31);
            const uint8_t g = static_cast<uint8_t>(((rgb565 >> 5) & 0x3F) * 255 / 63);
            const uint8_t b = static_cast<uint8_t>((rgb565 & 0x1F) * 255 / 31);
            return lv_color_make(r, g, b);
        }

        lv_color_t blockAccentColor(BlockType t) {
            switch (t) {
                case BlockType::Move:   return lv_color_hex(0x2F80ED);
                case BlockType::Draw:   return lv_color_hex(0x14A44D);
                case BlockType::If:     return lv_color_hex(0xA95DF5);
                case BlockType::Else:   return lv_color_hex(0xE056FD);
                case BlockType::Repeat: return lv_color_hex(0xF39C12);
                case BlockType::End:    return lv_color_hex(0x6C757D);
                default:                return lv_color_hex(0x343A40);
            }
        }

        lv_color_t flowColor(const ProgramStep& step, uint8_t flow_index) {
            if (flow_index == 0) return lv_color_hex(0x16A34A);
            return blockAccentColor(step.type);
        }

        void styleCard(lv_obj_t* obj, lv_color_t bg, lv_color_t border) {
            lv_obj_set_style_bg_color(obj, bg, 0);
            lv_obj_set_style_bg_opa(obj, LV_OPA_COVER, 0);
            lv_obj_set_style_border_color(obj, border, 0);
            lv_obj_set_style_border_width(obj, 1, 0);
            lv_obj_set_style_radius(obj, 6, 0);
            lv_obj_set_style_pad_all(obj, 4, 0);
        }

        void createColorSwatch(lv_obj_t* parent, lv_coord_t x, lv_coord_t y, uint8_t param, lv_coord_t size) {
            auto* swatch = lv_obj_create(parent);
            lv_obj_set_size(swatch, size, size);
            lv_obj_set_pos(swatch, x, y);
            lv_obj_set_style_radius(swatch, LV_RADIUS_CIRCLE, 0);
            lv_obj_set_style_bg_color(swatch, lvColorFromRgb565Fast(paintColorByParam(param)), 0);
            lv_obj_set_style_bg_opa(swatch, LV_OPA_COVER, 0);
            lv_obj_set_style_border_width(swatch, 1, 0);
            lv_obj_set_style_border_color(swatch, lv_color_hex(0x222222), 0);
            lv_obj_set_style_pad_all(swatch, 0, 0);
            lv_obj_clear_flag(swatch, LV_OBJ_FLAG_SCROLLABLE);
        }

        lv_point_t* reserveLinePoints(size_t count) {
            if (g_lvgl_ui.flow_line_point_count + count > g_lvgl_ui.flow_line_points.size()) return nullptr;
            auto* ptr = &g_lvgl_ui.flow_line_points[g_lvgl_ui.flow_line_point_count];
            g_lvgl_ui.flow_line_point_count += count;
            return ptr;
        }

        void styleFlowLine(lv_obj_t* line, lv_color_t color, uint8_t width) {
            lv_obj_set_style_line_color(line, color, 0);
            lv_obj_set_style_line_width(line, width, 0);
            lv_obj_set_style_line_opa(line, LV_OPA_COVER, 0);
            lv_obj_set_style_line_rounded(line, true, 0);
        }

        void drawFlowConnector(lv_obj_t* parent, lv_coord_t x, lv_coord_t top, lv_coord_t bottom) {
            auto* pts = reserveLinePoints(2);
            if (pts == nullptr) return;
            pts[0] = {x, top};
            pts[1] = {x, bottom};
            auto* line = lv_line_create(parent);
            lv_line_set_points(line, pts, 2);
            styleFlowLine(line, lv_color_hex(0x7A8795), 2);
            lv_obj_clear_flag(line, LV_OBJ_FLAG_SCROLLABLE);
        }

        void addTriangle(lv_obj_t* parent, lv_coord_t x, lv_coord_t y, lv_coord_t size, lv_color_t color, uint8_t width) {
            auto* pts = reserveLinePoints(4);
            if (pts == nullptr) return;
            const lv_coord_t max = static_cast<lv_coord_t>(size - 2);
            const lv_coord_t half = static_cast<lv_coord_t>(size / 2);
            pts[0] = {1, max};
            pts[1] = {half, 1};
            pts[2] = {max, max};
            pts[3] = {1, max};
            auto* line = lv_line_create(parent);
            lv_line_set_points(line, pts, 4);
            lv_obj_set_pos(line, x, y);
            styleFlowLine(line, color, width);
            lv_obj_clear_flag(line, LV_OBJ_FLAG_SCROLLABLE);
        }

        void addDiamond(lv_obj_t* parent, lv_coord_t x, lv_coord_t y, lv_coord_t size, lv_color_t color, uint8_t width) {
            auto* pts = reserveLinePoints(5);
            if (pts == nullptr) return;
            const lv_coord_t max = static_cast<lv_coord_t>(size - 2);
            const lv_coord_t half = static_cast<lv_coord_t>(size / 2);
            pts[0] = {half, 1};
            pts[1] = {max, half};
            pts[2] = {half, max};
            pts[3] = {1, half};
            pts[4] = {half, 1};
            auto* line = lv_line_create(parent);
            lv_line_set_points(line, pts, 5);
            lv_obj_set_pos(line, x, y);
            styleFlowLine(line, color, width);
            lv_obj_clear_flag(line, LV_OBJ_FLAG_SCROLLABLE);
        }

        void styleShape(lv_obj_t* obj, lv_coord_t x, lv_coord_t y, lv_coord_t size, lv_color_t color, bool selected, lv_coord_t radius) {
            lv_obj_set_pos(obj, x, y);
            lv_obj_set_size(obj, size, size);
            lv_obj_set_style_radius(obj, radius, 0);
            lv_obj_set_style_bg_color(obj, color, 0);
            lv_obj_set_style_bg_opa(obj, selected ? LV_OPA_30 : LV_OPA_20, 0);
            lv_obj_set_style_border_color(obj, color, 0);
            lv_obj_set_style_border_width(obj, selected ? 3 : 2, 0);
            lv_obj_set_style_pad_all(obj, 0, 0);
            lv_obj_clear_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
        }

        void addNodeSymbol(lv_obj_t* parent, const ProgramStep& step, uint8_t flow_index,
                           lv_color_t color, lv_coord_t x, lv_coord_t y, lv_coord_t size, bool selected) {
            const uint8_t line_width = selected ? 3 : 2;
            if (flow_index == 0) {
                auto* circle = lv_obj_create(parent);
                styleShape(circle, x, y, size, color, selected, LV_RADIUS_CIRCLE);
                return;
            }
            switch (step.type) {
                case BlockType::Move: {
                    auto* rect = lv_obj_create(parent);
                    styleShape(rect, x, y, size, color, selected, 2);
                    break;
                }
                case BlockType::Draw:
                    addTriangle(parent, x, y, size, color, line_width);
                    break;
                case BlockType::If:
                    addDiamond(parent, x, y, size, color, line_width);
                    break;
                case BlockType::Else: {
                    auto* alt = lv_obj_create(parent);
                    styleShape(alt, x, y, size, color, selected, 8);
                    auto* mark = lv_label_create(alt);
                    lv_label_set_text(mark, "E");
                    lv_obj_set_style_text_color(mark, color, 0);
                    lv_obj_center(mark);
                    break;
                }
                case BlockType::Repeat: {
                    auto* loop = lv_obj_create(parent);
                    styleShape(loop, x, y, size, color, selected, 6);
                    auto* loop_mark = lv_label_create(loop);
                    lv_label_set_text(loop_mark, "R");
                    lv_obj_set_style_text_color(loop_mark, color, 0);
                    lv_obj_center(loop_mark);
                    break;
                }
                case BlockType::End:
                default: {
                    auto* end = lv_obj_create(parent);
                    styleShape(end, x, y, size, color, selected, LV_RADIUS_CIRCLE);
                    break;
                }
            }
        }

        void drawMain(const ProgramState& s) {
            g_lvgl_ui.flow_line_point_count = 0;

            auto* scr = lv_scr_act();
            lv_obj_clean(scr);
            lv_obj_set_style_bg_color(scr, lv_color_hex(0xEEF3F9), 0);
            lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);

            const BlockType t = s.selected_block;
            const uint8_t shown_param = blockHasParam(t) ? std::max<uint8_t>(minParamForBlock(t), s.selected_param) : 0;
            const uint8_t param_max = maxParamForBlock(t);

            auto* header = lv_obj_create(scr);
            lv_obj_set_pos(header, UI_MARGIN, UI_MARGIN);
            lv_obj_set_size(header, UI_CARD_WIDTH, UI_HEADER_HEIGHT);
            styleCard(header, lv_color_hex(0xFFFFFF), blockAccentColor(t));
            lv_obj_clear_flag(header, LV_OBJ_FLAG_SCROLLABLE);

            auto* title = lv_label_create(header);
            lv_label_set_text_fmt(title, "SEL:%s  P:%u/%u", blockName(t), shown_param, param_max);
            lv_obj_set_pos(title, 6, 6);

            auto* subtitle = lv_label_create(header);
            lv_label_set_text_fmt(subtitle, "TURN:%s", turnName(s.turn));
            lv_obj_set_pos(subtitle, 6, 24);

            auto* controls = lv_label_create(header);
            lv_label_set_text(controls, "A:add  B:type  X:param  Y:run");
            lv_obj_set_pos(controls, 6, 40);

            if (t == BlockType::Draw || t == BlockType::If)
                createColorSwatch(header, UI_CARD_WIDTH - 28, 18, shown_param, 18);

            auto* flow = lv_obj_create(scr);
            lv_obj_set_pos(flow, UI_MARGIN, UI_LIST_Y);
            lv_obj_set_size(flow, UI_CARD_WIDTH, UI_LIST_HEIGHT);
            styleCard(flow, lv_color_hex(0xFFFFFF), lv_color_hex(0xC5D1DE));
            lv_obj_set_scrollbar_mode(flow, LV_SCROLLBAR_MODE_OFF);
            lv_obj_clear_flag(flow, LV_OBJ_FLAG_SCROLLABLE);

            constexpr lv_coord_t COL_GAP = 8;
            constexpr lv_coord_t INNER_X = 4;
            const lv_coord_t inner_w = static_cast<lv_coord_t>(UI_CARD_WIDTH - INNER_X * 2);
            const lv_coord_t left_w = static_cast<lv_coord_t>((inner_w - COL_GAP) / 2);
            const lv_coord_t right_w = static_cast<lv_coord_t>(inner_w - left_w - COL_GAP);
            const lv_coord_t left_x = INNER_X;
            const lv_coord_t right_x = static_cast<lv_coord_t>(left_x + left_w + COL_GAP);
            const lv_coord_t shape_size = 24;
            const lv_coord_t center_x = static_cast<lv_coord_t>(left_x + left_w / 2);

            auto* divider_pts = reserveLinePoints(2);
            if (divider_pts != nullptr) {
                divider_pts[0] = {static_cast<lv_coord_t>(left_x + left_w + COL_GAP / 2), 4};
                divider_pts[1] = {static_cast<lv_coord_t>(left_x + left_w + COL_GAP / 2), static_cast<lv_coord_t>(UI_LIST_HEIGHT - 6)};
                auto* divider = lv_line_create(flow);
                lv_line_set_points(divider, divider_pts, 2);
                styleFlowLine(divider, lv_color_hex(0xD8E0EA), 1);
                lv_obj_clear_flag(divider, LV_OBJ_FLAG_SCROLLABLE);
            }

            const uint8_t total = flowItemCount(s);
            const uint8_t visible = visibleFlowCount();
            const uint8_t top = flowTopIndex(s);
            const uint8_t end = static_cast<uint8_t>(std::min<uint16_t>(total, static_cast<uint16_t>(top + visible)));
            const uint8_t focus = focusFlowIndex(s);

            bool has_prev = false;
            lv_coord_t prev_bottom = 0;
            for (uint8_t flow_idx = top; flow_idx < end; flow_idx++) {
                const uint8_t row = static_cast<uint8_t>(flow_idx - top);
                const lv_coord_t row_y = static_cast<lv_coord_t>(FLOW_TOP_PADDING + row * (FLOW_NODE_HEIGHT + FLOW_NODE_GAP));
                const lv_coord_t symbol_y = static_cast<lv_coord_t>(row_y + (FLOW_NODE_HEIGHT - shape_size) / 2);
                const lv_coord_t symbol_x = static_cast<lv_coord_t>(center_x - shape_size / 2);

                if (has_prev) drawFlowConnector(flow, center_x, prev_bottom, symbol_y);

                const ProgramStep step = flowStep(s, flow_idx);
                const lv_color_t accent = flowColor(step, flow_idx);
                const bool selected = (flow_idx == focus);
                addNodeSymbol(flow, step, flow_idx, accent, symbol_x, symbol_y, shape_size, selected);

                const lv_coord_t info_y = row_y;
                if (selected) {
                    auto* focus_box = lv_obj_create(flow);
                    lv_obj_set_pos(focus_box, right_x, info_y);
                    lv_obj_set_size(focus_box, right_w, FLOW_NODE_HEIGHT);
                    lv_obj_set_style_radius(focus_box, 4, 0);
                    lv_obj_set_style_pad_all(focus_box, 0, 0);
                    lv_obj_set_style_bg_color(focus_box, lv_color_hex(0xEAF3FF), 0);
                    lv_obj_set_style_bg_opa(focus_box, LV_OPA_COVER, 0);
                    lv_obj_set_style_border_color(focus_box, accent, 0);
                    lv_obj_set_style_border_width(focus_box, 1, 0);
                    lv_obj_clear_flag(focus_box, LV_OBJ_FLAG_SCROLLABLE);
                }

                auto* info_title = lv_label_create(flow);
                if (flow_idx == 0)
                    lv_label_set_text_fmt(info_title, "%sRun", selected ? "> " : "");
                else
                    lv_label_set_text_fmt(info_title, "%s%s", selected ? "> " : "", blockName(step.type));
                lv_obj_set_pos(info_title, static_cast<lv_coord_t>(right_x + 4), static_cast<lv_coord_t>(info_y + 2));

                auto* info_detail = lv_label_create(flow);
                if (flow_idx == 0) {
                    lv_label_set_text(info_detail, "start node");
                } else {
                    const uint8_t param = shownParam(s, step, flow_idx);
                    if (blockHasParam(step.type)) {
                        lv_label_set_text_fmt(info_detail, "param:%u/%u%s", param, maxParamForBlock(step.type), step.from_ai ? " [AI]" : "");
                    } else {
                        lv_label_set_text_fmt(info_detail, "%s", step.from_ai ? "[AI]" : "no param");
                    }
                    if (step.type == BlockType::Draw || step.type == BlockType::If)
                        createColorSwatch(flow, static_cast<lv_coord_t>(right_x + right_w - 12), static_cast<lv_coord_t>(info_y + 11), param, 10);
                }
                lv_obj_set_pos(info_detail, static_cast<lv_coord_t>(right_x + 4), static_cast<lv_coord_t>(info_y + 18));

                prev_bottom = static_cast<lv_coord_t>(symbol_y + shape_size);
                has_prev = true;
            }
        }

        void drawColorSelect(const ProgramState& s) {
            auto* scr = lv_scr_act();
            lv_obj_clean(scr);
            lv_obj_set_style_bg_color(scr, lv_color_hex(0xEEF3F9), 0);
            lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);

            auto* panel = lv_obj_create(scr);
            lv_obj_set_pos(panel, UI_MARGIN, UI_MARGIN);
            lv_obj_set_size(panel, UI_CARD_WIDTH, static_cast<lv_coord_t>(LCD_HEIGHT - UI_MARGIN * 2));
            styleCard(panel, lv_color_hex(0xFFFFFF), lv_color_hex(0xA95DF5));

            auto* title = lv_label_create(panel);
            lv_label_set_text(title, "Select IF input color");
            lv_obj_set_pos(title, 8, 10);

            auto* hint = lv_label_create(panel);
            lv_label_set_text(hint, "X:next  Y:start  A:back");
            lv_obj_set_pos(hint, 8, 32);

            auto* dot_holder = lv_obj_create(panel);
            lv_obj_set_size(dot_holder, 90, 90);
            lv_obj_set_pos(dot_holder, static_cast<lv_coord_t>((UI_CARD_WIDTH - 90) / 2), 62);
            lv_obj_set_style_radius(dot_holder, LV_RADIUS_CIRCLE, 0);
            lv_obj_set_style_bg_color(dot_holder, lv_color_hex(0xF5F7FA), 0);
            lv_obj_set_style_border_color(dot_holder, lv_color_hex(0xCED8E3), 0);
            lv_obj_set_style_border_width(dot_holder, 1, 0);
            lv_obj_set_style_pad_all(dot_holder, 0, 0);

            createColorSwatch(dot_holder, 25, 25, s.run_input_color, 40);

            auto* selected = lv_label_create(panel);
            lv_label_set_text_fmt(selected, "COLOR PARAM: %u", s.run_input_color);
            lv_obj_set_pos(selected, static_cast<lv_coord_t>((UI_CARD_WIDTH - 110) / 2), 164);

            auto* note = lv_label_create(panel);
            lv_label_set_text(note, "IF blocks compare this color.");
            lv_obj_set_pos(note, 8, 190);
        }

        void drawRunPreview(const ProgramState& s) {
            auto* scr = lv_scr_act();
            lv_obj_clean(scr);
            lv_obj_set_style_bg_color(scr, lv_color_hex(0xF3F6FA), 0);
            lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);

            auto* top = lv_obj_create(scr);
            lv_obj_set_pos(top, UI_MARGIN, UI_MARGIN);
            lv_obj_set_size(top, UI_CARD_WIDTH, 36);
            styleCard(top, lv_color_hex(0xFFFFFF), lv_color_hex(0x14A44D));

            auto* title = lv_label_create(top);
            lv_label_set_text_fmt(title, "Run Preview  circles:%u", s.runtime.circle_count);
            lv_obj_set_pos(title, 8, 10);

            constexpr lv_coord_t preview_top = 48;
            constexpr lv_coord_t preview_w = static_cast<lv_coord_t>(UI_CARD_WIDTH);
            constexpr lv_coord_t preview_h = 156;

            auto* preview = lv_obj_create(scr);
            lv_obj_set_pos(preview, UI_MARGIN, preview_top);
            lv_obj_set_size(preview, preview_w, preview_h);
            styleCard(preview, lv_color_hex(0xFFFFFF), lv_color_hex(0xC5D1DE));
            lv_obj_set_style_clip_corner(preview, true, 0);
            lv_obj_set_style_pad_all(preview, 2, 0);
            lv_obj_set_scrollbar_mode(preview, LV_SCROLLBAR_MODE_OFF);

            constexpr uint16_t x_span = RESULT_MAX_X - RESULT_MIN_COORD;
            constexpr uint16_t y_span = RESULT_MAX_Y - RESULT_MIN_COORD;
            constexpr lv_coord_t usable_w = preview_w - 2 * RESULT_RADIUS - 8;
            constexpr lv_coord_t usable_h = preview_h - 2 * RESULT_RADIUS - 8;

            for (uint16_t i = 0; i < s.runtime.circle_count; i++) {
                const auto& c = s.runtime.circles[i];
                const uint8_t color_idx = c.color % COLOR_COUNT;
                const uint16_t rel_x = static_cast<uint16_t>(c.x - RESULT_MIN_COORD);
                const uint16_t rel_y = static_cast<uint16_t>(c.y - RESULT_MIN_COORD);
                const lv_coord_t px = static_cast<lv_coord_t>(4 + (static_cast<uint32_t>(rel_x) * usable_w) / std::max<uint16_t>(1, x_span));
                const lv_coord_t py = static_cast<lv_coord_t>(4 + (static_cast<uint32_t>(rel_y) * usable_h) / std::max<uint16_t>(1, y_span));

                auto* dot = lv_obj_create(preview);
                lv_obj_set_size(dot, static_cast<lv_coord_t>(RESULT_RADIUS * 2), static_cast<lv_coord_t>(RESULT_RADIUS * 2));
                lv_obj_set_pos(dot, px, py);
                lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, 0);
                lv_obj_set_style_bg_color(dot, lvColorFromRgb565Fast(kPaintColors[color_idx]), 0);
                lv_obj_set_style_bg_opa(dot, LV_OPA_COVER, 0);
                lv_obj_set_style_border_width(dot, 1, 0);
                lv_obj_set_style_border_color(dot, lv_color_hex(0x222222), 0);
                lv_obj_set_style_pad_all(dot, 0, 0);
            }

            auto* controls = lv_label_create(scr);
            lv_label_set_text(controls, "A:new game  X:exit");
            lv_obj_set_pos(controls, 12, 212);
        }

        void rebuildScreen(const ProgramState& s) {
            if (s.turn == TurnState::SelectInputColor)      drawColorSelect(s);
            else if (s.turn == TurnState::Finished)          drawRunPreview(s);
            else                                             drawMain(s);
        }

        void lvglFlushCallback(lv_disp_drv_t* drv, const lv_area_t* area, lv_color_t* color_p) {
            auto* ctx = static_cast<LvglUiContext*>(drv->user_data);
            if (ctx == nullptr) { lv_disp_flush_ready(drv); return; }

            const int32_t x1 = std::max<int32_t>(0, area->x1);
            const int32_t y1 = std::max<int32_t>(0, area->y1);
            const int32_t x2 = std::min<int32_t>(LCD_WIDTH - 1, area->x2);
            const int32_t y2 = std::min<int32_t>(LCD_HEIGHT - 1, area->y2);

            if (x2 < x1 || y2 < y1) { lv_disp_flush_ready(drv); return; }

            const int32_t flush_w = x2 - x1 + 1;
            const int32_t flush_h = y2 - y1 + 1;
            const int32_t src_w = area->x2 - area->x1 + 1;
            const int32_t src_x0 = x1 - area->x1;
            const int32_t src_y0 = y1 - area->y1;

            LCD_1IN3_SetWindows(static_cast<UWORD>(x1), static_cast<UWORD>(y1),
                                static_cast<UWORD>(x2 + 1), static_cast<UWORD>(y2 + 1));
            DEV_Digital_Write(LCD_DC_PIN, 1);
            DEV_Digital_Write(LCD_CS_PIN, 0);

            for (int32_t row = 0; row < flush_h; row++) {
                const lv_color_t* src = color_p + (src_y0 + row) * src_w + src_x0;
                auto* tx = ctx->lcd_tx_line_buffer.data();
                for (int32_t col = 0; col < flush_w; col++) {
                    const uint16_t color = src[col].full;
                    tx[col * 2] = static_cast<uint8_t>(color >> 8);
                    tx[col * 2 + 1] = static_cast<uint8_t>(color & 0xFF);
                }
                DEV_SPI_Write_nByte(tx, static_cast<uint32_t>(flush_w * 2));
            }
            DEV_Digital_Write(LCD_CS_PIN, 1);
            lv_disp_flush_ready(drv);
        }

        void initLvglDisplay() {
            lv_init();
            lv_disp_draw_buf_init(&g_lvgl_ui.lvgl_draw_buf, g_lvgl_ui.lvgl_draw_pixels.data(), nullptr,
                                  static_cast<uint32_t>(g_lvgl_ui.lvgl_draw_pixels.size()));
            lv_disp_drv_init(&g_lvgl_ui.lvgl_disp_drv);
            g_lvgl_ui.lvgl_disp_drv.hor_res = LCD_WIDTH;
            g_lvgl_ui.lvgl_disp_drv.ver_res = LCD_HEIGHT;
            g_lvgl_ui.lvgl_disp_drv.flush_cb = lvglFlushCallback;
            g_lvgl_ui.lvgl_disp_drv.draw_buf = &g_lvgl_ui.lvgl_draw_buf;
            g_lvgl_ui.lvgl_disp_drv.user_data = &g_lvgl_ui;
            lv_disp_drv_register(&g_lvgl_ui.lvgl_disp_drv);
        }

        void updateLvglTick(uint32_t elapsed_ms) {
            if (elapsed_ms > 0) lv_tick_inc(elapsed_ms);
        }

        void runLvglTasks() {
            lv_timer_handler();
        }
    } // namespace ui

} // anonymous namespace

// ── Main entry ──────────────────────────────────────────────────────────

int LCD() {
    if (DEV_Module_Init() != 0) return -1;
    DEV_SET_PWM(50);
    printf("1.3inch LCD init...\r\n");
    LCD_1IN3_Init(HORIZONTAL);
    LCD_1IN3_Clear(RGB565_WHITE);
    hardware::initInfraredPins();
    ui::initLvglDisplay();

    // Set Pico batch predictor
    g_predictor_fn = picoBatchPredictor;
    startYdfWorker();

    ProgramState state;
    initProgramState(state);
    state.rng_seed = std::random_device{}();

    bool running = true;
    bool needs_redraw = true;
    uint32_t last_tick_ms = to_ms_since_boot(get_absolute_time());

    while (running) {
        const uint32_t now_ms = to_ms_since_boot(get_absolute_time());
        ui::updateLvglTick(now_ms - last_tick_ms);
        last_tick_ms = now_ms;
        ui::runLvglTasks();

        if (state.turn == TurnState::PlayerTurn) {
            const bool changed = handlePlayerInput(state);
            if (state.move_count >= MAX_MOVES && state.turn == TurnState::PlayerTurn) {
                state.turn = TurnState::SelectInputColor;
                needs_redraw = true;
            }
            if (state.turn != TurnState::PlayerTurn) {
                needs_redraw = true;
                sleep_ms(20);
                continue;
            }
            if (changed || needs_redraw) {
                ui::rebuildScreen(state);
                ui::runLvglTasks();
                needs_redraw = false;
            }
            sleep_ms(12);
            continue;
        }

        if (state.turn == TurnState::SelectInputColor) {
            const bool changed = handleColorSelectInput(state);
            if (state.turn != TurnState::SelectInputColor) {
                needs_redraw = true;
                sleep_ms(20);
                continue;
            }
            if (changed || needs_redraw) {
                ui::rebuildScreen(state);
                ui::runLvglTasks();
                needs_redraw = false;
            }
            sleep_ms(12);
            continue;
        }

        if (state.turn == TurnState::AITurn) {
            performAITurn(state);
            ui::rebuildScreen(state);
            ui::runLvglTasks();
            needs_redraw = false;
            sleep_ms(180);
            continue;
        }

        if (state.turn == TurnState::RunProgram) {
            std::mt19937 rng(state.rng_seed);
            compileAndRun(state, rng);
            state.turn = TurnState::Finished;
            needs_redraw = true;
        }

        if (needs_redraw) {
            ui::rebuildScreen(state);
            ui::runLvglTasks();
            needs_redraw = false;
        }

        if (hardware::keyPressed(keyX)) {
            running = false;
            sleep_ms(180);
        } else if (hardware::keyPressed(keyA)) {
            initProgramState(state);
            state.rng_seed = std::random_device{}();
            needs_redraw = true;
            sleep_ms(180);
        } else {
            sleep_ms(30);
        }
    }

    stopYdfWorker();
    DEV_Module_Exit();
    return 0;
}

int main() {
    stdio_init_all();
    return LCD();
}
