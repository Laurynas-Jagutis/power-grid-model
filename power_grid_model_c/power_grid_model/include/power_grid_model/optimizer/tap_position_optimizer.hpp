// SPDX-FileCopyrightText: Contributors to the Power Grid Model project <powergridmodel@lfenergy.org>
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

#include "base_optimizer.hpp"

#include "../auxiliary/dataset.hpp"
#include "../auxiliary/meta_gen/update.hpp"
#include "../common/enum.hpp"
#include "../component/three_winding_transformer.hpp"
#include "../component/transformer.hpp"
#include "../component/transformer_tap_regulator.hpp"
#include "../main_core/output.hpp"

#include <boost/graph/compressed_sparse_row_graph.hpp>
#include <functional>
#include <queue>
#include <vector>

namespace power_grid_model::optimizer {
namespace tap_position_optimizer {

namespace detail = power_grid_model::optimizer::detail;

template <typename T>
concept transformer_c = std::derived_from<std::remove_cvref_t<T>, Transformer> ||
                        std::derived_from<std::remove_cvref_t<T>, ThreeWindingTransformer>;

using TrafoGraphIdx = Idx;
using EdgeWeight = int64_t;
using WeightedTrafo = std::pair<Idx2D, EdgeWeight>;
using WeightedTrafoList = std::vector<WeightedTrafo>;
using RankedTransformerGroups = std::vector<std::vector<Idx2D>>;
constexpr auto infty = std::numeric_limits<Idx>::max();

struct TrafoGraphVertex {
    bool is_source{}; // is_source = true if the vertex is a source
};

struct TrafoGraphEdge {
    Idx2D pos{};
    EdgeWeight weight{};
};

// TODO(mgovers): investigate whether this really is the correct graph structure
using TransformerGraph = boost::compressed_sparse_row_graph<boost::directedS, TrafoGraphVertex, TrafoGraphEdge,
                                                            boost::no_property, TrafoGraphIdx, TrafoGraphIdx>;

template <main_core::main_model_state_c State>
inline auto build_transformer_graph(State const& /*state*/) -> TransformerGraph {
    // TODO(nbharambe): implement
    return {};
}

inline auto process_edges_dijkstra(Idx v, std::vector<EdgeWeight>& edge_weight, std::vector<Idx2D>& edge_pos,
                                   TransformerGraph const& graph) -> void {
    using TrafoGraphElement = std::pair<EdgeWeight, TrafoGraphIdx>;
    std::priority_queue<TrafoGraphElement, std::vector<TrafoGraphElement>, std::greater<>> pq;
    edge_weight[v] = 0;
    edge_pos[v] = {v, v};
    pq.push({0, v});

    while (!pq.empty()) {
        auto [dist, u] = pq.top();
        pq.pop();

        if (dist != edge_weight[u]) {
            continue;
        }

        for (auto e : boost::make_iterator_range(boost::out_edges(u, graph))) {
            auto v = boost::target(e, graph);
            const EdgeWeight weight = graph[e].weight;

            if (edge_weight[u] + weight < edge_weight[v]) {
                edge_weight[v] = edge_weight[u] + weight;
                edge_pos[v] = graph[e].pos;
                pq.push({edge_weight[v], v});
            }
        }
    }
}

// Step 2: Initialize the rank of all vertices (transformer nodes) as infinite (INT_MAX)
// Step 3: Loop all the connected sources (status == 1)
//      a. Perform Dijkstra shortest path algorithm from the vertex with that source.
//         This is to determine the shortest path of all vertices to this particular source.
inline auto get_edge_weights(TransformerGraph const& graph) -> WeightedTrafoList {
    std::vector<EdgeWeight> edge_weight(boost::num_vertices(graph), infty);
    std::vector<Idx2D> edge_pos(boost::num_vertices(graph));

    for (auto v : boost::make_iterator_range(boost::vertices(graph))) {
        if (graph[v].is_source) {
            process_edges_dijkstra(v, edge_weight, edge_pos, graph);
        }
    }

    WeightedTrafoList result;
    for (size_t i = 0; i < edge_weight.size(); ++i) {
        result.emplace_back(edge_pos[i], edge_weight[i]);
    }

    return result;
}

// Step 4: Loop all transformers with automatic tap changers, including the transformers which are not
//         fully connected
//   a.Rank of the transformer <-
//       i. Infinity(INT_MAX), if tap side of the transformer is disconnected.
//          The transformer regulation should be ignored
//       ii.Rank of the vertex at the tap side of the transformer, if tap side of the transformer is connected
inline auto transformer_disconnected(Idx2D const& /*pos*/) -> bool {
    // <TODO: jguo> waiting for the functionalities in step 1 to be implemented
    return false;
}

inline auto rank_transformers(WeightedTrafoList const& w_trafo_list) -> RankedTransformerGroups {
    auto sorted_trafos = w_trafo_list;

    for (auto& trafo : sorted_trafos) {
        if (transformer_disconnected(trafo.first)) {
            trafo.second = infty;
        }
    }

    std::sort(sorted_trafos.begin(), sorted_trafos.end(),
              [](const WeightedTrafo& a, const WeightedTrafo& b) { return a.second < b.second; });

    RankedTransformerGroups groups;
    for (const auto& trafo : sorted_trafos) {
        if (groups.empty() || groups.back().back().pos != trafo.second) {
            groups.push_back(std::vector<Idx2D>{trafo.first});
        } else {
            groups.back().push_back(trafo.first);
        }
    }
    return groups;
}

template <main_core::main_model_state_c State>
inline auto rank_transformers(State const& state) -> RankedTransformerGroups {
    return rank_transformers(get_edge_weights(build_transformer_graph(state)));
}

template <typename StateCalculator, typename StateUpdater_, typename State_>
    requires main_core::component_container_c<typename State_::ComponentContainer, Transformer> &&
             main_core::component_container_c<typename State_::ComponentContainer, ThreeWindingTransformer> &&
             detail::steady_state_calculator_c<StateCalculator, State_> &&
             std::invocable<std::remove_cvref_t<StateUpdater_>, ConstDataset const&>
class TapPositionOptimizer : public detail::BaseOptimizer<StateCalculator, State_> {
  public:
    using Base = detail::BaseOptimizer<StateCalculator, State_>;
    using typename Base::Calculator;
    using typename Base::ResultType;
    using typename Base::State;
    using StateUpdater = StateUpdater_;

