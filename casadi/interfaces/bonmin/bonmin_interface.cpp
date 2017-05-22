/*
 *    This file is part of CasADi.
 *
 *    CasADi -- A symbolic framework for dynamic optimization.
 *    Copyright (C) 2010-2014 Joel Andersson, Joris Gillis, Moritz Diehl,
 *                            K.U. Leuven. All rights reserved.
 *    Copyright (C) 2011-2014 Greg Horn
 *
 *    CasADi is free software; you can redistribute it and/or
 *    modify it under the terms of the GNU Lesser General Public
 *    License as published by the Free Software Foundation; either
 *    version 3 of the License, or (at your option) any later version.
 *
 *    CasADi is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 *    Lesser General Public License for more details.
 *
 *    You should have received a copy of the GNU Lesser General Public
 *    License along with CasADi; if not, write to the Free Software
 *    Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
 *
 */



#include "bonmin_interface.hpp"
#include "bonmin_nlp.hpp"
#include "casadi/core/std_vector_tools.hpp"
#include "../../core/global_options.hpp"
#include "../../core/casadi_interrupt.hpp"

#include <ctime>
#include <stdlib.h>
#include <iostream>
#include <iomanip>
#include <chrono>

using namespace std;

namespace casadi {
  extern "C"
  int CASADI_NLPSOL_BONMIN_EXPORT
  casadi_register_nlpsol_bonmin(Nlpsol::Plugin* plugin) {
    plugin->creator = BonminInterface::creator;
    plugin->name = "bonmin";
    plugin->doc = BonminInterface::meta_doc.c_str();
    plugin->version = CASADI_VERSION;
    plugin->options = &BonminInterface::options_;
    return 0;
  }

  extern "C"
  void CASADI_NLPSOL_BONMIN_EXPORT casadi_load_nlpsol_bonmin() {
    Nlpsol::registerPlugin(casadi_register_nlpsol_bonmin);
  }

  BonminInterface::BonminInterface(const std::string& name, const Function& nlp)
    : Nlpsol(name, nlp) {
  }

  BonminInterface::~BonminInterface() {
    clear_memory();
  }

  Options BonminInterface::options_
  = {{&Nlpsol::options_},
     {{"pass_nonlinear_variables",
       {OT_BOOL,
        "Pass list of variables entering nonlinearly to BONMIN"}},
      {"bonmin",
       {OT_DICT,
        "Options to be passed to BONMIN"}},
      {"var_string_md",
       {OT_DICT,
        "String metadata (a dictionary with lists of strings) "
        "about variables to be passed to BONMIN"}},
      {"var_integer_md",
       {OT_DICT,
        "Integer metadata (a dictionary with lists of integers) "
        "about variables to be passed to BONMIN"}},
      {"var_numeric_md",
       {OT_DICT,
        "Numeric metadata (a dictionary with lists of reals) about "
        "variables to be passed to BONMIN"}},
      {"con_string_md",
       {OT_DICT,
        "String metadata (a dictionary with lists of strings) about "
        "constraints to be passed to BONMIN"}},
      {"con_integer_md",
       {OT_DICT,
        "Integer metadata (a dictionary with lists of integers) "
        "about constraints to be passed to BONMIN"}},
      {"con_numeric_md",
       {OT_DICT,
        "Numeric metadata (a dictionary with lists of reals) about "
        "constraints to be passed to BONMIN"}},
      {"hess_lag",
       {OT_FUNCTION,
        "Function for calculating the Hessian of the Lagrangian (autogenerated by default)"}},
      {"hess_lag_options",
       {OT_DICT,
        "Options for the autogenerated Hessian of the Lagrangian."}},
      {"jac_g",
       {OT_FUNCTION,
        "Function for calculating the Jacobian of the constraints "
        "(autogenerated by default)"}},
      {"jac_g_options",
       {OT_DICT,
        "Options for the autogenerated Jacobian of the constraints."}},
      {"grad_f",
       {OT_FUNCTION,
        "Function for calculating the gradient of the objective "
        "(column, autogenerated by default)"}},
      {"grad_f_options",
       {OT_DICT,
        "Options for the autogenerated gradient of the objective."}}
     }
  };

