/*
 scph.cpp

 Copyright (c) 2015 Terumasa Tadano

 This file is distributed under the terms of the MIT license.
 Please see the file 'LICENCE.txt' in the root directory 
 or http://opensource.org/licenses/mit-license.php for information.
*/

#include "mpi_common.h"
#include "scph.h"
#include "dynamical.h"
#include "kpoint.h"
#include "anharmonic_core.h"
#include "dielec.h"
#include "ewald.h"
#include "memory.h"
#include "thermodynamics.h"
#include "write_phonons.h"
#include "constants.h"
#include "system.h"
#include "error.h"
#include "gruneisen.h"
#include "mathfunctions.h"
#include "integration.h"
#include "parsephon.h"
#include "phonon_dos.h"
#include "symmetry_core.h"
#include "fcs_phonon.h"
#include <iostream>
#include <iomanip>
#include <complex>
#include <algorithm>
#include <fftw3.h>
#include <Eigen/Core>
#include <Eigen/Eigenvalues>
#include <boost/lexical_cast.hpp>
#include "timer.h"
#include <cmath>
#include <cstdlib>
#include <vector>

#if defined(WIN32) || defined(_WIN32)
#pragma comment(lib, "libfftw3-3.lib")
#pragma comment(lib, "libfftw3f-3.lib")
#pragma comment(lib, "libfftw3l-3.lib")
#endif

using namespace PHON_NS;

Scph::Scph(PHON *phon) : Pointers(phon)
{
    set_default_variables();
}

Scph::~Scph()
{
    deallocate_variables();
}

void Scph::set_default_variables()
{
    restart_scph = false;
    warmstart_scph = false;
    lower_temp = true;
    tolerance_scph = 1.0e-10;
    mixalpha = 0.1;
    maxiter = 100;
    print_self_consistent_fc2 = false;
    selfenergy_offdiagonal = true;

    // variables related to the structural optimization
    relax_coordinate = 0;
    relax_algo = 2;
    max_str_iter = 100;
    set_init_str = 0;
    coord_conv_tol = 0.001;
    mixbeta_coord = 0.5;
    alpha_steepest_decent = 1.0e4; 

    kmap_interpolate_to_scph = nullptr;
    evec_harmonic = nullptr;
    omega2_harmonic = nullptr;
    mat_transform_sym = nullptr;
    symop_minus_at_k = nullptr;
    kpoint_map_symmetry = nullptr;
    mindist_list_scph = nullptr;

    bubble = 0;
    phi3_reciprocal = nullptr;
    phi4_reciprocal = nullptr;
    compute_Cv_anharmonic = 0;

    kmesh_coarse = nullptr;
    kmesh_dense = nullptr;

    phase_factor_scph = nullptr;
}

void Scph::deallocate_variables()
{
    if (kmap_interpolate_to_scph) {
        deallocate(kmap_interpolate_to_scph);
    }
    if (mindist_list_scph) {
        deallocate(mindist_list_scph);
    }
    if (evec_harmonic) {
        deallocate(evec_harmonic);
    }
    if (omega2_harmonic) {
        deallocate(omega2_harmonic);
    }
    if (mat_transform_sym) {
        deallocate(mat_transform_sym);
    }
    if (symop_minus_at_k) {
        deallocate(symop_minus_at_k);
    }
    if (kpoint_map_symmetry) {
        deallocate(kpoint_map_symmetry);
    }
    if (phi3_reciprocal) {
        deallocate(phi3_reciprocal);
    }
    if (phi4_reciprocal) {
        deallocate(phi4_reciprocal);
    }

    if (kmesh_coarse) delete kmesh_coarse;
    if (kmesh_dense) delete kmesh_dense;

    if (phase_factor_scph) delete phase_factor_scph;
}

void Scph::setup_scph()
{
    //  MPI_Bcast(&relax_coordinate, 1, MPI_LOGICAL, 0, MPI_COMM_WORLD);
    MPI_Bcast(&relax_coordinate, 1, MPI_INTEGER, 0, MPI_COMM_WORLD);
    MPI_Bcast(&bubble, 1, MPI_UNSIGNED, 0, MPI_COMM_WORLD);

    setup_kmesh();
    setup_eigvecs();
    setup_transform_ifc();
    setup_pp_interaction();
    setup_transform_symmetry();
}

void Scph::exec_scph()
{
    const auto ns = dynamical->neval;
    const auto Tmin = system->Tmin;
    const auto Tmax = system->Tmax;
    const auto dT = system->dT;

    std::complex<double> ****delta_dymat_scph = nullptr;
    std::complex<double> ****delta_dymat_scph_plus_bubble = nullptr;
    // change of harmonic dymat by IFC renormalization
    std::complex<double> ****delta_harmonic_dymat_renormalize = nullptr; 

    const auto NT = static_cast<unsigned int>((Tmax - Tmin) / dT) + 1;

    MPI_Bcast(&restart_scph, 1, MPI_CXX_BOOL, 0, MPI_COMM_WORLD);
    MPI_Bcast(&selfenergy_offdiagonal, 1, MPI_CXX_BOOL, 0, MPI_COMM_WORLD);
    MPI_Bcast(&ialgo, 1, MPI_UNSIGNED, 0, MPI_COMM_WORLD);

    allocate(delta_dymat_scph, NT, ns, ns, kmesh_coarse->nk);
    allocate(delta_harmonic_dymat_renormalize, NT, ns, ns, kmesh_coarse->nk);

    // if(!relax_coordinate){
        zerofill_harmonic_dymat_renormalize(delta_harmonic_dymat_renormalize, NT);
    //}

    if (restart_scph) {

        // Read anharmonic correction to the dynamical matrix from the existing file
        load_scph_dymat_from_file(delta_dymat_scph, input->job_title + ".scph_dymat");
        load_scph_dymat_from_file(delta_harmonic_dymat_renormalize, input->job_title + ".renorm_harm_dymat");

    } else {

        if (dynamical->nonanalytic == 3) {
            exit("exec_scph",
                 "Sorry, NONANALYTIC=3 can't be used for the main loop of the SCPH calculation.");
        }
        // Solve the SCPH equation and obtain the correction to the dynamical matrix
        if(relax_coordinate == 0){
            exec_scph_main(delta_dymat_scph);
        }
        // Calculate SCPH + structural optimization and obtain the correction to the dynamical matrix
        else if(relax_coordinate == 1){
            exec_scph_relax_main(delta_dymat_scph, delta_harmonic_dymat_renormalize);
        }
        else{
            exec_scph_relax_cell_coordinate_main(delta_dymat_scph, delta_harmonic_dymat_renormalize);
        }

        // print_distance_harmonic_IFC();
        // return;

        if (mympi->my_rank == 0) {
            // write dymat for file
            store_scph_dymat_to_file(delta_dymat_scph, input->job_title + ".scph_dymat");
            store_scph_dymat_to_file(delta_harmonic_dymat_renormalize, input->job_title + ".renorm_harm_dymat");
            write_anharmonic_correction_fc2(delta_dymat_scph, NT);
        }
    }

    if (kpoint->kpoint_mode == 2) {
        if (thermodynamics->calc_FE_bubble) {
            compute_free_energy_bubble_SCPH(kmesh_interpolate,
                                            delta_dymat_scph);
        }
    }

    if (bubble) {
        allocate(delta_dymat_scph_plus_bubble, NT, ns, ns, kmesh_coarse->nk);
        bubble_correction(delta_dymat_scph,
                          delta_dymat_scph_plus_bubble);
        if (mympi->my_rank == 0) {
            write_anharmonic_correction_fc2(delta_dymat_scph_plus_bubble, NT, bubble);
        }
    }

    postprocess(delta_dymat_scph,
                delta_harmonic_dymat_renormalize,
                delta_dymat_scph_plus_bubble);

    deallocate(delta_dymat_scph);
    deallocate(delta_harmonic_dymat_renormalize);
    if (delta_dymat_scph_plus_bubble) deallocate(delta_dymat_scph_plus_bubble);

}

void Scph::postprocess(std::complex<double> ****delta_dymat_scph,
                       std::complex<double> ****delta_harmonic_dymat_renormalize,
                       std::complex<double> ****delta_dymat_scph_plus_bubble)
{
    double ***eval_anharm = nullptr;
    double ***eval_harm_renormalized = nullptr;
    const auto ns = dynamical->neval;
    const auto Tmin = system->Tmin;
    const auto Tmax = system->Tmax;
    const auto dT = system->dT;
    const auto NT = static_cast<unsigned int>((Tmax - Tmin) / dT) + 1;

    unsigned int nomega_dielec;

    if (mympi->my_rank == 0) {

        std::cout << '\n';
        std::cout << " Running postprocess of SCPH (calculation of free energy, MSD, DOS)" << std::endl;
        std::cout << " The number of temperature points: " << std::setw(4) << NT << std::endl;
        std::cout << "   ";

        std::complex<double> ***evec_tmp = nullptr;
        std::complex<double> ***evec_harm_renormalized_tmp = nullptr;
        double **eval_gam = nullptr;
        std::complex<double> ***evec_gam = nullptr;
        double **xk_gam = nullptr;

        double **dos_scph = nullptr;
        double ***pdos_scph = nullptr;
        double *heat_capacity = nullptr;
        double *heat_capacity_correction = nullptr;
        double *FE_QHA = nullptr;
        double *dFE_scph = nullptr;
        double **msd_scph = nullptr;
        double ***ucorr_scph = nullptr;
        double ****dielec_scph = nullptr;
        double *omega_grid = nullptr;
        double **domega_dt = nullptr;

        if (dos->kmesh_dos) {
            allocate(eval_anharm, NT, dos->kmesh_dos->nk, ns);
            allocate(evec_tmp, dos->kmesh_dos->nk, ns, ns);
            allocate(eval_harm_renormalized, NT, dos->kmesh_dos->nk, ns);
            allocate(evec_harm_renormalized_tmp, dos->kmesh_dos->nk, ns, ns);

            if (dos->compute_dos) {
                allocate(dos_scph, NT, dos->n_energy);

                if (dos->projected_dos) {
                    allocate(pdos_scph, NT, ns, dos->n_energy);
                }
            }
            allocate(heat_capacity, NT);
            allocate(FE_QHA, NT);
            allocate(dFE_scph, NT);

            if (writes->getPrintMSD()) {
                allocate(msd_scph, NT, ns);
            }
            if (writes->getPrintUcorr()) {
                allocate(ucorr_scph, NT, ns, ns);
            }
            if (compute_Cv_anharmonic) {
                allocate(heat_capacity_correction, NT);
                allocate(domega_dt, dos->kmesh_dos->nk, ns);
                if (compute_Cv_anharmonic == 1) {
                    // Use central difference to evaluate temperature derivative of
                    // anharmonic frequencies
                    heat_capacity_correction[0] = 0.0;
                    heat_capacity_correction[NT - 1] = 0.0;
                }
            }

            for (auto iT = 0; iT < NT; ++iT) {
                auto temperature = Tmin + dT * static_cast<double>(iT);

                exec_interpolation(kmesh_interpolate,
                                   delta_dymat_scph[iT],
                                   dos->kmesh_dos->nk,
                                   dos->kmesh_dos->xk,
                                   dos->kmesh_dos->kvec_na,
                                   eval_anharm[iT],
                                   evec_tmp);

                exec_interpolation(kmesh_interpolate,
                                   delta_harmonic_dymat_renormalize[iT],
                                   dos->kmesh_dos->nk,
                                   dos->kmesh_dos->xk,
                                   dos->kmesh_dos->kvec_na,
                                   eval_harm_renormalized[iT],
                                   evec_harm_renormalized_tmp);

                if (dos->compute_dos) {
                    dos->calc_dos_from_given_frequency(dos->kmesh_dos,
                                                       eval_anharm[iT],
                                                       dos->tetra_nodes_dos->get_ntetra(),
                                                       dos->tetra_nodes_dos->get_tetras(),
                                                       dos_scph[iT]);
                }

                heat_capacity[iT] = thermodynamics->Cv_tot(temperature,
                                                           dos->kmesh_dos->nk_irred,
                                                           ns,
                                                           dos->kmesh_dos->kpoint_irred_all,
                                                           &dos->kmesh_dos->weight_k[0],
                                                           eval_anharm[iT]);

                FE_QHA[iT] = thermodynamics->free_energy_QHA(temperature,
                                                             dos->kmesh_dos->nk_irred,
                                                             ns,
                                                             dos->kmesh_dos->kpoint_irred_all,
                                                             &dos->kmesh_dos->weight_k[0],
                                                             eval_anharm[iT]);

                dFE_scph[iT] = thermodynamics->FE_scph_correction(iT,
                                                                  eval_anharm[iT],
                                                                  evec_tmp,
                                                                  eval_harm_renormalized[iT],
                                                                  evec_harm_renormalized_tmp);

                if (writes->getPrintMSD()) {
                    double shift[3]{0.0, 0.0, 0.0};

                    for (auto is = 0; is < ns; ++is) {
                        msd_scph[iT][is] = thermodynamics->disp_corrfunc(temperature,
                                                                         is, is,
                                                                         shift,
                                                                         dos->kmesh_dos->nk,
                                                                         ns,
                                                                         dos->kmesh_dos->xk,
                                                                         eval_anharm[iT],
                                                                         evec_tmp);
                    }
                }

                if (writes->getPrintUcorr()) {
                    double shift[3];
                    for (auto i = 0; i < 3; ++i) shift[i] = static_cast<double>(writes->getShiftUcorr()[i]);

                    for (auto is = 0; is < ns; ++is) {
                        for (auto js = 0; js < ns; ++js) {
                            ucorr_scph[iT][is][js] = thermodynamics->disp_corrfunc(temperature,
                                                                                   is, js,
                                                                                   shift,
                                                                                   dos->kmesh_dos->nk,
                                                                                   ns,
                                                                                   dos->kmesh_dos->xk,
                                                                                   eval_anharm[iT],
                                                                                   evec_tmp);
                        }
                    }
                }

                if (compute_Cv_anharmonic == 1) {

                    if (iT >= 1 and iT <= NT - 2) {
                        get_derivative_central_diff(dT, dos->kmesh_dos->nk,
                                                    eval_anharm[iT - 1],
                                                    eval_anharm[iT + 1],
                                                    domega_dt);

                        heat_capacity_correction[iT] = thermodynamics->Cv_anharm_correction(temperature,
                                                                                            dos->kmesh_dos->nk_irred,
                                                                                            ns,
                                                                                            dos->kmesh_dos->kpoint_irred_all,
                                                                                            &dos->kmesh_dos->weight_k[0],
                                                                                            eval_anharm[iT],
                                                                                            domega_dt);
                    }

                }

                std::cout << '.' << std::flush;
                if (iT % 25 == 24) {
                    std::cout << std::endl;
                    std::cout << std::setw(3);
                }
            }
            std::cout << "\n\n";

            if (dos->compute_dos) {
                writes->write_scph_dos(dos_scph);
            }
            writes->write_scph_thermodynamics(heat_capacity,
                                              heat_capacity_correction,
                                              FE_QHA,
                                              dFE_scph);
            if (writes->getPrintMSD()) {
                writes->write_scph_msd(msd_scph);
            }
            if (writes->getPrintUcorr()) {
                writes->write_scph_ucorr(ucorr_scph);
            }

            // If delta_dymat_scph_plus_bubble != nullptr, run postprocess again with
            // delta_dymat_scph_plus_bubble.
            if (bubble > 0) {
                std::cout << std::endl;
                std::cout << "   ";

                for (auto iT = 0; iT < NT; ++iT) {
                    auto temperature = Tmin + dT * static_cast<double>(iT);

                    exec_interpolation(kmesh_interpolate,
                                       delta_dymat_scph_plus_bubble[iT],
                                       dos->kmesh_dos->nk,
                                       dos->kmesh_dos->xk,
                                       dos->kmesh_dos->kvec_na,
                                       eval_anharm[iT],
                                       evec_tmp);

                    if (dos->compute_dos) {
                        dos->calc_dos_from_given_frequency(dos->kmesh_dos,
                                                           eval_anharm[iT],
                                                           dos->tetra_nodes_dos->get_ntetra(),
                                                           dos->tetra_nodes_dos->get_tetras(),
                                                           dos_scph[iT]);
                    }

                    heat_capacity[iT] = thermodynamics->Cv_tot(temperature,
                                                               dos->kmesh_dos->nk_irred,
                                                               ns,
                                                               dos->kmesh_dos->kpoint_irred_all,
                                                               &dos->kmesh_dos->weight_k[0],
                                                               eval_anharm[iT]);

                    if (writes->getPrintMSD()) {
                        double shift[3]{0.0, 0.0, 0.0};

                        for (auto is = 0; is < ns; ++is) {
                            msd_scph[iT][is] = thermodynamics->disp_corrfunc(temperature,
                                                                             is, is,
                                                                             shift,
                                                                             dos->kmesh_dos->nk,
                                                                             ns,
                                                                             dos->kmesh_dos->xk,
                                                                             eval_anharm[iT],
                                                                             evec_tmp);
                        }
                    }

                    if (writes->getPrintUcorr()) {
                        double shift[3];
                        for (auto i = 0; i < 3; ++i) shift[i] = static_cast<double>(writes->getShiftUcorr()[i]);

                        for (auto is = 0; is < ns; ++is) {
                            for (auto js = 0; js < ns; ++js) {
                                ucorr_scph[iT][is][js] = thermodynamics->disp_corrfunc(temperature,
                                                                                       is, js,
                                                                                       shift,
                                                                                       dos->kmesh_dos->nk,
                                                                                       ns,
                                                                                       dos->kmesh_dos->xk,
                                                                                       eval_anharm[iT],
                                                                                       evec_tmp);
                            }
                        }
                    }

                    std::cout << '.' << std::flush;
                    if (iT % 25 == 24) {
                        std::cout << std::endl;
                        std::cout << std::setw(3);
                    }
                }

                std::cout << "\n\n";

                if (dos->compute_dos) {
                    writes->write_scph_dos(dos_scph, bubble);
                }
                if (writes->getPrintMSD()) {
                    writes->write_scph_msd(msd_scph, bubble);
                }
                if (writes->getPrintUcorr()) {
                    writes->write_scph_ucorr(ucorr_scph, bubble);
                }

            }
            deallocate(eval_anharm);
            eval_anharm = nullptr;
            deallocate(evec_tmp);
            evec_tmp = nullptr;
        }

        if (kpoint->kpoint_general) {
            allocate(eval_anharm, NT, kpoint->kpoint_general->nk, ns);
            allocate(evec_tmp, kpoint->kpoint_general->nk, ns, ns);

            for (auto iT = 0; iT < NT; ++iT) {
                exec_interpolation(kmesh_interpolate,
                                   delta_dymat_scph[iT],
                                   kpoint->kpoint_general->nk,
                                   kpoint->kpoint_general->xk,
                                   kpoint->kpoint_general->kvec_na,
                                   eval_anharm[iT],
                                   evec_tmp);
            }

            writes->write_scph_energy(kpoint->kpoint_general->nk,
                                      eval_anharm);

            if (bubble > 0) {
                for (auto iT = 0; iT < NT; ++iT) {
                    exec_interpolation(kmesh_interpolate,
                                       delta_dymat_scph_plus_bubble[iT],
                                       kpoint->kpoint_general->nk,
                                       kpoint->kpoint_general->xk,
                                       kpoint->kpoint_general->kvec_na,
                                       eval_anharm[iT],
                                       evec_tmp);
                }
                writes->write_scph_energy(kpoint->kpoint_general->nk,
                                          eval_anharm,
                                          bubble);
            }
            deallocate(eval_anharm);
            deallocate(evec_tmp);
            eval_anharm = nullptr;
            evec_tmp = nullptr;
        }

        if (kpoint->kpoint_bs) {
            allocate(eval_anharm, NT, kpoint->kpoint_bs->nk, ns);
            allocate(evec_tmp, kpoint->kpoint_bs->nk, ns, ns);

            for (auto iT = 0; iT < NT; ++iT) {
                exec_interpolation(kmesh_interpolate,
                                   delta_dymat_scph[iT],
                                   kpoint->kpoint_bs->nk,
                                   kpoint->kpoint_bs->xk,
                                   kpoint->kpoint_bs->kvec_na,
                                   eval_anharm[iT],
                                   evec_tmp);
            }

            writes->write_scph_bands(kpoint->kpoint_bs->nk,
                                     kpoint->kpoint_bs->kaxis,
                                     eval_anharm);

            if (bubble > 0) {
                for (auto iT = 0; iT < NT; ++iT) {
                    exec_interpolation(kmesh_interpolate,
                                       delta_dymat_scph_plus_bubble[iT],
                                       kpoint->kpoint_bs->nk,
                                       kpoint->kpoint_bs->xk,
                                       kpoint->kpoint_bs->kvec_na,
                                       eval_anharm[iT],
                                       evec_tmp);
                }
                writes->write_scph_bands(kpoint->kpoint_bs->nk,
                                         kpoint->kpoint_bs->kaxis,
                                         eval_anharm,
                                         bubble);
            }
            deallocate(eval_anharm);
            deallocate(evec_tmp);
            eval_anharm = nullptr;
            evec_tmp = nullptr;
        }

        if (dielec->calc_dielectric_constant) {
            omega_grid = dielec->get_omega_grid(nomega_dielec);
            allocate(dielec_scph, NT, nomega_dielec, 3, 3);
            allocate(eval_gam, 1, ns);
            allocate(evec_gam, 1, ns, ns);
            allocate(xk_gam, 1, 3);
            for (auto i = 0; i < 3; ++i) xk_gam[0][i] = 0.0;

            for (auto iT = 0; iT < NT; ++iT) {
                exec_interpolation(kmesh_interpolate,
                                   delta_dymat_scph[iT],
                                   1,
                                   xk_gam,
                                   xk_gam,
                                   eval_gam,
                                   evec_gam);

                for (auto is = 0; is < ns; ++is) {
                    if (eval_gam[0][is] < 0.0) {
                        eval_gam[0][is] = -std::pow(eval_gam[0][is], 2.0);
                    } else {
                        eval_gam[0][is] = std::pow(eval_gam[0][is], 2.0);
                    }
                }
                dielec->compute_dielectric_function(nomega_dielec,
                                                    omega_grid,
                                                    eval_gam[0],
                                                    evec_gam[0],
                                                    dielec_scph[iT]);
            }
            writes->write_scph_dielec(dielec_scph);
        }

        if (eval_anharm) deallocate(eval_anharm);
        if (evec_tmp) deallocate(evec_tmp);

        if (dos_scph) deallocate(dos_scph);
        if (pdos_scph) deallocate(pdos_scph);
        if (heat_capacity) deallocate(heat_capacity);
        if (heat_capacity_correction) deallocate(heat_capacity_correction);
        if (FE_QHA) deallocate(FE_QHA);
        if (dFE_scph) deallocate(dFE_scph);
        if (dielec_scph) deallocate(dielec_scph);

        if (eval_gam) deallocate(eval_gam);
        if (evec_gam) deallocate(evec_gam);
        if (xk_gam) deallocate(xk_gam);
    }
}

void Scph::load_scph_dymat_from_file(std::complex<double> ****dymat_out,
                                     std::string filename_dymat)
{
    const auto ns = dynamical->neval;
    const auto Tmin = system->Tmin;
    const auto Tmax = system->Tmax;
    const auto dT = system->dT;
    const auto NT = static_cast<unsigned int>((Tmax - Tmin) / dT) + 1;
    std::vector<double> Temp_array(NT);

    for (int i = 0; i < NT; ++i) {
        Temp_array[i] = Tmin + dT * static_cast<double>(i);
    }

    if (mympi->my_rank == 0) {

        const auto consider_offdiagonal = selfenergy_offdiagonal;
        double temp;
        std::ifstream ifs_dymat;
        // auto file_dymat = input->job_title + ".scph_dymat";
        auto file_dymat = filename_dymat;
        bool consider_offdiag_tmp;
        unsigned int nk_interpolate_ref[3];
        unsigned int nk_scph_tmp[3];
        double Tmin_tmp, Tmax_tmp, dT_tmp;
        double dymat_real, dymat_imag;
        std::string str_dummy;
        int nonanalytic_tmp;

        std::cout << " RESTART_SCPH is true." << std::endl;
        std::cout << " Dynamical matrix is read from file ...";

        ifs_dymat.open(file_dymat.c_str(), std::ios::in);

        if (!ifs_dymat) {
            exit("load_scph_dymat_from_file",
                 "Cannot open scph_dymat file");
        }

        // Read computational settings from file and check the consistency.
        ifs_dymat >> nk_interpolate_ref[0] >> nk_interpolate_ref[1] >> nk_interpolate_ref[2];
        ifs_dymat >> nk_scph_tmp[0] >> nk_scph_tmp[1] >> nk_scph_tmp[2];
        ifs_dymat >> Tmin_tmp >> Tmax_tmp >> dT_tmp;
        ifs_dymat >> nonanalytic_tmp >> consider_offdiag_tmp;

        if (nk_interpolate_ref[0] != kmesh_interpolate[0] ||
            nk_interpolate_ref[1] != kmesh_interpolate[1] ||
            nk_interpolate_ref[2] != kmesh_interpolate[2]) {
            exit("load_scph_dymat_from_file",
                 "The number of KMESH_INTERPOLATE is not consistent");
        }
        if (nk_scph_tmp[0] != kmesh_scph[0] ||
            nk_scph_tmp[1] != kmesh_scph[1] ||
            nk_scph_tmp[2] != kmesh_scph[2]) {
            exit("load_scph_dymat_from_file",
                 "The number of KMESH_SCPH is not consistent");
        }
        if (nonanalytic_tmp != dynamical->nonanalytic) {
            warn("load_scph_dymat_from_file",
                 "The NONANALYTIC tag is not consistent");
        }
        if (consider_offdiag_tmp != consider_offdiagonal) {
            exit("load_scph_dymat_from_file",
                 "The SELF_OFFDIAG tag is not consistent");
        }

        // Check if the precalculated data for the given temperature range exists
        const auto NT_ref = static_cast<unsigned int>((Tmax_tmp - Tmin_tmp) / dT_tmp) + 1;
        std::vector<double> Temp_array_ref(NT_ref);
        for (int i = 0; i < NT_ref; ++i) {
            Temp_array_ref[i] = Tmin_tmp + dT_tmp * static_cast<double>(i);
        }
        std::vector<int> flag_load(NT_ref);
        for (int i = 0; i < NT_ref; ++i) {
            flag_load[i] = 0;
            for (int j = 0; j < NT; ++j) {
                if (std::abs(Temp_array_ref[i] - Temp_array[j]) < eps6) {
                    flag_load[i] = 1;
                    break;
                }
            }
        }
        int icount = 0;
        for (int iT = 0; iT < NT_ref; ++iT) {
            ifs_dymat >> str_dummy >> temp;
            for (int is = 0; is < ns; ++is) {
                for (int js = 0; js < ns; ++js) {
                    for (int ik = 0; ik < kmesh_coarse->nk; ++ik) {
                        ifs_dymat >> dymat_real >> dymat_imag;
                        if (flag_load[iT]) {
                            dymat_out[icount][is][js][ik]
                                    = std::complex<double>(dymat_real, dymat_imag);
                        }
                    }
                }
            }
            if (flag_load[iT]) icount += 1;
        }

        ifs_dymat.close();

        if (icount != NT) {
            exit("load_scph_dymat_from_file",
                 "The temperature information is not consistent");
        }
        std::cout << " done." << std::endl;
    }
    // Broadcast to all MPI threads
    mpi_bcast_complex(dymat_out, NT, kmesh_coarse->nk, ns);
}

void Scph::store_scph_dymat_to_file(const std::complex<double> *const *const *const *dymat_in,
                                    std::string filename_dymat)
{
    int i;
    const auto ns = dynamical->neval;
    const auto Tmin = system->Tmin;
    const auto Tmax = system->Tmax;
    const auto dT = system->dT;
    std::ofstream ofs_dymat;
    // auto file_dymat = input->job_title + ".scph_dymat";
    auto file_dymat = filename_dymat;

    const auto NT = static_cast<unsigned int>((Tmax - Tmin) / dT) + 1;

    ofs_dymat.open(file_dymat.c_str(), std::ios::out);

    if (!ofs_dymat) {
        exit("store_scph_dymat_to_file",
             "Cannot open scph_dymat file");
    }
    for (i = 0; i < 3; ++i) {
        ofs_dymat << std::setw(5) << kmesh_interpolate[i];
    }
    ofs_dymat << std::endl;
    for (i = 0; i < 3; ++i) {
        ofs_dymat << std::setw(5) << kmesh_scph[i];
    }
    ofs_dymat << std::endl;
    ofs_dymat << std::setw(10) << Tmin;
    ofs_dymat << std::setw(10) << Tmax;
    ofs_dymat << std::setw(10) << dT << std::endl;
    ofs_dymat << std::setw(5) << dynamical->nonanalytic;
    ofs_dymat << std::setw(5) << selfenergy_offdiagonal << std::endl;

    for (auto iT = 0; iT < NT; ++iT) {
        const auto temp = Tmin + static_cast<double>(iT) * dT;
        ofs_dymat << "# " << temp << std::endl;
        for (auto is = 0; is < ns; ++is) {
            for (auto js = 0; js < ns; ++js) {
                for (auto ik = 0; ik < kmesh_coarse->nk; ++ik) {
                    ofs_dymat << std::setprecision(15)
                              << std::setw(25) << dymat_in[iT][is][js][ik].real();
                    ofs_dymat << std::setprecision(15)
                              << std::setw(25) << dymat_in[iT][is][js][ik].imag();
                    ofs_dymat << std::endl;
                }
            }
        }
    }
    ofs_dymat.close();
    std::cout << "  " << std::setw(input->job_title.length() + 12) << std::left << file_dymat;
    std::cout << " : Anharmonic dynamical matrix (restart file)" << std::endl;
}

void Scph::zerofill_harmonic_dymat_renormalize(std::complex<double> ****delta_harmonic_dymat_renormalize, 
                                               unsigned int NT)
{
    const auto ns = dynamical->neval;
    static auto complex_zero = std::complex<double>(0.0, 0.0);
    int iT, is1, is2, ik;

    for(iT = 0; iT < NT; iT++){
        for(is1 = 0; is1 < ns; is1++){
            for(is2 = 0; is2 < ns; is2++){
                for(ik = 0; ik < kmesh_coarse->nk; ik++){
                    delta_harmonic_dymat_renormalize[iT][is1][is2][ik] = complex_zero;
                }
            }
        }
    }
}

void Scph::exec_scph_main(std::complex<double> ****dymat_anharm)
{
    int ik, is;
    const auto nk = kmesh_dense->nk;
    const auto nk_interpolate = kmesh_coarse->nk;
    const auto ns = dynamical->neval;
    const auto nk_irred_interpolate = kmesh_coarse->nk_irred;
    const auto Tmin = system->Tmin;
    const auto Tmax = system->Tmax;
    const auto dT = system->dT;
    double ***omega2_anharm;
    std::complex<double> ***evec_anharm_tmp;
    std::complex<double> ***v3_array_all;
    std::complex<double> ***v4_array_all;

    std::complex<double> **delta_v2_array_renormalize;

    std::vector<double> vec_temp;

    const auto NT = static_cast<unsigned int>((Tmax - Tmin) / dT) + 1;

    // Compute matrix element of 4-phonon interaction

    allocate(omega2_anharm, NT, nk, ns);
    allocate(evec_anharm_tmp, nk, ns, ns);
    allocate(v4_array_all, nk_irred_interpolate * nk,
             ns * ns, ns * ns);

    // delta_v2_array_renormalize is zero when structural optimization is not performed
    allocate(delta_v2_array_renormalize, nk_interpolate, ns*ns);
    for(ik = 0; ik < nk_interpolate; ik++){
        for(is = 0; is < ns*ns; is++){
            delta_v2_array_renormalize[ik][is] = 0.0;
        }
    }

    // Calculate v4 array. 
    // This operation is the most expensive part of the calculation.
    if (selfenergy_offdiagonal & (ialgo == 1)) {
        compute_V4_elements_mpi_over_band(v4_array_all,
                                          evec_harmonic,
                                          selfenergy_offdiagonal);
    } else {
        compute_V4_elements_mpi_over_kpoint(v4_array_all,
                                            evec_harmonic,
                                            selfenergy_offdiagonal,
                                            relax_coordinate);
    }

    if (relax_coordinate) {
        allocate(v3_array_all, nk, ns, ns * ns);
        compute_V3_elements_mpi_over_kpoint(v3_array_all,
                                            evec_harmonic,
                                            selfenergy_offdiagonal);
    }

    if (mympi->my_rank == 0) {

        std::complex<double> ***cmat_convert;
        allocate(cmat_convert, nk, ns, ns);

        vec_temp.clear();

        if (lower_temp) {
            for (int i = NT - 1; i >= 0; --i) {
                vec_temp.push_back(Tmin + static_cast<double>(i) * dT);
            }
        } else {
            for (int i = 0; i < NT; ++i) {
                vec_temp.push_back(Tmin + static_cast<double>(i) * dT);
            }
        }

        auto converged_prev = false;

        for (double temp: vec_temp) {
            auto iT = static_cast<unsigned int>((temp - Tmin) / dT);

            // Initialize phonon eigenvectors with harmonic values

            for (ik = 0; ik < nk; ++ik) {
                for (is = 0; is < ns; ++is) {
                    for (int js = 0; js < ns; ++js) {
                        evec_anharm_tmp[ik][is][js] = evec_harmonic[ik][is][js];
                    }
                }
            }
            if (converged_prev) {
                if (lower_temp) {
                    for (ik = 0; ik < nk; ++ik) {
                        for (is = 0; is < ns; ++is) {
                            omega2_anharm[iT][ik][is] = omega2_anharm[iT + 1][ik][is];
                        }
                    }
                } else {
                    for (ik = 0; ik < nk; ++ik) {
                        for (is = 0; is < ns; ++is) {
                            omega2_anharm[iT][ik][is] = omega2_anharm[iT - 1][ik][is];
                        }
                    }
                }
            }

            compute_anharmonic_frequency(v4_array_all,
                                         omega2_anharm[iT],
                                         evec_anharm_tmp,
                                         temp,
                                         converged_prev,
                                         cmat_convert,
                                         selfenergy_offdiagonal,
                                         delta_v2_array_renormalize,
                                         writes->getVerbosity());

            calc_new_dymat_with_evec(dymat_anharm[iT],
                                     omega2_anharm[iT],
                                     evec_anharm_tmp);

            if (!warmstart_scph) converged_prev = false;
        }

        deallocate(cmat_convert);

    }

    mpi_bcast_complex(dymat_anharm, NT, kmesh_coarse->nk, ns);

    deallocate(omega2_anharm);
    deallocate(v4_array_all);
    deallocate(evec_anharm_tmp);
    deallocate(delta_v2_array_renormalize);
    if(relax_coordinate){
        deallocate(v3_array_all);
    }
}