  private:
    using ComponentContainer = typename State::ComponentContainer;

    class TransformerRef {
      public:
        TransformerRef() = default;

        TransformerRef(std::reference_wrapper<const Transformer> transformer, Idx2D const& index, Idx math_index)
            : transformer_{std::move(transformer)}, index_{index}, math_index_{math_index} {}

        TransformerRef(std::reference_wrapper<const ThreeWindingTransformer> three_winding_transformer,
                       Idx2D const& index, Idx math_index)
            : three_winding_transformer_{std::move(three_winding_transformer)},
              index_{index},
              math_index_{math_index} {}

        constexpr auto index() const { return index_; }
        constexpr auto math_index() const { return math_index_; }

        IntS tap_pos() const {
            return apply([](auto const& t) { return t.tap_pos(); });
        }
        IntS tap_min() const {
            return apply([](auto const& t) { return t.tap_min(); });
        }
        IntS tap_max() const {
            return apply([](auto const& t) { return t.tap_max(); });
        }
        bool connected_at_tap_side() const {
            return apply([](auto const& t) { return t.status(t.tap_side()); });
        }
        bool connected_at_control_side(TransformerTapRegulator const& regulator) const {
            return apply([side = regulator.control_side()](auto const& t) {
                if constexpr (std::derived_from<std::remove_cvref_t<decltype(t)>, Transformer>) {
                    return t.status(static_cast<BranchSide>(side));
                }
                if constexpr (std::derived_from<std::remove_cvref_t<decltype(t)>, ThreeWindingTransformer>) {
                    return t.status(static_cast<Branch3Side>(side));
                }
            });
        }

        template <typename Func>
            requires std::invocable<Func, Transformer const&> && std::invocable<Func, ThreeWindingTransformer const&>
        auto apply(Func const& func) const {
            assert(transformer_ || three_winding_transformer_);

            if (transformer_) {
                return func(transformer_->get());
            }
            if (three_winding_transformer_) {
                return func(three_winding_transformer_->get());
            }

            throw Unreachable{"TransformerRef::apply",
                              "This function should only be called on actual transformer references"};
        }

