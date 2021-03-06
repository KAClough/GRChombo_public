/* GRChombo
 * Copyright 2012 The GRChombo collaboration.
 * Please refer to LICENSE in GRChombo's root directory.
 */

// General includes common to most GR problems
#include "ScalarFieldLevel.hpp"
#include "AMRReductions.hpp"
#include "BoxLoops.hpp"
#include "ComputePack.hpp"
#include "NanCheck.hpp"
#include "SetValue.hpp"
#include "SmallDataIO.hpp"

// For tag cells
#include "FixedGridsTaggingCriterion.hpp"

// Problem specific includes
#include "BinaryNewtonFixedBG.hpp"
#include "ComplexScalarPotential.hpp"
#include "ExcisionDiagnostics.hpp"
#include "ExcisionEvolution.hpp"
#include "FixedBGComplexScalarField.hpp"
#include "FixedBGDensityAndAngularMom.hpp"
#include "FixedBGEnergyAndAngularMomFlux.hpp"
#include "FixedBGEvolution.hpp"
#include "ForceExtraction.hpp"
#include "InitialConditions.hpp"

// Things to do at each advance step, after the RK4 is calculated
void ScalarFieldLevel::specificAdvance()
{
    // Check for nan's
    if (m_p.nan_check)
        BoxLoops::loop(NanCheck(), m_state_new, m_state_new, SKIP_GHOST_CELLS,
                       disable_simd());
}

// Initial data for field and metric variables
void ScalarFieldLevel::initialData()
{
    CH_TIME("ScalarFieldLevel::initialData");
    if (m_verbosity)
        pout() << "ScalarFieldLevel::initialData " << m_level << endl;

    // First set everything to zero and set the initial chi to check data
    SetValue set_zero(0.0);
    BinaryNewtonFixedBG binary_newton(m_p.bg_params1, m_p.bg_params2, m_dx,
                                      m_p.center);
    auto compute_pack = make_compute_pack(set_zero, binary_newton);
    BoxLoops::loop(compute_pack, m_state_diagnostics, m_state_diagnostics,
                   SKIP_GHOST_CELLS);

    // now set the fields to evolve
    InitialConditions set_fields(m_p.field_amplitude_re, m_p.field_amplitude_im,
                                 m_p.potential_params.scalar_mass, m_p.center,
                                 m_dx);
    BoxLoops::loop(set_fields, m_state_new, m_state_new, FILL_GHOST_CELLS);
}

// Things to do before outputting a plot file
void ScalarFieldLevel::prePlotLevel() {}

// Things to do in RHS update, at each RK4 step
void ScalarFieldLevel::specificEvalRHS(GRLevelData &a_soln, GRLevelData &a_rhs,
                                       const double a_time)
{
    // Calculate MatterCCZ4 right hand side with matter_t = ScalarField
    ComplexScalarPotential potential(m_p.potential_params);
    ScalarFieldWithPotential scalar_field(potential);
    BinaryNewtonFixedBG binary_newton(m_p.bg_params1, m_p.bg_params2, m_dx,
                                      m_p.center, a_time, m_p.separation,
                                      m_p.omega_binary);
    FixedBGEvolution<ScalarFieldWithPotential, BinaryNewtonFixedBG> my_matter(
        scalar_field, binary_newton, m_p.sigma, m_dx, m_p.center);
    BoxLoops::loop(my_matter, a_soln, a_rhs, SKIP_GHOST_CELLS);

    // excise within horizon, no simd
    BoxLoops::loop(
        ExcisionEvolution<ScalarFieldWithPotential, BinaryNewtonFixedBG>(
            m_dx, m_p.center, binary_newton),
        a_soln, a_rhs, SKIP_GHOST_CELLS, disable_simd());
}

void ScalarFieldLevel::specificPostTimeStep()
{
    // At any level, but after the coarsest timestep
    double coarsest_dt = m_p.coarsest_dx * m_p.dt_multiplier;
    const double remainder = fmod(m_time, coarsest_dt);
    if (min(abs(remainder), abs(remainder - coarsest_dt)) < 1.0e-8)
    {
        // calculate the density of the PF, but excise the BH region completely
        fillAllGhosts();
        ComplexScalarPotential potential(m_p.potential_params);
        ScalarFieldWithPotential scalar_field(potential);
        BinaryNewtonFixedBG binary_newton(m_p.bg_params1, m_p.bg_params2, m_dx,
                                          m_p.center, m_time, m_p.separation,
                                          m_p.omega_binary);
        FixedBGDensityAndAngularMom<ScalarFieldWithPotential,
                                    BinaryNewtonFixedBG>
            densities(scalar_field, binary_newton, m_dx, m_p.center);
        FixedBGEnergyAndAngularMomFlux<ScalarFieldWithPotential,
                                       BinaryNewtonFixedBG>
            energy_fluxes(scalar_field, binary_newton, m_dx, m_p.center);
        BoxLoops::loop(
            make_compute_pack(densities, energy_fluxes, binary_newton),
            m_state_new, m_state_diagnostics, SKIP_GHOST_CELLS);
        // excise within horizon
        BoxLoops::loop(
            ExcisionDiagnostics<ScalarFieldWithPotential, BinaryNewtonFixedBG>(
                m_dx, m_p.center, binary_newton, m_p.inner_r, m_p.outer_r),
            m_state_diagnostics, m_state_diagnostics, SKIP_GHOST_CELLS,
            disable_simd());
    }

    // write out the integral after each coarse timestep
    if (m_level == 0)
    {
        bool first_step = (m_time == m_dt);

        // integrate the densities and write to a file
        AMRReductions<VariableType::diagnostic> amr_reductions(m_gr_amr);
        double rho_sum = amr_reductions.sum(c_rho);
        double rhoJ_sum = amr_reductions.sum(c_rhoJ);

        SmallDataIO integral_file(m_p.integral_filename, m_dt, m_time,
                                  m_restart_time, SmallDataIO::APPEND,
                                  first_step);
        // remove any duplicate data if this is post restart
        integral_file.remove_duplicate_time_data();
        std::vector<double> data_for_writing = {rho_sum, rhoJ_sum};
        // write data
        if (first_step)
        {
            integral_file.write_header_line({"rho", "rhoJ"});
        }
        integral_file.write_time_data_line(data_for_writing);

        // Now refresh the interpolator and do the interpolation
        m_gr_amr.m_interpolator->refresh();
        ForceExtraction my_extraction(m_p.extraction_params, m_dt, m_time,
                                      m_restart_time);
        my_extraction.execute_query(m_gr_amr.m_interpolator);
    }
}

void ScalarFieldLevel::computeTaggingCriterion(FArrayBox &tagging_criterion,
                                               const FArrayBox &current_state)
{
    BoxLoops::loop(FixedGridsTaggingCriterion(m_dx, m_level, m_p.regrid_length,
                                              m_p.center),
                   current_state, tagging_criterion, disable_simd());
}
