#include "scfsolver.hpp"
#include <algorithm>
#include <cfloat>

#define OPTIM_ENABLE_ARMA_WRAPPERS
#include "optim.hpp"

namespace OpenOrbitalOptimizer {
  using namespace OpenOrbitalOptimizer;

  template<typename Torb, typename Tbase>
  SCFSolver<Torb, Tbase>::SCFSolver(const arma::uvec & number_of_blocks_per_particle_type, const arma::Col<Tbase> & maximum_occupation, const arma::Col<Tbase> & number_of_particles, const FockBuilder<Torb, Tbase> & fock_builder, const std::vector<std::string> & block_descriptions) : number_of_blocks_per_particle_type_(number_of_blocks_per_particle_type), maximum_occupation_(maximum_occupation), number_of_particles_(number_of_particles), fock_builder_(fock_builder), block_descriptions_(block_descriptions) {
    // Run sanity checks
    size_t num_expected_blocks = arma::sum(number_of_blocks_per_particle_type_);
    if(maximum_occupation_.size() != num_expected_blocks)
      throw std::logic_error("Vector of maximum occupation is not of expected length!\n");
    if(number_of_particles_.size() != number_of_blocks_per_particle_type_.size())
      throw std::logic_error("Vector of number of particles is not of expected length!\n");
    number_of_blocks_ = num_expected_blocks;
  }

  template<typename Torb, typename Tbase>
  void SCFSolver<Torb, Tbase>::initialize_with_fock(const FockMatrix<Torb> & guess_fock) {
    // Compute orbitals
    auto diagonalized_fock = compute_orbitals(guess_fock);
    const auto & orbitals = diagonalized_fock.first;
    orbital_energies_ = diagonalized_fock.second;
    auto orbital_occupations = determine_occupations(orbital_energies_);
    initialize_with_orbitals(orbitals, orbital_occupations);
  }

  template<typename Torb, typename Tbase>
  void SCFSolver<Torb, Tbase>::initialize_with_orbitals(const Orbitals<Torb> & orbitals, const OrbitalOccupations<Tbase> & orbital_occupations) {
    add_entry(std::make_pair(orbitals, orbital_occupations));
  }

  template<typename Torb, typename Tbase>
  bool SCFSolver<Torb, Tbase>::add_entry(const DensityMatrix<Torb, Tbase> & density) {
    // Compute the Fock matrix
    auto fock = fock_builder_(density);
    printf("Evaluated energy % .10f\n",fock.first);
    return add_entry(density, fock);
  }

  template<typename Torb, typename Tbase>
  bool SCFSolver<Torb, Tbase>::add_entry(const DensityMatrix<Torb, Tbase> & density, const FockBuilderReturn<Torb, Tbase> & fock) {
    // Make this into a pair
    orbital_history_.push_back(std::make_pair(density, fock));

    if(orbital_history_.size()==1)
      // First try is a success by definition
      return true;
    else {
      // Otherwise we have to check if we lowered the energy
      bool return_value = fock.first < orbital_history_[0].second.first;
      // and now we resort the stack in increasing energy
      std::sort(orbital_history_.begin(), orbital_history_.end(), [](const OrbitalHistoryEntry<Torb, Tbase> & a, const OrbitalHistoryEntry<Torb, Tbase> & b) {return a.second.first < b.second.first;});
      // Drop last entry if we are over the history length limit
      if(orbital_history_.size() > maximum_history_length_)
        orbital_history_.pop_back();

      return return_value;
    }
  }

  template<typename Torb, typename Tbase>
  DiagonalizedFockMatrix<Torb, Tbase> SCFSolver<Torb, Tbase>::compute_orbitals(const FockMatrix<Torb> & fock) const {
    DiagonalizedFockMatrix<Torb, Tbase> diagonalized_fock;
    // Allocate memory for orbitals and orbital energies
    diagonalized_fock.first.resize(fock.size());
    diagonalized_fock.second.resize(fock.size());

    // Diagonalize all blocks
    for(size_t iblock = 0; iblock < fock.size(); iblock++) {
      // Symmetrize Fock matrix
      auto fsymm(0.5*(fock[iblock]+fock[iblock].t()));
      arma::eig_sym(diagonalized_fock.second[iblock], diagonalized_fock.first[iblock], fsymm);

      diagonalized_fock.second[iblock].t().print(block_descriptions_[iblock] + " orbital energies");
      fflush(stdout);
    }

    return diagonalized_fock;
  }

  template<typename Torb, typename Tbase>
  std::vector<OrbitalRotation> SCFSolver<Torb, Tbase>::degrees_of_freedom() const {
    std::vector<OrbitalRotation> dofs;
    // Reference calculation
    auto & reference_solution = orbital_history_[0];
    auto & reference_occupations = reference_solution.first.second;

    // List occupied-virtual rotations
    for(size_t iblock = 0; iblock < reference_occupations.size(); iblock++) {
      // Find the occupied and virtual blocks
      arma::uvec occupied_indices = arma::find(reference_occupations[iblock] > 0.0);
      arma::uvec virtual_indices = arma::find(reference_occupations[iblock] == 0.0);
      for(auto o: occupied_indices)
        for(auto v: virtual_indices)
          dofs.push_back(std::make_tuple(iblock, o, v));
    }

    return dofs;
  }