      private:
        static bool connected_at_side(transformer_c auto const& t, ControlSide side) {}

        Idx2D index_;
        Idx math_index_;

        std::optional<std::reference_wrapper<const Transformer>> transformer_;
        std::optional<std::reference_wrapper<const ThreeWindingTransformer>> three_winding_transformer_;
    };

    struct TapRegulatorRef {
        std::reference_wrapper<const TransformerTapRegulator> regulator;
        TransformerRef transformer;
    };

    struct UpdateBuffer {
        std::vector<TransformerUpdate> transformer_update;
        std::vector<ThreeWindingTransformerUpdate> three_winding_transformer_update;
    };

  public:
    TapPositionOptimizer(Calculator calculator, StateUpdater updater, OptimizerStrategy strategy)
        : calculate_{std::move(calculator)}, update_{std::move(updater)}, strategy_{strategy} {}

    auto optimize(State const& state, CalculationMethod method) -> ResultType final {
        auto const order = this->regulator_mapping(state, rank_transformers(state));

        auto const cache = this->cache_state(state, order);
        auto const optimal_output = optimize(state, order, method);
        update_state(cache);

        return optimal_output;
    }

    constexpr auto get_strategy() { return strategy_; }

  private:
    auto optimize(State const& state, std::vector<std::vector<TapRegulatorRef>> const& regulator_order,
                  CalculationMethod method) -> ResultType {
        initialize(state, regulator_order);

        if (auto const result = try_calculation_with_regulation(state, regulator_order, method);
            strategy_ == OptimizerStrategy::any) {
            return result;
        }

        // refine solution
        step_all(state, regulator_order);
        return try_calculation_with_regulation(state, regulator_order, method);
    }

    auto try_calculation_with_regulation(State const& state,
                                         std::vector<std::vector<TapRegulatorRef>> const& regulator_order,
                                         CalculationMethod method) -> ResultType {
        // TODO(mgovers): implement outer loop tap changer
        ResultType result;

        try {
            result = calculate_(state, method);
        } catch (SparseMatrixError) {
            result = calculate_(state, CalculationMethod::linear);
        } catch (IterationDiverge) {
            result = calculate_(state, CalculationMethod::linear);
        }

        bool tap_changed = true;
        while (tap_changed) {
            tap_changed = false;
            UpdateBuffer update_data;

            for (auto const& same_rank_regulators : regulator_order) {
                for (auto const& regulator : same_rank_regulators) {
                    if (regulator.transformer.connected_at_tap_side() &&
                        regulator.transformer.connected_at_control_side(regulator.regulator)) {
                        tap_changed = tap_changed || control_transformer(regulator, state, result, update_data);
                    }
                }
                if (tap_changed) {
                    break;
                }
            }
            if (tap_changed) {
                result = calculate_(state, method);
            }
        }

        return result;
    }

    // * u_measured = u_complex_at_bus_at_controlled_side + z_compensation * i_complex_at_branch_controlled_side
    // * v_measured = abs(u_measured)
    // * If v_measured > u_set + 0.5 * u_band
    //   * If tap_pos == tap_max
    //     * Return false
    //   * Change tap_pos one step towards tap_max
    //   * Return true
    // * If v_measured < u_set - 0.5 * u_band
    //   * If tap_pos == tap_min
    //     * Return false
    //   * Change tap_pos one step towards tap_min
    //   * Return true
    // * Return false
    bool control_transformer(TapRegulatorRef const& regulator, State const& state, ResultType const& math_output,
                             UpdateBuffer& update_data) {
        auto const math_index = regulator.transformer.math_index();
        auto const control = [&math_output, &math_index, &update_data](auto const& transformer) {
            using TransformerType = std::remove_cvref_t<decltype(transformer)>;

            const auto output = main_core::output_result(transformer, math_output, math_index);
            if constexpr (std::derived_from<TransformerType, Branch>) {
            }
        };
        return false;
    }

    void update_state(UpdateBuffer const& update_data) {
        ConstDataset const update_dataset{
            {"transformer", this->get_data_ptr<Transformer>(update_data)},
            {"three_winding_transformer", this->get_data_ptr<ThreeWindingTransformer>(update_data)}};

        update_(update_dataset);
    }