// relax internal coordinate only
void Scph::exec_scph_relax_main(std::complex<double> ****dymat_anharm,
                                          std::complex<double> ****delta_harmonic_dymat_renormalize)
{
    using namespace Eigen;

    int ik, is, js;
    int ik1, is1, is2;
    int iat1, iat2, ixyz1, ixyz2;
    std::string str_tmp, str_tmp2;
    static auto complex_zero = std::complex<double>(0.0, 0.0);

    const auto nk = kmesh_dense->nk;
    const auto nk_interpolate = kmesh_coarse->nk;
    const auto ns = dynamical->neval;
    const auto nk_irred_interpolate = kmesh_coarse->nk_irred;
    const auto Tmin = system->Tmin;
    const auto Tmax = system->Tmax;
    const auto dT = system->dT;
    double ***omega2_anharm;
    std::complex<double> **delta_v2_array_renormalize, **delta_v2_array_strain_dummy;
    std::complex<double> ***evec_anharm_tmp;
    // IFC-renormalized harmonic phonon
    double ***omega2_harm_renormalize;
    std::complex<double> ***evec_harm_renormalize_tmp;
    // original and renormalized IFCs
    std::complex<double> *v1_array_original, *v1_array_renormalized;
    std::complex<double> ***v3_array_original, ***v3_array_renormalized;
    std::complex<double> ***v4_array_original, ***v4_array_renormalized;
    double v0_original, v0_renormalized;
    v0_original = 0.0; // set original ground state energy as zero

    // generalized force dF/dq^{(0)}_{\lambda}
    std::complex<double> *v1_array_SCP;

    // structure optimization
    int i_str_loop, i_temp_loop;
    double dq0;
    MatrixXcd Cmat(ns, ns), v2_mat_full(ns, ns);
    MatrixXcd v2_mat_optical(ns-3, ns-3);
    VectorXcd dq0_vec(ns-3), v1_vec_SCP(ns-3);
    std::vector<int> harm_optical_modes(ns-3);
    // structure optimization parameters
    // double dq0_threashold = coord_conv_tol;
    double add_hess_diag_omega2;

    // coordinate
    double *q0, *delta_q0, *u0;
    double *force_array;
    
    // check
    std::complex<double> dFdq_q;

    std::vector<double> vec_temp;

    const auto NT = static_cast<unsigned int>((Tmax - Tmin) / dT) + 1;

    // Compute matrix element of 4-phonon interaction

    allocate(v1_array_SCP, ns);

    allocate(omega2_anharm, NT, nk, ns);
    allocate(evec_anharm_tmp, nk, ns, ns);

    allocate(v1_array_original, ns);
    allocate(v1_array_renormalized, ns);

    // IFC-renormalization of harmonic dynamical matrix
    allocate(omega2_harm_renormalize, NT, nk, ns);
    allocate(evec_harm_renormalize_tmp, nk, ns, ns);   
    allocate(delta_v2_array_renormalize, nk_interpolate, ns*ns);

    // always zero when primitive cell is fixed.
    allocate(delta_v2_array_strain_dummy, nk_interpolate, ns*ns);
    for(ik = 0; ik < nk_interpolate; ik++){
        for(is = 0; is < ns*ns; is++){
            delta_v2_array_strain_dummy[ik][is] = 0.0;
        }
    }

    allocate(q0, ns);
    allocate(delta_q0, ns);
    allocate(u0, ns);
    allocate(force_array, ns);

    allocate(v4_array_original, nk_irred_interpolate * kmesh_dense->nk,
                     ns * ns, ns * ns);
    allocate(v4_array_renormalized, nk_irred_interpolate * kmesh_dense->nk,
                     ns * ns, ns * ns);

    // Calculate v4 array. 
    // This operation is the most expensive part of the calculation.
    if (selfenergy_offdiagonal & (ialgo == 1)) {
        compute_V4_elements_mpi_over_band(v4_array_original,
                                          evec_harmonic,
                                          selfenergy_offdiagonal);
    } else {
        compute_V4_elements_mpi_over_kpoint(v4_array_original,
                                            evec_harmonic,
                                            selfenergy_offdiagonal,
                                            relax_coordinate);
    }
    
    allocate(v3_array_original, nk, ns, ns * ns);
    allocate(v3_array_renormalized, nk, ns, ns * ns);
    compute_V3_elements_mpi_over_kpoint(v3_array_original,
                                        evec_harmonic,
                                        selfenergy_offdiagonal);

    if(mympi->my_rank == 0){
        std::cout << "Setting up the structural optimization at finite temperature ..." << std::endl << std::endl;
    }
    // assume that the atomic forces are zero at initial structure
    for(is = 0; is < ns; is++){
        v1_array_original[is] = 0.0;
    }

    // determine the value to add to the diagonal part of the Hessian
    double Ry_to_kayser_tmp = Hz_to_kayser / time_ry;
    add_hess_diag_omega2 = std::pow(add_hess_diag/Ry_to_kayser_tmp, 2);

    // get indices of optical modes at Gamma point
    js = 0;
    for(is = 0; is < ns; is++){
        if(std::fabs(omega2_harmonic[0][is]) < eps8){
            continue;
        }
        harm_optical_modes[js] = is;
        js++;
    }
    
    if(mympi->my_rank == 0){
        std::cout << " Mode indices of optical modes at Gamma point: " << std::endl << " ";
        for(is = 0; is < ns-3; is++){
            std::cout << std::setw(5) << harm_optical_modes[is];
        }std::cout << std::endl;
    }

    if (mympi->my_rank == 0) {

        std::complex<double> ***cmat_convert;
        allocate(cmat_convert, nk, ns, ns);

        vec_temp.clear();

        if (lower_temp) {
            for (int i = NT - 1; i >= 0; --i) {
                vec_temp.push_back(Tmin + static_cast<double>(i) * dT);
            }
        } else {
            for (int i = 0; i < NT; ++i) {
                vec_temp.push_back(Tmin + static_cast<double>(i) * dT);
            }
        }

        auto converged_prev = false;

        // this is for test
        std::ofstream fout_q0_tmp;
        fout_q0_tmp.open("q0_tmp.txt");
        std::ofstream fout_u0_tmp;
        fout_u0_tmp.open("u0_tmp.txt");
        std::ofstream fout_q0;
        fout_q0.open("q0.txt");
        std::ofstream fout_u0;
        fout_u0.open("u0.txt");
        std::ofstream fout_force;
        fout_force.open("SCP_force.txt");
        std::ofstream fout_dFdq_q;
        fout_dFdq_q.open("dFdq_q.txt");
        std::ofstream fout_dFdq_q_total;
        fout_dFdq_q_total.open("dFdq_q_total.txt");
        std::ofstream fout_v0;
        fout_v0.open("v0.txt");

        i_temp_loop = -1;

        std::cout << "Start structural optimization." << std::endl;
        std::cout << " Internal coordinates are relaxed." << std::endl;
        std::cout << " Shape of the unit cell is fixed." << std::endl << std::endl;

        for (double temp : vec_temp) {
            i_temp_loop++;
            auto iT = static_cast<unsigned int>((temp - Tmin) / dT);

            std::cout << " ----------------------------------------------------------------" << std::endl;
            std::cout << " Temperature = " << temp << " K" << std::endl;
            std::cout << " temperature index : " << std::setw(4) << i_temp_loop << "/" << std::setw(4) << NT << std::endl << std::endl;

            // Initialize phonon eigenvectors with harmonic values

            for (ik = 0; ik < nk; ++ik) {
                for (is = 0; is < ns; ++is) {
                    for (int js = 0; js < ns; ++js) {
                        evec_anharm_tmp[ik][is][js] = evec_harmonic[ik][is][js];
                    }
                }
            }
            if (converged_prev) {
                if (lower_temp) {
                    for (ik = 0; ik < nk; ++ik) {
                        for (is = 0; is < ns; ++is) {
                            omega2_anharm[iT][ik][is] = omega2_anharm[iT + 1][ik][is];
                        }
                    }
                } else {
                    for (ik = 0; ik < nk; ++ik) {
                        for (is = 0; is < ns; ++is) {
                            omega2_anharm[iT][ik][is] = omega2_anharm[iT - 1][ik][is];
                        }
                    }
                }
            }

            if(set_init_str == 1){
                // read displace.in
                read_initial_q0(q0);
                calculate_u0(q0, u0);
                converged_prev = false;
            }
            else if(set_init_str == 2){
                // read displace.in at initial temperature
                if(i_temp_loop == 0){
                    read_initial_q0(q0);
                    calculate_u0(q0, u0);
                }
            }
            else if(set_init_str == 4){ // specifically for LiReO3-type
                if(i_temp_loop == 0){
                    read_initial_q0(q0);
                    calculate_u0(q0, u0);
                }
                else if (std::fabs(u0[2]) < 0.05){
                    read_initial_q0(q0);
                    calculate_u0(q0, u0);
                }
            }

            else if(set_init_str == 3){
                // read T-dependent initial structure from u0.in
                read_Tdep_initial_q0_from_u(q0, i_temp_loop);
                calculate_u0(q0, u0);
                
            }

            // structure loop
            // std::cout << "temperature : " << temp << " K" << std::endl;
            fout_q0_tmp << "temperature : " << temp << " K" << std::endl;
            fout_u0_tmp << "temperature : " << temp << " K" << std::endl;
            fout_force << "temperature : " << temp << " K" << std::endl;

            std::cout << " Initial atomic displacements [Bohr] : " << std::endl;
            for(iat1 = 0; iat1 < system->natmin; iat1++){
                std::cout << " ";
                for(ixyz1 = 0; ixyz1 < 3; ixyz1++){
                    get_xyz_string(ixyz1, str_tmp);
                    std::cout << std::setw(10) << ("u_{" + std::to_string(iat1) + "," + str_tmp + "}");
                }std::cout << " :";
                for(ixyz1 = 0; ixyz1 < 3; ixyz1++){
                    std::cout << std::scientific << std::setw(16) << std::setprecision(6) << u0[iat1*3 + ixyz1];
                }std::cout << std::endl;
            }std::cout << std::endl;

            std::cout << " ----------------------------------------------------------------" << std::endl;

            std::cout << " Start structural optimization at " << temp << " K." << std::endl;

            for(i_str_loop = 0; i_str_loop < max_str_iter; i_str_loop++){

                std::cout << std::endl << std::endl << " Structure loop :" << std::setw(5) << i_str_loop << std::endl;
                std::cout << " IFC renormalization ... ";

                //renormalize IFC
                renormalize_v1_array(v1_array_renormalized, v1_array_original, delta_v2_array_strain_dummy, v3_array_original, v4_array_original, q0);
                renormalize_v2_array(delta_v2_array_renormalize, delta_v2_array_strain_dummy, v3_array_original, v4_array_original, q0);
                renormalize_v3_array(v3_array_renormalized, v3_array_original, v4_array_original, q0);
                renormalize_v0(v0_renormalized, v0_original, v1_array_original, delta_v2_array_strain_dummy, v3_array_original, v4_array_original, q0);

                std::cout << "done!" << std::endl << std::endl;
                std::cout << " SCPH calculation." << std::endl << std::endl;

                // calculate PES force
                calculate_force_in_real_space(v1_array_renormalized, force_array);

                // copy v4_array_original to v4_array_renormalized
                for(ik = 0; ik < nk_irred_interpolate * kmesh_dense->nk; ik++){
                    for(is1 = 0; is1 < ns*ns; is1++){
                        for(is2 = 0; is2 < ns*ns; is2++){
                            v4_array_renormalized[ik][is1][is2] = v4_array_original[ik][is1][is2];
                        }
                    }
                }

                // solve SCP equation
                compute_anharmonic_frequency(v4_array_renormalized,
                                         omega2_anharm[iT],
                                         evec_anharm_tmp,
                                         temp,
                                         converged_prev,
                                         cmat_convert,
                                         selfenergy_offdiagonal,
                                         delta_v2_array_renormalize, 
                                         writes->getVerbosity());

                calc_new_dymat_with_evec(dymat_anharm[iT],
                                        omega2_anharm[iT],
                                        evec_anharm_tmp);

                // calculate SCP force
                compute_anharmonic_v1_array(v1_array_renormalized, v3_array_renormalized, cmat_convert, omega2_anharm[iT], temp, v1_array_SCP);

                // check force
                dFdq_q = 0.0;
                for(is = 0; is < ns; is++){
                    dFdq_q += q0[is] * (v1_array_SCP[is] - v1_array_renormalized[is]);
                }
                fout_dFdq_q << temp << " " << i_str_loop << " " << dFdq_q.real() << " " << dFdq_q.imag() << std::endl;

                // check force
                dFdq_q = 0.0;
                for(is = 0; is < ns; is++){
                    dFdq_q += q0[is] * v1_array_SCP[is];
                    // dFdq_q += q0[is] * v1_array_renormalized[is];
                    
                }
                fout_dFdq_q_total << temp << " " << i_str_loop << " " << dFdq_q.real() << " " << dFdq_q.imag() << std::endl;

                // change structure(is = 0,1,2 are TA modes)
                for(is = 0; is < ns; is++){
                    delta_q0[is] = 0.0;
                }
                if(relax_algo == 1){ // steepest decent
                    dq0 = 0.0;
                    for(is = 0; is < ns; is++){
                        // skip acoustic mode
                        if(std::fabs(omega2_harmonic[0][is]) < eps8){
                            continue;
                        }
                        delta_q0[is] = - alpha_steepest_decent * v1_array_SCP[is].real();
                        q0[is] += delta_q0[is];
                    }
                   
                }
                else if(relax_algo == 2){ // iterative solution of linear equation

                    // prepare harmonic IFC matrix
                    for(is = 0; is < ns; is++){
                        for(js = 0; js < ns; js++){
                            Cmat(js, is) = cmat_convert[0][is][js]; // transpose
                            v2_mat_full(is, js) = 0.0;
                        }
                        v2_mat_full(is, is) = omega2_anharm[iT][0][is];
                    }
                    v2_mat_full = Cmat.adjoint() * v2_mat_full * Cmat;

                    for(is = 0; is < ns-3; is++){
                        for(js = 0; js < ns-3; js++){
                            //v2_mat_optical(is, js) = v2_mat_full(is+3, js+3);
                            v2_mat_optical(is, js) = v2_mat_full(harm_optical_modes[is], harm_optical_modes[js]);
                        }
                        // This is for the robustness of the calculation
                        v2_mat_optical(is, is) += add_hess_diag_omega2;
                    }
                    // solve linear equation
                    for(is = 0; is < ns-3; is++){
                        v1_vec_SCP(is) = v1_array_SCP[harm_optical_modes[is]];
                    }
                    
                    dq0_vec = v2_mat_optical.colPivHouseholderQr().solve(v1_vec_SCP);

                    // update q0
                    dq0 = 0.0;
                    for(is = 0; is < ns-3; is++){
                        delta_q0[harm_optical_modes[is]] = - mixbeta_coord * dq0_vec(is).real();
                        q0[harm_optical_modes[is]] += delta_q0[harm_optical_modes[is]];
                    }

                }
                // calculate SCP force
                calculate_force_in_real_space(v1_array_SCP, force_array);

                // print SCP force
                fout_force << temp << " " << i_str_loop << " ";
                for(is = 0; is < ns; is++){
                    fout_force << force_array[is] << " ";
                }fout_force << std::endl;

                // print q0
                fout_q0_tmp << i_str_loop << " ";
                for(is = 0; is < ns; is++){
                    fout_q0_tmp << q0[is] << " ";
                }fout_q0_tmp << std::endl;
                calculate_u0(q0, u0);
                fout_u0_tmp << i_str_loop << " ";
                for(is = 0; is < ns; is++){
                    fout_u0_tmp << u0[is] << " ";
                }fout_u0_tmp << std::endl;

                // check convergence
                dq0 = 0.0;
                for(is = 0; is < ns; is++){
                    dq0 += delta_q0[is] * delta_q0[is];
                }dq0 = std::sqrt(dq0);

                if(dq0 < coord_conv_tol){
                    std::cout << "structure optimization converged in " << i_str_loop << "-th loop." << std::endl;
                    std::cout << "break from the structure loop." << std::endl << std::endl;
                    break;
                }
                
            }// close structure loop

            std::cout << " ----------------------------------------------------------------" << std::endl;
            std::cout << " Final atomic displacements [Bohr] at " << temp << " K" << std::endl;
            for(iat1 = 0; iat1 < system->natmin; iat1++){
                std::cout << " ";
                for(ixyz1 = 0; ixyz1 < 3; ixyz1++){
                    get_xyz_string(ixyz1, str_tmp);
                    std::cout << std::setw(10) << ("u_{" + std::to_string(iat1) + "," + str_tmp + "}");
                }std::cout << " :";
                for(ixyz1 = 0; ixyz1 < 3; ixyz1++){
                    std::cout << std::scientific << std::setw(16) << std::setprecision(6) << u0[iat1*3 + ixyz1];
                }std::cout << std::endl;
            }
            if(i_temp_loop == NT-1){
                std::cout << " ----------------------------------------------------------------" << std::endl << std::endl;
            }
            else{
                std::cout << std::endl;
            }

            // print obtained structure
            calculate_u0(q0, u0);
            fout_q0 << temp << " ";
            for(is = 0; is < ns; is++){
                fout_q0 << q0[is] << " ";
            }fout_q0 << std::endl;

            fout_u0 << temp << " ";
            for(is = 0; is < ns; is++){
                fout_u0 << u0[is] << " ";
            }fout_u0 << std::endl;

            fout_v0 << temp << " " << v0_renormalized << std::endl;

            if (!warmstart_scph) converged_prev = false;

            // get renormalization of harmonic dymat 
            for(ik = 0; ik < nk_irred_interpolate * kmesh_dense->nk; ik++){
                    for(is1 = 0; is1 < ns*ns; is1++){
                        for(is2 = 0; is2 < ns*ns; is2++){
                            v4_array_renormalized[ik][is1][is2] = complex_zero;
                        }
                    }
                }

            for (ik = 0; ik < nk; ++ik) {
                for (is = 0; is < ns; ++is) {
                    for (int js = 0; js < ns; ++js) {
                        evec_harm_renormalize_tmp[ik][is][js] = evec_harmonic[ik][is][js];
                    }
                }
            }

            compute_renormalized_harmonic_frequency(omega2_harm_renormalize[iT],
                                        evec_harm_renormalize_tmp,
                                        delta_v2_array_renormalize,
                                        writes->getVerbosity());

            calc_new_dymat_with_evec(delta_harmonic_dymat_renormalize[iT],
                                    omega2_harm_renormalize[iT],
                                    evec_harm_renormalize_tmp);
        }

        // this is for test
        fout_dFdq_q.close();
        fout_dFdq_q_total.close();
        fout_q0_tmp.close();
        fout_u0_tmp.close();
        fout_q0.close();
        fout_u0.close();
        fout_force.close();
        fout_v0.close();

        deallocate(cmat_convert);

    }

    mpi_bcast_complex(dymat_anharm, NT, kmesh_coarse->nk, ns);
    mpi_bcast_complex(delta_harmonic_dymat_renormalize, NT, kmesh_coarse->nk, ns);

    deallocate(omega2_anharm);
    deallocate(omega2_harm_renormalize);
    deallocate(v1_array_original);
    deallocate(v1_array_renormalized);
    deallocate(delta_v2_array_renormalize);
    deallocate(delta_v2_array_strain_dummy);
    deallocate(v4_array_original);
    deallocate(v4_array_renormalized);
    deallocate(v3_array_original);
    deallocate(v3_array_renormalized);
    deallocate(evec_anharm_tmp);
    deallocate(evec_harm_renormalize_tmp);
    deallocate(v1_array_SCP);

    deallocate(q0);
    deallocate(delta_q0);
    deallocate(u0);
    deallocate(force_array);
}

// relax internal coordinate and lattice
void Scph::exec_scph_relax_cell_coordinate_main(std::complex<double> ****dymat_anharm,
                                          std::complex<double> ****delta_harmonic_dymat_renormalize)
{
    using namespace Eigen;

    int ik, is, js;
    int ik1, is1, is2, is3, is4;
    int i1, i2;
    int iat1, iat2, ixyz1, ixyz2;
    std::string str_tmp;
    static auto complex_zero = std::complex<double>(0.0, 0.0);

    const auto nk = kmesh_dense->nk;
    const auto nk_interpolate = kmesh_coarse->nk;
    const auto ns = dynamical->neval;
    const auto nk_irred_interpolate = kmesh_coarse->nk_irred;
    const auto Tmin = system->Tmin;
    const auto Tmax = system->Tmax;
    const auto dT = system->dT;
    double ***omega2_anharm;
    std::complex<double> **delta_v2_array_renormalize;
    std::complex<double> **delta_v2_array_with_strain;
    std::complex<double> ***evec_anharm_tmp;
    // IFC-renormalized harmonic phonon
    double ***omega2_harm_renormalize;
    std::complex<double> ***evec_harm_renormalize_tmp;
    // original and renormalized IFCs
    std::complex<double> *v1_array_original, *v1_array_renormalized, *v1_array_with_strain;
    std::complex<double> ***v3_array_original, ***v3_array_renormalized, ***v3_array_with_strain;
    std::complex<double> ***v4_array_original, ***v4_array_renormalized, ***v4_array_with_strain;
    double v0_original, v0_renormalized, v0_with_strain;
    v0_original = 0.0; // set original ground state energy as zero

    // elastic constants
    double *C1_array;
    double **C2_array;
    double ***C3_array;

    // generalized force dF/dq^{(0)}_{\lambda}
    std::complex<double> *v1_array_SCP;

    std::complex<double> **del_v1_strain_from_harmonic;
    std::complex<double> **del_v1_strain_from_cubic;
    std::complex<double> **del_v1_strain_from_quartic;

    std::complex<double> ***del_v2_strain_from_cubic;
    std::complex<double> ***del_v2_strain_from_quartic;
    std::complex<double> ****del_v3_strain_from_quartic;

    std::complex<double> *del_v0_strain_with_strain_displace;
    std::complex<double> *del_v0_strain_SCP;

    // structure optimization
    int i_str_loop, i_temp_loop;
    double dq0;
    MatrixXcd Cmat(ns, ns), v2_mat_full(ns, ns);
    MatrixXcd v2_mat_optical(ns-3, ns-3);
    VectorXcd dq0_vec(ns-3), v1_vec_SCP(ns-3);
    std::vector<int> harm_optical_modes(ns-3);

    double du_tensor;
    double *delta_u_tensor;
    MatrixXcd C2_mat_tmp(6,6);
    VectorXcd du_tensor_vec(6), del_v0_strain_vec(6);

    int itmp1, itmp2, itmp3, itmp4, itmp5, itmp6;

    // coordinate
    double *q0, *delta_q0, *u0;
    double *force_array;

    // strain
    double **u_tensor, **eta_tensor;

    // coordinate optimization
    double dq0_threashold = coord_conv_tol;
    double add_hess_diag_omega2;

    double Ry_to_kayser_tmp = Hz_to_kayser / time_ry;
    add_hess_diag_omega2 = std::pow(add_hess_diag/Ry_to_kayser_tmp, 2);

    // cell optimization
    double pvcell = 0.0; // pressure * v_{cell,reference} [Ry]

    pvcell = stat_pressure * system->volume_p * std::pow(Bohr_in_Angstrom, 3) * 1.0e-30; // in 10^9 J = GJ
    pvcell *= 1.0e9/Ryd; // in Ry

    // check
    std::complex<double> dFdq_q;

    std::vector<double> vec_temp;

    const auto NT = static_cast<unsigned int>((Tmax - Tmin) / dT) + 1;

    // Compute matrix element of 4-phonon interaction

    allocate(v1_array_SCP, ns);

    allocate(omega2_anharm, NT, nk, ns);
    allocate(evec_anharm_tmp, nk, ns, ns);

    allocate(v1_array_original, ns);
    allocate(v1_array_with_strain, ns);
    allocate(v1_array_renormalized, ns);

    // IFC-renormalization of harmonic dynamical matrix
    allocate(omega2_harm_renormalize, NT, nk, ns);
    allocate(evec_harm_renormalize_tmp, nk, ns, ns);   
    allocate(delta_v2_array_renormalize, nk_interpolate, ns*ns);
    allocate(delta_v2_array_with_strain, nk_interpolate, ns*ns);

    allocate(q0, ns);
    allocate(delta_q0, ns);
    allocate(u0, ns);
    allocate(force_array, ns);

    allocate(u_tensor, 3, 3);
    allocate(eta_tensor, 3, 3);

    allocate(delta_u_tensor, 6);

    allocate(v4_array_original, nk_irred_interpolate * kmesh_dense->nk,
                     ns * ns, ns * ns);
    allocate(v4_array_renormalized, nk_irred_interpolate * kmesh_dense->nk,
                     ns * ns, ns * ns);

    allocate(v4_array_with_strain, nk_irred_interpolate * kmesh_dense->nk,
                     ns * ns, ns * ns);

    // Calculate v4 array. 
    // This operation is the most expensive part of the calculation.
    if (selfenergy_offdiagonal & (ialgo == 1)) {
        compute_V4_elements_mpi_over_band(v4_array_original,
                                          evec_harmonic,
                                          selfenergy_offdiagonal);
    } else {
        compute_V4_elements_mpi_over_kpoint(v4_array_original,
                                            evec_harmonic,
                                            selfenergy_offdiagonal,
                                            relax_coordinate);
    }

    allocate(v3_array_original, nk, ns, ns * ns);
    allocate(v3_array_renormalized, nk, ns, ns * ns);
    allocate(v3_array_with_strain, nk, ns, ns * ns);

    compute_V3_elements_mpi_over_kpoint(v3_array_original,
                                        evec_harmonic,
                                        selfenergy_offdiagonal);

    // assume that the atomic forces are zero at initial structure
    for(is = 0; is < ns; is++){
        v1_array_original[is] = 0.0;
    }

    // compute IFC renormalization by lattice relaxation
    std::cout << " RELAX_COORDINATE = " << relax_coordinate << ": ";
    std::cout << "Calculating derivatives of k-space IFCs by strain." << std::endl;

    std::cout << "  1st order derivatives of 1st-order IFCs (from harmonic IFCs) ... ";
    allocate(del_v1_strain_from_harmonic, 9, ns);
    compute_del_v1_strain_from_harmonic(del_v1_strain_from_harmonic, evec_harmonic);
    std::cout << "done!" << std::endl;
    timer->print_elapsed();

    std::cout << "  2nd order derivatives of 1st-order IFCs (from cubic IFCs) ... ";
    allocate(del_v1_strain_from_cubic, 81, ns);
    compute_del_v1_strain_from_cubic(del_v1_strain_from_cubic, evec_harmonic);
    std::cout << "done!" << std::endl;
    timer->print_elapsed();

    std::cout << "  3rd order derivatives of 1st-order IFCs (from quartic IFCs) ... ";
    allocate(del_v1_strain_from_quartic, 729, ns);
    compute_del_v1_strain_from_quartic(del_v1_strain_from_quartic, evec_harmonic);
    std::cout << "done!" << std::endl;
    timer->print_elapsed();

    //std::cout << "  from cubic to harmonic IFCs ... ";
    std::cout << "  1st order derivatives of harmonic IFCs (finite displacement method) ... " << std::endl;
    allocate(del_v2_strain_from_cubic, 9, nk, ns*ns);
    // compute_del_v2_strain_from_cubic(del_v2_strain_from_cubic, evec_harmonic);
    // calculate del_v2_strain_from_cubic by finite difference method in terms of strain
    calculate_del_v2_strain_from_cubic_by_finite_difference(evec_harmonic,
                                                            del_v2_strain_from_cubic);
    std::cout << "done!" << std::endl;
    timer->print_elapsed();

    std::cout << "  2cd order derivatives of harmonic IFCs (from quartic IFCs) ... ";
    allocate(del_v2_strain_from_quartic, 81, nk, ns*ns);
    compute_del_v2_strain_from_quartic(del_v2_strain_from_quartic, evec_harmonic);
    std::cout << "done!" << std::endl;
    timer->print_elapsed();

    std::cout << "  1st order derivatives of cubic IFCs (from quartic IFCs) ... ";
    allocate(del_v3_strain_from_quartic, 9, nk, ns, ns*ns);
    compute_del_v3_strain_from_quartic(del_v3_strain_from_quartic, evec_harmonic);
    std::cout << "done!" << std::endl; 
    timer->print_elapsed();

    // add correction from B_array to del_v2_strain_from_cubic
    // add_strain_mode_coupling_to_del_v2_strain(del_v2_strain_from_cubic, evec_harmonic, B_array);

    // for lattice relaxation
    allocate(del_v0_strain_with_strain_displace, 9);
    allocate(del_v0_strain_SCP, 9);

    // for(int itmp1 = 0; itmp1 < 81; itmp1++){
    //     for(int itmp2 = 0; itmp2 < nk; itmp2++){
    //         for(int itmp3 = 0; itmp3 < ns*ns; itmp3++){
    //             del_v2_strain_from_quartic[itmp1][itmp2][itmp3] = 0.0;
    //         }
    //     }
    // }

    // int ixyz1, ixyz2;
    // std::cout << "compute_del_v2_strain_from_cubic is done. " << std::endl;
    // std::cout << "ixyz1 = " << ixyz1 << " ixyz2 = " << ixyz2 << " is done" << std::endl;
    // for(ixyz1 = 0; ixyz1 < 3; ixyz1++){
    //     for(ixyz2 = 0; ixyz2 < 3; ixyz2++){
    //         for(ik = 0; ik < 5; ik++){
    //             for(is1 = 0; is1 < ns; is1++){
    //                 for(is2 = 0; is2 < ns; is2++){
    //                     std::cout << del_v2_strain_from_cubic[ixyz1*3 + ixyz2][ik][is1*ns+is2] << " ";
    //                 }std::cout << std::endl;
    //             }std::cout << std::endl;
    //         }
    //     }
    // }

    allocate(C1_array, 9);
    allocate(C2_array, 9, 9);
    allocate(C3_array, 9, 9, 9);


    // get indices of optical modes at Gamma point
    js = 0;
    for(is = 0; is < ns; is++){
        if(std::fabs(omega2_harmonic[0][is]) < eps8){
            continue;
        }
        harm_optical_modes[js] = is;
        js++;
    }
    
    if(mympi->my_rank == 0){
        std::cout << "mode indices of optical modes: " << std::endl;
        for(is = 0; is < ns-3; is++){
            std::cout << harm_optical_modes[is] << " ";
        }std::cout << std::endl;
    }

    if (mympi->my_rank == 0) {

        std::complex<double> ***cmat_convert;
        allocate(cmat_convert, nk, ns, ns);

        vec_temp.clear();

        if (lower_temp) {
            for (int i = NT - 1; i >= 0; --i) {
                vec_temp.push_back(Tmin + static_cast<double>(i) * dT);
            }
        } else {
            for (int i = 0; i < NT; ++i) {
                vec_temp.push_back(Tmin + static_cast<double>(i) * dT);
            }
        }

        auto converged_prev = false;

        // read elastic constants
        read_C1_array(C1_array);
        read_elastic_constants(C2_array, C3_array);

        // this is for test
        std::ofstream fout_q0_tmp;
        fout_q0_tmp.open("q0_tmp.txt");
        std::ofstream fout_u0_tmp;
        fout_u0_tmp.open("u0_tmp.txt");
        std::ofstream fout_q0;
        fout_q0.open("q0.txt");
        std::ofstream fout_u0;
        fout_u0.open("u0.txt");
        std::ofstream fout_force;
        fout_force.open("SCP_force.txt");
        std::ofstream fout_dFdq_q;
        fout_dFdq_q.open("dFdq_q.txt");
        std::ofstream fout_dFdq_q_total;
        fout_dFdq_q_total.open("dFdq_q_total.txt");
        std::ofstream fout_v0;
        fout_v0.open("v0.txt");

        // cell optimization
        std::ofstream fout_u_tensor_tmp;
        fout_u_tensor_tmp.open("u_tensor_tmp.txt");
        std::ofstream fout_u_tensor;
        fout_u_tensor.open("u_tensor.txt");

        i_temp_loop = -1;

        std::cout << "Start structural optimization." << std::endl;
        std::cout << " Internal coordinates and shape of the unit cell are relaxed." << std::endl;

        for (double temp : vec_temp) {
            i_temp_loop++;
            auto iT = static_cast<unsigned int>((temp - Tmin) / dT);

            std::cout << " ----------------------------------------------------------------" << std::endl;
            std::cout << " Temperature = " << temp << " K" << std::endl;
            std::cout << " temperature index : " << std::setw(4) << i_temp_loop << "/" << std::setw(4) << NT << std::endl << std::endl;


            // Initialize phonon eigenvectors with harmonic values

            for (ik = 0; ik < nk; ++ik) {
                for (is = 0; is < ns; ++is) {
                    for (int js = 0; js < ns; ++js) {
                        evec_anharm_tmp[ik][is][js] = evec_harmonic[ik][is][js];
                    }
                }
            }
            if (converged_prev) {
                if (lower_temp) {
                    for (ik = 0; ik < nk; ++ik) {
                        for (is = 0; is < ns; ++is) {
                            omega2_anharm[iT][ik][is] = omega2_anharm[iT + 1][ik][is];
                        }
                    }
                } else {
                    for (ik = 0; ik < nk; ++ik) {
                        for (is = 0; is < ns; ++is) {
                            omega2_anharm[iT][ik][is] = omega2_anharm[iT - 1][ik][is];
                        }
                    }
                }
            }

            if(set_init_str == 1){
                // read displace.in
                read_initial_q0(q0);
                calculate_u0(q0, u0);
                // read strain.in
                read_initial_strain(u_tensor);
                converged_prev = false;
            }
            else if(set_init_str == 2){
                // read displace.in at initial temperature
                if(i_temp_loop == 0){
                    read_initial_q0(q0);
                    calculate_u0(q0, u0);
                    read_initial_strain(u_tensor);
                }
                if(std::fabs(u0[5]) < 0.01){
                    read_initial_q0(q0);
                    calculate_u0(q0, u0);
                    converged_prev = false;
                }
            }
            else if(set_init_str == 3){
                // read T-dependent initial structure from u0.in
                read_Tdep_initial_q0_from_u(q0, i_temp_loop);
                calculate_u0(q0, u0);
                std::cout << "initial u0: " << std::endl;
                for(is = 0; is < ns; is++){
                    std::cout << u0[is] << " ";
                }std::cout << std::endl;
            }

            // std::cout << "temperature : " << temp << " K" << std::endl;
            fout_q0_tmp << "temperature : " << temp << " K" << std::endl;
            fout_u0_tmp << "temperature : " << temp << " K" << std::endl;
            fout_force << "temperature : " << temp << " K" << std::endl;

            fout_u_tensor_tmp << "temperature : " << temp << " K" << std::endl;

            std::cout << " Initial atomic displacements [Bohr] : " << std::endl;
            for(iat1 = 0; iat1 < system->natmin; iat1++){
                std::cout << " ";
                for(ixyz1 = 0; ixyz1 < 3; ixyz1++){
                    get_xyz_string(ixyz1, str_tmp);
                    std::cout << std::setw(10) << ("u_{" + std::to_string(iat1) + "," + str_tmp + "}");
                }std::cout << " :";
                for(ixyz1 = 0; ixyz1 < 3; ixyz1++){
                    std::cout << std::scientific << std::setw(16) << std::setprecision(6) << u0[iat1*3 + ixyz1];
                }std::cout << std::endl;
            }std::cout << std::endl;

            std::cout << " Initial strain (displacement gradient tensor u_{mu nu}) : " << std::endl;
            for(ixyz1 = 0; ixyz1 < 3; ixyz1++){
                std::cout << " ";
                for(ixyz2 = 0; ixyz2 < 3; ixyz2++){
                    std::cout << std::scientific << std::setw(16) << std::setprecision(6) << u_tensor[ixyz1][ixyz2];    
                }std::cout << std::endl;
            }std::cout << std::endl;

            std::cout << " ----------------------------------------------------------------" << std::endl;

            std::cout << " Start structural optimization at " << temp << " K." << std::endl;

            for(i_str_loop = 0; i_str_loop < max_str_iter; i_str_loop++){

                std::cout << std::endl << std::endl << " Structure loop :" << std::setw(5) << i_str_loop << std::endl;
                std::cout << " IFC renormalization ... ";

                // get eta tensor
                calculate_eta_tensor(eta_tensor, u_tensor);

                // calculate IFCs under strain
                renormalize_v0_from_strain(v0_with_strain, v0_original, eta_tensor, C1_array, C2_array, C3_array, u_tensor, pvcell);
                

                renormalize_v1_array_from_strain(v1_array_with_strain, 
                                                 v1_array_original,
                                                 del_v1_strain_from_harmonic, 
                                                 del_v1_strain_from_cubic, 
                                                 del_v1_strain_from_quartic, 
                                                 u_tensor);

                renormalize_v2_array_from_strain(delta_v2_array_with_strain, del_v2_strain_from_cubic, del_v2_strain_from_quartic, u_tensor);
                renormalize_v3_array_from_strain(v3_array_with_strain, v3_array_original, del_v3_strain_from_quartic, u_tensor);
                
                for(ik = 0; ik < nk_irred_interpolate * nk; ik++){
                    for(is = 0; is < ns*ns; is++){
                        for(is1 = 0; is1 < ns*ns; is1++){
                            v4_array_with_strain[ik][is][is1] = v4_array_original[ik][is][is1];
                        }
                    }
                }

                //renormalize IFC
                renormalize_v1_array(v1_array_renormalized, v1_array_with_strain, delta_v2_array_with_strain, v3_array_with_strain, v4_array_with_strain, q0);
                renormalize_v2_array(delta_v2_array_renormalize, delta_v2_array_with_strain, v3_array_with_strain, v4_array_with_strain, q0);
                renormalize_v3_array(v3_array_renormalized, v3_array_with_strain, v4_array_with_strain, q0);
                renormalize_v0(v0_renormalized, v0_with_strain, v1_array_with_strain, delta_v2_array_with_strain, v3_array_with_strain, v4_array_with_strain, q0);

                // calculate PES force
                calculate_force_in_real_space(v1_array_renormalized, force_array);

                // calculate PES gradient by strain
                calculate_del_v0_strain_with_strain_displace(del_v0_strain_with_strain_displace, 
                                                             C1_array,
                                                             C2_array,
                                                             C3_array,
                                                             eta_tensor,
                                                             u_tensor,
                                                             del_v1_strain_from_harmonic,
                                                             del_v1_strain_from_cubic,
                                                             del_v1_strain_from_quartic,
                                                             del_v2_strain_from_cubic,
                                                             del_v2_strain_from_quartic,
                                                             del_v3_strain_from_quartic,
                                                             q0,
                                                             pvcell);




                // copy v4_array_original to v4_array_renormalized
                for(ik = 0; ik < nk_irred_interpolate * kmesh_dense->nk; ik++){
                    for(is1 = 0; is1 < ns*ns; is1++){
                        for(is2 = 0; is2 < ns*ns; is2++){
                            v4_array_renormalized[ik][is1][is2] = v4_array_original[ik][is1][is2];
                        }
                    }
                }

                std::cout << "done!" << std::endl << std::endl;
                std::cout << " SCPH calculation." << std::endl << std::endl;

                // solve SCP equation
                compute_anharmonic_frequency(v4_array_renormalized,
                                         omega2_anharm[iT],
                                         evec_anharm_tmp,
                                         temp,
                                         converged_prev,
                                         cmat_convert,
                                         selfenergy_offdiagonal,
                                         delta_v2_array_renormalize, 
                                         writes->getVerbosity());

                calc_new_dymat_with_evec(dymat_anharm[iT],
                                        omega2_anharm[iT],
                                        evec_anharm_tmp);

                // calculate SCP force
                compute_anharmonic_v1_array(v1_array_renormalized, v3_array_renormalized, cmat_convert, omega2_anharm[iT], temp, v1_array_SCP);

                // calculate gradient of SCP free energy with respect to the strain
                compute_anharmonic_del_v0_strain(del_v0_strain_SCP, 
                                            del_v0_strain_with_strain_displace,
                                            del_v2_strain_from_cubic,
                                            del_v2_strain_from_quartic,
                                            del_v3_strain_from_quartic,
                                            u_tensor,
                                            q0,
                                            cmat_convert, 
                                            omega2_anharm[iT], 
                                            temp);

                // check force
                dFdq_q = 0.0;
                for(is = 0; is < ns; is++){
                    dFdq_q += q0[is] * (v1_array_SCP[is] - v1_array_renormalized[is]);
                }
                fout_dFdq_q << temp << " " << i_str_loop << " " << dFdq_q.real() << " " << dFdq_q.imag() << std::endl;

                // check force
                dFdq_q = 0.0;
                for(is = 0; is < ns; is++){
                    dFdq_q += q0[is] * v1_array_SCP[is];
                    // dFdq_q += q0[is] * v1_array_renormalized[is];
                    
                }
                fout_dFdq_q_total << temp << " " << i_str_loop << " " << dFdq_q.real() << " " << dFdq_q.imag() << std::endl;

                // change structure(is = 0,1,2 are TA modes)
                for(is = 0; is < ns; is++){
                    delta_q0[is] = 0.0;
                }
                for(is = 0; is < 6; is++){
                    delta_u_tensor[is] = 0.0;
                }
                if(relax_algo == 1){ // steepest decent
                    dq0 = 0.0;
                    for(is = 0; is < ns; is++){
                        // skip acoustic mode
                        if(std::fabs(omega2_harmonic[0][is]) < eps8){
                            continue;
                        }
                        delta_q0[is] = - alpha_steepest_decent * v1_array_SCP[is].real();
                        q0[is] += delta_q0[is];
                    }
                   
                }
                else if(relax_algo == 2){ // iterative solution of linear equation

                    // prepare harmonic IFC matrix
                    for(is = 0; is < ns; is++){
                        for(js = 0; js < ns; js++){
                            Cmat(js, is) = cmat_convert[0][is][js]; // transpose
                            v2_mat_full(is, js) = 0.0;
                        }
                        v2_mat_full(is, is) = omega2_anharm[iT][0][is];
                    }
                    v2_mat_full = Cmat.adjoint() * v2_mat_full * Cmat;

                    for(is = 0; is < ns-3; is++){
                        for(js = 0; js < ns-3; js++){
                            //v2_mat_optical(is, js) = v2_mat_full(is+3, js+3);
                            v2_mat_optical(is, js) = v2_mat_full(harm_optical_modes[is], harm_optical_modes[js]);
                        }
                        v2_mat_optical(is, is) += add_hess_diag_omega2;
                    }
                    // solve linear equation
                    for(is = 0; is < ns-3; is++){
                        v1_vec_SCP(is) = v1_array_SCP[harm_optical_modes[is]];
                    }
                    
                    dq0_vec = v2_mat_optical.colPivHouseholderQr().solve(v1_vec_SCP);

                    // update q0
                    dq0 = 0.0;
                    for(is = 0; is < ns-3; is++){
                        delta_q0[harm_optical_modes[is]] = - mixbeta_coord * dq0_vec(is).real();
                        // delta_q0[harm_optical_modes[is]] = 0.0;
                        q0[harm_optical_modes[is]] += delta_q0[harm_optical_modes[is]];
                    }

                    // prepare matrix of elastic constants and vector of del_v0_strain_SCP
                    for(itmp1 = 0; itmp1 < 3; itmp1++){
                            del_v0_strain_vec(itmp1) = del_v0_strain_SCP[itmp1*3+itmp1];

                            itmp2 = (itmp1+1)%3;
                            itmp3 = (itmp1+2)%3;
                            del_v0_strain_vec(itmp1+3) = del_v0_strain_SCP[itmp2*3+itmp3];
                    }

                    for(itmp1 = 0; itmp1 < 3; itmp1++){
                        for(itmp2 = 0; itmp2 < 3; itmp2++){
                            C2_mat_tmp(itmp1, itmp2) = C2_array[itmp1*3+itmp1][itmp2*3+itmp2];
                        }
                    }
                    for(itmp1 = 0; itmp1 < 3; itmp1++){
                        for(itmp2 = 0; itmp2 < 3; itmp2++){
                            itmp3 = (itmp2+1)%3;
                            itmp4 = (itmp2+2)%3;
                            C2_mat_tmp(itmp1, itmp2+3) = 2.0 * C2_array[itmp1*3+itmp1][itmp3*3+itmp4];
                            C2_mat_tmp(itmp2+3, itmp1) = C2_array[itmp3*3+itmp4][itmp1*3+itmp1];
                        }
                    }
                    for(itmp1 = 0; itmp1 < 3; itmp1++){
                        for(itmp2 = 0; itmp2 < 3; itmp2++){
                            itmp3 = (itmp1+1)%3;
                            itmp4 = (itmp1+2)%3;
                            itmp5 = (itmp2+1)%3;
                            itmp6 = (itmp2+2)%3;
                            C2_mat_tmp(itmp1+3, itmp2+3) = 2.0 * C2_array[itmp3*3+itmp4][itmp5*3+itmp6];
                        }
                    }

                    // solve linear equation
                    du_tensor_vec = C2_mat_tmp.colPivHouseholderQr().solve(del_v0_strain_vec);

                    // update u tensor
                    for(is = 0; is < 6; is++){
                        delta_u_tensor[is] = - mixbeta_cell * du_tensor_vec(is).real();
                        // delta_u_tensor[is] = 0.0;
                        if(is < 3){
                            u_tensor[is][is] += delta_u_tensor[is];
                        }
                        else{
                            itmp1 = (is+1)%3;
                            itmp2 = (is+2)%3;
                            u_tensor[itmp1][itmp2] += delta_u_tensor[is];
                            u_tensor[itmp2][itmp1] += delta_u_tensor[is];
                        }
                    }
                }
                // calculate SCP force
                calculate_force_in_real_space(v1_array_SCP, force_array);

                // print SCP force
                fout_force << temp << " " << i_str_loop << " ";
                for(is = 0; is < ns; is++){
                    fout_force << force_array[is] << " ";
                }fout_force << std::endl;

                // print q0
                fout_q0_tmp << i_str_loop << " ";
                for(is = 0; is < ns; is++){
                    fout_q0_tmp << q0[is] << " ";
                }fout_q0_tmp << std::endl;
                calculate_u0(q0, u0);
                fout_u0_tmp << i_str_loop << " ";
                for(is = 0; is < ns; is++){
                    fout_u0_tmp << u0[is] << " ";
                }fout_u0_tmp << std::endl;

                fout_u_tensor_tmp << i_str_loop << " ";
                for(is = 0; is < 9; is++){
                    fout_u_tensor_tmp << u_tensor[is/3][is%3] << " ";
                }fout_u_tensor_tmp << std::endl;


                // check convergence
                dq0 = 0.0;
                for(is = 0; is < ns; is++){
                    dq0 += delta_q0[is] * delta_q0[is];
                }dq0 = std::sqrt(dq0);

                du_tensor = 0.0;
                for(is = 0; is < 6; is++){
                    du_tensor += delta_u_tensor[is] * delta_u_tensor[is];
                    if(is >= 3){
                        du_tensor += delta_u_tensor[is] * delta_u_tensor[is];
                    }
                }du_tensor = std::sqrt(du_tensor);

                if(dq0 < dq0_threashold && du_tensor < cell_conv_tol){
                    std::cout << "structure optimization converged in " << i_str_loop << "-th loop." << std::endl;
                    std::cout << "break from the structure loop." << std::endl;
                    break;
                }
                
            }// close structure loop

            std::cout << " ----------------------------------------------------------------" << std::endl;
            std::cout << " Final atomic displacements [Bohr] at " << temp << " K" << std::endl;
            for(iat1 = 0; iat1 < system->natmin; iat1++){
                std::cout << " ";
                for(ixyz1 = 0; ixyz1 < 3; ixyz1++){
                    get_xyz_string(ixyz1, str_tmp);
                    std::cout << std::setw(10) << ("u_{" + std::to_string(iat1) + "," + str_tmp + "}");
                }std::cout << " :";
                for(ixyz1 = 0; ixyz1 < 3; ixyz1++){
                    std::cout << std::scientific << std::setw(16) << std::setprecision(6) << u0[iat1*3 + ixyz1];
                }std::cout << std::endl;
            }std::cout << std::endl;
            
            std::cout << " Final strain (displacement gradient tensor u_{mu nu}) : " << std::endl;
            for(ixyz1 = 0; ixyz1 < 3; ixyz1++){
                std::cout << " ";
                for(ixyz2 = 0; ixyz2 < 3; ixyz2++){
                    std::cout << std::scientific << std::setw(16) << std::setprecision(6) << u_tensor[ixyz1][ixyz2];    
                }std::cout << std::endl;
            }if(i_temp_loop == NT-1){
                std::cout << " ----------------------------------------------------------------" << std::endl << std::endl;
            }
            else{
                std::cout << std::endl;
            }

            // print obtained structure
            calculate_u0(q0, u0);
            fout_q0 << temp << " ";
            for(is = 0; is < ns; is++){
                fout_q0 << q0[is] << " ";
            }fout_q0 << std::endl;

            fout_u0 << temp << " ";
            for(is = 0; is < ns; is++){
                fout_u0 << u0[is] << " ";
            }fout_u0 << std::endl;

            fout_u_tensor << temp << " ";
            for(is = 0; is < 9; is++){
                fout_u_tensor << u_tensor[is/3][is%3] << " ";
            }fout_u_tensor << std::endl;

            fout_v0 << temp << " " << v0_renormalized << std::endl;

            if (!warmstart_scph) converged_prev = false;

            // get renormalization of harmonic dymat 
            for(ik = 0; ik < nk_irred_interpolate * kmesh_dense->nk; ik++){
                    for(is1 = 0; is1 < ns*ns; is1++){
                        for(is2 = 0; is2 < ns*ns; is2++){
                            v4_array_renormalized[ik][is1][is2] = complex_zero;
                        }
                    }
                }

            for (ik = 0; ik < nk; ++ik) {
                for (is = 0; is < ns; ++is) {
                    for (int js = 0; js < ns; ++js) {
                        evec_harm_renormalize_tmp[ik][is][js] = evec_harmonic[ik][is][js];
                    }
                }
            }

            compute_renormalized_harmonic_frequency(omega2_harm_renormalize[iT],
                                        evec_harm_renormalize_tmp,
                                        delta_v2_array_renormalize,
                                        writes->getVerbosity());

            calc_new_dymat_with_evec(delta_harmonic_dymat_renormalize[iT],
                                    omega2_harm_renormalize[iT],
                                    evec_harm_renormalize_tmp);
        }

        // this is for test
        fout_dFdq_q.close();
        fout_dFdq_q_total.close();
        fout_q0_tmp.close();
        fout_u0_tmp.close();
        fout_q0.close();
        fout_u0.close();
        fout_force.close();
        fout_v0.close();

        deallocate(cmat_convert);

    }

    mpi_bcast_complex(dymat_anharm, NT, kmesh_coarse->nk, ns);
    mpi_bcast_complex(delta_harmonic_dymat_renormalize, NT, kmesh_coarse->nk, ns);

    deallocate(omega2_anharm);
    deallocate(omega2_harm_renormalize);
    deallocate(v1_array_original);
    deallocate(v1_array_with_strain);
    deallocate(v1_array_renormalized);
    deallocate(delta_v2_array_renormalize);
    deallocate(delta_v2_array_with_strain);

    deallocate(v4_array_original);
    deallocate(v4_array_renormalized);
    deallocate(v4_array_with_strain);
    deallocate(v3_array_original);
    deallocate(v3_array_renormalized);
    deallocate(v3_array_with_strain);
    deallocate(evec_anharm_tmp);
    deallocate(evec_harm_renormalize_tmp);
    deallocate(v1_array_SCP);

    deallocate(del_v1_strain_from_harmonic);
    deallocate(del_v1_strain_from_cubic);
    deallocate(del_v1_strain_from_quartic);
    deallocate(del_v2_strain_from_cubic);
    deallocate(del_v2_strain_from_quartic);
    deallocate(del_v3_strain_from_quartic);

    deallocate(del_v0_strain_with_strain_displace);
    deallocate(del_v0_strain_SCP);

    deallocate(C1_array);
    deallocate(C2_array);
    deallocate(C3_array);

    deallocate(q0);
    deallocate(delta_q0);
    deallocate(u0);
    deallocate(force_array);

    deallocate(u_tensor);
    deallocate(eta_tensor);

    deallocate(delta_u_tensor);
}

