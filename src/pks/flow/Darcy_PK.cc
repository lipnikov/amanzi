/*
  This is the flow component of the Amanzi code.

  Copyright 2010-201x held jointly by LANS/LANL, LBNL, and PNNL. 
  Amanzi is released under the three-clause BSD License. 
  The terms of use and "as is" disclaimer for this license are 
  provided in the top-level COPYRIGHT file.

  Authors: Neil Carlson (version 1) 
           Konstantin Lipnikov (version 2) (lipnikov@lanl.gov)
*/

#include <vector>

#include "Epetra_Import.h"
#include "Epetra_Vector.h"

#include "errors.hh"
#include "exceptions.hh"
#include "LinearOperatorFactory.hh"
#include "mfd3d_diffusion.hh"
#include "OperatorDiffusionFactory.hh"
#include "tensor.hh"

#include "Darcy_PK.hh"
#include "FlowDefs.hh"
#include "Flow_SourceFactory.hh"

#include "darcy_velocity_evaluator.hh"
#include "primary_variable_field_evaluator.hh"

namespace Amanzi {
namespace Flow {

/* ******************************************************************
* Simplest possible constructor: extracts lists and requires fields.
****************************************************************** */
Darcy_PK::Darcy_PK(Teuchos::ParameterList& glist, Teuchos::RCP<State> S) : Flow_PK()
{
  S_ = S;

  mesh_ = S_->GetMesh();
  dim = mesh_->space_dimension();

  // We need the flow list
  Teuchos::ParameterList flow_list;
  if (glist.isSublist("Flow")) {
    flow_list = glist.sublist("Flow");
  } else {
    Errors::Message msg("Flow PK: input parameter list does not have <Flow> sublist.");
    Exceptions::amanzi_throw(msg);
  }

  if (flow_list.isSublist("Darcy Problem")) {
    dp_list_ = flow_list.sublist("Darcy Problem");
  } else {
    Errors::Message msg("Flow PK: input parameter list does not have <Darcy Problem> sublist.");
    Exceptions::amanzi_throw(msg);
  }

  // We also need iscaleneous sublists
  if (glist.isSublist("Preconditioners")) {
    preconditioner_list_ = glist.sublist("Preconditioners");
  } else {
    Errors::Message msg("Flow PK: input XML does not have <Preconditioners> sublist.");
    Exceptions::amanzi_throw(msg);
  }

  if (glist.isSublist("Solvers")) {
    linear_operator_list_ = glist.sublist("Solvers");
  } else {
    Errors::Message msg("Flow PK: input XML does not have <Solvers> sublist.");
    Exceptions::amanzi_throw(msg);
  }

  // for creating fields
  std::vector<std::string> names(2);
  names[0] = "cell"; 
  names[1] = "face";

  std::vector<AmanziMesh::Entity_kind> locations(2);
  locations[0] = AmanziMesh::CELL; 
  locations[1] = AmanziMesh::FACE;

  std::vector<int> ndofs(2, 1);

  // require state variables for the Darcy PK
  if (!S_->HasField("fluid_density")) {
    S_->RequireScalar("fluid_density", passwd_);
  }
  if (!S_->HasField("fluid_viscosity")) {
    S_->RequireScalar("fluid_viscosity", passwd_);
  }
  if (!S_->HasField("gravity")) {
    S_->RequireConstantVector("gravity", passwd_, dim);
  }

  if (!S_->HasField("pressure")) {
    S_->RequireField("pressure", passwd_)->SetMesh(mesh_)->SetGhosted(true)
      ->SetComponents(names, locations, ndofs);
  }

  if (!S_->HasField("permeability")) {
    S_->RequireField("permeability", passwd_)->SetMesh(mesh_)->SetGhosted(true)
      ->SetComponent("cell", AmanziMesh::CELL, dim);
  }

  if (!S_->HasField("porosity")) {
    S_->RequireField("porosity", passwd_)->SetMesh(mesh_)->SetGhosted(true)
      ->SetComponent("cell", AmanziMesh::CELL, 1);
  }

  if (!S_->HasField("specific_storage")) {
    S_->RequireField("specific_storage", passwd_)->SetMesh(mesh_)->SetGhosted(true)
      ->SetComponent("cell", AmanziMesh::CELL, 1);
  }
  if (!S_->HasField("specific_yield")) {
    S_->RequireField("specific_yield", passwd_)->SetMesh(mesh_)->SetGhosted(true)
      ->SetComponent("cell", AmanziMesh::CELL, 1);
  }
  if (!S_->HasField("water_saturation")) {
    S_->RequireField("water_saturation", passwd_)->SetMesh(mesh_)->SetGhosted(true)
      ->SetComponent("cell", AmanziMesh::CELL, 1);
  }
  if (!S_->HasField("prev_water_saturation")) {
    S_->RequireField("prev_water_saturation", passwd_)->SetMesh(mesh_)->SetGhosted(true)
      ->SetComponent("cell", AmanziMesh::CELL, 1);
  }
  if (!S_->HasField("darcy_flux")) {
    S_->RequireField("darcy_flux", passwd_)->SetMesh(mesh_)->SetGhosted(true)
      ->SetComponent("face", AmanziMesh::FACE, 1);

    Teuchos::ParameterList elist;
    elist.set<std::string>("evaluator name", "darcy_flux");
    darcy_flux_eval = Teuchos::rcp(new PrimaryVariableFieldEvaluator(elist));
    S_->SetFieldEvaluator("darcy_flux", darcy_flux_eval);
  }

  // secondary fields and evaluators
  if (!S_->HasField("darcy_velocity")) {
    S_->RequireField("darcy_velocity", "darcy_velocity")->SetMesh(mesh_)->SetGhosted(true)
      ->SetComponent("cell", AmanziMesh::CELL, dim);

    Teuchos::ParameterList elist;
    Teuchos::RCP<DarcyVelocityEvaluator> eval = Teuchos::rcp(new DarcyVelocityEvaluator(elist));
    S_->SetFieldEvaluator("darcy_velocity", eval);
  }


  if (!S_->HasField("hydraulic_head")) {
    S_->RequireField("hydraulic_head", passwd_)->SetMesh(mesh_)->SetGhosted(true)
      ->SetComponent("cell", AmanziMesh::CELL, 1);
  }
}


/* ******************************************************************
* Clean memory.
****************************************************************** */
Darcy_PK::~Darcy_PK()
{
  if (bc_pressure != NULL) delete bc_pressure;
  if (bc_head != NULL) delete bc_head;
  if (bc_flux != NULL) delete bc_flux;
  if (bc_seepage != NULL) delete bc_seepage;

  if (ti_specs != NULL) {
    OutputTimeHistory(dp_list_, ti_specs->dT_history);
  }

  if (src_sink != NULL) delete src_sink;
  if (vo_ != NULL) delete vo_;
}


/* ******************************************************************
* Extract information from Diffusion Problem parameter list.
****************************************************************** */
void Darcy_PK::Initialize(const Teuchos::Ptr<State>& S)
{
  // Initialize defaults
  bc_pressure = NULL; 
  bc_head = NULL;
  bc_flux = NULL;
  bc_seepage = NULL; 
  src_sink = NULL;

  ti_specs = NULL;
  src_sink = NULL;
  src_sink_distribution = 0;

  // Initilize various common data depending on mesh and state.
  Flow_PK::Init();

  // Time control specific to this PK.
  ResetPKtimes(0.0, FLOW_INITIAL_DT);
  dT_desirable_ = dT;

  // Allocate memory for boundary data. 
  bc_model.resize(nfaces_wghost, 0);
  bc_submodel.resize(nfaces_wghost, 0);
  bc_value.resize(nfaces_wghost, 0.0);
  bc_coef.resize(nfaces_wghost, 0.0);
  op_bc_ = Teuchos::rcp(new Operators::BCs(bc_model, bc_value));

  rainfall_factor.resize(nfaces_wghost, 1.0);

  // create verbosity object
  vo_ = new VerboseObject("FlowPK::Darcy", dp_list_); 

  // Process Native XML.
  ProcessParameterList(dp_list_);

  // Create solution and auxiliary data for time history.
  solution = Teuchos::rcp(new CompositeVector(*(S_->GetFieldData("pressure"))));
  solution->PutScalar(0.0);

  const Epetra_BlockMap& cmap = mesh_->cell_map(false);
  pdot_cells_prev = Teuchos::rcp(new Epetra_Vector(cmap));
  pdot_cells = Teuchos::rcp(new Epetra_Vector(cmap));
  
  // Initialize times.
  double time = S_->time();
  if (time >= 0.0) T_physics = time;

  // Initialize boundary condtions. 
  ProcessShiftWaterTableList(dp_list_);

  time = T_physics;
  bc_pressure->Compute(time);
  bc_flux->Compute(time);
  bc_seepage->Compute(time);
  if (shift_water_table_.getRawPtr() == NULL) {
    bc_head->Compute(time);
  } else {
    bc_head->ComputeShift(time, shift_water_table_->Values());
  }

  const CompositeVector& pressure = *S_->GetFieldData("pressure");
  ComputeBCs(pressure);

  // Allocate memory for other fundamental structures
  K.resize(ncells_owned);

  if (src_sink_distribution & Amanzi::Functions::DOMAIN_FUNCTION_ACTION_DISTRIBUTE_PERMEABILITY) {
    Kxy = Teuchos::rcp(new Epetra_Vector(mesh_->cell_map(false)));
  }
}


/* ******************************************************************
* Initialization of auxiliary variables (lambda and two saturations).
* WARNING: Flow_PK may use complex initialization of the remaining 
* state variables.
****************************************************************** */
void Darcy_PK::InitializeAuxiliaryData()
{
  // pressures (lambda is not important when solver is very accurate)
  CompositeVector& cv = *S_->GetFieldData("pressure", passwd_);
  const Epetra_MultiVector& pressure = *(cv.ViewComponent("cell"));
  Epetra_MultiVector& lambda = *(cv.ViewComponent("face"));

  DeriveFaceValuesFromCellValues(pressure, lambda);

  // saturations
  if (!S_->GetField("water_saturation", passwd_)->initialized()) {
    S_->GetFieldData("water_saturation", passwd_)->PutScalar(1.0);
    S_->GetField("water_saturation", passwd_)->set_initialized();
  }
  if (!S_->GetField("prev_water_saturation", passwd_)->initialized()) {
    S_->GetFieldData("prev_water_saturation", passwd_)->PutScalar(1.0);
    S_->GetField("prev_water_saturation", passwd_)->set_initialized();
  }
}


/* ******************************************************************
* Wrapper for a steady-state solver
****************************************************************** */
void Darcy_PK::InitializeSteadySaturated()
{ 
  if (vo_->getVerbLevel() >= Teuchos::VERB_MEDIUM) {
    Teuchos::OSTab tab = vo_->getOSTab();
    *vo_->os() << "initializing with a saturated steady state..." << std::endl;
  }
  double T = S_->time();
  SolveFullySaturatedProblem(T, *solution);
}


/* ******************************************************************
* Specific initialization of a steady state time integration phase.
* WARNING: now it is equivalent to transient phase.
****************************************************************** */
void Darcy_PK::InitSteadyState(double T0, double dT0)
{
  specific_yield_copy_ = Teuchos::null;

  if (ti_specs != NULL) OutputTimeHistory(dp_list_, ti_specs->dT_history);
  ti_specs = &ti_specs_sss_;

  InitNextTI(T0, dT0, ti_specs_sss_);

  error_control_ = FLOW_TI_ERROR_CONTROL_PRESSURE;  // usually 1e-4;
}


/* ******************************************************************
* Specific initialization of a transient time integration phase.  
****************************************************************** */
void Darcy_PK::InitTransient(double T0, double dT0)
{
  UpdateSpecificYield_();

  if (ti_specs != NULL) OutputTimeHistory(dp_list_, ti_specs->dT_history);
  ti_specs = &ti_specs_trs_;

  InitNextTI(T0, dT0, ti_specs_trs_);

  error_control_ = FLOW_TI_ERROR_CONTROL_PRESSURE;  // usually 1e-4
}


/* ******************************************************************
* Generic initialization of a next time integration phase.
****************************************************************** */
void Darcy_PK::InitNextTI(double T0, double dT0, TI_Specs& ti_specs)
{
  if (vo_->getVerbLevel() >= Teuchos::VERB_MEDIUM) {
    Teuchos::OSTab tab = vo_->getOSTab();
    *vo_->os() << std::endl
        << "****************************************" << std::endl
        << vo_->color("green") << "New TI phase: " << ti_specs.ti_method_name.c_str() << vo_->reset() << std::endl
        << "****************************************" << std::endl
        << "  start T=" << T0 / FLOW_YEAR << " [y], dT=" << dT0 << " [sec]" << std::endl
        << "  time stepping id=" << ti_specs.dT_method << std::endl
        << "  sources distribution id=" << src_sink_distribution << std::endl
        << "  linear solver name: " << ti_specs.solver_name.c_str() << std::endl
        << "  preconditioner: " << ti_specs.preconditioner_name.c_str() << std::endl;
    if (ti_specs.initialize_with_darcy) {
      *vo_->os() << "  initial pressure guess: \"saturated solution\"" << std::endl;
    } else {
      *vo_->os() << "  initial pressure guess: \"from state\"" << std::endl;
    }
  }

  // set up initial guess for solution
  Epetra_MultiVector& pressure = *S_->GetFieldData("pressure", passwd_)->ViewComponent("cell");
  Epetra_MultiVector& p = *solution->ViewComponent("cell");
  Epetra_MultiVector& lambda = *solution->ViewComponent("face", true);
  p = pressure;

  ResetPKtimes(T0, dT0);
  dT_desirable_ = dT0;  // The minimum desirable time step from now on.
  ti_specs.num_itrs = 0;

  // initialize diffusion operator
  SetAbsolutePermeabilityTensor();

  Teuchos::ParameterList& oplist = dp_list_.sublist("operators").sublist("diffusion operator").sublist("matrix");
  Operators::OperatorDiffusionFactory opfactory;
  op_ = opfactory.Create(mesh_, op_bc_, oplist, gravity_);
  op_->InitOperator(K, Teuchos::null, Teuchos::null, rho_, mu_);
  op_->UpdateMatrices(Teuchos::null, Teuchos::null);

  int schema_prec_dofs = op_->schema_prec_dofs();
  op_->SymbolicAssembleMatrix(schema_prec_dofs);
  op_->CreateCheckPoint();

  // Well modeling (one-time call)
  if (src_sink_distribution & Amanzi::Functions::DOMAIN_FUNCTION_ACTION_DISTRIBUTE_PERMEABILITY) {
    CalculatePermeabilityFactorInWell();
  }

  // Initialize source
  if (src_sink != NULL) {
    if (src_sink_distribution & Amanzi::Functions::DOMAIN_FUNCTION_ACTION_DISTRIBUTE_PERMEABILITY) {
      src_sink->ComputeDistribute(T0, Kxy->Values()); 
    } else {
      src_sink->ComputeDistribute(T0, NULL);
    }
  }
  
  // make initial guess consistent with boundary conditions
  if (ti_specs.initialize_with_darcy) {
    DeriveFaceValuesFromCellValues(p, lambda);

    SolveFullySaturatedProblem(T0, *solution);
    pressure = p;

    // Call this initialization procedure only once. Use case: multiple
    // restart of a single phase transient time integrator.
    ti_specs.initialize_with_darcy = false;

    if (vo_->getVerbLevel() >= Teuchos::VERB_HIGH) {
      VV_PrintHeadExtrema(*solution);
    }
  }
}


/* ******************************************************************
* Wrapper for a steady-state solver
****************************************************************** */
int Darcy_PK::AdvanceToSteadyState(double T0, double dT0)
{ 
  ti_specs = &ti_specs_sss_;
  SolveFullySaturatedProblem(T0, *solution);
  return 0;
}


/* ******************************************************************* 
* Performs one time step of size dT. The boundary conditions are 
* calculated only once, during the initialization step.  
******************************************************************* */
int Darcy_PK::Advance(double dT_MPC, double& dT_actual) 
{
  dT = dT_MPC;
  double time = S_->time();
  if (time >= 0.0) T_physics = time;

  // update boundary conditions and source terms
  time = T_physics;
  bc_pressure->Compute(time);
  bc_flux->Compute(time);
  bc_seepage->Compute(time);
  if (shift_water_table_.getRawPtr() == NULL)
    bc_head->Compute(time);
  else
    bc_head->ComputeShift(time, shift_water_table_->Values());

  if (src_sink != NULL) {
    if (src_sink_distribution & Amanzi::Functions::DOMAIN_FUNCTION_ACTION_DISTRIBUTE_PERMEABILITY) {
      src_sink->ComputeDistribute(time, Kxy->Values()); 
    } else {
      src_sink->ComputeDistribute(time, NULL);
    }
  }

  ComputeBCs(*solution);

  // calculate and assemble elemental stifness matrices
  double factor = 1.0 / g_;
  const CompositeVector& ss = *S_->GetFieldData("specific_storage");
  CompositeVector ss_g(ss); 
  ss_g.Update(0.0, ss, factor);

  factor = 1.0 / (g_ * dT);
  CompositeVector sy_g(*specific_yield_copy_); 
  sy_g.Scale(factor);

  op_->RestoreCheckPoint();
  op_->AddAccumulationTerm(*solution, ss_g, dT);
  op_->AddAccumulationTerm(*solution, sy_g);
  op_->ApplyBCs();

  int schema_prec_dofs = op_->schema_prec_dofs();
  op_->AssembleMatrix(schema_prec_dofs);
  op_->InitPreconditioner(ti_specs->preconditioner_name, preconditioner_list_);

  CompositeVector& rhs = *op_->rhs();
  if (src_sink != NULL) AddSourceTerms(rhs);

  // create linear solver
  AmanziSolvers::LinearOperatorFactory<Operators::OperatorDiffusion, CompositeVector, CompositeVectorSpace> factory;
  Teuchos::RCP<AmanziSolvers::LinearOperator<Operators::OperatorDiffusion, CompositeVector, CompositeVectorSpace> >
     solver = factory.Create(ti_specs->solver_name, linear_operator_list_, op_);

  solver->add_criteria(AmanziSolvers::LIN_SOLVER_MAKE_ONE_ITERATION);
  solver->ApplyInverse(rhs, *solution);

  ti_specs->num_itrs++;

  if (vo_->getVerbLevel() >= Teuchos::VERB_HIGH) {
    double pnorm;
    solution->Norm2(&pnorm);

    Teuchos::OSTab tab = vo_->getOSTab();
    *vo_->os() << "pressure solver (" << solver->name()
               << "): ||p,lambda||=" << pnorm << std::endl;
    VV_PrintHeadExtrema(*solution);
  }

  // calculate time derivative and 2nd-order solution approximation
  if (ti_specs->dT_method == FLOW_DT_ADAPTIVE) {
    const Epetra_MultiVector& p = *S_->GetFieldData("pressure")->ViewComponent("cell");  // pressure at t^n
    Epetra_MultiVector& p_cell = *solution->ViewComponent("cell");  // pressure at t^{n+1}

    for (int c = 0; c < ncells_owned; c++) {
      (*pdot_cells)[c] = (p_cell[0][c] - p[0][c]) / dT; 
      p_cell[0][c] = p[0][c] + ((*pdot_cells_prev)[c] + (*pdot_cells)[c]) * dT / 2;
    }
  }

  // estimate time multiplier
  if (ti_specs->dT_method == FLOW_DT_ADAPTIVE) {
    double err, dTfactor;
    err = ErrorEstimate_(&dTfactor);
    if (err > 0.0) throw 1000;  // fix (lipnikov@lan.gov)
    dT_desirable_ = std::min(dT_MPC * dTfactor, ti_specs->dTmax);
  } else {
    dT_desirable_ = std::min(dT_desirable_ * ti_specs->dTfactor, ti_specs->dTmax);
  }

  // Darcy_PK always takes suggested time step
  dT_actual = dT_MPC;

  dt_tuple times(time, dT_MPC);
  ti_specs->dT_history.push_back(times);

  return 0;
}


/* ******************************************************************
* Transfer data from the external flow state FS_MPC. MPC may request
* to populate the original state FS. 
****************************************************************** */
void Darcy_PK::CommitState(double dt, const Teuchos::RCP<State>& S)
{
  CompositeVector& p = *S_->GetFieldData("pressure", passwd_);
  p = *solution;

  // calculate darcy mass flux
  CompositeVector& darcy_flux = *S_->GetFieldData("darcy_flux", passwd_);
  op_->UpdateFlux(*solution, darcy_flux);

  Epetra_MultiVector& flux = *darcy_flux.ViewComponent("face", true);
  for (int f = 0; f < nfaces_owned; f++) flux[0][f] /= rho_;

  // update time derivative
  *pdot_cells_prev = *pdot_cells;
}


/* ******************************************************************
* Add area/length factor to specific yield. 
****************************************************************** */
void Darcy_PK::UpdateSpecificYield_()
{
  specific_yield_copy_ = Teuchos::rcp(new CompositeVector(*S_->GetFieldData("specific_yield"), true));

  // do we have non-zero specific yield? 
  double tmp;
  specific_yield_copy_->Norm2(&tmp);
  if (tmp == 0.0) return;

  // populate ghost cells
  specific_yield_copy_->ScatterMasterToGhosted();
  const Epetra_MultiVector& specific_yield = *specific_yield_copy_->ViewComponent("cell", true);

  WhetStone::MFD3D_Diffusion mfd3d(mesh_);
  AmanziMesh::Entity_ID_List faces;
  std::vector<int> dirs;

  int negative_yield = 0;
  for (int c = 0; c < ncells_owned; c++) {
    if (specific_yield[0][c] > 0.0) {
      mesh_->cell_get_faces_and_dirs(c, &faces, &dirs);

      double area = 0.0;
      int nfaces = faces.size();
      for (int n = 0; n < nfaces; n++) {
        int f = faces[n];
        int c2 = mfd3d.cell_get_face_adj_cell(c, f);

        if (c2 >= 0) {
          if (specific_yield[0][c2] <= 0.0)  // cell in the fully saturated layer
            area -= (mesh_->face_normal(f))[dim - 1] * dirs[n];
        }
      }
      specific_yield[0][c] *= area;
      if (area <= 0.0) negative_yield++;
    }
  }

#ifdef HAVE_MPI
  int negative_yield_tmp = negative_yield;
  mesh_->get_comm()->MaxAll(&negative_yield_tmp, &negative_yield, 1);
#endif
  if (negative_yield > 0) {
    Errors::Message msg;
    msg << "Flow PK: configuration of the yield region leads to negative yield interfaces.";
    Exceptions::amanzi_throw(msg);
  }
}

}  // namespace Flow
}  // namespace Amanzi
