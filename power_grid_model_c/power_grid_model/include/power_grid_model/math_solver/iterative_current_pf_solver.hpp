// SPDX-FileCopyrightText: Contributors to the Power Grid Model project <powergridmodel@lfenergy.org>
//
// SPDX-License-Identifier: MPL-2.0

// Check if the name means anything
#pragma once

/*
Iterative Power Flow

Description:
    The algorithm similar to jacobi way of solving linear equations.
    Only I_inj is calculated fresh on each iteration based on latest values of U.
    Linear equation here: I_inj = YU

Prefactorization:
    If the Y bus matrix does not change, then there is no need for factorizing it again to solve linear equations.
    Hence it is done only once in the first iteration and same result is used in subsequent iterations.
    Same factorization is also used in subsequent batches

Steps:
    Initialize U with averaged u_ref, ie source voltage and phase shifts accounted
    Initialize solver
    while maximum deviation > error tolerance
        Calculate I_inj with U of previous iteration as per load/gen types.
        Solve YU = I_inj using prefactorization.
        Find maximum deviation in voltage buses U
        Update U
    Calculate output values from U result

    Initialize solver:
        Source admittance is not included in Y bus matrix here. Include that to complete the Y bus matrix.
        Invalidate prefactorization if parameters change, ie y bus values changes

    Calculating Injected current:
        Initialize I_inj = 0
        For each bus i
            For source on bus i, I_inj_i = y_ref * u_uref
            For Loads on bus i:
                If type is constant PQ: I_inj_i += conj(S_inj_j/U_i)
                If type is constant impedance: I_inj_i += conj((S_inj_j * abs(U_i)^2) / U_i) = conj((S_inj_j) * U_i
                If type is constant current: I_inj_i += conj(S_inj_j*abs(U_i)/U_i)

Nomenclature:
    I_inj : Injected current
    S_inj : Injected power
    Y : Y bus matrix
    U : Bus Voltage
    u_ref : reference voltage for source
    y_ref : Source admittance


*/

#include "block_matrix.hpp"
#include "common_solver_functions.hpp"
#include "iterative_pf_solver.hpp"
#include "sparse_lu_solver.hpp"
#include "y_bus.hpp"

#include "../calculation_parameters.hpp"
#include "../common/common.hpp"
#include "../common/exception.hpp"
#include "../common/three_phase_tensor.hpp"
#include "../common/timer.hpp"

namespace power_grid_model::math_solver {

// hide implementation in inside namespace
namespace iterative_current_pf {

// solver
template <symmetry_tag sym>
class IterativeCurrentPFSolver : public IterativePFSolver<sym, IterativeCurrentPFSolver<sym>> {
  public:
    IterativeCurrentPFSolver(YBus<sym> const& y_bus, std::shared_ptr<MathModelTopology const> const& topo_ptr)
        : IterativePFSolver<sym, IterativeCurrentPFSolver>{y_bus, topo_ptr}, rhs_u_(y_bus.size()) {}

    // Add source admittance to Y bus and set variable for prepared y bus to true
    void initialize_derived_solver(YBus<sym> const& y_bus, PowerFlowInput<sym> const& input, MathOutput<sym>& output) {
        this->prefactorize_linear_lhs(y_bus);
    }

    // Prepare matrix calculates injected current ie. RHS of solver for each iteration.
    void prepare_matrix_and_rhs(YBus<sym> const& y_bus, PowerFlowInput<sym> const& input,
                                ComplexValueVector<sym> const& u) {
        // set rhs to zero for iteration start
        std::fill(rhs_u_.begin(), rhs_u_.end(), ComplexValue<sym>{0.0});

        this->add_sources_linear_rhs(y_bus, input, rhs_u_);
        add_loads(input, u);
    }

    // Solve the linear equations I_inj = YU
    // inplace
    void solve_matrix() {
        this->linear_sparse_solver_.solve_with_prefactorized_matrix(*this->linear_mat_data_, *this->linear_perm_,
                                                                    rhs_u_, rhs_u_);
    }

    // Find maximum deviation in voltage among all buses
    double iterate_unknown(ComplexValueVector<sym>& u) {
        double max_dev = 0.0;
        // loop all buses
        for (Idx bus_number = 0; bus_number != this->n_bus_; ++bus_number) {
            // Get maximum iteration for a bus
            double const dev = max_val(cabs(rhs_u_[bus_number] - u[bus_number]));
            // Keep maximum deviation of all buses
            max_dev = std::max(dev, max_dev);
            // assign updated values
            u[bus_number] = rhs_u_[bus_number];
        }
        return max_dev;
    }

  private:
    ComplexValueVector<sym> rhs_u_;

    void add_loads(PowerFlowInput<sym> const& input, ComplexValueVector<sym> const& u) {
        std::vector<LoadGenType> const& load_gen_type = *this->load_gen_type_;
        for (auto const& [bus_number, load_gens] : enumerated_zip_sequence(*this->load_gens_per_bus_)) {
            for (Idx const load_number : load_gens) {
                // load type
                LoadGenType const type = load_gen_type[load_number];
                switch (type) {
                    using enum LoadGenType;

                case const_pq:
                    // I_inj_i = conj(S_inj_j/U_i) for constant PQ type
                    rhs_u_[bus_number] += conj(input.s_injection[load_number] / u[bus_number]);
                    break;
                case const_y:
                    // I_inj_i = conj((S_inj_j * abs(U_i)^2) / U_i) = conj((S_inj_j) * U_i for const impedance type
                    rhs_u_[bus_number] += conj(input.s_injection[load_number]) * u[bus_number];
                    break;
                case const_i:
                    // I_inj_i = conj(S_inj_j*abs(U_i)/U_i) for const current type
                    rhs_u_[bus_number] += conj(input.s_injection[load_number] * cabs(u[bus_number]) / u[bus_number]);
                    break;
                default:
                    throw MissingCaseForEnumError("Injection current calculation", type);
                }
            }
        }
    }
};

template class IterativeCurrentPFSolver<symmetric_t>;
template class IterativeCurrentPFSolver<asymmetric_t>;

} // namespace iterative_current_pf

using iterative_current_pf::IterativeCurrentPFSolver;

} // namespace power_grid_model::math_solver
