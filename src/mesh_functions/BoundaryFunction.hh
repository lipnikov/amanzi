/*
Copyright 2010-2012 held jointly by LANS/LANL, LBNL, and PNNL. 
Amanzi is released under the three-clause BSD License. 
The terms of use and "as is" disclaimer for this license are 
provided in the top-level COPYRIGHT file.

Authors: Neil Carlson (nnc@lanl.gov)
         Konstantin Lipnikov (lipnikov@lanl.gov)
*/

#ifndef __AMANZI_BOUNDARY_FUNCTION_HH__
#define __AMANZI_BOUNDARY_FUNCTION_HH__

#include <vector>
#include <string>
#include <utility>

#include "Teuchos_RCP.hpp"

#include "Mesh.hh"
#include "Function.hh"
#include "MeshFunction.hh"


namespace Amanzi {

const int BOUNDARY_FUNCTION_ACTION_NONE = 0;
const int BOUNDARY_FUNCTION_ACTION_HEAD_RELATIVE = 1;

typedef std::pair<std::string, int> Action;

class BoundaryFunction : public MeshFunction {
 public:
  BoundaryFunction(const Teuchos::RCP<const AmanziMesh::Mesh>& mesh) { mesh_ = mesh; }

  void Define(const std::vector<std::string>& regions,
              const Teuchos::RCP<const Function>& f, int action);
  void Compute(double T);
  void ComputeShift(double T, double* shift);

  const std::vector<Action>& actions() { return actions_; } 

 private:
  std::vector<Action> actions_;
};

} // namespace Amanzi

#endif  // AMANZI_BOUNDARY_FUNCTION_HH_