  template<typename Torb, typename Tbase>
  arma::Col<Tbase> SCFSolver<Torb, Tbase>::orbital_gradient_vector() const {
    // Get the degrees of freedom
    auto dof_list = degrees_of_freedom();
    arma::Col<Tbase> orb_grad;

    if constexpr (arma::is_real<Torb>::value) {
      orb_grad.zeros(dof_list.size());
    } else {
      orb_grad.zeros(2*dof_list.size());
    }

    // Extract the orbital gradient
    for(size_t idof = 0; idof < dof_list.size(); idof++) {
      auto dof(dof_list[idof]);
      auto iblock = std::get<0>(dof);
      auto iorb = std::get<1>(dof);
      auto jorb = std::get<2>(dof);
      auto fock_block = get_fock_matrix_block(0, iblock);
      auto orbital_block = get_orbital_block(0, iblock);
      auto occ_block = get_orbital_occupation_block(0, iblock);

      arma::Mat<Torb> fock_mo = orbital_block.t() * fock_block * orbital_block;
      orb_grad(idof) = 2*std::real(fock_mo(iorb,jorb))*(occ_block(jorb)-occ_block(iorb));
      if constexpr (!arma::is_real<Torb>::value) {
        orb_grad(dof_list.size() + idof) = 2*std::imag(fock_mo(iorb,jorb))*(occ_block(jorb)-occ_block(iorb));
      }
    }
    return orb_grad;
  }

  template<typename Torb, typename Tbase>
  arma::Col<Tbase> SCFSolver<Torb, Tbase>::diagonal_orbital_hessian() const {
    // Get the degrees of freedom
    auto dof_list = degrees_of_freedom();
    arma::Col<Tbase> orb_hess;

    if constexpr (arma::is_real<Torb>::value) {
      orb_hess.zeros(dof_list.size());
    } else {
      orb_hess.zeros(2*dof_list.size());
    }

    // Extract the orbital hessient
    for(size_t idof = 0; idof < dof_list.size(); idof++) {
      auto dof(dof_list[idof]);
      auto iblock = std::get<0>(dof);
      auto iorb = std::get<1>(dof);
      auto jorb = std::get<2>(dof);
      auto fock_block = get_fock_matrix_block(0, iblock);
      auto orbital_block = get_orbital_block(0, iblock);
      auto occ_block = get_orbital_occupation_block(0, iblock);

      arma::Mat<Torb> fock_mo = orbital_block.t() * fock_block * orbital_block;
      orb_hess(idof) = 2*std::real((fock_mo(iorb,iorb)-fock_mo(jorb,jorb))*(occ_block(jorb)-occ_block(iorb)));
      if constexpr (!arma::is_real<Torb>::value) {
        orb_hess(dof_list.size() + idof) = orb_hess(idof);
      }
    }
    return orb_hess;
  }

  template<typename Torb, typename Tbase>
  Orbitals<Torb> SCFSolver<Torb, Tbase>::form_rotation_matrices(const arma::Col<Tbase> & x) const {
    const Orbitals<Torb> & reference_orbitals(orbital_history_[0].first.first);

    // Get the degrees of freedom
    auto dof_list = degrees_of_freedom();
    arma::Col<Tbase> orb_grad(dof_list.size());
    // Sort them by symmetry
    std::vector<std::vector<std::tuple<arma::uword, arma::uword, size_t>>> blocked_dof(reference_orbitals.size());
    for(size_t idof=0; idof<dof_list.size(); idof++) {
      auto dof = dof_list[idof];
      auto iblock = std::get<0>(dof);
      auto iorb = std::get<1>(dof);
      auto jorb = std::get<2>(dof);
      blocked_dof[iblock].push_back(std::make_tuple(iorb,jorb,idof));
    }

    // Form the rotation matrices
    Orbitals<Torb> kappa(reference_orbitals.size());
    for(size_t iblock=0; iblock < reference_orbitals.size(); iblock++) {
      // Collect the rotation parameters
      kappa[iblock].zeros(reference_orbitals[iblock].n_cols, reference_orbitals[iblock].n_cols);
      for(auto dof: blocked_dof[iblock]) {
        auto iorb = std::get<0>(dof);
        auto jorb = std::get<1>(dof);
        auto idof = std::get<2>(dof);
        kappa[iblock](iorb,jorb) = x(idof);
      }
      // imaginary parameters
      if constexpr (!arma::is_real<Torb>::value) {
          for(auto dof: blocked_dof[iblock]) {
            auto iorb = std::get<0>(dof);
            auto jorb = std::get<1>(dof);
            auto idof = std::get<2>(dof);
            kappa[iblock](iorb,jorb) += Torb(0.0,x(dof_list.size()+idof));
          }
        }
      // Antisymmetrize
      kappa[iblock] -= arma::trans(kappa[iblock]);
    }

    return kappa;
  }