    auto initialize(State const& state, std::vector<std::vector<TapRegulatorRef>> const& regulator_order) {
        using namespace std::string_literals;

        constexpr auto get_max = [](transformer_c auto const& transformer) -> IntS { return transformer.tap_max(); };
        constexpr auto get_min = [](transformer_c auto const& transformer) -> IntS { return transformer.tap_min(); };

        switch (strategy_) {
        case OptimizerStrategy::any:
            break;
        case OptimizerStrategy::global_minimum:
            [[fallthrough]];
        case OptimizerStrategy::local_minimum:
            adjust_voltage(get_min, state, regulator_order);
            break;
        case OptimizerStrategy::global_maximum:
            [[fallthrough]];
        case OptimizerStrategy::local_maximum:
            adjust_voltage(get_max, state, regulator_order);
            break;
        default:
            throw MissingCaseForEnumError{"TapPositionOptimizer::initialize"s, strategy_};
        }
    }

    void step_all(State const& state, std::vector<std::vector<TapRegulatorRef>> const& regulator_order) {
        using namespace std::string_literals;

        constexpr auto one_step_up = [](transformer_c auto const& transformer) -> IntS {
            auto const tap_pos = transformer.tap_pos();
            auto const tap_max = transformer.tap_max();
            return tap_pos < tap_max ? tap_pos + 1 : tap_max;
        };
        constexpr auto one_step_down = [](transformer_c auto const& transformer) -> IntS {
            auto const tap_pos = transformer.tap_pos();
            auto const tap_min = transformer.tap_min();
            return tap_pos > tap_min ? tap_pos - 1 : tap_min;
        };

        switch (strategy_) {
        case OptimizerStrategy::any:
            break;
        case OptimizerStrategy::global_minimum:
            [[fallthrough]];
        case OptimizerStrategy::local_minimum:
            adjust_voltage(one_step_down, state, regulator_order);
            break;
        case OptimizerStrategy::global_maximum:
            [[fallthrough]];
        case OptimizerStrategy::local_maximum:
            adjust_voltage(one_step_up, state, regulator_order);
            break;
        default:
            throw MissingCaseForEnumError{"TapPositionOptimizer::initialize"s, strategy_};
        }
    }

    template <typename Func>
        requires std::invocable<Func, Transformer const&> && std::invocable<Func, ThreeWindingTransformer const&> &&
                 std::same_as<std::invoke_result_t<Func, Transformer const&>, IntS> &&
                 std::same_as<std::invoke_result_t<Func, ThreeWindingTransformer const&>, IntS>
    auto adjust_voltage(Func new_tap_pos, State const& state,
                        std::vector<std::vector<TapRegulatorRef>> const& regulator_order) {
        UpdateBuffer update_data;

        auto const cache_transformer = [&new_tap_pos, &update_data](transformer_c auto const& transformer) {
            auto result = get_nan_update(transformer);
            result.id = transformer.id();
            result.tap_pos = new_tap_pos(transformer);
            get<std::remove_cvref_t<decltype(transformer)>>(update_data).push_back(result);
        };

        for (auto const& sub_order : regulator_order) {
            for (auto const& regulator : sub_order) {
                regulator.transformer.apply(cache_transformer);
            }
        }

        update_state(update_data);
    }

    template <typename T> constexpr static auto& get_component(State const& state, auto const& id_or_index) {
        return state.components.template get_item<T>(id_or_index);
    }

    template <typename T> constexpr static auto get_sequence(State const& state, auto const& id_or_index) {
        return state.components.template get_seq<T>(id_or_index);
    }

    static TransformerTapRegulator const& find_regulator(State const& state, ID regulated_object) {
        auto const regulators = state.components.template iter<TransformerTapRegulator>();

        auto result_it = std::ranges::find_if(regulators, [regulated_object](auto const& regulator) {
            return regulator.regulated_object() == regulated_object;
        });
        assert(result_it != regulators.end());

        return *result_it;
    }