void Scph::read_C1_array(double *C1_array)
{
    std::fstream fin_C1_array;
    std::string str_tmp;
    int natmin = system->natmin;
    int i1, i2, i3;

    // initialize elastic constants
    for(i1 = 0; i1 < 9; i1++){
        C1_array[i1] = 0.0;
    }

    fin_C1_array.open("C1_array.in");

    if(!fin_C1_array){
        std::cout << "Warning in Scph::read_C1_array: file C1_array.in could not open." << std::endl;
        std::cout << "all elastic constants are set 0." << std::endl;
        return ;
    }

    fin_C1_array >> str_tmp;
    for(i1 = 0; i1 < 9; i1++){
        fin_C1_array >> C1_array[i1];
    }

    std::cout << "C1_array: " << std::endl;
    for(i1 = 0; i1 < 9; i1++){
        std::cout << C1_array[i1] << " ";
    }std::cout << std::endl;

    return ;
}

void Scph::read_elastic_constants(double **C2_array, 
                                  double ***C3_array)
{
    std::fstream fin_elastic_constants;
    std::string str_tmp;
    int natmin = system->natmin;
    int i1, i2, i3;

    // initialize elastic constants
    // for(i1 = 0; i1 < 9; i1++){
    //     for(i2 = 0; i2 < 9; i2++){
    //         C2_array[i1][i2] = 0.0;
    //         for(i3 = 0; i3 < 9; i3++){
    //             C3_array[i1][i2][i3] = 0.0;
    //         }
    //     }
    // }

    fin_elastic_constants.open("elastic_constants.in");

    if(!fin_elastic_constants){
        std::cout << "Warning in Scph::read_elastic_constants: file elastic_constants.in could not open." << std::endl;
        std::cout << "all elastic constants are set 0." << std::endl;
        return ;
    }

    fin_elastic_constants >> str_tmp;
    for(i1 = 0; i1 < 9; i1++){
        for(i2 = 0; i2 < 9; i2++){
            fin_elastic_constants >> C2_array[i1][i2];
        }
    }
    fin_elastic_constants >> str_tmp;
    for(i1 = 0; i1 < 9; i1++){
        for(i2 = 0; i2 < 9; i2++){
            for(i3 = 0; i3 < 9; i3++){
                fin_elastic_constants >> C3_array[i1][i2][i3];
            }
        }
    }
}

void Scph::read_initial_q0(double *q0)
{
    std::fstream fin_displace;
    std::string str_tmp;
    int natmin = system->natmin;
    int is, is2, i_atm, ixyz, ijk;
    auto ns = dynamical->neval;
    double unit;
    double a[3][3];
    double u_fractional[3], u_xyz[3];

    // initialize q0
    for(is = 0; is < ns; is++){
        q0[is] = 0.0;
    }

    fin_displace.open("displace.in");

    if(!fin_displace){
        std::cout << "Warning in Scph::read_original_q0: file displace.in could not open." << std::endl;
        std::cout << "all q0 is set 0." << std::endl;
        return ;
    }

    fin_displace >> str_tmp;
    fin_displace >> unit;
    fin_displace >> a[0][0] >> a[0][1] >> a[0][2];
    fin_displace >> a[1][0] >> a[1][1] >> a[1][2];
    fin_displace >> a[2][0] >> a[2][1] >> a[2][2];

    for(ijk = 0; ijk < 3; ijk++){
        for(ixyz = 0; ixyz < 3; ixyz++){
            a[ixyz][ijk] *= unit;
        }
    }

    fin_displace >> str_tmp;


    for(i_atm = 0; i_atm < natmin; i_atm++){
        fin_displace >> u_fractional[0] >> u_fractional[1] >> u_fractional[2];

        // std::cout << u_fractional[0] << " " << u_fractional[1] << " " << u_fractional[2] << std::endl;

        for(ixyz = 0; ixyz < 3; ixyz++){
            u_xyz[ixyz] = 0.0;
            for(ijk = 0; ijk < 3; ijk++){
                u_xyz[ixyz] += a[ijk][ixyz] * u_fractional[ijk];
            }
        }

        for(is = 0; is < ns; is++){
            for(ixyz = 0; ixyz < 3; ixyz++){
                q0[is] += evec_harmonic[0][is][i_atm*3+ixyz].real() * std::sqrt(system->mass[system->map_p2s[i_atm][0]]) * u_xyz[ixyz]; 
            }
        }
    }
    
    fin_displace.close();
    return ;
}


void Scph::read_initial_strain(double **u_tensor)
{
    std::fstream fin_strain;
    std::string str_tmp;
    int i, j;

    fin_strain.open("strain.in");

    if(!fin_strain){
        std::cout << "Warning in Scph::read_initial_strain: file strain.in could not open." << std::endl;
        std::cout << "all u_tensor is not set." << std::endl;
        return ;
    }

    fin_strain >> str_tmp;
    fin_strain >> u_tensor[0][0] >> u_tensor[0][1] >> u_tensor[0][2];
    fin_strain >> u_tensor[1][0] >> u_tensor[1][1] >> u_tensor[1][2]; 
    fin_strain >> u_tensor[2][0] >> u_tensor[2][1] >> u_tensor[2][2]; 

    fin_strain.close();

    return ;
}


void Scph::read_Tdep_initial_q0_from_u(double *q0, int i_temp_loop)
{
    std::fstream fin_displace;
    std::string str_tmp;
    int natmin = system->natmin;
    int is, i_atm, ixyz, i;
    auto ns = dynamical->neval;
    //double unit;
    double a[3][3];
    double d_tmp, temp_tmp;
    //double u_fractional[3], u_xyz[3];

    std::vector<double> u_xyz;

    // initialize q0
    for(is = 0; is < ns; is++){
        q0[is] = 0.0;
    }

    fin_displace.open("u0.in");

    if(!fin_displace){
        std::cout << "Warning in Scph::read_Tdep_initial_q0_from_u: file u0.in could not open." << std::endl;
        std::cout << "all q0 is set 0." << std::endl;
        return ;
    }

    // read unnecessary lines
    for(i = 0; i < i_temp_loop; i++){
        std::getline(fin_displace, str_tmp); 
    }
    fin_displace >> temp_tmp;
    for(is = 0; is < ns; is++){
        fin_displace >> d_tmp;
        u_xyz.push_back(d_tmp);
    }

    for(is = 0; is < ns; is++){
        for(i_atm = 0; i_atm < natmin; i_atm++){
            for(ixyz = 0; ixyz < 3; ixyz++){
                q0[is] += evec_harmonic[0][is][i_atm*3+ixyz].real() * std::sqrt(system->mass[system->map_p2s[i_atm][0]]) * u_xyz[i_atm*3+ixyz]; 
            }
        }
    }

    std::cout << "temperature = " << temp_tmp << std::endl;
    std::cout << "initial q0: " << std::endl;
    for(is = 0; is < ns; is++){
        std::cout << q0[is] << " ";
    }std::cout << std::endl;
    
    fin_displace.close();
    return ;
}

void Scph::calculate_u0(double *q0, double *u0){
    int natmin = system->natmin;
    int is, is2, i_atm, ixyz;
    auto ns = dynamical->neval;

    for(i_atm = 0; i_atm < natmin; i_atm++){
        for(ixyz = 0; ixyz < 3; ixyz++){
            is = i_atm*3 + ixyz;
            u0[is] = 0.0;
            for(is2 = 0; is2 < ns; is2++){
                // skip acoustic mode
                if(std::fabs(omega2_harmonic[0][is2]) < eps8){
                    continue;
                }
                u0[is] += evec_harmonic[0][is2][is].real() * q0[is2];
            }
            u0[is] /= std::sqrt(system->mass[system->map_p2s[i_atm][0]]);
        }
    }

    return;
}

void Scph::calculate_force_in_real_space(std::complex<double> *v1_array_renormalized, double *force_array)
{   
    // std::ofstream fout_force;
    int natmin = system->natmin;
    auto ns = dynamical->neval;
    int is, iatm, ixyz;
    double force[3] = {0.0, 0.0, 0.0};

    // fout_force.open("force.out");

    for(iatm = 0; iatm < natmin; iatm++){
        for(ixyz = 0; ixyz < 3; ixyz++){
            force[ixyz] = 0.0;
            for(is = 0; is < ns; is++){
                force[ixyz] -= evec_harmonic[0][is][iatm*3+ixyz].real() * std::sqrt(system->mass[system->map_p2s[iatm][0]]) * v1_array_renormalized[is].real();
            }
            force_array[iatm*3 + ixyz] = force[ixyz];
        }
        // fout_force << force[0] << " " << force[1] << " " << force[2] << std::endl;
    }
}


void Scph::compute_V3_elements_mpi_over_kpoint(std::complex<double> ***v3_out,
                                               const std::complex<double> *const *const *evec_in,
                                               const bool self_offdiag)
{
    // Calculate the matrix elements of quartic terms in reciprocal space.
    // This is the most expensive part of the SCPH calculation.

    auto ns = dynamical->neval;
    auto ns2 = ns * ns;
    auto ns3 = ns * ns * ns;
    unsigned int is, js, ks;
    unsigned int **ind;
    unsigned int i, j;
    std::complex<double> ret;
    long int ii;

    const auto nk_scph = kmesh_dense->nk;
    const auto ngroup_v3 = anharmonic_core->get_ngroup_fcs(3);
    const auto factor = std::pow(0.5, 2) / static_cast<double>(nk_scph);
    static auto complex_zero = std::complex<double>(0.0, 0.0);
    std::complex<double> *v3_array_at_kpair;
    std::complex<double> ***v3_mpi;

    if (mympi->my_rank == 0) {
        if (self_offdiag) {
            std::cout << " SELF_OFFDIAG = 1: Calculating all components of v3_array ... ";
        } else {
            std::cout << " SELF_OFFDIAG = 0: Calculating diagonal components of v3_array ... ";
        }
    }

    allocate(v3_array_at_kpair, ngroup_v3);
    allocate(ind, ngroup_v3, 3);
    allocate(v3_mpi, nk_scph, ns, ns2);

    for (unsigned int ik = mympi->my_rank; ik < nk_scph; ik += mympi->nprocs) {

        anharmonic_core->calc_phi3_reciprocal(kmesh_dense->xk[ik],
                                              kmesh_dense->xk[kmesh_dense->kindex_minus_xk[ik]],
                                              phase_factor_scph,
                                              phi3_reciprocal);

#ifdef _OPENMP
#pragma omp parallel for private(j)
#endif
        for (ii = 0; ii < ngroup_v3; ++ii) {
            v3_array_at_kpair[ii] = phi3_reciprocal[ii] * anharmonic_core->get_invmass_factor(3)[ii];
            for (j = 0; j < 3; ++j) ind[ii][j] = anharmonic_core->get_evec_index(3)[ii][j];
        }

#pragma omp parallel for private(is)
        for (ii = 0; ii < ns; ++ii) {
            for (is = 0; is < ns2; ++is) {
                v3_mpi[ik][ii][is] = complex_zero;
                v3_out[ik][ii][is] = complex_zero;
            }
        }

        if (self_offdiag) {

            // All matrix elements will be calculated when considering the off-diagonal
            // elements of the phonon self-energy (i.e., when considering polarization mixing).

#pragma omp parallel for private(is, js, ks, ret, i)
            for (ii = 0; ii < ns3; ++ii) {
                is = ii / ns2;
                js = (ii - ns2 * is) / ns;
                ks = ii % ns;

                ret = std::complex<double>(0.0, 0.0);

                for (i = 0; i < ngroup_v3; ++i) {

                    ret += v3_array_at_kpair[i]
                           * evec_in[0][is][ind[i][0]]
                           * evec_in[ik][js][ind[i][1]]
                           * std::conj(evec_in[ik][ks][ind[i][2]]);
                }

                v3_mpi[ik][is][ns * js + ks] = factor * ret;
            }

        } else {

            // Only diagonal elements will be computed when neglecting the polarization mixing.

            if (ik == 0) {
#pragma omp parallel for private(is, js, ks, ret, i)
                for (ii = 0; ii < ns3; ++ii) {
                    is = ii / ns2;
                    js = (ii - ns2 * is) / ns;
                    ks = ii % ns;

                    ret = std::complex<double>(0.0, 0.0);

                    for (i = 0; i < ngroup_v3; ++i) {

                        ret += v3_array_at_kpair[i]
                               * evec_in[0][is][ind[i][0]]
                               * evec_in[ik][js][ind[i][1]]
                               * std::conj(evec_in[ik][ks][ind[i][2]]);
                    }

                    v3_mpi[ik][is][ns * js + ks] = factor * ret;
                }
            } else {

#pragma omp parallel for private(is, js, ret, i)
                for (ii = 0; ii < ns2; ++ii) {
                    is = ii / ns;
                    js = ii % ns;

                    ret = std::complex<double>(0.0, 0.0);

                    for (i = 0; i < ngroup_v3; ++i) {

                        ret += v3_array_at_kpair[i]
                               * evec_in[0][is][ind[i][0]]
                               * evec_in[ik][js][ind[i][1]]
                               * std::conj(evec_in[ik][js][ind[i][2]]);
                    }

                    v3_mpi[ik][is][(ns + 1) * js] = factor * ret;
                }
            }
        }
    }

    deallocate(v3_array_at_kpair);
    deallocate(ind);
#ifdef MPI_CXX_DOUBLE_COMPLEX
    MPI_Allreduce(&v3_mpi[0][0][0], &v3_out[0][0][0],
                  static_cast<int>(nk_scph) * ns3,
                  MPI_CXX_DOUBLE_COMPLEX, MPI_SUM, MPI_COMM_WORLD);
#else
    MPI_Allreduce(&v3_mpi[0][0][0], &v3_out[0][0][0], static_cast<int>(nk_scph) * ns3,
                  MPI_COMPLEX16, MPI_SUM, MPI_COMM_WORLD);
#endif

    deallocate(v3_mpi);

    zerofill_elements_acoustic_at_gamma(omega2_harmonic, v3_out, 3);

    if (mympi->my_rank == 0) {
        std::cout << " done !" << std::endl;
        timer->print_elapsed();
    }
}

void Scph::compute_V3_elements_for_given_IFCs(std::complex<double> ***v3_out,
                                               const int ngroup_v3_in,
                                               std::vector<double> *fcs_group_v3_in,
                                               std::vector<RelativeVector> *relvec_v3_in,
                                               double *invmass_v3_in,
                                               int **evec_index_v3_in,
                                               const std::complex<double> *const *const *evec_in,
                                               const bool self_offdiag)
{

    auto ns = dynamical->neval;
    auto ns2 = ns * ns;
    auto ns3 = ns * ns * ns;
    unsigned int is, js, ks;
    unsigned int **ind;
    unsigned int i, j;
    std::complex<double> ret;
    long int ii;

    const auto nk_scph = kmesh_dense->nk;
    // const auto ngroup_v3 = anharmonic_core->get_ngroup_fcs(3);
    // const auto ngroup_v3_in = fcs_group_v3_in.size();
    const auto factor = std::pow(0.5, 2) / static_cast<double>(nk_scph);
    static auto complex_zero = std::complex<double>(0.0, 0.0);
    std::complex<double> *v3_array_at_kpair;
    std::complex<double> ***v3_mpi;
    std::complex<double> *phi3_reciprocal_tmp;
    
    allocate(phi3_reciprocal_tmp, ngroup_v3_in);
    allocate(v3_array_at_kpair, ngroup_v3_in);
    allocate(ind, ngroup_v3_in, 3);
    allocate(v3_mpi, nk_scph, ns, ns2);

    for (unsigned int ik = mympi->my_rank; ik < nk_scph; ik += mympi->nprocs) {

        anharmonic_core->calc_phi3_reciprocal_for_given_IFCs(kmesh_dense->xk[ik],
                                              kmesh_dense->xk[kmesh_dense->kindex_minus_xk[ik]],
                                              ngroup_v3_in,
                                              fcs_group_v3_in,
                                              relvec_v3_in,
                                              phase_factor_scph,
                                              phi3_reciprocal_tmp);

        
#ifdef _OPENMP
#pragma omp parallel for private(j)
#endif
        for (ii = 0; ii < ngroup_v3_in; ++ii) {
            v3_array_at_kpair[ii] = phi3_reciprocal_tmp[ii] * invmass_v3_in[ii];
            for (j = 0; j < 3; ++j) ind[ii][j] = evec_index_v3_in[ii][j];
        }

#pragma omp parallel for private(is)
        for (ii = 0; ii < ns; ++ii) {
            for (is = 0; is < ns2; ++is) {
                v3_mpi[ik][ii][is] = complex_zero;
                v3_out[ik][ii][is] = complex_zero;
            }
        }

        if (self_offdiag) {

            // All matrix elements will be calculated when considering the off-diagonal
            // elements of the phonon self-energy (i.e., when considering polarization mixing).

#pragma omp parallel for private(is, js, ks, ret, i)
            for (ii = 0; ii < ns3; ++ii) {
                is = ii / ns2;
                js = (ii - ns2 * is) / ns;
                ks = ii % ns;

                ret = std::complex<double>(0.0, 0.0);

                for (i = 0; i < ngroup_v3_in; ++i) {

                    ret += v3_array_at_kpair[i]
                           * evec_in[0][is][ind[i][0]]
                           * evec_in[ik][js][ind[i][1]]
                           * std::conj(evec_in[ik][ks][ind[i][2]]);
                }

                v3_mpi[ik][is][ns * js + ks] = factor * ret;
            }

        } else {

            // Only diagonal elements will be computed when neglecting the polarization mixing.

            if (ik == 0) {
#pragma omp parallel for private(is, js, ks, ret, i)
                for (ii = 0; ii < ns3; ++ii) {
                    is = ii / ns2;
                    js = (ii - ns2 * is) / ns;
                    ks = ii % ns;

                    ret = std::complex<double>(0.0, 0.0);

                    for (i = 0; i < ngroup_v3_in; ++i) {

                        ret += v3_array_at_kpair[i]
                               * evec_in[0][is][ind[i][0]]
                               * evec_in[ik][js][ind[i][1]]
                               * std::conj(evec_in[ik][ks][ind[i][2]]);
                    }

                    v3_mpi[ik][is][ns * js + ks] = factor * ret;
                }
            } else {

#pragma omp parallel for private(is, js, ret, i)
                for (ii = 0; ii < ns2; ++ii) {
                    is = ii / ns;
                    js = ii % ns;

                    ret = std::complex<double>(0.0, 0.0);

                    for (i = 0; i < ngroup_v3_in; ++i) {

                        ret += v3_array_at_kpair[i]
                               * evec_in[0][is][ind[i][0]]
                               * evec_in[ik][js][ind[i][1]]
                               * std::conj(evec_in[ik][js][ind[i][2]]);
                    }

                    v3_mpi[ik][is][(ns + 1) * js] = factor * ret;
                }
            }
        }
    }

    deallocate(v3_array_at_kpair);
    deallocate(ind);
#ifdef MPI_CXX_DOUBLE_COMPLEX
    MPI_Allreduce(&v3_mpi[0][0][0], &v3_out[0][0][0],
                  static_cast<int>(nk_scph) * ns3,
                  MPI_CXX_DOUBLE_COMPLEX, MPI_SUM, MPI_COMM_WORLD);
#else
    MPI_Allreduce(&v3_mpi[0][0][0], &v3_out[0][0][0], static_cast<int>(nk_scph) * ns3,
                  MPI_COMPLEX16, MPI_SUM, MPI_COMM_WORLD);
#endif

    deallocate(v3_mpi);

    zerofill_elements_acoustic_at_gamma(omega2_harmonic, v3_out, 3);
}


void Scph::compute_V4_elements_mpi_over_kpoint(std::complex<double> ***v4_out,
                                               std::complex<double> ***evec_in,
                                               const bool self_offdiag,
                                               const bool relax)
{
    // Calculate the matrix elements of quartic terms in reciprocal space.
    // This is the most expensive part of the SCPH calculation.

    const size_t nk_reduced_interpolate = kmesh_coarse->nk_irred;
    const size_t ns = dynamical->neval;
    const size_t ns2 = ns * ns;
    const size_t ns3 = ns * ns * ns;
    const size_t ns4 = ns * ns * ns * ns;
    size_t is, js, ks, ls;
    unsigned int **ind;
    unsigned int i, j;
    std::complex<double> ret;
    long int ii;

    const auto nk_scph = kmesh_dense->nk;
    const auto ngroup_v4 = anharmonic_core->get_ngroup_fcs(4);
    const auto factor = std::pow(0.5, 2) / static_cast<double>(nk_scph);
    static auto complex_zero = std::complex<double>(0.0, 0.0);
    std::complex<double> *v4_array_at_kpair;
    std::complex<double> ***v4_mpi;

    const size_t nk2_prod = nk_reduced_interpolate * nk_scph;

    if (mympi->my_rank == 0) {
        if (self_offdiag) {
            std::cout << " SELF_OFFDIAG = 1: Calculating all components of v4_array ... ";
        } else {
            std::cout << " SELF_OFFDIAG = 0: Calculating diagonal components of v4_array ... ";
        }
    }

    allocate(v4_array_at_kpair, ngroup_v4);
    allocate(ind, ngroup_v4, 4);
    allocate(v4_mpi, nk2_prod, ns2, ns2);

    for (size_t ik_prod = mympi->my_rank; ik_prod < nk2_prod; ik_prod += mympi->nprocs) {
        const auto ik = ik_prod / nk_scph;
        const auto jk = ik_prod % nk_scph;

        const unsigned int knum = kmap_interpolate_to_scph[kmesh_coarse->kpoint_irred_all[ik][0].knum];

        anharmonic_core->calc_phi4_reciprocal(kmesh_dense->xk[knum],
                                              kmesh_dense->xk[jk],
                                              kmesh_dense->xk[kmesh_dense->kindex_minus_xk[jk]],
                                              phase_factor_scph,
                                              phi4_reciprocal);

#ifdef _OPENMP
#pragma omp parallel for private(j)
#endif
        for (ii = 0; ii < ngroup_v4; ++ii) {
            v4_array_at_kpair[ii] = phi4_reciprocal[ii] * anharmonic_core->get_invmass_factor(4)[ii];
            for (j = 0; j < 4; ++j) ind[ii][j] = anharmonic_core->get_evec_index(4)[ii][j];
        }

#pragma omp parallel for private(is)
        for (ii = 0; ii < ns2; ++ii) {
            for (is = 0; is < ns2; ++is) {
                v4_mpi[ik_prod][ii][is] = complex_zero;
                v4_out[ik_prod][ii][is] = complex_zero;
            }
        }

        if (self_offdiag) {

            // All matrix elements will be calculated when considering the off-diagonal
            // elements of the phonon self-energy (loop diagram).

#pragma omp parallel for private(is, js, ks, ls, ret, i)
            for (ii = 0; ii < ns4; ++ii) {
                is = ii / ns3;
                js = (ii - ns3 * is) / ns2;
                ks = (ii - ns3 * is - ns2 * js) / ns;
                ls = ii % ns;

                // if (is < js) continue;

                ret = std::complex<double>(0.0, 0.0);

                for (i = 0; i < ngroup_v4; ++i) {

                    ret += v4_array_at_kpair[i]
                           * std::conj(evec_in[knum][is][ind[i][0]])
                           * evec_in[knum][js][ind[i][1]]
                           * evec_in[jk][ks][ind[i][2]]
                           * std::conj(evec_in[jk][ls][ind[i][3]]);
                }

                v4_mpi[ik_prod][ns * is + js][ns * ks + ls] = factor * ret;
            }

        } else {

            // Only diagonal elements will be computed when neglecting the polarization mixing.

            if (relax && (knum == 0 || jk == 0)) {

#pragma omp parallel for private(is, js, ks, ls, ret, i)
                for (ii = 0; ii < ns4; ++ii) {
                    is = ii / ns3;
                    js = (ii - ns3 * is) / ns2;
                    ks = (ii - ns3 * is - ns2 * js) / ns;
                    ls = ii % ns;

                    // if (is < js) continue;

                    ret = std::complex<double>(0.0, 0.0);

                    for (i = 0; i < ngroup_v4; ++i) {

                        ret += v4_array_at_kpair[i]
                               * std::conj(evec_in[knum][is][ind[i][0]])
                               * evec_in[knum][js][ind[i][1]]
                               * evec_in[jk][ks][ind[i][2]]
                               * std::conj(evec_in[jk][ls][ind[i][3]]);
                    }

                    v4_mpi[ik_prod][ns * is + js][ns * ks + ls] = factor * ret;
                }

            } else {

#pragma omp parallel for private(is, js, ret, i)
                for (ii = 0; ii < ns2; ++ii) {
                    is = ii / ns;
                    js = ii % ns;

                    ret = std::complex<double>(0.0, 0.0);

                    for (i = 0; i < ngroup_v4; ++i) {

                        ret += v4_array_at_kpair[i]
                               * std::conj(evec_in[knum][is][ind[i][0]])
                               * evec_in[knum][is][ind[i][1]]
                               * evec_in[jk][js][ind[i][2]]
                               * std::conj(evec_in[jk][js][ind[i][3]]);
                    }

                    v4_mpi[ik_prod][(ns + 1) * is][(ns + 1) * js] = factor * ret;
                }
            }
        }
    }

    deallocate(v4_array_at_kpair);
    deallocate(ind);

// Now, communicate the calculated data.
// When the data count is larger than 2^31-1, split it.

    long maxsize = 1;
    maxsize = (maxsize << 31) - 1;

    const size_t count = nk2_prod * ns4;
    const size_t count_sub = ns4;

    if (count <= maxsize) {
#ifdef MPI_CXX_DOUBLE_COMPLEX
        MPI_Allreduce(&v4_mpi[0][0][0], &v4_out[0][0][0], count,
                      MPI_CXX_DOUBLE_COMPLEX, MPI_SUM, MPI_COMM_WORLD);
#else
        MPI_Allreduce(&v4_mpi[0][0][0], &v4_out[0][0][0], count,
                      MPI_COMPLEX16, MPI_SUM, MPI_COMM_WORLD);
#endif
    } else if (count_sub <= maxsize) {
        for (size_t ik_prod = 0; ik_prod < nk2_prod; ++ik_prod) {
#ifdef MPI_CXX_DOUBLE_COMPLEX
            MPI_Allreduce(&v4_mpi[ik_prod][0][0], &v4_out[ik_prod][0][0],
                          count_sub,
                          MPI_CXX_DOUBLE_COMPLEX, MPI_SUM, MPI_COMM_WORLD);
#else
            MPI_Allreduce(&v4_mpi[ik_prod][0][0], &v4_out[ik_prod][0][0],
                          count_sub,
                          MPI_COMPLEX16, MPI_SUM, MPI_COMM_WORLD);
#endif
        }
    } else {
        for (size_t ik_prod = 0; ik_prod < nk2_prod; ++ik_prod) {
            for (is = 0; is < ns2; ++is) {
#ifdef MPI_CXX_DOUBLE_COMPLEX
                MPI_Allreduce(&v4_mpi[ik_prod][is][0], &v4_out[ik_prod][is][0],
                              ns2,
                              MPI_CXX_DOUBLE_COMPLEX, MPI_SUM, MPI_COMM_WORLD);
#else
                MPI_Allreduce(&v4_mpi[ik_prod][is][0], &v4_out[ik_prod][is][0],
                              ns2,
                              MPI_COMPLEX16, MPI_SUM, MPI_COMM_WORLD);
#endif
            }
        }
    }

    deallocate(v4_mpi);

    zerofill_elements_acoustic_at_gamma(omega2_harmonic, v4_out, 4);

    if (mympi->my_rank == 0) {
        std::cout << " done !" << std::endl;
        timer->print_elapsed();
    }
}