  template<typename Torb, typename Tbase>
  Orbitals<Torb> SCFSolver<Torb, Tbase>::rotate_orbitals(const arma::Col<Tbase> & x) const {
    auto kappa(form_rotation_matrices(x));

    // Rotate the orbitals
    Orbitals<Torb> new_orbitals(orbital_history_[0].first.first);
    for(size_t iblock=0; iblock < new_orbitals.size(); iblock++) {
      // Exponentiated kappa
      arma::Mat<Torb> expkappa;

#if 0
      expkappa = arma::expmat(kappa[iblock]);
#else
      // Do eigendecomposition
      arma::Col<Tbase> eval;
      arma::Mat<std::complex<Tbase>> evec;
      arma::Mat<std::complex<Tbase>> kappa_imag(kappa[iblock]*std::complex<Tbase>(0.0,-1.0));
      arma::eig_sym(eval, evec, kappa_imag);
      // Exponentiate
      arma::Mat<std::complex<Tbase>> expkappa_imag(evec*arma::diagmat(arma::exp(eval*std::complex<Tbase>(0.0,1.0)))*evec.t());
      if constexpr (arma::is_real<Torb>::value) {
        expkappa = arma::real(expkappa_imag);
      } else {
        expkappa = expkappa_imag;
      }
#endif

      // Do the rotation
      new_orbitals[iblock] = new_orbitals[iblock]*expkappa;
    }

    return new_orbitals;
  }

  template<typename Torb, typename Tbase>
  OrbitalHistoryEntry<Torb, Tbase> SCFSolver<Torb, Tbase>::evaluate_rotation(const arma::Col<Tbase> & x) const {
    // Rotate orbitals
    auto new_orbitals(rotate_orbitals(x));
    // Compute the Fock matrix
    auto reference_occupations = orbital_history_[0].first.second;

    auto density_matrix = std::make_pair(new_orbitals, reference_occupations);
    auto fock = fock_builder_(density_matrix);
    return std::make_pair(density_matrix, fock);
  }

  template<typename Torb, typename Tbase>
  Tbase SCFSolver<Torb, Tbase>::maximum_rotation_step(const arma::Col<Tbase> & x) const {
    // Get the rotation matrices
    auto kappa(form_rotation_matrices(x));

    Tbase maximum_step = std::numeric_limits<Tbase>::max();
    for(size_t iblock=0; iblock < kappa.size(); iblock++) {
      arma::Col<Tbase> eval;
      arma::Mat<std::complex<Tbase>> evec;
      arma::Mat<std::complex<Tbase>> kappa_imag(kappa[iblock]*std::complex<Tbase>(0.0,-1.0));
      arma::eig_sym(eval, evec, kappa_imag);

      // Assume objective function is 4th order in orbitals
      Tbase block_maximum = 0.5*M_PI/arma::abs(eval).max();
      maximum_step = std::min(maximum_step, block_maximum);
    }

    return maximum_step;
  }

  template<typename Torb, typename Tbase>
  arma::Col<Tbase> SCFSolver<Torb, Tbase>::precondition_search_direction(const arma::Col<Tbase> & gradient, const arma::Col<Tbase> & diagonal_hessian, double shift) const {
    // Build positive definite diagonal Hessian
    auto positive_hessian(diagonal_hessian-arma::min(diagonal_hessian)+shift);
    // and divide the gradient by its square root
    return gradient/arma::sqrt(positive_hessian);
  }

