/* Copyright 2023 Grant Johnson, Remi Lehe
 *
 * This file is part of WarpX.
 *
 * License: BSD-3-Clause-LBNL
 */
#include "ablastr/coarsen/sample.H"
#include "Particles/Pusher/UpdateMomentumHigueraCary.H"
#include "Utils/WarpXProfilerWrapper.H"

#include "MusclHancockUtils.H"
#include "WarpXFluidContainer.H"
#include "WarpX.H"
#include <ablastr/utils/Communication.H>
using namespace ablastr::utils::communication;
using namespace amrex;

WarpXFluidContainer::WarpXFluidContainer(int nlevs_max, int ispecies, const std::string &name)
{
    species_id = ispecies;
    species_name = name;

    plasma_injector = std::make_unique<PlasmaInjector>(species_id, species_name);
    physical_species = plasma_injector->getPhysicalSpecies();
    charge = plasma_injector->getCharge();
    mass = plasma_injector->getMass();

    ReadParameters();

    // Resize the list of MultiFabs for the right number of levels
    N.resize(nlevs_max);
    NU.resize(nlevs_max);
}

void WarpXFluidContainer::ReadParameters()
{
    static bool initialized = false;
    if (!initialized)
    {
        const ParmParse pp_species_name(species_name);
        pp_species_name.query("do_not_deposit", do_not_deposit);
        pp_species_name.query("do_not_gather", do_not_gather);
        pp_species_name.query("do_not_push", do_not_push);
        initialized = true;
    }
}

void WarpXFluidContainer::AllocateLevelMFs(int lev, const BoxArray &ba, const DistributionMapping &dm)
{
    int ncomps = 1;
    amrex::IntVect nguards = {AMREX_D_DECL(2, 2, 2)};

    WarpX &warpx = WarpX::GetInstance();

    // set human-readable tag for each MultiFab
    auto const tag = [lev](std::string tagname)
    {
        tagname.append("[l=").append(std::to_string(lev)).append("]");
        return tagname;
    };

    warpx.AllocInitMultiFab(N[lev], amrex::convert(ba, amrex::IntVect::TheNodeVector()),
                            dm, ncomps, nguards, lev, tag("fluid density"), 0.0_rt);

    warpx.AllocInitMultiFab(NU[lev][0], amrex::convert(ba, amrex::IntVect::TheNodeVector()),
                            dm, ncomps, nguards, lev, tag("fluid momentum density [x]"), 0.0_rt);
    warpx.AllocInitMultiFab(NU[lev][1], amrex::convert(ba, amrex::IntVect::TheNodeVector()),
                            dm, ncomps, nguards, lev, tag("fluid momentum density [y]"), 0.0_rt);
    warpx.AllocInitMultiFab(NU[lev][2], amrex::convert(ba, amrex::IntVect::TheNodeVector()),
                            dm, ncomps, nguards, lev, tag("fluid momentum density [z]"), 0.0_rt);
}

void WarpXFluidContainer::InitData(int lev)
{
    WARPX_PROFILE("WarpXFluidContainer::InitData");

    // Extract objects that give the initial density and momentum
    InjectorDensity *inj_rho = plasma_injector->getInjectorDensity();
    InjectorMomentum *inj_mom = plasma_injector->getInjectorMomentumDevice();

    // Extract grid geometry properties
    WarpX &warpx = WarpX::GetInstance();
    const amrex::Geometry &geom = warpx.Geom(lev);
    const auto dx = geom.CellSizeArray();
    const auto problo = geom.ProbLoArray();
    const amrex::Real clight = PhysConst::c;

    // Loop through cells and initialize their value
#ifdef AMREX_USE_OMP
#pragma omp parallel if (amrex::Gpu::notInLaunchRegion())
#endif
    for (MFIter mfi(*N[lev], TilingIfNotGPU()); mfi.isValid(); ++mfi)
    {

        amrex::Box const &tile_box = mfi.tilebox(N[lev]->ixType().toIntVect());
        amrex::Array4<Real> const &N_arr = N[lev]->array(mfi);
        amrex::Array4<Real> const &NUx_arr = NU[lev][0]->array(mfi);
        amrex::Array4<Real> const &NUy_arr = NU[lev][1]->array(mfi);
        amrex::Array4<Real> const &NUz_arr = NU[lev][2]->array(mfi);

        amrex::ParallelFor(tile_box,
            [=] AMREX_GPU_DEVICE(int i, int j, int k) noexcept
            {
#if defined(WARPX_DIM_3D)
                amrex::Real x = problo[0] + i * dx[0];
                amrex::Real y = problo[1] + j * dx[1];
                amrex::Real z = problo[2] + k * dx[2];
#elif defined(WARPX_DIM_XZ) || defined(WARPX_DIM_RZ)
                amrex::Real x = problo[0] + i * dx[0];
                amrex::Real y = 0.0_rt;
                amrex::Real z = problo[1] + j * dx[1];
#else
                amrex::Real x = 0.0_rt;
                amrex::Real y = 0.0_rt;
                amrex::Real z = problo[0] + i * dx[0];
#endif

                amrex::Real n = inj_rho->getDensity(x, y, z);
                auto u = inj_mom->getBulkMomentum(x, y, z);

                N_arr(i, j, k) = n;
                NUx_arr(i, j, k) = n * u.x * clight;
                NUy_arr(i, j, k) = n * u.y * clight;
                NUz_arr(i, j, k) = n * u.z * clight;
            }
        );
    }

    // Fill guard cells
    const amrex::Periodicity &period = geom.periodicity();
    FillBoundary(*N[lev], N[lev]->nGrowVect(), WarpX::do_single_precision_comms, period);
    FillBoundary(*NU[lev][0], NU[lev][0]->nGrowVect(), WarpX::do_single_precision_comms, period);
    FillBoundary(*NU[lev][1], NU[lev][1]->nGrowVect(), WarpX::do_single_precision_comms, period);
    FillBoundary(*NU[lev][2], NU[lev][2]->nGrowVect(), WarpX::do_single_precision_comms, period);
}