void Scph::compute_V4_elements_mpi_over_band(std::complex<double> ***v4_out,
                                             std::complex<double> ***evec_in,
                                             const bool self_offdiag)
{
    // Calculate the matrix elements of quartic terms in reciprocal space.
    // This is the most expensive part of the SCPH calculation.

    size_t ik_prod;
    const size_t nk_reduced_interpolate = kmesh_coarse->nk_irred;
    const size_t ns = dynamical->neval;
    const size_t ns2 = ns * ns;
    const size_t ns4 = ns * ns * ns * ns;
    int is, js;
    unsigned int knum;
    unsigned int **ind;
    unsigned int i, j;
    long int *nset_mpi;

    const auto nk_scph = kmesh_dense->nk;
    const auto ngroup_v4 = anharmonic_core->get_ngroup_fcs(4);
    auto factor = std::pow(0.5, 2) / static_cast<double>(nk_scph);
    static auto complex_zero = std::complex<double>(0.0, 0.0);
    std::complex<double> *v4_array_at_kpair;
    std::complex<double> ***v4_mpi;

    std::vector<int> ik_vec, jk_vec, is_vec, js_vec;

    auto nk2_prod = nk_reduced_interpolate * nk_scph;

    if (mympi->my_rank == 0) {
        if (self_offdiag) {
            std::cout << " IALGO = 1 : Use different algorithm efficient when nbands >> nk\n";
            std::cout << " SELF_OFFDIAG = 1: Calculating all components of v4_array ... \n";
        } else {
            exit("compute_V4_elements_mpi_over_kpoint",
                 "This function can be used only when SELF_OFFDIAG = 1");
        }
    }

    allocate(nset_mpi, mympi->nprocs);

    long int nset_tot = nk2_prod * ((ns2 - ns) / 2 + ns);
    long int nset_each = nset_tot / mympi->nprocs;
    long int nres = nset_tot - nset_each * mympi->nprocs;

    for (i = 0; i < mympi->nprocs; ++i) {
        nset_mpi[i] = nset_each;
        if (nres > i) {
            nset_mpi[i] += 1;
        }
    }

    MPI_Bcast(&nset_mpi[0], mympi->nprocs, MPI_LONG, 0, MPI_COMM_WORLD);
    long int nstart = 0;
    for (i = 0; i < mympi->my_rank; ++i) {
        nstart += nset_mpi[i];
    }
    long int nend = nstart + nset_mpi[mympi->my_rank];
    nset_each = nset_mpi[mympi->my_rank];
    deallocate(nset_mpi);

    ik_vec.clear();
    jk_vec.clear();
    is_vec.clear();
    js_vec.clear();

    long int icount = 0;
    for (ik_prod = 0; ik_prod < nk2_prod; ++ik_prod) {
        for (is = 0; is < ns; ++is) {
            for (js = 0; js < ns; ++js) {
                // if (is < js) continue;

                if (icount >= nstart && icount < nend) {
                    ik_vec.push_back(ik_prod / nk_scph);
                    jk_vec.push_back(ik_prod % nk_scph);
                    is_vec.push_back(is);
                    js_vec.push_back(js);
                }
                ++icount;
            }
        }
    }

    allocate(v4_array_at_kpair, ngroup_v4);
    allocate(ind, ngroup_v4, 4);
    allocate(v4_mpi, nk2_prod, ns2, ns2);

    for (ik_prod = 0; ik_prod < nk2_prod; ++ik_prod) {
#pragma omp parallel for private (js)
        for (is = 0; is < ns2; ++is) {
            for (js = 0; js < ns2; ++js) {
                v4_mpi[ik_prod][is][js] = complex_zero;
                v4_out[ik_prod][is][js] = complex_zero;
            }
        }
    }

    int ik_old = -1;
    int jk_old = -1;

    if (mympi->my_rank == 0) {
        std::cout << " Total number of sets to compute : " << nset_each << std::endl;
    }

    for (long int ii = 0; ii < nset_each; ++ii) {

        auto ik_now = ik_vec[ii];
        auto jk_now = jk_vec[ii];
        auto is_now = is_vec[ii];
        auto js_now = js_vec[ii];

        if (!(ik_now == ik_old && jk_now == jk_old)) {

            // Update v4_array_at_kpair and ind

            knum = kmap_interpolate_to_scph[kmesh_coarse->kpoint_irred_all[ik_now][0].knum];

            anharmonic_core->calc_phi4_reciprocal(kmesh_dense->xk[knum],
                                                  kmesh_dense->xk[jk_now],
                                                  kmesh_dense->xk[kmesh_dense->kindex_minus_xk[jk_now]],
                                                  phase_factor_scph,
                                                  phi4_reciprocal);

#ifdef _OPENMP
#pragma omp parallel for private(j)
#endif
            for (ii = 0; ii < ngroup_v4; ++ii) {
                v4_array_at_kpair[ii] = phi4_reciprocal[ii] * anharmonic_core->get_invmass_factor(4)[ii];
                for (j = 0; j < 4; ++j) ind[ii][j] = anharmonic_core->get_evec_index(4)[ii][j];
            }
            ik_old = ik_now;
            jk_old = jk_now;
        }

        ik_prod = ik_now * nk_scph + jk_now;
        int is_prod = ns * is_now + js_now;

#pragma omp parallel for private (i)
        for (js = 0; js < ns2; ++js) {

            unsigned int ks = js / ns;
            unsigned int ls = js % ns;

            auto ret = std::complex<double>(0.0, 0.0);

            for (i = 0; i < ngroup_v4; ++i) {

                ret += v4_array_at_kpair[i]
                       * std::conj(evec_in[knum][is_now][ind[i][0]])
                       * evec_in[knum][js_now][ind[i][1]]
                       * evec_in[jk_now][ks][ind[i][2]]
                       * std::conj(evec_in[jk_now][ls][ind[i][3]]);
            }

            v4_mpi[ik_prod][is_prod][js] = factor * ret;
        }

        if (mympi->my_rank == 0) {
            std::cout << " SET " << ii + 1 << " done. " << std::endl;
        }

    } // loop over nk2_prod*ns2

    deallocate(v4_array_at_kpair);
    deallocate(ind);

// Now, communicate the calculated data.
// When the data count is larger than 2^31-1, split it.

    long maxsize = 1;
    maxsize = (maxsize << 31) - 1;

    const size_t count = nk2_prod * ns4;
    const size_t count_sub = ns4;

    if (mympi->my_rank == 0) {
        std::cout << "Communicating v4_array over MPI ...";
    }
    if (count <= maxsize) {
#ifdef MPI_CXX_DOUBLE_COMPLEX
        MPI_Allreduce(&v4_mpi[0][0][0], &v4_out[0][0][0], count,
                      MPI_CXX_DOUBLE_COMPLEX, MPI_SUM, MPI_COMM_WORLD);
#else
        MPI_Allreduce(&v4_mpi[0][0][0], &v4_out[0][0][0], count,
                      MPI_COMPLEX16, MPI_SUM, MPI_COMM_WORLD);
#endif
    } else if (count_sub <= maxsize) {
        for (size_t ik_prod = 0; ik_prod < nk2_prod; ++ik_prod) {
#ifdef MPI_CXX_DOUBLE_COMPLEX
            MPI_Allreduce(&v4_mpi[ik_prod][0][0], &v4_out[ik_prod][0][0],
                          count_sub,
                          MPI_CXX_DOUBLE_COMPLEX, MPI_SUM, MPI_COMM_WORLD);
#else
            MPI_Allreduce(&v4_mpi[ik_prod][0][0], &v4_out[ik_prod][0][0],
                          count_sub,
                          MPI_COMPLEX16, MPI_SUM, MPI_COMM_WORLD);
#endif
        }
    } else {
        for (size_t ik_prod = 0; ik_prod < nk2_prod; ++ik_prod) {
            for (is = 0; is < ns2; ++is) {
#ifdef MPI_CXX_DOUBLE_COMPLEX
                MPI_Allreduce(&v4_mpi[ik_prod][is][0], &v4_out[ik_prod][is][0],
                              ns2,
                              MPI_CXX_DOUBLE_COMPLEX, MPI_SUM, MPI_COMM_WORLD);
#else
                MPI_Allreduce(&v4_mpi[ik_prod][is][0], &v4_out[ik_prod][is][0],
                              ns2,
                              MPI_COMPLEX16, MPI_SUM, MPI_COMM_WORLD);
#endif
            }
        }
    }
    if (mympi->my_rank == 0) {
        std::cout << "done.\n";
    }

    deallocate(v4_mpi);

    zerofill_elements_acoustic_at_gamma(omega2_harmonic, v4_out, 4);

    if (mympi->my_rank == 0) {
        std::cout << " done !" << std::endl;
        timer->print_elapsed();
    }
}

void Scph::zerofill_elements_acoustic_at_gamma(double **omega2,
                                               std::complex<double> ***v_elems,
                                               const int fc_order) const
{
    // Set V3 or V4 elements involving acoustic modes at Gamma point
    // exactly zero.

    int jk;
    int is, js, ks, ls;
    const auto ns = dynamical->neval;
    bool *is_acoustic;
    allocate(is_acoustic, ns);
    int nacoustic;
    auto threshould = 1.0e-24;
    const auto nk_reduced_interpolate = kmesh_coarse->nk_irred;
    static auto complex_zero = std::complex<double>(0.0, 0.0);

    if (!(fc_order == 3 || fc_order == 4)) {
        exit("zerofill_elements_acoustic_at_gamma",
             "The fc_order must be either 3 or 4.");
    }

    do {
        nacoustic = 0;
        for (is = 0; is < ns; ++is) {
            if (std::abs(omega2[0][is]) < threshould) {
                is_acoustic[is] = true;
                ++nacoustic;
            } else {
                is_acoustic[is] = false;
            }
        }
        if (nacoustic > 3) {
            exit("zerofill_elements_acoustic_at_gamma",
                 "Could not assign acoustic modes at Gamma.");
        }
        threshould *= 2.0;
    } while (nacoustic < 3);

    if (fc_order == 3) {

        // Set V3 to zeros so as to avoid mixing with gamma acoustic modes
        // jk = 0;
        for (is = 0; is < ns; ++is) {
            for (ks = 0; ks < ns; ++ks) {
                for (ls = 0; ls < ns; ++ls) {
                    if (is_acoustic[ks] || is_acoustic[ls]) {
                        v_elems[0][is][ns * ks + ls] = complex_zero;
                    }
                }
            }
        }

        // ik = 0;
        for (jk = 0; jk < kmesh_dense->nk; ++jk) {
            for (is = 0; is < ns; ++is) {
                if (is_acoustic[is]) {
                    for (ks = 0; ks < ns; ++ks) {
                        for (ls = 0; ls < ns; ++ls) {
                            v_elems[jk][is][ns * ks + ls] = complex_zero;
                        }
                    }
                }
            }
        }

    } else if (fc_order == 4) {
        // Set V4 to zeros so as to avoid mixing with gamma acoustic modes
        // jk = 0;
        for (int ik = 0; ik < nk_reduced_interpolate; ++ik) {
            for (is = 0; is < ns; ++is) {
                for (js = 0; js < ns; ++js) {
                    for (ks = 0; ks < ns; ++ks) {
                        for (ls = 0; ls < ns; ++ls) {
                            if (is_acoustic[ks] || is_acoustic[ls]) {
                                v_elems[kmesh_dense->nk * ik][ns * is + js][ns * ks + ls] = complex_zero;
                            }
                        }
                    }
                }
            }
        }
        // ik = 0;
        for (jk = 0; jk < kmesh_dense->nk; ++jk) {
            for (is = 0; is < ns; ++is) {
                for (js = 0; js < ns; ++js) {
                    if (is_acoustic[is] || is_acoustic[js]) {
                        for (ks = 0; ks < ns; ++ks) {
                            for (ls = 0; ls < ns; ++ls) {
                                v_elems[jk][ns * is + js][ns * ks + ls] = complex_zero;
                            }
                        }
                    }
                }
            }
        }
    }

    deallocate(is_acoustic);
}

void Scph::compute_del_v1_strain_from_harmonic(std::complex<double> **del_v1_strain_from_harmonic,
                                               std::complex<double> ***evec_harmonic)
{
    // calculate renormalization in real space
    int natmin = system->natmin;
    int nat = system->nat;
    int ns = dynamical->neval;
    double **del_v1_strain_from_harmonic_in_real_space;
    allocate(del_v1_strain_from_harmonic_in_real_space, 9, ns);

    double *inv_sqrt_mass;

    int i, ind1, is1;
    int ixyz, ixyz1;
    double vec[3];

    // prepare supercell shift
    double **xshift_s;
    const auto ncell_s = 27;

    allocate(xshift_s, ncell_s, 3);

    unsigned int icell = 0;
    int ix, iy, iz;
    for (i = 0; i < 3; ++i) xshift_s[0][i] = 0;
    icell = 1;
    for (ix = -1; ix <= 1; ++ix) {
        for (iy = -1; iy <= 1; ++iy) {
            for (iz = -1; iz <= 1; ++iz) {
                if (ix == 0 && iy == 0 && iz == 0) continue;

                xshift_s[icell][0] = ix*1.0;
                xshift_s[icell][1] = iy*1.0;
                xshift_s[icell][2] = iz*1.0;

                ++icell;
            }
        }
    }

    // calculate renormalization in real space
    for(ixyz = 0; ixyz < 9; ixyz++){
        for(i = 0; i < ns; i++){
            del_v1_strain_from_harmonic_in_real_space[ixyz][i] = 0.0;
        }
    }

    for(auto &it : fcs_phonon->fc2_ext){
        ind1 = it.atm1*3 + it.xyz1;

        for(ixyz1 = 0; ixyz1 < 3; ixyz1++){
            vec[ixyz1] = system->xr_s_no_displace[it.atm2][ixyz1]
                        - system->xr_s_no_displace[system->map_p2s[it.atm1][0]][ixyz1]
                        + xshift_s[it.cell_s][ixyz1];
        }
        rotvec(vec, vec, system->lavec_s);
        for(ixyz1 = 0; ixyz1 < 3; ixyz1++){
            del_v1_strain_from_harmonic_in_real_space[it.xyz2*3 + ixyz1][ind1] += it.fcs_val * vec[ixyz1];
        }
    }


    // transform to Fourier space

    allocate(inv_sqrt_mass, natmin);
    for(i = 0; i < natmin; i++){
        inv_sqrt_mass[i] = 1.0/std::sqrt(system->mass[system->map_p2s[i][0]]);
    }

    for(ixyz = 0; ixyz < 9; ixyz++){
        for(is1 = 0; is1 < ns; is1++){
            del_v1_strain_from_harmonic[ixyz][is1] = 0.0;
            for(i = 0; i < natmin; i++){
                for(ixyz1 = 0; ixyz1 < 3; ixyz1++){
                    del_v1_strain_from_harmonic[ixyz][is1] += evec_harmonic[0][is1][i*3+ixyz1] * inv_sqrt_mass[i] * del_v1_strain_from_harmonic_in_real_space[ixyz][i*3+ixyz1];
                }
            }
        }
    }

    deallocate(del_v1_strain_from_harmonic_in_real_space);
    deallocate(inv_sqrt_mass);
    deallocate(xshift_s);

    return;
}

void Scph::compute_del_v1_strain_from_cubic(std::complex<double> **del_v1_strain_from_cubic,
                                               std::complex<double> ***evec_harmonic)
{
    // calculate renormalization in real space
    int natmin = system->natmin;
    int nat = system->nat;
    int ns = dynamical->neval;
    double **del_v1_strain_from_cubic_in_real_space;
    allocate(del_v1_strain_from_cubic_in_real_space, 81, ns);

    double *inv_sqrt_mass;

    int i, ind1, is1;
    int ixyz, ixyz1, ixyz2;
    int ixyz_comb;
    double vec1[3], vec2[3];

    // prepare supercell shift
    double **xshift_s;
    const auto ncell_s = 27;

    allocate(xshift_s, ncell_s, 3);

    unsigned int icell = 0;
    int ix, iy, iz;
    for (i = 0; i < 3; ++i) xshift_s[0][i] = 0;
    icell = 1;
    for (ix = -1; ix <= 1; ++ix) {
        for (iy = -1; iy <= 1; ++iy) {
            for (iz = -1; iz <= 1; ++iz) {
                if (ix == 0 && iy == 0 && iz == 0) continue;

                xshift_s[icell][0] = ix*1.0;
                xshift_s[icell][1] = iy*1.0;
                xshift_s[icell][2] = iz*1.0;

                ++icell;
            }
        }
    }

    // calculate renormalization in real space
    for(ixyz = 0; ixyz < 81; ixyz++){
        for(i = 0; i < ns; i++){
            del_v1_strain_from_cubic_in_real_space[ixyz][i] = 0.0;
        }
    }

    for(auto &it : fcs_phonon->force_constant_with_cell[1]){

        ind1 = it.pairs[0].index;

        for(ixyz1 = 0; ixyz1 < 3; ixyz1++){
            vec1[ixyz1] = system->xr_s_no_displace[system->map_p2s[it.pairs[1].index / 3][it.pairs[1].tran]][ixyz1]
                        - system->xr_s_no_displace[system->map_p2s[it.pairs[0].index / 3][0]][ixyz1]
                        + xshift_s[it.pairs[1].cell_s][ixyz1];
            vec2[ixyz1] = system->xr_s_no_displace[system->map_p2s[it.pairs[2].index / 3][it.pairs[2].tran]][ixyz1]
                        - system->xr_s_no_displace[system->map_p2s[it.pairs[0].index / 3][0]][ixyz1]
                        + xshift_s[it.pairs[2].cell_s][ixyz1];
        }
        rotvec(vec1, vec1, system->lavec_s);
        rotvec(vec2, vec2, system->lavec_s);

        for(ixyz1 = 0; ixyz1 < 3; ixyz1++){
            for(ixyz2 = 0; ixyz2 < 3; ixyz2++){
                ixyz_comb = (it.pairs[1].index%3)*27 + ixyz1*9 + (it.pairs[2].index%3)*3 + ixyz2;
                del_v1_strain_from_cubic_in_real_space[ixyz_comb][ind1] += it.fcs_val * vec1[ixyz1] * vec2[ixyz2];
            }
        }
    }


    // transform to Fourier space
    allocate(inv_sqrt_mass, natmin);
    for(i = 0; i < natmin; i++){
        inv_sqrt_mass[i] = 1.0/std::sqrt(system->mass[system->map_p2s[i][0]]);
    }

    for(ixyz = 0; ixyz < 81; ixyz++){
        for(is1 = 0; is1 < ns; is1++){
            del_v1_strain_from_cubic[ixyz][is1] = 0.0;
            for(i = 0; i < natmin; i++){
                for(ixyz1 = 0; ixyz1 < 3; ixyz1++){
                    del_v1_strain_from_cubic[ixyz][is1] += evec_harmonic[0][is1][i*3+ixyz1] * inv_sqrt_mass[i] * del_v1_strain_from_cubic_in_real_space[ixyz][i*3+ixyz1];
                }
            }
        }
    }
    deallocate(del_v1_strain_from_cubic_in_real_space);
    deallocate(inv_sqrt_mass);
    deallocate(xshift_s);

    return;
}

void Scph::compute_del_v1_strain_from_quartic(std::complex<double> **del_v1_strain_from_quartic,
                                               std::complex<double> ***evec_harmonic)
{
    // calculate renormalization in real space
    int natmin = system->natmin;
    int nat = system->nat;
    int ns = dynamical->neval;
    double **del_v1_strain_from_quartic_in_real_space;

    allocate(del_v1_strain_from_quartic_in_real_space, 729, ns);

    double *inv_sqrt_mass;

    int i, ind1, is1;
    int ixyz, ixyz1, ixyz2, ixyz3;
    int ixyz_comb;
    double vec1[3], vec2[3], vec3[3];

    // prepare supercell shift
    double **xshift_s;
    const auto ncell_s = 27;

    allocate(xshift_s, ncell_s, 3);

    unsigned int icell = 0;
    int ix, iy, iz;
    for (i = 0; i < 3; ++i) xshift_s[0][i] = 0;
    icell = 1;
    for (ix = -1; ix <= 1; ++ix) {
        for (iy = -1; iy <= 1; ++iy) {
            for (iz = -1; iz <= 1; ++iz) {
                if (ix == 0 && iy == 0 && iz == 0) continue;

                xshift_s[icell][0] = ix*1.0;
                xshift_s[icell][1] = iy*1.0;
                xshift_s[icell][2] = iz*1.0;

                ++icell;
            }
        }
    }

    // calculate renormalization in real space
    for(ixyz = 0; ixyz < 729; ixyz++){
        for(i = 0; i < ns; i++){
            del_v1_strain_from_quartic_in_real_space[ixyz][i] = 0.0;
        }
    }

    for(auto &it : fcs_phonon->force_constant_with_cell[2]){

        ind1 = it.pairs[0].index;

        for(ixyz1 = 0; ixyz1 < 3; ixyz1++){
            vec1[ixyz1] = system->xr_s_no_displace[system->map_p2s[it.pairs[1].index / 3][it.pairs[1].tran]][ixyz1]
                        - system->xr_s_no_displace[system->map_p2s[it.pairs[0].index / 3][0]][ixyz1]
                        + xshift_s[it.pairs[1].cell_s][ixyz1];
            vec2[ixyz1] = system->xr_s_no_displace[system->map_p2s[it.pairs[2].index / 3][it.pairs[2].tran]][ixyz1]
                        - system->xr_s_no_displace[system->map_p2s[it.pairs[0].index / 3][0]][ixyz1]
                        + xshift_s[it.pairs[2].cell_s][ixyz1];
            vec3[ixyz1] = system->xr_s_no_displace[system->map_p2s[it.pairs[3].index / 3][it.pairs[3].tran]][ixyz1]
                        - system->xr_s_no_displace[system->map_p2s[it.pairs[0].index / 3][0]][ixyz1]
                        + xshift_s[it.pairs[3].cell_s][ixyz1];
        }
        rotvec(vec1, vec1, system->lavec_s);
        rotvec(vec2, vec2, system->lavec_s);
        rotvec(vec3, vec3, system->lavec_s);

        for(ixyz1 = 0; ixyz1 < 3; ixyz1++){
            for(ixyz2 = 0; ixyz2 < 3; ixyz2++){
                for(ixyz3 = 0; ixyz3 < 3; ixyz3++){
                    ixyz_comb = (it.pairs[1].index%3)*243 + ixyz1*81 + (it.pairs[2].index%3)*27 + ixyz2*9 + (it.pairs[3].index%3)*3 + ixyz3;
                    del_v1_strain_from_quartic_in_real_space[ixyz_comb][ind1] += it.fcs_val * vec1[ixyz1] * vec2[ixyz2] * vec3[ixyz3];
                }
            }
        }
    }


    // transform to Fourier space

    allocate(inv_sqrt_mass, natmin);
    for(i = 0; i < natmin; i++){
        inv_sqrt_mass[i] = 1.0/std::sqrt(system->mass[system->map_p2s[i][0]]);
    }

    for(ixyz = 0; ixyz < 729; ixyz++){
        for(is1 = 0; is1 < ns; is1++){
            del_v1_strain_from_quartic[ixyz][is1] = 0.0;
            for(i = 0; i < natmin; i++){
                for(ixyz1 = 0; ixyz1 < 3; ixyz1++){
                    del_v1_strain_from_quartic[ixyz][is1] += evec_harmonic[0][is1][i*3+ixyz1] * inv_sqrt_mass[i] * del_v1_strain_from_quartic_in_real_space[ixyz][i*3+ixyz1];
                }
            }
        }
    }
    deallocate(del_v1_strain_from_quartic_in_real_space);
    deallocate(inv_sqrt_mass);
    deallocate(xshift_s);

    return;
}

void Scph::compute_del_v2_strain_from_cubic(std::complex<double> ***del_v2_strain_from_cubic,
                                            std::complex<double> ***evec_harmonic)
{
    using namespace Eigen;

    const auto ns = dynamical->neval;
    const auto nk = kmesh_dense->nk;
    const auto nk_interpolate = kmesh_coarse->nk;
    int ixyz1, ixyz2;
    int is1, is2, ik, knum;

    std::vector<FcsArrayWithCell> delta_fcs;
    std::vector<FcsClassExtent> delta_fcs2;
    FcsClassExtent fc_extent_tmp;

    std::complex<double> **mat_tmp;
    allocate(mat_tmp, ns, ns);

    MatrixXcd Dymat(ns, ns);
    MatrixXcd evec_tmp(ns, ns);
    
    for(ixyz1 = 0; ixyz1 < 3; ixyz1++){
        for(ixyz2 = 0; ixyz2 < 3; ixyz2++){
            // calculate renormalization in real space
            compute_del_v_strain_in_real_space1(fcs_phonon->force_constant_with_cell[1],
                                                           delta_fcs, ixyz1, ixyz2, 1);

            // change format of harmonic IFCs.
            delta_fcs2.clear();

            for (const auto &it: delta_fcs){
                fc_extent_tmp = from_FcsArrayWithCell_to_FcsClassExtent(it);
                delta_fcs2.emplace_back(fc_extent_tmp);
            }
            
            for(ik = 0; ik < nk; ik++){
                // Fourier transformation
                dynamical->calc_analytic_k(kmesh_dense->xk[ik],
                                        delta_fcs2,
                                        mat_tmp);
                // Unitary transformation
                // knum = kmap_interpolate_to_scph[ik];
                knum = ik;
                for(is1 = 0; is1 < ns; is1++){
                    for(is2 = 0; is2 < ns; is2++){
                        Dymat(is1, is2) = mat_tmp[is1][is2];
                        evec_tmp(is1, is2) = evec_harmonic[knum][is2][is1]; // transpose
                    }
                }
                Dymat = evec_tmp.adjoint() * Dymat * evec_tmp;

                // copy result to del_v2_strain_from_cubic
                for(is1 = 0; is1 < ns; is1++){
                    for(is2 = 0; is2 < ns; is2++){
                        del_v2_strain_from_cubic[ixyz1*3 + ixyz2][ik][is1*ns+is2] = Dymat(is1, is2);
                    }
                }
            }
        }
    }

    deallocate(mat_tmp);
}

void Scph::compute_del_v2_strain_from_quartic(std::complex<double> ***del_v2_strain_from_quartic,
                                            std::complex<double> ***evec_harmonic)
{
    using namespace Eigen;

    const auto ns = dynamical->neval;
    const auto nk = kmesh_dense->nk;
    const auto nk_interpolate = kmesh_coarse->nk;
    int ixyz11, ixyz12, ixyz21, ixyz22, ixyz, itmp;
    int is1, is2, ik, knum;

#pragma omp parallel private(ixyz, itmp, ixyz11, ixyz12, ixyz21, ixyz22, is1, is2, ik, knum)
{
    std::vector<FcsArrayWithCell> delta_fcs;
    std::vector<FcsClassExtent> delta_fcs2;
    FcsClassExtent fc_extent_tmp;

    std::complex<double> **mat_tmp;
    allocate(mat_tmp, ns, ns);

    MatrixXcd Dymat(ns, ns);
    MatrixXcd evec_tmp(ns, ns);
#pragma omp for
    for(ixyz = 0; ixyz < 81; ixyz++){
        itmp = ixyz;
        ixyz22 = itmp % 3;
        itmp /= 3;
        ixyz21 = itmp % 3;
        itmp /= 3;
        ixyz12 = itmp % 3;
        ixyz11 = itmp / 3;

        // calculate renormalization in real space
        compute_del_v_strain_in_real_space2(fcs_phonon->force_constant_with_cell[2],
                                                        delta_fcs, ixyz11, ixyz12, ixyz21, ixyz22, 1);


        // change format of harmonic IFCs.
        delta_fcs2.clear();

        for (const auto &it: delta_fcs){
            fc_extent_tmp = from_FcsArrayWithCell_to_FcsClassExtent(it);
            delta_fcs2.emplace_back(fc_extent_tmp);
        }
        
        for(ik = 0; ik < nk; ik++){
            dynamical->calc_analytic_k(kmesh_dense->xk[ik],
                                    delta_fcs2,
                                    mat_tmp);
            // Unitary transformation
            // knum = kmap_interpolate_to_scph[ik];
            knum = ik;
            for(is1 = 0; is1 < ns; is1++){
                for(is2 = 0; is2 < ns; is2++){
                    Dymat(is1, is2) = mat_tmp[is1][is2];
                    evec_tmp(is1, is2) = evec_harmonic[knum][is2][is1]; // transpose
                }
            }
            Dymat = evec_tmp.adjoint() * Dymat * evec_tmp;

            // copy result to del_v2_strain_from_cubic
            for(is1 = 0; is1 < ns; is1++){
                for(is2 = 0; is2 < ns; is2++){
                    del_v2_strain_from_quartic[ixyz][ik][is1*ns+is2] = Dymat(is1, is2);
                }
            }
        }
    }

    deallocate(mat_tmp);
}

}

void Scph::compute_del_v3_strain_from_quartic(std::complex<double> ****del_v3_strain_from_quartic,
                                            std::complex<double> ***evec_harmonic)
{
    using namespace Eigen;

    const auto ns = dynamical->neval;
    const auto nk = kmesh_dense->nk;
    const auto nk_interpolate = kmesh_coarse->nk;

    int ngroup_tmp;
    double *invmass_v3_tmp;
    int **evec_index_v3_tmp;
    std::vector<double> *fcs_group_tmp;
    std::vector<RelativeVector> *relvec_tmp;
    std::complex<double> *phi3_reciprocal_tmp;

    int i;
    int ixyz1, ixyz2, itmp;
    int is1, is2, ik, knum;

    double *invsqrt_mass_p;
    
    allocate(invsqrt_mass_p, system->natmin);

    for (i = 0; i < system->natmin; ++i) {
        invsqrt_mass_p[i] = std::sqrt(1.0 / system->mass[system->map_p2s[i][0]]);
    }


    // calculate renormalization in real space

    std::vector<FcsArrayWithCell> delta_fcs;

    for(ixyz1 = 0; ixyz1 < 3; ixyz1++){
        for(ixyz2 = 0; ixyz2 < 3; ixyz2++){

            // calculate renormalization in real space
            compute_del_v_strain_in_real_space1(fcs_phonon->force_constant_with_cell[2],
                                                           delta_fcs, ixyz1, ixyz2, 1);
            
            // prepare for the Fourier-transformation
            std::sort(delta_fcs.begin(), delta_fcs.end());

            anharmonic_core->prepare_group_of_force_constants(delta_fcs, 3, 
                                                              ngroup_tmp, fcs_group_tmp);

            allocate(invmass_v3_tmp, ngroup_tmp);
            allocate(evec_index_v3_tmp, ngroup_tmp, 3);
            allocate(relvec_tmp, ngroup_tmp);
            allocate(phi3_reciprocal_tmp, ngroup_tmp);

            anharmonic_core->prepare_relative_vector(delta_fcs,
                                                3,
                                                ngroup_tmp,
                                                fcs_group_tmp,
                                                relvec_tmp);
            
            int k = 0;
            for (i = 0; i < ngroup_tmp; ++i) {
                for (int j = 0; j < 3; ++j) {
                    evec_index_v3_tmp[i][j] = delta_fcs[k].pairs[j].index;
                }
                invmass_v3_tmp[i]
                        = invsqrt_mass_p[evec_index_v3_tmp[i][0] / 3]
                        * invsqrt_mass_p[evec_index_v3_tmp[i][1] / 3]
                        * invsqrt_mass_p[evec_index_v3_tmp[i][2] / 3];
                k += fcs_group_tmp[i].size();
            }
            
            compute_V3_elements_for_given_IFCs(del_v3_strain_from_quartic[ixyz1*3+ixyz2],
                                            ngroup_tmp,
                                            fcs_group_tmp,
                                            relvec_tmp,
                                            invmass_v3_tmp,
                                            evec_index_v3_tmp,
                                            evec_harmonic,
                                            selfenergy_offdiagonal);

            deallocate(fcs_group_tmp);
            deallocate(invmass_v3_tmp);
            deallocate(evec_index_v3_tmp);
            deallocate(relvec_tmp);
            deallocate(phi3_reciprocal_tmp);

        }
    }

    

    deallocate(invsqrt_mass_p);
}



// for cubic structure (BTO)
// not for arbitrary crystal structures.
void Scph::calculate_del_v2_strain_from_cubic_by_finite_difference(std::complex<double> ***evec_harmonic,
                                                                    std::complex<double> ***del_v2_strain_from_cubic)
{   
    using namespace Eigen;

    int natmin = system->natmin;
    int nat = system->nat;
    int ntran = system->ntran;
    int nk_interpolate = kmesh_coarse->nk;
    int nk = kmesh_dense->nk;
    int ns = dynamical->neval;

    double dzz, dxy;
    std::string filename_zz_plus, filename_zz_minus, filename_xy;

    int **symm_mapping_s;
    int **inv_translation_mapping;

    std::vector<FcsClassExtent> fc2_zz_plus;
    std::vector<FcsClassExtent> fc2_zz_minus;
    std::vector<FcsClassExtent> fc2_xy;
    std::vector<FcsClassExtent> fc2_tmp;
    FcsClassExtent fce_tmp;

    int ixyz1, ixyz2, ixyz3, ixyz4;
    int ixyz1_2, ixyz2_2, ixyz3_2, ixyz4_2;
    int i1, i2;
    int iat1, iat2, iat1_2, iat2_2, iat2_2_prim;
    int itran1, itran2, itran3;
    int ik;
    int is1, is2, is, js;
    int isymm;

    std::complex<double> ***dymat_q, **dymat_tmp;
    std::complex<double> ***dymat_new;

    const auto nk1 = kmesh_interpolate[0];
    const auto nk2 = kmesh_interpolate[1];
    const auto nk3 = kmesh_interpolate[2];
    
    MatrixXcd dymat_tmp_mode(ns, ns);
    MatrixXcd dymat_tmp_alphamu(ns, ns);
    MatrixXcd evec_tmp(ns, ns);

    // read input 
    std::fstream fin_strain_mode_coupling;
    std::fstream fin_strain_mode_coupling_symmetrized;

    double ****B_array_real_space_in;
    int **exist_in;
    double ****B_array_real_space_symmetrized;
    int ****count_tmp;
    int mapping_xyz[3];

    allocate(exist_in, 3, 3);
    allocate(B_array_real_space_in, 3, 3, natmin*3, nat*3);
    allocate(B_array_real_space_symmetrized, 3, 3, natmin*3, nat*3);
    allocate(count_tmp, 3, 3, natmin*3, nat*3);

    for(ixyz1 = 0; ixyz1 < 3; ixyz1++){
        for(ixyz2 = 0; ixyz2 < 3; ixyz2++){
            exist_in[ixyz1][ixyz2] = 0;
            for(i1 = 0; i1 < natmin*3; i1++){
                for(i2 = 0; i2 < nat*3; i2++){
                    B_array_real_space_in[ixyz1][ixyz2][i1][i2] = 0.0;
                    B_array_real_space_symmetrized[ixyz1][ixyz2][i1][i2] = 0.0;
                    count_tmp[ixyz1][ixyz2][i1][i2] = 0;
                }
            }
        }
    }

    fin_strain_mode_coupling_symmetrized.open("B_array.txt");

    if(fin_strain_mode_coupling_symmetrized){
        // read symmetrized B array from input
        for(ixyz1 = 0; ixyz1 < 3; ixyz1++){
            for(ixyz2 = 0; ixyz2 < 3; ixyz2++){
                for(i1 = 0; i1 < natmin*3; i1++){
                    for(i2 = 0; i2 < nat*3; i2++){
                        fin_strain_mode_coupling_symmetrized >> B_array_real_space_symmetrized[ixyz1][ixyz2][i1][i2];
                    }
                }
            }
        }

        fin_strain_mode_coupling_symmetrized.close();
    }
    else{
        // calculate B array by finite difference method and symmetrize.
        std::cout << "calculate B array by finite difference method and symmetrize." << std::endl;

        fin_strain_mode_coupling.open("strain_mode.in");

        if(!fin_strain_mode_coupling){
            std::cout << "Warning in Scph::calculate_del_v2_strain_from_cubic_by_finite_difference: file strain_mode.in could not open." << std::endl;
            std::cout << "all q0 is set 0." << std::endl;
            exit("calculate_del_v2_strain_from_cubic_by_finite_difference",
                        "strain_mode.in not found");
        }

        fin_strain_mode_coupling >> dzz >> dxy;
        fin_strain_mode_coupling >> filename_zz_plus;
        fin_strain_mode_coupling >> filename_zz_minus;
        fin_strain_mode_coupling >> filename_xy;

        // debug
        std::cout << "dzz = " << dzz << ", dxy = " << dxy << std::endl;
        std::cout << "filename_zz_plus: " << filename_zz_plus << std::endl;
        std::cout << "filename_zz_minus: " << filename_zz_minus << std::endl;
        std::cout << "filename_xy: " << filename_xy << std::endl;

        fcs_phonon->load_fc2_xml_tmp(filename_zz_plus, fc2_zz_plus);
        fcs_phonon->load_fc2_xml_tmp(filename_zz_minus, fc2_zz_minus);
        fcs_phonon->load_fc2_xml_tmp(filename_xy, fc2_xy);


        // calculate input B_array in real space
        exist_in[2][2] = 1; // zz component

        for (const auto &it: fc2_zz_plus){
            B_array_real_space_in[2][2][it.atm1*3 + it.xyz1][it.atm2*3+it.xyz2] += it.fcs_val;
        }
        for (const auto &it: fc2_zz_minus){
            B_array_real_space_in[2][2][it.atm1*3 + it.xyz1][it.atm2*3+it.xyz2] -= it.fcs_val;
        }
        
        for(i1 = 0; i1 < natmin*3; i1++){
            for(i2 = 0; i2 < nat*3; i2++){
                B_array_real_space_in[2][2][i1][i2] /= (2.0*dzz);
            }
        }
    
        exist_in[0][1] = exist_in[1][0] = 1;
        for (const auto &it: fc2_zz_plus){
            B_array_real_space_in[1][0][it.atm1*3 + it.xyz1][it.atm2*3+it.xyz2] += it.fcs_val;
            B_array_real_space_in[0][1][it.atm1*3 + it.xyz1][it.atm2*3+it.xyz2] += it.fcs_val;
        }
        for (const auto &it: fcs_phonon->fc2_ext){
            B_array_real_space_in[1][0][it.atm1*3 + it.xyz1][it.atm2*3+it.xyz2] -= it.fcs_val;
            B_array_real_space_in[0][1][it.atm1*3 + it.xyz1][it.atm2*3+it.xyz2] -= it.fcs_val;
        }
        
        for(i1 = 0; i1 < natmin*3; i1++){
            for(i2 = 0; i2 < nat*3; i2++){
                B_array_real_space_in[0][1][i1][i2] /= (2.0*dxy);
                B_array_real_space_in[1][0][i1][i2] /= (2.0*dxy);
            }
        }
    

        // make mapping information
        allocate(symm_mapping_s, symmetry->SymmListWithMap.size(), nat);
        make_supercell_mapping_by_symmetry_operations(symm_mapping_s);

        allocate(inv_translation_mapping, ntran, ntran);
        make_inverse_translation_mapping(inv_translation_mapping); 

        // debug: no symmetrization
        // for(ixyz1 = 0; ixyz1 < 3; ixyz1++){
        //     for(ixyz2 = 0; ixyz2 < 3; ixyz2++){
        //         for(i1 = 0; i1 < natmin*3; i1++){
        //             for(i2 = 0; i2 < nat*3; i2++){
        //                 B_array_real_space_symmetrized[ixyz1][ixyz2][i1][i2] = B_array_real_space_in[ixyz1][ixyz2][i1][i2];
        //             }
        //         }
        //     }
        // }

        std::cout << "number of symmetry operations : " << symmetry->SymmListWithMap.size() << std::endl;
        // symmetrize and replicate
        for(isymm = 0; isymm < symmetry->SymmListWithMap.size(); isymm++){

            // debug
            // std::cout << "isymm = " << isymm << std::endl;
            // std::cout << "rotation matrix" << std::endl;
            // for(ixyz1 = 0; ixyz1 < 3; ixyz1++){
            //     for(ixyz2 = 0; ixyz2 < 3; ixyz2++){
            //         std::cout << symmetry->SymmListWithMap[isymm].rot[ixyz1*3+ixyz2] << " ";
            //     }std::cout << std::endl;
            // }

            // make mapping of xyz by the rotation matrix
            for(ixyz1 = 0; ixyz1 < 3; ixyz1++){
                for(ixyz2 = 0; ixyz2 < 3; ixyz2++){
                    if(std::fabs(std::fabs(symmetry->SymmListWithMap[isymm].rot[ixyz1*3+ixyz2])- 1.0) < eps6){
                        mapping_xyz[ixyz2] = ixyz1;
                    }
                }
            }

            // debug
            // std::cout << "mapping_xyz" << std::endl;
            // for(ixyz1 = 0; ixyz1 < 3; ixyz1++){
            //     std::cout << mapping_xyz[ixyz1] << " ";
            // }std::cout << std::endl;

            for(ixyz1 = 0; ixyz1 < 3; ixyz1++){
                for(ixyz2 = 0; ixyz2 < 3; ixyz2++){
                    // skip if B_array[ixyz1][ixyz2] is not included in the input
                    if(exist_in[ixyz1][ixyz2] == 0){
                        continue;
                    }

                    for(iat1 = 0; iat1 < natmin; iat1++){
                        // skip if the center atom is not included in the original primitive cell
                        // if(symm_mapping_s[isymm][system->map_p2s[iat1][0]] != 
                        //       system->map_p2s[symmetry->SymmListWithMap[isymm].mapping[iat1]][0]){
                        //     std::cout << "center atom is not included in the original primitive cell" << std::endl;
                        //     continue;
                        // }
                        
                        iat1_2 = symmetry->SymmListWithMap[isymm].mapping[iat1];
                        // std::cout << "iat1_2: " << iat1_2 << std::endl;
                        ixyz1_2 = mapping_xyz[ixyz1];
                        ixyz2_2 = mapping_xyz[ixyz2];

                        for(i1 = 0; i1 < ntran; i1++){
                            if(system->map_p2s[iat1_2][i1] == symm_mapping_s[isymm][system->map_p2s[iat1][0]]){
                                itran1 = i1;
                            }
                        }

                        for(iat2 = 0; iat2 < nat; iat2++){
                            iat2_2 = symm_mapping_s[isymm][iat2]; // temporary
                            iat2_2_prim = system->map_s2p[iat2_2].atom_num;
                            itran2 = system->map_s2p[iat2_2].tran_num;
                            
                            iat2_2 = system->map_p2s[iat2_2_prim][inv_translation_mapping[itran1][itran2]];

                            for(ixyz3 = 0; ixyz3 < 3; ixyz3++){
                                for(ixyz4 = 0; ixyz4 < 3; ixyz4++){
                                    ixyz3_2 = mapping_xyz[ixyz3];
                                    ixyz4_2 = mapping_xyz[ixyz4];

                                    B_array_real_space_symmetrized[ixyz1_2][ixyz2_2][iat1_2*3+ixyz3_2][iat2_2*3+ixyz4_2]
                                        += B_array_real_space_in[ixyz1][ixyz2][iat1*3+ixyz3][iat2*3+ixyz4]
                                            * symmetry->SymmListWithMap[isymm].rot[ixyz1_2*3+ixyz1]
                                            * symmetry->SymmListWithMap[isymm].rot[ixyz2_2*3+ixyz2]
                                            * symmetry->SymmListWithMap[isymm].rot[ixyz3_2*3+ixyz3]
                                            * symmetry->SymmListWithMap[isymm].rot[ixyz4_2*3+ixyz4];
                                    // std::cout << "before: " << count_tmp[ixyz1_2][ixyz2_2][iat1_2*3+ixyz3_2][iat2_2*3+ixyz4_2];
                                    count_tmp[ixyz1_2][ixyz2_2][iat1_2*3+ixyz3_2][iat2_2*3+ixyz4_2]++;
                                    // std::cout << " after: " << count_tmp[ixyz1_2][ixyz2_2][iat1_2*3+ixyz3_2][iat2_2*3+ixyz4_2] << std::endl;
                                }
                            }
                        }
                    }
                }
            }
        }

        std::cout << "count_tmp " << std::endl;

        for(ixyz1 = 0; ixyz1 < 3; ixyz1++){
            for(ixyz2 = 0; ixyz2 < 3; ixyz2++){
                for(i1 = 0; i1 < natmin*3; i1++){
                    for(i2 = 0; i2 < nat*3; i2++){
                        if(count_tmp[ixyz1][ixyz2][i1][i2] == 0){
                            std::cout << "Warning: B_array[" << ixyz1 << "][" << ixyz2 << "][" << i1 << "][" << i2 << "] is not given" << std::endl;
                            std::cout << "The corresponding component is set zero." << std::endl;
                            B_array_real_space_symmetrized[ixyz1][ixyz2][i1][i2] = 0.0;
                        }
                        else{
                            B_array_real_space_symmetrized[ixyz1][ixyz2][i1][i2] /= count_tmp[ixyz1][ixyz2][i1][i2];
                        }
                        // debug
                        std::cout << ixyz1 << " " << ixyz2 << " " << i1 << " " << i2 << " " << count_tmp[ixyz1][ixyz2][i1][i2] << std::endl;
                    }
                }
            }
        }

        deallocate(symm_mapping_s);
        deallocate(inv_translation_mapping); 
        
        // write B_array_real_space_symmetrized to an output file
        std::ofstream fout_B_array;
        fout_B_array.open("B_array.txt");

        for(ixyz1 = 0; ixyz1 < 3; ixyz1++){
            for(ixyz2 = 0; ixyz2 < 3; ixyz2++){
                for(i1 = 0; i1 < natmin*3; i1++){
                    for(i2 = 0; i2 < nat*3; i2++){
                        fout_B_array << B_array_real_space_symmetrized[ixyz1][ixyz2][i1][i2] << " ";
                    }fout_B_array << std::endl;
                }
            }
        }

        fout_B_array.close();
    }

    // std::cout << "symmetrize and replicate B_array is done." << std::endl;

    // Fourier transform and interpolate
    allocate(dymat_q, ns, ns, nk_interpolate);
    allocate(dymat_new, ns, ns, nk_interpolate);
    allocate(dymat_tmp, ns, ns);

    // std::cout << "start interpolation." << std::endl;

    for(ixyz1 = 0; ixyz1 < 3; ixyz1++){
        for(ixyz2 = 0; ixyz2 < 3; ixyz2++){

            // std::cout << "ixyz1 = " << ixyz1 << ", ixyz2 = " << ixyz2 << std::endl;

            // put B_array to std::vector<FcsClassExtent> format
            fc2_tmp.clear();
            for(i1 = 0; i1 < natmin*3; i1++){
                for(i2 = 0; i2 < nat*3; i2++){
                    fce_tmp.atm1 = i1/3;
                    fce_tmp.atm2 = i2/3;
                    fce_tmp.xyz1 = i1%3;
                    fce_tmp.xyz2 = i2%3;
                    fce_tmp.cell_s = 0;
                    fce_tmp.fcs_val = B_array_real_space_symmetrized[ixyz1][ixyz2][i1][i2];
                    fc2_tmp.push_back(fce_tmp);
                }
            }

            // std::cout << "fc2_tmp is prepared." << std::endl;

            // Fourier transform
            for(ik = 0; ik < nk_interpolate; ik++){
                
                dynamical->calc_analytic_k(kmesh_coarse->xk[ik],
                                    fc2_tmp,
                                    dymat_tmp);

                for(is1 = 0; is1 < ns; is1++){
                    for(is2 = 0; is2 < ns; is2++){
                        dymat_q[is1][is2][ik] = dymat_tmp[is1][is2];
                    }
                }
            }
            // std::cout << "Fourier transformation to k space is done." << std::endl;

            // interpolation
            for (is1 = 0; is1 < ns; is1++) {
                for (is2 = 0; is2 < ns; is2++) {
                    fftw_plan plan = fftw_plan_dft_3d(nk1, nk2, nk3,
                                                    reinterpret_cast<fftw_complex *>(dymat_q[is1][is2]),
                                                    reinterpret_cast<fftw_complex *>(dymat_new[is1][is2]),
                                                    FFTW_FORWARD, FFTW_ESTIMATE);
                    fftw_execute(plan);
                    fftw_destroy_plan(plan);

                    for (ik = 0; ik < nk_interpolate; ++ik){
                        dymat_new[is1][is2][ik] /= static_cast<double>(nk_interpolate);
                    }
                }
            }

            for(ik = 0; ik < nk; ik++){
                r2q(kmesh_dense->xk[ik], nk1, nk2, nk3, ns, dymat_new, dymat_tmp);

                // std::cout << "r2q is done." << std::endl;

                // transform to mode-representation
                for(is = 0; is < ns; is++){
                    for(js = 0; js < ns; js++){
                        evec_tmp(is, js) = evec_harmonic[ik][js][is]; // transpose
                        dymat_tmp_alphamu(is, js) = dymat_tmp[is][js];
                    }
                }
                dymat_tmp_mode = evec_tmp.adjoint() * dymat_tmp_alphamu * evec_tmp;

                // std::cout << "substitute to del_v2_strain_from_cubic." << std::endl;

                for(is = 0; is < ns; is++){
                    for(js = 0; js < ns; js++){
                        del_v2_strain_from_cubic[ixyz1*3+ixyz2][ik][is*ns+js] += dymat_tmp_mode(is, js);
                    }
                }
            }

        }
    }

    deallocate(exist_in);
    deallocate(B_array_real_space_in);
    deallocate(B_array_real_space_symmetrized);
    
    deallocate(count_tmp);
    deallocate(dymat_q);
    deallocate(dymat_tmp);
    deallocate(dymat_new);

}