  void BonminInterface::init(const Dict& opts) {
    // Call the init method of the base class
    Nlpsol::init(opts);

    // Default options
    pass_nonlinear_variables_ = false;
    Dict hess_lag_options, jac_g_options, grad_f_options;

    // Read user options
    for (auto&& op : opts) {
      if (op.first=="bonmin") {
        opts_ = op.second;
      } else if (op.first=="pass_nonlinear_variables") {
        pass_nonlinear_variables_ = op.second;
      } else if (op.first=="var_string_md") {
        var_string_md_ = op.second;
      } else if (op.first=="var_integer_md") {
        var_integer_md_ = op.second;
      } else if (op.first=="var_numeric_md") {
        var_numeric_md_ = op.second;
      } else if (op.first=="con_string_md") {
        con_string_md_ = op.second;
      } else if (op.first=="con_integer_md") {
        con_integer_md_ = op.second;
      } else if (op.first=="con_numeric_md") {
        con_numeric_md_ = op.second;
      } else if (op.first=="hess_lag_options") {
        hess_lag_options = op.second;
      } else if (op.first=="jac_g_options") {
        jac_g_options = op.second;
      } else if (op.first=="grad_f_options") {
        grad_f_options = op.second;
      } else if (op.first=="hess_lag") {
        Function f = op.second;
        casadi_assert(f.n_in()==4);
        casadi_assert(f.n_out()==1);
        set_function(f, "nlp_hess_l");
      } else if (op.first=="jac_g") {
        Function f = op.second;
        casadi_assert(f.n_in()==2);
        casadi_assert(f.n_out()==2);
        set_function(f, "nlp_jac_g");
      } else if (op.first=="grad_f") {
        Function f = op.second;
        casadi_assert(f.n_in()==2);
        casadi_assert(f.n_out()==2);
        set_function(f, "nlp_grad_f");
      }
    }

    // Do we need second order derivatives?
    exact_hessian_ = true;
    auto hessian_approximation = opts_.find("hessian_approximation");
    if (hessian_approximation!=opts_.end()) {
      exact_hessian_ = hessian_approximation->second == "exact";
    }

    // Setup NLP functions
    create_function("nlp_f", {"x", "p"}, {"f"});
    create_function("nlp_g", {"x", "p"}, {"g"});
    if (!has_function("nlp_grad_f")) {
      create_function("nlp_grad_f", {"x", "p"}, {"f", "grad:f:x"});
    }
    if (!has_function("nlp_jac_g")) {
      create_function("nlp_jac_g", {"x", "p"}, {"g", "jac:g:x"});
    }
    jacg_sp_ = get_function("nlp_jac_g").sparsity_out(1);

    // Allocate temporary work vectors
    if (exact_hessian_) {
      if (!has_function("nlp_hess_l")) {
        create_function("nlp_hess_l", {"x", "p", "lam:f", "lam:g"},
                        {"hess:gamma:x:x"}, {{"gamma", {"f", "g"}}});
      }
      hesslag_sp_ = get_function("nlp_hess_l").sparsity_out(0);
    } else if (pass_nonlinear_variables_) {
      nl_ex_ = oracle_.which_depends("x", {"f", "g"}, 2, false);
    }

    // Allocate work vectors
    alloc_w(nx_, true); // xk_
    alloc_w(ng_, true); // lam_gk_
    alloc_w(nx_, true); // lam_xk_
    alloc_w(ng_, true); // gk_
    alloc_w(nx_, true); // grad_fk_
    alloc_w(jacg_sp_.nnz(), true); // jac_gk_
    if (exact_hessian_) {
      alloc_w(hesslag_sp_.nnz(), true); // hess_lk_
    }
  }

  void BonminInterface::init_memory(void* mem) const {
    Nlpsol::init_memory(mem);
    //auto m = static_cast<BonminMemory*>(mem);
  }

  void BonminInterface::set_work(void* mem, const double**& arg, double**& res,
                                int*& iw, double*& w) const {
    auto m = static_cast<BonminMemory*>(mem);

    // Set work in base classes
    Nlpsol::set_work(mem, arg, res, iw, w);

    // Work vectors
    m->xk = w; w += nx_;
    m->lam_gk = w; w += ng_;
    m->lam_xk = w; w += nx_;
    m->gk = w; w += ng_;
    m->grad_fk = w; w += nx_;
    m->jac_gk = w; w += jacg_sp_.nnz();
    if (exact_hessian_) {
      m->hess_lk = w; w += hesslag_sp_.nnz();
    }
  }