void WarpXFluidContainer::Evolve(
    int lev,
    const amrex::MultiFab &Ex, const amrex::MultiFab &Ey, const amrex::MultiFab &Ez,
    const amrex::MultiFab &Bx, const amrex::MultiFab &By, const amrex::MultiFab &Bz,
    amrex::MultiFab &jx, amrex::MultiFab &jy, amrex::MultiFab &jz, bool skip_deposition)
{

    // Step the Lorentz Term
    GatherAndPush(lev, Ex, Ey, Ez, Bx, By, Bz);

    // Step the Advective term
    AdvectivePush_Muscl(lev);

    // Deposit J to the simulation mesh
    if (!skip_deposition)
    {
        DepositCurrent(lev, jx, jy, jz);
    }
}


// Muscl Advection Update
void WarpXFluidContainer::AdvectivePush_Muscl (int lev)
{
    WARPX_PROFILE("WarpXFluidContainer::AdvectivePush_Muscl");

    // Grab the grid spacing
    WarpX &warpx = WarpX::GetInstance();
    const Real dt = warpx.getdt(lev);
    const amrex::Geometry &geom = warpx.Geom(lev);
    const auto dx = geom.CellSizeArray();
    const amrex::Real clight = PhysConst::c;
    const amrex::Periodicity &period = geom.periodicity();
    auto cx = (dt/dx[0]);
    auto cy = (dt/dx[1]);
    auto cz = (dt/dx[2]);
    auto cx_half = 0.5*(dt/dx[0]);
    auto cy_half = 0.5*(dt/dx[1]);
    auto cz_half = 0.5*(dt/dx[2]);

    // Temporary velocities
    amrex::MultiFab tmp_Vx(N[lev]->boxArray(), N[lev]->DistributionMap(), 1, 1);
    amrex::MultiFab tmp_Vy(N[lev]->boxArray(), N[lev]->DistributionMap(), 1, 1);
    amrex::MultiFab tmp_Vz(N[lev]->boxArray(), N[lev]->DistributionMap(), 1, 1);

    // Temporary Half-step values
    amrex::BoxArray ba = N[lev]->boxArray();
    amrex::MultiFab tmp_Q_minus_x( amrex::convert(ba, IntVect(0,1,1)), N[lev]->DistributionMap(), 4, 1);
    amrex::MultiFab tmp_Q_plus_x( amrex::convert(ba, IntVect(0,1,1)), N[lev]->DistributionMap(), 4, 1);
    amrex::MultiFab tmp_Q_minus_y( amrex::convert(ba, IntVect(1,0,1)), N[lev]->DistributionMap(), 4, 1);
    amrex::MultiFab tmp_Q_plus_y( amrex::convert(ba, IntVect(1,0,1)), N[lev]->DistributionMap(), 4, 1);
    amrex::MultiFab tmp_Q_minus_z( amrex::convert(ba, IntVect(1,1,0)), N[lev]->DistributionMap(), 4, 1);
    amrex::MultiFab tmp_Q_plus_z( amrex::convert(ba, IntVect(1,1,0)), N[lev]->DistributionMap(), 4, 1);

    // Advection push
    #ifdef AMREX_USE_OMP
    #pragma omp parallel if (amrex::Gpu::notInLaunchRegion())
    #endif
    for (MFIter mfi(*N[lev], TilingIfNotGPU()); mfi.isValid(); ++mfi)
    {
        // Grow the entire domain
        amrex::Box box = mfi.validbox();
        box.grow(1);

        // Loop over a box with one extra gridpoint to avoid needing to communicate
        // the temporary arrays
        amrex::Box const tile_box = mfi.growntilebox(1);

        // Only select tiles within the grown grid
        amrex::Box const box_x = amrex::convert( box, tmp_Q_minus_x.ixType() );
        amrex::Box const box_y = amrex::convert( box, tmp_Q_minus_y.ixType() );
        amrex::Box const box_z = amrex::convert( box, tmp_Q_minus_z.ixType() );

        amrex::Array4<Real> const &N_arr = N[lev]->array(mfi);
        amrex::Array4<Real> const &NUx_arr = NU[lev][0]->array(mfi);
        amrex::Array4<Real> const &NUy_arr = NU[lev][1]->array(mfi);
        amrex::Array4<Real> const &NUz_arr = NU[lev][2]->array(mfi);

        amrex::Array4<amrex::Real> Vx = tmp_Vx.array(mfi);
        amrex::Array4<amrex::Real> Vy = tmp_Vy.array(mfi);
        amrex::Array4<amrex::Real> Vz = tmp_Vz.array(mfi);

        amrex::Array4<amrex::Real> Q_minus_x = tmp_Q_minus_x.array(mfi);
        amrex::Array4<amrex::Real> Q_plus_x = tmp_Q_plus_x.array(mfi);

        amrex::Array4<amrex::Real> Q_minus_y = tmp_Q_minus_y.array(mfi);
        amrex::Array4<amrex::Real> Q_plus_y = tmp_Q_plus_y.array(mfi);

        amrex::Array4<amrex::Real> Q_minus_z = tmp_Q_minus_z.array(mfi);
        amrex::Array4<amrex::Real> Q_plus_z = tmp_Q_plus_z.array(mfi);

        amrex::ParallelFor(tile_box,
            [=] AMREX_GPU_DEVICE(int i, int j, int k) noexcept
            {

                // - Grab local Uz Uy Ux gamma
                // Isolate U from NU
                auto Ux = (NUx_arr(i, j, k) / N_arr(i,j,k));
                auto Uy = (NUy_arr(i, j, k) / N_arr(i,j,k));
                auto Uz = (NUz_arr(i, j, k) / N_arr(i,j,k));
                auto Uz_sq = Uz*Uz; auto Uy_sq = Uy*Uy; auto Ux_sq = Ux*Ux;
                auto Uz_cubed = Uz_sq*Uz; auto Uy_cubed = Uy_sq*Uy; auto Ux_cubed = Ux_sq*Ux;
                auto c_sq = clight*clight; 
                auto gamma = sqrt(1.0 + (Ux_sq + Uy_sq + Uz_sq)/(c_sq) );
                auto gamma_cubed = gamma*gamma*gamma;
                auto a = c_sq*gamma_cubed;

                // Compute Vx Vy Vz
                Vx(i,j,k) = Ux/gamma;
                Vy(i,j,k) = Uy/gamma;
                Vz(i,j,k) = Uz/gamma;

                // Select the specific implmentation depending on dimensionality
                #if defined(WARPX_DIM_3D)

                // Compute the Flux-Jacobian Elements in x
                auto A00x = (Ux*(Uz_sq)+Ux*(Uy_sq)+(Ux_cubed))/a;
                auto A01x = ((c_sq)+(Uz_sq)+(Uy_sq))/a;
                auto A02x = -(Ux*Uy)/a;
                auto A03x = -(Ux*Uz)/a;

                auto A10x = -(Ux_sq)/(gamma_cubed);
                auto A11x = (2.0*Ux*(c_sq)+2.0*Ux*(Uz_sq)+2.0*Ux*(Uy_sq)+(Ux_cubed))/a;
                auto A12x = -((Ux_sq)*Uy)/a;
                auto A13x = -((Ux_sq)*Uz)/a;

                auto A20x = -(Ux*Uy)/(gamma_cubed);
                auto A21x = (Uy*(c_sq)+Uy*(Uz_sq)+(Uy_cubed))/a;
                auto A22x = (Ux*(c_sq)+Ux*(Uz_sq)+(Ux_cubed))/a;
                auto A23x = -(Ux*Uy*Uz)/a;

                auto A30x = -(Ux*Uz)/(gamma_cubed);
                auto A31x = (Uz*(c_sq)+(Uz_cubed)+(Uy_sq)*Uz)/a;
                auto A32x = -(Ux*Uy*Uz)/a;
                auto A33x = (Ux*(c_sq)+Ux*(Uy_sq)+(Ux_cubed))/a;


                // Compute the Flux-Jacobian Elements in y
                auto A00y = (Uy*(Uz_sq)+(Uy_cubed)+(Ux_sq)*Uy)/a;
                auto A01y = -(Ux*Uy)/a;
                auto A02y = ((c_sq)+(Uz_sq)+(Ux_sq))/a;
                auto A03y = -(Uy*Uz)/a;

                auto A10y = -(Ux*Uy)/(gamma_cubed);
                auto A11y = (Uy*(c_sq)+Uy*(Uz_sq)+(Uy_cubed))/a;
                auto A12y = (Ux*(c_sq)+Ux*(Uz_sq)+(Ux_cubed))/a;
                auto A13y = -(Ux*Uy*Uz)/a;

                auto A20y = -(Uy_sq)/(gamma_cubed);
                auto A21y = -(Ux*(Uy_sq))/a;
                auto A22y = (2.0*Uy*(c_sq)+2.0*Uy*(Uz_sq)+(Uy_cubed)+2.0*(Ux_sq)*Uy)/a;
                auto A23y = -((Uy_sq)*Uz)/a;

                auto A30y = -(Uy*Uz)/(gamma_cubed);
                auto A31y = -(Ux*Uy*Uz)/a;
                auto A32y = (Uz*(c_sq)+(Uz_cubed)+(Ux_sq)*Uz)/a;
                auto A33y = (Uy*(c_sq)+(Uy_cubed)+(Ux_sq)*Uy)/a;


                // Compute the Flux-Jacobian Elements in z
                auto A00z = ((Uz_cubed)+((Uy_sq)+(Ux_sq))*Uz)/a;
                auto A01z = -(Ux*Uz)/a;
                auto A02z = -(Uy*Uz)/a;
                auto A03z = ((c_sq)+(Uy_sq)+(Ux_sq))/a;

                auto A10z = -(Ux*Uz)/(gamma_cubed);
                auto A11z = (Uz*(c_sq)+(Uz_cubed)+(Uy_sq)*Uz)/a;
                auto A12z = -(Ux*Uy*Uz)/a;
                auto A13z = (Ux*(c_sq)+Ux*(Uy_sq)+(Ux_cubed))/a;

                auto A20z = -(Uy*Uz)/(gamma_cubed);
                auto A21z = -(Ux*Uy*Uz)/a;
                auto A22z = (Uz*(c_sq)+(Uz_cubed)+(Ux_sq)*Uz)/a;
                auto A23z = (Uy*(c_sq)+(Uy_cubed)+(Ux_sq)*Uy)/a;

                auto A30z = -(Uz_sq)/(gamma_cubed);
                auto A31z = -(Ux*(Uz_sq))/a;
                auto A32z = -(Uy*(Uz_sq))/a;
                auto A33z = (2.0*Uz*(c_sq)+(Uz_cubed)+(2.0*(Uy_sq)+2.0*(Ux_sq))*Uz)/a;

                // Compute the cell slopes x
                auto dQ0x = ave( N_arr(i,j,k) - N_arr(i-1,j,k) , N_arr(i+1,j,k) - N_arr(i,j,k) );
                auto dQ1x = ave( NUx_arr(i,j,k) - NUx_arr(i-1,j,k) , NUx_arr(i+1,j,k) - NUx_arr(i,j,k) );
                auto dQ2x = ave( NUy_arr(i,j,k) - NUy_arr(i-1,j,k) , NUy_arr(i+1,j,k) - NUy_arr(i,j,k) );
                auto dQ3x = ave( NUz_arr(i,j,k) - NUz_arr(i-1,j,k) , NUz_arr(i+1,j,k) - NUz_arr(i,j,k) );

                // Compute the cell slopes y
                auto dQ0y = ave( N_arr(i,j,k) - N_arr(i,j-1,k) , N_arr(i,j+1,k) - N_arr(i,j,k) );
                auto dQ1y = ave( NUx_arr(i,j,k) - NUx_arr(i,j-1,k) , NUx_arr(i,j+1,k) - NUx_arr(i,j,k) );
                auto dQ2y = ave( NUy_arr(i,j,k) - NUy_arr(i,j-1,k) , NUy_arr(i,j+1,k) - NUy_arr(i,j,k) );
                auto dQ3y = ave( NUz_arr(i,j,k) - NUz_arr(i,j-1,k) , NUz_arr(i,j+1,k) - NUz_arr(i,j,k) );

                // Compute the cell slopes z
                auto dQ0z = ave( N_arr(i,j,k) - N_arr(i,j,k-1) , N_arr(i,j,k+1) - N_arr(i,j,k) );
                auto dQ1z = ave( NUx_arr(i,j,k) - NUx_arr(i,j,k-1) , NUx_arr(i,j,k+1) - NUx_arr(i,j,k) );
                auto dQ2z = ave( NUy_arr(i,j,k) - NUy_arr(i,j,k-1) , NUy_arr(i,j,k+1) - NUy_arr(i,j,k) );
                auto dQ3z = ave( NUz_arr(i,j,k) - NUz_arr(i,j,k-1) , NUz_arr(i,j,k+1) - NUz_arr(i,j,k) );

                // Compute Q ([ N, NU]) at the halfsteps (Q_tidle) using the slopes (dQ)
                auto AdQ0x = A00x*dQ0x + A01x*dQ1x + A02x*dQ2x + A03x*dQ3x;
                auto AdQ1x = A10x*dQ0x + A11x*dQ1x + A12x*dQ2x + A13x*dQ3x;
                auto AdQ2x = A20x*dQ0x + A21x*dQ1x + A22x*dQ2x + A23x*dQ3x;
                auto AdQ3x = A30x*dQ0x + A31x*dQ1x + A32x*dQ2x + A33x*dQ3x;
                auto AdQ0y = A00y*dQ0y + A01y*dQ1y + A02y*dQ2y + A03y*dQ3y;
                auto AdQ1y = A10y*dQ0y + A11y*dQ1y + A12y*dQ2y + A13y*dQ3y;
                auto AdQ2y = A20y*dQ0y + A21y*dQ1y + A22y*dQ2y + A23y*dQ3y;
                auto AdQ3y = A30y*dQ0y + A31y*dQ1y + A32y*dQ2y + A33y*dQ3y;
                auto AdQ0z = A00z*dQ0z + A01z*dQ1z + A02z*dQ2z + A03z*dQ3z;
                auto AdQ1z = A10z*dQ0z + A11z*dQ1z + A12z*dQ2z + A13z*dQ3z;
                auto AdQ2z = A20z*dQ0z + A21z*dQ1z + A22z*dQ2z + A23z*dQ3z;
                auto AdQ3z = A30z*dQ0z + A31z*dQ1z + A32z*dQ2z + A33z*dQ3z;
                auto Q_tilde0 = N_arr(i,j,k)   - cx_half*AdQ0x - cy_half*AdQ0y - cz_half*AdQ0z;
                auto Q_tilde1 = NUx_arr(i,j,k) - cx_half*AdQ1x - cy_half*AdQ1y - cz_half*AdQ1z;
                auto Q_tilde2 = NUy_arr(i,j,k) - cx_half*AdQ2x - cy_half*AdQ2y - cz_half*AdQ2z;
                auto Q_tilde3 = NUz_arr(i,j,k) - cx_half*AdQ3x - cy_half*AdQ3y - cz_half*AdQ3z;

                // Predict Q at the cell edges (x)
                // (note that _plus is shifted due to grid location)
                if ( box_x.contains(i,j,k) ) {
                    Q_minus_x(i,j,k,0) = Q_tilde0 + dQ0x/2.0;
                    Q_minus_x(i,j,k,1) = Q_tilde1 + dQ1x/2.0;
                    Q_minus_x(i,j,k,2) = Q_tilde2 + dQ2x/2.0;
                    Q_minus_x(i,j,k,3) = Q_tilde3 + dQ3x/2.0;
                }
                if ( box_x.contains(i-1,j,k) ) {
                    Q_plus_x(i-1,j,k,0) = Q_tilde0 - dQ0x/2.0;
                    Q_plus_x(i-1,j,k,1) = Q_tilde1 - dQ1x/2.0;
                    Q_plus_x(i-1,j,k,2) = Q_tilde2 - dQ2x/2.0;
                    Q_plus_x(i-1,j,k,3) = Q_tilde3 - dQ3x/2.0;
                }

                // Predict Q at the cell edges (y)
                if ( box_y.contains(i,j,k) ) {
                    Q_minus_y(i,j,k,0) = Q_tilde0 + dQ0y/2.0;
                    Q_minus_y(i,j,k,1) = Q_tilde1 + dQ1y/2.0;
                    Q_minus_y(i,j,k,2) = Q_tilde2 + dQ2y/2.0;
                    Q_minus_y(i,j,k,3) = Q_tilde3 + dQ3y/2.0;
                }
                if ( box_y.contains(i,j-1,k) ) {
                    Q_plus_y(i,j-1,k,0) = Q_tilde0 - dQ0y/2.0;
                    Q_plus_y(i,j-1,k,1) = Q_tilde1 - dQ1y/2.0;
                    Q_plus_y(i,j-1,k,2) = Q_tilde2 - dQ2y/2.0;
                    Q_plus_y(i,j-1,k,3) = Q_tilde3 - dQ3y/2.0;
                }
                if ( box_z.contains(i,j,k) ) {
                // Predict Q at the cell edges (z)
                    Q_minus_z(i,j,k,0) = Q_tilde0 + dQ0z/2.0;
                    Q_minus_z(i,j,k,1) = Q_tilde1 + dQ1z/2.0;
                    Q_minus_z(i,j,k,2) = Q_tilde2 + dQ2z/2.0;
                    Q_minus_z(i,j,k,3) = Q_tilde3 + dQ3z/2.0;
                }
                if ( box_z.contains(i,j,k-1) ) {
                    Q_plus_z(i,j,k-1,0) = Q_tilde0 - dQ0z/2.0;
                    Q_plus_z(i,j,k-1,1) = Q_tilde1 - dQ1z/2.0;
                    Q_plus_z(i,j,k-1,2) = Q_tilde2 - dQ2z/2.0;
                    Q_plus_z(i,j,k-1,3) = Q_tilde3 - dQ3z/2.0;
                }


                #elif defined(WARPX_DIM_XZ) || defined(WARPX_DIM_RZ)

                #else

                #endif
            }
        );
    }

    // Advection push
    #ifdef AMREX_USE_OMP
    #pragma omp parallel if (amrex::Gpu::notInLaunchRegion())
    #endif
    for (MFIter mfi(*N[lev], TilingIfNotGPU()); mfi.isValid(); ++mfi)
    {
        amrex::Box const &tile_box = mfi.tilebox(N[lev]->ixType().toIntVect());
        amrex::Array4<Real> N_arr = N[lev]->array(mfi);
        amrex::Array4<Real> NUx_arr = NU[lev][0]->array(mfi);
        amrex::Array4<Real> NUy_arr = NU[lev][1]->array(mfi);
        amrex::Array4<Real> NUz_arr = NU[lev][2]->array(mfi);

        amrex::Array4<amrex::Real> const &Vx = tmp_Vx.array(mfi);
        amrex::Array4<amrex::Real> const &Vy = tmp_Vy.array(mfi);
        amrex::Array4<amrex::Real> const &Vz = tmp_Vz.array(mfi);

        amrex::Array4<amrex::Real> const &Q_minus_x = tmp_Q_minus_x.array(mfi);
        amrex::Array4<amrex::Real> const &Q_plus_x = tmp_Q_plus_x.array(mfi);

        amrex::Array4<amrex::Real> const &Q_minus_y = tmp_Q_minus_y.array(mfi);
        amrex::Array4<amrex::Real> const &Q_plus_y = tmp_Q_plus_y.array(mfi);

        amrex::Array4<amrex::Real> const &Q_minus_z = tmp_Q_minus_z.array(mfi);
        amrex::Array4<amrex::Real> const &Q_plus_z = tmp_Q_plus_z.array(mfi);

        amrex::ParallelFor(tile_box,
            [=] AMREX_GPU_DEVICE(int i, int j, int k) noexcept
            {

                // Select the specific implmentation depending on dimensionality
                #if defined(WARPX_DIM_3D)


                // compute the fluxes:
                // (note that _plus is shifted due to grid location)
                auto F0_minusx = flux(Q_minus_x(i-1,j,k,0),Q_plus_x(i-1,j,k,0),  Vx(i-1,j,k),Vx(i,j,k));
                auto F0_plusx =  flux(Q_minus_x(i,j,k,0),  Q_plus_x(i,j,k,0),Vx(i,j,k),  Vx(i+1,j,k));
                auto F1_minusx = flux(Q_minus_x(i-1,j,k,1),Q_plus_x(i-1,j,k,1),  Vx(i-1,j,k),Vx(i,j,k));
                auto F1_plusx =  flux(Q_minus_x(i,j,k,1),  Q_plus_x(i,j,k,1),Vx(i,j,k),  Vx(i+1,j,k));
                auto F2_minusx = flux(Q_minus_x(i-1,j,k,2),Q_plus_x(i-1,j,k,2),  Vx(i-1,j,k),Vx(i,j,k));
                auto F2_plusx =  flux(Q_minus_x(i,j,k,2),  Q_plus_x(i,j,k,2),Vx(i,j,k),  Vx(i+1,j,k));
                auto F3_minusx = flux(Q_minus_x(i-1,j,k,3),Q_plus_x(i-1,j,k,3),  Vx(i-1,j,k),Vx(i,j,k));
                auto F3_plusx =  flux(Q_minus_x(i,j,k,3),  Q_plus_x(i,j,k,3),Vx(i,j,k),  Vx(i+1,j,k));

                auto F0_minusy = flux(Q_minus_y(i,j-1,k,0),Q_plus_y(i,j-1,k,0),  Vy(i,j-1,k),Vy(i,j,k));
                auto F0_plusy =  flux(Q_minus_y(i,j,k,0),  Q_plus_y(i,j,k,0),Vy(i,j,k),  Vy(i,j+1,k));
                auto F1_minusy = flux(Q_minus_y(i,j-1,k,1),Q_plus_y(i,j-1,k,1),  Vy(i,j-1,k),Vy(i,j,k));
                auto F1_plusy =  flux(Q_minus_y(i,j,k,1),  Q_plus_y(i,j,k,1),Vy(i,j,k),  Vy(i,j+1,k));
                auto F2_minusy = flux(Q_minus_y(i,j-1,k,2),Q_plus_y(i,j-1,k,2),  Vy(i,j-1,k),Vy(i,j,k));
                auto F2_plusy =  flux(Q_minus_y(i,j,k,2),  Q_plus_y(i,j,k,2),Vy(i,j,k),  Vy(i,j+1,k));
                auto F3_minusy = flux(Q_minus_y(i,j-1,k,3),Q_plus_y(i,j-1,k,3),  Vy(i,j-1,k),Vy(i,j,k));
                auto F3_plusy =  flux(Q_minus_y(i,j,k,3),  Q_plus_y(i,j,k,3),Vy(i,j,k),  Vy(i,j+1,k));

                auto F0_minusz = flux(Q_minus_z(i,j,k-1,0),Q_plus_z(i,j,k-1,0),  Vz(i,j,k-1),Vz(i,j,k));
                auto F0_plusz =  flux(Q_minus_z(i,j,k,0),  Q_plus_z(i,j,k,0),Vz(i,j,k),  Vz(i,j,k+1));
                auto F1_minusz = flux(Q_minus_z(i,j,k-1,1),Q_plus_z(i,j,k-1,1),  Vz(i,j,k-1),Vz(i,j,k));
                auto F1_plusz =  flux(Q_minus_z(i,j,k,1),  Q_plus_z(i,j,k,1),Vz(i,j,k),  Vz(i,j,k+1));
                auto F2_minusz = flux(Q_minus_z(i,j,k-1,2),Q_plus_z(i,j,k-1,2),  Vz(i,j,k-1),Vz(i,j,k));
                auto F2_plusz =  flux(Q_minus_z(i,j,k,2),  Q_plus_z(i,j,k,2),Vz(i,j,k),  Vz(i,j,k+1));
                auto F3_minusz = flux(Q_minus_z(i,j,k-1,3),Q_plus_z(i,j,k-1,3),  Vz(i,j,k-1),Vz(i,j,k));
                auto F3_plusz =  flux(Q_minus_z(i,j,k,3),  Q_plus_z(i,j,k,3),Vz(i,j,k),  Vz(i,j,k+1));

                // Update Q from tn -> tn + dt
                N_arr(i,j,k) = N_arr(i,j,k) - cx*(F0_plusx - F0_minusx)
                                            - cy*(F0_plusy - F0_minusy)
                                            - cz*(F0_plusz - F0_minusz);
                NUx_arr(i,j,k) = NUx_arr(i,j,k) - cx*(F1_plusx - F1_minusx)
                                                - cy*(F1_plusy - F1_minusy)
                                                - cz*(F1_plusz - F1_minusz);
                NUy_arr(i,j,k) = NUy_arr(i,j,k) - cx*(F2_plusx - F2_minusx)
                                                - cy*(F2_plusy - F2_minusy)
                                                - cz*(F2_plusz - F2_minusz);
                NUz_arr(i,j,k) = NUz_arr(i,j,k) - cx*(F3_plusx - F3_minusx)
                                                - cy*(F3_plusy - F3_minusy)
                                                - cz*(F3_plusz - F3_minusz);

                #elif defined(WARPX_DIM_XZ) || defined(WARPX_DIM_RZ)

                #else

                #endif
            }
        );
    }

    // Fill guard cells
    FillBoundary(*N[lev], N[lev]->nGrowVect(), WarpX::do_single_precision_comms, period);
    FillBoundary(*NU[lev][0], NU[lev][0]->nGrowVect(), WarpX::do_single_precision_comms, period);
    FillBoundary(*NU[lev][1], NU[lev][1]->nGrowVect(), WarpX::do_single_precision_comms, period);
    FillBoundary(*NU[lev][2], NU[lev][2]->nGrowVect(), WarpX::do_single_precision_comms, period);
}