void Scph::make_supercell_mapping_by_symmetry_operations(int **symm_mapping_s)
{
    int natmin = system->natmin;
    int nat = system->nat;
    int ntran = system->ntran;

    double rotmat[3][3];
    double shift[3];
    double **xtmp;
    double xr_tmp[3];
    int i, j;
    int iat1, jat1, itran1, iat_prim;
    int isymm;
    int atm_found, iflag;
    double dtmp, dtmp2;

    // allocate(symm_mapping_s, symmetry->SymmListWithMap.size(), nat);

    // atomici positions in cartesian coordinate
    allocate(xtmp, nat, 3);
    for (auto i = 0; i < nat; ++i) {
        rotvec(xtmp[i], system->xr_s[i], system->lavec_s);
    }

    // make mapping
    isymm = -1;
    for (const auto &it: symmetry->SymmListWithMap) {

        isymm++;

        std::cout << "isymm = " << isymm << std::endl;

        for (i = 0; i < 3; ++i) {
            for (j = 0; j < 3; ++j) {
                rotmat[i][j] = it.rot[3*i+j];
            }
        }
        for (i = 0; i < 3; ++i) {
            shift[i] = it.shift[i];
        }
        rotvec(shift, shift, system->lavec_p);

        for(iat1 = 0; iat1 < nat; iat1++){
            std::cout << "iat1: " << iat1 << std::endl;
            // operate symmetry operation in cartesian coordinate
            rotvec(xr_tmp, xtmp[iat1], rotmat);
            for(i = 0; i < 3; i++){
                xr_tmp[i] += shift[i];
            }
            // transform to fractional coordinate of supercell
            rotvec(xr_tmp, xr_tmp, system->rlavec_s);
            for(i = 0; i < 3; i++){
                xr_tmp[i] /= 2.0 * pi;
            }
            std::cout << "after rotation ";
            for(i = 0; i < 3; i++){
                std::cout << xr_tmp[i] << " ";
            }std::cout << std::endl;
            for(i = 0; i < 3; i++){
                xr_tmp[i] = std::fmod(xr_tmp[i]+1.0, 1.0);
            }
            std::cout << "moved in the supercell ";
            for(i = 0; i < 3; i++){
                std::cout << xr_tmp[i] << " ";
            }std::cout << std::endl;

            // search for corresponding atom in the supercell
            atm_found = 0;
            for(itran1 = 0; itran1 < ntran; itran1++){
                jat1 = system->map_p2s[it.mapping[system->map_s2p[iat1].atom_num]][itran1];
                iflag = 1;
                for(i = 0; i < 3; i++){
                    // This is for robustness when numerical error is present.
                    dtmp = std::min(std::fabs(system->xr_s[jat1][i] - xr_tmp[i]),
                                    std::min(std::fabs(system->xr_s[jat1][i] - xr_tmp[i] + 1.0),
                                            std::fabs(system->xr_s[jat1][i] - xr_tmp[i] - 1.0) )
                                    );
                    if(dtmp > eps6){
                        iflag = 0;
                    }
                }
                if(iflag == 1){
                    atm_found = 1;
                    symm_mapping_s[isymm][iat1] = jat1;
                    break;
                }

            }
            if(atm_found == 0){
                std::cout << "Error: corresponding atom is not found." << std::endl;
            }
        }
    }

    deallocate(xtmp);

    // check one-to-one correspondence
    int *map_tmp;
    allocate(map_tmp, nat);

    std::cout << "check one-to-one correspondence." << std::endl;

    for(isymm = 0; isymm < symmetry->SymmListWithMap.size(); isymm++){
        // initialize
        for(iat1 = 0; iat1 < nat; iat1++){
            map_tmp[iat1] = 0;
        }
        // check
        for(iat1 = 0; iat1 < nat; iat1++){
            map_tmp[symm_mapping_s[isymm][iat1]] = 1;
        }
        for(iat1 = 0; iat1 < nat; iat1++){
            if(map_tmp[iat1] == 0){
                std::cout << "no atom is moved to atom " << iat1 << " by symmetry operation " << isymm << ". " << std::endl;
            }
        }
    }


    deallocate(map_tmp);


}

void Scph::make_inverse_translation_mapping(int **inv_translation_mapping)
{
    int ntran = system->ntran;

    int i1, i2, i3;
    int ixyz1, ixyz2;
    double x_tran1[3], x_tran2[3];
    
    int is_found, itmp;
    double dtmp;

    // allocate(inv_translation_mapping, ntran, ntran);

    for(i1 = 0; i1 < ntran; i1++){
        // bring i1-th primitive cell to the originalprimitive cell
        for(ixyz1 = 0; ixyz1 < 3; ixyz1++){
            x_tran1[ixyz1] = system->xr_s[system->map_p2s[0][i1]][ixyz1] - system->xr_s[system->map_p2s[0][0]][ixyz1];
            x_tran1[ixyz1] = std::fmod(x_tran1[ixyz1]+1.0, 1.0);
        }

        // check i2-th primitive cell is moved to i3-th primitive cell
        for(i2 = 0; i2 < ntran; i2++){
            is_found = 0;
            for(i3 = 0; i3 < ntran; i3++){
                // calculate translation vector
                for(ixyz1 = 0; ixyz1 < 3; ixyz1++){
                    x_tran2[ixyz1] = system->xr_s[system->map_p2s[0][i2]][ixyz1] - system->xr_s[system->map_p2s[0][i3]][ixyz1];
                    x_tran2[ixyz1] = std::fmod(x_tran2[ixyz1]+1.0, 1.0);
                }

                // compare translation vector
                itmp = 1;
                for(ixyz1 = 0; ixyz1 < 3; ixyz1++){
                    dtmp = std::min(std::fabs(x_tran1[ixyz1] - x_tran2[ixyz1]), std::fabs(x_tran1[ixyz1] - x_tran2[ixyz1] + 1.0));
                    dtmp = std::min(dtmp, std::fabs(x_tran1[ixyz1] - x_tran2[ixyz1] - 1.0));
                    std::cout << i1 << " " << i2 << " " << i3 << " " << ixyz1 << " " << dtmp << std::endl;
                    if(dtmp > eps6){
                        itmp = 0;
                        break;
                    }
                }
                if(itmp == 1){
                    inv_translation_mapping[i1][i2] = i3;
                    is_found = 1;
                    break;
                }
            }
            if(is_found == 0){
                std::cout << "Error: corresponding primitive cell is not found in make_inverse_translation_mapping." << std::endl;
            }
        }        
    }

    // debug
    std::cout << "position of the primitive cell" << std::endl;
    for(i1 = 0; i1 < ntran; i1++){
        for(ixyz1 = 0; ixyz1 < 3; ixyz1++){
            std::cout << system->xr_s[system->map_p2s[0][i1]][ixyz1] << " ";
        }std::cout << std::endl;
    }std::cout << std::endl;

    std::cout << "inv_translation_mapping" << std::endl;
    for(i1 = 0; i1 < ntran; i1++){
        for(i2 = 0; i2 < ntran; i2++){
            std::cout << inv_translation_mapping[i1][i2] << " ";
        }std::cout << std::endl;
    }std::cout << std::endl;

}

void Scph::add_strain_mode_coupling_to_del_v2_strain(std::complex<double> ***del_v2_strain_from_cubic,
                                                    std::complex<double> ***evec_harmonic,
                                                    std::vector<std::vector<double>> &B_array)
{

    using namespace Eigen;

    std::cout << "start add_strain_mode_coupling_to_del_v2_strain" << std::endl;

    int ik, jk;
    int i1;
    int is, js;
    const auto nk = kmesh_dense->nk;
    const auto nk_interpolate = kmesh_coarse->nk;
    const auto ns = dynamical->neval;
    unsigned int knum, knum_interpolate;
    const auto nk_irred_interpolate = kmesh_coarse->nk_irred;
    const auto nk1 = kmesh_interpolate[0];
    const auto nk2 = kmesh_interpolate[1];
    const auto nk3 = kmesh_interpolate[2];

    std::complex<double> ***dymat_q;
    std::complex<double> ***dymat_new;
    std::complex<double> **mat_tmp;

    allocate(dymat_q, ns, ns, nk_interpolate);
    allocate(dymat_new, ns, ns, nk_interpolate);
    allocate(mat_tmp, ns, ns);

    MatrixXcd dymat_tmp_mode(ns, ns);
    MatrixXcd dymat_tmp_alphamu(ns, ns);
    MatrixXcd evec_tmp(ns, ns);

    // debug variable
    std::complex<double> ***del_v2_strain_from_cubic_correction;
    allocate(del_v2_strain_from_cubic_correction, 9, nk, ns*ns);
    for(i1 = 0; i1 < 9; i1++){
        for(ik = 0; ik < nk; ik++){
            for(is = 0; is < ns*ns; is++){
                del_v2_strain_from_cubic_correction[i1][ik][is] = 0.0;
            }
        }
    }

    std::cout << "start calculating correction" << std::endl;

    for(i1 = 0; i1 < 9; i1++){
        for(is = 0; is < ns; is++){
            for(js = 0; js < ns; js++){
                for(ik = 0; ik < nk_interpolate; ik++){
                    dymat_q[is][js][ik] = 0.0;
                }
            }
        }

        std::cout << "initialize dymat_q is done." << std::endl;
        // transform B array from mode representation to (alpha, mu) representation
        for(is = 0; is < ns; is++){
            for(js = 0; js < ns; js++){
                evec_tmp(is, js) = evec_harmonic[0][js][is]; // transpose
                dymat_tmp_mode(is, js) = B_array[i1][is*ns+js];
            }
        }
        dymat_tmp_alphamu = evec_tmp * dymat_tmp_mode * evec_tmp.adjoint();

        std::cout << "transformation of B array is done." << std::endl;
        
        // add correction on Gamma point
        for(is = 0; is < ns; is++){
            for(js = 0; js < ns; js++){
                dymat_q[is][js][0] = dymat_tmp_alphamu(is, js);
            }
        }

        std::cout << "calculation of dymat_q is done." << std::endl;

        // Fourier transform to real space
        for (is = 0; is < ns; ++is) {
            for (js = 0; js < ns; ++js) {
                fftw_plan plan = fftw_plan_dft_3d(nk1, nk2, nk3,
                                                    reinterpret_cast<fftw_complex *>(dymat_q[is][js]),
                                                    reinterpret_cast<fftw_complex *>(dymat_new[is][js]),
                                                    FFTW_FORWARD, FFTW_ESTIMATE);
                fftw_execute(plan);
                fftw_destroy_plan(plan);

                for (ik = 0; ik < nk_interpolate; ++ik)
                    dymat_new[is][js][ik] /= static_cast<double>(nk_interpolate);
            }
        }

        std::cout << "Fourier transformation is done\nstart interpolation" << std::endl;

        // interpolation     
        for(ik = 0; ik < nk; ik++){
            r2q(kmesh_dense->xk[ik], nk1, nk2, nk3, ns, dymat_new, mat_tmp);

            std::cout << "r2q is done." << std::endl;

            // transform to mode-representation
            for(is = 0; is < ns; is++){
                for(js = 0; js < ns; js++){
                    evec_tmp(is, js) = evec_harmonic[ik][js][is]; // transpose
                    dymat_tmp_alphamu(is, js) = mat_tmp[is][js];
                }
            }
            dymat_tmp_mode = evec_tmp.adjoint() * dymat_tmp_alphamu * evec_tmp;

            std::cout << "transformation is done.\nadd correction." << std::endl;

            // add correction 
            for(is = 0; is < ns; is++){
                for(js = 0; js < ns; js++){
                    del_v2_strain_from_cubic[i1][ik][is*ns+js] += dymat_tmp_mode(is, js);
                    del_v2_strain_from_cubic_correction[i1][ik][is*ns+js] = dymat_tmp_mode(is, js);
                }
            }
            
        }

        // debug
        for(ik = 0; ik < nk_interpolate; ik++){
            knum = kmap_interpolate_to_scph[ik];
            std::cout << "ik_interpolate = " << ik << ", i1 = " << i1 << std::endl;
            for(is = 0; is < ns; is++){
                for(js = 0; js < ns; js++){
                    std::cout << del_v2_strain_from_cubic_correction[i1][knum][is*ns+js];
                }std::cout << std::endl;
            }std::cout << std::endl;
        }std::cout << std::endl;

    }

    deallocate(dymat_q);
    deallocate(dymat_new);
    // debug variable
    deallocate(del_v2_strain_from_cubic_correction);

}

void Scph::compute_del_v_strain_in_real_space1(const std::vector<FcsArrayWithCell> &fcs_in,
                                               std::vector<FcsArrayWithCell> &delta_fcs,
                                               const int ixyz1,
                                               const int ixyz2,
                                               const int mirror_image_mode)
{
    unsigned int i, j;
    double vec[3], vec_origin[3];
    double fcs_tmp = 0.0;

    std::vector<FcsAlignedForGruneisen> fcs_aligned;
    std::vector<AtomCellSuper> pairs_vec;
    std::vector<int> index_old, index_now;
    std::vector<int> index_with_cell, index_with_cell_old;
    std::set<std::vector<int>> set_index_uniq;
    AtomCellSuper pairs_tmp;

    const auto norder = fcs_in[0].pairs.size();
    unsigned int nmulti;

    delta_fcs.clear();
    fcs_aligned.clear();

    for (const auto &it: fcs_in) {
        fcs_aligned.emplace_back(it.fcs_val, it.pairs);
    }
    std::sort(fcs_aligned.begin(), fcs_aligned.end());

    // std::cout << "fcs_aligned has is sorted" << std::endl;
    // std::cout << "norder = " << norder << std::endl;

    // prepare supercell shift
    double **xshift_s;
    const auto ncell_s = 27;

    allocate(xshift_s, ncell_s, 3);

    unsigned int icell = 0;
    int ix, iy, iz;
    for (i = 0; i < 3; ++i) xshift_s[0][i] = 0;
    icell = 1;
    for (ix = -1; ix <= 1; ++ix) {
        for (iy = -1; iy <= 1; ++iy) {
            for (iz = -1; iz <= 1; ++iz) {
                if (ix == 0 && iy == 0 && iz == 0) continue;

                xshift_s[icell][0] = ix*1.0;
                xshift_s[icell][1] = iy*1.0;
                xshift_s[icell][2] = iz*1.0;

                ++icell;
            }
        }
    }

    if(mirror_image_mode == 0){
        // original implementation
        // calculate IFC renormalization for the same atomic combination in the supercell
        // but with different mirror images at once and assign the average value for each
        // mirror images.
        index_old.clear();
        const auto nelems = 2 * (norder - 2) + 1;
        for (i = 0; i < nelems; ++i) index_old.push_back(-1);

        index_with_cell.clear();
        set_index_uniq.clear();

        for (const auto &it: fcs_aligned) {

            // if the xyz does not match the considering coponent
            if(it.pairs[norder - 1].index % 3 != ixyz1){
                continue;
            }

            index_now.clear();
            index_with_cell.clear();

            index_now.push_back(it.pairs[0].index);
            index_with_cell.push_back(it.pairs[0].index);

            for (i = 1; i < norder - 1; ++i) {
                index_now.push_back(it.pairs[i].index);
                index_now.push_back(it.pairs[i].tran);

                index_with_cell.push_back(it.pairs[i].index);
                index_with_cell.push_back(it.pairs[i].tran);
                index_with_cell.push_back(it.pairs[i].cell_s);
            }

            if (index_now != index_old) {

                if (index_old[0] != -1) {

                    nmulti = set_index_uniq.size();
                    fcs_tmp /= static_cast<double>(nmulti);

                    if (std::abs(fcs_tmp) > eps15) {
                        for (const auto &it2: set_index_uniq) {

                            pairs_vec.clear();

                            pairs_tmp.index = it2[0];
                            pairs_tmp.tran = 0;
                            pairs_tmp.cell_s = 0;
                            pairs_vec.push_back(pairs_tmp);
                            for (i = 1; i < norder - 1; ++i) {
                                pairs_tmp.index = it2[3 * i - 2];
                                pairs_tmp.tran = it2[3 * i - 1];
                                pairs_tmp.cell_s = it2[3 * i];
                                pairs_vec.push_back(pairs_tmp);
                            }
                            delta_fcs.emplace_back(fcs_tmp, pairs_vec);
                        }
                    }
                    set_index_uniq.clear();
                }

                fcs_tmp = 0.0;
                index_old.clear();
                index_old.reserve(index_now.size());
                std::copy(index_now.begin(), index_now.end(), std::back_inserter(index_old));
            }

            set_index_uniq.insert(index_with_cell);

            for(i = 0; i < 3; i++){
                // vec_origin[i] = system->xr_s_anharm[system->map_p2s_anharm[it.pairs[0].index / 3][0]][i];
                vec_origin[i] = system->xr_s_no_displace[system->map_p2s[it.pairs[0].index / 3][0]][i];
                for(j = 1; j < norder-1; j++){
                    // vec_origin[i] += system->xr_s_anharm[system->map_p2s_anharm[it.pairs[j].index / 3][it.pairs[j].tran]][i]
                    //                 + xshift_s[it.pairs[j].cell_s][i];
                    vec_origin[i] += system->xr_s_no_displace[system->map_p2s[it.pairs[j].index / 3][it.pairs[j].tran]][i]
                                     + xshift_s[it.pairs[j].cell_s][i];
                }
                vec_origin[i] /= (norder-1);
            }

            for (i = 0; i < 3; ++i) {
                // vec[i] = system->xr_s_anharm[system->map_p2s_anharm[it.pairs[norder - 1].index / 3][it.pairs[norder -
                //                                                                                             1].tran]][i]
                //         // - system->xr_s_anharm[system->map_p2s_anharm[it.pairs[0].index / 3][0]][i]
                //         - vec_origin[i]
                //         + xshift_s[it.pairs[norder - 1].cell_s][i];
                vec[i] = system->xr_s_no_displace[system->map_p2s[it.pairs[norder - 1].index / 3][it.pairs[norder -
                                                                                                            1].tran]][i]
                        // - system->xr_s_anharm[system->map_p2s_anharm[it.pairs[0].index / 3][0]][i]
                        - vec_origin[i]
                        + xshift_s[it.pairs[norder - 1].cell_s][i];
            }

            rotvec(vec, vec, system->lavec_s);

            fcs_tmp += it.fcs_val * vec[ixyz2];
            // it.pairs[norder - 1].index % 3 == ixyz has been checked.
        }

        nmulti = set_index_uniq.size();
        fcs_tmp /= static_cast<double>(nmulti);

        if (std::abs(fcs_tmp) > eps15) {
            for (const auto &it2: set_index_uniq) {

                pairs_vec.clear();

                pairs_tmp.index = it2[0];
                pairs_tmp.tran = 0;
                pairs_tmp.cell_s = 0;
                pairs_vec.push_back(pairs_tmp);
                for (i = 1; i < norder - 1; ++i) {
                    pairs_tmp.index = it2[3 * i - 2];
                    pairs_tmp.tran = it2[3 * i - 1];
                    pairs_tmp.cell_s = it2[3 * i];
                    pairs_vec.push_back(pairs_tmp);
                }
                delta_fcs.emplace_back(fcs_tmp, pairs_vec);
            }
        }
    }
    else{ // if(mirror_image_mode != 0)
        // new implementation
        // calculate IFC renormalization separately for each mirror image combinations.
        index_with_cell_old.clear();
        const auto nelems = 3 * (norder - 2) + 1;
        for (i = 0; i < nelems; ++i) index_with_cell_old.push_back(-1);

        index_with_cell.clear();

        for (const auto &it: fcs_aligned) {

            // std::cout << "FCS" << std::endl;
            // std::cout << "index ";
            // for(auto ittmp: it.pairs){
            //     std::cout << ittmp.index << " ";
            // }std::cout << std::endl;

            // if the xyz does not match the considering coponent
            if(it.pairs[norder - 1].index % 3 != ixyz1){
                continue;
            }

            index_with_cell.clear();

            index_with_cell.push_back(it.pairs[0].index);

            for (i = 1; i < norder - 1; ++i) {
                index_with_cell.push_back(it.pairs[i].index);
                index_with_cell.push_back(it.pairs[i].tran);
                index_with_cell.push_back(it.pairs[i].cell_s);
            }

            if (index_with_cell != index_with_cell_old) {

                if (index_with_cell_old[0] != -1) {

                    if (std::abs(fcs_tmp) > eps15) {

                        pairs_vec.clear();

                        pairs_tmp.index = index_with_cell_old[0];
                        pairs_tmp.tran = 0;
                        pairs_tmp.cell_s = 0;
                        pairs_vec.push_back(pairs_tmp);
                        for (i = 1; i < norder - 1; ++i) {
                            pairs_tmp.index = index_with_cell_old[3 * i - 2];
                            pairs_tmp.tran = index_with_cell_old[3 * i - 1];
                            pairs_tmp.cell_s = index_with_cell_old[3 * i];
                            pairs_vec.push_back(pairs_tmp);
                        }
                        delta_fcs.emplace_back(fcs_tmp, pairs_vec);
                    }
                }

                fcs_tmp = 0.0;
                index_with_cell_old.clear();
                index_with_cell_old.reserve(index_with_cell.size());
                std::copy(index_with_cell.begin(), index_with_cell.end(), std::back_inserter(index_with_cell_old));
            }

            for(i = 0; i < 3; i++){
                // vec_origin[i] = system->xr_s_anharm[system->map_p2s_anharm[it.pairs[0].index / 3][0]][i];
                vec_origin[i] = system->xr_s_no_displace[system->map_p2s[it.pairs[0].index / 3][0]][i];
                for(j = 1; j < norder-1; j++){
                    // vec_origin[i] += system->xr_s_anharm[system->map_p2s_anharm[it.pairs[j].index / 3][it.pairs[j].tran]][i]
                    //                 + xshift_s[it.pairs[j].cell_s][i];
                    vec_origin[i] += system->xr_s_no_displace[system->map_p2s[it.pairs[j].index / 3][it.pairs[j].tran]][i]
                                    + xshift_s[it.pairs[j].cell_s][i];
                }
                vec_origin[i] /= (norder-1);
            }

            for (i = 0; i < 3; ++i) {
                // vec[i] = system->xr_s_anharm[system->map_p2s_anharm[it.pairs[norder - 1].index / 3][it.pairs[norder -
                //                                                                                             1].tran]][i]
                //         // - system->xr_s_anharm[system->map_p2s_anharm[it.pairs[0].index / 3][0]][i]
                //         - vec_origin[i]
                //         + xshift_s[it.pairs[norder - 1].cell_s][i];

                vec[i] = system->xr_s_no_displace[system->map_p2s[it.pairs[norder - 1].index / 3][it.pairs[norder -
                                                                                                            1].tran]][i]
                        // - system->xr_s_no_displace[system->map_p2s_anharm[it.pairs[0].index / 3][0]][i]
                        - vec_origin[i]
                        + xshift_s[it.pairs[norder - 1].cell_s][i];
            }

            rotvec(vec, vec, system->lavec_s);

            fcs_tmp += it.fcs_val * vec[ixyz2];
            // it.pairs[norder - 1].index % 3 == ixyz1 has been checked.
        }
        // std::cout << "loop done" << std::endl;
        // std::cout << "index_with_cell " << std::endl;
        // for(auto index_with_cell_elements: index_with_cell){
        //     std::cout << index_with_cell_elements << " ";
        // }std::cout << std::endl;

        if (std::abs(fcs_tmp) > eps15) {

            pairs_vec.clear();

            pairs_tmp.index = index_with_cell[0];
            pairs_tmp.tran = 0;
            pairs_tmp.cell_s = 0;
            pairs_vec.push_back(pairs_tmp);
            for (i = 1; i < norder - 1; ++i) {
                pairs_tmp.index = index_with_cell[3 * i - 2];
                pairs_tmp.tran = index_with_cell[3 * i - 1];
                pairs_tmp.cell_s = index_with_cell[3 * i];
                pairs_vec.push_back(pairs_tmp);
            }
            delta_fcs.emplace_back(fcs_tmp, pairs_vec);
        }
        // std::cout << "done." << std::endl;
    }

    deallocate(xshift_s);

    fcs_aligned.clear();
    set_index_uniq.clear();
}

// mirror_image_mode = 1 in default.
// mirror_image_mode = 0 has not been thoroughly tested.
void Scph::compute_del_v_strain_in_real_space2(const std::vector<FcsArrayWithCell> &fcs_in,
                                               std::vector<FcsArrayWithCell> &delta_fcs,
                                               const int ixyz11,
                                               const int ixyz12,
                                               const int ixyz21,
                                               const int ixyz22,
                                               const int mirror_image_mode)
{
    unsigned int i, j;
    double vec1[3], vec2[3], vec_origin[3];
    double fcs_tmp = 0.0;

    std::vector<FcsAlignedForGruneisen2> fcs_aligned;
    std::vector<AtomCellSuper> pairs_vec;
    std::vector<int> index_old, index_now;
    std::vector<int> index_with_cell, index_with_cell_old;
    std::set<std::vector<int>> set_index_uniq;
    AtomCellSuper pairs_tmp;

    const auto norder = fcs_in[0].pairs.size();
    unsigned int nmulti;

    delta_fcs.clear();
    fcs_aligned.clear();

    for (const auto &it: fcs_in) {
        fcs_aligned.emplace_back(it.fcs_val, it.pairs);
    }
    std::sort(fcs_aligned.begin(), fcs_aligned.end());

    // prepare supercell shift
    double **xshift_s;
    const auto ncell_s = 27;

    allocate(xshift_s, ncell_s, 3);

    unsigned int icell = 0;
    int ix, iy, iz;
    for (i = 0; i < 3; ++i) xshift_s[0][i] = 0;
    icell = 1;
    for (ix = -1; ix <= 1; ++ix) {
        for (iy = -1; iy <= 1; ++iy) {
            for (iz = -1; iz <= 1; ++iz) {
                if (ix == 0 && iy == 0 && iz == 0) continue;

                xshift_s[icell][0] = ix*1.0;
                xshift_s[icell][1] = iy*1.0;
                xshift_s[icell][2] = iz*1.0;

                ++icell;
            }
        }
    }

    if(mirror_image_mode == 0){
        // original implementation
        // calculate IFC renormalization for the same atomic combination in the supercell
        // but with different mirror images at once and assign the average value for each
        // mirror images.
        index_old.clear();
        const auto nelems = 2 * (norder - 3) + 1;
        for (i = 0; i < nelems; ++i) index_old.push_back(-1);

        index_with_cell.clear();
        set_index_uniq.clear();

        for (const auto &it: fcs_aligned) {

            // if the xyz does not match the considering coponent
            if(it.pairs[norder - 2].index % 3 != ixyz11 || it.pairs[norder-1].index % 3 != ixyz21){
                continue;
            }

            index_now.clear();
            index_with_cell.clear();

            index_now.push_back(it.pairs[0].index);
            index_with_cell.push_back(it.pairs[0].index);

            for (i = 1; i < norder - 2; ++i) {
                index_now.push_back(it.pairs[i].index);
                index_now.push_back(it.pairs[i].tran);

                index_with_cell.push_back(it.pairs[i].index);
                index_with_cell.push_back(it.pairs[i].tran);
                index_with_cell.push_back(it.pairs[i].cell_s);
            }

            if (index_now != index_old) {

                if (index_old[0] != -1) {

                    nmulti = set_index_uniq.size();
                    fcs_tmp /= static_cast<double>(nmulti);

                    if (std::abs(fcs_tmp) > eps15) {
                        for (const auto &it2: set_index_uniq) {

                            pairs_vec.clear();

                            pairs_tmp.index = it2[0];
                            pairs_tmp.tran = 0;
                            pairs_tmp.cell_s = 0;
                            pairs_vec.push_back(pairs_tmp);
                            for (i = 1; i < norder - 2; ++i) {
                                pairs_tmp.index = it2[3 * i - 2];
                                pairs_tmp.tran = it2[3 * i - 1];
                                pairs_tmp.cell_s = it2[3 * i];
                                pairs_vec.push_back(pairs_tmp);
                            }
                            delta_fcs.emplace_back(fcs_tmp, pairs_vec);
                        }
                    }
                    set_index_uniq.clear();
                }

                fcs_tmp = 0.0;
                index_old.clear();
                index_old.reserve(index_now.size());
                std::copy(index_now.begin(), index_now.end(), std::back_inserter(index_old));
            }

            set_index_uniq.insert(index_with_cell);

            for(i = 0; i < 3; i++){
                // vec_origin[i] = system->xr_s_anharm[system->map_p2s_anharm[it.pairs[0].index / 3][0]][i];
                vec_origin[i] = system->xr_s_no_displace[system->map_p2s[it.pairs[0].index / 3][0]][i];
                for(j = 1; j < norder-2; j++){
                    // vec_origin[i] += system->xr_s_anharm[system->map_p2s_anharm[it.pairs[j].index / 3][it.pairs[j].tran]][i]
                    //                 + xshift_s[it.pairs[j].cell_s][i];
                    vec_origin[i] += system->xr_s_no_displace[system->map_p2s[it.pairs[j].index / 3][it.pairs[j].tran]][i]
                                    + xshift_s[it.pairs[j].cell_s][i];
                }
                vec_origin[i] /= (norder-2);
            }

            for (i = 0; i < 3; ++i) {
                // vec1[i] = system->xr_s_anharm[system->map_p2s_anharm[it.pairs[norder - 2].index / 3][it.pairs[norder - 2].tran]]
                //         [i]
                //         // - system->xr_s_anharm[system->map_p2s_anharm[it.pairs[0].index / 3][0]][i]
                //         - vec_origin[i]
                //         + xshift_s[it.pairs[norder - 2].cell_s][i];

                vec1[i] = system->xr_s_no_displace[system->map_p2s[it.pairs[norder - 2].index / 3][it.pairs[norder - 2].tran]][i]
                        // - system->xr_s_no_displace[system->map_p2s[it.pairs[0].index / 3][0]][i]
                        - vec_origin[i]
                        + xshift_s[it.pairs[norder - 2].cell_s][i];
                // vec2[i] = system->xr_s_anharm[system->map_p2s_anharm[it.pairs[norder - 1].index / 3][it.pairs[norder -
                //                                                                                             1].tran]]
                //         [i]
                //         // - system->xr_s_anharm[system->map_p2s_anharm[it.pairs[0].index / 3][0]][i]
                //         - vec_origin[i]
                //         + xshift_s[it.pairs[norder - 1].cell_s][i];

                vec2[i] = system->xr_s_no_displace[system->map_p2s[it.pairs[norder - 1].index / 3][it.pairs[norder - 1].tran]][i]
                        // - system->xr_s_no_displace[system->map_p2s[it.pairs[0].index / 3][0]][i]
                        - vec_origin[i]
                        + xshift_s[it.pairs[norder - 1].cell_s][i];
            }

            rotvec(vec1, vec1, system->lavec_s);
            rotvec(vec2, vec2, system->lavec_s);

            fcs_tmp += it.fcs_val * vec1[ixyz12] * vec2[ixyz22];
            // xyz component of the IFC has already been checked
        }

        nmulti = set_index_uniq.size();
        fcs_tmp /= static_cast<double>(nmulti);

        if (std::abs(fcs_tmp) > eps15) {
            for (const auto &it2: set_index_uniq) {

                pairs_vec.clear();

                pairs_tmp.index = it2[0];
                pairs_tmp.tran = 0;
                pairs_tmp.cell_s = 0;
                pairs_vec.push_back(pairs_tmp);
                for (i = 1; i < norder - 2; ++i) {
                    pairs_tmp.index = it2[3 * i - 2];
                    pairs_tmp.tran = it2[3 * i - 1];
                    pairs_tmp.cell_s = it2[3 * i];
                    pairs_vec.push_back(pairs_tmp);
                }
                delta_fcs.emplace_back(fcs_tmp, pairs_vec);
            }
        }
    }
    else{ // if(mirror_image_mode != 0)
        // new implementation
        // calculate IFC renormalization separately for each mirror image combinations.
        index_with_cell_old.clear();
        const auto nelems = 3 * (norder - 3) + 1;
        for (i = 0; i < nelems; ++i) index_with_cell_old.push_back(-1);

        index_with_cell.clear();

        for (const auto &it: fcs_aligned) {

            // std::cout << "FCS" << std::endl;
            // std::cout << "index ";
            // for(auto ittmp: it.pairs){
            //     std::cout << ittmp.index << " ";
            // }std::cout << std::endl;

            // if the xyz does not match the considering coponent
            if(it.pairs[norder - 2].index % 3 != ixyz11 || it.pairs[norder-1].index % 3 != ixyz21){
                continue;
            }

            index_with_cell.clear();

            index_with_cell.push_back(it.pairs[0].index);

            for (i = 1; i < norder - 2; ++i) {
                index_with_cell.push_back(it.pairs[i].index);
                index_with_cell.push_back(it.pairs[i].tran);
                index_with_cell.push_back(it.pairs[i].cell_s);
            }

            if (index_with_cell != index_with_cell_old) {

                if (index_with_cell_old[0] != -1) {

                    if (std::abs(fcs_tmp) > eps15) {

                        pairs_vec.clear();

                        pairs_tmp.index = index_with_cell_old[0];
                        pairs_tmp.tran = 0;
                        pairs_tmp.cell_s = 0;
                        pairs_vec.push_back(pairs_tmp);
                        for (i = 1; i < norder - 2; ++i) {
                            pairs_tmp.index = index_with_cell_old[3 * i - 2];
                            pairs_tmp.tran = index_with_cell_old[3 * i - 1];
                            pairs_tmp.cell_s = index_with_cell_old[3 * i];
                            pairs_vec.push_back(pairs_tmp);
                        }
                        delta_fcs.emplace_back(fcs_tmp, pairs_vec);
                    }
                }

                fcs_tmp = 0.0;
                index_with_cell_old.clear();
                index_with_cell_old.reserve(index_with_cell.size());
                std::copy(index_with_cell.begin(), index_with_cell.end(), std::back_inserter(index_with_cell_old));
            }

            for(i = 0; i < 3; i++){
                // vec_origin[i] = system->xr_s_anharm[system->map_p2s_anharm[it.pairs[0].index / 3][0]][i];
                vec_origin[i] = system->xr_s_no_displace[system->map_p2s[it.pairs[0].index / 3][0]][i];
                for(j = 1; j < norder-2; j++){
                    // vec_origin[i] += system->xr_s_anharm[system->map_p2s_anharm[it.pairs[j].index / 3][it.pairs[j].tran]][i]
                    //                 + xshift_s[it.pairs[j].cell_s][i];
                    vec_origin[i] += system->xr_s_no_displace[system->map_p2s[it.pairs[j].index / 3][it.pairs[j].tran]][i]
                                    + xshift_s[it.pairs[j].cell_s][i];
                }
                vec_origin[i] /= (norder-2);
            }

            for (i = 0; i < 3; ++i) {
                // vec1[i] = system->xr_s_anharm[system->map_p2s_anharm[it.pairs[norder - 2].index / 3][it.pairs[norder -
                //                                                                                             2].tran]]
                //         [i]
                //         // - system->xr_s_anharm[system->map_p2s_anharm[it.pairs[0].index / 3][0]][i]
                //         - vec_origin[i]
                //         + xshift_s[it.pairs[norder - 2].cell_s][i];

                vec1[i] = system->xr_s_no_displace[system->map_p2s[it.pairs[norder - 2].index / 3][it.pairs[norder -
                                                                                                            2].tran]][i]
                        // - system->xr_s_no_displace[system->map_p2s[it.pairs[0].index / 3][0]][i]
                        - vec_origin[i]
                        + xshift_s[it.pairs[norder - 2].cell_s][i];

                // vec2[i] = system->xr_s_anharm[system->map_p2s_anharm[it.pairs[norder - 1].index / 3][it.pairs[norder -
                //                                                                                             1].tran]]
                //         [i]
                //         // - system->xr_s_anharm[system->map_p2s_anharm[it.pairs[0].index / 3][0]][i]
                //         - vec_origin[i]
                //         + xshift_s[it.pairs[norder - 1].cell_s][i];

                vec2[i] = system->xr_s_no_displace[system->map_p2s[it.pairs[norder - 1].index / 3][it.pairs[norder -
                                                                                                            1].tran]][i]
                        // - system->xr_s_no_displace[system->map_p2s[it.pairs[0].index / 3][0]][i]
                        - vec_origin[i]
                        + xshift_s[it.pairs[norder - 1].cell_s][i];
            }

            // rotvec(vec1, vec1, system->lavec_s_anharm);
            // rotvec(vec2, vec2, system->lavec_s_anharm);
            rotvec(vec1, vec1, system->lavec_s);
            rotvec(vec2, vec2, system->lavec_s);

            fcs_tmp += it.fcs_val * vec1[ixyz12] * vec2[ixyz22];
            // xyz components of IFC have been checked
        }

        if (std::abs(fcs_tmp) > eps15) {

            pairs_vec.clear();

            pairs_tmp.index = index_with_cell[0];
            pairs_tmp.tran = 0;
            pairs_tmp.cell_s = 0;
            pairs_vec.push_back(pairs_tmp);
            for (i = 1; i < norder - 2; ++i) {
                pairs_tmp.index = index_with_cell[3 * i - 2];
                pairs_tmp.tran = index_with_cell[3 * i - 1];
                pairs_tmp.cell_s = index_with_cell[3 * i];
                pairs_vec.push_back(pairs_tmp);
            }
            delta_fcs.emplace_back(fcs_tmp, pairs_vec);
        }
        // std::cout << "done." << std::endl;
    }

    deallocate(xshift_s);

    fcs_aligned.clear();
    set_index_uniq.clear();
}