  template<typename Torb, typename Tbase>
  void SCFSolver<Torb, Tbase>::steepest_descent_step() {
    // Reference energy
    auto reference_energy = orbital_history_[0].second.first;

    // Get the orbital gradient
    auto gradient = orbital_gradient_vector();
    // and the diagonal Hessian
    auto diagonal_hessian = diagonal_orbital_hessian();

    // Precondition search direction
    auto search_direction = precondition_search_direction(-gradient, diagonal_hessian);

    // Ensure that the search direction is down-hill
    if(arma::dot(search_direction, gradient) > 0.0) {
      printf("Warning - preconditioned search direction was not downhill, resetting it\n");
      search_direction = -gradient;
    }

    // Helper to evaluate steps
    std::function<Tbase(Tbase)> evaluate_step = [this, search_direction](Tbase length){
      if(length==0.0)
        // We just get the reference energy
        return orbital_history_[0].second.first;
      auto p(search_direction*length);
      auto entry = evaluate_rotation(p);
      // We can add the evaluated Fock matrix to the history
      if(length!=0.0)
        add_entry(entry.first, entry.second);
      printf("Evaluated step %e with energy %.10f\n", length, entry.second.first);
      return entry.second.first;
    };
    std::function<Tbase(Tbase)> scan_step = [this, search_direction](Tbase length){
      auto p(search_direction*length);
      auto entry = evaluate_rotation(p);
      return entry.second.first;
    };

    // Determine the maximal step size
    double Tmu = maximum_rotation_step(search_direction);
    // This step is a whole quasiperiod. Since we are going downhill,
    // the minimum would be at Tmu/4. However, since the function is
    // nonlinear, the minimum is found at a shorter distance. We use
    // Tmu/5 as the trial step
    auto step = Tmu/5.0;

    // Current energy
    auto initial_energy(evaluate_step(0.0));

    static int iter=0;
    arma::Col<Tbase> ttest(arma::linspace<arma::Col<Tbase>>(0.0,1.0,51)*Tmu);


    arma::Mat<Tbase> data(ttest.n_elem, 2);
    data.col(0)=ttest;
    for(size_t i=0;i<ttest.n_elem;i++)
      data(i,1) = scan_step(ttest(i));
    std::ostringstream oss;
    oss << "scan_" << iter << ".dat";
    data.save(oss.str(),arma::raw_ascii);
    iter++;

#if 0
    // Test the routines
    auto dof_list = degrees_of_freedom();
    auto g(search_direction);
    for(size_t i=0;i<g.n_elem;i++) {
      auto dof(dof_list[i]);
      auto iblock = std::get<0>(dof);
      auto iorb = std::get<1>(dof);
      auto jorb = std::get<2>(dof);

      double hh=cbrt(DBL_EPSILON);
      //double hh=1e-10;

      std::function<Tbase(Tbase)> eval = [this, search_direction, i](double xi){
        auto p(search_direction);
        p.zeros();
        p(i) = xi;
        return evaluate_rotation(p);
      };

      auto E2mi = eval(-2*hh);
      auto Emi = eval(-hh);
      auto Ei = eval(hh);
      auto E2i = eval(2*hh);

      double twop = (Ei-initial_energy)/hh;
      double threep = (Ei-Emi)/(2*hh);
      printf("i=%i twop=%e threep=%e\n",i,twop,threep);

      double h2diff = (Ei - 2*initial_energy + Emi)/(hh*hh);
      double h4diff = (-1/12.0*E2mi +4.0/3.0*Emi - 5.0/2.0*initial_energy + 4.0/3.0*Ei -1./12.0*E2i)/(hh*hh);

      g(i) = threep;
      printf("g(%3i), block %i orbitals %i-%i, % e vs % e (two-point   % e) difference % e ratio % e\n",i,iblock, iorb, jorb, gradient(i),g(i),twop,gradient(i)-g(i),gradient(i)/g(i));
      printf("h(%3i), block %i orbitals %i-%i, % e vs % e (three-point % e) difference % e ratio % e\n",i,iblock, iorb, jorb, diagonal_hessian(i),h4diff,h2diff,diagonal_hessian(i)-h4diff,diagonal_hessian(i)/h4diff);
      fflush(stdout);

    }
    gradient.print("Analytic gradient");
    g.print("Finite difference gradient");
    (gradient/g).print("Ratio");
#endif

    // Line search
    for(size_t itrial=0; itrial<10; itrial++) {
      printf("Trial iteration %i\n",itrial);

      // Evaluate the energy
      auto trial_energy = evaluate_step(step);
      if(trial_energy < initial_energy)
        // We already decreased the energy! Don't do anything more,
        // because our expansion point has already changed and going
        // further would make no sense.
        break;

      // Now we can fit a second order polynomial y = a x^2 + dE x +
      // initial_energy to our data: we know the initial value and the slope, and
      // the observed value.
      auto dE = arma::dot(gradient, search_direction);
      auto a = (trial_energy - dE*step - initial_energy)/(step*step);

      printf("a = %e\n",a);

      // To be realistic, the parabola should open up
      auto fit_okay = a>0.0;
      if(fit_okay) {
        auto predicted_step = -dE/(2.0*a);
        auto predicted_energy = a * predicted_step*predicted_step + dE*predicted_step + initial_energy;

        // To be reliable, the predicted optimal step should also be
        // in [0.0, step]
        if(predicted_step < 0.0 || predicted_step > step)
          fit_okay = false;
        if(predicted_step == step)
          // Nothing to do since the step was already evaluated!
          break;

        if(fit_okay) {
          auto observed_energy = evaluate_step(predicted_step);
          printf("Predicted energy % .10f observed energy % .10f difference %e\n", predicted_energy, observed_energy,predicted_energy-observed_energy);

          if(observed_energy < initial_energy) {
            break;
          } else {
            printf("Error: energy did not decrease in line search! Decreasing trial step size\n");
            step /= 2.0;
          }
        }
      }
    }
  }

  template<typename Torb, typename Tbase>
  Tbase SCFSolver<Torb, Tbase>::occupation_difference(const OrbitalOccupations<Tbase> & old_occ, const OrbitalOccupations<Tbase> & new_occ) const {
    Tbase diff = 0.0;
    for(size_t iblock = 0; iblock<old_occ.size(); iblock++)
      diff += arma::sum(arma::abs(new_occ[iblock]-old_occ[iblock]));
    return diff;
  }