// Momentum source from fields
void WarpXFluidContainer::GatherAndPush (
    int lev,
    const amrex::MultiFab& Ex, const amrex::MultiFab& Ey, const amrex::MultiFab& Ez,
    const amrex::MultiFab& Bx, const amrex::MultiFab& By, const amrex::MultiFab& Bz)
{
    WARPX_PROFILE("WarpXFluidContainer::GatherAndPush");

    WarpX &warpx = WarpX::GetInstance();
    const amrex::Real q = getCharge();
    const amrex::Real m = getMass();
    const Real dt = warpx.getdt(lev);

   // Prepare interpolation of current components to cell center
    auto Nodal_type = amrex::GpuArray<int, 3>{0, 0, 0};
    auto Ex_type = amrex::GpuArray<int, 3>{0, 0, 0};
    auto Ey_type = amrex::GpuArray<int, 3>{0, 0, 0};
    auto Ez_type = amrex::GpuArray<int, 3>{0, 0, 0};
    auto Bx_type = amrex::GpuArray<int, 3>{0, 0, 0};
    auto By_type = amrex::GpuArray<int, 3>{0, 0, 0};
    auto Bz_type = amrex::GpuArray<int, 3>{0, 0, 0};
    for (int i = 0; i < AMREX_SPACEDIM; ++i)
    {
        Nodal_type[i] = N[lev]->ixType()[i];
        Ex_type[i] = Ex.ixType()[i];
        Ey_type[i] = Ey.ixType()[i];
        Ez_type[i] = Ez.ixType()[i];
        Bx_type[i] = Bx.ixType()[i];
        By_type[i] = By.ixType()[i];
        Bz_type[i] = Bz.ixType()[i];
    }


    // H&C push the momentum
    #ifdef AMREX_USE_OMP
    #pragma omp parallel if (amrex::Gpu::notInLaunchRegion())
    #endif
    for (MFIter mfi(*N[lev], TilingIfNotGPU()); mfi.isValid(); ++mfi)
    {

        amrex::Box const &tile_box = mfi.tilebox(N[lev]->ixType().toIntVect());

        amrex::Array4<Real> const &N_arr = N[lev]->array(mfi);
        amrex::Array4<Real> NUx_arr = NU[lev][0]->array(mfi);
        amrex::Array4<Real> NUy_arr = NU[lev][1]->array(mfi);
        amrex::Array4<Real> NUz_arr = NU[lev][2]->array(mfi);

        amrex::Array4<const amrex::Real> const& Ex_arr = Ex.array(mfi);
        amrex::Array4<const amrex::Real> const& Ey_arr = Ey.array(mfi);
        amrex::Array4<const amrex::Real> const& Ez_arr = Ez.array(mfi);
        amrex::Array4<const amrex::Real> const& Bx_arr = Bx.array(mfi);
        amrex::Array4<const amrex::Real> const& By_arr = By.array(mfi);
        amrex::Array4<const amrex::Real> const& Bz_arr = Bz.array(mfi);

        // Here, we do not perform any coarsening.
        amrex::GpuArray<int, 3U> coarsening_ratio = {AMREX_D_DECL(1, 1, 1)};

        amrex::ParallelFor(tile_box,
            [=] AMREX_GPU_DEVICE(int i, int j, int k) noexcept
            {

                // Interpolate fields from tmp to Nodal points
                amrex::Real Ex_Nodal = ablastr::coarsen::sample::Interp(Ex_arr,
                    Ex_type, Nodal_type, coarsening_ratio, i, j, k, 0);
                amrex::Real Ey_Nodal = ablastr::coarsen::sample::Interp(Ey_arr,
                    Ey_type, Nodal_type, coarsening_ratio, i, j, k, 0);
                amrex::Real Ez_Nodal = ablastr::coarsen::sample::Interp(Ez_arr,
                    Ez_type, Nodal_type, coarsening_ratio, i, j, k, 0);
                amrex::Real Bx_Nodal = ablastr::coarsen::sample::Interp(Bx_arr,
                    Bx_type, Nodal_type, coarsening_ratio, i, j, k, 0);
                amrex::Real By_Nodal = ablastr::coarsen::sample::Interp(By_arr,
                    By_type, Nodal_type, coarsening_ratio, i, j, k, 0);
                amrex::Real Bz_Nodal = ablastr::coarsen::sample::Interp(Bz_arr,
                    Bz_type, Nodal_type, coarsening_ratio, i, j, k, 0);

                // Isolate U from NU
                auto tmp_Ux = (NUx_arr(i, j, k) / N_arr(i,j,k));
                auto tmp_Uy = (NUy_arr(i, j, k) / N_arr(i,j,k));
                auto tmp_Uz = (NUz_arr(i, j, k) / N_arr(i,j,k));

                // Push the fluid momentum
                UpdateMomentumHigueraCary(tmp_Ux, tmp_Uy, tmp_Uz,
                    Ex_Nodal, Ey_Nodal, Ez_Nodal,
                    Bx_Nodal, By_Nodal, Bz_Nodal, q, m, dt );

                // Calculate NU
                NUx_arr(i,j,k) = N_arr(i,j,k)*tmp_Ux;
                NUy_arr(i,j,k) = N_arr(i,j,k)*tmp_Uy;
                NUz_arr(i,j,k) = N_arr(i,j,k)*tmp_Uz;

            }
        );
    }

    // Fill guard cells
    const amrex::Geometry &geom = warpx.Geom(lev);
    const amrex::Periodicity &period = geom.periodicity();
    FillBoundary(*NU[lev][0], NU[lev][0]->nGrowVect(), WarpX::do_single_precision_comms, period);
    FillBoundary(*NU[lev][1], NU[lev][1]->nGrowVect(), WarpX::do_single_precision_comms, period);
    FillBoundary(*NU[lev][2], NU[lev][2]->nGrowVect(), WarpX::do_single_precision_comms, period);
}