    static TapRegulatorRef regulator_mapping(State const& state, Idx2D const& transformer_index) {
        auto map_regulator = [&state, &transformer_index]<typename T>() {
            static_assert(std::derived_from<T, Branch> || std::derived_from<T, Branch3>);
            using BranchType = std::conditional_t<std::derived_from<T, Branch>, Branch, Branch3>;

            auto const& transformer = get_component<T>(state, transformer_index);
            auto const math_index = get_sequence<BranchType>(state, transformer_index);
            return TapRegulatorRef{.regulator = std::cref(find_regulator(state, transformer.id())),
                                   .transformer = {std::cref(transformer), transformer_index, math_index}};
        };
        if (is_in_group<Transformer>(transformer_index)) {
            return map_regulator.template operator()<Transformer>();
        }
        if (is_in_group<ThreeWindingTransformer>(transformer_index)) {
            return map_regulator.template operator()<ThreeWindingTransformer>();
        }
        throw Unreachable{"TapPositionOptimizer::regulator_mapping", "Ranked transformers are regulated"};
    }

    static auto regulator_mapping(State const& state, std::vector<Idx2D> const& order) {
        std::vector<TapRegulatorRef> result;

        for (auto const& index : order) {
            result.push_back(regulator_mapping(state, index));
        }

        return result;
    }

    static auto regulator_mapping(State const& state, RankedTransformerGroups const& order) {
        std::vector<std::vector<TapRegulatorRef>> result;

        for (auto const& sub_order : order) {
            result.push_back(regulator_mapping(state, sub_order));
        }

        return result;
    }

    constexpr static auto component_cache_update(transformer_c auto const& transformer) {
        auto result = get_nan_update(transformer);

        result.id = transformer.id();
        result.tap_pos = transformer.tap_pos();

        return transformer.inverse(result);
    }

    static UpdateBuffer cache_state(State const& state,
                                    std::vector<std::vector<TapRegulatorRef>> const& regulator_order) {
        UpdateBuffer result;

        auto const cache_transformer = [&result](transformer_c auto const& transformer) {
            get<std::remove_cvref_t<decltype(transformer)>>(result).push_back(component_cache_update(transformer));
        };

        for (auto const& same_rank_regulators : regulator_order) {
            for (auto const& regulator_index : same_rank_regulators) {
                regulator_index.transformer.apply(cache_transformer);
            }
        }

        return result;
    }

    template <typename T> static constexpr auto group_count(std::vector<Idx2D> const& indices) {
        return std::ranges::count_if(indices, is_in_group<T>);
    }

    template <typename T> static constexpr auto is_in_group(Idx2D const& index) {
        constexpr auto group_idx = ComponentContainer::template get_type_idx<T>();
        return index.group == group_idx;
    }

    template <transformer_c T> static auto get(UpdateBuffer& update_data) {
        if constexpr (std::derived_from<std::remove_cvref_t<T>, Transformer>) {
            return update_data.transformer_update;
        }
        if constexpr (std::derived_from<std::remove_cvref_t<T>, ThreeWindingTransformer>) {
            return update_data.three_winding_transformer_update;
        }
    }

    template <transformer_c T> static auto const& get(UpdateBuffer const& update_data) {
        if constexpr (std::derived_from<std::remove_cvref_t<T>, Transformer>) {
            return update_data.transformer_update;
        }
        if constexpr (std::derived_from<std::remove_cvref_t<T>, ThreeWindingTransformer>) {
            return update_data.three_winding_transformer_update;
        }
    }

    template <typename T>
        requires requires(UpdateBuffer const& u) {
                     { get<T>(u).data() } -> std::convertible_to<void const*>;
                     { get<T>(u).size() } -> std::convertible_to<Idx>;
                 }
    static auto const get_data_ptr(UpdateBuffer const& update_buffer) {
        auto const& data = get<T>(update_buffer);
        return ConstDataPointer{data.data(), static_cast<Idx>(data.size())};
    };

    static auto get_nan_update(auto const& component) {
        return meta_data::get_component_nan<typename std::remove_cvref_t<decltype(component)>::UpdateType>{}();
    }

    Calculator calculate_;
    StateUpdater update_;
    OptimizerStrategy strategy_;
};

} // namespace tap_position_optimizer

template <typename StateCalculator, typename StateUpdater, typename State>
using TapPositionOptimizer = tap_position_optimizer::TapPositionOptimizer<StateCalculator, StateUpdater, State>;

} // namespace power_grid_model::optimizer