  template<typename Torb, typename Tbase>
  OrbitalOccupations<Tbase> SCFSolver<Torb, Tbase>::determine_occupations(const OrbitalEnergies<Tbase> & orbital_energies) const {
    // Allocate the return
    OrbitalOccupations<Tbase> occupations(orbital_energies.size());
    for(size_t iblock=0; iblock<orbital_energies.size(); iblock++)
      occupations[iblock].zeros(orbital_energies[iblock].size());

    // Loop over particle types
    for(size_t particle_type = 0; particle_type < number_of_blocks_per_particle_type_.size(); particle_type++) {
      // Compute the offset in the block array
      size_t block_offset = (particle_type>0) ? arma::sum(number_of_blocks_per_particle_type_.subvec(0,particle_type-1)) : 0;

      // Collect the orbital energies with the block index and the in-block index for this particle type
      std::vector<std::tuple<Tbase, size_t, size_t>> all_energies;
      for(size_t iblock = block_offset; iblock < block_offset + number_of_blocks_per_particle_type_(particle_type); iblock++)
        for(size_t iorb = 0; iorb < orbital_energies[iblock].size(); iorb++)
          all_energies.push_back(std::make_tuple(orbital_energies[iblock](iorb), iblock, iorb));

      // Sort the energies in increasing order
      std::stable_sort(all_energies.begin(), all_energies.end(), [](const std::tuple<Tbase, size_t, size_t> & a, const std::tuple<Tbase, size_t, size_t> & b) {return std::get<0>(a) < std::get<0>(b);});

      // Fill the orbitals in increasing energy. This is how many
      // particles we have to place
      Tbase num_left = number_of_particles_(particle_type);
      for(auto fill_orbital : all_energies) {
        // Increase number of occupied orbitals
        auto iblock = std::get<1>(fill_orbital);
        auto iorb = std::get<2>(fill_orbital);
        occupations[iblock](iorb) = std::min(maximum_occupation_(iblock), num_left);
        // It is probably safer to do this for the sake of floating
        // point accuracy, since comparison to zero can be difficult
        if(num_left <= maximum_occupation_(iblock))
          break;
        num_left -= occupations[iblock](iorb);
      }
    }

    if(orbital_history_.size()) {
      // Check if occupations have changed
      double occ_diff = occupation_difference(orbital_history_[0].first.second, occupations);
      if(occ_diff > occupation_change_threshold_) {
        std::cout << "Warning: occupations changed by " << occ_diff << " from previous iteration\n";
      }
    }

    for(size_t l=0;l<occupations.size();l++) {
      occupations[l].t().print(block_descriptions_[l] + " occupations");
    }

    return occupations;
  }

  // Function to extract error vectors
  template <typename Tmatrix, typename Tbase>
  arma::Col<Tbase> extract_error_vector(const arma::Mat<Tmatrix> & mat) {
    if constexpr (arma::is_real<Tmatrix>::value) {
      return arma::vectorise(mat);
    } else {
      return arma::join_cols(arma::vectorise(arma::real(mat)),arma::vectorise(arma::imag(mat)));
    }
  }

  template<typename Torb, typename Tbase>
  arma::Mat<Torb> SCFSolver<Torb, Tbase>::get_density_matrix_block(size_t ihist, size_t iblock) const {
    auto entry = orbital_history_[ihist];
    auto & density_matrix = entry.first;
    return density_matrix.first[iblock] * arma::diagmat(density_matrix.second[iblock]) * arma::trans(density_matrix.first[iblock]);
  }

  template<typename Torb, typename Tbase>
  arma::Mat<Torb> SCFSolver<Torb, Tbase>::get_fock_matrix_block(size_t ihist, size_t iblock) const {
    auto entry = orbital_history_[ihist];
    return entry.second.second[iblock];
  }

  template<typename Torb, typename Tbase>
  arma::Mat<Torb> SCFSolver<Torb, Tbase>::get_orbital_block(size_t ihist, size_t iblock) const {
    auto entry = orbital_history_[ihist];
    return entry.first.first[iblock];
  }

  template<typename Torb, typename Tbase>
  arma::Col<Tbase> SCFSolver<Torb, Tbase>::get_orbital_occupation_block(size_t ihist, size_t iblock) const {
    auto entry = orbital_history_[ihist];
    return entry.first.second[iblock];
  }

  template<typename Torb, typename Tbase>
  arma::Col<Tbase> SCFSolver<Torb, Tbase>::diis_error_vector(size_t ihist) const {
    // Form error vectors
    std::vector<arma::Col<Tbase>> error_vectors(orbital_history_[ihist].second.second.size());
    for(size_t iblock = 0; iblock<number_of_blocks_;iblock++) {
      // Error is measured by FPS-SPF = FP - PF, since we have a unit metric.
      auto F = get_fock_matrix_block(ihist, iblock);
      auto P = get_density_matrix_block(ihist, iblock);
      auto FP = F*P;
      error_vectors[iblock] = extract_error_vector<Torb, Tbase>(FP - arma::trans(FP));
      //printf("ihist %i block %i density norm %e error vector norm %e\n",ihist,iblock,arma::norm(P, error_norm_.c_str()), arma::norm(error_vectors[iblock],error_norm_.c_str()));
    }

    // Compound error vector
    size_t nelem = 0;
    for(auto & block: error_vectors)
      nelem += block.size();

    arma::Col<Tbase> return_vector(nelem);
    size_t ioff=0;
    for(auto & block: error_vectors) {
      return_vector.subvec(ioff,ioff+block.size()-1) = block;
      ioff += block.size();
    }
    return return_vector;
  }