void WarpXFluidContainer::DepositCharge(int lev, amrex::MultiFab &rho)
{
    WARPX_PROFILE("WarpXFluidContainer::DepositCharge");

    WarpX &warpx = WarpX::GetInstance();
    const amrex::Geometry &geom = warpx.Geom(lev);
    const amrex::Periodicity &period = geom.periodicity();
    const amrex::Real q = getCharge();
    auto const &owner_mask_rho = amrex::OwnerMask(rho, period);

    // Assertion, make sure rho is at the same location as N
    AMREX_ALWAYS_ASSERT(rho.ixType().nodeCentered());

    // Loop over and deposit charge density
    #ifdef AMREX_USE_OMP
    #pragma omp parallel if (amrex::Gpu::notInLaunchRegion())
    #endif
    for (MFIter mfi(*N[lev], TilingIfNotGPU()); mfi.isValid(); ++mfi)
    {

        amrex::Box const &tile_box = mfi.tilebox(N[lev]->ixType().toIntVect());
        amrex::Array4<Real> const &N_arr = N[lev]->array(mfi);
        amrex::Array4<amrex::Real> rho_arr = rho.array(mfi);
        auto owner_mask_rho_arr = owner_mask_rho->array(mfi);

        // Deposit Rho
        amrex::ParallelFor(tile_box,
            [=] AMREX_GPU_DEVICE(int i, int j, int k) noexcept
            {
                if ( owner_mask_rho_arr(i,j,k) ) rho_arr(i,j,k) += q*N_arr(i,j,k);
            }
        );
    }
}