FcsClassExtent Scph::from_FcsArrayWithCell_to_FcsClassExtent(const FcsArrayWithCell &fc_in){

    FcsClassExtent fc_out;
    unsigned int atm1, atm2, xyz1, xyz2, cell_s;
    double fcs_val;

    if(fc_in.pairs.size() != 2){
        std::cout << "Warning in from_FcsArrayWithCell_to_FcsClassExtent:\n only harmonic IFC can be transformed to FcsClassExtent format." << std::endl;
    }

    // fc_out.atm1 = system->map_p2s_anharm[fc_in.pairs[0].index / 3][0];
    fc_out.atm1 = fc_in.pairs[0].index / 3;
    fc_out.atm2 = system->map_p2s_anharm[fc_in.pairs[1].index / 3][fc_in.pairs[1].tran];
    fc_out.xyz1 = fc_in.pairs[0].index % 3;
    fc_out.xyz2 = fc_in.pairs[1].index % 3;
    fc_out.cell_s = fc_in.pairs[1].cell_s;
    fc_out.fcs_val = fc_in.fcs_val;

    return fc_out;
}

void Scph::calculate_del_v0_strain_with_strain_displace(std::complex<double> *del_v0_strain_with_strain_displace, 
                                               double *C1_array,
                                               double **C2_array,
                                               double ***C3_array,
                                               double **eta_tensor,
                                               double **u_tensor,
                                               std::complex<double> **del_v1_strain_from_harmonic,
                                               std::complex<double> **del_v1_strain_from_cubic,
                                               std::complex<double> **del_v1_strain_from_quartic,
                                               std::complex<double> ***del_v2_strain_from_cubic,
                                               std::complex<double> ***del_v2_strain_from_quartic,
                                               std::complex<double> ****del_v3_strain_from_quartic,
                                               double *q0,
                                               double pvcell)
{

    int ns = dynamical->neval;
    int nk = kmesh_dense->nk;
    double **del_eta_del_u;
    double *del_v0_del_eta;
    double *del_v0_strain_with_strain;

    std::complex<double> **del_v1_strain_with_strain;
    std::complex<double> ***del_v2_strain_with_strain;
    // double *del_v3_strain;
    
    allocate(del_eta_del_u, 9, 9);
    allocate(del_v0_del_eta, 9);
    allocate(del_v0_strain_with_strain, 9);

    allocate(del_v1_strain_with_strain, 9, ns);
    allocate(del_v2_strain_with_strain, 9, ns, ns);

    double factor = 1.0/6.0 * 4.0 * nk;
    int i1, i2, i3, ixyz1, ixyz2, ixyz3, ixyz4;
    int is1, is2, is3;

    
    
    // calculate the derivative of eta_tensor by u_tensor
    for(i1 = 0; i1 < 9; i1++){
        ixyz1 = i1/3;
        ixyz2 = i1%3;
        for(i2 = 0; i2 < 9; i2++){
            ixyz3 = i2/3;
            ixyz4 = i2%3;

            del_eta_del_u[i1][i2] = 0.0;
            
            if(ixyz1 == ixyz3 && ixyz2 == ixyz4){
                del_eta_del_u[i1][i2] += 0.5;
            }
            if(ixyz2 == ixyz3 && ixyz1 == ixyz4){
                del_eta_del_u[i1][i2] += 0.5;
            }
            if(ixyz1 == ixyz3){
                del_eta_del_u[i1][i2] += 0.5 * u_tensor[ixyz2][ixyz4];
            }
            if(ixyz2 == ixyz3){
                del_eta_del_u[i1][i2] += 0.5 * u_tensor[ixyz1][ixyz4];
            }
        }
    }

    // debug
/*    std::cout << "del_eta_del_u: " << std::endl;
    for(i1 = 0; i1 < 9; i1++){
        for(i2 = 0; i2 < 3; i2++){
            for(i3 = 0; i3 < 3; i3++){
                std::cout << del_eta_del_u[i2*3+i3][i1] << " ";
            }std::cout << std::endl;
        }std::cout << std::endl;
    }std::cout << std::endl;
*/
    // calculate del_v0_del_eta
    for(i1 = 0; i1 < 9; i1++){
        // del_v0_del_eta[i1] = 0.0;
        del_v0_del_eta[i1] = C1_array[i1];
        for(i2 = 0; i2 < 9; i2++){
            del_v0_del_eta[i1] += C2_array[i1][i2] * eta_tensor[i2/3][i2%3];
            for(i3 = 0; i3 < 9; i3++){
                del_v0_del_eta[i1] += 0.5 * C3_array[i1][i2][i3] * eta_tensor[i2/3][i2%3] * eta_tensor[i3/3][i3%3];
            }
        }
    }

    // calculate contribution from v0 without atomic displacements
    for(i1 = 0; i1 < 9; i1++){
        del_v0_strain_with_strain[i1] = 0.0;
        for(i2 = 0; i2 < 9; i2++){
            del_v0_strain_with_strain[i1] += del_eta_del_u[i2][i1] * del_v0_del_eta[i2];
        }
    }

    // add pV term
    double F_tensor[3][3]; // F_{mu nu} = delta_{mu nu} + u_{mu nu}
    for(i1 = 0; i1 < 3; i1++){
        for(i2 = 0; i2 < 3; i2++){
            F_tensor[i1][i2] = u_tensor[i1][i2];
        }
        F_tensor[i1][i1] += 1.0;
    }
    for(i1 = 0; i1 < 9; i1++){
        is1 = i1/3;
        is2 = i1%3;
        ixyz1 = (is1+1)%3;
        ixyz2 = (is1+2)%3;
        ixyz3 = (is2+1)%3;
        ixyz4 = (is2+2)%3;

        del_v0_strain_with_strain[i1] += pvcell * (F_tensor[ixyz1][ixyz3]*F_tensor[ixyz2][ixyz4] - F_tensor[ixyz1][ixyz4]*F_tensor[ixyz2][ixyz3]);
    }

    // calculate del_v1_strain
    for(i1 = 0; i1 < 9; i1++){
        for(is1 = 0; is1 < ns; is1++){
            del_v1_strain_with_strain[i1][is1] = del_v1_strain_from_harmonic[i1][is1];
            for(i2 = 0; i2 < 9; i2++){
                del_v1_strain_with_strain[i1][is1] += del_v1_strain_from_cubic[i1*9+i2][is1] * u_tensor[i2/3][i2%3];
                for(i3 = 0; i3 < 9; i3++){
                    del_v1_strain_with_strain[i1][is1] += 0.5 * del_v1_strain_from_quartic[i1*81+i2*9+i3][is1] * u_tensor[i2/3][i2%3] * u_tensor[i3/3][i3%3];
                }
            }
        }
    }
    
    // calculate del_v2_strain
    for(i1 = 0; i1 < 9; i1++){
        for(is1 = 0; is1 < ns; is1++){
            for(is2 = 0; is2 < ns; is2++){
                del_v2_strain_with_strain[i1][is1][is2] = del_v2_strain_from_cubic[i1][0][is1*ns+is2];
                for(i2 = 0; i2 < 9; i2++){
                    del_v2_strain_with_strain[i1][is1][is2] += del_v2_strain_from_quartic[i1*9+i2][0][is1*ns+is2] * u_tensor[i2/3][i2%3];
                }
            }
        }
    }

    // calculate del_v0_strain_with_strain_displace
    for(i1 = 0; i1 < 9; i1++){
        del_v0_strain_with_strain_displace[i1] = del_v0_strain_with_strain[i1];
        for(is1 = 0; is1 < ns; is1++){
            del_v0_strain_with_strain_displace[i1] += del_v1_strain_with_strain[i1][is1] * q0[is1];
            for(is2 = 0; is2 < ns; is2++){
                del_v0_strain_with_strain_displace[i1] += 0.5 * del_v2_strain_with_strain[i1][is1][is2] * q0[is1] * q0[is2];
                for(is3 = 0; is3 < ns; is3++){
                    del_v0_strain_with_strain_displace[i1] += factor * del_v3_strain_from_quartic[i1][0][is1][is2*ns+is3] * q0[is1] * q0[is2] * q0[is3];
                }
            }
        }
    }
    

    deallocate(del_eta_del_u);
    deallocate(del_v0_del_eta);
    deallocate(del_v0_strain_with_strain);
    deallocate(del_v1_strain_with_strain);
    deallocate(del_v2_strain_with_strain);

    return;
}


void Scph::renormalize_v1_array(std::complex<double> *v1_array_renormalized, 
                                std::complex<double> *v1_array_original, 
                                std::complex<double> **delta_v2_array_original,
                                std::complex<double> ***v3_array_original, 
                                std::complex<double> ***v4_array_original,
                                double *q0)
{
    int is, is1, is2, is3;
    int ik_irred0 = kpoint_map_symmetry[0].knum_irred_orig;
    const auto ns = dynamical->neval;
    double factor = 0.5 * 4.0 * kmesh_dense->nk;
    double factor2 = 1.0/6.0 * 4.0 * kmesh_dense->nk;
    
    // renormalize v1 array
    // std::cout << "renormalize_v1_array: " << std::endl;
    for(is = 0; is < ns; is++){

        // std::cout << "is = " << is << std::endl;
        v1_array_renormalized[is] = v1_array_original[is];

        // std::cout << "v1_array_original " << v1_array_renormalized[is];

        v1_array_renormalized[is] += omega2_harmonic[0][is] * q0[is]; // original v2 is assumed to be diagonal

        // std::cout << "original harmonic " << v1_array_renormalized[is] << std::endl;

        for(is1 = 0; is1 < ns; is1++){
            v1_array_renormalized[is] += delta_v2_array_original[0][is*ns+is1] * q0[is1];
            // std::cout << "is1 = " << is1 << " " << delta_v2_array_original[0][is*ns+is1] * q0[is1] << std::endl;
        }

        // std::cout << "all harmonic " << v1_array_renormalized[is] << std::endl;

        for(is1 = 0; is1 < ns; is1++){
            for(is2 = 0; is2 < ns; is2++){
                v1_array_renormalized[is] += factor * v3_array_original[0][is][is1*ns+is2] 
                                            * q0[is1] * q0[is2];
            }
        }

        for(is1 = 0; is1 < ns; is1++){
            for(is2 = 0; is2 < ns; is2++){
                for(is3 = 0; is3 < ns; is3++){
                    
                    v1_array_renormalized[is] += factor2 * v4_array_original[ik_irred0*kmesh_dense->nk][is*ns+is1][is2*ns+is3] 
                                             * q0[is1] * q0[is2] * q0[is3]; 
                    // the factor 4.0 appears due to the definition of v4_array = 1.0/(4.0*N_scph) Phi_4
                }
            }
        }
        
    }
}

void Scph::renormalize_v2_array(std::complex<double> **delta_v2_array_renormalize, 
                                std::complex<double> **delta_v2_array_original,
                                std::complex<double> ***v3_array_original, 
                                std::complex<double> ***v4_array_original,  
                                double *q0)
{
    using namespace Eigen;

    int ik;
    int is1, is2, js1, js2;
    int knum, knum_interpolate;
    int nk_scph = kmesh_dense->nk;
    int nk_interpolate = kmesh_coarse->nk;
    double factor = 4.0 * nk_scph;
    double factor2 = 4.0 * nk_scph * 0.5;

    const auto complex_zero = std::complex<double>(0.0, 0.0);
    
    const auto ns = dynamical->neval;
    const auto nk_irred_interpolate = kmesh_coarse->nk_irred;
    std::complex<double> ***dymat_q;
    MatrixXcd Dymat(ns, ns);
    MatrixXcd evec_tmp(ns, ns);
    allocate(dymat_q, ns, ns, nk_interpolate);

    for(ik = 0; ik < nk_irred_interpolate; ik++){

        knum_interpolate = kmesh_coarse->kpoint_irred_all[ik][0].knum;//kp_irred_interpolate[ik][0].knum;
        knum = kmap_interpolate_to_scph[knum_interpolate];

        // calculate renormalization
        for(is1 = 0; is1 < ns; is1++){
            for(is2 = 0; is2 < ns; is2++){
                Dymat(is1, is2) = complex_zero;
                for(js1 = 0; js1 < ns; js1++){
                    // cubic reormalization
                    Dymat(is1, is2) += factor * v3_array_original[knum][js1][is2*ns+is1] 
                                       * q0[js1];
                    // quartic renormalization
                    for(js2 = 0; js2 < ns; js2++){
                        Dymat(is1, is2) += factor2 * v4_array_original[ik*nk_scph][is1*ns+is2][js1*ns+js2] 
                                           * q0[js1] * q0[js2];
                    }
                }
            }
        }

        // unitary transform Dymat
        for(is1 = 0; is1 < ns; is1++){
            for(is2 = 0; is2 < ns; is2++){
                evec_tmp(is1, is2) = evec_harmonic[knum][is2][is1]; // transpose
            }
        }
        Dymat = evec_tmp * Dymat * evec_tmp.adjoint();
        
        // symmetrize dynamical matrix
        symmetrize_dynamical_matrix(ik, Dymat);
        
        // store to dymat_q
        for(is1 = 0; is1 < ns; is1++){
            for(is2 = 0; is2 < ns; is2++){
                dymat_q[is1][is2][knum_interpolate] = Dymat(is1, is2);
            }
        }
    }
    // replicate dymat_q to all q
    replicate_dymat_for_all_kpoints(dymat_q);
    
    // copy to delta_v2_array_renormalize
    for(ik = 0; ik < nk_interpolate; ik++){
        knum = kmap_interpolate_to_scph[ik];

        // unitary transform Dymat
        for(is1 = 0; is1 < ns; is1++){
            for(is2 = 0; is2 < ns; is2++){
                Dymat(is1, is2) = dymat_q[is1][is2][ik];
                evec_tmp(is1, is2) = evec_harmonic[knum][is2][is1]; // transpose
            }
        }
        Dymat = evec_tmp.adjoint() * Dymat * evec_tmp;

        for(is1 = 0; is1 < ns; is1++){
            for(is2 = 0; is2 < ns; is2++){
                delta_v2_array_renormalize[ik][is1*ns+is2] = Dymat(is1, is2);
            }
        }
    }
    
    // add original delta_v2_array
    // This is important when cell relaxation is performed
    for(ik = 0; ik < nk_interpolate; ik++){
        for(is1 = 0; is1 < ns*ns; is1++){
            delta_v2_array_renormalize[ik][is1] += delta_v2_array_original[ik][is1];
        }
    }

    deallocate(dymat_q);
    
}

void Scph::renormalize_v3_array(std::complex<double> ***v3_array_renormalized,
                                std::complex<double> ***v3_array_original, 
                                std::complex<double> ***v4_array_original, 
                                double *q0)
{
    int ik;
    int is1, is2, is3, js;
    const auto ns = dynamical->neval;
    int ik_irred0 = kpoint_map_symmetry[0].knum_irred_orig;
    int nk_scph = kmesh_dense->nk;

    for(ik = 0; ik < nk_scph; ik++){
        for(is1 = 0; is1 < ns; is1++){
            for(int is2 = 0; is2 < ns; is2++){
                for(int is3 = 0; is3 < ns; is3++){
                    v3_array_renormalized[ik][is1][is2*ns+is3] = v3_array_original[ik][is1][is2*ns+is3];
                    for(int js = 0; js < ns; js++){
                        v3_array_renormalized[ik][is1][is2*ns+is3] += 
                            v4_array_original[ik_irred0*nk_scph+ik][js*ns+is1][is2*ns+is3] * q0[js];
                    }
                }
            }
        }
    }

}

void Scph::renormalize_v0(double &v0_renormalized,
                          double v0_original,
                          std::complex<double> *v1_array_original,
                          std::complex<double> **delta_v2_array_original,
                          std::complex<double> ***v3_array_original,
                          std::complex<double> ***v4_array_original,
                          double *q0)
{
    int is1, is2, is3, is4;
    const auto ns = dynamical->neval;
    int nk_scph = kmesh_dense->nk;
    double factor2 = 1.0/2.0;
    double factor3 = 1.0/6.0 * 4.0 * nk_scph;;
    double factor4 = 1.0/24.0 * 4.0 * nk_scph;;

    std::complex<double> v0_renormalized_tmp;

    v0_renormalized_tmp = v0_original;
    // renormalize from the 1st order, harmonic IFC
    for(is1 = 0; is1 < ns; is1++){
        v0_renormalized_tmp += v1_array_original[is1] * q0[is1];
        v0_renormalized_tmp += factor2 * omega2_harmonic[0][is1] * q0[is1] * q0[is1]; // original v2 is assumed to be diagonal
    }
    // renormalize from the delta_v2_array
    for(is1 = 0; is1 < ns; is1++){
        for(is2 = 0; is2 < ns; is2++){
            v0_renormalized_tmp += factor2 * delta_v2_array_original[0][is1*ns+is2] * q0[is1] * q0[is2];
        }
    }
    // renormalize from the cubic, quartic IFC
    for(is1 = 0; is1 < ns; is1++){
        for(is2 = 0; is2 < ns; is2++){
            for(is3 = 0; is3 < ns; is3++){
                v0_renormalized_tmp += factor3 * v3_array_original[0][is1][is2*ns+is3] * q0[is1] * q0[is2] * q0[is3];
                for(is4 = 0; is4 < ns; is4++){
                    v0_renormalized_tmp += factor4 * v4_array_original[0][is2*ns+is1][is3*ns+is4] * q0[is1] * q0[is2] * q0[is3] * q0[is4];
                }
            }
        }
    } 

    v0_renormalized = v0_renormalized_tmp.real();

}

void Scph::calculate_eta_tensor(double **eta_tensor, 
                                double **u_tensor)
{
    int i1, i2, j;
    for(int i1 = 0; i1 < 3; i1++){
        for(int i2 = 0; i2 < 3; i2++){
            eta_tensor[i1][i2] = 0.5 * (u_tensor[i1][i2] + u_tensor[i2][i1]);
            for(j = 0; j < 3; j++){
                eta_tensor[i1][i2] += u_tensor[i1][j] * u_tensor[i2][j];
            }
        }
    }
    return ;
}

void Scph::renormalize_v0_from_strain(double &v0_with_strain, 
                                      double v0_original, 
                                      double **eta_tensor, 
                                      double *C1_array,
                                      double **C2_array, 
                                      double ***C3_array,
                                      double **u_tensor,
                                      double pvcell)
{
    int ixyz1, ixyz2, ixyz3, ixyz4, ixyz5, ixyz6;

    double factor1  = 0.5;
    double factor2 = 1.0/6.0;

    v0_with_strain = v0_original;

    for(ixyz1 = 0; ixyz1 < 3; ixyz1++){
        for(ixyz2 = 0; ixyz2 < 3; ixyz2++){
            v0_with_strain += C1_array[ixyz1*3+ixyz2] * eta_tensor[ixyz1][ixyz2];
            for(ixyz3 = 0; ixyz3 < 3; ixyz3++){
                for(ixyz4 = 0; ixyz4 < 3; ixyz4++){
                    v0_with_strain += factor1 * C2_array[ixyz1*3+ixyz2][ixyz3*3+ixyz4] * eta_tensor[ixyz1][ixyz2] * eta_tensor[ixyz3][ixyz4];
                    for(ixyz5 = 0; ixyz5 < 3; ixyz5++){
                        for(ixyz6 = 0; ixyz6 < 3; ixyz6++){
                            v0_with_strain += factor2 * C3_array[ixyz1*3+ixyz2][ixyz3*3+ixyz4][ixyz5*3+ixyz6]
                                            * eta_tensor[ixyz1][ixyz2] * eta_tensor[ixyz3][ixyz4] * eta_tensor[ixyz5][ixyz6];
                        }
                    }
                }
            }
        }
    }

    // add pV term
    double vec_tmp1[3], vec_tmp2[3], vec_tmp3[3];
    for(ixyz1 = 0; ixyz1 < 3; ixyz1++){
        vec_tmp1[ixyz1] = u_tensor[0][ixyz1];
        vec_tmp2[ixyz1] = u_tensor[1][ixyz1];
        vec_tmp3[ixyz1] = u_tensor[2][ixyz1];
    }
    vec_tmp1[0] += 1.0;
    vec_tmp2[1] += 1.0;
    vec_tmp3[2] += 1.0;

    double det_F_tensor = system->volume(vec_tmp1, vec_tmp2, vec_tmp3);

    v0_with_strain += pvcell * det_F_tensor;

}

void Scph::renormalize_v1_array_from_strain(std::complex<double> *v1_array_with_strain, 
                                            std::complex<double> *v1_array_original,
                                            std::complex<double> **del_v1_strain_from_harmonic, 
                                            std::complex<double> **del_v1_strain_from_cubic, 
                                            std::complex<double> **del_v1_strain_from_quartic, 
                                            double **u_tensor)
{
    const auto ns = dynamical->neval;

    int ixyz1, ixyz2, ixyz3, ixyz4, ixyz5, ixyz6;
    int ixyz_comb;
    int is;

    double factor1 = 0.5;
    double factor2 = 1.0/6.0;

    for(is = 0; is < ns; is++){
        // original 1st-order IFCs
        v1_array_with_strain[is] = v1_array_original[is];

        for(ixyz1 = 0; ixyz1 < 3; ixyz1++){
            for(ixyz2 = 0; ixyz2 < 3; ixyz2++){
                // renormalization from harmonic IFCs
                v1_array_with_strain[is] += del_v1_strain_from_harmonic[ixyz1*3+ixyz2][is] * u_tensor[ixyz1][ixyz2];

                for(ixyz3 = 0; ixyz3 < 3; ixyz3++){
                    for(ixyz4 = 0; ixyz4 < 3; ixyz4++){
                        // renormalization from cubic IFCs
                        ixyz_comb = ixyz1 * 27 + ixyz2 * 9 + ixyz3 * 3 + ixyz4;
                        v1_array_with_strain[is] += factor1 * del_v1_strain_from_cubic[ixyz_comb][is] 
                                                    * u_tensor[ixyz1][ixyz2] * u_tensor[ixyz3][ixyz4];

                        for(ixyz5 = 0; ixyz5 < 3; ixyz5++){
                            for(ixyz6 = 0; ixyz6 < 3; ixyz6++){
                                // renormalization from quartic IFCs
                                ixyz_comb = ixyz1 * 243 + ixyz2 * 81 + ixyz3 * 27 + ixyz4 * 9 + ixyz5 * 3 + ixyz6;
                                v1_array_with_strain[is] += factor2 * del_v1_strain_from_quartic[ixyz_comb][is]
                                                            * u_tensor[ixyz1][ixyz2] * u_tensor[ixyz3][ixyz4] * u_tensor[ixyz5][ixyz6];
                            }
                        }
                    }
                }
            }
        }
    }

    return ;
}

void Scph::renormalize_v2_array_from_strain(std::complex<double> **delta_v2_array_renormalize, 
                                            std::complex<double> ***del_v2_strain_from_cubic, 
                                            std::complex<double> ***del_v2_strain_from_quartic,
                                            double **u_tensor)
{
    const auto nk = kmesh_dense->nk;
    const auto nk_interpolate = kmesh_coarse->nk;
    const auto ns = dynamical->neval;
    int ik, knum;
    int is1, is2;
    int ixyz1, ixyz2;
    int ixyz, ixyz11, ixyz12, ixyz21, ixyz22, itmp;

    // initialize delta_v2_array_renormalize
    for(ik = 0; ik < nk_interpolate; ik++){
        for(is1 = 0; is1 < ns; is1++){
            for(is2 = 0; is2 < ns; is2++){
                delta_v2_array_renormalize[ik][is1*ns+is2] = 0.0;
            }
        }
    }
    // calculate delta_v2_array_renormalize

    // renormalization from cubic IFCs
    for(ixyz1 = 0; ixyz1 < 3; ixyz1++){
        for(ixyz2 = 0; ixyz2 < 3; ixyz2++){
            for(ik = 0; ik < nk_interpolate; ik++){
                knum = kmap_interpolate_to_scph[ik];
                for(is1 = 0; is1 < ns; is1++){
                    for(is2 = 0; is2 < ns; is2++){
                        delta_v2_array_renormalize[ik][is1*ns+is2] += 
                          del_v2_strain_from_cubic[ixyz1*3 + ixyz2][knum][is1*ns+is2] * u_tensor[ixyz1][ixyz2];
                    }
                }
            }
        }
    }

    // renormalization from quartic IFCs
    for(ixyz = 0; ixyz < 81; ixyz++){
        itmp = ixyz;
        ixyz22 = itmp % 3;
        itmp /= 3;
        ixyz21 = itmp % 3;
        itmp /= 3;
        ixyz12 = itmp % 3;
        ixyz11 = itmp / 3;

        for(ik = 0; ik < nk_interpolate; ik++){
            knum = kmap_interpolate_to_scph[ik];
            for(is1 = 0; is1 < ns; is1++){
                for(is2 = 0; is2 < ns; is2++){
                    delta_v2_array_renormalize[ik][is1*ns+is2] += 
                        0.5 * del_v2_strain_from_quartic[ixyz][knum][is1*ns+is2] * u_tensor[ixyz11][ixyz12] * u_tensor[ixyz21][ixyz22];
                }
            }
        }
    }

    return;
    
}

void Scph::renormalize_v3_array_from_strain(std::complex<double> ***v3_array_with_strain, 
                                            std::complex<double> ***v3_array_original, 
                                            std::complex<double> ****del_v3_strain_from_quartic,
                                            double **u_tensor)
{
    const auto nk_scph = kmesh_dense->nk;
    const auto nk_interpolate = kmesh_coarse->nk;
    const auto ns = dynamical->neval;
    int ik;
    int is1, is2, is3;
    int ixyz1, ixyz2;
    int ixyz, ixyz11, ixyz12, ixyz21, ixyz22, itmp;

    // allocate(v3_array_renormalized, nk, ns, ns * ns);

    for(ik = 0; ik < nk_scph; ik++){
        for(is1 = 0; is1 < ns; is1++){
            for(is2 = 0; is2 < ns; is2++){
                for(is3 = 0; is3 < ns; is3++){
                    // original cubic IFC
                    v3_array_with_strain[ik][is1][is2*ns+is3] = v3_array_original[ik][is1][is2*ns+is3];

                    // renormalization from strain
                    for(ixyz1 = 0; ixyz1 < 3; ixyz1++){
                        for(ixyz2 = 0; ixyz2 < 3; ixyz2++){
                            v3_array_with_strain[ik][is1][is2*ns+is3] +=
                              del_v3_strain_from_quartic[ixyz1*3+ixyz2][ik][is1][is2*ns+is3] * u_tensor[ixyz1][ixyz2];
                        }
                    }

                }
            }
        }
    }

    return;
}
                      
void Scph::compute_anharmonic_v1_array(std::complex<double> *v1_array_renormalized, 
                            std::complex<double> ***v3_array_renormalized, 
                            std::complex<double> ***cmat_convert, 
                            double ** omega2_anharm_T, 
                            const double T_in,
                            std::complex<double> *v1_array_SCP)
{
    using namespace Eigen;

    int is, js, js1, js2, ik; 
    double n1, omega1_tmp;
    std::complex<double> Qtmp;
    const auto ns = dynamical->neval;
    int nk_scph = kmesh_dense->nk;

    MatrixXcd Cmat(ns, ns);
    MatrixXcd v3mat_original_mode(ns, ns), v3mat_tmp(ns, ns);

    // get gradient of the BO surface
    for(is = 0; is < ns; is++){
        v1_array_SCP[is] = v1_array_renormalized[is];
        // v1_array_SCP[is] = 0.0; // for test
    }

    // calculate SCP renormalization
    for(is = 0; is < ns; is++){
        for(ik = 0; ik < nk_scph; ik++){
            // unitary transform phi3 to SCP mode
            for(js1 = 0; js1 < ns; js1++){
                for(js2 = 0; js2 < ns; js2++){
                    Cmat(js2, js1) = cmat_convert[ik][js1][js2]; // transpose
                    v3mat_original_mode(js1, js2) = v3_array_renormalized[ik][is][js1*ns+js2];
                }
            }
            v3mat_tmp = Cmat * v3mat_original_mode * Cmat.adjoint();
            
            // update v1_array_SCP
            for(js = 0; js < ns; js++){
                omega1_tmp = std::sqrt(std::fabs(omega2_anharm_T[ik][js]));
                if (std::abs(omega1_tmp) < eps8) {
                    Qtmp = 0.0;
                }
                else{
                    if(thermodynamics->classical){
                        Qtmp = std::complex<double>(2.0 * T_in * thermodynamics->T_to_Ryd / (omega1_tmp * omega1_tmp), 0.0);
                    }
                    else{
                        n1 = thermodynamics->fB(omega1_tmp, T_in);
                        Qtmp = std::complex<double>((2.0 * n1 + 1.0) / omega1_tmp, 0.0);
                    }
                }
                
                v1_array_SCP[is] += v3mat_tmp(js, js) * Qtmp;
            }
        }
    }

}

void Scph::compute_anharmonic_del_v0_strain(std::complex<double> *del_v0_strain_SCP, 
                                            std::complex<double> *del_v0_strain_with_strain_displace,
                                            std::complex<double> ***del_v2_strain_from_cubic,
                                            std::complex<double> ***del_v2_strain_from_quartic,
                                            std::complex<double> ****del_v3_strain_from_quartic,
                                            double **u_tensor,
                                            double *q0,
                                            std::complex<double> ***cmat_convert, 
                                            double ** omega2_anharm_T, 
                                            const double T_in)
{
    
    using namespace Eigen;

    int nk = kmesh_dense->nk;
    int ns = dynamical->neval;
    double factor = 4.0 * static_cast<double>(nk);
    double factor2 = 1.0/factor;
    
    std::complex<double> ***del_v2_strain_with_strain_displace;
    allocate(del_v2_strain_with_strain_displace, 9, nk, ns*ns);
    
    int i1, i2;
    int ik;
    int is1, is2, is3, js, js1, js2;
    double n1, omega1_tmp;
    std::complex<double> Qtmp;

    MatrixXcd Cmat(ns, ns);
    MatrixXcd del_v2_strain_mat_original_mode(ns, ns), del_v2_strain_mat(ns, ns);

    // calculate del_v2_strain_with_strain_displace
    for(i1 = 0; i1 < 9; i1++){
        for(ik = 0; ik < nk; ik++){
            for(is1 = 0; is1 < ns; is1++){
                for(is2 = 0; is2 < ns; is2++){
                    del_v2_strain_with_strain_displace[i1][ik][is1*ns+is2] = del_v2_strain_from_cubic[i1][ik][is1*ns+is2];
                    // renormalization by strain
                    for(i2 = 0; i2 < 9; i2++){
                        del_v2_strain_with_strain_displace[i1][ik][is1*ns+is2] += del_v2_strain_from_quartic[i1*9+i2][ik][is1*ns+is2] * u_tensor[i2/3][i2%3];
                    }
                    // renormalization by displace
                    for(is3 = 0; is3 < ns; is3++){
                        del_v2_strain_with_strain_displace[i1][ik][is1*ns+is2] += factor * del_v3_strain_from_quartic[i1][ik][is3][is2*ns+is1] * q0[is3];
                    }
                }
            }
        }
    }

    // potential energy term
    for(i1 = 0; i1 < 9; i1++){
        del_v0_strain_SCP[i1] = del_v0_strain_with_strain_displace[i1];
    }
    
    // SCP renormalization
    for(i1 = 0; i1 < 9; i1++){
        for(ik = 0; ik < nk; ik++){
            // unitary transform the derivative of harmonic IFCs to SCP mode
            for(js1 = 0; js1 < ns; js1++){
                for(js2 = 0; js2 < ns; js2++){
                    Cmat(js1, js2) = cmat_convert[ik][js1][js2];
                    del_v2_strain_mat_original_mode(js1, js2) = del_v2_strain_with_strain_displace[i1][ik][js1*ns+js2];
                }
            }
            del_v2_strain_mat = Cmat.adjoint() * del_v2_strain_mat_original_mode * Cmat;

            // update del_v0_strain_SCP
            for(js = 0; js < ns; js++){
                omega1_tmp = std::sqrt(std::fabs(omega2_anharm_T[ik][js]));
                if (std::abs(omega1_tmp) < eps8) {
                    Qtmp = 0.0;
                }
                else{
                    if(omega2_anharm_T[ik][js] < 0.0){
                        std::cout << "Warning in compute_anharmonic_del_v0_strain: squared SCP frequency is negative. ik = " << ik << std::endl;
                    }
                    if(thermodynamics->classical){
                        Qtmp = std::complex<double>(2.0 * T_in * thermodynamics->T_to_Ryd / (omega1_tmp * omega1_tmp), 0.0);
                    }
                    else{
                        n1 = thermodynamics->fB(omega1_tmp, T_in);
                        Qtmp = std::complex<double>((2.0 * n1 + 1.0) / omega1_tmp, 0.0);
                    }
                }
                
                del_v0_strain_SCP[i1] += factor2 * del_v2_strain_mat(js, js) * Qtmp;
            }
        }

    }

    deallocate(del_v2_strain_with_strain_displace);
}

