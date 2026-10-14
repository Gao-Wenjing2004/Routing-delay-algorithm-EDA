#pragma once

#define SRB_V6_ENABLE_MACRO10_FAMILY_PHASE 1
#include "srb_v4.hpp"
#ifdef P2_FAST_MODEL
#include "p2_student_data_fast.hpp"
#else
#include "p2_student_data.hpp"
#endif
#ifdef P2_COMPILED_STUDENT
#include "p2_compiled_student.hpp"
#endif
#ifdef P2_COMPILED_TEACHER
#include "p2_compiled_teacher.hpp"
#endif
#ifdef P16_BLENDED_FOREST
#include "p16_student_data.hpp"
#ifdef P16_COMPILED
#include "p16_compiled.hpp"
#endif
#endif
#ifdef V9_EXACT_MEMORY
#include "exact_memory_data.hpp"
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

#ifdef V9_EXACT_MEMORY
#if defined(_MSC_VER)
#define V9_NOINLINE __declspec(noinline)
#else
#define V9_NOINLINE __attribute__((noinline))
#endif
V9_NOINLINE inline uint32_t apply_exact_memory(
    uint32_t delay,
    const srb_fast::Endpoint& source,
    const srb_fast::Endpoint& target,
    const Environment& environment,
    int source_class,
    int target_class,
    int source_route,
    int target_route) {
    const int displacement = (environment.dx + 8) * 17 + environment.dy + 8;
    const int source_family = srb_v4_family_data::kPortToFamily[source.port];
    const int target_family = srb_v4_family_data::kPortToFamily[target.port];
    const int class_pair = source_class * 17 + target_class;
    const int family_pair = source_family * 216 + target_family;
    const int route_pair = source_route * 161 + target_route;
    const int correction_q15 =
        v9_exact_memory::kClassDisplacement[class_pair * 289 + displacement] +
        v9_exact_memory::kFamilyPair[family_pair] +
        v9_exact_memory::kRoutePair[route_pair];
    return rounded_delay(
        static_cast<double>(delay) * (1.0 + v9_exact_memory::kScale * correction_q15));
}
#undef V9_NOINLINE
#endif

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

    bool parse_spec(srb_fast::Slice from,
                    srb_fast::Slice to,
                    srb_fast::Endpoint& source,
                    srb_fast::Endpoint& target) const {
        return parser_.parse_endpoint(from, source) && parser_.parse_endpoint(to, target);
    }

#ifdef P24_DEFAULT_SUBMISSION_FAST_PATH
    bool predict_endpoints_base(const srb_fast::Endpoint& source,
                                const srb_fast::Endpoint& target,
                                srb_v4::Prediction& prediction) {
#ifdef P38_FAST_SUBMISSION_PREDICT
        return base_.predict_endpoints_submission_fast(source, target, prediction);
#else
        return base_.predict_endpoints(source, target, prediction);
#endif
    }
#endif

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
#ifndef P18_FAST_BASE
        prediction.source_class = p2_student_data::kSourceClass[source.port];
        prediction.target_class = p2_student_data::kTargetClass[target.port];
        prediction.source_route = p2_student_data::kSourceRoute[source.port];
        prediction.target_route = p2_student_data::kTargetRoute[target.port];
#endif
        prediction.base_delay = base_prediction.delay;
        if (base_prediction.delay == 0) {
            prediction.teacher_delay = 0;
            prediction.delay = 0;
            prediction.uncertainty = 0.0f;
            return true;
        }

#ifdef P18_FAST_BASE
        // P18 deliberately omits all learned forests and residual memories.
        // The packed Atlas selector only needs geometry and the already
        // computed Block intersection count, so avoid constructing 42 unused
        // features and scanning Gap prefixes on every one of 10^9 rows.
        {
        Environment environment;
        environment.dx = static_cast<int>(target.x) - source.x;
        environment.dy = static_cast<int>(target.y) - source.y;
        environment.ax = std::abs(environment.dx);
        environment.ay = std::abs(environment.dy);
        environment.cheb = base_prediction.chebyshev;
        environment.direction = base_prediction.direction;
        environment.block_mask = base_prediction.block_mask;
        environment.block_count = base_prediction.intersected_blocks;
        prediction.environment = environment;
        prediction.teacher_delay = base_prediction.delay;
        prediction.delay = base_prediction.delay;
        prediction.uncertainty = 0.0f;
        return true;
        }
