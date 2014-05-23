/*
  This is the operators component of the Amanzi code. 

  Copyright 2010-2012 held jointly by LANS/LANL, LBNL, and PNNL. 
  Amanzi is released under the three-clause BSD License. 
  The terms of use and "as is" disclaimer for this license are 
  provided in the top-level COPYRIGHT file.

  Author: Konstantin Lipnikov (lipnikov@lanl.gov)
*/

#include <vector>

#include "Epetra_Vector.h"
#include "Epetra_FECrsGraph.h"

#include "errors.hh"
#include "WhetStoneDefs.hh"
#include "mfd3d_diffusion.hh"

#include "PreconditionerFactory.hh"
#include "OperatorDefs.hh"
#include "OperatorDiffusion.hh"


namespace Amanzi {
namespace Operators {

/* ******************************************************************
* Initialization of the operator.                                           
****************************************************************** */
void OperatorDiffusion::InitOperator(
    std::vector<WhetStone::Tensor>& K, 
    Teuchos::RCP<const CompositeVector> k, Teuchos::RCP<const CompositeVector> dkdp,
    double rho, double mu)
{
  K_ = &K;
  k_ = k;
  dkdp_ = dkdp;

  rho_ = rho;
  mu_ = mu;
  scalar_rho_mu_ = true;

  if (schema_ == OPERATOR_SCHEMA_BASE_CELL + OPERATOR_SCHEMA_DOFS_FACE + OPERATOR_SCHEMA_DOFS_CELL) {
    CreateMassMatrices_();
  }
}


/* ******************************************************************
* Initialization of the operator.                                           
****************************************************************** */
void OperatorDiffusion::InitOperator(
    std::vector<WhetStone::Tensor>& K,
    Teuchos::RCP<const CompositeVector> k, Teuchos::RCP<const CompositeVector> dkdp,
    Teuchos::RCP<const CompositeVector> rho, Teuchos::RCP<const CompositeVector> mu)
{
  K_ = &K;
  k_ = k;
  dkdp_ = dkdp;

  rho_cv_ = rho;
  mu_cv_ = mu;
  scalar_rho_mu_ = false;

  if (schema_ == OPERATOR_SCHEMA_BASE_CELL + OPERATOR_SCHEMA_DOFS_FACE + OPERATOR_SCHEMA_DOFS_CELL) {
    CreateMassMatrices_();
  }
}


/* ******************************************************************
* Calculate elemental matrices.
****************************************************************** */
void OperatorDiffusion::UpdateMatrices(Teuchos::RCP<const CompositeVector> flux)
{
  if (schema_dofs_ == OPERATOR_SCHEMA_DOFS_NODE) {
    UpdateMatricesNodal_();
  } else if (schema_dofs_ == OPERATOR_SCHEMA_DOFS_CELL + OPERATOR_SCHEMA_DOFS_FACE) {
    UpdateMatricesMixed_(flux);
  } else if (schema_dofs_ == OPERATOR_SCHEMA_DOFS_CELL) {
    UpdateMatricesTPFA_();
  }
}


/* ******************************************************************
* Basic routine of each operator: creation of matrices.
****************************************************************** */
void OperatorDiffusion::UpdateMatricesMixed_(Teuchos::RCP<const CompositeVector> flux)
{
  // find location of matrix blocks
  int schema_dofs = OPERATOR_SCHEMA_DOFS_CELL + OPERATOR_SCHEMA_DOFS_FACE;
  int m(0), nblocks = blocks_.size();
  bool flag(false);

  for (int n = 0; n < nblocks; n++) {
    int schema = blocks_schema_[n];
    if ((schema & schema_dofs) == schema_dofs) {
      m = n;
      flag = true;
      break;
    }
  }

  if (flag == false) { 
    m = nblocks++;
    blocks_schema_.push_back(OPERATOR_SCHEMA_BASE_CELL + OPERATOR_SCHEMA_DOFS_FACE + OPERATOR_SCHEMA_DOFS_CELL);
    blocks_.push_back(Teuchos::rcp(new std::vector<WhetStone::DenseMatrix>));
    blocks_shadow_.push_back(Teuchos::rcp(new std::vector<WhetStone::DenseMatrix>));
  }
  std::vector<WhetStone::DenseMatrix>& matrix = *blocks_[m];
  std::vector<WhetStone::DenseMatrix>& matrix_shadow = *blocks_shadow_[m];
  WhetStone::DenseMatrix null_matrix;

  // update matrix blocks
  AmanziMesh::Entity_ID_List faces;
  std::vector<int> dirs;

  for (int c = 0; c < ncells_owned; c++) {
    mesh_->cell_get_faces_and_dirs(c, &faces, &dirs);
    int nfaces = faces.size();

    WhetStone::DenseMatrix& Wff = Wff_cells_[c];
    WhetStone::DenseMatrix Acell(nfaces + 1, nfaces + 1);

    // Update terms due to nonlinear coefficient
    double kc(1.0); 
    if (k_ != Teuchos::null) {
      const Epetra_MultiVector& k_cell = *k_->ViewComponent("cell");
      kc = k_cell[0][c];
    }

    double matsum = 0.0;  // elimination of mass matrix
    for (int n = 0; n < nfaces; n++) {
      double rowsum = 0.0;
      for (int m = 0; m < nfaces; m++) {
        double tmp = Wff(n, m) * kc;
        rowsum += tmp;
        Acell(n, m) = tmp;
      }

      Acell(n, nfaces) = -rowsum;
      Acell(nfaces, n) = -rowsum;
      matsum += rowsum;
    }
    Acell(nfaces, nfaces) = matsum;

    // Update terms due to dependence of k on the solution.
    if (flux !=  Teuchos::null && k_ != Teuchos::null) {
      const Epetra_MultiVector& k_face = *k_->ViewComponent("face");
      const Epetra_MultiVector& dkdp_face = *dkdp_->ViewComponent("face");

      const Epetra_MultiVector& flux_data = *flux->ViewComponent("face", true);
      for (int n = 0; n < nfaces; n++) {
        int f = faces[n];
        double dkf = dkdp_face[0][f];
        double  kf = k_face[0][f];
        double alpha = (dkf / kf) * flux_data[0][f] * dirs[n];
        if (alpha > 0) {
          Acell(n, n) += kc * alpha;
        }
      }
    }

    if (flag) {
      matrix[c] += Acell;
    } else {
      matrix.push_back(Acell);
      matrix_shadow.push_back(null_matrix);
    }
  }
}


/* ******************************************************************
* Calculate elemental inverse mass matrices.                                           
****************************************************************** */
void OperatorDiffusion::UpdateMatricesNodal_()
{
  // find location of matrix blocks
  int m(0), nblocks = blocks_.size();
  bool flag(false);

  for (int nb = 0; nb < nblocks; nb++) {
    int schema = blocks_schema_[nb];
    if (schema == OPERATOR_SCHEMA_BASE_CELL + OPERATOR_SCHEMA_DOFS_NODE) {
      m = nb;
      flag = true;
      break;
    }
  }

  if (flag == false) { 
    m = nblocks++;
    blocks_schema_.push_back(OPERATOR_SCHEMA_BASE_CELL + OPERATOR_SCHEMA_DOFS_NODE);
    blocks_.push_back(Teuchos::rcp(new std::vector<WhetStone::DenseMatrix>));
    blocks_shadow_.push_back(Teuchos::rcp(new std::vector<WhetStone::DenseMatrix>));
  }
  std::vector<WhetStone::DenseMatrix>& matrix = *blocks_[m];
  std::vector<WhetStone::DenseMatrix>& matrix_shadow = *blocks_shadow_[m];
  WhetStone::DenseMatrix null_matrix;

  // update matrix blocks
  int dim = mesh_->space_dimension();
  WhetStone::MFD3D_Diffusion mfd(mesh_);
  mfd.ModifyStabilityScalingFactor(factor_);

  AmanziMesh::Entity_ID_List nodes;

  for (int c = 0; c < ncells_owned; c++) {
    mesh_->cell_get_nodes(c, &nodes);
    int nnodes = nodes.size();

    WhetStone::DenseMatrix Acell(nnodes, nnodes);
    int ok = mfd.StiffnessMatrix(c, (*K_)[c], Acell);

    if (ok == WhetStone::WHETSTONE_ELEMENTAL_MATRIX_FAILED) {
      Errors::Message msg("Stiffness_MFD: unexpected failure of LAPACK in WhetStone.");
      Exceptions::amanzi_throw(msg);
    }

    if (flag) {
      matrix[c] += Acell;
    } else {
      matrix.push_back(Acell);
      matrix_shadow.push_back(null_matrix);
    }
  }
}


/* ******************************************************************
* Calculate and assemble fluxes using the TPFA scheme.
****************************************************************** */
void OperatorDiffusion::UpdateMatricesTPFA_()
{
  // find location of matrix blocks
  int m(0), nblocks = blocks_.size();
  bool flag(false);

  for (int nb = 0; nb < nblocks; nb++) {
    int schema = blocks_schema_[nb];
    if (schema == OPERATOR_SCHEMA_BASE_FACE + OPERATOR_SCHEMA_DOFS_CELL) {
      m = nb;
      flag = true;
      break;
    }
  }

  if (flag == false) { 
    m = nblocks++;
    blocks_schema_.push_back(OPERATOR_SCHEMA_BASE_FACE + OPERATOR_SCHEMA_DOFS_CELL);
    blocks_.push_back(Teuchos::rcp(new std::vector<WhetStone::DenseMatrix>));
    blocks_shadow_.push_back(Teuchos::rcp(new std::vector<WhetStone::DenseMatrix>));
  }
  std::vector<WhetStone::DenseMatrix>& matrix = *blocks_[m];
  std::vector<WhetStone::DenseMatrix>& matrix_shadow = *blocks_shadow_[m];
  WhetStone::DenseMatrix null_matrix;

  // populate transmissibilities
  WhetStone::MFD3D_Diffusion mfd(mesh_);

  CompositeVectorSpace cv_space;
  cv_space.SetMesh(mesh_);
  cv_space.SetGhosted(true);
  cv_space.SetComponent("face", AmanziMesh::FACE, 1);

  Teuchos::RCP<CompositeVector> T = Teuchos::RCP<CompositeVector>(new CompositeVector(cv_space, true));
  Epetra_MultiVector& Ttmp = *T->ViewComponent("face", true);

  AmanziMesh::Entity_ID_List cells, faces;
  Ttmp.PutScalar(0.0);
  for (int c = 0; c < ncells_owned; c++) {
    mesh_->cell_get_faces(c, &faces);
    int nfaces = faces.size();

    WhetStone::DenseMatrix Mff(nfaces, nfaces);
    mfd.MassMatrixInverseTPFA(c, (*K_)[c], Mff);
   
    for (int n = 0; n < nfaces; n++) {
      int f = faces[n];
      Ttmp[0][f] += 1.0 / Mff(n, n);
    }
  }
  T->GatherGhostedToMaster();
 
  // populate the global matrix

  for (int f = 0; f < nfaces_owned; f++) {
    mesh_->face_get_cells(f, AmanziMesh::USED, &cells);
    int ncells = cells.size();
    WhetStone::DenseMatrix Aface(ncells, ncells);

    if (ncells == 2) {
      double coef = 1.0 / Ttmp[0][f];
      Aface(0, 0) =  coef;
      Aface(1, 1) =  coef;
      Aface(0, 1) = -coef;
      Aface(1, 0) = -coef;
    } else {
      Aface(0, 0) = 0.0;
    }

    if (flag) {
      matrix[f] += Aface;
    } else {
      matrix.push_back(Aface);
      matrix_shadow.push_back(null_matrix);
    }
  }
}


/* ******************************************************************
* A small factory for assembling of matrices for preconditioners.
****************************************************************** */
void OperatorDiffusion::AssembleMatrix(int schema)
{
  if (special_assembling_) {
    // We do not need it since preconditoner creates independent matrix.
    // AssembleMatrixSpecial_();
  } else {
    Operator::AssembleMatrix(schema);
  }
}


/* ******************************************************************
* Special assemble of elemental face-based matrices. 
****************************************************************** */
void OperatorDiffusion::ModifyMatrix(const CompositeVector& u)
{
  if (schema_dofs_ != OPERATOR_SCHEMA_DOFS_CELL + OPERATOR_SCHEMA_DOFS_FACE) {
    std::cout << "Schema " << schema_dofs_ << " is not supported" << std::endl;
    ASSERT(0);
  }

  // find location of face-based matrices
  int m(0), nblocks = blocks_.size();
  for (int nb = 0; nb < nblocks; nb++) {
    if (blocks_schema_[nb] == schema_) {
      m = nb;
      break;
    }
  }
  std::vector<WhetStone::DenseMatrix>& matrix = *blocks_[m];

  // populate the matrix
  AmanziMesh::Entity_ID_List faces;
  const Epetra_MultiVector& u_c = *u.ViewComponent("cell");
  Epetra_MultiVector& rhs_f = *rhs_->ViewComponent("face", true);

  for (int f = nfaces_owned; f < nfaces_wghost; f++) {
    rhs_f[0][f] = 0.0;
  }

  for (int c = 0; c < ncells_owned; c++) {
    mesh_->cell_get_faces(c, &faces);
    int nfaces = faces.size();

    WhetStone::DenseMatrix& Acell = matrix[c];

    for (int n = 0; n < nfaces; n++) {
      int f = faces[n];
      rhs_f[0][f] -= Acell(n, nfaces) * u_c[0][c];
      Acell(n, nfaces) = 0.0;
      Acell(nfaces, n) = 0.0;
    }
  }

  // Assemble all right-hand sides
  rhs_->GatherGhostedToMaster("face", Add);
}


/* ******************************************************************
* The cell-based and face-based d.o.f. are packed together into 
* the X and Y vectors.
****************************************************************** */
int OperatorDiffusion::ApplyInverse(const CompositeVector& X, CompositeVector& Y) const
{
  int ierr;
  if (special_assembling_) {
    ierr = ApplyInverseSpecial_(X, Y);
  } else {
    ierr = Operator::ApplyInverse(X, Y);
  }
  return ierr;
}

 
/* ******************************************************************
* The cell-based and face-based d.o.f. are packed together into 
* the X and Y vectors.
****************************************************************** */
int OperatorDiffusion::ApplyInverseSpecial_(const CompositeVector& X, CompositeVector& Y) const
{
  // Y = X;
  // return 0;

  // find the block of matrices
  int m, nblocks = blocks_.size();
  for (int nb = 0; nb < nblocks; nb++) {
    int schema = blocks_schema_[nb];
    if ((schema & OPERATOR_SCHEMA_DOFS_FACE) && (schema & OPERATOR_SCHEMA_DOFS_CELL)) {
      m = nb;
      break;
    }
  }
  std::vector<WhetStone::DenseMatrix>& matrix = *blocks_[m];

  // apply preconditioner inversion
  const Epetra_MultiVector& Xc = *X.ViewComponent("cell");
  const Epetra_MultiVector& Xf = *X.ViewComponent("face", true);

  Epetra_MultiVector& Yc = *Y.ViewComponent("cell");
  Epetra_MultiVector& Yf = *Y.ViewComponent("face", true);

  // Temporary cell and face vectors.
  CompositeVector T(X);
  Epetra_MultiVector& Tf = *T.ViewComponent("face", true);

  // FORWARD ELIMINATION:  Tf = Xf - Afc inv(Acc) Xc
  AmanziMesh::Entity_ID_List faces;
  Epetra_MultiVector& diag = *diagonal_->ViewComponent("cell");

  for (int f = nfaces_owned; f < nfaces_wghost; f++) {
    Tf[0][f] = 0.0;
  }

  for (int c = 0; c < ncells_owned; c++) {
    mesh_->cell_get_faces(c, &faces);
    int nfaces = faces.size();

    WhetStone::DenseMatrix& Acell = matrix[c];

    double tmp = Xc[0][c] / (Acell(nfaces, nfaces) + diag[0][c]);
    for (int n = 0; n < nfaces; n++) {
      int f = faces[n];
      Tf[0][f] -= Acell(n, nfaces) * tmp;
    }
  }

  // Solve the Schur complement system Sff * Yf = Tf.
  T.GatherGhostedToMaster("face", Add);

  preconditioner_->ApplyInverse(Tf, Yf);

  Y.ScatterMasterToGhosted("face");

  // BACKWARD SUBSTITUTION:  Yc = inv(Acc) (Xc - Acf Yf)
  for (int c = 0; c < ncells_owned; c++) {
    mesh_->cell_get_faces(c, &faces);
    int nfaces = faces.size();

    WhetStone::DenseMatrix& Acell = matrix[c];

    double tmp = Xc[0][c];
    for (int n = 0; n < nfaces; n++) {
      int f = faces[n];
      tmp -= Acell(nfaces, n) * Yf[0][f];
    }
    Yc[0][c] = tmp / (Acell(nfaces, nfaces) + diag[0][c]);
  }

  return 0;
}


/* ******************************************************************
* Initialization of the preconditioner                                                 
****************************************************************** */
void OperatorDiffusion::InitPreconditioner(
    const std::string& prec_name, const Teuchos::ParameterList& plist,
    std::vector<int>& bc_model, std::vector<double>& bc_values)
{
  if (special_assembling_) { 
#ifdef OPERATORS_MATRIX_FE_CRS
    InitPreconditionerSpecialFE_(prec_name, plist, bc_model, bc_values);
#else
    InitPreconditionerSpecialCRS_(prec_name, plist, bc_model, bc_values);
#endif
  } else {
    Operator::InitPreconditioner(prec_name, plist, bc_model, bc_values);
  }
}


#ifdef OPERATORS_MATRIX_FE_CRS
/* ******************************************************************
* Routine assembles the Schur complement for face-based degrees 
* of freedom.
****************************************************************** */
void OperatorDiffusion::InitPreconditionerSpecialFE_(
    const std::string& prec_name, const Teuchos::ParameterList& plist,
    std::vector<int>& bc_model, std::vector<double>& bc_values)
{
  // find the block of matrices
  int schema_dofs = OPERATOR_SCHEMA_DOFS_FACE + OPERATOR_SCHEMA_DOFS_CELL;
  int m(0), nblocks = blocks_schema_.size();
  for (int nb = 0; nb < nblocks; nb++) {
    int schema = blocks_schema_[nb];
    if (schema & schema_dofs) {
      m = nb;
      break;
    }
  }
  std::vector<WhetStone::DenseMatrix>& matrix = *blocks_[m];

  // create a face-based stiffness matrix from A.
  A_->PutScalar(0.0);

  const Epetra_Map& fmap_wghost = mesh_->face_map(true);
  AmanziMesh::Entity_ID_List faces;
  int gid[OPERATOR_MAX_FACES];

  Epetra_MultiVector& diag = *diagonal_->ViewComponent("cell");

  for (int c = 0; c < ncells_owned; c++) {
    mesh_->cell_get_faces(c, &faces);
    int nfaces = faces.size();

    WhetStone::DenseMatrix Scell(nfaces, nfaces);
    WhetStone::DenseMatrix& Acell = matrix[c];

    double tmp = Acell(nfaces, nfaces) + diag[0][c];
    for (int n = 0; n < nfaces; n++) {
      for (int m = 0; m < nfaces; m++) {
        Scell(n, m) = Acell(n, m) - Acell(n, nfaces) * Acell(nfaces, m) / tmp;
      }
    }

    for (int n = 0; n < nfaces; n++) {  // Symbolic boundary conditions
      int f = faces[n];
      if (bc_model[f] == OPERATOR_BC_FACE_DIRICHLET) {
        for (int m = 0; m < nfaces; m++) Scell(n, m) = Scell(m, n) = 0.0;
        Scell(n, n) = 1.0;
      }
    }

    for (int n = 0; n < nfaces; n++) {
      gid[n] = fmap_wghost.GID(faces[n]);
    }
    A_->SumIntoGlobalValues(nfaces, gid, Scell.Values());
  }
  A_->GlobalAssemble();

  // redefine (if necessary) preconditioner since only 
  // one preconditioner is allowed.
  AmanziPreconditioners::PreconditionerFactory factory;
  preconditioner_ = factory.Create(prec_name, plist);
  preconditioner_->Update(A_);
}


#else
/* ******************************************************************
* Routine assembles the Schur complement for face-based degrees 
* of freedom.
****************************************************************** */
void OperatorDiffusion::InitPreconditionerSpecialCRS_(
    const std::string& prec_name, const Teuchos::ParameterList& plist,
    std::vector<int>& bc_model, std::vector<double>& bc_values)
{
  // find the block of matrices
  int schema_dofs = OPERATOR_SCHEMA_DOFS_FACE + OPERATOR_SCHEMA_DOFS_CELL;
  int m(0), nblocks = blocks_schema_.size();
  for (int nb = 0; nb < nblocks; nb++) {
    int schema = blocks_schema_[nb];
    if (schema & schema_dofs) {
      m = nb;
      break;
    }
  }
  std::vector<WhetStone::DenseMatrix>& matrix = *blocks_[m];

  // create a face-based stiffness matrix from A.
  A_->PutScalar(0.0);

  const Epetra_Map& fmap_wghost = mesh_->face_map(true);
  AmanziMesh::Entity_ID_List faces;

  int gid[OPERATOR_MAX_FACES];
  double values[OPERATOR_MAX_FACES];

  Epetra_MultiVector& diag = *diagonal_->ViewComponent("cell");

  for (int c = 0; c < ncells_owned; c++) {
    mesh_->cell_get_faces(c, &faces);
    int nfaces = faces.size();

    WhetStone::DenseMatrix Scell(nfaces, nfaces);
    WhetStone::DenseMatrix& Acell = matrix[c];

    double tmp = Acell(nfaces, nfaces) + diag[0][c];
    for (int n = 0; n < nfaces; n++) {
      for (int m = 0; m < nfaces; m++) {
        Scell(n, m) = Acell(n, m) - Acell(n, nfaces) * Acell(nfaces, m) / tmp;
      }
    }

    for (int n = 0; n < nfaces; n++) {  // Symbolic boundary conditions
      int f = faces[n];
      if (bc_model[f] == OPERATOR_BC_FACE_DIRICHLET) {
        for (int m = 0; m < nfaces; m++) Scell(n, m) = Scell(m, n) = 0.0;
        Scell(n, n) = 1.0;
      }
    }

    for (int n = 0; n < nfaces; n++) {
      gid[n] = fmap_wghost.GID(faces[n]);
    }
    for (int n = 0; n < nfaces; n++) {
      for (int m = 0; m < nfaces; m++) values[m] = Scell(n, m);
      A_->SumIntoMyValues(gid[n], nfaces, values, gid);
    }

  }
  A_->FillComplete();

  // redefine (if necessary) preconditioner since only 
  // one preconditioner is allowed.
  AmanziPreconditioners::PreconditionerFactory factory;
  preconditioner_ = factory.Create(prec_name, plist);
  preconditioner_->Update(A_);
}
#endif


/* ******************************************************************
* WARNING: Since diffusive flux is not continuous, we derive it only
* once (using flag) and in exactly the same manner as other routines.
* **************************************************************** */
void OperatorDiffusion::UpdateFlux(const CompositeVector& u, CompositeVector& flux)
{
  // find location of face-based matrices
  int schema_dofs = OPERATOR_SCHEMA_DOFS_CELL + OPERATOR_SCHEMA_DOFS_FACE;
  int m(0), nblocks = blocks_.size();
  for (int nb = 0; nb < nblocks; nb++) {
    if (blocks_schema_[nb] & schema_dofs) {
      m = nb;
      break;
    }
  }
  std::vector<WhetStone::DenseMatrix>& matrix = *blocks_[m];
  std::vector<WhetStone::DenseMatrix>& matrix_shadow = *blocks_shadow_[m];

  // Initialize intensity in ghost faces.
  flux.PutScalar(0.0);
  u.ScatterMasterToGhosted("face");

  const Epetra_MultiVector& u_cells = *u.ViewComponent("cell");
  const Epetra_MultiVector& u_faces = *u.ViewComponent("face", true);
  Epetra_MultiVector& flux_data = *flux.ViewComponent("face", true);

  AmanziMesh::Entity_ID_List faces;
  std::vector<int> dirs;
  std::vector<int> flag(nfaces_wghost, 0);

  for (int c = 0; c < ncells_owned; c++) {
    mesh_->cell_get_faces_and_dirs(c, &faces, &dirs);
    int nfaces = faces.size();

    WhetStone::DenseVector v(nfaces + 1), av(nfaces + 1);
    for (int n = 0; n < nfaces; n++) {
      v(n) = u_faces[0][faces[n]];
    }
    v(nfaces) = u_cells[0][c];

    if (matrix_shadow[c].NumRows() == 0) { 
      WhetStone::DenseMatrix& Acell = matrix[c];
      Acell.Multiply(v, av, false);
    } else {
      WhetStone::DenseMatrix& Acell = matrix_shadow[c];
      Acell.Multiply(v, av, false);
    }

    for (int n = 0; n < nfaces; n++) {
      int f = faces[n];
      if (f < nfaces_owned && !flag[f]) {
        flux_data[0][f] -= av(n) * dirs[n];
        flag[f] = 1;
      }
    }
  }
}


/* ******************************************************************
* Calculate elemental inverse mass matrices.
****************************************************************** */
void OperatorDiffusion::CreateMassMatrices_()
{
  WhetStone::MFD3D_Diffusion mfd(mesh_);
  mfd.ModifyStabilityScalingFactor(factor_);

  bool surface_mesh = (mesh_->cell_dimension() != mesh_->space_dimension());
  AmanziMesh::Entity_ID_List faces;

  Wff_cells_.clear();

  for (int c = 0; c < ncells_owned; c++) {
    mesh_->cell_get_faces(c, &faces);
    int nfaces = faces.size();

    int ok;
    WhetStone::Tensor& Kc = (*K_)[c];
    WhetStone::DenseMatrix Wff(nfaces, nfaces);
    if (surface_mesh) {
      ok = mfd.MassMatrixInverseSurface(c, Kc, Wff);
    } else {
      int method = mfd_primary_;
      ok = WhetStone::WHETSTONE_ELEMENTAL_MATRIX_FAILED;

      // try primary and then secondary discretization methods.
      if (method == WhetStone::DIFFUSION_HEXAHEDRA_MONOTONE) {
        ok = mfd.MassMatrixInverseMMatrixHex(c, Kc, Wff);
        method = mfd_secondary_;
      } else if (method == WhetStone::DIFFUSION_POLYHEDRA_MONOTONE) {
        ok = mfd.MassMatrixInverseMMatrix(c, Kc, Wff);
        method = mfd_secondary_;
      }

      if (ok != WhetStone::WHETSTONE_ELEMENTAL_MATRIX_OK) {
        if (method == WhetStone::DIFFUSION_OPTIMIZED_SCALED) {
          ok = mfd.MassMatrixInverseOptimizedScaled(c, Kc, Wff);
        } else if(method == WhetStone::DIFFUSION_TPFA) {
          ok = mfd.MassMatrixInverseTPFA(c, Kc, Wff);
        } else if(method == WhetStone::DIFFUSION_SUPPORT_OPERATOR) {
          ok = mfd.MassMatrixInverseSO(c, Kc, Wff);
        } else if(method == WhetStone::DIFFUSION_POLYHEDRA_SCALED) {
          ok = mfd.MassMatrixInverseScaled(c, Kc, Wff);
        }
      }
    }

    if (scalar_rho_mu_) {
      Wff *= rho_ / mu_;
    } else {
      const Epetra_MultiVector& rho = *rho_cv_->ViewComponent("cell");
      const Epetra_MultiVector& mu = *mu_cv_->ViewComponent("cell");
      Wff *= rho[0][c] / mu[0][c];
    }

    Wff_cells_.push_back(Wff);

    if (ok == WhetStone::WHETSTONE_ELEMENTAL_MATRIX_FAILED) {
      Errors::Message msg("OperatorDiffusion: unexpected failure in WhetStone.");
      Exceptions::amanzi_throw(msg);
    }
  }
}


/* ******************************************************************
* Put here stuff that has to be done in constructor.
****************************************************************** */
void OperatorDiffusion::InitDiffusion_(const Teuchos::ParameterList& plist)
{
  // Define stencil for the MFD diffusion method.
  std::vector<std::string> names;
  names = plist.get<Teuchos::Array<std::string> > ("schema").toVector();

  schema_dofs_ = 0;
  for (int i = 0; i < names.size(); i++) {
    if (names[i] == "cell") {
      schema_dofs_ += OPERATOR_SCHEMA_DOFS_CELL;
    } else if (names[i] == "node") {
      schema_dofs_ += OPERATOR_SCHEMA_DOFS_NODE;
    } else if (names[i] == "face") {
      schema_dofs_ += OPERATOR_SCHEMA_DOFS_FACE;
    }
  }

  // define stencil for preconditionre
  if (plist.isParameter("preconditioner schema")) {
    names = plist.get<Teuchos::Array<std::string> > ("preconditioner schema").toVector();

    schema_prec_dofs_ = 0;
    for (int i = 0; i < names.size(); i++) {
      if (names[i] == "cell") {
        schema_prec_dofs_ += OPERATOR_SCHEMA_DOFS_CELL;
      } else if (names[i] == "node") {
        schema_prec_dofs_ += OPERATOR_SCHEMA_DOFS_NODE;
      } else if (names[i] == "face") {
        schema_prec_dofs_ += OPERATOR_SCHEMA_DOFS_FACE;
      }
    } 
  } else {
    schema_prec_dofs_ = schema_dofs_;
  }

  special_assembling_ = false;
  if (schema_prec_dofs_ != schema_dofs_) special_assembling_ = true;

  // Define base for assembling.
  std::string primary = plist.get<std::string>("discretization primary");
  std::string secondary = plist.get<std::string>("discretization secondary");

  schema_base_ = 0;
  if (primary == "two point flux approximation") {
    schema_base_ = OPERATOR_SCHEMA_BASE_FACE;
  } else {
    schema_base_ = OPERATOR_SCHEMA_BASE_CELL;
  }

  // Primary discretization methods
  if (primary == "monotone mfd hex") {
    mfd_primary_ = WhetStone::DIFFUSION_HEXAHEDRA_MONOTONE;
  } else if (primary == "monotone mfd") {
    mfd_primary_ = WhetStone::DIFFUSION_POLYHEDRA_MONOTONE;
  } else if (primary == "two point flux approximation") {
    mfd_primary_ = WhetStone::DIFFUSION_TPFA;
  } else if (primary == "optimized mfd scaled") {
    mfd_primary_ = WhetStone::DIFFUSION_OPTIMIZED_SCALED;
  } else if (primary == "support operator") {
    mfd_primary_ = WhetStone::DIFFUSION_SUPPORT_OPERATOR;
  } else if (primary == "mfd scaled") {
    mfd_primary_ = WhetStone::DIFFUSION_POLYHEDRA_SCALED;
  } else {
    Errors::Message msg("OperatorDiffusion: primary discretization method is not supported.");
    Exceptions::amanzi_throw(msg);
  }

  // Secondary discretization methods
  if (secondary == "two point flux approximation") {
    mfd_secondary_ = WhetStone::DIFFUSION_TPFA;
  } else if (secondary == "optimized mfd scaled") {
    mfd_secondary_ = WhetStone::DIFFUSION_OPTIMIZED_SCALED;
  } else if (secondary == "support operator") {
    mfd_secondary_ = WhetStone::DIFFUSION_SUPPORT_OPERATOR;
  } else if (primary == "mfd scaled") {
    mfd_primary_ = WhetStone::DIFFUSION_POLYHEDRA_SCALED;
  } else {
    Errors::Message msg("OperatorDiffusion: secondary discretization method is not supported.");
    Exceptions::amanzi_throw(msg);
  }

  // Define other parameters.
  schema_ = schema_base_ + schema_dofs_;
  factor_ = 1.0;
}

}  // namespace Operators
}  // namespace Amanzi