void Scph::setup_kmesh()
{
    unsigned int ik;
    unsigned int i;
    double xtmp[3];
    // Setup k points for SCPH equation
    MPI_Bcast(&kmesh_scph[0], 3, MPI_UNSIGNED, 0, MPI_COMM_WORLD);
    MPI_Bcast(&kmesh_interpolate[0], 3, MPI_UNSIGNED, 0, MPI_COMM_WORLD);

    kmesh_coarse = new KpointMeshUniform(kmesh_interpolate);
    kmesh_dense = new KpointMeshUniform(kmesh_scph);
    kmesh_coarse->setup(symmetry->SymmList, system->rlavec_p);
    kmesh_dense->setup(symmetry->SymmList, system->rlavec_p);

    if (mympi->my_rank == 0) {
//        if (verbosity > 0) {
        std::cout << " Setting up the SCPH calculations ..." << std::endl << std::endl;
        std::cout << "  Gamma-centered uniform grid with the following mesh density:" << std::endl;
        std::cout << "  nk1:" << std::setw(5) << kmesh_scph[0] << std::endl;
        std::cout << "  nk2:" << std::setw(5) << kmesh_scph[1] << std::endl;
        std::cout << "  nk3:" << std::setw(5) << kmesh_scph[2] << std::endl;
        std::cout << std::endl;
        std::cout << "  Number of k points : " << kmesh_dense->nk << std::endl;
        std::cout << "  Number of irreducible k points : " << kmesh_dense->nk_irred << std::endl;
        std::cout << std::endl;
        std::cout << "  Fourier interpolation from reciprocal to real space" << std::endl;
        std::cout << "  will be performed with the following mesh density:" << std::endl;
        std::cout << "  nk1:" << std::setw(5) << kmesh_interpolate[0] << std::endl;
        std::cout << "  nk2:" << std::setw(5) << kmesh_interpolate[1] << std::endl;
        std::cout << "  nk3:" << std::setw(5) << kmesh_interpolate[2] << std::endl;
        std::cout << std::endl;
        std::cout << "  Number of k points : " << kmesh_coarse->nk << std::endl;
        std::cout << "  Number of irreducible k points : "
                  << kmesh_coarse->nk_irred << std::endl;
//        }
    }

    allocate(kmap_interpolate_to_scph, kmesh_coarse->nk);

    for (ik = 0; ik < kmesh_coarse->nk; ++ik) {
        for (i = 0; i < 3; ++i) xtmp[i] = kmesh_coarse->xk[ik][i];

        const auto loc = kmesh_dense->get_knum(xtmp);

        if (loc == -1)
            exit("setup_kmesh",
                 "KMESH_INTERPOLATE should be a integral multiple of KMESH_SCPH");
        kmap_interpolate_to_scph[ik] = loc;
    }
}

void Scph::setup_transform_symmetry()
{
    // Construct small_group_at_k, symop_minus_at_k, and
    // mat_transport_sym.

    unsigned int ik;
    unsigned int is, js;
    unsigned int icrd, jcrd;
    double x1[3], x2[3], k[3], k_minus[3], Sk[3], xtmp[3];
    double S_cart[3][3], S_frac[3][3], S_frac_inv[3][3];
    double S_recip[3][3];
    std::complex<double> im(0.0, 1.0);
    std::complex<double> **gamma_tmp;
    bool *flag;

    const auto natmin = system->natmin;
    const auto ns = dynamical->neval;
    const auto nk_irred_interpolate = kmesh_coarse->nk_irred;

    allocate(gamma_tmp, ns, ns);
    allocate(mat_transform_sym, nk_irred_interpolate,
             symmetry->nsym, ns, ns);
    //allocate(small_group_at_k, nk_irred_interpolate);
    allocate(symop_minus_at_k, nk_irred_interpolate);
    allocate(kpoint_map_symmetry, kmesh_coarse->nk);
    allocate(flag, kmesh_coarse->nk);

    for (ik = 0; ik < kmesh_coarse->nk; ++ik) {
        flag[ik] = false;
    }

    for (ik = 0; ik < nk_irred_interpolate; ++ik) {

        symop_minus_at_k[ik].clear();

        const auto knum = kmesh_coarse->kpoint_irred_all[ik][0].knum;
        for (icrd = 0; icrd < 3; ++icrd) {
            k[icrd] = kmesh_coarse->xk[knum][icrd];
            k_minus[icrd] = -k[icrd];
        }
        const auto knum_minus = kmesh_coarse->kindex_minus_xk[knum];

        unsigned int isym = 0;

        for (const auto &it: symmetry->SymmListWithMap) {

            for (icrd = 0; icrd < 3; ++icrd) {
                for (jcrd = 0; jcrd < 3; ++jcrd) {
                    S_cart[icrd][jcrd] = it.rot[3 * icrd + jcrd];
                    S_frac[icrd][jcrd] = it.rot_real[3 * icrd + jcrd];
                    S_recip[icrd][jcrd] = it.rot_reciprocal[3 * icrd + jcrd];
                }
            }

            invmat3(S_frac_inv, S_frac);
            rotvec(Sk, k, S_recip);

            for (auto i = 0; i < 3; ++i) Sk[i] = Sk[i] - nint(Sk[i]);

            const auto knum_sym = kmesh_coarse->get_knum(Sk);
            if (knum_sym == -1)
                exit("setup_transform_symmetry",
                     "kpoint not found");

            if (knum_sym == knum_minus) symop_minus_at_k[ik].push_back(isym);

            if (!flag[knum_sym]) {
                kpoint_map_symmetry[knum_sym].symmetry_op = isym;
                kpoint_map_symmetry[knum_sym].knum_irred_orig = ik;
                kpoint_map_symmetry[knum_sym].knum_orig = knum;
                flag[knum_sym] = true;
            }

            for (is = 0; is < ns; ++is) {
                for (js = 0; js < ns; ++js) {
                    gamma_tmp[is][js] = std::complex<double>(0.0, 0.0);
                }
            }

            for (unsigned int jat = 0; jat < natmin; ++jat) {
                const auto iat = it.mapping[jat];

                // Fractional coordinates of x1 and x2
                for (icrd = 0; icrd < 3; ++icrd) {
                    x1[icrd] = system->xr_p[system->map_p2s[iat][0]][icrd];
                    x2[icrd] = system->xr_p[system->map_p2s[jat][0]][icrd];
                }

                rotvec(xtmp, x1, S_frac_inv);
                for (icrd = 0; icrd < 3; ++icrd) {
                    xtmp[icrd] = xtmp[icrd] - x2[icrd];
                }

                auto phase = 2.0 * pi * (k[0] * xtmp[0] + k[1] * xtmp[1] + k[2] * xtmp[2]);

                for (icrd = 0; icrd < 3; ++icrd) {
                    for (jcrd = 0; jcrd < 3; ++jcrd) {
                        gamma_tmp[3 * iat + icrd][3 * jat + jcrd]
                                = S_cart[icrd][jcrd] * std::exp(im * phase);
                    }
                }
            }

            for (is = 0; is < ns; ++is) {
                for (js = 0; js < ns; ++js) {
                    mat_transform_sym[ik][isym][is][js] = gamma_tmp[is][js];
                }
            }

            ++isym;
        }
    }

    deallocate(gamma_tmp);
    deallocate(flag);
}

void Scph::symmetrize_dynamical_matrix(const unsigned int ik,
                                       Eigen::MatrixXcd &dymat) const
{
    // Symmetrize the dynamical matrix of given index ik.
    using namespace Eigen;
    unsigned int i, isym;
    unsigned int is, js;
    const auto ns = dynamical->neval;
    MatrixXcd dymat_sym = MatrixXcd::Zero(ns, ns);
    MatrixXcd dymat_tmp(ns, ns), gamma(ns, ns);

    const auto nsym_small = kmesh_coarse->small_group_of_k[ik].size();
    const auto nsym_minus = symop_minus_at_k[ik].size();

    for (i = 0; i < nsym_minus; ++i) {
        isym = symop_minus_at_k[ik][i];

        for (is = 0; is < ns; ++is) {
            for (js = 0; js < ns; ++js) {
                gamma(is, js) = mat_transform_sym[ik][isym][is][js];
            }
        }

        dymat_tmp = gamma * dymat * gamma.transpose().conjugate();
        dymat_sym += dymat_tmp.conjugate();
    }

    for (i = 0; i < nsym_small; ++i) {
        isym = kmesh_coarse->small_group_of_k[ik][i];

        for (is = 0; is < ns; ++is) {
            for (js = 0; js < ns; ++js) {
                gamma(is, js) = mat_transform_sym[ik][isym][is][js];
            }
        }

        dymat_tmp = gamma * dymat * gamma.transpose().conjugate();
        dymat_sym += dymat_tmp;
    }

    dymat = dymat_sym / static_cast<double>(nsym_small + nsym_minus);
}

void Scph::replicate_dymat_for_all_kpoints(std::complex<double> ***dymat_inout) const
{
    using namespace Eigen;
    unsigned int i;
    unsigned int is, js;
    const auto ns = dynamical->neval;
    MatrixXcd dymat_tmp(ns, ns), gamma(ns, ns), dymat(ns, ns);

    std::complex<double> ***dymat_all;

    allocate(dymat_all, ns, ns, kmesh_coarse->nk);

    for (i = 0; i < kmesh_coarse->nk; ++i) {

        const auto ik_irred = kpoint_map_symmetry[i].knum_irred_orig;
        const auto ik_orig = kpoint_map_symmetry[i].knum_orig;
        const auto isym = kpoint_map_symmetry[i].symmetry_op;

        for (is = 0; is < ns; ++is) {
            for (js = 0; js < ns; ++js) {
                gamma(is, js) = mat_transform_sym[ik_irred][isym][is][js];
                dymat(is, js) = dymat_inout[is][js][ik_orig];
            }
        }
        dymat_tmp = gamma * dymat * gamma.transpose().conjugate();

        for (is = 0; is < ns; ++is) {
            for (js = 0; js < ns; ++js) {
                dymat_all[is][js][i] = dymat_tmp(is, js);
            }
        }
    }

    for (is = 0; is < ns; ++is) {
        for (js = 0; js < ns; ++js) {
            for (i = 0; i < kmesh_coarse->nk; ++i) {
                dymat_inout[is][js][i] = dymat_all[is][js][i];
            }
        }
    }
    deallocate(dymat_all);
}

void Scph::setup_eigvecs()
{
    const auto ns = dynamical->neval;

    if (mympi->my_rank == 0) {
        std::cout << std::endl
                  << " Diagonalizing dynamical matrices for all k points ... ";
    }

    allocate(evec_harmonic, kmesh_dense->nk, ns, ns);
    allocate(omega2_harmonic, kmesh_dense->nk, ns);

    // Calculate phonon eigenvalues and eigenvectors for all k-points for scph

#pragma omp parallel for
    for (int ik = 0; ik < kmesh_dense->nk; ++ik) {

        dynamical->eval_k(kmesh_dense->xk[ik],
                          kmesh_dense->kvec_na[ik],
                          fcs_phonon->fc2_ext,
                          omega2_harmonic[ik],
                          evec_harmonic[ik], true);
    }

    if (mympi->my_rank == 0) {
        std::cout << "done !" << std::endl;
    }
}

void Scph::setup_pp_interaction()
{
    // Prepare information for calculating ph-ph interaction coefficients.

    if (mympi->my_rank == 0) {
        if (relax_coordinate || bubble > 0) {
            std::cout << " Preparing for calculating V3 & V4  ...";
        } else {
            std::cout << " Preparing for calculating V4  ...";
        }
    }

    if (anharmonic_core->quartic_mode != 1) {
        exit("setup_pp_interaction",
             "quartic_mode should be 1 for SCPH");
    }

    // Setup for V3 if relax_coordinate = True.
    if (relax_coordinate || bubble > 0) {
        allocate(phi3_reciprocal, anharmonic_core->get_ngroup_fcs(3));
    }
    allocate(phi4_reciprocal, anharmonic_core->get_ngroup_fcs(4));

    phase_factor_scph = new PhaseFactorStorage(kmesh_dense->nk_i);
    phase_factor_scph->create(true);

    if (mympi->my_rank == 0) {
        std::cout << " done!" << std::endl;
    }
}

void Scph::setup_transform_ifc()
{
    // Compute mindist_list_scph necessary to calculate dynamical matrix
    // from the real-space force constants

    int i, j;
    int ix, iy, iz;
    const auto nk = kmesh_coarse->nk;
    const auto nat = system->natmin;
    unsigned int iat;

    int **shift_cell, **shift_cell_super;
    double **xf_p;
    double ****x_all;

    const int nkx = static_cast<int>(kmesh_coarse->nk_i[0]); // This should be int (must not be unsigned int).
    const int nky = static_cast<int>(kmesh_coarse->nk_i[1]); // same as above
    const int nkz = static_cast<int>(kmesh_coarse->nk_i[2]); // same as above

    const auto ncell = nk;
    const auto ncell_s = 27;

    allocate(shift_cell, ncell, 3);
    allocate(shift_cell_super, ncell_s, 3);
    allocate(xf_p, nat, 3);
    allocate(x_all, ncell_s, ncell, nat, 3);

    unsigned int icell = 0;
    for (ix = 0; ix < nkx; ++ix) {
        for (iy = 0; iy < nky; ++iy) {
            for (iz = 0; iz < nkz; ++iz) {

                shift_cell[icell][0] = ix;
                shift_cell[icell][1] = iy;
                shift_cell[icell][2] = iz;

                ++icell;
            }
        }
    }

    for (i = 0; i < 3; ++i) shift_cell_super[0][i] = 0;
    icell = 1;
    for (ix = -1; ix <= 1; ++ix) {
        for (iy = -1; iy <= 1; ++iy) {
            for (iz = -1; iz <= 1; ++iz) {
                if (ix == 0 && iy == 0 && iz == 0) continue;

                shift_cell_super[icell][0] = ix;
                shift_cell_super[icell][1] = iy;
                shift_cell_super[icell][2] = iz;

                ++icell;
            }
        }
    }

    for (i = 0; i < nat; ++i) {
        rotvec(xf_p[i], system->xr_s_no_displace[system->map_p2s[i][0]], system->lavec_s);
        // rotvec(xf_p[i], system->xr_s[system->map_p2s[i][0]], system->lavec_s);
        rotvec(xf_p[i], xf_p[i], system->rlavec_p);
        for (j = 0; j < 3; ++j) xf_p[i][j] /= 2.0 * pi;
    }

    for (i = 0; i < ncell_s; ++i) {
        for (j = 0; j < ncell; ++j) {
            for (iat = 0; iat < nat; ++iat) {
                x_all[i][j][iat][0] = xf_p[iat][0] + static_cast<double>(shift_cell[j][0])
                                      + static_cast<double>(nkx * shift_cell_super[i][0]);
                x_all[i][j][iat][1] = xf_p[iat][1] + static_cast<double>(shift_cell[j][1])
                                      + static_cast<double>(nky * shift_cell_super[i][1]);
                x_all[i][j][iat][2] = xf_p[iat][2] + static_cast<double>(shift_cell[j][2])
                                      + static_cast<double>(nkz * shift_cell_super[i][2]);

                rotvec(x_all[i][j][iat], x_all[i][j][iat], system->lavec_p);
            }
        }
    }

    double dist;
    std::vector<DistList> dist_tmp;
    ShiftCell shift_tmp{};
    std::vector<int> vec_tmp;

    allocate(mindist_list_scph, nat, nat, ncell);

    for (iat = 0; iat < nat; ++iat) {
        for (unsigned int jat = 0; jat < nat; ++jat) {
            for (icell = 0; icell < ncell; ++icell) {

                dist_tmp.clear();
                for (i = 0; i < ncell_s; ++i) {
                    dist = distance(x_all[0][0][iat], x_all[i][icell][jat]);
                    dist_tmp.emplace_back(i, dist);
                }
                std::sort(dist_tmp.begin(), dist_tmp.end());

                const auto dist_min = dist_tmp[0].dist;
                mindist_list_scph[iat][jat][icell].dist = dist_min;

                for (i = 0; i < ncell_s; ++i) {
                    dist = dist_tmp[i].dist;

                    if (std::abs(dist_min - dist) < eps8) {

                        shift_tmp.sx = shift_cell[icell][0]
                                       + nkx * shift_cell_super[dist_tmp[i].cell_s][0];
                        shift_tmp.sy = shift_cell[icell][1]
                                       + nky * shift_cell_super[dist_tmp[i].cell_s][1];
                        shift_tmp.sz = shift_cell[icell][2]
                                       + nkz * shift_cell_super[dist_tmp[i].cell_s][2];

                        mindist_list_scph[iat][jat][icell].shift.push_back(shift_tmp);
                    }
                }

            }
        }
    }

    deallocate(shift_cell);
    deallocate(shift_cell_super);
    deallocate(xf_p);
    deallocate(x_all);
}

void Scph::exec_interpolation(const unsigned int kmesh_orig[3],
                              std::complex<double> ***dymat_r,
                              const unsigned int nk_dense,
                              double **xk_dense,
                              double **kvec_dense,
                              double **eval_out,
                              std::complex<double> ***evec_out)
{
    unsigned int i, j, is;
    const auto ns = dynamical->neval;
    const auto nk1 = kmesh_orig[0];
    const auto nk2 = kmesh_orig[1];
    const auto nk3 = kmesh_orig[2];

    double *eval_real;
    std::complex<double> **mat_tmp;
    std::complex<double> **mat_harmonic, **mat_harmonic_na;
    std::vector<double> eval_vec(ns);

    allocate(mat_tmp, ns, ns);
    allocate(eval_real, ns);
    allocate(mat_harmonic, ns, ns);

    if (dynamical->nonanalytic) {
        allocate(mat_harmonic_na, ns, ns);
    }

    for (int ik = 0; ik < nk_dense; ++ik) {

        if (dynamical->nonanalytic == 3) {
            dynamical->calc_analytic_k(xk_dense[ik],
                                       ewald->fc2_without_dipole,
                                       mat_harmonic);
        } else {
            dynamical->calc_analytic_k(xk_dense[ik],
                                       fcs_phonon->fc2_ext,
                                       mat_harmonic);
        }

        r2q(xk_dense[ik], nk1, nk2, nk3, ns, dymat_r, mat_tmp);

        for (i = 0; i < ns; ++i) {
            for (j = 0; j < ns; ++j) {
                mat_tmp[i][j] += mat_harmonic[i][j];
            }
        }

        if (dynamical->nonanalytic) {

            if (dynamical->nonanalytic == 1) {
                dynamical->calc_nonanalytic_k(xk_dense[ik],
                                              kvec_dense[ik],
                                              mat_harmonic_na);
            } else if (dynamical->nonanalytic == 2) {
                dynamical->calc_nonanalytic_k2(xk_dense[ik],
                                               kvec_dense[ik],
                                               mat_harmonic_na);

            } else if (dynamical->nonanalytic == 3) {
                ewald->add_longrange_matrix(xk_dense[ik],
                                            kvec_dense[ik],
                                            mat_harmonic_na);
            }

            for (i = 0; i < ns; ++i) {
                for (j = 0; j < ns; ++j) {
                    mat_tmp[i][j] += mat_harmonic_na[i][j];
                }
            }
        }

        // debug
        // std::cout << "total dynamical matrix" << std::endl;
        // for(i = 0; i < ns; i++){
        //     for(j = 0; j < ns; j++){
        //         std::cout << mat_tmp[i][j] << " ";
        //     }std::cout << std::endl;
        // }std::cout << std::endl;


        diagonalize_interpolated_matrix(mat_tmp, eval_real, evec_out[ik], true);

        for (is = 0; is < ns; ++is) {
            const auto eval_tmp = eval_real[is];

            if (eval_tmp < 0.0) {
                eval_vec[is] = -std::sqrt(-eval_tmp);
            } else {
                eval_vec[is] = std::sqrt(eval_tmp);
            }
        }

        for (is = 0; is < ns; ++is) eval_out[ik][is] = eval_vec[is];

    }

    deallocate(eval_real);
    deallocate(mat_tmp);
    deallocate(mat_harmonic);

    if (dynamical->nonanalytic) {
        deallocate(mat_harmonic_na);
    }
}

void Scph::r2q(const double *xk_in,
               const unsigned int nx,
               const unsigned int ny,
               const unsigned int nz,
               const unsigned int ns,
               std::complex<double> ***dymat_r_in,
               std::complex<double> **dymat_k_out) const
{
    std::complex<double> im(0.0, 1.0);

    const auto ncell = nx * ny * nz;

    for (unsigned int i = 0; i < ns; ++i) {

        const auto iat = i / 3;

        for (unsigned int j = 0; j < ns; ++j) {

            const auto jat = j / 3;

            dymat_k_out[i][j] = std::complex<double>(0.0, 0.0);

            for (unsigned int icell = 0; icell < ncell; ++icell) {

                auto exp_phase = std::complex<double>(0.0, 0.0);

                // This operation is necessary for the Hermiticity of the dynamical matrix.
                for (const auto &it: mindist_list_scph[iat][jat][icell].shift) {

                    auto phase = 2.0 * pi
                                 * (static_cast<double>(it.sx) * xk_in[0]
                                    + static_cast<double>(it.sy) * xk_in[1]
                                    + static_cast<double>(it.sz) * xk_in[2]);

                    exp_phase += std::exp(im * phase);
                }
                exp_phase /= static_cast<double>(mindist_list_scph[iat][jat][icell].shift.size());

                dymat_k_out[i][j] += dymat_r_in[i][j][icell] * exp_phase;
            }

        }
    }
}

void Scph::diagonalize_interpolated_matrix(std::complex<double> **mat_in,
                                           double *eval_out,
                                           std::complex<double> **evec_out,
                                           const bool require_evec) const
{
    unsigned int i, j;
    char JOBZ;
    int INFO;
    double *RWORK;
    std::complex<double> *amat;
    std::complex<double> *WORK;

    int ns = dynamical->neval;

    int LWORK = (2 * ns - 1) * 10;
    allocate(RWORK, 3 * ns - 2);
    allocate(WORK, LWORK);

    if (require_evec) {
        JOBZ = 'V';
    } else {
        JOBZ = 'N';
    }

    char UPLO = 'U';

    allocate(amat, ns * ns);

    unsigned int k = 0;
    for (j = 0; j < ns; ++j) {
        for (i = 0; i < ns; ++i) {
            amat[k++] = mat_in[i][j];
        }
    }

    zheev_(&JOBZ, &UPLO, &ns, amat, &ns, eval_out, WORK, &LWORK, RWORK, &INFO);

    k = 0;

    if (require_evec) {
        // Here we transpose the matrix evec_out so that 
        // evec_out[i] becomes phonon eigenvector of i-th mode.
        for (j = 0; j < ns; ++j) {
            for (i = 0; i < ns; ++i) {
                evec_out[j][i] = amat[k++];
            }
        }
    }

    deallocate(amat);
    deallocate(WORK);
    deallocate(RWORK);
}

void Scph::find_degeneracy(std::vector<int> *degeneracy_out,
                           const unsigned int nk_in,
                           double **eval_in) const
{
    // eval is omega^2 in atomic unit

    const auto ns = dynamical->neval;
    const auto tol_omega = 1.0e-7;

    for (unsigned int ik = 0; ik < nk_in; ++ik) {

        degeneracy_out[ik].clear();

        auto omega_prev = eval_in[ik][0];
        auto ideg = 1;

        for (unsigned int is = 1; is < ns; ++is) {
            const auto omega_now = eval_in[ik][is];

            if (std::abs(omega_now - omega_prev) < tol_omega) {
                ++ideg;
            } else {
                degeneracy_out[ik].push_back(ideg);
                ideg = 1;
                omega_prev = omega_now;
            }

        }
        degeneracy_out[ik].push_back(ideg);
    }
}

void Scph::calc_new_dymat_with_evec(std::complex<double> ***dymat_out,
                                    double **omega2_in,
                                    std::complex<double> ***evec_in)
{
    std::complex<double> *polarization_matrix, *mat_tmp;
    std::complex<double> *eigval_matrix, *dmat;
    std::complex<double> *beta;
    std::complex<double> ***dymat_q, **dymat_harmonic;
    std::complex<double> im(0.0, 1.0);

    unsigned int ik, is, js;
    int ns = dynamical->neval;

    const unsigned int ns2 = ns * ns;

    auto alpha = std::complex<double>(1.0, 0.0);

    char TRANSA[] = "N";
    char TRANSB[] = "C";

    allocate(polarization_matrix, ns2);
    allocate(mat_tmp, ns2);
    allocate(eigval_matrix, ns2);
    allocate(beta, ns);
    allocate(dmat, ns2);
    allocate(dymat_q, ns, ns, kmesh_coarse->nk);
    allocate(dymat_harmonic, ns, ns);

    for (is = 0; is < ns; ++is) beta[is] = std::complex<double>(0.0, 0.0);

    for (ik = 0; ik < kmesh_coarse->nk; ++ik) {

        const auto knum = kmap_interpolate_to_scph[ik];

        // create eigval matrix

        for (is = 0; is < ns2; ++is) eigval_matrix[is] = std::complex<double>(0.0, 0.0);

        unsigned int m = 0;
        for (is = 0; is < ns; ++is) {
            for (js = 0; js < ns; ++js) {
                if (is == js) {
                    eigval_matrix[m] = omega2_in[knum][is];
                }
                ++m;
            }
        }

        // create polarization matrix

        m = 0;

        for (is = 0; is < ns; ++is) {
            for (js = 0; js < ns; ++js) {
                polarization_matrix[m++] = evec_in[knum][is][js];
            }
        }

        zgemm_(TRANSA, TRANSB, &ns, &ns, &ns, &alpha,
               eigval_matrix, &ns, polarization_matrix, &ns, beta, mat_tmp, &ns);
        zgemm_(TRANSA, TRANSA, &ns, &ns, &ns, &alpha,
               polarization_matrix, &ns, mat_tmp, &ns, beta, dmat, &ns);

        m = 0;

        for (js = 0; js < ns; ++js) {
            for (is = 0; is < ns; ++is) {
                dymat_q[is][js][ik] = dmat[m];
                ++m;
            }
        }


        // Subtract harmonic contribution
        dynamical->calc_analytic_k(kmesh_coarse->xk[ik],
                                   fcs_phonon->fc2_ext,
                                   dymat_harmonic);

        for (is = 0; is < ns; ++is) {
            for (js = 0; js < ns; ++js) {
                dymat_q[is][js][ik] -= dymat_harmonic[is][js];
            }
        }
    }

    deallocate(polarization_matrix);
    deallocate(mat_tmp);
    deallocate(eigval_matrix);
    deallocate(beta);
    deallocate(dmat);
    deallocate(dymat_harmonic);

    const auto nk1 = kmesh_interpolate[0];
    const auto nk2 = kmesh_interpolate[1];
    const auto nk3 = kmesh_interpolate[2];

    std::vector<std::vector<double>> xk_dup;

    int icell = 0;

    for (int ix = 0; ix < nk1; ++ix) {
        for (int iy = 0; iy < nk2; ++iy) {
            for (int iz = 0; iz < nk3; ++iz) {

                for (is = 0; is < ns; ++is) {
                    for (js = 0; js < ns; ++js) {
                        dymat_out[is][js][icell] = std::complex<double>(0.0, 0.0);
                    }
                }

                for (ik = 0; ik < kmesh_coarse->nk; ++ik) {

                    duplicate_xk_boundary(kmesh_coarse->xk[ik], xk_dup);

                    auto cexp_phase = std::complex<double>(0.0, 0.0);

                    for (const auto &i: xk_dup) {

                        auto phase = 2.0 * pi * (i[0] * static_cast<double>(ix)
                                                 + i[1] * static_cast<double>(iy)
                                                 + i[2] * static_cast<double>(iz));
                        cexp_phase += std::exp(-im * phase);

                    }
                    cexp_phase /= static_cast<double>(xk_dup.size());

                    for (is = 0; is < ns; ++is) {
                        for (js = 0; js < ns; ++js) {
                            dymat_out[is][js][icell] += dymat_q[is][js][ik] * cexp_phase;
                        }
                    }

                }
                for (is = 0; is < ns; ++is) {
                    for (js = 0; js < ns; ++js) {
                        dymat_out[is][js][icell] /= static_cast<double>(kmesh_coarse->nk);
                    }
                }

                ++icell;
            }
        }
    }

    deallocate(dymat_q);
}

void Scph::compute_anharmonic_frequency(std::complex<double> ***v4_array_all,
                                        double **omega2_out,
                                        std::complex<double> ***evec_anharm_scph,
                                        const double temp,
                                        bool &flag_converged,
                                        std::complex<double> ***cmat_convert,
                                        const bool offdiag,
                                        std::complex<double> **delta_v2_array_renormalize,
                                        const unsigned int verbosity)
{
    // This is the main function of the SCPH equation.
    // The detailed algorithm can be found in PRB 92, 054301 (2015).
    // Eigen3 library is used for the compact notation of matrix-matrix products.

    using namespace Eigen;

    int ik, jk;
    unsigned int i;
    unsigned int is, js, ks;
    unsigned int kk;
    const auto nk = kmesh_dense->nk;
    const auto ns = dynamical->neval;
    unsigned int knum, knum_interpolate;
    const auto nk_interpolate = kmesh_coarse->nk;
    const auto nk_irred_interpolate = kmesh_coarse->nk_irred;
    const auto nk1 = kmesh_interpolate[0];
    const auto nk2 = kmesh_interpolate[1];
    const auto nk3 = kmesh_interpolate[2];
    int iloop;

    MatrixXd omega_now(nk, ns), omega_old(nk, ns);
    MatrixXd omega2_HA(nk, ns);
    MatrixXcd mat_tmp(ns, ns), evec_tmp(ns, ns);

    VectorXd eval_tmp(ns);
    MatrixXcd Dymat(ns, ns);
    MatrixXcd Fmat(ns, ns);
    MatrixXcd Qmat = MatrixXcd::Zero(ns, ns);
    MatrixXcd Cmat(ns, ns), Dmat(ns, ns);

    double diff;
    double conv_tol = tolerance_scph;
    double alpha = mixalpha;

    double **eval_interpolate;
    double re_tmp, im_tmp;
    bool has_negative;

    std::complex<double> ctmp;
    std::complex<double> ***mat_omega2_harmonic;
    std::complex<double> ***evec_initial;
    std::complex<double> ***dmat_convert;
    std::complex<double> ***dmat_convert_old;
    std::complex<double> ***evec_new;
    std::complex<double> ***dymat_new, ***dymat_harmonic;
    std::complex<double> ***dymat_q;
    std::complex<double> ***Fmat0;

    const auto complex_one = std::complex<double>(1.0, 0.0);
    const auto complex_zero = std::complex<double>(0.0, 0.0);

    SelfAdjointEigenSolver<MatrixXcd> saes;

    allocate(mat_omega2_harmonic, nk_interpolate, ns, ns);
    allocate(eval_interpolate, nk, ns);
    allocate(evec_initial, nk, ns, ns);
    allocate(evec_new, nk, ns, ns);
    allocate(dmat_convert, nk, ns, ns);
    allocate(dmat_convert_old, nk, ns, ns);
    allocate(dymat_new, ns, ns, nk_interpolate);
    allocate(dymat_q, ns, ns, nk_interpolate);
    allocate(dymat_harmonic, nk_interpolate, ns, ns);
    allocate(Fmat0, nk_irred_interpolate, ns, ns);

    const auto T_in = temp;

    std::cout << " Temperature = " << T_in << " K" << std::endl;

    // Set initial values

    for (ik = 0; ik < nk; ++ik) {
        for (is = 0; is < ns; ++is) {

            if (flag_converged) {
                if (omega2_out[ik][is] < 0.0 && std::abs(omega2_out[ik][is]) > 1.0e-16 && verbosity > 0) {
                    std::cout << "Warning : Large negative frequency detected" << std::endl;
                }

                if (omega2_out[ik][is] < 0.0) {
                    omega_now(ik, is) = std::sqrt(-omega2_out[ik][is]);
                } else {
                    omega_now(ik, is) = std::sqrt(omega2_out[ik][is]);
                }
            } else {
                if (omega2_harmonic[ik][is] < 0.0) {
                    omega_now(ik, is) = std::sqrt(-omega2_harmonic[ik][is]);
                } else {
                    omega_now(ik, is) = std::sqrt(omega2_harmonic[ik][is]);
                }
            }

            omega2_HA(ik, is) = omega2_harmonic[ik][is];

            for (js = 0; js < ns; ++js) {
                evec_initial[ik][is][js] = evec_harmonic[ik][is][js];

                if (!flag_converged) {
                    // Initialize Cmat with identity matrix
                    if (is == js) {
                        cmat_convert[ik][is][js] = complex_one;
                    } else {
                        cmat_convert[ik][is][js] = complex_zero;
                    }
                }

            }
        }
    }

    // Set initial harmonic dymat and eigenvalues

    for (ik = 0; ik < nk_interpolate; ++ik) {
        knum = kmap_interpolate_to_scph[ik];

        for (is = 0; is < ns; ++is) {
            for (js = 0; js < ns; ++js) {
                mat_omega2_harmonic[ik][is][js] = complex_zero;
            }
            mat_omega2_harmonic[ik][is][is] = std::complex<double>(omega2_HA(knum, is), 0.0);
            eval_interpolate[knum][is] = omega2_HA(knum, is);
        }

        dynamical->calc_analytic_k(kmesh_coarse->xk[ik],
                                   fcs_phonon->fc2_ext,
                                   dymat_harmonic[ik]);
    }

    for (ik = 0; ik < nk_irred_interpolate; ++ik) {

        knum_interpolate = kmesh_coarse->kpoint_irred_all[ik][0].knum;

        // Fmat harmonic
        for (is = 0; is < ns; ++is) {
            for (js = 0; js < ns; ++js) {
                if (is == js) {
                    Fmat0[ik][is][js] = mat_omega2_harmonic[knum_interpolate][is][is];
                } else {
                    Fmat0[ik][is][js] = complex_zero;
                }
                Fmat0[ik][is][js] += delta_v2_array_renormalize[knum_interpolate][is*ns+js];
            }
        }
    }

    int icount = 0;

    // Main loop
    for (iloop = 0; iloop < maxiter; ++iloop) {

        for (ik = 0; ik < nk; ++ik) {
            for (is = 0; is < ns; ++is) {
                auto omega1 = omega_now(ik, is);
                if (std::abs(omega1) < eps8) {
                    Qmat(is, is) = complex_zero;
                } else {
                    // Note that the missing factor 2 in the denominator of Qmat is 
                    // already considered in the v4_array_all.
                    if (thermodynamics->classical) {
                        Qmat(is, is) = std::complex<double>(2.0 * T_in * thermodynamics->T_to_Ryd / (omega1 * omega1),
                                                            0.0);
                    } else {
                        auto n1 = thermodynamics->fB(omega1, T_in);
                        Qmat(is, is) = std::complex<double>((2.0 * n1 + 1.0) / omega1, 0.0);
                    }
                }
            }

            for (is = 0; is < ns; ++is) {
                for (js = 0; js < ns; ++js) {
                    Cmat(is, js) = cmat_convert[ik][is][js];
                }
            }

            Dmat = Cmat * Qmat * Cmat.adjoint();

            for (is = 0; is < ns; ++is) {
                for (js = 0; js < ns; ++js) {
                    dmat_convert[ik][is][js] = Dmat(is, js);
                }
            }
        }

        // Mixing dmat
        if (iloop > 0) {
#pragma omp parallel for private(is, js)
            for (ik = 0; ik < nk; ++ik) {
                for (is = 0; is < ns; ++is) {
                    for (js = 0; js < ns; ++js) {
                        dmat_convert[ik][is][js] = alpha * dmat_convert[ik][is][js]
                                                   + (1.0 - alpha) * dmat_convert_old[ik][is][js];
                    }
                }
            }
        }

        for (ik = 0; ik < nk_irred_interpolate; ++ik) {

            knum_interpolate = kmesh_coarse->kpoint_irred_all[ik][0].knum;
            knum = kmap_interpolate_to_scph[knum_interpolate];

            // Fmat harmonic

            for (is = 0; is < ns; ++is) {
                for (js = 0; js < ns; ++js) {
                    Fmat(is, js) = Fmat0[ik][is][js];
                }
            }

            // Anharmonic correction to Fmat

            if (!offdiag) {
                for (is = 0; is < ns; ++is) {
                    i = (ns + 1) * is;

                    re_tmp = 0.0;
                    im_tmp = 0.0;

#pragma omp parallel for private(jk, kk, ks, ctmp), reduction(+:re_tmp, im_tmp)
                    for (jk = 0; jk < nk; ++jk) {

                        kk = nk * ik + jk;

                        for (ks = 0; ks < ns; ++ks) {
                            ctmp = v4_array_all[kk][i][(ns + 1) * ks]
                                   * dmat_convert[jk][ks][ks];
                            re_tmp += ctmp.real();
                            im_tmp += ctmp.imag();
                        }
                    }
                    Fmat(is, is) += std::complex<double>(re_tmp, im_tmp);
                }
            } else {

                // Anharmonic correction to Fmat

                for (is = 0; is < ns; ++is) {
                    for (js = 0; js <= is; ++js) {

                        i = ns * is + js;

                        re_tmp = 0.0;
                        im_tmp = 0.0;

#pragma omp parallel for private(jk, kk, ks, ctmp), reduction(+:re_tmp, im_tmp)
                        for (jk = 0; jk < nk; ++jk) {

                            kk = nk * ik + jk;

                            for (ks = 0; ks < ns; ++ks) {
                                for (unsigned int ls = 0; ls < ns; ++ls) {
                                    ctmp = v4_array_all[kk][i][ns * ks + ls]
                                           * dmat_convert[jk][ks][ls];
                                    re_tmp += ctmp.real();
                                    im_tmp += ctmp.imag();
                                }
                            }
                        }
                        Fmat(is, js) += std::complex<double>(re_tmp, im_tmp);
                    }
                }
            }

            saes.compute(Fmat);
            eval_tmp = saes.eigenvalues();

            for (is = 0; is < ns; ++is) {

                double omega2_tmp = eval_tmp(is);

                if (omega2_tmp < 0.0 && std::abs(omega2_tmp) > 1.0e-16) {

                    if (verbosity > 1) {
                        std::cout << " Detect imaginary : ";
                        std::cout << "  knum = " << knum + 1 << " is = " << is + 1 << '\n';
                        for (int j = 0; j < 3; ++j) {
                            std::cout << "  xk = " << std::setw(15) << kmesh_dense->xk[knum][j];
                        }
                        std::cout << '\n';
                    }

                    if (v4_array_all[nk * ik + knum][(ns + 1) * is][(ns + 1) * is].real() > 0.0) {
                        if (verbosity > 1) {
                            std::cout << "  onsite V4 is positive\n\n";
                        }

                        if (flag_converged) {
                            ++icount;
                            eval_tmp(is) = omega2_out[knum][is] * std::pow(0.99, icount);
                        } else {
                            ++icount;
                            eval_tmp(is) = -eval_tmp(is) * std::pow(0.99, icount);
                        }
                    } else {
                        if (verbosity > 1) {
                            std::cout << "  onsite V4 is negative\n\n";
                        }
                        eval_tmp(is) = std::abs(omega2_tmp);
                    }
                }
            }

            for (is = 0; is < ns; ++is) {
                for (js = 0; js < ns; ++js) {
                    evec_tmp(is, js) = evec_initial[knum][is][js];

                    if (is == js) {
                        Dymat(is, js) = std::complex<double>(eval_tmp(is), 0.0);
                    } else {
                        Dymat(is, js) = complex_zero;
                    }
                }
            }

            // New eigenvector matrix E_{new}= E_{old} * C
            mat_tmp = evec_tmp.transpose() * saes.eigenvectors();
            Dymat = mat_tmp * Dymat * mat_tmp.adjoint();

#ifdef _DEBUG2
            Dymat_sym = Dymat;
            symmetrize_dynamical_matrix(ik, Dymat_sym);
            std::complex<double> **dymat_exact;
            allocate(dymat_exact, ns, ns);
            std::cout << "ik = " << ik + 1 << std::endl;
            std::cout << "Dymat" << std::endl;
            std::cout << Dymat << std::endl;
            std::cout << "Dymat_sym" << std::endl;
            std::cout << Dymat_sym << std::endl;
            dynamical->calc_analytic_k(xk_interpolate[knum_interpolate], fcs_phonon->fc2_ext, dymat_exact);
            for (is = 0; is < ns; ++is) {
                for (js = 0; js < ns; ++js) {
                    Dymat_sym(is,js) = dymat_exact[is][js];
                }
            }
            std::cout << "Dymat_exact" << std::endl;
            std::cout << Dymat_sym << std::endl;
            deallocate(dymat_exact);

#endif
            symmetrize_dynamical_matrix(ik, Dymat);

            for (is = 0; is < ns; ++is) {
                for (js = 0; js < ns; ++js) {
                    dymat_q[is][js][knum_interpolate] = Dymat(is, js);
                }
            }
        } // close loop ik

        replicate_dymat_for_all_kpoints(dymat_q);

#ifdef _DEBUG2
        for (ik = 0; ik < nk_interpolate; ++ik) {

            knum = kmap_interpolate_to_scph[ik];
                
            for (is = 0; is < ns; ++is) {
                for (js = 0; js < ns; ++js) {
                    Dymat(is,js) = dymat_q[is][js][ik];
                }
            }

            saes.compute(Dymat);
            eval_tmp = saes.eigenvalues();

            for (is = 0; is < ns; ++is) {
                eval_orig(is) = omega2_harmonic(knum,is);
            }

            std::cout << " ik = " << std::setw(4) << ik + 1 << " : ";
            for (i = 0; i < 3; ++i)  std::cout << std::setw(15) << xk_scph[knum][i];
            std::cout << std::endl;

            for (is = 0; is < ns; ++is) {
                std::cout << std::setw(15) << eval_tmp(is);
                std::cout << std::setw(15) << eval_orig(is);
                std::cout << std::setw(15) << eval_tmp(is) - eval_orig(is) << std::endl;
            }
            
        }
#endif

        // Subtract harmonic contribution to the dynamical matrix
        for (ik = 0; ik < nk_interpolate; ++ik) {
            for (is = 0; is < ns; ++is) {
                for (js = 0; js < ns; ++js) {
                    dymat_q[is][js][ik] -= dymat_harmonic[ik][is][js];
                }
            }
        }

        for (is = 0; is < ns; ++is) {
            for (js = 0; js < ns; ++js) {
                fftw_plan plan = fftw_plan_dft_3d(nk1, nk2, nk3,
                                                  reinterpret_cast<fftw_complex *>(dymat_q[is][js]),
                                                  reinterpret_cast<fftw_complex *>(dymat_new[is][js]),
                                                  FFTW_FORWARD, FFTW_ESTIMATE);
                fftw_execute(plan);
                fftw_destroy_plan(plan);

                for (ik = 0; ik < nk_interpolate; ++ik)
                    dymat_new[is][js][ik] /= static_cast<double>(nk_interpolate);
            }
        }

        exec_interpolation(kmesh_interpolate,
                           dymat_new,
                           nk,
                           kmesh_dense->xk,
                           kmesh_dense->kvec_na,
                           eval_interpolate, evec_new);

        for (ik = 0; ik < nk; ++ik) {

            for (is = 0; is < ns; ++is) {
                for (js = 0; js < ns; ++js) {
                    mat_tmp(is, js) = evec_initial[ik][js][is];
                    evec_tmp(is, js) = evec_new[ik][js][is];
                }
            }

            Cmat = mat_tmp.adjoint() * evec_tmp;

            for (is = 0; is < ns; ++is) {
                omega_now(ik, is) = eval_interpolate[ik][is];
                for (js = 0; js < ns; ++js) {
                    cmat_convert[ik][is][js] = Cmat(is, js);
                }
            }
        }

        if (iloop == 0) {
            if (verbosity > 0) {
                std::cout << "  SCPH ITER " << std::setw(5) << iloop + 1 << " :  DIFF = N/A" << std::endl;
            }

            for (ik = 0; ik < nk; ++ik) {
                for (is = 0; is < ns; ++is) {
                    omega_old(ik, is) = omega_now(ik, is);
                }
            }

        } else {

            diff = 0.0;

            for (ik = 0; ik < nk_interpolate; ++ik) {
                knum = kmap_interpolate_to_scph[ik];
                for (is = 0; is < ns; ++is) {
                    diff += std::pow(omega_now(knum, is) - omega_old(knum, is), 2.0);
                }
            }
            diff /= static_cast<double>(nk_interpolate * ns);
            if (verbosity > 0) {
                std::cout << "  SCPH ITER " << std::setw(5) << iloop + 1 << " : ";
                std::cout << " DIFF = " << std::setw(15) << std::sqrt(diff) << std::endl;
            }
            for (ik = 0; ik < nk; ++ik) {
                for (is = 0; is < ns; ++is) {
                    omega_old(ik, is) = omega_now(ik, is);
                }
            }
            if (std::sqrt(diff) < conv_tol) {
                has_negative = false;

                for (ik = 0; ik < nk_interpolate; ++ik) {
                    knum = kmap_interpolate_to_scph[ik];
                    for (is = 0; is < ns; ++is) {
                        if (omega_now(knum, is) < 0.0 && std::abs(omega_now(knum, is)) > eps8) {
                            has_negative = true;
                            break;
                        }
                    }
                }
                if (!has_negative) {
                    if (verbosity > 0) std::cout << "  DIFF < SCPH_TOL : break SCPH loop\n";
                    break;
                }
                if (verbosity > 0) std::cout << "  DIFF < SCPH_TOL but a negative frequency is detected.\n";
            }
        }

        for (ik = 0; ik < nk; ++ik) {
            for (is = 0; is < ns; ++is) {
                for (js = 0; js < ns; ++js) {
                    dmat_convert_old[ik][is][js] = dmat_convert[ik][is][js];
                }
            }
        }
    } // end loop iteration

    if (std::sqrt(diff) < conv_tol) {
        if (verbosity > 0) {
            std::cout << " Temp = " << T_in;
            std::cout << " : convergence achieved in " << std::setw(5)
                      << iloop + 1 << " iterations." << std::endl;
        }
        flag_converged = true;
    } else {
        if (verbosity > 0) {
            std::cout << "Temp = " << T_in;
            std::cout << " : not converged." << std::endl;
        }
        flag_converged = false;
    }

    for (ik = 0; ik < nk; ++ik) {
        for (is = 0; is < ns; ++is) {
            if (eval_interpolate[ik][is] < 0.0) {
                if (std::abs(eval_interpolate[ik][is]) <= eps10) {
                    omega2_out[ik][is] = 0.0;
                } else {
                    omega2_out[ik][is] = -std::pow(eval_interpolate[ik][is], 2.0);
                }
            } else {
                omega2_out[ik][is] = std::pow(eval_interpolate[ik][is], 2.0);
            }
            for (js = 0; js < ns; ++js) {
                evec_anharm_scph[ik][is][js] = evec_new[ik][is][js];
            }
        }
    }

    if (verbosity > 1) {
        std::cout << "New eigenvalues" << std::endl;
        for (ik = 0; ik < nk_interpolate; ++ik) {
            knum = kmap_interpolate_to_scph[ik];
            for (is = 0; is < ns; ++is) {
                std::cout << " ik_interpolate = " << std::setw(5) << ik + 1;
                std::cout << " is = " << std::setw(5) << is + 1;
                std::cout << " omega2 = " << std::setw(15) << omega2_out[knum][is] << std::endl;
            }
            std::cout << std::endl;
        }
    }

    deallocate(mat_omega2_harmonic);
    deallocate(eval_interpolate);
    deallocate(evec_initial);
    deallocate(dmat_convert);
    deallocate(dmat_convert_old);
    deallocate(evec_new);
    deallocate(dymat_new);
    deallocate(dymat_q);
    deallocate(dymat_harmonic);
    deallocate(Fmat0);
}


