/* -*-  mode: c++; c-default-style: "google"; indent-tabs-mode: nil -*- */
#include <cstdlib>
#include <cmath>
#include <iostream>
#include <vector>
#include <typeinfo>

#include <UnitTest++.h>

#include "Teuchos_RCP.hpp"
#include "Teuchos_ParameterList.hpp"
#include "Teuchos_ParameterXMLFileReader.hpp"
#include "Epetra_SerialComm.h"

#include "InputParserIS.hh"
#include "MeshFactory.hh"
#include "GenerationSpec.hh"
#include "State_Old.hh"

#include "chemistry_pk.hh"
#include "chemistry_state.hh"
#include "species.hh"
#include "chemistry_exception.hh"

#include "dbc.hh"
#include "errors.hh"
#include "exceptions.hh"

/*****************************************************************************
 **
 **  Tests for trilinos based chemistry process kernel in chemistry-pk.cc
 **
 *****************************************************************************/

SUITE(GeochemistryTestsChemistryPK) {
  namespace ac = amanzi::chemistry;
  namespace ag = Amanzi::AmanziGeometry;
  namespace am = Amanzi::AmanziMesh;

  /*****************************************************************************
   **
   **  Common testing code
   **
   *****************************************************************************/
  class ChemistryPKTest {
   public:
    ChemistryPKTest();
    ~ChemistryPKTest();
 
    void RunTest(const std::string name, double* gamma);

   protected:
    ac::Chemistry_PK* cpk_;
    Teuchos::ParameterList chemistry_parameter_list_;
    Teuchos::RCP<ac::Chemistry_State> chemistry_state_;

   private:
    Epetra_SerialComm* comm_;
    ag::GeometricModelPtr gm_;
    Teuchos::RCP<Amanzi::AmanziMesh::Mesh> mesh_;
    Teuchos::RCP<State_Old> state_;
  };  // end class SpeciationTest

  ChemistryPKTest::ChemistryPKTest() {
    // assume that no errors or exceptions will occur in the
    // mesh/state related code....
    
    // get the parameter list from the input file.
    std::string xml_input_filename("test_chemistry_pk.xml");
    
    Teuchos::ParameterXMLFileReader xmlreader(xml_input_filename);
    Teuchos::ParameterList input_spec(xmlreader.getParameters());

    // Chemistry uses the official input spec, not the unstructured
    // native, but we need to translate for state.
    Teuchos::ParameterList parameter_list;
    parameter_list = Amanzi::AmanziInput::translate(&input_spec, 1);
    
    //std::cout << input_spec << std::endl;
    //std::cout << parameter_list << std::endl;

    // create a test mesh
    comm_ = new Epetra_SerialComm();
    Teuchos::ParameterList mesh_parameter_list =
        parameter_list.sublist("Mesh").sublist("Unstructured").sublist("Generate Mesh");
    am::GenerationSpec g(mesh_parameter_list);
    
    Teuchos::ParameterList region_parameter_list = parameter_list.sublist("Regions");
    gm_ = 
        new ag::GeometricModel(3, region_parameter_list, (const Epetra_MpiComm *)comm_);
  
    am::FrameworkPreference pref;
    pref.clear();
    pref.push_back(am::Simple);

    am::MeshFactory meshfactory((Epetra_MpiComm *)comm_);
    meshfactory.preference(pref);

    mesh_ = meshfactory(mesh_parameter_list, gm_);

    // get the state parameter list and create the state object
    Teuchos::ParameterList state_parameter_list = parameter_list.sublist("State");
    state_ = Teuchos::rcp(new State_Old(state_parameter_list, mesh_));
 
    // create the chemistry state object from the state
    chemistry_state_ = Teuchos::rcp(new ac::Chemistry_State(state_));
 
    // create the chemistry parameter list
    chemistry_parameter_list_ = parameter_list.sublist("Chemistry");
  }

  ChemistryPKTest::~ChemistryPKTest() {
    //delete cpk_;
    delete comm_;
    delete gm_;
  }

  void ChemistryPKTest::RunTest(const std::string name, double * gamma) {
  }  // end ChemistryPKTest::RunTest()

  /*****************************************************************************
   **
   **  individual tests
   **
   *****************************************************************************/
  TEST_FIXTURE(ChemistryPKTest, ChemistryPK_constructor) {
    // just make sure that we can have all the pieces together to set
    // up a chemistry process kernel....
    try {
      cpk_ = new ac::Chemistry_PK(chemistry_parameter_list_, chemistry_state_);
    } catch (ac::ChemistryException chem_error) {
      std::cout << chem_error.what() << std::endl;
    } catch (std::exception e) {
      std::cout << e.what() << std::endl;
    }
    // debug flag should be set after the constructor is finished....
    CHECK_EQUAL(false, cpk_->debug());
  }  // end TEST_FIXTURE()

  TEST_FIXTURE(ChemistryPKTest, ChemistryPK_initialize) {
    // make sure that we can initialize the pk and internal chemistry
    // object correctly based on the xml input....
    try {
      cpk_ = new ac::Chemistry_PK(chemistry_parameter_list_, chemistry_state_);
      amanzi::chemistry::chem_out->AddLevel("silent");
      cpk_->InitializeChemistry();
    } catch (std::exception e) {
      std::cout << e.what() << std::endl;
      throw e;
    }
    // assume all is right with the world if we exited w/o an error
    CHECK_EQUAL(0, 0);
  }  // end TEST_FIXTURE()

  TEST_FIXTURE(ChemistryPKTest, ChemistryPK_get_chem_output_names) {
    try {
      cpk_ = new ac::Chemistry_PK(chemistry_parameter_list_, chemistry_state_);
      amanzi::chemistry::chem_out->AddLevel("silent");
      cpk_->InitializeChemistry();
    } catch (std::exception e) {
      std::cout << e.what() << std::endl;
      throw e;
    }
    std::vector<std::string> names;
    cpk_->set_chemistry_output_names(&names);
    CHECK_EQUAL(names.at(0), "pH");
  }  // end TEST_FIXTURE()

  TEST_FIXTURE(ChemistryPKTest, ChemistryPK_set_component_names) {
    cpk_ = new ac::Chemistry_PK(chemistry_parameter_list_, chemistry_state_);
    amanzi::chemistry::chem_out->AddLevel("silent");
    cpk_->InitializeChemistry();
    std::vector<std::string> names;
    cpk_->set_component_names(&names);
    CHECK_EQUAL(names.at(0), "Al+++");
    CHECK_EQUAL(names.at(1), "H+");
    CHECK_EQUAL(names.at(2), "HPO4--");
    CHECK_EQUAL(names.at(3), "SiO2(aq)");
    CHECK_EQUAL(names.at(4), "UO2++");
  }  // end TEST_FIXTURE()
}  // end SUITE(GeochemistryTestChemistryPK)