  template<typename Torb, typename Tbase>
  arma::Mat<Tbase> SCFSolver<Torb, Tbase>::diis_error_matrix() const {
    // Set up the DIIS error matrix
    size_t N=orbital_history_.size();

    // These are the orbital gradient dot products
    arma::Mat<Tbase> B(N,N,arma::fill::zeros);
    for(size_t ihist=0; ihist<N; ihist++) {
      arma::Col<Tbase> ei = diis_error_vector(ihist);
      for(size_t jhist=0; jhist<=ihist; jhist++) {
        arma::Col<Tbase> ej = diis_error_vector(jhist);
        B(jhist,ihist) = B(ihist, jhist) = arma::dot(ei,ej);
      }
    }
    return B;
  }

  template<typename Torb, typename Tbase>
  arma::Col<Tbase> SCFSolver<Torb, Tbase>::c1diis_weights() const {
    // Set up the DIIS error matrix
    size_t N=orbital_history_.size();
    arma::Mat<Tbase> B(diis_error_matrix());

    /*
      The C1-DIIS method is equivalent to solving the group of linear
      equations
      B w = lambda 1       (1)

      where B is the error matrix, w are the DIIS weights, lambda is the
      Lagrange multiplier that guarantees that the weights sum to unity,
      and 1 stands for a unit vector (1 1 ... 1)^T.

      By rescaling the weights as w -> w/lambda, equation (1) is
      reverted to the form
      B w = 1              (2)

      which can easily be solved using SVD techniques.

      Finally, the weights are renormalized to satisfy
      \sum_i w_i = 1
      which takes care of the Lagrange multipliers.
    */

    // Right-hand side of equation is
    arma::vec rh(N);
    rh.ones();

    // Solve C1-DIIS eigenproblem
    arma::Mat<Tbase> Bvec;
    arma::Col<Tbase> Bval;
    arma::eig_sym(Bval, Bvec, B);

    // Form solution
    arma::Col<Tbase> diis_weights(N,arma::fill::zeros);
    for(size_t i=0;i<N;i++)
      if(Bval(i)!=0.0)
        diis_weights += arma::dot(Bvec.col(i),rh)/Bval(i) * Bvec.col(i);

    // Sanity check for no elements: use even weights
    if(arma::sum(arma::abs(diis_weights))==0.0)
      diis_weights.ones();

    // Normalize solution
    diis_weights/=arma::sum(diis_weights);

    return diis_weights;
  }

  template<typename Torb, typename Tbase>
  arma::Col<Tbase> SCFSolver<Torb, Tbase>::c2diis_weights(double rejection_threshold) const {
    // Set up the DIIS error matrix
    arma::Mat<Tbase> B(diis_error_matrix());

    // Solve C2-DIIS eigenproblem
    arma::Mat<Tbase> evec;
    arma::Col<Tbase> eval;
    arma::eig_sym(eval, evec, B);

    // Normalize solution vectors
    arma::Mat<Tbase> candidate_solutions(evec);
    for(size_t icol=0;icol<evec.n_cols;icol++)
      candidate_solutions.col(icol) /= arma::sum(candidate_solutions.col(icol));

    // Find best solution that satisfies rejection threshold. Error
    // norms for the extrapolated vectors
    arma::Col<Tbase> error_norms(evec.n_cols,arma::fill::ones);
    error_norms *= std::numeric_limits<Tbase>::max();

    for(size_t icol=0; icol < evec.n_cols; icol++) {
      arma::Col<Tbase> soln = candidate_solutions.col(icol);
      // Skip solutions that have large elements
      if(arma::max(arma::abs(soln)) >= rejection_threshold)
        continue;
      // Compute extrapolated error
      arma::Col<Tbase> extrapolated_error = B * soln;
      error_norms(icol) = arma::norm(extrapolated_error, 2);
    }

    // Sort the solutions in the extrapolated error
    arma::uvec sortidx;
    sortidx = arma::sort_index(error_norms);
    error_norms(sortidx).print("Sorted C2DIIS errors");

    arma::Col<Tbase> diis_weights;
    for(auto index: sortidx) {
      diis_weights = candidate_solutions.col(index);
      // Skip solutions that have extrapolated error in the same order
      // of magnitude as the used floating point precision
      if(error_norms(index) >= 5*std::numeric_limits<Tbase>::epsilon()) {
        printf("Using C2DIIS solution index %i\n",index);
        break;
      }
    }

    return diis_weights;
  }

