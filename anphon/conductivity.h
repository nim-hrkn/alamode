/*
 conductivity.h

 Copyright (c) 2014, 2015, 2016 Terumasa Tadano

 This file is distributed under the terms of the MIT license.
 Please see the file 'LICENCE.txt' in the root directory 
 or http://opensource.org/licenses/mit-license.php for information.
*/

#pragma once

#include "pointers.h"
#include "kpoint.h"
#include <vector>
#include <set>
#include <complex>

namespace PHON_NS {
class Conductivity : protected Pointers {
public:
    Conductivity(class PHON *);

    ~Conductivity();

    void setup_kappa();

    void prepare_restart();

    void calc_anharmonic_imagself();

    void compute_kappa();

    int calc_kappa_spec;
    unsigned int ntemp;
    double **damping3;
    double ***kappa;
    double ***kappa_spec;
    double ***kappa_coherent;
    double *temperature;
    int calc_coherent;

private:
    void set_default_variables();

    void deallocate_variables();

    double ***vel;
    std::complex<double> ****velmat;
    unsigned int nk, ns;
    int nshift_restart;
    std::vector<int> vks_l, vks_done;
    std::set<int> vks_job;
    std::string file_coherent_elems;

    void write_result_gamma(unsigned int,
                            unsigned int,
                            double ***,
                            double **) const;

    void average_self_energy_at_degenerate_point(const int n,
                                                 const int m,
                                                 const KpointMeshUniform *kmesh_in,
                                                 const double *const *eval_in,
                                                 double **damping) const;

    void compute_frequency_resolved_kappa(const int ntemp,
                                          const int smearing_method,
                                          const KpointMeshUniform *kmesh_in,
                                          const double *const *eval_in,
                                          const double *const *const *const *kappa_mode,
                                          double ***kappa_spec_out) const;

    void compute_kappa_intraband(const KpointMeshUniform *kmesh_in,
                                 const double *const *eval_in,
                                 const double *const *lifetime,
                                 double ***kappa_intra,
                                 double ***kappa_spec_out) const;

    void compute_kappa_coherent(const KpointMeshUniform *kmesh_in,
                                const double *const *eval_in,
                                const double *const *gamma_total,
                                double ***kappa_coherent_out) const;
};
}