#endif

        const Environment environment = classify(source, target, base_prediction);
        prediction.environment = environment;
        float features[42];
        build_features(source, target, environment, base_prediction.delay, features);
        float teacher_correction = 0.0f;
#ifndef P2_PROFILE_SKIP_TEACHER
        if (environment.cheb <= 72 && environment.block_count == 0) {
#ifdef P2_COMPILED_TEACHER
            teacher_correction = p2_compiled::evaluate_teacher(features);
#else
            teacher_correction = evaluate_forest(
                p2_student_data::kTeacherNodes,
                p2_student_data::kTeacherRoots,
                p2_student_data::kTeacherMasks,
                features);
#endif
        }
#endif
        const uint32_t teacher_delay = rounded_delay(
            static_cast<double>(base_prediction.delay) * (1.0 + teacher_correction));
        prediction.teacher_delay = teacher_delay;
        update_base_features(teacher_delay, features);
        const float student_correction =
#ifdef P2_PROFILE_SKIP_STUDENT
            0.0f;
#elif defined(P2_COMPILED_STUDENT)
            p2_compiled::evaluate_student(features);
#else
            evaluate_forest(
            p2_student_data::kStudentNodes,
            p2_student_data::kStudentRoots,
            p2_student_data::kStudentMasks,
            features);
#endif
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
#ifdef V9_EXACT_MEMORY
        // Three additive tables distilled from independently generated exact
        // Dijkstra labels.  Training/validation is split by the complete source
        // endpoint, so translations from a held-out source must generalize.
        if (environment.cheb <= 8) {
            prediction.delay = apply_exact_memory(
                prediction.delay, source, target, environment,
                prediction.source_class, prediction.target_class,
                prediction.source_route, prediction.target_route);
        }
#endif
#ifdef P16_BLENDED_FOREST
        // P16 is trained on the complete pre-Atlas P8 output.  Applying it at
        // the end preserves that feature contract; P14 then overrides this
        // value whenever its packed exact lookup completes.
        update_base_features(prediction.delay, features);
        const float p16_correction =
#ifdef P16_COMPILED
            p16_compiled::evaluate(features);
#else
            evaluate_forest(p16_data::kNodes, p16_data::kRoots, p16_data::kMasks, features);
#endif
        prediction.delay = rounded_delay(
            static_cast<double>(prediction.delay) * (1.0 + p16_correction));
#endif
        const float scale = static_cast<float>(std::max<uint32_t>(prediction.delay, 1));
        prediction.uncertainty =
            (std::abs(static_cast<float>(teacher_delay) - base_prediction.delay) +
             std::abs(static_cast<float>(prediction.delay) - teacher_delay)) / scale;
        return true;
    }

private:
    static Environment classify(const srb_fast::Endpoint& source,
                                const srb_fast::Endpoint& target,
                                const srb_v4::Prediction& base_prediction) {
        Environment result;
        result.dx = static_cast<int>(target.x) - source.x;
        result.dy = static_cast<int>(target.y) - source.y;
        result.ax = std::abs(result.dx);
        result.ay = std::abs(result.dy);
        result.cheb = base_prediction.chebyshev;
        result.direction = base_prediction.direction;
        result.boundary = std::min({
            static_cast<int>(source.x), static_cast<int>(source.y),
            static_cast<int>(target.x), static_cast<int>(target.y),
            119 - static_cast<int>(source.x), 549 - static_cast<int>(source.y),
            119 - static_cast<int>(target.x), 549 - static_cast<int>(target.y),
        });
        result.gap_delay =
            std::abs(static_cast<int>(srb_fast_data::kXGapPrefix[target.x]) -
                     static_cast<int>(srb_fast_data::kXGapPrefix[source.x])) +
            std::abs(static_cast<int>(srb_fast_data::kYGapPrefix[target.y]) -
                     static_cast<int>(srb_fast_data::kYGapPrefix[source.y]));
        result.vertical_gaps =
            std::abs(static_cast<int>(srb_v4_data::kXGapCountPrefix[target.x]) -
                     static_cast<int>(srb_v4_data::kXGapCountPrefix[source.x]));
        result.horizontal_gaps =
            std::abs(static_cast<int>(srb_v4_data::kYGapCountPrefix[target.y]) -
                     static_cast<int>(srb_v4_data::kYGapCountPrefix[source.y]));
        result.block_mask = base_prediction.block_mask;
        result.block_count = base_prediction.intersected_blocks;
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