  template<typename Torb, typename Tbase>
  FockMatrix<Torb> SCFSolver<Torb, Tbase>::extrapolate_fock(const arma::Col<Tbase> & weights) const {
    if(weights.n_elem != orbital_history_.size()) {
      std::ostringstream oss;
      oss << "Inconsistent weights: " << weights.n_elem << " elements vs orbital history of size " << orbital_history_.size() << "!\n";
      throw std::logic_error(oss.str());
    }

    // Form DIIS extrapolated Fock matrix
    FockMatrix<Torb> extrapolated_fock(orbital_history_[0].second.second);
    for(size_t iblock = 0; iblock < extrapolated_fock.size(); iblock++) {
      // Apply the DIIS weight
      extrapolated_fock[iblock] *= weights(0);
      // and add the other blocks
      for(size_t ihist=1; ihist < orbital_history_.size(); ihist++)
        extrapolated_fock[iblock] += weights(ihist) * orbital_history_[ihist].second.second[iblock];
    }

    return extrapolated_fock;
  }

  template<typename Torb, typename Tbase>
  bool SCFSolver<Torb, Tbase>::attempt_extrapolation(const arma::Col<Tbase> & weights) {
    // Get the extrapolated Fock matrix
    auto fock(extrapolate_fock(weights));

    // Diagonalize the extrapolated Fock matrix
    auto diagonalized_fock = compute_orbitals(fock);
    auto & new_orbitals = diagonalized_fock.first;
    auto & new_orbital_energies = diagonalized_fock.second;

    // Determine new occupations
    auto new_occupations = determine_occupations(new_orbital_energies);
    // this needs to be a copy!
    auto reference_occupations = orbital_history_[0].first.second;

    printf("attempt_extrapolation: occupation difference %e\n",occupation_difference(reference_occupations, new_occupations));

    // Try first updating the orbitals, but not the occupations
    bool ref_success = add_entry(std::make_pair(new_orbitals, reference_occupations));

    // If occupations have changed, also check if updating the
    // occupations lowers the energy
    bool occ_success = false;
    if(occupation_difference(reference_occupations, new_occupations) > occupation_change_threshold_) {
      occ_success = add_entry(std::make_pair(new_orbitals, new_occupations));
      if(occ_success)
        printf("Changing occupations decreased energy\n");
      else
        printf("Changing occupations failed to decrease energy\n");
    }

    // Clean up history from incorrect occupation data
    if(occ_success) {
      size_t nremoved=0;
      for(size_t ihist=orbital_history_.size()-1;ihist>0;ihist--)
        if(occupation_difference(orbital_history_[0].first.second, orbital_history_[ihist].first.second) > occupation_change_threshold_) {
          nremoved++;
          orbital_history_.erase(orbital_history_.begin()+ihist);
        }
      printf("Removed %i entries corresponding to bad occupations\n",nremoved);
    }

    // Extrapolation was a success if either worked
    return ref_success || occ_success;
  }

  template<typename Torb, typename Tbase>
  arma::Col<Tbase> SCFSolver<Torb, Tbase>::adiis_linear_term() const {
    arma::Col<Tbase> ret(orbital_history_.size(),arma::fill::zeros);
    for(size_t iblock=0;iblock<number_of_blocks_;iblock++) {
      const auto & Dn = get_density_matrix_block(0, iblock);
      const auto & Fn = get_fock_matrix_block(0, iblock);
      for(size_t ihist=0;ihist<ret.size();ihist++) {
        // D_i - D_n
        arma::Mat<Torb> dD(get_density_matrix_block(ihist, iblock) - Dn);
        ret(ihist) += std::real(arma::trace(dD*Fn));
      }
    }
    return ret;
  }

  template<typename Torb, typename Tbase>
  arma::Mat<Tbase> SCFSolver<Torb, Tbase>::adiis_quadratic_term() const {
    arma::Mat<Tbase> ret(orbital_history_.size(),orbital_history_.size(),arma::fill::zeros);
    for(size_t iblock=0;iblock<number_of_blocks_;iblock++) {
      const auto & Dn = get_density_matrix_block(0, iblock);
      const auto & Fn = get_fock_matrix_block(0, iblock);
      for(size_t ihist=0;ihist<orbital_history_.size();ihist++) {
        for(size_t jhist=0;jhist<orbital_history_.size();jhist++) {
          // D_i - D_n
          arma::Mat<Torb> dD(get_density_matrix_block(ihist, iblock) - Dn);
          // F_j - F_n
          arma::Mat<Torb> dF(get_fock_matrix_block(jhist, iblock) - Fn);
          ret(ihist,jhist) += std::real(arma::trace(dD*dF));
        }
      }
    }
    // Only the symmetric part matters!
    return 0.5*(ret+ret.t());
  }

