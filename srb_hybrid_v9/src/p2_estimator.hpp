#pragma once

#define SRB_V6_ENABLE_MACRO10_FAMILY_PHASE 1
#include "srb_v4.hpp"
#ifdef P2_FAST_MODEL
#include "p2_student_data_fast.hpp"
#else
#include "p2_student_data.hpp"
#endif

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace p2_runtime {

struct Environment {
    int dx = 0;
    int dy = 0;
    int ax = 0;
    int ay = 0;
    int cheb = 0;
    int direction = 0;
    int boundary = 0;
    int gap_delay = 0;
    int horizontal_gaps = 0;
    int vertical_gaps = 0;
    uint32_t block_mask = 0;
    int block_count = 0;
};

struct DetailedPrediction {
    srb_fast::Endpoint source{};
    srb_fast::Endpoint target{};
    Environment environment{};
    uint32_t base_delay = 0;
    uint32_t teacher_delay = 0;
    uint32_t delay = 0;
    float uncertainty = 0.0f;
    int source_class = 0;
    int target_class = 0;
    int source_route = 0;
    int target_route = 0;
};

inline int positive_mod(int value, int period) {
    const int result = value % period;
    return result < 0 ? result + period : result;
}

inline uint32_t rounded_delay(double value) {
    if (!(value > 0.0)) return 0;
    const double rounded = std::floor(value + 0.5);
    if (rounded >= static_cast<double>(UINT32_MAX)) return UINT32_MAX;
    return static_cast<uint32_t>(rounded);
}

template <std::size_t NodeCount, std::size_t TreeCount, std::size_t MaskCount>
inline float evaluate_forest(
    const p2_student_data::Node (&nodes)[NodeCount],
    const int32_t (&roots)[TreeCount],
    const p2_student_data::CatMask (&masks)[MaskCount],
    const float* features) {
    float result = 0.0f;
    for (std::size_t tree = 0; tree < TreeCount; ++tree) {
        int32_t index = roots[tree];
        while (nodes[index].feature >= 0) {
            const auto& node = nodes[index];
            bool go_left = false;
            if (node.categorical) {
                const int category = static_cast<int>(features[node.feature]);
                if (category >= 0 && category < 256 && node.mask >= 0) {
                    go_left = (masks[node.mask].word[category >> 6] &
                               (UINT64_C(1) << (category & 63))) != 0;
                }
            } else {
                go_left = features[node.feature] <= node.threshold;
            }
            index = go_left ? node.left : node.right;
        }
        result += nodes[index].value;
    }
    return result;
}

class Estimator {
public:
    Estimator()
        : base_("", false, true, false, true,
                srb_v4::ResidualKernel::Family, 0) {}

    bool predict_spec(srb_fast::Slice from, srb_fast::Slice to, uint32_t& delay) {
        DetailedPrediction prediction;
        if (!predict_spec_detailed(from, to, prediction)) return false;
        delay = prediction.delay;
        return true;
    }

    bool predict_spec_detailed(srb_fast::Slice from,
                               srb_fast::Slice to,
                               DetailedPrediction& prediction) {
        srb_fast::Endpoint source, target;
        if (!parser_.parse_endpoint(from, source) || !parser_.parse_endpoint(to, target)) {
            return false;
        }
        prediction.source = source;
        prediction.target = target;
        return predict_endpoints_detailed(source, target, prediction);
    }

    bool predict_endpoints_detailed(const srb_fast::Endpoint& source,
                                    const srb_fast::Endpoint& target,
                                    DetailedPrediction& prediction) {
        srb_v4::Prediction base_prediction;
        if (!base_.predict_endpoints(source, target, base_prediction)) return false;
        prediction.source = source;
        prediction.target = target;
        prediction.source_class = p2_student_data::kSourceClass[source.port];
        prediction.target_class = p2_student_data::kTargetClass[target.port];
        prediction.source_route = p2_student_data::kSourceRoute[source.port];
        prediction.target_route = p2_student_data::kTargetRoute[target.port];
        prediction.base_delay = base_prediction.delay;
        if (base_prediction.delay == 0) {
            prediction.teacher_delay = 0;
            prediction.delay = 0;
            prediction.uncertainty = 0.0f;
            return true;
        }

        const Environment environment = classify(source, target);
        prediction.environment = environment;
        float features[42];
        build_features(source, target, environment, base_prediction.delay, features);
        float teacher_correction = 0.0f;
        if (environment.cheb <= 72 && environment.block_count == 0) {
            teacher_correction = evaluate_forest(
                p2_student_data::kTeacherNodes,
                p2_student_data::kTeacherRoots,
                p2_student_data::kTeacherMasks,
                features);
        }
        const uint32_t teacher_delay = rounded_delay(
            static_cast<double>(base_prediction.delay) * (1.0 + teacher_correction));
        prediction.teacher_delay = teacher_delay;
        update_base_features(teacher_delay, features);
        const float student_correction = evaluate_forest(
            p2_student_data::kStudentNodes,
            p2_student_data::kStudentRoots,
            p2_student_data::kStudentMasks,
            features);
        const uint32_t student_delay = rounded_delay(
            static_cast<double>(teacher_delay) * (1.0 + student_correction));
#ifdef P2_POST_MEMORY
        const int source_family = static_cast<int>(features[18]);
        const int direction = static_cast<int>(features[20]);
        const int remainder10 = static_cast<int>(features[22]);
        const int distance_bin = static_cast<int>(features[29]);
        const int source_family_distance =
            (source_family * 9 + direction) * 18 + distance_bin;
        const int source_family_macro10 = source_family * 100 + remainder10;
        const int direction_remainder10 = direction * 100 + remainder10;
        int32_t post_sum = p2_student_data::kPostMacro8Direction[direction_remainder10];
        if (p2_student_data::kPostSourceFamilyActive[source_family]) {
            post_sum += p2_student_data::kPostSourceFamilyDistance[source_family_distance] +
                        p2_student_data::kPostSourceFamilyMacro8[source_family_macro10];
        }
        const float post_correction = p2_student_data::kPostAlpha *
                                      p2_student_data::kPostScale * post_sum;
        prediction.delay = rounded_delay(static_cast<double>(student_delay) * (1.0 + post_correction));
#else
        prediction.delay = student_delay;
#endif
        const float scale = static_cast<float>(std::max<uint32_t>(prediction.delay, 1));
        prediction.uncertainty =
            (std::abs(static_cast<float>(teacher_delay) - base_prediction.delay) +
             std::abs(static_cast<float>(prediction.delay) - teacher_delay)) / scale;
        return true;
    }

private:
    static Environment classify(const srb_fast::Endpoint& source,
                                const srb_fast::Endpoint& target) {
        Environment result;
        result.dx = static_cast<int>(target.x) - source.x;
        result.dy = static_cast<int>(target.y) - source.y;
        result.ax = std::abs(result.dx);
        result.ay = std::abs(result.dy);
        result.cheb = std::max(result.ax, result.ay);
        result.direction = (result.dx > 0 ? 1 : 0) + (result.dx < 0 ? 2 : 0) +
                           (result.dy > 0 ? 3 : 0) + (result.dy < 0 ? 6 : 0);
        result.boundary = std::min({
            static_cast<int>(source.x), static_cast<int>(source.y),
            static_cast<int>(target.x), static_cast<int>(target.y),
            119 - static_cast<int>(source.x), 549 - static_cast<int>(source.y),
            119 - static_cast<int>(target.x), 549 - static_cast<int>(target.y),
        });
        const int min_x = std::min<int>(source.x, target.x);
        const int max_x = std::max<int>(source.x, target.x);
        const int min_y = std::min<int>(source.y, target.y);
        const int max_y = std::max<int>(source.y, target.y);
        for (const auto& gap : p2_student_data::kGaps) {
            const bool crossed = gap.vertical
                ? (min_x <= gap.site && gap.site < max_x)
                : (min_y <= gap.site && gap.site < max_y);
            if (!crossed) continue;
            result.gap_delay += gap.delay;
            if (gap.vertical) ++result.vertical_gaps;
            else ++result.horizontal_gaps;
        }
        uint32_t bit = 1;
        for (const auto& block : p2_student_data::kBlocks) {
            const bool hit = min_x <= block.right && max_x >= block.left &&
                             min_y <= block.upper && max_y >= block.lower;
            if (hit) {
                result.block_mask |= bit;
                ++result.block_count;
            }
            bit <<= 1;
        }
        return result;
    }

    static void build_features(const srb_fast::Endpoint& source,
                               const srb_fast::Endpoint& target,
                               const Environment& env,
                               uint32_t base,
                               float* f) {
        const int minimum = std::min(env.ax, env.ay);
        const float ratio = static_cast<float>(minimum) / std::max(env.cheb, 1);
        const int source_phase10 = (source.x % 10) * 10 + source.y % 10;
        const int target_phase10 = (target.x % 10) * 10 + target.y % 10;
        const int remainder8 = positive_mod(env.dx, 8) * 8 + positive_mod(env.dy, 8);
        const int remainder10 = positive_mod(env.dx, 10) * 10 + positive_mod(env.dy, 10);
        const int distance_bin = std::min(17, env.cheb / 32);
        const int source_family = srb_v4_family_data::kPortToFamily[source.port];
        const int target_family = srb_v4_family_data::kPortToFamily[target.port];
        const int source_class = p2_student_data::kSourceClass[source.port];
        const int target_class = p2_student_data::kTargetClass[target.port];
        const int source_route = p2_student_data::kSourceRoute[source.port];
        const int target_route = p2_student_data::kTargetRoute[target.port];
        update_base_features(base, f);
        f[1] = static_cast<float>(env.dx) / 600.0f;
        f[2] = static_cast<float>(env.dy) / 600.0f;
        f[3] = static_cast<float>(env.ax) / 600.0f;
        f[4] = static_cast<float>(env.ay) / 600.0f;
        f[5] = static_cast<float>(env.cheb) / 600.0f;
        f[6] = ratio;
        f[7] = static_cast<float>(env.boundary) / 128.0f;
        f[8] = static_cast<float>(env.gap_delay) / 512.0f;
        // P1/P0/V5 auxiliary predictions were deliberately disabled in the
        // selected runtime model after total-score timing analysis.  During
        // training their absent values are represented by the current base.
        f[10] = 0.0f; f[11] = 1.0f;
        f[13] = 0.0f; f[14] = 1.0f;
        f[16] = 0.0f; f[17] = 1.0f;
        f[18] = source_family;
        f[19] = target_family;
        f[20] = env.direction;
        f[21] = remainder8;
        f[22] = remainder10;
        f[23] = source_phase10;
        f[24] = target_phase10;
        f[25] = env.block_mask;
        f[26] = env.block_count;
        f[27] = env.horizontal_gaps;
        f[28] = env.vertical_gaps;
        f[29] = distance_bin;
        f[30] = source.port % 8;
        f[31] = target.port % 8;
        f[32] = static_cast<float>(source.port) / 496.0f;
        f[33] = static_cast<float>(target.port) / 496.0f;
        f[34] = (static_cast<int>(source.port) * 503 + target.port) % 251;
        f[35] = (source_family * 223 + target_family) % 251;
        f[36] = source_class;
        f[37] = target_class;
        f[38] = (source_class * 17 + target_class) % 251;
        f[39] = source_route;
        f[40] = target_route;
        f[41] = (source_route * 163 + target_route) % 251;
    }

    static void update_base_features(uint32_t base, float* f) {
        const float scaled = static_cast<float>(base) / 4096.0f;
        f[0] = scaled;
        f[9] = scaled;
        f[12] = scaled;
        f[15] = scaled;
    }

    srb_fast::FastEstimator parser_;
    srb_v4::Estimator base_;
};

}  // namespace p2_runtime