  inline const char* return_status_string(Bonmin::TMINLP::SolverReturn status) {
    switch (status) {
    case Bonmin::TMINLP::MINLP_ERROR:
      return "MINLP_ERROR";
    case Bonmin::TMINLP::SUCCESS:
      return "SUCCESS";
    case Bonmin::TMINLP::INFEASIBLE:
      return "INFEASIBLE";
    case Bonmin::TMINLP::CONTINUOUS_UNBOUNDED:
      return "CONTINUOUS_UNBOUNDED";
    case Bonmin::TMINLP::LIMIT_EXCEEDED:
      return "LIMIT_EXCEEDED";
    case Bonmin::TMINLP::USER_INTERRUPT:
      return "USER_INTERRUPT";
    }
    return "Unknown";
  }


  /** \brief Helper class to direct messages to userOut()
  *
  * IPOPT has the concept of a Jorunal/Journalist
  * BOONMIN and CBC do not.
  */
  class BonMinMessageHandler : public CoinMessageHandler {
  public:
    BonMinMessageHandler(): CoinMessageHandler() { }
    /// Core of the class: the method that directs the messages
    int print() override {
      userOut() << messageBuffer_ << std::endl;
      return 0;
    }
    ~BonMinMessageHandler() override { }
    BonMinMessageHandler(const BonMinMessageHandler &other): CoinMessageHandler(other) {}
    BonMinMessageHandler(const CoinMessageHandler &other): CoinMessageHandler(other) {}
    BonMinMessageHandler & operator=(const BonMinMessageHandler &rhs) {
      BonMinMessageHandler::operator=(rhs);
      return *this;
    }
    CoinMessageHandler* clone() const override {
      return new BonMinMessageHandler(*this);
    }
  };

  void BonminInterface::solve(void* mem) const {
    auto m = static_cast<BonminMemory*>(mem);

    // Check the provided inputs
    checkInputs(mem);

    // Reset statistics
    m->inf_pr.clear();
    m->inf_du.clear();
    m->mu.clear();
    m->d_norm.clear();
    m->regularization_size.clear();
    m->alpha_pr.clear();
    m->alpha_du.clear();
    m->obj.clear();
    m->ls_trials.clear();

    // Reset number of iterations
    m->n_iter = 0;

    // Statistics
    for (auto&& s : m->fstats) s.second.reset();

    // MINLP instance
    SmartPtr<BonminUserClass> tminlp = new BonminUserClass(*this, m);

    BonMinMessageHandler mh;

    // Start an BONMIN application
    BonminSetup bonmin(&mh);

    SmartPtr<OptionsList> options = new OptionsList();
    SmartPtr<Journalist> journalist= new Journalist();
    SmartPtr<Bonmin::RegisteredOptions> roptions = new Bonmin::RegisteredOptions();

    {
      // Direct output through casadi::userOut()
      StreamJournal* jrnl_raw = new StreamJournal("console", J_ITERSUMMARY);
      jrnl_raw->SetOutputStream(&casadi::userOut());
      jrnl_raw->SetPrintLevel(J_DBG, J_NONE);
      SmartPtr<Journal> jrnl = jrnl_raw;
      journalist->AddJournal(jrnl);
    }

    options->SetJournalist(journalist);
    options->SetRegisteredOptions(roptions);
    bonmin.setOptionsAndJournalist(roptions, options, journalist);
    bonmin.registerOptions();
    // Get all options available in BONMIN
    auto regops = bonmin.roptions()->RegisteredOptionsList();

    // Pass all the options to BONMIN
    for (auto&& op : opts_) {
      // Find the option
      auto regops_it = regops.find(op.first);
      if (regops_it==regops.end()) {
        casadi_error("No such BONMIN option: " + op.first);
      }

      // Get the type
      Ipopt::RegisteredOptionType ipopt_type = regops_it->second->Type();

      // Pass to BONMIN
      bool ret;
      switch (ipopt_type) {
      case Ipopt::OT_Number:
        ret = bonmin.options()->SetNumericValue(op.first, op.second.to_double(), false);
        break;
      case Ipopt::OT_Integer:
        ret = bonmin.options()->SetIntegerValue(op.first, op.second.to_int(), false);
        break;
      case Ipopt::OT_String:
        ret = bonmin.options()->SetStringValue(op.first, op.second.to_string(), false);
        break;
      case Ipopt::OT_Unknown:
      default:
        casadi_warning("Cannot handle option \"" + op.first + "\", ignored");
        continue;
      }
      if (!ret) casadi_error("Invalid options were detected by BONMIN.");
    }

    // Initialize
    bonmin.initialize(GetRawPtr(tminlp));

    m->fstats.at("mainloop").tic();

    if (true) {
      // Branch-and-bound
      Bab bb;
      bb(bonmin);
    }

    m->fstats.at("mainloop").toc();

    // Save results to outputs
    casadi_copy(&m->fk, 1, m->f);
    casadi_copy(m->xk, nx_, m->x);
    casadi_copy(m->lam_gk, ng_, m->lam_g);
    casadi_copy(m->lam_xk, nx_, m->lam_x);
    casadi_copy(m->gk, ng_, m->g);

  }