  template<typename Torb, typename Tbase>
  arma::Col<Tbase> SCFSolver<Torb, Tbase>::adiis_weights() const {
    // Form linear and quadratic terms
    auto linear_term = adiis_linear_term();
    auto quadratic_term = adiis_quadratic_term();

    // OptimLib doesn't support float, so these routines are all in double precision.
    // Function to compute weights from the parameters
    std::function<arma::vec(const arma::vec & x)> x_to_weight = [](const arma::vec & x) { return arma::square(x)/arma::dot(x,x); };
    // and its Jacobian
    std::function<arma::mat(const arma::vec & x)> x_to_weight_jacobian = [x_to_weight](const arma::vec & x) {
      auto w(x_to_weight(x));
      auto xnorm = arma::norm(x,2);
      arma::mat jac(x.n_elem,x.n_elem,arma::fill::zeros);
      for(size_t i=0;i<x.n_elem;i++) {
        for(size_t j=0;j<x.n_elem;j++) {
          jac(i,j) -= w(j)*2.0*x(i)/xnorm;
        }
        jac(i,i) += 2.0*x(i)/xnorm;
      }
      //jac.print("Jacobian");
      return jac;
    };

    // Function to compute the ADIIS energy and gradient
    std::function<Tbase(const arma::vec & x, arma::vec *grad, void *opt_data)> adiis_energy_gradient = [linear_term, quadratic_term, x_to_weight, x_to_weight_jacobian](const arma::vec & x, arma::vec *grad, void *opt_data) {
      (void) opt_data;
      auto w(x_to_weight(x));
      arma::vec g = x_to_weight_jacobian(x)*(linear_term + quadratic_term*w);
      if(grad!=nullptr) {
        *grad = g;
      }

      //linear_term.print("Linear term");
      //quadratic_term.print("Quadratic term");
      //g.print("Gradient");

      auto fval = arma::dot(linear_term, w) + 0.5*arma::dot(w, quadratic_term*w);
      //printf("fval = %e\n",fval);
      return fval;
    };

    // Optimization
    arma::vec x(orbital_history_.size(),arma::fill::ones);
    bool success = optim::bfgs(x, adiis_energy_gradient, nullptr);
    if (success) {
      std::cout << "ADIIS optimization successful\n";
    } else {
      std::cout << "ADIIS optimization failed\n";
    }

    return arma::conv_to<arma::Col<Tbase>>::from(x_to_weight(x));
  }

  template<typename Torb, typename Tbase>
  void SCFSolver<Torb, Tbase>::run()  {
    double old_energy = orbital_history_[0].second.first;
    for(size_t iteration=1; iteration <= maximum_iterations_; iteration++) {
      // Compute DIIS error
      double diis_error = arma::norm(diis_error_vector(0),error_norm_.c_str());

      printf("\n\nIteration %i: energy % .10f change %e DIIS error vector %s norm %e\n", orbital_history_[0].second.first, orbital_history_[0].second.first-old_energy, iteration, error_norm_.c_str(), diis_error);
      printf("History size %i\n",orbital_history_.size());
      if(diis_error < convergence_threshold_) {
        printf("Converged!\n");
        break;
      }

      if(iteration == 1) {
        // The orbitals can be bad, so start with a steepest descent
        // step to give DIIS a better starting point
        double old_energy = orbital_history_[0].second.first;
        steepest_descent_step();

      } else {
        // Form DIIS and ADIIS weights
        //arma::Col<Tbase> c2diis_w(c2diis_weights());
        arma::Col<Tbase> c2diis_w(c2diis_weights());
        c2diis_w.print("C2DIIS weights");
        arma::Col<Tbase> c1diis_w(c1diis_weights());
        c1diis_w.print("C1DIIS weights");
        arma::Col<Tbase> adiis_w(adiis_weights());
        adiis_w.print("ADIIS weights");

        arma::Mat<Tbase> diis_errmat(diis_error_matrix());
        printf("C1DIIS extrapolated error norm %e\n",arma::norm(diis_errmat*c1diis_w,error_norm_.c_str()));
        printf("C2DIIS extrapolated error norm %e\n",arma::norm(diis_errmat*c2diis_w,error_norm_.c_str()));
        printf("ADIIS extrapolated error norm %e\n",arma::norm(diis_errmat*adiis_w,error_norm_.c_str()));

        // Form DIIS weights
        arma::Col<Tbase> diis_weights(orbital_history_.size(), arma::fill::zeros);
        if(diis_error < diis_threshold_) {
          printf("C2DIIS extrapolation\n");
          diis_weights = c2diis_w;
          //printf("C1DIIS extrapolation\n");
          //diis_weights = c1diis_w;
        } else if(diis_error < diis_epsilon_) {
          printf("Mixed DIIS and ADIIS\n");
          double adiis_coeff = (diis_error-diis_threshold_)/(diis_epsilon_-diis_threshold_);
          double c2diis_coeff = 1.0 - adiis_coeff;
          diis_weights = adiis_coeff * adiis_w + c2diis_coeff * c2diis_w;
        } else {
          diis_weights = adiis_w;
        }
        diis_weights.print("Extrapolation weigths");

        // Perform extrapolation. If it does not lower the energy, we do
        // a scaled steepest descent step, instead.
        old_energy = orbital_history_[0].second.first;
        if(!attempt_extrapolation(diis_weights)) {
          printf("Warning: did not go down in energy!\n");
          steepest_descent_step();
        }
      }
    }
  }


  // Instantiate myclass for the supported template type parameters
  template class SCFSolver<float, float>;
  template class SCFSolver<std::complex<float>, float>;
  template class SCFSolver<double, double>;
  template class SCFSolver<std::complex<double>, double>;
}
