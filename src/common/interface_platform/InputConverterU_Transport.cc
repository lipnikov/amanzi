/*
  Input Converter

  Copyright 2010-201x held jointly by LANS/LANL, LBNL, and PNNL. 
  Amanzi is released under the three-clause BSD License. 
  The terms of use and "as is" disclaimer for this license are 
  provided in the top-level COPYRIGHT file.

  Authors: Markus Berndt
           Jeffrey Johnson
           Konstantin Lipnikov (lipnikov@lanl.gov)
*/

#include <algorithm>
#include <sstream>
#include <string>

//TPLs
#include <xercesc/dom/DOM.hpp>

// Amanzi's
#include "errors.hh"
#include "exceptions.hh"
#include "dbc.hh"

#include "InputConverterU.hh"
#include "InputConverterU_Defs.hh"

namespace Amanzi {
namespace AmanziInput {

XERCES_CPP_NAMESPACE_USE

/* ******************************************************************
* Create transport list.
****************************************************************** */
Teuchos::ParameterList InputConverterU::TranslateTransport_()
{
  Teuchos::ParameterList out_list;

  if (vo_->getVerbLevel() >= Teuchos::VERB_HIGH)
    *vo_->os() << "Translating transport" << std::endl;

  MemoryManager mm;

  char *text, *tagname;
  DOMNodeList *node_list, *children;
  DOMNode* node;
  DOMElement* element;

  // process CFL number
  bool flag;
  double cfl(1.0);

  node = GetUniqueElementByTagsString_("unstructured_controls, unstr_transport_controls, cfl", flag);
  if (flag) {
    text = mm.transcode(node->getTextContent());
    cfl = strtod(text, NULL);
  }

  // set defaults for transport
  out_list.set<int>("spatial discretization order", 1);
  out_list.set<int>("temporal discretization order", 1);
  out_list.set<double>("cfl", cfl);
  out_list.set<std::string>("flow mode", "transient");

  out_list.set<std::string>("solver", "PCG with Hypre AMG");
  out_list.set<std::string>("enable internal tests", "no");
  out_list.set<bool>("transport subcycling", TRANSPORT_SUBCYCLING);

  // overwrite data from expert parameters  
  node = GetUniqueElementByTagsString_("unstructured_controls, unstr_transport_controls, sub_cycling", flag);
  if (flag) {
    text = mm.transcode(node->getTextContent());
    out_list.set<bool>("transport subcycling", (strcmp(text, "on") == 0));
  }

  int poly_order(0);
  node = GetUniqueElementByTagsString_("unstructured_controls, unstr_transport_controls, algorithm", flag);
  if (flag) {
    std::string order = GetTextContentS_(node, "explicit first-order, explicit second-order");
    if (order == "explicit first-order") {
      out_list.set<int>("spatial discretization order", 1);
      out_list.set<int>("temporal discretization order", 1);
    } else if (order == "explicit second-order") {
      out_list.set<int>("spatial discretization order", 2);
      out_list.set<int>("temporal discretization order", 2);
      poly_order = 1;
    }
  }

  Teuchos::ParameterList& trp_lift = out_list.sublist("reconstruction");
  trp_lift.set<int>("polynomial order", poly_order);
  trp_lift.set<std::string>("limiter", "tensorial");
  trp_lift.set<bool>("limiter extension for transport", true);

  // check if we need to write a dispersivity sublist
  bool dispersion = doc_->getElementsByTagName(mm.transcode("dispersion_tensor"))->getLength() > 0;
  dispersion |= doc_->getElementsByTagName(mm.transcode("tortuosity"))->getLength() > 0;
  dispersion |= doc_->getElementsByTagName(mm.transcode("tortuosity_gas"))->getLength() > 0;

  // create dispersion list
  if (dispersion) {
    node_list = doc_->getElementsByTagName(mm.transcode("materials"));

    Teuchos::ParameterList& mat_list = out_list.sublist("material properties");

    children = node_list->item(0)->getChildNodes();
    for (int i = 0; i < children->getLength(); ++i) {
      DOMNode* inode = children->item(i);
      if (inode->getNodeType() != DOMNode::ELEMENT_NODE) continue;

      tagname = mm.transcode(inode->getNodeName());
      if (strcmp(tagname, "material") != 0) continue;

      std::string mat_name = GetAttributeValueS_(static_cast<DOMElement*>(inode), "name");

      // -- regions
      node = GetUniqueElementByTagsString_(inode, "assigned_regions", flag);
      std::vector<std::string> regions = CharToStrings_(mm.transcode(node->getTextContent()));

      Teuchos::ParameterList& tmp_list = mat_list.sublist(mat_name);
      tmp_list.set<Teuchos::Array<std::string> >("regions", regions);

      // -- dispersion tensor
      node = GetUniqueElementByTagsString_(inode, "mechanical_properties, dispersion_tensor", flag);
      if (flag) {
        double al, alh, alv, at, ath, atv;
        std::string model = GetAttributeValueS_(static_cast<DOMElement*>(node), "type");
        if (strcmp(model.c_str(), "uniform_isotropic") == 0) { 
          tmp_list.set<std::string>("model", "Bear");

          element = static_cast<DOMElement*>(node);
          al = GetAttributeValueD_(element, "alpha_l", TYPE_NUMERICAL, "m");
          at = GetAttributeValueD_(element, "alpha_t", TYPE_NUMERICAL, "m");

          tmp_list.sublist("parameters for Bear").set<double>("alpha_l", al)
                                                 .set<double>("alpha_t", at);
        } else if (strcmp(model.c_str(), "burnett_frind") == 0) {
          tmp_list.set<std::string>("model", "Burnett-Frind");

          element = static_cast<DOMElement*>(node);
          al = GetAttributeValueD_(element, "alpha_l", TYPE_NUMERICAL, "m");
          ath = GetAttributeValueD_(element, "alpha_th", TYPE_NUMERICAL, "m");
          atv = GetAttributeValueD_(element, "alpha_tv", TYPE_NUMERICAL, "m");

          tmp_list.sublist("parameters for Burnett-Frind")
              .set<double>("alpha_l", al).set<double>("alpha_th", ath)
              .set<double>("alpha_tv", atv);

          transport_permeability_ = true;
        } else if (strcmp(model.c_str(), "lichtner_kelkar_robinson") == 0) {
          tmp_list.set<std::string>("model", "Lichtner-Kelkar-Robinson");

          element = static_cast<DOMElement*>(node);
          alh = GetAttributeValueD_(element, "alpha_lh", TYPE_NUMERICAL, "m");
          alv = GetAttributeValueD_(element, "alpha_lv", TYPE_NUMERICAL, "m");
          ath = GetAttributeValueD_(element, "alpha_th", TYPE_NUMERICAL, "m");
          ath = GetAttributeValueD_(element, "alpha_tv", TYPE_NUMERICAL, "m");

          tmp_list.sublist("parameters for Lichtner-Kelkar-Robinson")
              .set<double>("alpha_lh", alh).set<double>("alpha_lv", alv)
              .set<double>("alpha_th", ath).set<double>("alpha_tv", atv);

          transport_permeability_ = true;
        } 
      }

      // -- tortousity
      node = GetUniqueElementByTagsString_(inode, "mechanical_properties, tortuosity", flag);
      if (flag) {
        double val = GetAttributeValueD_(static_cast<DOMElement*>(node), "value");
        tmp_list.set<double>("aqueous tortuosity", val);
      }

      node = GetUniqueElementByTagsString_(inode, "mechanical_properties, tortuosity_gas", flag);
      if (flag) {
        double val = GetAttributeValueD_(static_cast<DOMElement*>(node), "value");
        tmp_list.set<double>("gaseous tortuosity", val);
      }
    }
  }

  // -- molecular diffusion
  //    check in phases->water list (other solutes are ignored)
  node = GetUniqueElementByTagsString_("phases, liquid_phase, dissolved_components, solutes", flag);
  if (flag) {
    Teuchos::ParameterList& diff_list = out_list.sublist("molecular diffusion");
    std::vector<std::string> aqueous_names;
    std::vector<double> aqueous_values;

    children = node->getChildNodes();
    for (int i = 0; i < children->getLength(); ++i) {
      DOMNode* inode = children->item(i);
      element = static_cast<DOMElement*>(inode);  
      if (inode->getNodeType() != DOMNode::ELEMENT_NODE) continue;

      tagname = mm.transcode(inode->getNodeName());
      if (strcmp(tagname, "solute") != 0) continue;

      double val = GetAttributeValueD_(element, "coefficient_of_diffusion", TYPE_NUMERICAL, "", false);
      text = mm.transcode(inode->getTextContent());

      aqueous_names.push_back(TrimString_(text));
      aqueous_values.push_back(val);
    }

    diff_list.set<Teuchos::Array<std::string> >("aqueous names", aqueous_names);
    diff_list.set<Teuchos::Array<double> >("aqueous values", aqueous_values);
  }

  // -- molecular diffusion
  //    check in phases->air list (other solutes are ignored)
  node = GetUniqueElementByTagsString_("phases, gas_phase, dissolved_components, solutes", flag);
  if (flag) {
    Teuchos::ParameterList& diff_list = out_list.sublist("molecular diffusion");
    std::vector<std::string> gaseous_names;
    std::vector<double> gaseous_values, henry_coef;

    children = node->getChildNodes();
    for (int i = 0; i < children->getLength(); ++i) {
      DOMNode* inode = children->item(i);
      element = static_cast<DOMElement*>(inode);  
      if (inode->getNodeType() != DOMNode::ELEMENT_NODE) continue;

      tagname = mm.transcode(inode->getNodeName());
      if (strcmp(tagname, "solute") != 0) continue;

      double val = GetAttributeValueD_(element, "coefficient_of_diffusion", TYPE_NUMERICAL, "", false);
      double kh = GetAttributeValueD_(element, "kh");
      text = mm.transcode(inode->getTextContent());

      gaseous_names.push_back(TrimString_(text));
      gaseous_values.push_back(val);
      henry_coef.push_back(kh);
    }

    diff_list.set<Teuchos::Array<std::string> >("gaseous names", gaseous_names);
    diff_list.set<Teuchos::Array<double> >("gaseous values", gaseous_values);
    diff_list.set<Teuchos::Array<double> >("air-water partitioning coefficient", henry_coef);
  }

  // add dispersion/diffusion operator
  node = GetUniqueElementByTagsString_(
      "unstructured_controls, unstr_transport_controls, dispersion_discretization_method", flag);
  std::string disc_methods;
  if (flag)
    disc_methods = mm.transcode(node->getTextContent());
  else
    disc_methods = (mesh_rectangular_) ? "mfd-monotone_for_hex" : "mfd-optimized_for_monotonicity";
  disc_methods.append(", mfd-two_point_flux_approximation");

  out_list.sublist("operators") = TranslateDiffusionOperator_(
      disc_methods, "diffusion_operator", "", "", "", false);

  // multiscale model list
  out_list.sublist("multiscale models") = TranslateTransportMSM_();
  if (out_list.sublist("multiscale models").numParams() > 0) {
    out_list.sublist("physical models and assumptions")
        .set<std::string>("multiscale model", "dual porosity");
  }

  // create the sources and boundary conditions lists
  out_list.sublist("boundary conditions") = TranslateTransportBCs_();
  out_list.sublist("source terms") = TranslateTransportSources_();

  // remaining global parameters
  out_list.set<int>("number of aqueous components", phases_["water"].size());
  out_list.set<int>("number of gaseous components", phases_["air"].size());

  out_list.sublist("physical models and assumptions")
      .set<bool>("effective transport porosity", use_transport_porosity_);

  // cross coupling of PKs
  out_list.sublist("physical models and assumptions")
      .set<bool>("permeability field is required", transport_permeability_);

  out_list.sublist("verbose object") = verb_list_.sublist("verbose object");
  return out_list;
}


/* ******************************************************************
* Create list of multiscale models.
****************************************************************** */
Teuchos::ParameterList InputConverterU::TranslateTransportMSM_()
{
  Teuchos::ParameterList out_list, empty_list;

  Teuchos::OSTab tab = vo_->getOSTab();
  if (vo_->getVerbLevel() >= Teuchos::VERB_HIGH)
    *vo_->os() << "Translating multiscale models" << std::endl;

  MemoryManager mm;
  DOMNodeList *node_list, *children;
  DOMNode* node;
  DOMElement* element;

  bool flag;
  std::string model, rel_perm;

  node_list = doc_->getElementsByTagName(mm.transcode("materials"));
  element = static_cast<DOMElement*>(node_list->item(0));
  children = element->getElementsByTagName(mm.transcode("material"));

  for (int i = 0; i < children->getLength(); ++i) {
    DOMNode* inode = children->item(i); 

    node = GetUniqueElementByTagsString_(inode, "assigned_regions", flag);
    std::vector<std::string> regions = CharToStrings_(mm.transcode(node->getTextContent()));

    node = GetUniqueElementByTagsString_(inode, "multiscale_structure, solute_transfer_coefficient", flag);
    if (!flag) return empty_list;
    double omega = std::strtod(mm.transcode(node->getTextContent()), NULL);
    
    std::stringstream ss;
    ss << "MSM " << i;

    Teuchos::ParameterList& msm_list = out_list.sublist(ss.str());

    msm_list.set<std::string>("multiscale model", "dual porosity")
        .set<double>("solute transfer coefficient", omega)
        .set<Teuchos::Array<std::string> >("regions", regions);
  }

  return out_list;
}


/* ******************************************************************
* Create list of transport BCs.
****************************************************************** */
Teuchos::ParameterList InputConverterU::TranslateTransportBCs_()
{
  Teuchos::ParameterList out_list;

  Teuchos::OSTab tab = vo_->getOSTab();
  if (vo_->getVerbLevel() >= Teuchos::VERB_HIGH)
      *vo_->os() << "Translating boundary conditions" << std::endl;

  MemoryManager mm;

  char *text, *tagname;
  DOMNodeList *node_list, *children;
  DOMNode *node, *phase;
  DOMElement* element;

  node_list = doc_->getElementsByTagName(mm.transcode("boundary_conditions"));
  if (node_list->getLength() == 0) return out_list;

  children = node_list->item(0)->getChildNodes();

  for (int i = 0; i < children->getLength(); ++i) {
    DOMNode* inode = children->item(i);
    if (inode->getNodeType() != DOMNode::ELEMENT_NODE) continue;
    tagname = mm.transcode(inode->getNodeName());
    std::string bcname = GetAttributeValueS_(static_cast<DOMElement*>(inode), "name");

    // read the assigned regions
    bool flag;
    node = GetUniqueElementByTagsString_(inode, "assigned_regions", flag);
    text = mm.transcode(node->getTextContent());
    std::vector<std::string> regions = CharToStrings_(text);

    vv_bc_regions_.insert(vv_bc_regions_.end(), regions.begin(), regions.end());

    // process different phases
    // -- liquid phase
    phase = GetUniqueElementByTagsString_(inode, "liquid_phase", flag);
    if (flag) {
      element = static_cast<DOMElement*>(phase);
      DOMNodeList* solutes = element->getElementsByTagName(mm.transcode("solute_component"));
      TranslateTransportBCsGroup_(bcname, regions, solutes, out_list);
    }

    // -- gas phase
    phase = GetUniqueElementByTagsString_(inode, "gas_phase", flag);
    if (flag) {
      element = static_cast<DOMElement*>(phase);
      DOMNodeList* solutes = element->getElementsByTagName(mm.transcode("solute_component"));
      TranslateTransportBCsGroup_(bcname, regions, solutes, out_list);
    }

    // geochemical BCs 
    node = GetUniqueElementByTagsString_(inode, "liquid_phase, geochemistry_component", flag);
    if (flag) {
      std::string bctype;
      std::vector<DOMNode*> same_list = GetSameChildNodes_(node, bctype, flag, true);
      std::map<double, std::string> tp_forms, tp_values;

      for (int j = 0; j < same_list.size(); ++j) {
        element = static_cast<DOMElement*>(same_list[j]);
        double t0 = GetAttributeValueD_(element, "start", TYPE_TIME, "s");
        tp_values[t0] = GetAttributeValueS_(element, "name");
        tp_forms[t0] = GetAttributeValueS_(element, "function", TYPE_NONE, false, "constant");
        // no form -> use geochemistry engine
      }

      // create vectors of values and forms
      std::vector<double> times;
      std::vector<std::string> forms, values;
      for (std::map<double, std::string>::iterator it = tp_values.begin(); it != tp_values.end(); ++it) {
        times.push_back(it->first);
        values.push_back(it->second);
        forms.push_back(tp_forms[it->first]);
      }

      if (times.size() == 1) {
        times.push_back(times[0] + 1e+20);
        values.push_back(values[0]);
      } else {
        forms.pop_back();
      }

      // save in the XML files  
      Teuchos::ParameterList& tbc_list = out_list.sublist("geochemical");
      Teuchos::Array<std::string> solute_names;
      for (int i = 0; i < phases_["water"].size(); ++i) {
        solute_names.push_back(phases_["water"][i]);
      }
      Teuchos::ParameterList& bc = tbc_list.sublist(bcname);
      bc.set<Teuchos::Array<std::string> >("regions", regions);
      bc.set<Teuchos::Array<std::string> >("solutes", solute_names);
      bc.set<Teuchos::Array<double> >("times", times);
      bc.set<Teuchos::Array<std::string> >("geochemical conditions", values);
      bc.set<Teuchos::Array<std::string> >("time functions", forms);
    }
  }

  // backward compatibility: translate constraints for native chemistry
  TranslateTransportBCsAmanziGeochemistry_(out_list);

  return out_list;
}


/* ******************************************************************
* Create list of transport BCs for particular group of solutes.
* Solutes may have only one element, see schema for details.
****************************************************************** */
void InputConverterU::TranslateTransportBCsGroup_(
    std::string& bcname, std::vector<std::string>& regions,
    DOMNodeList* solutes, Teuchos::ParameterList& out_list)
{
  if (solutes->getLength() == 0) return;
 
  DOMNode* node = solutes->item(0);
  DOMElement* element;

  // get child nodes with the same tagname
  bool flag;
  std::string bctype, solute_name, tmp_name, unit;
  std::vector<DOMNode*> same_list = GetSameChildNodes_(node, bctype, flag, true);

  while (same_list.size() > 0) {
    // process a group of elements named after the 0-th element
    solute_name = GetAttributeValueS_(static_cast<DOMElement*>(same_list[0]), "name");

    std::map<double, double> tp_values;
    std::map<double, std::string> tp_forms;

    for (std::vector<DOMNode*>::iterator it = same_list.begin(); it != same_list.end(); ++it) {
      element = static_cast<DOMElement*>(*it);
      tmp_name = GetAttributeValueS_(element, "name");

      if (tmp_name == solute_name) {
        double t0 = GetAttributeValueD_(element, "start", TYPE_TIME, "s");
        tp_forms[t0] = GetAttributeValueS_(element, "function");
        GetAttributeValueD_(element, "value", TYPE_NUMERICAL, "molar");  // just a check
        tp_values[t0] = ConvertUnits_(GetAttributeValueS_(element, "value"), unit, solute_molar_mass_[solute_name]);

        same_list.erase(it);
        it--;
      } 
    }

    // Check for spatially dependent BCs. Only one is allowed (FIXME)
    // We extract both data and unit strings.
    bool space_bc(false);
    std::vector<double> data;

    element = static_cast<DOMElement*>(same_list[0]);
    std::string space_bc_name = GetAttributeValueS_(element, "space_function", TYPE_NONE, false);
    if (space_bc_name == "gaussian") {
      space_bc = true;
      std::vector<std::string> tmp;
      tmp = GetAttributeVectorS_(element, "space_data");
      for (int i = 0; i < tmp.size(); ++i) {
        data.push_back(ConvertUnits_(tmp[i], unit, solute_molar_mass_[solute_name]));
      }
    }

    // create vectors of values and forms
    std::vector<double> times, values;
    std::vector<std::string> forms;
    for (std::map<double, double>::iterator it = tp_values.begin(); it != tp_values.end(); ++it) {
      times.push_back(it->first);
      values.push_back(it->second);
      forms.push_back(tp_forms[it->first]);
    }
    forms.pop_back();
     
    // save in the XML files  
    Teuchos::ParameterList& tbc_list = out_list.sublist("concentration");
    Teuchos::ParameterList& bc = tbc_list.sublist(solute_name).sublist(bcname);
    bc.set<Teuchos::Array<std::string> >("regions", regions);

    Teuchos::ParameterList& bcfn = bc.sublist("boundary concentration");
    if (space_bc) {
      TranslateFunctionGaussian_(data, bcfn);
    } else if (times.size() == 1) {
      bcfn.sublist("function-constant").set<double>("value", values[0]);
    } else {
      bcfn.sublist("function-tabular")
          .set<Teuchos::Array<double> >("x values", times)
          .set<Teuchos::Array<double> >("y values", values)
          .set<Teuchos::Array<std::string> >("forms", forms);
    }
  }
}


/* ******************************************************************
* Create list of transport BCs for native chemistry.
* Solutes may have only one element, see schema for details.
****************************************************************** */
void InputConverterU::TranslateTransportBCsAmanziGeochemistry_(
    Teuchos::ParameterList& out_list)
{
  if (out_list.isSublist("geochemical") &&
      pk_model_["chemistry"] == "amanzi") {

    bool flag;
    std::string name, bc_name;
    DOMNode* node;
    DOMElement* element;

    node = GetUniqueElementByTagsString_("geochemistry, constraints", flag);

    Teuchos::ParameterList& bc_new = out_list.sublist("concentration");
    Teuchos::ParameterList& bc_old = out_list.sublist("geochemical");

    for (Teuchos::ParameterList::ConstIterator it = bc_old.begin(); it != bc_old.end(); ++it) {
      name = it->first;
 
      Teuchos::ParameterList& bco = bc_old.sublist(name);
      std::vector<std::string> solutes = bco.get<Teuchos::Array<std::string> >("solutes").toVector();

      for (int n = 0; n < solutes.size(); ++n) {
        Teuchos::ParameterList& bcn = bc_new.sublist(solutes[n]).sublist(name);
        Teuchos::ParameterList& fnc = bcn.sublist("boundary concentration").sublist("function-tabular");

        bcn.set("regions", bco.get<Teuchos::Array<std::string> >("regions"));
        fnc.set("x values", bco.get<Teuchos::Array<double> >("times"));
        fnc.set("forms", bco.get<Teuchos::Array<std::string> >("time functions"));
      
        // convert constraints to values
        Teuchos::Array<double> values;
        std::vector<std::string> constraints = 
            bco.get<Teuchos::Array<std::string> >("geochemical conditions").toVector();

        for (int i = 0; i < constraints.size(); ++i) {
          element = GetUniqueChildByAttribute_(node, "name", constraints[i], flag, true);
          element = GetUniqueChildByAttribute_(element, "name", solutes[n], flag, true);
          values.push_back(GetAttributeValueD_(element, "value"));
        }
        fnc.set("y values", values);
      }
    }

    out_list.remove("geochemical");
  }
}


/* ******************************************************************
* Create list of transport sources.
****************************************************************** */
Teuchos::ParameterList InputConverterU::TranslateTransportSources_()
{
  Teuchos::ParameterList out_list;

  Teuchos::OSTab tab = vo_->getOSTab();
  if (vo_->getVerbLevel() >= Teuchos::VERB_HIGH)
      *vo_->os() << "Translating source terms" << std::endl;

  MemoryManager mm;

  char *text, *tagname;
  DOMNodeList *node_list, *children;
  DOMNode *node;
  DOMElement* element;

  node_list = doc_->getElementsByTagName(mm.transcode("sources"));
  if (node_list->getLength() == 0) return out_list;

  children = node_list->item(0)->getChildNodes();

  for (int i = 0; i < children->getLength(); ++i) {
    DOMNode* inode = children->item(i);
    if (inode->getNodeType() != DOMNode::ELEMENT_NODE) continue;
    tagname = mm.transcode(inode->getNodeName());
    std::string srcname = GetAttributeValueS_(static_cast<DOMElement*>(inode), "name");

    // read the assigned regions
    bool flag;
    node = GetUniqueElementByTagsString_(inode, "assigned_regions", flag);
    text = mm.transcode(node->getTextContent());
    std::vector<std::string> regions = CharToStrings_(text);

    vv_src_regions_.insert(vv_src_regions_.end(), regions.begin(), regions.end());

    // process different phases
    // -- liquid phase
    DOMNode* phase_l = GetUniqueElementByTagsString_(inode, "liquid_phase", flag);
    if (flag) {
      element = static_cast<DOMElement*>(phase_l);
      DOMNodeList* solutes = element->getElementsByTagName(mm.transcode("solute_component"));
      TranslateTransportSourcesGroup_(srcname, regions, solutes, phase_l, out_list);
    }

    // -- gas phase
    DOMNode* phase_g = GetUniqueElementByTagsString_(inode, "gas_phase", flag);
    if (flag) {
      element = static_cast<DOMElement*>(phase_g);
      DOMNodeList* solutes = element->getElementsByTagName(mm.transcode("solute_component"));
      TranslateTransportSourcesGroup_(srcname, regions, solutes, phase_l, out_list);
    }
  }

  return out_list;
}


/* ******************************************************************
* Create list of transport sources.
****************************************************************** */
void InputConverterU::TranslateTransportSourcesGroup_(
    std::string& srcname, std::vector<std::string>& regions,
    DOMNodeList* solutes, DOMNode* phase_l, Teuchos::ParameterList& out_list)
{
  MemoryManager mm;
  DOMNodeList* node_list;
  DOMNode* node;
  DOMElement* element;

  for (int n = 0; n < solutes->getLength(); ++n) {
    node = solutes->item(n);

    // get a group of similar elements defined by the first element
    bool flag;
    std::string srctype, solute_name, weight, srctype_flow, unit("mol/s"), unit_in;

    std::vector<DOMNode*> same_list = GetSameChildNodes_(node, srctype, flag, true);
    solute_name = GetAttributeValueS_(static_cast<DOMElement*>(same_list[0]), "name");

    // weighting method
    bool classical(true), mass_fraction(false);
    char* text = mm.transcode(same_list[0]->getNodeName());
    if (strcmp(text, "volume_weighted") == 0) {
      weight = WeightVolumeSubmodel_(regions);
    } else if (strcmp(text, "perm_weighted") == 0) {
      weight = "permeability";
    } else if (strcmp(text, "uniform_conc") == 0) {
      weight = "none";
      unit = "mol/s/m^3";
    } else if (strcmp(text, "flow_weighted_conc") == 0) {
      element = static_cast<DOMElement*>(phase_l);
      node_list = element->getElementsByTagName(mm.transcode("liquid_component")); 
      GetSameChildNodes_(node_list->item(0), srctype_flow, flag, true);
      weight = (srctype_flow == "volume_weighted") ? "volume" : "permeability";
    } else if (strcmp(text, "flow_mass_fraction_conc") == 0) {
      weight = WeightVolumeSubmodel_(regions);
      mass_fraction = true;
      unit = "-";
    } else if (strcmp(text, "diffusion_dominated_release") == 0) {
      weight = "volume";
      classical = false;
    } else {
      ThrowErrorIllformed_(srcname, "element", text);
    } 
    if (weight == "permeability") transport_permeability_ = true;

    if (classical) {
      std::map<double, double> tp_values;
      std::map<double, std::string> tp_forms;

      for (int j = 0; j < same_list.size(); ++j) {
        element = static_cast<DOMElement*>(same_list[j]);
        double t0 = GetAttributeValueD_(element, "start", TYPE_TIME, "s");
        tp_forms[t0] = GetAttributeValueS_(element, "function");
        tp_values[t0] = ConvertUnits_(GetAttributeValueS_(element, "value"),
                                      unit_in, solute_molar_mass_[solute_name]);
        GetAttributeValueD_(element, "value", TYPE_NUMERICAL, unit);  // unit check

        // ugly correction when liquid/solute lists match
        if (mass_fraction) {
          element = static_cast<DOMElement*>(phase_l);
          node_list = element->getElementsByTagName(mm.transcode("liquid_component")); 
          std::vector<DOMNode*> tmp_list = GetSameChildNodes_(node_list->item(0), srctype_flow, flag, true);

          if (tmp_list.size() != same_list.size())
            ThrowErrorIllformed_(srcname, "liquid_component", text);

          element = static_cast<DOMElement*>(tmp_list[j]);
          double val = GetAttributeValueD_(element, "value");
          if (val > 0) {
            tp_values[t0] *= val / solute_molar_mass_[solute_name];
          } else {
            tp_values[t0] = val / rho_;
          }
        }
      }

      // create vectors of values and forms
      std::vector<double> times, values;
      std::vector<std::string> forms;
      for (std::map<double, double>::iterator it = tp_values.begin(); it != tp_values.end(); ++it) {
        times.push_back(it->first);
        values.push_back(it->second);
        forms.push_back(tp_forms[it->first]);
      }
      forms.pop_back();
     
      // save in the XML files  
      Teuchos::ParameterList& src_list = out_list.sublist("concentration");
      Teuchos::ParameterList& src = src_list.sublist(solute_name).sublist(srcname);
      src.set<Teuchos::Array<std::string> >("regions", regions);
      src.set<std::string>("spatial distribution method", weight);

      Teuchos::ParameterList& srcfn = src.sublist("well");
      if (times.size() == 1) {
        srcfn.sublist("function-constant").set<double>("value", values[0]);
      } else {
        srcfn.sublist("function-tabular")
            .set<Teuchos::Array<double> >("x values", times)
            .set<Teuchos::Array<double> >("y values", values)
            .set<Teuchos::Array<std::string> >("forms", forms);
      }
    } else {
      element = static_cast<DOMElement*>(same_list[0]);
      double total = GetAttributeValueD_(element, "total_inventory", "mol");
      double diff = GetAttributeValueD_(element, "effective_diffusion_coefficient");
      double length = GetAttributeValueD_(element, "mixing_length", "m");
      std::vector<double> times;
      times.push_back(GetAttributeValueD_(element, "start", TYPE_TIME, "s"));

      element = static_cast<DOMElement*>(same_list[1]);
      times.push_back(GetAttributeValueD_(element, "start", TYPE_TIME, "s"));

      // save data in the XML
      Teuchos::ParameterList& src_list = out_list.sublist("concentration");
      Teuchos::ParameterList& src = src_list.sublist(solute_name).sublist(srcname);
      src.set<Teuchos::Array<std::string> >("regions", regions);
      src.set<std::string>("spatial distribution method", weight);

      std::vector<double> values(2, 0.0);
      std::vector<std::string> forms(1, "SQRT");
      double amplitude = 2 * total / length * std::pow(diff / M_PI, 0.5); 

      Teuchos::ParameterList& srcfn = src.sublist("well").sublist("function-tabular");
      srcfn.set<Teuchos::Array<double> >("x values", times)
           .set<Teuchos::Array<double> >("y values", values)
           .set<Teuchos::Array<std::string> >("forms", forms);

      Teuchos::ParameterList& func = srcfn.sublist("SQRT").sublist("function-standard-math");
      func.set<std::string>("operator", "sqrt")
          .set<double>("parameter", 0.5)
          .set<double>("amplitude", amplitude)
          .set<double>("shift", times[0]);
    }
  }
}


/* ******************************************************************
* Create list of transport sources.
****************************************************************** */
std::string InputConverterU::WeightVolumeSubmodel_(const std::vector<std::string>& regions)
{
  std::string weight("volume");
  for (int k = 0; k < regions.size(); ++k) {
    if (region_type_[regions[k]] == 1) weight = "volume fraction";
  }
  return weight;
}

}  // namespace AmanziInput
}  // namespace Amanzi