  bool BonminInterface::
  intermediate_callback(BonminMemory* m, const double* x, const double* z_L, const double* z_U,
                        const double* g, const double* lambda, double obj_value, int iter,
                        double inf_pr, double inf_du, double mu, double d_norm,
                        double regularization_size, double alpha_du, double alpha_pr,
                        int ls_trials, bool full_callback) const {
    m->n_iter += 1;
    try {
      log("intermediate_callback started");
      m->inf_pr.push_back(inf_pr);
      m->inf_du.push_back(inf_du);
      m->mu.push_back(mu);
      m->d_norm.push_back(d_norm);
      m->regularization_size.push_back(regularization_size);
      m->alpha_pr.push_back(alpha_pr);
      m->alpha_du.push_back(alpha_du);
      m->ls_trials.push_back(ls_trials);
      m->obj.push_back(obj_value);
      if (!fcallback_.is_null()) {
        m->fstats.at("callback_fun").tic();
        if (full_callback) {
          casadi_copy(x, nx_, m->xk);
          for (int i=0; i<nx_; ++i) {
            m->lam_xk[i] = z_U[i]-z_L[i];
          }
          casadi_copy(lambda, ng_, m->lam_gk);
          casadi_copy(g, ng_, m->gk);
        } else {
          if (iter==0) {
            userOut<true, PL_WARN>()
              << "Warning: intermediate_callback is disfunctional in your installation. "
              "You will only be able to use stats(). "
              "See https://github.com/casadi/casadi/wiki/enableBonminCallback to enable it."
              << endl;
          }
        }

        // Inputs
        fill_n(m->arg, fcallback_.n_in(), nullptr);
        if (full_callback) {
          // The values used below are meaningless
          // when not doing a full_callback
          m->arg[NLPSOL_X] = x;
          m->arg[NLPSOL_F] = &obj_value;
          m->arg[NLPSOL_G] = g;
          m->arg[NLPSOL_LAM_P] = 0;
          m->arg[NLPSOL_LAM_X] = m->lam_xk;
          m->arg[NLPSOL_LAM_G] = m->lam_gk;
        }

        // Outputs
        fill_n(m->res, fcallback_.n_out(), nullptr);
        double ret_double;
        m->res[0] = &ret_double;

        fcallback_(m->arg, m->res, m->iw, m->w, 0);
        int ret = static_cast<int>(ret_double);

        m->fstats.at("callback_fun").toc();
        return  !ret;
      } else {
        return 1;
      }
    } catch(exception& ex) {
      if (iteration_callback_ignore_errors_) {
        userOut<true, PL_WARN>() << "intermediate_callback: " << ex.what() << endl;
      } else {
        throw ex;
      }
      return 1;
    }
  }