void Scph::compute_renormalized_harmonic_frequency(double **omega2_out,
                                        std::complex<double> ***evec_harm_renormalized,
                                        std::complex<double> **delta_v2_array_renormalize,
                                        const unsigned int verbosity)
{
    using namespace Eigen;

    int ik, jk;
    int is, js;
    const auto nk = kmesh_dense->nk;
    const auto nk_interpolate = kmesh_coarse->nk;
    const auto ns = dynamical->neval;
    unsigned int knum, knum_interpolate;
    const auto nk_irred_interpolate = kmesh_coarse->nk_irred;
    const auto nk1 = kmesh_interpolate[0];
    const auto nk2 = kmesh_interpolate[1];
    const auto nk3 = kmesh_interpolate[2];

    MatrixXcd evec_tmp(ns, ns);

    MatrixXcd Dymat(ns, ns);
    MatrixXcd Fmat(ns, ns);

    // double diff;
    // double conv_tol = tolerance_scph;
    // double alpha = mixalpha;

    double **eval_interpolate;
    double re_tmp, im_tmp;
    bool has_negative;

    std::complex<double> ***dymat_new, ***dymat_harmonic_without_renormalize;
    std::complex<double> ***dymat_q;

    const auto complex_one = std::complex<double>(1.0, 0.0);
    const auto complex_zero = std::complex<double>(0.0, 0.0);

    SelfAdjointEigenSolver <MatrixXcd> saes;

    allocate(eval_interpolate, nk, ns);
    allocate(dymat_new, ns, ns, nk_interpolate);
    allocate(dymat_q, ns, ns, nk_interpolate);
    allocate(dymat_harmonic_without_renormalize, nk_interpolate, ns, ns);

    // Set initial harmonic dymat without IFC renormalization

    for (ik = 0; ik < nk_interpolate; ++ik) {
        
        dynamical->calc_analytic_k(kmesh_coarse->xk[ik],
                                   fcs_phonon->fc2_ext,
                                   dymat_harmonic_without_renormalize[ik]);
    }
    
    for (ik = 0; ik < nk_irred_interpolate; ++ik) {

        knum_interpolate = kmesh_coarse->kpoint_irred_all[ik][0].knum;
        knum = kmap_interpolate_to_scph[knum_interpolate];

        // calculate Fmat
        for (is = 0; is < ns; ++is) {
            for (js = 0; js < ns; ++js) {
                if (is == js) {
                    Fmat(is, js) = omega2_harmonic[knum][is];
                } else {
                    Fmat(is, js) = complex_zero;
                }
                Fmat(is, js) += delta_v2_array_renormalize[knum_interpolate][is*ns+js];
            }
        }

        for (is = 0; is < ns; ++is) {
            for (js = 0; js < ns; ++js) {
                evec_tmp(is, js) = evec_harmonic[knum][js][is]; // transpose
            }
        }

        Dymat = evec_tmp * Fmat * evec_tmp.adjoint();

        symmetrize_dynamical_matrix(ik, Dymat);
        for (is = 0; is < ns; ++is) {
            for (js = 0; js < ns; ++js) {
                dymat_q[is][js][knum_interpolate] = Dymat(is, js);
            }
        }
    }

    replicate_dymat_for_all_kpoints(dymat_q);

    // Subtract harmonic contribution to the dynamical matrix
    for (ik = 0; ik < nk_interpolate; ++ik) {
        for (is = 0; is < ns; ++is) {
            for (js = 0; js < ns; ++js) {
                dymat_q[is][js][ik] -= dymat_harmonic_without_renormalize[ik][is][js];
            }
        }
    }

    for (is = 0; is < ns; ++is) {
        for (js = 0; js < ns; ++js) {
            fftw_plan plan = fftw_plan_dft_3d(nk1, nk2, nk3,
                                                reinterpret_cast<fftw_complex *>(dymat_q[is][js]),
                                                reinterpret_cast<fftw_complex *>(dymat_new[is][js]),
                                                FFTW_FORWARD, FFTW_ESTIMATE);
            fftw_execute(plan);
            fftw_destroy_plan(plan);

            for (ik = 0; ik < nk_interpolate; ++ik)
                dymat_new[is][js][ik] /= static_cast<double>(nk_interpolate);
        }
    }

    exec_interpolation(kmesh_interpolate,
                        dymat_new,
                        nk,
                        kmesh_dense->xk,
                        kmesh_dense->kvec_na,
                        eval_interpolate, evec_harm_renormalized);

    for (ik = 0; ik < nk; ++ik) {
        for (is = 0; is < ns; ++is) {
            if (eval_interpolate[ik][is] < 0.0) {
                if (std::abs(eval_interpolate[ik][is]) <= eps10) {
                    omega2_out[ik][is] = 0.0;
                } else {
                    omega2_out[ik][is] = -std::pow(eval_interpolate[ik][is], 2.0);
                }
            } else {
                omega2_out[ik][is] = std::pow(eval_interpolate[ik][is], 2.0);
            }
        }
    }


    deallocate(dymat_q);
    deallocate(dymat_new);

    deallocate(eval_interpolate);
    deallocate(dymat_harmonic_without_renormalize);
}

void Scph::compute_free_energy_bubble_SCPH(const unsigned int kmesh[3],
                                           std::complex<double> ****delta_dymat_scph)
{
    const auto NT = static_cast<unsigned int>((system->Tmax - system->Tmin) / system->dT) + 1;
    const auto nk_ref = dos->kmesh_dos->nk;
    const auto ns = dynamical->neval;
    double ***eval;
    std::complex<double> ****evec;

    if (mympi->my_rank == 0) {
        std::cout << std::endl;
        std::cout << " -----------------------------------------------------------------"
                  << std::endl;
        std::cout << " Calculating the vibrational free energy from the Bubble diagram " << std::endl;
        std::cout << " on top of the SCPH calculation." << std::endl;
        std::cout << '\n';
        std::cout << " This calculation requires allocation of additional memory:" << std::endl;

        size_t nsize = nk_ref * ns * ns * NT * sizeof(std::complex<double>)
                       + nk_ref * ns * NT * sizeof(double);

        const auto nsize_dble = static_cast<double>(nsize) / 100000000.0;
        std::cout << "  Estimated memory usage per MPI process: " << std::setw(10)
                  << std::fixed << std::setprecision(4) << nsize_dble << " GByte." << std::endl;

        std::cout << "  To avoid possible faults associated with insufficient memory,\n"
                     "  please reduce the number of MPI processes per node and/or\n"
                     "  the number of temperagure grids.\n\n";
    }

    allocate(thermodynamics->FE_bubble, NT);
    allocate(eval, NT, nk_ref, ns);
    allocate(evec, NT, nk_ref, ns, ns); // This requires lots of RAM

    for (auto iT = 0; iT < NT; ++iT) {
        exec_interpolation(kmesh,
                           delta_dymat_scph[iT],
                           nk_ref,
                           dos->kmesh_dos->xk,
                           dos->kmesh_dos->kvec_na,
                           eval[iT],
                           evec[iT]);
    }

    thermodynamics->compute_FE_bubble_SCPH(eval, evec, thermodynamics->FE_bubble);

    deallocate(eval);
    deallocate(evec);

    if (mympi->my_rank == 0) {
        std::cout << " done!" << std::endl << std::endl;
    }
}

void Scph::bubble_correction(std::complex<double> ****delta_dymat_scph,
                             std::complex<double> ****delta_dymat_scph_plus_bubble)
{
    const auto NT = static_cast<unsigned int>((system->Tmax - system->Tmin) / system->dT) + 1;
    const auto ns = dynamical->neval;

    auto epsilon = integration->epsilon;
    const auto nk_irred_interpolate = kmesh_coarse->nk_irred;
    const auto nk_scph = kmesh_dense->nk;

    double **eval = nullptr;
    double ***eval_bubble = nullptr;
    std::complex<double> ***evec;
    double *real_self = nullptr;
    std::vector<std::complex<double>> omegalist;

    if (mympi->my_rank == 0) {
        std::cout << std::endl;
        std::cout << " -----------------------------------------------------------------"
                  << std::endl;
        std::cout << " Calculating the bubble self-energy " << std::endl;
        std::cout << " on top of the SCPH calculation." << std::endl;
        std::cout << '\n';
    }

    allocate(eval, nk_scph, ns);
    allocate(evec, nk_scph, ns, ns);

    if (mympi->my_rank == 0) {
        allocate(eval_bubble, NT, nk_scph, ns);
        for (auto iT = 0; iT < NT; ++iT) {
            for (auto ik = 0; ik < nk_scph; ++ik) {
                for (auto is = 0; is < ns; ++is) {
                    eval_bubble[iT][ik][is] = 0.0;
                }
            }
        }
        allocate(real_self, ns);
    }

    std::vector<int> *degeneracy_at_k;
    allocate(degeneracy_at_k, nk_scph);

    for (auto iT = 0; iT < NT; ++iT) {
        const auto temp = system->Tmin + system->dT * float(iT);

        exec_interpolation(kmesh_interpolate,
                           delta_dymat_scph[iT],
                           nk_scph,
                           kmesh_dense->xk,
                           kmesh_dense->kvec_na,
                           eval,
                           evec);

        find_degeneracy(degeneracy_at_k,
                        nk_scph,
                        eval);

        if (mympi->my_rank == 0) std::cout << " Temperature (K) : " << std::setw(6) << temp << '\n';

        for (auto ik = 0; ik < nk_irred_interpolate; ++ik) {

            auto knum_interpolate = kmesh_coarse->kpoint_irred_all[ik][0].knum;
            auto knum = kmap_interpolate_to_scph[knum_interpolate];

            if (mympi->my_rank == 0) {
                std::cout << "  Irred. k: " << std::setw(5) << ik + 1 << " (";
                for (auto m = 0; m < 3; ++m) std::cout << std::setw(15) << kmesh_dense->xk[knum][m];
                std::cout << ")\n";
            }

            for (unsigned int snum = 0; snum < ns; ++snum) {

                if (eval[knum][snum] < eps8) {
                    if (mympi->my_rank == 0) real_self[snum] = 0.0;
                } else {
                    omegalist.clear();

                    if (bubble == 1) {

                        omegalist.push_back(im * epsilon);

                        auto se_bubble = get_bubble_selfenergy(kmesh_dense,
                                                               ns,
                                                               eval,
                                                               evec,
                                                               knum,
                                                               snum,
                                                               temp,
                                                               omegalist);

                        if (mympi->my_rank == 0) real_self[snum] = se_bubble[0].real();

                    } else if (bubble == 2) {

                        omegalist.push_back(eval[knum][snum] + im * epsilon);

                        auto se_bubble = get_bubble_selfenergy(kmesh_dense,
                                                               ns,
                                                               eval,
                                                               evec,
                                                               knum,
                                                               snum,
                                                               temp,
                                                               omegalist);

                        if (mympi->my_rank == 0) real_self[snum] = se_bubble[0].real();

                    } else if (bubble == 3) {

                        auto maxfreq = eval[knum][snum] + 50.0 * time_ry / Hz_to_kayser;
                        auto minfreq = eval[knum][snum] - 50.0 * time_ry / Hz_to_kayser;

                        if (minfreq < 0.0) minfreq = 0.0;

                        const auto domega = 0.1 * time_ry / Hz_to_kayser;
                        auto nomega = static_cast<unsigned int>((maxfreq - minfreq) / domega) + 1;

                        for (auto iomega = 0; iomega < nomega; ++iomega) {
                            omegalist.push_back(minfreq + static_cast<double>(iomega) * domega + im * epsilon);
                        }

                        auto se_bubble = get_bubble_selfenergy(kmesh_dense,
                                                               ns,
                                                               eval,
                                                               evec,
                                                               knum,
                                                               snum,
                                                               temp,
                                                               omegalist);

                        if (mympi->my_rank == 0) {

                            std::vector<double> nonlinear_func(nomega);
                            for (auto iomega = 0; iomega < nomega; ++iomega) {
                                nonlinear_func[iomega] = omegalist[iomega].real() * omegalist[iomega].real()
                                                         - eval[knum][snum] * eval[knum][snum]
                                                         + 2.0 * eval[knum][snum] * se_bubble[iomega].real();
                            }

                            // find a root of nonlinear_func = 0 from the sign change.
                            int count_root = 0;
                            std::vector<unsigned int> root_index;

                            for (auto iomega = 0; iomega < nomega - 1; ++iomega) {
                                if (nonlinear_func[iomega] * nonlinear_func[iomega + 1] < 0.0) {
                                    ++count_root;
                                    root_index.push_back(iomega);
                                }
                            }

                            if (count_root == 0) {
                                warn("bubble_correction",
                                     "Could not find a root in the nonlinear equation at this temperature. "
                                     "Use the w=0 component.");

                                real_self[snum] = se_bubble[0].real();

                            } else {
                                if (count_root > 1) {
                                    warn("bubble_correction",
                                         "Multiple roots were found in the nonlinear equation at this temperature. "
                                         "Use the lowest-frequency solution");
                                    std::cout << "   solution found at the following frequencies:\n";
                                    for (auto iroot = 0; iroot < count_root; ++iroot) {
                                        std::cout << std::setw(15)
                                                  << writes->in_kayser(omegalist[root_index[iroot]].real());
                                    }
                                    std::cout << '\n';
                                }

                                // Instead of performing a linear interpolation (secant method) of nonlinear_func,
                                // we interpolate the bubble self-energy. Since the frequency grid is dense (0.1 cm^-1 step),
                                // this approximation should not make any problems (hopefully).

                                double omega_solution = omegalist[root_index[0] + 1].real()
                                                        - nonlinear_func[root_index[0] + 1]
                                                          * domega / (nonlinear_func[root_index[0] + 1] -
                                                                      nonlinear_func[root_index[0]]);

                                real_self[snum] = (se_bubble[root_index[0] + 1].real()
                                                   - se_bubble[root_index[0]].real())
                                                  * (omega_solution - omegalist[root_index[0] + 1].real()) / domega
                                                  + se_bubble[root_index[0] + 1].real();
                            }
                        }
                    }
                }
                if (mympi->my_rank == 0) {
                    std::cout << "   branch : " << std::setw(5) << snum + 1;
                    std::cout << " omega (SC1) = " << std::setw(15) << writes->in_kayser(eval[knum][snum])
                              << " (cm^-1); ";
                    std::cout << " Re[Self] = " << std::setw(15) << writes->in_kayser(real_self[snum]) << " (cm^-1)\n";
                }
            }

            if (mympi->my_rank == 0) {
                // average self energy of degenerate modes
                int ishift = 0;
                double real_self_avg = 0.0;

                for (const auto &it: degeneracy_at_k[knum]) {
                    for (auto m = 0; m < it; ++m) {
                        real_self_avg += real_self[m + ishift];
                    }
                    real_self_avg /= static_cast<double>(it);

                    for (auto m = 0; m < it; ++m) {
                        real_self[m + ishift] = real_self_avg;
                    }
                    real_self_avg = 0.0;
                    ishift += it;
                }

                for (unsigned int snum = 0; snum < ns; ++snum) {
                    eval_bubble[iT][knum][snum] = eval[knum][snum] * eval[knum][snum]
                                                  - 2.0 * eval[knum][snum] * real_self[snum];
                    for (auto jk = 1; jk < kmesh_coarse->kpoint_irred_all[ik].size(); ++jk) {
                        auto knum2 = kmap_interpolate_to_scph[kmesh_coarse->kpoint_irred_all[ik][jk].knum];
                        eval_bubble[iT][knum2][snum] = eval_bubble[iT][knum][snum];
                    }
                }

                std::cout << '\n';
            }
        }

        if (mympi->my_rank == 0) {
            calc_new_dymat_with_evec(delta_dymat_scph_plus_bubble[iT],
                                     eval_bubble[iT],
                                     evec);
        }
    }

    deallocate(eval);
    deallocate(evec);
    deallocate(degeneracy_at_k);

    if (eval_bubble) deallocate(eval_bubble);

    if (mympi->my_rank == 0) {
        std::cout << " done!" << std::endl << std::endl;
    }
}

std::vector<std::complex<double>> Scph::get_bubble_selfenergy(const KpointMeshUniform *kmesh_in,
                                                              const unsigned int ns_in,
                                                              const double *const *eval_in,
                                                              const std::complex<double> *const *const *evec_in,
                                                              const unsigned int knum,
                                                              const unsigned int snum,
                                                              const double temp_in,
                                                              const std::vector<std::complex<double>> &omegalist)
{
    unsigned int arr_cubic[3];
    double xk_tmp[3];
    std::complex<double> omega_sum[2];

    double factor = 1.0 / (static_cast<double>(kmesh_in->nk) * std::pow(2.0, 4));
    const auto ns2 = ns_in * ns_in;
    const auto nks = kmesh_in->nk * ns2;

    double n1, n2;
    double f1, f2;

    auto knum_minus = kmesh_in->kindex_minus_xk[knum];
    arr_cubic[0] = ns_in * knum_minus + snum;

    std::vector<std::complex<double>> se_bubble(omegalist.size());

    const auto nomega = omegalist.size();

    std::complex<double> *ret_sum, *ret_mpi;
    allocate(ret_sum, nomega);
    allocate(ret_mpi, nomega);

    for (auto iomega = 0; iomega < nomega; ++iomega) {
        ret_sum[iomega] = std::complex<double>(0.0, 0.0);
        ret_mpi[iomega] = std::complex<double>(0.0, 0.0);
    }

    for (auto iks = mympi->my_rank; iks < nks; iks += mympi->nprocs) {

        auto ik1 = iks / ns2;
        auto is1 = (iks % ns2) / ns_in;
        auto is2 = iks % ns_in;

        for (auto m = 0; m < 3; ++m) xk_tmp[m] = kmesh_in->xk[knum][m] - kmesh_in->xk[ik1][m];
        auto ik2 = kmesh_in->get_knum(xk_tmp);

        double omega1 = eval_in[ik1][is1];
        double omega2 = eval_in[ik2][is2];

        arr_cubic[1] = ns_in * ik1 + is1;
        arr_cubic[2] = ns_in * ik2 + is2;

        double v3_tmp = std::norm(anharmonic_core->V3(arr_cubic,
                                                      kmesh_in->xk,
                                                      eval_in,
                                                      evec_in,
                                                      phase_factor_scph));

        if (thermodynamics->classical) {
            n1 = thermodynamics->fC(omega1, temp_in);
            n2 = thermodynamics->fC(omega2, temp_in);
            f1 = n1 + n2;
            f2 = n2 - n1;
        } else {
            n1 = thermodynamics->fB(omega1, temp_in);
            n2 = thermodynamics->fB(omega2, temp_in);
            f1 = n1 + n2 + 1.0;
            f2 = n2 - n1;
        }
        for (auto iomega = 0; iomega < nomega; ++iomega) {
            omega_sum[0] = 1.0 / (omegalist[iomega] + omega1 + omega2) - 1.0 / (omegalist[iomega] - omega1 - omega2);
            omega_sum[1] = 1.0 / (omegalist[iomega] + omega1 - omega2) - 1.0 / (omegalist[iomega] - omega1 + omega2);
            ret_mpi[iomega] += v3_tmp * (f1 * omega_sum[0] + f2 * omega_sum[1]);
        }
    }
    for (auto iomega = 0; iomega < nomega; ++iomega) {
        ret_mpi[iomega] *= factor;
    }

    MPI_Reduce(&ret_mpi[0], &ret_sum[0], nomega, MPI_COMPLEX16, MPI_SUM, 0, MPI_COMM_WORLD);

    for (auto iomega = 0; iomega < nomega; ++iomega) {
        se_bubble[iomega] = ret_sum[iomega];
    }

    deallocate(ret_mpi);
    deallocate(ret_sum);

    return se_bubble;
}

double Scph::distance(double *x1,
                      double *x2)
{
    auto dist = std::pow(x1[0] - x2[0], 2)
                + std::pow(x1[1] - x2[1], 2)
                + std::pow(x1[2] - x2[2], 2);
    dist = std::sqrt(dist);

    return dist;
}

void Scph::duplicate_xk_boundary(double *xk_in,
                                 std::vector<std::vector<double>> &vec_xk)
{
    int i;
    int n[3];
    double sign[3];
    std::vector<double> vec_tmp;

    vec_xk.clear();

    for (i = 0; i < 3; ++i) {
        if (std::abs(std::abs(xk_in[i]) - 0.5) < eps) {
            n[i] = 2;
        } else {
            n[i] = 1;
        }
    }

    for (i = 0; i < n[0]; ++i) {
        sign[0] = 1.0 - 2.0 * static_cast<double>(i);
        for (int j = 0; j < n[1]; ++j) {
            sign[1] = 1.0 - 2.0 * static_cast<double>(j);
            for (int k = 0; k < n[2]; ++k) {
                sign[2] = 1.0 - 2.0 * static_cast<double>(k);

                vec_tmp.clear();
                for (int l = 0; l < 3; ++l) {
                    vec_tmp.push_back(sign[l] * xk_in[l]);
                }
                vec_xk.push_back(vec_tmp);

            }
        }
    }
}

void Scph::print_distance_harmonic_IFC()
{
    const auto nat = system->nat;
    const auto natmin = system->natmin;

    double ***harmonic_IFCs;
    allocate(harmonic_IFCs, natmin, nat, 9);

    int i1, i2, ixyz;

    // initialize harmonic_IFCs
    for(i1 = 0; i1 < natmin; i1++){
        for(i2 = 0; i2 < nat; i2++){
            for(ixyz = 0; ixyz < 9; ixyz++){
                harmonic_IFCs[i1][i2][ixyz] = 0.0;
            }
        }
    }
    
    for(auto &it : fcs_phonon->fc2_ext){
        ixyz = it.xyz1 * 3 + it.xyz2;
        harmonic_IFCs[it.atm1][it.atm2][ixyz] += it.fcs_val;
    }

    // get minimum distance for pairs
    unsigned int i, j;
    const unsigned int nneib = 27;

    double ***xcrd;

    std::vector<DistWithCell> **distall;
    double **min_dist_tmp;

    allocate(distall, natmin, nat);
    allocate(xcrd, nneib, nat, 3);
    allocate(min_dist_tmp, natmin, nat);

    for (i = 0; i < nat; ++i) {
        for (j = 0; j < 3; ++j) {
            xcrd[0][i][j] = system->xr_s_no_displace[i][j];
            // xcrd[0][i][j] = system->xr_s[i][j];
        }
    }
    auto icell = 0;
    for (auto isize = -1; isize <= 1; ++isize) {
        for (auto jsize = -1; jsize <= 1; ++jsize) {
            for (auto ksize = -1; ksize <= 1; ++ksize) {

                if (isize == 0 && jsize == 0 && ksize == 0) continue;

                ++icell;
                for (i = 0; i < nat; ++i) {
                    xcrd[icell][i][0] = system->xr_s_no_displace[i][0] + static_cast<double>(isize);
                    xcrd[icell][i][1] = system->xr_s_no_displace[i][1] + static_cast<double>(jsize);
                    xcrd[icell][i][2] = system->xr_s_no_displace[i][2] + static_cast<double>(ksize);
                    // xcrd[icell][i][0] = system->xr_s[i][0] + static_cast<double>(isize);
                    // xcrd[icell][i][1] = system->xr_s[i][1] + static_cast<double>(jsize);
                    // xcrd[icell][i][2] = system->xr_s[i][2] + static_cast<double>(ksize);
                }
            }
        }
    }

    for (icell = 0; icell < nneib; ++icell) {
        for (i = 0; i < nat; ++i) {
            rotvec(xcrd[icell][i], xcrd[icell][i], system->lavec_s);
        }
    }

    for (i = 0; i < natmin; ++i) {
        const auto iat = system->map_p2s[i][0];
        for (j = 0; j < nat; ++j) {
            distall[i][j].clear();
            for (icell = 0; icell < nneib; ++icell) {

                double dist_tmp = distance(xcrd[0][iat], xcrd[icell][j]);
                distall[i][j].emplace_back(icell, dist_tmp);
            }
            std::sort(distall[i][j].begin(), distall[i][j].end());
            min_dist_tmp[i][j] = distall[i][j][0].dist;
        }
    }

    // print result

    std::ofstream fin_harmonic_IFCs;

    fin_harmonic_IFCs.open("harmonic_IFCs.txt");
    for(i1 = 0; i1 < natmin; i1++){
        for(i2 = 0; i2 < nat; i2++){
            for(ixyz = 0; ixyz < 9; ixyz++){
                fin_harmonic_IFCs << min_dist_tmp[i1][i2] << " " << harmonic_IFCs[i1][i2][ixyz] << std::endl;
            }
        }
    }

    return;
}

void Scph::write_anharmonic_correction_fc2(std::complex<double> ****delta_dymat,
                                           const unsigned int NT,
                                           const int type)
{
    unsigned int i, j;
    const auto Tmin = system->Tmin;
    const auto dT = system->dT;
    double ***delta_fc2;
    double **xtmp;
    const auto ns = dynamical->neval;
    unsigned int is, js, icell;
    unsigned int iat, jat;

    std::string file_fc2;
    std::ofstream ofs_fc2;

    if (type == 0) {
        file_fc2 = input->job_title + ".scph_dfc2";
    } else if (type == 1) {
        file_fc2 = input->job_title + ".scph+bubble(0)_dfc2";
    } else if (type == 2) {
        file_fc2 = input->job_title + ".scph+bubble(w)_dfc2";
    } else if (type == 3) {
        file_fc2 = input->job_title + ".scph+bubble(wQP)_dfc2";
    }

    ofs_fc2.open(file_fc2.c_str(), std::ios::out);
    if (!ofs_fc2)
        exit("write_anharmonic_correction_fc2",
             "Cannot open file_fc2");

    const auto ncell = kmesh_interpolate[0] * kmesh_interpolate[1] * kmesh_interpolate[2];

    allocate(delta_fc2, ns, ns, ncell);

    allocate(xtmp, system->natmin, 3);

    ofs_fc2.precision(10);

    for (i = 0; i < system->natmin; ++i) {
        rotvec(xtmp[i], system->xr_s[system->map_p2s[i][0]], system->lavec_s);
        rotvec(xtmp[i], xtmp[i], system->rlavec_p);
        for (j = 0; j < 3; ++j) xtmp[i][j] /= 2.0 * pi;
    }

    for (i = 0; i < 3; ++i) {
        for (j = 0; j < 3; ++j) {
            ofs_fc2 << std::setw(20) << system->lavec_p[j][i];
        }
        ofs_fc2 << std::endl;
    }
    ofs_fc2 << std::setw(5) << system->natmin << std::setw(5) << system->nkd << std::endl;
    for (i = 0; i < system->nkd; ++i) {
        ofs_fc2 << std::setw(5) << system->symbol_kd[i];
    }
    ofs_fc2 << std::endl;

    for (i = 0; i < system->natmin; ++i) {
        for (j = 0; j < 3; ++j) {
            ofs_fc2 << std::setw(20) << xtmp[i][j];
        }
        ofs_fc2 << std::setw(5) << system->kd[system->map_p2s[i][0]] + 1 << std::endl;
    }

    deallocate(xtmp);

    for (unsigned int iT = 0; iT < NT; ++iT) {
        const auto temp = Tmin + dT * static_cast<double>(iT);

        ofs_fc2 << "# Temp = " << temp << std::endl;

        for (is = 0; is < ns; ++is) {
            iat = is / 3;

            for (js = 0; js < ns; ++js) {
                jat = js / 3;

                for (icell = 0; icell < ncell; ++icell) {
                    delta_fc2[is][js][icell]
                            = delta_dymat[iT][is][js][icell].real()
                              * std::sqrt(system->mass[system->map_p2s[iat][0]]
                                          * system->mass[system->map_p2s[jat][0]]);
                }

            }
        }

        for (icell = 0; icell < ncell; ++icell) {

            for (is = 0; is < ns; ++is) {
                iat = is / 3;
                const auto icrd = is % 3;

                for (js = 0; js < ns; ++js) {
                    jat = js / 3;
                    const auto jcrd = js % 3;

                    const auto nmulti = mindist_list_scph[iat][jat][icell].shift.size();

                    for (auto it = mindist_list_scph[iat][jat][icell].shift.cbegin();
                         it != mindist_list_scph[iat][jat][icell].shift.cend(); ++it) {

                        ofs_fc2 << std::setw(4) << (*it).sx;
                        ofs_fc2 << std::setw(4) << (*it).sy;
                        ofs_fc2 << std::setw(4) << (*it).sz;
                        ofs_fc2 << std::setw(5) << iat << std::setw(3) << icrd;
                        ofs_fc2 << std::setw(4) << jat << std::setw(3) << jcrd;
                        ofs_fc2 << std::setprecision(15) << std::setw(25)
                                << delta_fc2[is][js][icell] / static_cast<double>(nmulti) << std::endl;

                    }

                }
            }
        }

        ofs_fc2 << std::endl;
    }

    deallocate(delta_fc2);

    ofs_fc2.close();
    std::cout << "  " << std::setw(input->job_title.length() + 12) << std::left << file_fc2;

    if (type == 0) {
        std::cout << " : Anharmonic corrections to the second-order IFCs (SCPH)" << std::endl;
    } else if (type == 1) {
        std::cout << " : Anharmonic corrections to the second-order IFCs (SCPH+Bubble(0))" << std::endl;
    } else if (type == 2) {
        std::cout << " : Anharmonic corrections to the second-order IFCs (SCPH+Bubble(w))" << std::endl;
    } else if (type == 3) {
        std::cout << " : Anharmonic corrections to the second-order IFCs (SCPH+Bubble(wQP))" << std::endl;
    }
}

void Scph::mpi_bcast_complex(std::complex<double> ****data,
                             const unsigned int NT,
                             const unsigned int nk,
                             const unsigned int ns)
{
    const int _NT = static_cast<int>(NT);
    const int _nk = static_cast<int>(nk);
    const int _ns = static_cast<int>(ns);

#ifdef MPI_CXX_DOUBLE_COMPLEX
    MPI_Bcast(&data[0][0][0][0], _NT * _nk * _ns * _ns,
              MPI_CXX_DOUBLE_COMPLEX, 0, MPI_COMM_WORLD);
#elif defined MPI_DOUBLE_COMPLEX
    MPI_Bcast(&data[0][0][0][0], _NT * _nk * _ns * _ns, MPI_DOUBLE_COMPLEX, 0, MPI_COMM_WORLD);
#else
    MPI_Bcast(&data[0][0][0][0], _NT * _nk * _ns * _ns, MPI_COMPLEX16, 0, MPI_COMM_WORLD);
#endif
}

void Scph::get_derivative_central_diff(const double delta_t,
                                       const unsigned int nk,
                                       double **omega0,
                                       double **omega2,
                                       double **domega_dt)
{
    const auto ns = dynamical->neval;
    const auto inv_dt = 1.0 / (2.0 * delta_t);
    for (auto ik = 0; ik < nk; ++ik) {
        for (auto is = 0; is < ns; ++is) {
            domega_dt[ik][is] = (omega2[ik][is] - omega0[ik][is]) * inv_dt;
            //    std::cout << "domega_dt = " << domega_dt[ik][is] << '\n';
        }
    }
}
int Scph::get_xyz_string(int ixyz, std::string& xyz_str){
    if(ixyz == 0){
        xyz_str = "x";
    }
    else if(ixyz == 1){
        xyz_str = "y";
    }
    else{
        xyz_str = "z";
    }
    return 0;

}