/*
  This is the energy component of the ATS / Amanzi code.

  Copyright 2010-201x held jointly by LANS/LANL, LBNL, and PNNL. 
  Amanzi is released under the three-clause BSD License. 
  The terms of use and "as is" disclaimer for this license are 
  provided in the top-level COPYRIGHT file.

  Author: Ethan Coon

  Linear interpolant of thermal conductivity.
*/

#include "twophase_thermal_conductivity_peterslidard.hh"

namespace Amanzi {
namespace Energy {

// registry of method
Utils::RegisteredFactory<ThermalConductivityTwoPhase, ThermalConductivityTwoPhasePetersLidard>
    ThermalConductivityTwoPhasePetersLidard::factory_("two-phase Peters-Lidard");

}  // namespace Energy
}  // namespace Amanzi