  void BonminInterface::
  finalize_solution(BonminMemory* m, TMINLP::SolverReturn status,
      const double* x, double obj_value) const {
    try {
      // Get primal solution
      casadi_copy(x, nx_, m->xk);

      // Get optimal cost
      m->fk = obj_value;

      // Get dual solution (simple bounds)
      if (m->lam_xk) {
        for (int i=0; i<nx_; ++i) {
          m->lam_xk[i] = casadi::nan;
        }
      }

      // Get dual solution (nonlinear bounds)
      casadi_fill(m->lam_gk, ng_, nan);

      // Get the constraints
      casadi_fill(m->gk, ng_, nan);

      // Get statistics
      m->iter_count = 0;

      // Interpret return code
      m->return_status = return_status_string(status);

    } catch(exception& ex) {
      userOut<true, PL_WARN>() << "finalize_solution failed: " << ex.what() << endl;
    }
  }

  bool BonminInterface::
  get_bounds_info(BonminMemory* m, double* x_l, double* x_u,
                  double* g_l, double* g_u) const {
    try {
      casadi_copy(m->lbx, nx_, x_l);
      casadi_copy(m->ubx, nx_, x_u);
      casadi_copy(m->lbg, ng_, g_l);
      casadi_copy(m->ubg, ng_, g_u);
      return true;
    } catch(exception& ex) {
      userOut<true, PL_WARN>() << "get_bounds_info failed: " << ex.what() << endl;
      return false;
    }
  }

  bool BonminInterface::
  get_starting_point(BonminMemory* m, bool init_x, double* x,
                     bool init_z, double* z_L, double* z_U,
                     bool init_lambda, double* lambda) const {
    try {
      // Initialize primal variables
      if (init_x) {
        casadi_copy(m->x0, nx_, x);
      }

      // Initialize dual variables (simple bounds)
      if (init_z) {
        if (m->lam_x0) {
          for (int i=0; i<nx_; ++i) {
            z_L[i] = max(0., -m->lam_x0[i]);
            z_U[i] = max(0., m->lam_x0[i]);
          }
        } else {
          casadi_fill(z_L, nx_, 0.);
          casadi_fill(z_U, nx_, 0.);
        }
      }

      // Initialize dual variables (nonlinear bounds)
      if (init_lambda) {
        casadi_copy(m->lam_g0, ng_, lambda);
      }

      return true;
    } catch(exception& ex) {
      userOut<true, PL_WARN>() << "get_starting_point failed: " << ex.what() << endl;
      return false;
    }
  }

  void BonminInterface::get_nlp_info(BonminMemory* m, int& nx, int& ng,
                                    int& nnz_jac_g, int& nnz_h_lag) const {
    try {
      // Number of variables
      nx = nx_;

      // Number of constraints
      ng = ng_;

      // Number of Jacobian nonzeros
      nnz_jac_g = ng_==0 ? 0 : jacg_sp_.nnz();

      // Number of Hessian nonzeros (only upper triangular half)
      nnz_h_lag = exact_hessian_ ? hesslag_sp_.nnz() : 0;

    } catch(exception& ex) {
      userOut<true, PL_WARN>() << "get_nlp_info failed: " << ex.what() << endl;
    }
  }

  int BonminInterface::get_number_of_nonlinear_variables() const {
    try {
      if (!pass_nonlinear_variables_) {
        // No Hessian has been interfaced
        return -1;
      } else {
        // Number of variables that appear nonlinearily
        int nv = 0;
        for (auto&& i : nl_ex_) if (i) nv++;
        return nv;
      }
    } catch(exception& ex) {
      userOut<true, PL_WARN>() << "get_number_of_nonlinear_variables failed: " << ex.what() << endl;
      return -1;
    }
  }

  bool BonminInterface::
  get_list_of_nonlinear_variables(int num_nonlin_vars, int* pos_nonlin_vars) const {
    try {
      for (int i=0; i<nl_ex_.size(); ++i) {
        if (nl_ex_[i]) *pos_nonlin_vars++ = i;
      }
      return true;
    } catch(exception& ex) {
      userOut<true, PL_WARN>() << "get_list_of_nonlinear_variables failed: " << ex.what() << endl;
      return false;
    }
  }

  BonminMemory::BonminMemory() {
    this->return_status = "Unset";
  }

  BonminMemory::~BonminMemory() {
  }

  Dict BonminInterface::get_stats(void* mem) const {
    Dict stats = Nlpsol::get_stats(mem);
    auto m = static_cast<BonminMemory*>(mem);
    stats["return_status"] = m->return_status;
    stats["iter_count"] = m->iter_count;
    return stats;
  }

} // namespace casadi