void WarpXFluidContainer::DepositCurrent(
    int lev,
    amrex::MultiFab &jx, amrex::MultiFab &jy, amrex::MultiFab &jz)
{
    WARPX_PROFILE("WarpXFluidContainer::DepositCurrent");

    // Temporary nodal currents
    amrex::MultiFab tmp_jx_fluid(N[lev]->boxArray(), N[lev]->DistributionMap(), 1, 0);
    amrex::MultiFab tmp_jy_fluid(N[lev]->boxArray(), N[lev]->DistributionMap(), 1, 0);
    amrex::MultiFab tmp_jz_fluid(N[lev]->boxArray(), N[lev]->DistributionMap(), 1, 0);

    const amrex::Real inv_clight_sq = 1.0_prt / PhysConst::c / PhysConst::c;
    const amrex::Real q = getCharge();

    // Prepare interpolation of current components to cell center
    auto j_nodal_type = amrex::GpuArray<int, 3>{0, 0, 0};
    auto jx_type = amrex::GpuArray<int, 3>{0, 0, 0};
    auto jy_type = amrex::GpuArray<int, 3>{0, 0, 0};
    auto jz_type = amrex::GpuArray<int, 3>{0, 0, 0};
    for (int i = 0; i < AMREX_SPACEDIM; ++i)
    {
        j_nodal_type[i] = tmp_jx_fluid.ixType()[i];
        jx_type[i] = jx.ixType()[i];
        jy_type[i] = jy.ixType()[i];
        jz_type[i] = jz.ixType()[i];
    }

    // We now need to create a mask to fix the double counting.
    WarpX &warpx = WarpX::GetInstance();
    const amrex::Geometry &geom = warpx.Geom(lev);
    const amrex::Periodicity &period = geom.periodicity();
    auto const &owner_mask_x = amrex::OwnerMask(jx, period);
    auto const &owner_mask_y = amrex::OwnerMask(jy, period);
    auto const &owner_mask_z = amrex::OwnerMask(jz, period);

    // Calculate j at the nodes
    #ifdef AMREX_USE_OMP
    #pragma omp parallel if (amrex::Gpu::notInLaunchRegion())
    #endif
    for (MFIter mfi(*N[lev], TilingIfNotGPU()); mfi.isValid(); ++mfi)
    {
        amrex::Box const &tile_box = mfi.tilebox(N[lev]->ixType().toIntVect());

        amrex::Array4<Real> const &N_arr = N[lev]->array(mfi);
        amrex::Array4<Real> const &NUx_arr = NU[lev][0]->array(mfi);
        amrex::Array4<Real> const &NUy_arr = NU[lev][1]->array(mfi);
        amrex::Array4<Real> const &NUz_arr = NU[lev][2]->array(mfi);

        amrex::Array4<amrex::Real> tmp_jx_fluid_arr = tmp_jx_fluid.array(mfi);
        amrex::Array4<amrex::Real> tmp_jy_fluid_arr = tmp_jy_fluid.array(mfi);
        amrex::Array4<amrex::Real> tmp_jz_fluid_arr = tmp_jz_fluid.array(mfi);

        amrex::ParallelFor(tile_box,
            [=] AMREX_GPU_DEVICE(int i, int j, int k) noexcept
            {
                // Calculate J from fluid quantities
                auto gamma = std::sqrt(N_arr(i, j, k) * N_arr(i, j, k) + (NUx_arr(i, j, k) * NUx_arr(i, j, k) + NUy_arr(i, j, k) * NUy_arr(i, j, k) + NUz_arr(i, j, k) * NUz_arr(i, j, k)) * inv_clight_sq) / N_arr(i, j, k);
                tmp_jx_fluid_arr(i, j, k) = q * (NUx_arr(i, j, k) / gamma);
                tmp_jy_fluid_arr(i, j, k) = q * (NUy_arr(i, j, k) / gamma);
                tmp_jz_fluid_arr(i, j, k) = q * (NUz_arr(i, j, k) / gamma);
            }
        );
    }

    // Interpolate j from the nodes to the simulation mesh (typically Yee mesh)
    #ifdef AMREX_USE_OMP
    #pragma omp parallel if (amrex::Gpu::notInLaunchRegion())
    #endif
    for (MFIter mfi(*N[lev], TilingIfNotGPU()); mfi.isValid(); ++mfi)
    {
        amrex::Box const &tile_box_x = mfi.tilebox(jx.ixType().toIntVect());
        amrex::Box const &tile_box_y = mfi.tilebox(jy.ixType().toIntVect());
        amrex::Box const &tile_box_z = mfi.tilebox(jz.ixType().toIntVect());

        amrex::Array4<amrex::Real> jx_arr = jx.array(mfi);
        amrex::Array4<amrex::Real> jy_arr = jy.array(mfi);
        amrex::Array4<amrex::Real> jz_arr = jz.array(mfi);

        amrex::Array4<amrex::Real> tmp_jx_fluid_arr = tmp_jx_fluid.array(mfi);
        amrex::Array4<amrex::Real> tmp_jy_fluid_arr = tmp_jy_fluid.array(mfi);
        amrex::Array4<amrex::Real> tmp_jz_fluid_arr = tmp_jz_fluid.array(mfi);

        auto owner_mask_x_arr = owner_mask_x->array(mfi);
        auto owner_mask_y_arr = owner_mask_y->array(mfi);
        auto owner_mask_z_arr = owner_mask_z->array(mfi);

        // When using the `Interp` function, one needs to specify whether coarsening is desired.
        // Here, we do not perform any coarsening.
        amrex::GpuArray<int, 3U> coarsening_ratio = {AMREX_D_DECL(1, 1, 1)};

        // Interpolate fluid current and deposit it
        // ( mask double counting )
        amrex::ParallelFor( tile_box_x, tile_box_y, tile_box_z,
            [=] AMREX_GPU_DEVICE(int i, int j, int k) noexcept
            {
                amrex::Real jx_tmp = ablastr::coarsen::sample::Interp(tmp_jx_fluid_arr,
                    j_nodal_type, jx_type, coarsening_ratio, i, j, k, 0);
                if ( owner_mask_x_arr(i,j,k) ) jx_arr(i, j, k) += jx_tmp;
            },
            [=] AMREX_GPU_DEVICE(int i, int j, int k) noexcept
            {
                amrex::Real jy_tmp = ablastr::coarsen::sample::Interp(tmp_jy_fluid_arr,
                    j_nodal_type, jy_type, coarsening_ratio, i, j, k, 0);
                if ( owner_mask_y_arr(i,j,k) ) jy_arr(i, j, k) += jy_tmp;
            },
            [=] AMREX_GPU_DEVICE(int i, int j, int k) noexcept
            {
                amrex::Real jz_tmp = ablastr::coarsen::sample::Interp(tmp_jz_fluid_arr,
                    j_nodal_type, jz_type, coarsening_ratio, i, j, k, 0);
                if ( owner_mask_z_arr(i,j,k) ) jz_arr(i, j, k) += jz_tmp;
            }
        );
    }
}