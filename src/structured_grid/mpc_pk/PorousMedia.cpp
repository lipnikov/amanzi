#include <winstd.H>

#include <algorithm>
#include <vector>
#include <cmath>

#include <ErrorList.H>
#include <Interpolater.H>
#include <MultiGrid.H>
#include <ArrayLim.H>
#include <Profiler.H>
#include <TagBox.H>
#include <DataServices.H>
#include <AmrData.H>
#include <time.h> 

#include <Godunov.H>
#include <PorousMedia.H>
#include <PROB_PM_F.H>
#include <POROUS_F.H>
#include <POROUSMEDIA_F.H>
#include <VISCOPERATOR_F.H>

#ifdef BL_USE_OMP
#include "omp.h"
#endif

#ifdef AMANZI
#include "exceptions.hh"
#include "errors.hh"
#include "simple_thermo_database.hh"
#include "verbosity.hh"
#include "chemistry_exception.hh"
#endif

#define GEOM_GROW   1
#define HYP_GROW    3
#define PRESS_GROW  1

#define DEF_LIMITS(fab,fabdat,fablo,fabhi)	\
  const int* fablo = (fab).loVect();		\
  const int* fabhi = (fab).hiVect();		\
  Real* fabdat = (fab).dataPtr();

#define DEF_CLIMITS(fab,fabdat,fablo,fabhi)	\
  const int* fablo = (fab).loVect();		\
  const int* fabhi = (fab).hiVect();		\
  const Real* fabdat = (fab).dataPtr();

//
// Static objects.
//
ErrorList PorousMedia::err_list;
BCRec     PorousMedia::phys_bc;
BCRec     PorousMedia::pres_bc;
MacProj*  PorousMedia::mac_projector = 0;
Godunov*  PorousMedia::godunov       = 0;

static Real BL_ONEATM = 101325.0;

namespace
{
  const std::string solid("Solid");
  const std::string absorbed("Absorbed");
  const std::string ctotal("Total");
}

void
PorousMedia::variableCleanUp ()
{
  desc_lst.clear();
  derive_lst.clear();
  err_list.clear();

  delete kappadata;
  kappadata = 0;
  delete phidata;
  phidata = 0;

  delete mac_projector;
  mac_projector = 0;

  delete godunov;
  godunov = 0;

  model_list.clear();
  bc_list.clear();
  obs_list.clear();
  phase_list.clear();
  comp_list.clear();
  tracer_list.clear();
  region_list.clear();

  for (int i=0;i<region_array.size();i++)
    delete region_array[i];

  rock_array.clear();
  observation_array.clear();
  source_array.clear();
  region_array.clear();

#ifdef AMANZI
  if (do_chem > -1)
    {
      chemSolve.clear();
      components.clear();
      parameters.clear();
    }
#endif
}

PorousMedia::PorousMedia ()
{
  Ssync        = 0;
  advflux_reg  = 0;
  viscflux_reg = 0;
  u_mac_prev   = 0;
  u_macG_prev  = 0;
  u_mac_curr   = 0;
  u_macG_curr  = 0;
  u_macG_trac  = 0;
  u_corr       = 0;
  kappa        = 0;
  kpedge       = 0;
  kr_coef      = 0;
  cpl_coef     = 0;
  lambda       = 0;
  lambda_cc    = 0;
  lambdap1_cc  = 0;
  dlambda_cc   = 0;
  rock_phi     = 0;
  aofs         = 0;
  diffusion    = 0;
  dt_eig       = 0;
  rhs_RhoD     = 0;
	
}

PorousMedia::PorousMedia (Amr&            papa,
                          int             lev,
                          const Geometry& level_geom,
                          const BoxArray& bl,
                          Real            time)
  :
  AmrLevel(papa,lev,level_geom,bl,time),
  //
  // Make room for ncomps+ntracers in aux_boundary_data_old.
  // With AMANZI we only use the ntracers parts.  But by using ncomps+ntracers
  // we don't need to worry about the case when ntracers==0.
  //
  aux_boundary_data_old(bl,HYP_GROW,ncomps+ntracers,level_geom),
  FillPatchedOldState_ok(true)
{
  //
  // Build metric coefficients for RZ calculations.
  //
  buildMetrics();

  //
  // Set up reflux registers.
  //
  advflux_reg  = 0;
  viscflux_reg = 0;
  if (level > 0 && do_reflux)
    {
      advflux_reg  = new FluxRegister(grids,crse_ratio,level,NUM_SCALARS);
      viscflux_reg = new FluxRegister(grids,crse_ratio,level,NUM_SCALARS);
    }

  //
  // Initialize work multifabs.
  //
  Ssync        = 0;
  u_mac_prev   = 0;
  u_macG_prev  = 0;
  u_mac_curr   = 0;
  u_macG_curr  = 0;
  u_macG_trac  = 0;
  rhs_RhoD     = 0;
  u_corr       = 0;
  kappa        = 0;
  kpedge       = 0;
  kr_coef      = 0;
  cpl_coef     = 0;
  lambda       = 0;
  lambda_cc    = 0;
  lambdap1_cc  = 0;
  dlambda_cc   = 0;
  rock_phi     = 0;
  aofs         = 0;

  //
  // Set up the godunov box.
  //
  SetGodunov();
  //
  // Set up diffusion.
  //
  diffusion = new Diffusion(parent,this,
			    (level > 0) ? getLevel(level-1).diffusion : 0,
			    ndiff,viscflux_reg,volume,area,
			    is_diffusive,visc_coef);
  
  // Allocate space for variable diffusion coefficients
  diffn_cc   = 0;
  diffnp1_cc = 0;
  if (variable_scal_diff) 
    {
      diffn_cc   = new MultiFab(grids, ndiff, 1);
      diffnp1_cc = new MultiFab(grids, ndiff, 1);
    }

  // Allocate space for the capillary pressure diffusive term
  pcn_cc   = 0;
  pcnp1_cc = 0;
  if (have_capillary)
    {
      pcn_cc     = new MultiFab(grids, 1, 2);
      pcnp1_cc   = new MultiFab(grids, 1, 2);
      (*pcn_cc).setVal(0.);
      (*pcnp1_cc).setVal(0.);
    }

  //
  // Set up the mac projector.
  //
  if (mac_projector == 0)
    {
      mac_projector = new MacProj(parent,parent->finestLevel(),
				  &phys_bc,do_any_diffuse);
    }
  mac_projector->install_level(level,this,volume,area);

  //
  // Alloc MultiFab to hold rock quantities
  //
  BL_ASSERT(kappa == 0);
  kappa = new MultiFab(grids,1,3);

  BL_ASSERT(rock_phi == 0);
  rock_phi = new MultiFab(grids,1,3);

  if (model != model_list["single-phase"] ||
      model != model_list["single-phase-solid"])
    {
      BL_ASSERT(kr_coef == 0);
      kr_coef = new MultiFab(grids,5,1);
      (*kr_coef).setVal(0.);

      BL_ASSERT(cpl_coef == 0);
      cpl_coef = new MultiFab(grids,5,1);
      (*cpl_coef).setVal(0.);

      BL_ASSERT(lambda_cc == 0);
      lambda_cc = new MultiFab(grids,ncomps,1);
      (*lambda_cc).setVal(1.);
    
      BL_ASSERT(lambdap1_cc == 0);
      lambdap1_cc = new MultiFab(grids,ncomps,1);
      (*lambdap1_cc).setVal(1.);

      BL_ASSERT(dlambda_cc == 0);
      dlambda_cc = new MultiFab(grids,3,1);
      (*dlambda_cc).setVal(0.);
    }

  BL_ASSERT(lambda == 0);
  lambda = new MultiFab[BL_SPACEDIM];
  for (int dir = 0; dir < BL_SPACEDIM; dir++)
    {
      BoxArray edge_grids(grids);
      edge_grids.surroundingNodes(dir);
      lambda[dir].define(edge_grids,1,0,Fab_allocate);
      lambda[dir].setVal(1.e40);
    }
  //
  // Alloc MultiFab to hold u_mac
  //
  BL_ASSERT(u_mac_prev  == 0);
  BL_ASSERT(u_mac_curr  == 0);
  BL_ASSERT(u_macG_trac == 0);
  BL_ASSERT(rhs_RhoD == 0);
  u_mac_prev  = new MultiFab[BL_SPACEDIM];
  u_mac_curr  = new MultiFab[BL_SPACEDIM];
  u_macG_trac = new MultiFab[BL_SPACEDIM];
  rhs_RhoD    = new MultiFab[BL_SPACEDIM];
  for (int dir = 0; dir < BL_SPACEDIM; dir++)
    {
      BoxArray edge_grids(grids);
      edge_grids.surroundingNodes(dir);
      u_mac_prev[dir].define(edge_grids,1,0,Fab_allocate);
      u_mac_prev[dir].setVal(1.e40);
      u_mac_curr[dir].define(edge_grids,1,0,Fab_allocate);
      u_mac_curr[dir].setVal(1.e40);
      rhs_RhoD[dir].define(edge_grids,1,0,Fab_allocate);
      rhs_RhoD[dir].setVal(1.e40);
      edge_grids.grow(1);
      u_macG_trac[dir].define(edge_grids,1,0,Fab_allocate);
      u_macG_trac[dir].setVal(1.e40);	
    }
  BL_ASSERT(kpedge == 0);
  kpedge     = new MultiFab[BL_SPACEDIM];
  for (int dir = 0; dir < BL_SPACEDIM; dir++)
    {
      BoxArray edge_gridskp(grids);
      edge_gridskp.surroundingNodes(dir).grow(1);
      kpedge[dir].define(edge_gridskp,1,0,Fab_allocate);
      kpedge[dir].setVal(1.e40);
    }

  // Must initialize to zero because we test on zero in estDt.
  dt_eig = 0;
}

PorousMedia::~PorousMedia ()
{
  delete Ssync;
  delete advflux_reg;
  delete viscflux_reg;
  delete [] u_mac_prev;
  delete [] u_mac_curr;
  delete [] u_macG_prev;
  delete [] u_macG_curr;
  delete [] u_macG_trac;
  delete [] u_corr;

  u_macG_prev = 0;
  u_macG_curr = 0;
  u_macG_trac = 0;
  u_corr      = 0;
      
  delete [] rhs_RhoD;
  delete [] kpedge;
  delete [] lambda;
  delete kappa;
  delete rock_phi;

  if (kr_coef)
    delete kr_coef;
  if (cpl_coef)
    delete cpl_coef;
  if (lambda_cc)
    delete lambda_cc;
  if (lambdap1_cc)
    delete lambdap1_cc;
  if (dlambda_cc)
    delete dlambda_cc;

 
  // Remove the arrays for variable viscosity and diffusivity
  // and delete the Diffusion object
  if (variable_scal_diff)
    {
      delete diffn_cc;
      delete diffnp1_cc;
    }
  if (have_capillary)
    {
      delete pcn_cc;
      delete pcnp1_cc;
    }
  delete diffusion;

}

void
PorousMedia::allocOldData ()
{
  for (int k = 0; k < num_state_type; k++)
    {
      state[k].allocOldData();
    }
}

void
PorousMedia::removeOldData ()
{
  AmrLevel::removeOldData();
}

void
PorousMedia::SetGodunov()
{
  if (godunov == 0)
    godunov = new Godunov();
}

void
PorousMedia::restart (Amr&          papa,
                      std::istream& is,
                      bool          bReadSpecial)
{
  AmrLevel::restart(papa,is,bReadSpecial);
  is >> dt_eig;

  if (verbose && ParallelDescriptor::IOProcessor())
    std::cout << "Estimated time step = " << dt_eig << '\n';
  //
  // Make room for ncomps+ntracers in aux_boundary_data_old.
  // With AMANZI we only use the ntracers parts.  But by using ncomps+ntracers
  // we don't need to worry about the case when ntracers==0.
  //
  aux_boundary_data_old.initialize(grids,HYP_GROW,ncomps+ntracers,Geom());

  FillPatchedOldState_ok = true;

  set_overdetermined_boundary_cells(state[State_Type].curTime());
  //
  // Set the godunov box.
  //
  SetGodunov();
    
  if (mac_projector == 0)
    {
      mac_projector = new MacProj(parent,parent->finestLevel(),
				  &phys_bc,do_any_diffuse);
    }
  mac_projector->install_level(level,this,volume,area );

  //
  // Build metric coefficients for RZ calculations.
  //
  buildMetrics();

  BL_ASSERT(advflux_reg == 0);
  if (level > 0 && do_reflux)
    {
      advflux_reg = new FluxRegister(grids,crse_ratio,level,NUM_SCALARS);
    }
  BL_ASSERT(viscflux_reg == 0);
  if (level > 0 && do_reflux)
    {
      viscflux_reg = new FluxRegister(grids,crse_ratio,level,NUM_SCALARS);
    }

  BL_ASSERT(Ssync == 0);
  if (level < parent->finestLevel())
    Ssync = new MultiFab(grids,NUM_SCALARS,1);

  diffusion = new Diffusion(parent, this,
			    (level > 0) ? getLevel(level-1).diffusion : 0,
			    ndiff, viscflux_reg, volume, area,
			    is_diffusive, visc_coef);
  //
  // Allocate the storage for variable diffusivity
  //
  diffn_cc   = 0;
  diffnp1_cc = 0;    
  if (variable_scal_diff) 
    {
      diffn_cc   = new MultiFab(grids, ndiff, 1);
      diffnp1_cc = new MultiFab(grids, ndiff, 1);
    }
  //
  // Allocate the storage for capillary pressure
  //
  pcn_cc     = 0;
  pcnp1_cc   = 0;    
  if (have_capillary) 
    {
      pcn_cc     = new MultiFab(grids, 1, 2);
      pcnp1_cc   = new MultiFab(grids, 1, 2);
      (*pcn_cc).setVal(0.);
      (*pcnp1_cc).setVal(0.);
    }

  is_first_step_after_regrid = false;
  old_intersect_new          = grids;

  //
  // Alloc MultiFab to hold rock quantities
  //
  BL_ASSERT(kappa == 0);
  kappa = new MultiFab(grids,1,3); 

  BL_ASSERT(rock_phi == 0);
  rock_phi = new MultiFab(grids,1,3);

  if (model != model_list["single-phase"] ||
      model != model_list["single-phase-solid"])
    {
      BL_ASSERT(kr_coef == 0);
      kr_coef = new MultiFab(grids,5,1);
      (*kr_coef).setVal(0.);

      BL_ASSERT(cpl_coef == 0);
      cpl_coef = new MultiFab(grids,5,1);
      (*cpl_coef).setVal(0.);

      BL_ASSERT(lambda_cc == 0);
      lambda_cc = new MultiFab(grids,ncomps,1);
      (*lambda_cc).setVal(1.);
    
      BL_ASSERT(lambdap1_cc == 0);
      lambdap1_cc = new MultiFab(grids,ncomps,1);
      (*lambdap1_cc).setVal(1.);

      BL_ASSERT(dlambda_cc == 0);
      dlambda_cc = new MultiFab(grids,3,1);
      (*dlambda_cc).setVal(0.);
    }

  BL_ASSERT(lambda == 0);
  lambda = new MultiFab[BL_SPACEDIM];
  for (int dir = 0; dir < BL_SPACEDIM; dir++)
    {
      BoxArray edge_gridskp(grids);
      edge_gridskp.surroundingNodes(dir);
      lambda[dir].define(edge_gridskp,1,0,Fab_allocate);
      lambda[dir].setVal(1.e40);
    }

  BL_ASSERT(kpedge == 0);
  kpedge     = new MultiFab[BL_SPACEDIM];
  for (int dir = 0; dir < BL_SPACEDIM; dir++)
    {
      BoxArray edge_gridskp(grids);
      edge_gridskp.surroundingNodes(dir).grow(1);
      kpedge[dir].define(edge_gridskp,1,0,Fab_allocate);
      kpedge[dir].setVal(1.e40);
    }
  
  init_rock_properties();

  //
  // Alloc MultiFab to hold u_mac
  //
  u_mac_prev  = new MultiFab[BL_SPACEDIM];
  u_mac_curr  = new MultiFab[BL_SPACEDIM];
  u_macG_trac = new MultiFab[BL_SPACEDIM];
  rhs_RhoD    = new MultiFab[BL_SPACEDIM];
  for (int dir = 0; dir < BL_SPACEDIM; dir++)
    {
      BoxArray edge_grids(grids);
      edge_grids.surroundingNodes(dir);
      u_mac_prev[dir].define(edge_grids,1,0,Fab_allocate);
    }

  std::string Level = BoxLib::Concatenate("Level_", level, 1);
  std::string FullPath = papa.theRestartFile();
  if (!FullPath.empty() && FullPath[FullPath.length()-1] != '/')
    FullPath += '/';
  FullPath += Level;
        
  std::string uxfile = "/umac_x";
  std::string uyfile = "/umac_y";
  uxfile = FullPath + uxfile;
  uyfile = FullPath + uyfile;
  VisMF::Read(u_mac_curr[0],uxfile);
  VisMF::Read(u_mac_curr[1],uyfile);

#if (BL_SPACEDIM == 3)
  std::string uzfile = "/umac_z";
  uzfile = FullPath + uzfile;
  VisMF::Read(u_mac_curr[2],uzfile);
#endif

  std::string utxfile = "/umact_x";
  std::string utyfile = "/umact_y";
  utxfile = FullPath + utxfile;
  utyfile = FullPath + utyfile;
  VisMF::Read(u_macG_trac[0],utxfile);
  VisMF::Read(u_macG_trac[1],utyfile);

#if (BL_SPACEDIM == 3)
  std::string utzfile = "/umact_z";
  utzfile = FullPath + utzfile;
  VisMF::Read(u_macG_trac[2],utzfile);
#endif
  
#ifdef MG_USE_FBOXLIB
  if (model != model_list["richard"])
    {
      std::string rxfile = "/rhs_RhoD_x";
      std::string ryfile = "/rhs_RhoD_y";
      rxfile = FullPath + rxfile;
      ryfile = FullPath + ryfile;
      VisMF::Read(rhs_RhoD[0],rxfile);
      VisMF::Read(rhs_RhoD[1],ryfile);
      
#if (BL_SPACEDIM == 3)
      std::string rzfile = "/rhs_RhoD_z";
      rzfile = FullPath + rzfile;
      VisMF::Read(rhs_RhoD[2],rzfile);
#endif
    }
#endif

  is_grid_changed_after_regrid = true;
  if (grids == papa.getLevel(level).boxArray())
    is_grid_changed_after_regrid = false;

}

void
PorousMedia::buildMetrics ()
{
  //
  // Build volume and face area arrays.
  //
  geom.GetVolume(volume,grids,GEOM_GROW);
  for (int dir = 0; dir < BL_SPACEDIM; dir++)
    {
      geom.GetFaceArea(area[dir],grids,dir,GEOM_GROW);
    }
}

//
// Reset the time levels to time (time) and timestep dt.
// This is done at the start of the timestep in the pressure iteration section.
//

void
PorousMedia::resetState (Real time,
                         Real dt_old,
                         Real dt_new)
{
  for (int k = 0; k < num_state_type; k++)
    {
      state[k].reset();
      state[k].setTimeLevel(time,dt_old,dt_new);
    }
}

//
// Set the time levels to time (time) and timestep dt.
//
void
PorousMedia::setTimeLevel (Real time,
                           Real dt_old,
                           Real dt_new)
{
  for (int k = 0; k < num_state_type; k++)
    state[k].setTimeLevel(time,dt_old,dt_new);
}

//
// This function initializes the all relevant data.  
// It calls subroutines in PROB_$D.F
//
void
PorousMedia::initData ()
{
  BL_PROFILE(BL_PROFILE_THIS_NAME() + "::initData()");

  if (verbose > 1 && ParallelDescriptor::IOProcessor())
    std::cout << "Initializing data ...\n";

  // 
  // Initialize rock properties
  //
  init_rock_properties();
  //
  // Initialize the state and the pressure.
  //
  const Real* dx       = geom.CellSize();
  MultiFab&   S_new    = get_new_data(State_Type);
  MultiFab&   P_new    = get_new_data(Press_Type);
  MultiFab&   U_new    = get_new_data(  Vel_Type);
  MultiFab&   U_vcr    = get_new_data(  Vcr_Type);
    
  const Real  cur_time = state[State_Type].curTime();
  S_new.setVal(0.);

  //
  // Initialized only based on solutions at the current level
  //
  for (MFIter mfi(S_new); mfi.isValid(); ++mfi)
    {
      BL_ASSERT(grids[mfi.index()] == mfi.validbox());

      FArrayBox& sdat = S_new[mfi];
      DEF_LIMITS(sdat,s_ptr,s_lo,s_hi);

      for (Array<BCData>::iterator it = ic_array.begin(); it < ic_array.end(); it++)
	{
	  if ((*it).type == bc_list["file"]) 
	    {
	      std::cerr << "Initialization of initial condition based on "
			<< "a file has not been implemented yet.\n";
	      BoxLib::Abort("PorousMedia::initData()");
	    }
	  else if ((*it).type == bc_list["scalar"]) 
	    {
	      for (Array<int>::iterator jt = (*it).region.begin(); 
		   jt < (*it).region.end(); jt++)
		region_array[*jt]->setVal(S_new[mfi],(*it).param,
					  dx,0,0,ncomps);
	    }
	  else if ((*it).type == bc_list["hydrostatic"]) 
	    {
	      BL_ASSERT(model >= 2);
	      FArrayBox& cdat = (*cpl_coef)[mfi];
	      const int n_cpl_coef = cpl_coef->nComp();
	      DEF_CLIMITS(cdat,c_ptr,c_lo,c_hi);
	      FORT_HYDRO(s_ptr, ARLIM(s_lo),ARLIM(s_hi), 
			 density.dataPtr(),&ncomps, 
			 c_ptr, ARLIM(c_lo),ARLIM(c_hi), &n_cpl_coef,
			 dx, &(*it).param[0], &gravity);
	    }
	  else if ((*it).type == bc_list["rockhold"])
	    {
	      BL_ASSERT(model >= 2);
	      const Real* prob_hi   = geom.ProbHi();
	      FArrayBox& cdat = (*cpl_coef)[mfi];
	      const int n_cpl_coef = cpl_coef->nComp();
	      DEF_CLIMITS(cdat,c_ptr,c_lo,c_hi);
	      std::string file_1d = "bc-cribs2.out";
	      std::ifstream inFile(file_1d.c_str(),std::ios::in);
	      std::string buffer;
	      std::getline(inFile,buffer);
	      int nz, buf_int;
	      inFile >> nz;
	      Real depth[nz], pressure[nz];
	      inFile >> buf_int >> depth[0] >> pressure[0];
	      std::getline(inFile,buffer);
	      for (int j=1;j<nz;j++)
		{
		  inFile >> buf_int >> buf_int >> depth[j] >> pressure[j];
		  std::getline(inFile,buffer);
		}

	      inFile.close();
	      FORT_ROCKHOLD(s_ptr, ARLIM(s_lo),ARLIM(s_hi), 
			    density.dataPtr(),&ncomps,
			    depth,pressure,&nz,
			    c_ptr, ARLIM(c_lo),ARLIM(c_hi), &n_cpl_coef,
			    dx,  &gravity, prob_hi);
	    }
	  else if ((*it).type == bc_list["zero_total_velocity"])
	    {	     
	      BL_ASSERT(model != model_list["single-phase"] && 
			model != model_list["single-phase-solid"]);
	      int nc = 1;
	      FArrayBox& kdat = (*kr_coef)[mfi];
	      FArrayBox& pdat = (*kappa)[mfi];
	      const int n_kr_coef = kr_coef->nComp();
	      DEF_CLIMITS(kdat,k_ptr,k_lo,k_hi);
	      DEF_CLIMITS(pdat,p_ptr,p_lo,p_hi);
	      FORT_STEADYSTATE(s_ptr, ARLIM(s_lo),ARLIM(s_hi), 
			       density.dataPtr(),muval.dataPtr(),&ncomps,
			       p_ptr, ARLIM(p_lo),ARLIM(p_hi), 
			       k_ptr, ARLIM(k_lo),ARLIM(k_hi), &n_kr_coef,
			       dx, &(*it).param[ncomps], &nc, &gravity);
	    }
#ifdef MG_USE_FBOXLIB
	  else if ((*it).type == bc_list["richard"]) 
	    {
	      BL_ASSERT(model != model_list["single-phase"] && 
			model != model_list["single-phase-solid"]);
	      BL_ASSERT(have_capillary == 1);
	      int nc = 1;
	      FArrayBox& kdat = (*kr_coef)[mfi];
	      FArrayBox& pdat = (*kappa)[mfi];
	      const int n_kr_coef = kr_coef->nComp();
	      DEF_CLIMITS(kdat,k_ptr,k_lo,k_hi);
	      DEF_CLIMITS(pdat,p_ptr,p_lo,p_hi);
	      FORT_STEADYSTATE(s_ptr, ARLIM(s_lo),ARLIM(s_hi), 
			       density.dataPtr(),muval.dataPtr(),&ncomps,
			       p_ptr, ARLIM(p_lo),ARLIM(p_hi), 
			       k_ptr, ARLIM(k_lo),ARLIM(k_hi), &n_kr_coef,
			       dx, &rinflow_vel_hi[1], &nc, &gravity);
	    }
#endif
	  else
	    {
	      FORT_INITDATA(&level,&cur_time,
			    s_ptr, ARLIM(s_lo),ARLIM(s_hi), 
			    density.dataPtr(), &ncomps, dx);
	    }
	}

      if (ntracers > 0)
	{		  
	  for (Array<BCData>::iterator it=tic_array.begin(); it<tic_array.end(); it++)
	    {
	      if ((*it).type == bc_list["file"]) 
		{
		  std::cerr << "Initialization of initial condition based on "
			    << "a file has not been implemented yet.\n";
		  BoxLib::Abort("PorousMedia::initData()");
		}
	      else if ((*it).type == bc_list["scalar"]) 
		{
		  for (Array<int>::iterator jt = (*it).region.begin(); 
		       jt < (*it).region.end(); jt++)
		    region_array[*jt]->setVal(S_new[mfi],(*it).param,
					      dx,0,ncomps,
					      ncomps+ntracers);
		}
	      else
		FORT_INIT_TRACER(&level,&cur_time,
				 s_ptr, ARLIM(s_lo),ARLIM(s_hi), 
				 (*it).param.dataPtr(), 
				 &ncomps, &ntracers, dx);
	    }
	}
    }

  FillStateBndry(cur_time,State_Type,0,ncomps+ntracers);
  P_new.setVal(0.);
  U_new.setVal(0.);
  U_vcr.setVal(0.);
  if (have_capillary) calcCapillary(cur_time);

  //
  // compute lambda
  // 
  calcLambda(cur_time);
  //
  // Initialize u_mac_curr 
  //
  if (model == model_list["richard"])
    {
      MultiFab::Copy(P_new,*pcnp1_cc,0,0,1,1);
      P_new.mult(-1.0,1);
      compute_vel_phase(u_mac_curr,0,cur_time);
    }
  else
    mac_project(u_mac_curr,rhs_RhoD,cur_time);

  umac_edge_to_cen(u_mac_curr, Vel_Type); 
  is_grid_changed_after_regrid = false;

  //
  // Richard initialization
  //
  bool do_brute_force = false;
  //do_brute_force = true;
#ifdef MG_USE_FBOXLIB
  if (ic_array[0].type == bc_list["richard"]) 
    {
      if (do_brute_force)
	richard_eqb_update(u_mac_curr);
      else
	{
	  int prev_nwt_iter = 1000;
	  int curr_nwt_iter;
	  Real dt = 1e6;
	  MultiFab tmp(grids,1,0);
	  Real err = 1.0;
	  int k = 0;
	  while (err > 1.e-8 && k < 80000)
	    {
	      k++;
	      MultiFab::Copy(tmp,S_new,0,0,1,0);
	      tmp.mult(-1.0);
	      richard_scalar_update(dt,curr_nwt_iter,u_mac_curr);
	      if (curr_nwt_iter <= prev_nwt_iter && curr_nwt_iter < 20 )
		dt = dt*1.2;
	      else if (curr_nwt_iter > prev_nwt_iter )
		dt = dt*0.9;
	      prev_nwt_iter = curr_nwt_iter;
	      MultiFab::Add(tmp,S_new,0,0,1,0);
	      err = tmp.norm2(0)/S_new.norm2(0);
	      if (ParallelDescriptor::IOProcessor())
		std::cout << k << " " << dt << " " << curr_nwt_iter << " " << err << std::endl;
	    }
	}
    }
#endif

#ifdef AMANZI
  if (do_chem > -1)
    {
      get_new_data(FuncCount_Type).setVal(1);

      Real dt_tmp = 1e3;
      strang_chem(S_new,dt_tmp);
    }
#endif

  is_first_step_after_regrid = true;
  old_intersect_new          = grids;
}

//
// Fills a new level n with best level n and coarser data available.
//

void
PorousMedia::init (AmrLevel& old)
{
  init_rock_properties();

  PorousMedia*  oldns     = (PorousMedia*) &old;
  const Real    dt_new    = parent->dtLevel(level);
  const Real    cur_time  = oldns->state[State_Type].curTime();
  const Real    prev_time = oldns->state[State_Type].prevTime();
  const Real    dt_old    = cur_time - prev_time;

  MultiFab&     S_new     = get_new_data(State_Type);
  MultiFab&     P_new     = get_new_data(Press_Type);
  MultiFab&     U_new     = get_new_data(  Vel_Type);
  MultiFab&     U_cor     = get_new_data(  Vcr_Type);

  U_cor.setVal(0.);

  dt_eig = oldns->dt_eig;
    
  setTimeLevel(cur_time,dt_old,dt_new);
    
  //
  // Get best state data: from old. 
  //
  for (FillPatchIterator fpi(old,S_new,0,cur_time,State_Type,0,NUM_SCALARS);
       fpi.isValid();
       ++fpi)
  {
    S_new[fpi.index()].copy(fpi());
  }
  //
  // Subsequent pressure solve will give the correct pressure.
  // 
  for (FillPatchIterator fpi(old,P_new,0,cur_time,Press_Type,0,1);
       fpi.isValid();
       ++fpi)
  {
    P_new[fpi.index()].copy(fpi());
  }

  //
  // Get best edge-centered velocity data: from old.
  //
  const BoxArray& old_grids = oldns->grids;
  is_grid_changed_after_regrid = true;
  if (old_grids == grids)
    {
      for (int dir=0; dir<BL_SPACEDIM; ++dir)
	{
	  u_mac_curr[dir].copy(oldns->u_mac_curr[dir]);
	  rhs_RhoD[dir].copy(oldns->rhs_RhoD[dir]);
	  u_macG_trac[dir].copy(oldns->u_macG_trac[dir]);
	}
      is_grid_changed_after_regrid = false;
    }


  /*  if (!is_grid_changed_after_regrid && model == model_list["richard"])
    {
      MultiFab P_tmp(grids,1,1);
      MultiFab::Copy(P_tmp,P_new,0,0,1,1);
      P_tmp.mult(-1.0);
      calcInvCapillary(S_new,P_tmp);
      }*/

  //
  // Get best cell-centered velocity data: from old.
  //
  for (FillPatchIterator fpi(old,U_new,0,cur_time,Vel_Type,0,BL_SPACEDIM);
       fpi.isValid();
       ++fpi)
    {
      U_new[fpi.index()].copy(fpi());
    }

#ifdef AMANZI
  if (do_chem > -1)
    {
      MultiFab& FC_new  = get_new_data(FuncCount_Type); 
      
      for (FillPatchIterator fpi(old,FC_new,FC_new.nGrow(),cur_time,FuncCount_Type,0,1);
	   fpi.isValid();
           ++fpi)
	{
	  FC_new[fpi.index()].copy(fpi());
	}
    }
#endif
    
  old_intersect_new          = BoxLib::intersect(grids,oldns->boxArray());
  is_first_step_after_regrid = true;
}

void
PorousMedia::init ()
{
  BL_ASSERT(level > 0);
    
  MultiFab& S_new = get_new_data(State_Type);
  MultiFab& P_new = get_new_data(Press_Type);
  MultiFab& U_new = get_new_data(  Vel_Type);
  MultiFab& U_cor = get_new_data(  Vcr_Type);
   
  const Array<Real>& dt_amr = parent->dtLevel();
  Array<Real>        dt_new(level+1);

  for (int lev = 0; lev < level; lev++)
    dt_new[lev] = dt_amr[lev];
  //
  // Guess new dt from new data (interpolated from coarser level).
  //
  const Real dt = dt_new[level-1]/Real(parent->MaxRefRatio(level-1));
  dt_new[level] = dt;
  parent->setDtLevel(dt_new);

  //
  // Compute dt based on old data.
  //
  PorousMedia& old       = getLevel(level-1);
  const Real   cur_time  = old.state[State_Type].curTime();
  const Real   prev_time = old.state[State_Type].prevTime();
  const Real   dt_old    = (cur_time-prev_time)/Real(parent->MaxRefRatio(level-1));

  setTimeLevel(cur_time,dt_old,dt);
  //
  // Get best coarse state, pressure and velocity data.
  //
  FillCoarsePatch(S_new,0,cur_time,State_Type,0,NUM_SCALARS);
  FillCoarsePatch(P_new,0,cur_time,Press_Type,0,1);
  FillCoarsePatch(U_new,0,cur_time,  Vel_Type,0,BL_SPACEDIM);
  U_cor.setVal(0.);

#ifdef AMANZI
  if (do_chem > -1)
    {
      FillCoarsePatch(get_new_data(FuncCount_Type),0,cur_time,FuncCount_Type,0,1);
    }
#endif

  init_rock_properties();
  old_intersect_new = grids;
}

//
// ADVANCE FUNCTIONS
//

//
// This function ensures that the multifab registers and boundary
// flux registers needed for syncing the composite grid
//
//     u_mac, umacG, Ssync, fr_adv, fr_visc
//
// are initialized to zero.  These quantities and the  
// advective velocity registers (mac_reg) are compiled by first
// setting them to the coarse value acquired during a coarse timestep
// and then incrementing in the fine values acquired during the
// subcycled fine timesteps.  This compilation procedure occurs in
// different parts for different quantities
//
// * u_mac is set in mac_project.
// * fr_adv, fr_visc are set in scalar_advect
// * Ssync is set in subcycled calls to post_timestep
// * mac_reg is set in mac_project
//
// After these quantities have been compiled during a coarse
// timestep and subcycled fine timesteps.  The post_timestep function
// uses them to sync the fine and coarse levels.  If the coarse level
// is not the base level, post_timestep modifies the next coarsest levels
// registers appropriately.
//
// Note :: There is a little ambiguity as to which level owns the
// boundary flux registers.  The Multifab registers are quantities
// sized by the coarse level BoxArray and belong to the coarse level.
// The fine levels own the boundary registers, since they are sized by
// the boundaries of the fine level BoxArray.
//

void
PorousMedia::advance_setup (Real time,
                            Real dt,
                            int  iteration,
                            int  ncycle)
{
  BL_PROFILE(BL_PROFILE_THIS_NAME() + "::advance_setup()");

  const int finest_level = parent->finestLevel();

  if (level < finest_level)
    {
      if (Ssync == 0)
	Ssync = new MultiFab(grids,NUM_SCALARS,1);
      Ssync->setVal(0);
    }

  //
  // Set reflux registers to zero.
  //
  if (do_reflux && level < finest_level)
    {
      getAdvFluxReg(level+1).setVal(0);
      getViscFluxReg(level+1).setVal(0);
    }

  //
  // Alloc space for edge velocities (normal comp only).
  //
  if (u_macG_prev == 0)
    {
      u_macG_prev = new MultiFab[BL_SPACEDIM];

      for (int dir = 0; dir < BL_SPACEDIM; dir++)
        {
	  BoxArray edge_grids(grids);
	  edge_grids.surroundingNodes(dir).grow(1);
	  u_macG_prev[dir].define(edge_grids,1,0,Fab_allocate);
	  u_macG_prev[dir].setVal(1.e40);
        }
    }
  if (u_macG_curr == 0)
    {
      u_macG_curr = new MultiFab[BL_SPACEDIM];

      for (int dir = 0; dir < BL_SPACEDIM; dir++)
        {
	  BoxArray edge_grids(grids);
	  edge_grids.surroundingNodes(dir).grow(1);
	  u_macG_curr[dir].define(edge_grids,1,0,Fab_allocate);
	  u_macG_curr[dir].setVal(1.e40);
        }
    }
  //
  // Set up state multifabs for the advance.
  //
  for (int k = 0; k < num_state_type; k++)
    {
      state[k].allocOldData();
      state[k].swapTimeLevels(dt);
    }
  //
  // Alloc MultiFab to hold advective update terms.
  //
  BL_ASSERT(aofs == 0);
  aofs = new MultiFab(grids,NUM_SCALARS,0);
  
  //
  // Compute lambda at cell centers
  //
  if (model != model_list["single-phase"] || 
      model != model_list["single-phase-solid"]) 
    {
      calcLambda(time); 
#ifdef MG_USE_FBOXLIB
      if (model != model_list["richard"])
#endif
	calcDLambda(time);
      MultiFab::Copy(*lambdap1_cc,*lambda_cc,0,0,ncomps,1);
    }
  //
  // Compute diffusion coefficients
  //
  if (variable_scal_diff)
    {
      calcDiffusivity(time,0,ncomps);
      MultiFab::Copy(*diffnp1_cc,*diffn_cc,0,0,ndiff,1);
    }
  //
  // Compute capillary diffusive coefficients
  //
  if (have_capillary)
    {
      calcCapillary(time);
      MultiFab::Copy(*pcnp1_cc,*pcn_cc,0,0,1,(*pcnp1_cc).nGrow());
    }  
  //
  // If we are not doing a full advection scheme, u_mac_curr 
  // must be recomputed if grid has changed after a timestep.
  //
#ifdef MG_USE_FBOXLIB
  if (model != model_list["richard"])
#endif
    {
      if (do_simple == 0 && (full_cycle == 1 || no_corrector == 1))
	{
	  if (n_pressure_interval == 0)
	    mac_project(u_mac_curr,rhs_RhoD,time);
	  else
	    {
	      if (level == 0)   it_pressure += 1;
	      
	      if (it_pressure == n_pressure_interval &&
		  parent->levelSteps(level)%parent->nCycle(level)==parent->nCycle(level)-1)
		{
		  mac_project(u_mac_curr,rhs_RhoD,time);
		  if (level == parent->finestLevel()) it_pressure = 0;
		}	    
	    }
	}
      else if (is_grid_changed_after_regrid)
	{
	  mac_project(u_mac_curr,rhs_RhoD,time);
	}
    }
  //
  // Alloc MultiFab to hold correction velocity.
  //
  if (u_corr == 0)
    {
      u_corr = new MultiFab[BL_SPACEDIM];
      for (int dir = 0; dir < BL_SPACEDIM; dir++)
	{
	  BoxArray edge_grids(grids);
	  edge_grids.surroundingNodes(dir).grow(1);
	  u_corr[dir].define(edge_grids,1,0,Fab_allocate);
	  u_corr[dir].setVal(0.);
	}
    }
    
  //
  // Swap the time levels of u_mac
  //
  MultiFab* dummy = u_mac_curr;
  u_mac_curr = u_mac_prev;
  u_mac_prev = dummy;

#ifdef AMANZI
  if (do_chem > -1)
    {
      aux_boundary_data_old.setVal(1.e30);
    }
#endif

  //
  // Copy cell-centered correction velocity computed in 
  // previous timestep to current timestep.
  //
  MultiFab& Uc_old = get_old_data(Vcr_Type);
  MultiFab& Uc_new = get_new_data(Vcr_Type);
  MultiFab::Copy(Uc_new,Uc_old,0,0,BL_SPACEDIM,Uc_new.nGrow());
}

//
// Clean up after the advance function.
//
void
PorousMedia::advance_cleanup (Real dt,
                              int  iteration,
                              int  ncycle)
{
  delete aofs;
  aofs = 0;
}

//
// Compute a timestep at a level. Return largest safe timestep.
//
Real
PorousMedia::advance (Real time,
                      Real dt,
                      int  iteration,
                      int  ncycle)
{
  BL_PROFILE(BL_PROFILE_THIS_NAME() + "::advance()");

  if (do_multilevel_full) 
  {
    if (level == 0)
      multilevel_advance(time,dt,iteration,ncycle);
    else
      if (verbose && ParallelDescriptor::IOProcessor())
	std::cout << " Doing multilevel solve : skipping level advance.\n";
  }
  else
  {
    if (verbose && ParallelDescriptor::IOProcessor())
      {
	std::cout << "Advancing grids at level " << level
		  << " : starting time = "       << time
		  << " with dt = "               << dt << '\n';
      }

    advance_setup(time,dt,iteration,ncycle);

    FillPatchedOldState_ok = true;
    //
    // Advance the old state for a Strang-split dt/2.  Include grow cells in
    // advance, and squirrel these away for diffusion and Godunov guys to
    // access for overwriting non-advanced fill-patched grow cell data.
    //
    MultiFab& S_new = get_new_data(State_Type);
    MultiFab& S_old = get_old_data(State_Type);
    
    MultiFab& P_new = get_new_data(Press_Type);
    MultiFab& P_old = get_old_data(Press_Type);
    
    MultiFab::Copy(S_new,S_old,0,0,NUM_SCALARS,S_old.nGrow()); 
    MultiFab::Copy(P_new,P_old,0,0,1,P_old.nGrow());
    
    const Real pcTime = state[State_Type].curTime();
    
    FillStateBndry (pcTime,State_Type,0,ncomps+ntracers);
    FillStateBndry (pcTime,Press_Type,0,1);
    //
    // If do_chem <= -1, then no reaction.
    // Otherwise, type of reactions depends on magnitude of do_chem.
    //
    if (do_chem > -1)
      {
	if (do_full_strang)
	  {
	    if (verbose && ParallelDescriptor::IOProcessor())
	      std::cout << "... advancing 1/2 strang step for chemistry\n";
	    //
	    // tmpFABs holds data from aux_boundary_data_old after reaction.
	    //
	    // We force it to have same distribution as aux_boundary_data_old.
	    //
	    MultiFab tmpFABs;
	    
	    BL_ASSERT(aux_boundary_data_old.nComp() == ncomps+ntracers);
	    
	    tmpFABs.define(aux_boundary_data_old.equivBoxArray(),
			   ncomps+ntracers,
			   0,
			   aux_boundary_data_old.DistributionMap(),
			   Fab_allocate);
	    
	    tmpFABs.setVal(1.e30);
	    
	    const int ngrow = aux_boundary_data_old.nGrow();

	    BoxArray ba(S_old.boxArray());

	    ba.grow(ngrow);
	    //
	    // This MF is guaranteed to cover tmpFABs & valid region of S_old.
	    //
	    MultiFab tmpS_old(ba,ntracers,0);
	    //
	    // Note that S_old & tmpS_old have the same distribution.
	    //
	    for (FillPatchIterator fpi(*this,S_old,ngrow,time,State_Type,ncomps,ntracers);
		 fpi.isValid();
		 ++fpi)
	      {
		tmpS_old[fpi.index()].copy(fpi());
	      }
	    
	    tmpFABs.copy(tmpS_old,0,ncomps,ntracers);
	    
	    tmpS_old.clear();
	    //
	    // strang_chem() expects ncomps+ntracers but only uses and/or modifies ntracers.
	    //
	    strang_chem(tmpFABs,dt/2,ngrow);
	    //
	    // Only copy the tracer stuff.
	    //
	    aux_boundary_data_old.copyFrom(tmpFABs,ncomps,ncomps,ntracers);
	    
	    tmpFABs.clear();
	    
	    strang_chem(S_old,dt/2);
	    
	    FillPatchedOldState_ok = false;
	  }
      }
    // 
    // do_simple: 2 ==> Only solve the tracer equations; assume steady state.
    //            1 ==> Only solve the pressure equation at time 0.
    //            0 ==> Solve the pressure equation at every timestep.
    //
#ifdef MG_USE_FBOXLIB
    if (model == model_list["richard"]) 
      {
	advance_richard(time,dt);
      }
    else
#endif
      {
	if (do_simple == 2 && !is_grid_changed_after_regrid)
	  advance_tracer(time,dt);
	else if (do_simple == 1  && !is_grid_changed_after_regrid)
	  advance_simple(time,dt);
	else
	  advance_incompressible(time,dt);
      }
    
    is_grid_changed_after_regrid = false;
    
    // second half of the strang splitting
    if (do_chem > -1)
      {      
	if (do_full_strang)
	  {
	    if (verbose && ParallelDescriptor::IOProcessor())
	      std::cout << "Second 1/2 Strang step of chemistry\n";
	    
	    strang_chem(S_new,dt/2.0);
	    
	    FillPatchedOldState_ok = true;
	  }
	else
	  {
	    if (n_chem_interval == 0)
	      {
		if (verbose && ParallelDescriptor::IOProcessor())
		  std::cout << "... advancing full strang step for chemistry\n";
		strang_chem(S_new,dt);
	      }
	    else
	      {
		if (level == 0)
		  {
		    it_chem += 1;
		    dt_chem += dt;
		  }
		
		if (it_chem == n_chem_interval &&
		    parent->levelSteps(level)%parent->nCycle(level)==parent->nCycle(level)-1 &&
		    level == parent->finestLevel())
		  {
		    if (verbose && ParallelDescriptor::IOProcessor())
		      std::cout << "... advancing full strang step for chemistry with dt ="
				<< dt_chem << "\n";
		    
		    strang_chem(S_new,dt_chem);
		    
		    it_chem = 0;
		    dt_chem = 0;
		    
		  }
	      }
	  }
      }
    
    // 
    // Check sum of components
    //
    if (verbose) check_sum();
    
    //predictDT(u_macG_curr);
    
    //
    // Clean up after the predicted value at t^n+1.
    // Estimate new timestep from umac cfl.
    //
    advance_cleanup(dt,iteration,ncycle);
  }

  // Dummy value : not used for determining time step.
  Real dt_test = 1.e20; 
  return dt_test; 

}

void
PorousMedia::multilevel_advance (Real time,
				 Real dt,
				 int  iteration,
				 int  ncycle)
{
  BL_PROFILE(BL_PROFILE_THIS_NAME() + "::multilevel_advance()");

  BL_ASSERT(do_multilevel_full);

  if (level == 0) 
  {
    for (int lev = 0; lev <= parent->finestLevel(); lev++)
    {
      PorousMedia&    pm_lev   = getLevel(lev);
      
      pm_lev.advance_setup(time,dt,iteration,ncycle);
      FillPatchedOldState_ok = true;

      MultiFab& S_new = pm_lev.get_new_data(State_Type);
      MultiFab& S_old = pm_lev.get_old_data(State_Type);

      MultiFab& P_new = pm_lev.get_new_data(Press_Type);
      MultiFab& P_old = pm_lev.get_old_data(Press_Type);

      S_new.setVal(0.);
      P_new.setVal(0.);
      MultiFab::Copy(S_new,S_old,0,0,ncomps+ntracers,S_old.nGrow()); 
      MultiFab::Copy(P_new,P_old,0,0,1,P_old.nGrow());

      Real pcTime = pm_lev.state[State_Type].curTime();
      pm_lev.FillStateBndry (pcTime,State_Type,0,ncomps+ntracers);
      pm_lev.FillStateBndry (pcTime,Press_Type,0,1);

      if (have_capillary)	pm_lev.calcCapillary(pcTime);

    }
    
    // If do_chem <= -1, then no reaction.
    // Otherwise, type of reactions depends on magnitude of have_corereact.
    if (do_chem > -1)
      {
	if (do_full_strang)
	  {
	    if (verbose && ParallelDescriptor::IOProcessor())
	      std::cout << "... advancing 1/2 strang step for chemistry\n";
	    
	    for (int lev = 0; lev <= parent->finestLevel(); lev++)
	      {
		PorousMedia& pm_lev = getLevel(lev);
		MultiFab&    S_old  = pm_lev.get_old_data(State_Type);
		
		//tmpFABs holds data from aux_boundary_data_old after reaction.
		MultiFab tmpFABs;
		
		BL_ASSERT(pm_lev.aux_boundary_data_old.nComp() == ncomps+ntracers);
		
		tmpFABs.define(pm_lev.aux_boundary_data_old.equivBoxArray(),
			       ncomps,0,
			       pm_lev.aux_boundary_data_old.DistributionMap(),
			       Fab_allocate);
		
		tmpFABs.setVal(1.e30);
		
		const int ngrow = pm_lev.aux_boundary_data_old.nGrow();
		
		BoxArray ba(S_old.boxArray());
		
		ba.grow(ngrow);
      
		// This MF is guaranteed to cover tmpFABs & valid region of S_old.
		MultiFab tmpS_old(ba,ntracers,0);
      
		// Note that S_old & tmpS_old have the same distribution.
		for (FillPatchIterator fpi(pm_lev,S_old,ngrow,time,State_Type,ncomps,ntracers);
		     fpi.isValid();++fpi) 
		  tmpS_old[fpi.index()].copy(fpi());
      
		tmpFABs.copy(tmpS_old,0,ncomps,ntracers);
		
		tmpS_old.clear();
		
		pm_lev.strang_chem(tmpFABs,dt/2,ngrow);

		pm_lev.aux_boundary_data_old.copyFrom(tmpFABs,ncomps,ncomps,ntracers);
   
		// Activate hook in FillPatch hack to get better data now.
		FillPatchedOldState_ok = false;
	      }

	    if (verbose && ParallelDescriptor::IOProcessor())
	      std::cout << "PorousMedia::advance(): end of first 1/2 Strang step\n";
	  }
      }
  }
#ifdef MG_USE_FBOXLIB
    if (model == model_list["richard"]) 
      {
	advance_multilevel_richard(time,dt);
      }
#endif
  
  //
  // second half of the strang splitting
  //
  if (do_chem > -1)
    {
      if (do_full_strang)
	{
	  if (verbose && ParallelDescriptor::IOProcessor())
	    std::cout << "Second 1/2 Strang step of chemistry\n";
	  
	  for (int lev = 0; lev <= parent->finestLevel(); lev++)
	    {
	      PorousMedia&  pm_lev = getLevel(lev);
	      MultiFab&     S_new  = pm_lev.get_new_data(State_Type);	      
	      pm_lev.strang_chem(S_new,dt/2.0);
	    }
	}
      else
	{
	  if (n_chem_interval == 0)
	    {
	      if (verbose && ParallelDescriptor::IOProcessor())
		std::cout << "... advancing full strang step for chemistry\n";
	      
	      for (int lev = 0; lev <= parent->finestLevel(); lev++)
		{
		  PorousMedia&  pm_lev = getLevel(lev);
		  MultiFab&     S_new  = pm_lev.get_new_data(State_Type);
		  
		  pm_lev.strang_chem(S_new,dt);
		}
	    }
	  else
	    {
	      it_chem += 1;
	      dt_chem += dt;
	      
	      if (it_chem == n_chem_interval)
		{
		  if (verbose && ParallelDescriptor::IOProcessor())
		    std::cout << "... advancing full strang step for chemistry with dt ="
			      << dt_chem << "\n";
		  for (int lev = 0; lev <= parent->finestLevel(); lev++)
		    {
		      PorousMedia&  pm_lev = getLevel(lev);
		      MultiFab&     S_new  = pm_lev.get_new_data(State_Type);
		  
		      pm_lev.strang_chem(S_new,dt_chem);
		    }
		  
		  it_chem = 0;
		  dt_chem = 0;
		  
		}
	    }	    
	}

      FillPatchedOldState_ok = true;
    }

  for (int lev = parent->finestLevel(); lev >= 0; lev--)
  {
    PorousMedia&  pm_lev     = getLevel(lev);

    pm_lev.avgDown();

    if (verbose) pm_lev.check_sum();      

    pm_lev.advance_cleanup(dt,iteration,ncycle);
  }

  if (verbose && ParallelDescriptor::IOProcessor())
    std::cout << "PorousMedia::advance(): end of multilevel advance\n";

}

void
PorousMedia::advance_incompressible (Real time,
				     Real dt)
{
  // 
  // Time stepping for incompressible flow.  
  // For single-phase constant-density problem, use advance_simple.
  //
  BL_PROFILE(BL_PROFILE_THIS_NAME() + "::advance_incompressible()");

  const Real cur_time = state[State_Type].curTime();
  MultiFab& S_new     = get_new_data(State_Type);
  int lscalar         = ncomps - 1; 

  if (n_pressure_interval !=0)
    check_minmax(u_mac_prev);

  MultiFab* rhod_tmp = 0;
  if (do_any_diffuse)
    {
      rhod_tmp = new MultiFab[BL_SPACEDIM];
      for (int dir =0; dir < BL_SPACEDIM; dir++) 
	{
	  BoxArray edge_grids(grids);
	  edge_grids.surroundingNodes(dir);
	  rhod_tmp[dir].define(edge_grids,1,0,Fab_allocate);
	  rhod_tmp[dir].setVal(0.0);
	  rhod_tmp[dir].plus(rhs_RhoD[dir],0,1,0);
	}
    }
     
  if (level == 0) 
    create_umac_grown(u_mac_prev,u_macG_prev);
  else 
    {
      PArray<MultiFab> u_macG_crse(BL_SPACEDIM,PArrayManage);
      GetCrseUmac(u_macG_crse,time);
      create_umac_grown(u_mac_prev,u_macG_crse,u_macG_prev); 
    }
  
  for (int dir = 0; dir < BL_SPACEDIM; dir++)
    MultiFab::Copy(u_macG_trac[dir],u_macG_prev[dir],0,0,1,0);
  
  //
  // Predictor: Advance the component conservation equations
  //
  int corrector = 0;
    
  if (no_corrector == 1)
    {
      corrector = 1;

      // copy u_mac_prev to u_mac_curr since we are not solving for u_mac_curr
      for (int dir = 0; dir < BL_SPACEDIM; dir++)
	{
	  MultiFab::Copy(u_mac_curr[dir],u_mac_prev[dir],0,0,1,0);
	  MultiFab::Copy(u_macG_curr[dir],u_macG_prev[dir],0,0,1,0);
	}

      // contribute to velocity register
      mac_projector->contribute_to_mac_reg(level,u_mac_prev);
      if (do_any_diffuse)
	{
	  for (int dir = 0; dir < BL_SPACEDIM; dir++) 
	    rhod_tmp[dir].mult(-1.0);
	  mac_projector->contribute_to_mac_reg_rhoD(level,rhod_tmp);
	}

      // Compute the advective term
      scalar_advection(u_macG_trac,dt,0,lscalar,true);

      // Add the advective and other terms to get scalars at t^{n+1}.
      scalar_update(dt,0,ncomps,corrector,u_macG_trac);

      if (ntracers > 0)
	{
	  int ltracer = ncomps+ntracers-1;
	  tracer_advection(u_macG_trac,dt,ncomps,ltracer,true);
	}

      predictDT(u_macG_prev);

      umac_edge_to_cen(u_mac_prev,Vel_Type);

    }

  else
    {
      // Compute the advective term
      scalar_advection(u_macG_trac,dt,0,lscalar,false);

      // Add the advective and other terms to get scalars at t^{n+1}.
      scalar_update(dt,0,ncomps,corrector);

      if (do_chem > -1)
	{
	  if (do_full_strang)
	    strang_chem(S_new,dt/2.0);
	}

      //
      // Corrector Step
      //    
      if (model > 1)
	calcLambda(cur_time);

      // Do a MAC projection to define edge velocities at time t^(n+1)
      mac_project(u_mac_curr,rhs_RhoD,cur_time);
    
      if (do_any_diffuse)
	{
	  for (int dir = 0; dir < BL_SPACEDIM; dir++) 
	    {
	      rhod_tmp[dir].plus(rhs_RhoD[dir],0,1,0);
	      rhod_tmp[dir].mult(-0.5);
	    }
	  mac_projector->contribute_to_mac_reg_rhoD(level,rhod_tmp);
	}

      if (level == 0) 
	create_umac_grown(u_mac_curr,u_macG_curr);
      else 
	{
	  PArray<MultiFab> u_macG_crse(BL_SPACEDIM,PArrayManage);
	  GetCrseUmac(u_macG_crse,time+dt);
	  create_umac_grown(u_mac_curr,u_macG_crse,u_macG_curr);
	}

      // Create velocity at time t^{n+1/2}.
      MultiFab* u_mac_nph  = new MultiFab[BL_SPACEDIM];
      MultiFab* u_macG_nph = new MultiFab[BL_SPACEDIM];
      for (int dir = 0; dir < BL_SPACEDIM; dir++)
	{
	  BoxArray edge_grids(grids);
	  edge_grids.surroundingNodes(dir);
	  u_mac_nph[dir].define(edge_grids,1,0,Fab_allocate);
	  MultiFab::Copy(u_mac_nph[dir],u_mac_prev[dir],0,0,1,0);
	  u_mac_nph[dir].plus(u_mac_curr[dir],0,1,0);
	  u_mac_nph[dir].mult(0.5);
	  if (do_any_diffuse)
	    u_mac_nph[dir].plus(rhod_tmp[dir],0,1,0);

	  edge_grids.grow(1);
	  u_macG_nph[dir].define(edge_grids,1,0,Fab_allocate);
	  MultiFab::Copy(u_macG_nph[dir],u_macG_prev[dir],0,0,1,0);
	  u_macG_nph[dir].plus(u_macG_curr[dir],0,1,0);
	  u_macG_nph[dir].mult(0.5);

	  MultiFab::Copy(u_macG_trac[dir],u_macG_nph[dir],0,0,1,0);
	}

      mac_projector->contribute_to_mac_reg(level,u_mac_nph);
    
      umac_edge_to_cen(u_mac_nph,Vel_Type);

      // Re-advect component equations 
      corrector = 1;
      if (variable_scal_diff)
	calcDiffusivity (cur_time,0,ncomps);

      scalar_advection(u_macG_trac,dt,0,lscalar,true);
    
      scalar_update(dt,0,ncomps,corrector,u_macG_trac);

      if (ntracers > 0)
	{
	  int ltracer = ncomps+ntracers-1;
	  tracer_advection(u_macG_trac,dt,ncomps,ltracer,true);
	}

      // predict the next time step.  
      predictDT(u_macG_curr);

      // hack to see water pahse velocity
      // umac_edge_to_cen(u_macG_trac,Vel_Type);

      delete [] u_mac_nph;
      delete [] u_macG_nph;
    }
  
  if (do_any_diffuse) delete [] rhod_tmp;

  //
  // Check the divergence conditions of v_1 (water)
  //
  MultiFab divutmp(grids,1,0);
  divutmp.setVal(0);
  mac_projector->check_div_cond(level,divutmp,u_macG_trac,rhs_RhoD);
  MultiFab::Copy(S_new,divutmp,0,ncomps+ntracers,1,0);
  if (have_capillary) MultiFab::Copy(S_new,*pcnp1_cc,0,ncomps+ntracers+1,1,1);
  
}

void
PorousMedia::advance_simple (Real time,
			     Real dt)
{
  // 
  // Time stepping for incompressible single-phase single-density flow.
  //
  if (level == 0) 
    create_umac_grown(u_mac_prev,u_macG_prev);
  else 
    {
      PArray<MultiFab> u_macG_crse(BL_SPACEDIM,PArrayManage);
      GetCrseUmac(u_macG_crse,time);
      create_umac_grown(u_mac_prev,u_macG_crse,u_macG_prev); 
    }

  //
  // Single advance the component conservation equations
  //
  int corrector = 0;

  // Compute the coefficients for diffusion operators.
  if (variable_scal_diff)
    {
      calcDiffusivity(time,0,ncomps);
      MultiFab::Copy(*diffnp1_cc,*diffn_cc,0,0,ndiff,diffn_cc->nGrow());
    }

  // Compute the advective term
  scalar_advection(u_macG_prev,dt,0,ncomps,true);
    
  // Add the advective and other terms to get scalars at t^{n+1}.
  scalar_update(dt,0,ncomps,corrector);

  for (int dir = 0; dir < BL_SPACEDIM; dir++)
    u_mac_curr[dir].copy(u_mac_prev[dir]);

  umac_edge_to_cen(u_mac_curr,Vel_Type);
 
}

#ifdef MG_USE_FBOXLIB
void
PorousMedia::advance_richard (Real time,
			      Real dt)
{
  // 
  // Time stepping for richard's equation
  //
  int curr_nwt_iter;
  richard_scalar_update(dt,curr_nwt_iter,u_mac_curr);  
  MultiFab& P_new = get_new_data(Press_Type);
  MultiFab::Copy(P_new,*pcnp1_cc,0,0,1,1);
  P_new.mult(-1.0,1);
  compute_vel_phase(u_mac_curr,0,time+dt);
    
  if (level == 0) 
    create_umac_grown(u_mac_curr,u_macG_trac);
  else 
    {
      PArray<MultiFab> u_macG_crse(BL_SPACEDIM,PArrayManage);
      GetCrseUmac(u_macG_crse,time);
      create_umac_grown(u_mac_curr,u_macG_crse,u_macG_trac); 
    }

  umac_edge_to_cen(u_mac_curr,Vel_Type); 
  if (ntracers > 0)
    {
      int ltracer = ncomps+ntracers-1;
      tracer_advection(u_macG_trac,dt,ncomps,ltracer,true);
    }
 
  // predict the next time step. 
  Real dt_nwt = dt; 
  predictDT(u_macG_trac);
  if (curr_nwt_iter <= richard_iter && curr_nwt_iter < 4 && dt_nwt < richard_max_dt)
    dt_nwt = dt_nwt*1.1;
  else if (curr_nwt_iter > 5)
    dt_nwt = dt_nwt*0.75;
  else if (curr_nwt_iter < 2) 
    dt_nwt = dt_nwt*1.1;
  richard_iter = curr_nwt_iter;
  dt_eig = std::min(dt_eig,dt_nwt); 
}

void
PorousMedia::advance_multilevel_richard (Real time,
					 Real dt)
{
  // 
  // Time stepping for richard's equation
  //
  int curr_nwt_iter;
  int nlevs = parent->finestLevel() - level + 1;
  richard_composite_update(dt,curr_nwt_iter);

  for (int lev=0; lev<nlevs; lev++)
    {
      PorousMedia&    fine_lev   = getLevel(lev);  
      fine_lev.compute_vel_phase(fine_lev.u_mac_curr,0,time+dt);

      if (lev == 0) 
	fine_lev.create_umac_grown(fine_lev.u_mac_curr,
				   fine_lev.u_macG_trac);
      else 
	{
	  PArray<MultiFab> u_macG_crse(BL_SPACEDIM,PArrayManage);
	  fine_lev.GetCrseUmac(u_macG_crse,time);
	  fine_lev.create_umac_grown(fine_lev.u_mac_curr,u_macG_crse,
				     fine_lev.u_macG_trac); 
	}

      fine_lev.umac_edge_to_cen(fine_lev.u_mac_curr,Vel_Type); 
      if (ntracers > 0)
	{
	  int ltracer = ncomps+ntracers-1;
	  fine_lev.tracer_advection(fine_lev.u_macG_trac,dt,ncomps,ltracer,true);
	}
 
      // predict the next time step. 
      Real dt_nwt = dt; 
      fine_lev.predictDT(fine_lev.u_macG_trac);
  
      if (curr_nwt_iter <= richard_iter && curr_nwt_iter < 4 && dt_nwt < richard_max_dt)
	dt_nwt = dt_nwt*1.1;
      else if (curr_nwt_iter > 5 )
	dt_nwt = dt_nwt*0.75;
      else if (curr_nwt_iter < 2 ) 
	dt_nwt = dt_nwt*1.1;
      richard_iter = curr_nwt_iter;
      dt_eig = std::min(dt_eig,dt_nwt); 
    }
}
#endif

void
PorousMedia::advance_tracer (Real time,
			     Real dt)
{
  // 
  // Time stepping for tracers, assuming steady-state condition. 
  //

  BL_ASSERT(ntracers > 0);
    
  int ltracer = ncomps+ntracers-1;
  tracer_advection(u_macG_trac,dt,ncomps,ltracer,true); 
}

void
PorousMedia::create_lambda (Real time) 
{
  // 
  // lambda_T is evaluated at edges.  
  // 

  if (model == model_list["single-phase"] || 
      model == model_list["single-phase-rock"]) 
    {
      for (int dir=0; dir<BL_SPACEDIM; dir++)
	{
	  for (MFIter mfi(lambda[dir]); mfi.isValid(); ++mfi)
	    {
	      const Box& ebox = lambda[dir][mfi].box();
	      lambda[dir][mfi].copy(kpedge[dir][mfi],ebox,0,ebox,0,1);
	    }
	}
    }
  else
    {
      MultiFab& S = get_new_data(State_Type);

      const TimeLevel whichTime = which_time(State_Type,time);
      BL_ASSERT(whichTime == AmrOldTime || whichTime == AmrNewTime);    
      MultiFab* lcc = (whichTime == AmrOldTime) ? lambda_cc : lambdap1_cc;

      const int*  domlo    = geom.Domain().loVect();
      const int*  domhi    = geom.Domain().hiVect();

      for (FillPatchIterator S_fpi(*this,S,1,time,State_Type,0,ncomps);
	   S_fpi.isValid();
           ++S_fpi)
	{

	  dirichletStateBC(S_fpi(),1,time);

	  const int i = S_fpi.index();
	  BL_ASSERT(grids[i] == S_fpi.validbox());

	  const int* lo     = S_fpi.validbox().loVect();
	  const int* hi     = S_fpi.validbox().hiVect();

	  const Real* ldat  = (*lcc)[i].dataPtr();
	  const int* l_lo   = (*lcc)[i].loVect();
	  const int* l_hi   = (*lcc)[i].hiVect();

	  const int* lx_lo  = lambda[0][i].loVect();
	  const int* lx_hi  = lambda[0][i].hiVect();
	  const Real* lxdat = lambda[0][i].dataPtr();

	  const int* ly_lo  = lambda[1][i].loVect();
	  const int* ly_hi  = lambda[1][i].hiVect();
	  const Real* lydat = lambda[1][i].dataPtr();

#if(BL_SPACEDIM==3)
	  const int* lz_lo  = lambda[2][i].loVect();
	  const int* lz_hi  = lambda[2][i].hiVect();
	  const Real* lzdat = lambda[2][i].dataPtr();
#endif

	  const int* kx_lo  = kpedge[0][i].loVect();
	  const int* kx_hi  = kpedge[0][i].hiVect();
	  const Real* kxdat = kpedge[0][i].dataPtr();

	  const int* ky_lo  = kpedge[1][i].loVect();
	  const int* ky_hi  = kpedge[1][i].hiVect();
	  const Real* kydat = kpedge[1][i].dataPtr();

#if(BL_SPACEDIM==3)
	  const int* kz_lo  = kpedge[2][i].loVect();
	  const int* kz_hi  = kpedge[2][i].hiVect();
	  const Real* kzdat = kpedge[2][i].dataPtr();
#endif

	  Array<int> bc;
	  bc = getBCArray(State_Type,i,0,1);

	  FORT_MK_MACCOEF (lxdat,ARLIM(lx_lo),ARLIM(lx_hi),
			   lydat,ARLIM(ly_lo),ARLIM(ly_hi),
#if (BL_SPACEDIM==3)
			   lzdat,ARLIM(lz_lo),ARLIM(lz_hi),
#endif
			   kxdat,ARLIM(kx_lo),ARLIM(kx_hi),
			   kydat,ARLIM(ky_lo),ARLIM(ky_hi),
#if (BL_SPACEDIM==3)
			   kzdat,ARLIM(kz_lo),ARLIM(kz_hi),
#endif
			   ldat,ARLIM(l_lo),ARLIM(l_hi),
			   lo,hi,domlo,domhi,bc.dataPtr());
	}
    }
}

void
PorousMedia::mac_project (MultiFab* u_mac, MultiFab* RhoD, Real time)
{
  BL_PROFILE(BL_PROFILE_THIS_NAME() + "::mac_project()");

  if (verbose && ParallelDescriptor::IOProcessor())
    std::cout << "... mac_projection at level " << level 
	      << " at time " << time << '\n';
  
  create_lambda(time);

  MultiFab RhoG(grids,1,1); 
  RhoG.setVal(0);
  for (int dir=0; dir < BL_SPACEDIM; dir ++)
    {
      RhoD[dir].setVal(0.0);
      u_mac[dir].setVal(0.0);
    }

  initialize_umac(u_mac,RhoG,RhoD,time);

  const TimeLevel whichTime = which_time(State_Type,time);
  BL_ASSERT(whichTime == AmrOldTime || whichTime == AmrNewTime);

  MultiFab* phi = 0;
  if (whichTime == AmrOldTime) 
    phi = &get_old_data(Press_Type);
  else if (whichTime == AmrNewTime) 
    phi = &get_new_data(Press_Type);

  // Always start with an initial guess of zero in the interior
  phi->setVal(0.);

  const BCRec& p_bc = desc_lst[Press_Type].getBC(0);

  // Set the boundary conditions *before* we define mac_bndry
  // so the values will end up in mac_bndry
  mac_projector->set_dirichlet_bcs(level, phi, RhoG, p_bc, 
				   press_lo, press_hi);
  phi->FillBoundary();

  PressBndry mac_bndry(grids,1,geom);
  const int src_comp   = 0;
  const int dest_comp  = 0;
  const int num_comp   = 1;
  if (level == 0)
    {
      mac_bndry.setBndryValues(*phi,src_comp,dest_comp,num_comp,p_bc);
    }
  else
    {
      MultiFab CPhi;
      GetCrsePressure(CPhi,time);
      BoxArray crse_boxes = BoxArray(grids).coarsen(crse_ratio);
      const int in_rad     = 0;
      const int out_rad    = 1;
      const int extent_rad = 2;
      BndryRegister crse_br(crse_boxes,in_rad,out_rad,extent_rad,num_comp);
      crse_br.copyFrom(CPhi,extent_rad,src_comp,dest_comp,num_comp);
      mac_bndry.setBndryValues(crse_br,src_comp,*phi,src_comp,
			       dest_comp,num_comp,crse_ratio,p_bc);
    }
  //
  // get source term
  //
  int do_rho_scale = 1;

  MultiFab* forces = 0;

  if (do_source_term)
    {
      forces = new MultiFab(grids,ncomps,0);
      forces->setVal(0.);
      for (MFIter mfi(*forces); mfi.isValid(); ++mfi)
	{
	  int i = mfi.index();
	  getForce((*forces)[mfi],i,0,0,ncomps,time,do_rho_scale);
	}
    }
  const Real strt_time = ParallelDescriptor::second();
  mac_projector->mac_project(level,u_mac,lambda,RhoD,forces,
			     phi,mac_bndry,p_bc);
  
  if (do_source_term)
    delete forces;
    

  if (model != model_list["single-phase"] || 
      model != model_list["single-phase-solid"]) 
    {
      MultiFab* u_phase = new MultiFab[BL_SPACEDIM];
      for (int dir = 0; dir < BL_SPACEDIM; dir++)
	{
	  BoxArray edge_grids(grids);
	  edge_grids.surroundingNodes(dir);
	  u_phase[dir].define(edge_grids,1,0,Fab_allocate);
	  u_phase[dir].setVal(1.e40);
	}

      compute_vel_phase(u_phase,u_mac,time);

      umac_cpy_edge_to_cen(u_phase,Vcr_Type,1);

      delete [] u_phase;

    }

  // compute time spend in mac_project()
  if (verbose > 1)
    {
      const int IOProc   = ParallelDescriptor::IOProcessorNumber();
      Real      run_time = ParallelDescriptor::second() - strt_time;
      ParallelDescriptor::ReduceRealMax(run_time,IOProc);
      if (ParallelDescriptor::IOProcessor())
        std::cout << "PorousMedia:mac_project(): lev: " << level
                  << ", time: " << run_time << '\n';
    }
}

void
PorousMedia::initialize_umac (MultiFab* u_mac, MultiFab& RhoG, 
			      MultiFab* RhoD, Real time) 
{

  //
  // u_mac is initilized such that its divergence is 
  //   \nabla \rho g
  // RhoG is initialized such that p + RhoG*\Delta x is 
  //   the hydrostatic pressure 
  // RhoD is initizlized such that its divergence is 
  //   the diffusive term due to variable density formulation.
  //
  const int* domain_lo = geom.Domain().loVect();
  const int* domain_hi = geom.Domain().hiVect();
  const Real* dx       = geom.CellSize();

  MultiFab& S = get_new_data(State_Type);
  Array<Real> const_diff_coef(ncomps);
  for (int i=0;i<ncomps;i++)
    const_diff_coef[i] = visc_coef[i];
  const TimeLevel whichTime = which_time(State_Type,time);
  BL_ASSERT(whichTime == AmrOldTime || whichTime == AmrNewTime);
  MultiFab* pc;
  if (have_capillary)
    pc = (whichTime == AmrOldTime) ? pcn_cc : pcnp1_cc;
  else
    {
      pc = new MultiFab(grids,1,1);
      (*pc).setVal(0.);
    }
  MultiFab* lbd = (whichTime == AmrOldTime) ? lambda_cc : lambdap1_cc;
  for (FillPatchIterator S_fpi(*this,S,1,time,State_Type,0,ncomps);
       S_fpi.isValid();
       ++S_fpi)
    {
      dirichletStateBC(S_fpi(),1,time);

      const int  i   = S_fpi.index();
      const int* lo  = grids[i].loVect();
      const int* hi  = grids[i].hiVect();
	
      const int* lx_lo  = lambda[0][i].loVect();
      const int* lx_hi  = lambda[0][i].hiVect();
      const Real* lxdat = lambda[0][i].dataPtr();

      const int* ly_lo  = lambda[1][i].loVect();
      const int* ly_hi  = lambda[1][i].hiVect();
      const Real* lydat = lambda[1][i].dataPtr();

      const int* kx_lo  = kpedge[0][i].loVect();
      const int* kx_hi  = kpedge[0][i].hiVect();
      const Real* kxdat = kpedge[0][i].dataPtr();

      const int* ky_lo  = kpedge[1][i].loVect();
      const int* ky_hi  = kpedge[1][i].hiVect();
      const Real* kydat = kpedge[1][i].dataPtr();

      FArrayBox& Sfab   = S_fpi();
      const Real* ndat  = Sfab.dataPtr(); 
      const int*  n_lo  = Sfab.loVect();
      const int*  n_hi  = Sfab.hiVect();
	
      Box bx_mac(u_mac[0][i].box());
      const int* umlo   = bx_mac.loVect();
      const int* umhi   = bx_mac.hiVect();
      const Real* umdat = u_mac[0][i].dataPtr();

      Box by_mac(u_mac[1][i].box());
      const int* vmlo   = by_mac.loVect();
      const int* vmhi   = by_mac.hiVect();
      const Real* vmdat = u_mac[1][i].dataPtr();

      const int* rglo   = RhoG[i].loVect();
      const int* rghi   = RhoG[i].hiVect();
      const Real* rgdat = RhoG[i].dataPtr();

      const int* pclo   = (*pc)[i].loVect();
      const int* pchi   = (*pc)[i].hiVect();
      const Real* pcdat = (*pc)[i].dataPtr();

      const int* lbdlo   = (*lbd)[i].loVect();
      const int* lbdhi   = (*lbd)[i].hiVect();
      const Real* lbddat = (*lbd)[i].dataPtr();

      Box rx_mac(RhoD[0][i].box());
      const int* rxlo   = rx_mac.loVect();
      const int* rxhi   = rx_mac.hiVect();
      const Real* rxdat = RhoD[0][i].dataPtr();

      Box ry_mac(RhoD[1][i].box());
      const int* rylo   = ry_mac.loVect();
      const int* ryhi   = ry_mac.hiVect();
      const Real* rydat = RhoD[1][i].dataPtr();

      const int* p_lo   = (*rock_phi)[i].loVect();
      const int* p_hi   = (*rock_phi)[i].hiVect();
      const Real* pdat  = (*rock_phi)[i].dataPtr();

      Array<int> s_bc;
      s_bc = getBCArray(State_Type,i,0,1);

      Array<int> press_bc;
      press_bc = getBCArray(Press_Type,i,0,1);

#if (BL_SPACEDIM == 2)	
      FORT_INIT_UMAC (umdat,ARLIM(umlo),ARLIM(umhi),
		      vmdat,ARLIM(vmlo),ARLIM(vmhi),
		      pcdat,ARLIM(pclo),ARLIM(pchi),
		      lbddat,ARLIM(lbdlo),ARLIM(lbdhi),
		      lxdat,ARLIM(lx_lo),ARLIM(lx_hi),
		      lydat,ARLIM(ly_lo),ARLIM(ly_hi),
		      kxdat,ARLIM(kx_lo),ARLIM(kx_hi),
		      kydat,ARLIM(ky_lo),ARLIM(ky_hi),
		      rgdat,ARLIM(rglo),ARLIM(rghi),
		      rxdat,ARLIM(rxlo),ARLIM(rxhi),
		      rydat,ARLIM(rylo),ARLIM(ryhi),
		      ndat ,ARLIM(n_lo),ARLIM(n_hi),
		      pdat ,ARLIM(p_lo),ARLIM(p_hi),
		      const_diff_coef.dataPtr(),
		      s_bc.dataPtr(),press_bc.dataPtr(),
		      domain_lo,domain_hi,dx,lo,hi,
		      &wt_lo, &wt_hi,
		      inflow_bc_lo.dataPtr(),inflow_bc_hi.dataPtr(), 
		      inflow_vel_lo.dataPtr(),inflow_vel_hi.dataPtr());

#elif (BL_SPACEDIM == 3)
      Box bz_mac(u_mac[2][i].box());
      const int* wmlo  = bz_mac.loVect();
      const int* wmhi  = bz_mac.hiVect();
      const Real* wmdat = u_mac[2][i].dataPtr();

      const int* lz_lo  = lambda[2][i].loVect();
      const int* lz_hi  = lambda[2][i].hiVect();
      const Real* lzdat = lambda[2][i].dataPtr();

      const int* kz_lo  = kpedge[2][i].loVect();
      const int* kz_hi  = kpedge[2][i].hiVect();
      const Real* kzdat = kpedge[2][i].dataPtr();

      Box rz_mac(RhoD[2][i].box());
      const int* rzlo  = rz_mac.loVect();
      const int* rzhi  = rz_mac.hiVect();
      const Real* rzdat = RhoD[2][i].dataPtr();

      FORT_INIT_UMAC (umdat,ARLIM(umlo),ARLIM(umhi),
		      vmdat,ARLIM(vmlo),ARLIM(vmhi),
		      wmdat,ARLIM(wmlo),ARLIM(wmhi),
		      pcdat,ARLIM(pclo),ARLIM(pchi),
		      lbddat,ARLIM(lbdlo),ARLIM(lbdhi),
		      lxdat,ARLIM(lx_lo),ARLIM(lx_hi),
		      lydat,ARLIM(ly_lo),ARLIM(ly_hi),
		      lzdat,ARLIM(lz_lo),ARLIM(lz_hi),
		      kxdat,ARLIM(kx_lo),ARLIM(kx_hi),
		      kydat,ARLIM(ky_lo),ARLIM(ky_hi),
		      kzdat,ARLIM(kz_lo),ARLIM(kz_hi),
		      rgdat,ARLIM(rglo),ARLIM(rghi),
		      rxdat,ARLIM(rxlo),ARLIM(rxhi),
		      rydat,ARLIM(rylo),ARLIM(ryhi),
		      rzdat,ARLIM(rzlo),ARLIM(rzhi),
		      ndat,ARLIM(n_lo),ARLIM(n_hi),
		      pdat ,ARLIM(p_lo),ARLIM(p_hi),
		      const_diff_coef.dataPtr(),
		      s_bc.dataPtr(),press_bc.dataPtr(),
		      domain_lo,domain_hi,dx,lo,hi,
		      &wt_lo, &wt_hi,
		      inflow_bc_lo.dataPtr(),inflow_bc_hi.dataPtr(), 
		      inflow_vel_lo.dataPtr(),inflow_vel_hi.dataPtr());
#endif
    }
    
  RhoG.FillBoundary();

  if (!have_capillary)
    delete pc;

}

void
PorousMedia::compute_vel_phase (MultiFab* u_phase, MultiFab* u_mac,
				Real time) 
{
  //
  // The phase velocity of component 1 is given by 
  //   v_1 = \lambda_1/\lambda_T ( v_T + \lambda_2 \nabla p_c)
  //

  const int* domain_lo = geom.Domain().loVect();
  const int* domain_hi = geom.Domain().hiVect();
  const Real* dx       = geom.CellSize();

  MultiFab& S = get_data(State_Type,time);

  const TimeLevel whichTime = which_time(State_Type,time);
  BL_ASSERT(whichTime == AmrOldTime || whichTime == AmrNewTime);

  MultiFab* pc;
  if (have_capillary)
    pc = (whichTime == AmrOldTime) ? pcn_cc : pcnp1_cc;
  else
    {
      pc = new MultiFab(grids,1,1);
      (*pc).setVal(0.);
    }

  MultiFab* lbd = (whichTime == AmrOldTime) ? lambda_cc : lambdap1_cc;
    
  for (FillPatchIterator S_fpi(*this,S,1,time,State_Type,0,ncomps);
       S_fpi.isValid();
       ++S_fpi)
    {
      
      dirichletStateBC(S_fpi(),1,time);

      const int  i   = S_fpi.index();
      const int* lo  = grids[i].loVect();
      const int* hi  = grids[i].hiVect();

      const int* kx_lo  = kpedge[0][i].loVect();
      const int* kx_hi  = kpedge[0][i].hiVect();
      const Real* kxdat = kpedge[0][i].dataPtr();

      const int* ky_lo  = kpedge[1][i].loVect();
      const int* ky_hi  = kpedge[1][i].hiVect();
      const Real* kydat = kpedge[1][i].dataPtr();
	
      Box bx_mac(u_mac[0][i].box());
      const int* umlo   = bx_mac.loVect();
      const int* umhi   = bx_mac.hiVect();
      const Real* umdat = u_mac[0][i].dataPtr();

      Box by_mac(u_mac[1][i].box());
      const int* vmlo   = by_mac.loVect();
      const int* vmhi   = by_mac.hiVect();
      const Real* vmdat = u_mac[1][i].dataPtr();

      Box ax_mac(u_phase[0][i].box());
      const int* uplo   = ax_mac.loVect();
      const int* uphi   = ax_mac.hiVect();
      const Real* updat = u_phase[0][i].dataPtr();

      Box ay_mac(u_phase[1][i].box());
      const int* vplo   = ay_mac.loVect();
      const int* vphi   = ay_mac.hiVect();
      const Real* vpdat = u_phase[1][i].dataPtr();

      const int* pclo   = (*pc)[i].loVect();
      const int* pchi   = (*pc)[i].hiVect();
      const Real* pcdat = (*pc)[i].dataPtr();

      const int* lbdlo   = (*lbd)[i].loVect();
      const int* lbdhi   = (*lbd)[i].hiVect();
      const Real* lbddat = (*lbd)[i].dataPtr();

      Array<int> s_bc;
      s_bc = getBCArray(State_Type,i,0,1);


#if (BL_SPACEDIM == 2)	
      FORT_UPHASE (updat,ARLIM(uplo),ARLIM(uphi),
		   vpdat,ARLIM(vplo),ARLIM(vphi),
		   umdat,ARLIM(umlo),ARLIM(umhi),
		   vmdat,ARLIM(vmlo),ARLIM(vmhi),
		   pcdat,ARLIM(pclo),ARLIM(pchi),
		   lbddat,ARLIM(lbdlo),ARLIM(lbdhi),
		   kxdat,ARLIM(kx_lo),ARLIM(kx_hi),
		   kydat,ARLIM(ky_lo),ARLIM(ky_hi),
		   s_bc.dataPtr(),
		   domain_lo,domain_hi,dx,lo,hi);

#elif (BL_SPACEDIM == 3)
      Box bz_mac(u_mac[2][i].box());
      const int* wmlo  = bz_mac.loVect();
      const int* wmhi  = bz_mac.hiVect();
      const Real* wmdat = u_mac[2][i].dataPtr();

      Box az_mac(u_phase[2][i].box());
      const int* wplo  = az_mac.loVect();
      const int* wphi  = az_mac.hiVect();
      const Real* wpdat = u_phase[2][i].dataPtr();

      const int* kz_lo  = kpedge[2][i].loVect();
      const int* kz_hi  = kpedge[2][i].hiVect();
      const Real* kzdat = kpedge[2][i].dataPtr();

      FORT_UPHASE (updat,ARLIM(uplo),ARLIM(uphi),
		   vpdat,ARLIM(vplo),ARLIM(vphi),
		   wpdat,ARLIM(wplo),ARLIM(wphi),
		   umdat,ARLIM(umlo),ARLIM(umhi),
		   vmdat,ARLIM(vmlo),ARLIM(vmhi),
		   wmdat,ARLIM(wmlo),ARLIM(wmhi),
		   pcdat,ARLIM(pclo),ARLIM(pchi),
		   lbddat,ARLIM(lbdlo),ARLIM(lbdhi),
		   kxdat,ARLIM(kx_lo),ARLIM(kx_hi),
		   kydat,ARLIM(ky_lo),ARLIM(ky_hi),
		   kzdat,ARLIM(kz_lo),ARLIM(kz_hi),
		   s_bc.dataPtr(),
		   domain_lo,domain_hi,dx,lo,hi);
#endif
    }

  if (!have_capillary)
    delete pc;
}

void
PorousMedia::compute_vel_phase (MultiFab* u_phase, 
				int nc,
				Real time) 
{
  //
  // The phase velocity of component n is given by 
  //   v_n = \lambda_n ( \nabla p_n - \rho \gvec)
  // We are going to assume the p stored in PRESS_TYPE 
  // correspond to p_n.  
  // This will need to be generalized in future.
  //

  const int* domain_lo = geom.Domain().loVect();
  const int* domain_hi = geom.Domain().hiVect();
  const Real* dx       = geom.CellSize();

  MultiFab& S = get_data(State_Type,time);
  MultiFab& P = get_data(Press_Type,time);

  const TimeLevel whichTime = which_time(State_Type,time);
  BL_ASSERT(whichTime == AmrOldTime || whichTime == AmrNewTime);

  MultiFab* lbd = (whichTime == AmrOldTime) ? lambda_cc : lambdap1_cc;
    
  for (FillPatchIterator S_fpi(*this,S,1,time,State_Type,0,ncomps);
       S_fpi.isValid();
       ++S_fpi)
    {
      dirichletStateBC(S_fpi(),1,time);

      const int  i   = S_fpi.index();
      const int* lo  = grids[i].loVect();
      const int* hi  = grids[i].hiVect();
      
      Box ax_mac(u_phase[0][i].box());
      const int* uplo   = ax_mac.loVect();
      const int* uphi   = ax_mac.hiVect();
      const Real* updat = u_phase[0][i].dataPtr();

      Box ay_mac(u_phase[1][i].box());
      const int* vplo   = ay_mac.loVect();
      const int* vphi   = ay_mac.hiVect();
      const Real* vpdat = u_phase[1][i].dataPtr();

      const int* plo   = P[i].loVect();
      const int* phi   = P[i].hiVect();
      const Real* pdat = P[i].dataPtr();

      const int* lbdlo   = (*lbd)[i].loVect();
      const int* lbdhi   = (*lbd)[i].hiVect();
      const Real* lbddat = (*lbd)[i].dataPtr();

      Array<int> bc = getBCArray(Press_Type,i,0,1);

#if (BL_SPACEDIM == 2)	
      FORT_UPHASE_P (updat,ARLIM(uplo),ARLIM(uphi),
		     vpdat,ARLIM(vplo),ARLIM(vphi),
		     lbddat,ARLIM(lbdlo),ARLIM(lbdhi),
		     pdat,ARLIM(plo),ARLIM(phi),
		     lo,hi,domain_lo,domain_hi,dx,bc.dataPtr(),
		     rinflow_bc_lo.dataPtr(),rinflow_bc_hi.dataPtr(),
		     rinflow_vel_lo.dataPtr(),rinflow_vel_hi.dataPtr());

#elif (BL_SPACEDIM == 3)
      Box az_mac(u_phase[2][i].box());
      const int* wplo  = az_mac.loVect();
      const int* wphi  = az_mac.hiVect();
      const Real* wpdat = u_phase[2][i].dataPtr();
      FORT_UPHASE_P (updat,ARLIM(uplo),ARLIM(uphi),
		     vpdat,ARLIM(vplo),ARLIM(vphi),
		     wpdat,ARLIM(wplo),ARLIM(wphi),
		     lbddat,ARLIM(lbdlo),ARLIM(lbdhi),
		     pdat,ARLIM(plo),ARLIM(phi),
		     lo,hi,domain_lo,domain_hi,dx,bc.dataPtr(),
		     rinflow_bc_lo.dataPtr(),rinflow_bc_hi.dataPtr(),
		     rinflow_vel_lo.dataPtr(),rinflow_vel_hi.dataPtr());
#endif
    }
  // multiply by kedge
  for (int dir = 0; dir < BL_SPACEDIM; dir++)
    {
      for (MFIter ecMfi(u_phase[dir]); ecMfi.isValid(); ++ecMfi)
	u_phase[dir][ecMfi].mult(kpedge[dir][ecMfi],0,0,1);
    }
}

// =========================================
// Functions related to advection equations.
// =========================================

//
// scalar_advection advects the scalars based on Godunov scheme.
//
void
PorousMedia::scalar_advection (MultiFab* u_macG,
                               Real dt,
                               int  fscalar,
                               int  lscalar,
                               bool reflux_on_this_call)
{
  BL_PROFILE(BL_PROFILE_THIS_NAME() + "::scalar_advection()");

  if (verbose && ParallelDescriptor::IOProcessor())
  {
    if (reflux_on_this_call) 
      std::cout << "... advect scalars with contribution to refluxing \n";
    else 
      std::cout << "... advect scalars\n";
  }
  
  //
  // Get simulation parameters.
  //
  const Real* dx        = geom.CellSize();
  const Real  prev_time = state[State_Type].prevTime();
  const Real  curr_time = state[State_Type].curTime();
  int nscal             = lscalar - fscalar + 1;

  //
  // Get the viscous terms.
  // For model 0-1 => diffusion term
  //             2 => capillary pressure term
  //
  MultiFab visc_terms(grids,nscal,1);
  visc_terms.setVal(0);
  int do_visc_terms = 1;
  if (be_cn_theta != 1.0 && (do_visc_terms || have_capillary) && !do_cpl_advect)
      getViscTerms(visc_terms,fscalar,nscal,prev_time);

  //
  // Divergence of velocity: set to zero for now.
  //
  MultiFab* divu_fp = new MultiFab(grids,1,1);
  (*divu_fp).setVal(0.);
  //
  // Set up the grid loop.
  //
  FArrayBox flux[BL_SPACEDIM], tforces, pctmp, phitmp, kappatmp;

  Array<int> state_bc;
  int do_new = 1; 
  setPhysBoundaryValues(State_Type,fscalar,nscal,do_new);
  
  // S_new is only used as a container to hold
  // time t^{n+1} inflow boundary conditions
  MultiFab& S_new = get_new_data(State_Type); 
  MultiFab fluxes[BL_SPACEDIM];
  
  if (reflux_on_this_call && do_reflux && level < parent->finestLevel())
    {
      for (int i = 0; i < BL_SPACEDIM; i++)
	{
	  BoxArray ba = grids;
	  ba.surroundingNodes(i);
	  fluxes[i].define(ba, nscal, 0, Fab_allocate);
	}
    }
  
  for (FillPatchIterator S_fpi(*this,get_old_data(State_Type),HYP_GROW,
			       prev_time,State_Type,fscalar,nscal);
       S_fpi.isValid();
       ++S_fpi)
    {
      dirichletStateBC(S_fpi(),HYP_GROW,prev_time);

      const int i = S_fpi.index();
      
      getForce(tforces,i,1,fscalar,nscal,curr_time);
      godunov->Setup(grids[i], flux[0], flux[1], 
#if (BL_SPACEDIM == 3)  
		     flux[2], 
#endif
		     nscal,model);	   
      
      Real eigmax_m[BL_SPACEDIM] = {D_DECL(-1.e20,-1.e20,-1.e20)};
      
      int state_ind = 0;
      int use_conserv_diff = (advectionType[state_ind] == Conservative);
      
      godunov->Sum_tf_divu_visc(S_fpi(),tforces,state_ind,nscal,
				visc_terms[i],state_ind,
				(*divu_fp)[i],use_conserv_diff);
      
      state_bc = getBCArray(State_Type,i,state_ind,1);
      
      //
      // Polymer model.
      //
      if (model == model_list["polymer"]) 
	{ 
	  godunov->AdvectStatePmr(grids[i], dx, dt, 
				  area[0][i], u_macG[0][i], flux[0], kpedge[0][i],
				  area[1][i], u_macG[1][i], flux[1], kpedge[1][i],
#if (BL_SPACEDIM == 3)                        
				  area[2][i], u_macG[2][i], flux[2], kpedge[2][i],
#endif
				  S_fpi(),S_new[i],tforces,
				  (*divu_fp)[i] , state_ind,
				  (*aofs)[i]    , state_ind,
				  (*rock_phi)[i], (*kappa)[i],
				  use_conserv_diff,
				  state_ind,state_bc.dataPtr(),volume[i],
				  nscal,gravity,eigmax_m);
	}

      //
      // Single phase  model.
      //
      else if (model == model_list["single-phase"] || 
	       model == model_list["single-phase-solid"])
	{
	  godunov->AdvectStateLin(grids[i], dx, dt, 
				  area[0][i], u_macG[0][i], flux[0],
				  area[1][i], u_macG[1][i], flux[1], 
#if (BL_SPACEDIM == 3)                        
				  area[2][i], u_macG[2][i], flux[2],
#endif
				  S_fpi(),S_new[i],tforces, state_ind,
				  (*aofs)[i]    , state_ind,
				  (*rock_phi)[i], state_ind,
				  state_bc.dataPtr(),volume[i],nscal);	
	}
      //
      // Two-phase two-component model.
      //
      else if (model == model_list["two-phase"])
	{
	  const int n_kr_coef = kr_coef->nComp();
	  if (do_cpl_advect) 
	    {
	      Box box = (*pcn_cc)[i].box();
	      pctmp.resize(box,1);
	      pctmp.copy((*pcn_cc)[i],box,0,box,0,1);
	      pctmp.plus((*pcnp1_cc)[i],box,0,0,1);
	      pctmp.mult(0.5);
	      godunov->AdvectStateCpl(grids[i], dx, dt, 
				      area[0][i], u_macG[0][i], flux[0], kpedge[0][i], lambda[0][i],
				      area[1][i], u_macG[1][i], flux[1], kpedge[1][i], lambda[1][i],
#if (BL_SPACEDIM == 3)                        
				      area[2][i], u_macG[2][i], flux[2], kpedge[2][i], lambda[2][i],
#endif
				      S_fpi(), S_new[i], tforces,
				      (*divu_fp)[i] , state_ind,
				      (*aofs)[i]    , state_ind,
				      (*rock_phi)[i], (*kappa)[i],  pctmp,
				      (*lambda_cc)[i],(*dlambda_cc)[i], 
				      (*kr_coef)[i],n_kr_coef,
				      use_conserv_diff,
				      state_ind,state_bc.dataPtr(),volume[i],nscal);
	    }
	  else
	    godunov->AdvectStateRmn(grids[i], dx, dt, 
				    area[0][i], u_macG[0][i], flux[0], kpedge[0][i],
				    area[1][i], u_macG[1][i], flux[1], kpedge[1][i],
#if (BL_SPACEDIM == 3)                        
				    area[2][i], u_macG[2][i], flux[2], kpedge[2][i],
#endif
				    S_fpi(),S_new[i],tforces,
				    (*divu_fp)[i] , state_ind,
				    (*aofs)[i]    , state_ind,
				    (*rock_phi)[i], (*kappa)[i], 
				    (*lambda_cc)[i],(*dlambda_cc)[i], 
				    (*kr_coef)[i],n_kr_coef,
				    use_conserv_diff,
				    state_ind,state_bc.dataPtr(),volume[i],nscal);
	
	}

      //
      // Set aofs of components in solid phase to zero.
      //
      if ((model == model_list["single-phase-solid"]) && (nphases > 1)) 
	{
	  for (int ii=0; ii<ncomps; ii++) 
	    {
	      if (solid.compare(pNames[pType[ii]]) == 0)
		(*aofs)[i].setVal(0.,ii);	      
	    }
	}
      
      if (reflux_on_this_call)
        {
	  if (do_reflux)
	    {
	      if (level < parent->finestLevel())
		{
		  for (int d = 0; d < BL_SPACEDIM; d++)
		    fluxes[d][i].copy(flux[d]);
		}
	      
	      if (level > 0)
		{
		  for (int d = 0; d < BL_SPACEDIM; d++)
		    advflux_reg->FineAdd(flux[d],d,i,0,state_ind,nscal,dt);
		}
	    }
        }
      
      //
      // Allocate the eigenvalues into scalar array.
      //
      
      if (model == model_list["two-phase"])
	{
	  S_new[i].setVal(0.0,ncomps+ntracers);
	  S_new[i].setVal(0.0,ncomps+ntracers+1);
	  godunov->Getdfdn(S_new[i],ncomps+ntracers,ncomps,0,4);
	  godunov->Getdfdn(S_new[i],ncomps+ntracers+1,ncomps,1,4);
	}
    } 
  for (int d = 0; d < BL_SPACEDIM; d++)
    lambda[d].FillBoundary();

  //MultiFab::Copy(S_new,*aofs,0,ncomps+ntracers,1,0);
  //MultiFab::Copy(S_new,visc_terms,0,ncomps+ntracers+1,1,0);

  delete divu_fp;
  
  if (do_reflux && level < parent->finestLevel() && reflux_on_this_call)
    {
      for (int d = 0; d < BL_SPACEDIM; d++)
	getAdvFluxReg(level+1).CrseInit(fluxes[d],d,0,0,nscal,-dt);
    }
}

void
PorousMedia::scalar_update (Real      dt,
                            int       first_scalar,
                            int       num_comp,
			    int       corrector,
			    MultiFab* u_mac)
{
  BL_PROFILE(BL_PROFILE_THIS_NAME() + "::scalar_update()");
  if (verbose > 1 && ParallelDescriptor::IOProcessor())
    std::cout << "... update scalars\n";

  int last_scalar = num_comp-1;

  scalar_advection_update(dt, first_scalar, last_scalar, corrector);
  if (do_any_diffuse) 
    scalar_diffusion_update(dt, first_scalar, last_scalar, corrector);
  if (have_capillary)
    {
      if (!do_cpl_advect)
	scalar_capillary_update(dt, corrector, u_mac);
      else
	{
	  if (do_cpl_advect == 2)
	    {
	      Real pcTime = state[State_Type].curTime();
	      calcCapillary(pcTime);
	    }
	  else
	    diff_capillary_update(dt, corrector, u_mac);
	}
    }

  if (idx_dominant > -1)
    scalar_adjust_constraint(first_scalar,last_scalar);
}

void
PorousMedia::scalar_advection_update (Real dt,
                                      int  first_scalar,
                                      int  last_scalar,
				      int  corrector)
{
  BL_PROFILE(BL_PROFILE_THIS_NAME() + "::scalar_advection_update()");

  MultiFab&  S_old    = get_old_data(State_Type);
  MultiFab&  S_new    = get_new_data(State_Type);
  MultiFab&  Aofs     = *aofs;
  MultiFab&  Rockphi  = *rock_phi;
  FArrayBox  tforces;
    
  // Compute inviscid estimate of scalars.
  // component first_scalar -> last_scalar: N
  Real pcTime = state[State_Type].curTime();
  int nscal = last_scalar - first_scalar + 1;
  for (MFIter mfi(S_new); mfi.isValid(); ++mfi)
    {
      const int i = mfi.index();
      getForce(tforces,i,0,first_scalar,nscal,pcTime);
      godunov->Add_aofs_tf(S_old[i],S_new[i],first_scalar,nscal,
			   Aofs[i],first_scalar,tforces,0,Rockphi[i],grids[i],dt);
    }


  FillStateBndry(pcTime,State_Type,first_scalar,nscal);
  S_new.FillBoundary();
  if (idx_dominant > -1 && last_scalar < ncomps)
    scalar_adjust_constraint(first_scalar,last_scalar);

  //
  // Write out the min and max of each component of the new state.
  //
  if (corrector || verbose > 1) check_minmax();
}

void
PorousMedia::tracer_advection_update (Real dt,
                                      int  first_scalar,
                                      int  last_scalar,
				      int  corrector)
{
  BL_PROFILE(BL_PROFILE_THIS_NAME() + "::tracer_advection_update()");

  MultiFab&  S_old    = get_old_data(State_Type);
  MultiFab&  S_new    = get_new_data(State_Type);
  MultiFab&  Aofs     = *aofs;
  MultiFab&  Rockphi  = *rock_phi;
  FArrayBox  tforces;
    
  int nscal = ncomps + ntracers;
    
  //
  // Advect only the Total
  //
  Array<int> idx_total;
  for (int ii = 0; ii < ntracers; ii++)
    if (ctotal.compare(qNames[tType[ii]]) == 0) 
      idx_total.push_back(ii+ncomps+1);

  Real pcTime = state[State_Type].curTime();
  for (MFIter mfi(S_new); mfi.isValid(); ++mfi)
    {
      const int i = mfi.index();
      getForce_Tracer(tforces,i,0,0,ntracers,pcTime);

      godunov->Add_aofs_tracer(S_old[i],S_new[i],0,nscal,
			       Aofs[i],0,tforces,0,Rockphi[i],grids[i],
			       idx_total,dt);
	
    }
  S_new.FillBoundary();

  //
  // Write out the min and max of each component of the new state.
  //
  if (corrector || verbose > 1) check_minmax(first_scalar,last_scalar);
}

void
PorousMedia::scalar_diffusion_update (Real dt,
				      int first_scalar,
				      int last_scalar,
				      int corrector)

{
  BL_PROFILE(BL_PROFILE_THIS_NAME() + "::scalar_diffusion_update()");

  if (verbose > 1 && ParallelDescriptor::IOProcessor())
    std::cout << "... diffuse scalars\n";

  BL_ASSERT(model == model_list["single-phase"] || model == model_list["single-phase-solid"]);

  const Real strt_time = ParallelDescriptor::second();

  // Build single component edge-centered array of MultiFabs for fluxes
  MultiFab** fluxSCn;
  MultiFab** fluxSCnp1;

  diffusion->allocFluxBoxesLevel(fluxSCn,  0,1);
  diffusion->allocFluxBoxesLevel(fluxSCnp1,0,1);


  MultiFab* rho;
  MultiFab& S_new = get_new_data(State_Type);
  rho = new MultiFab(grids,1,1);
  MultiFab::Copy(*rho,S_new,0,0,1,1);

  for (int kk = 1; kk<ncomps; kk++)
    {
      if (solid.compare(pNames[pType[kk]]) != 0) 
	MultiFab::Add(*rho,S_new,kk,0,1,1);
    }

  diffusion->set_rho(rho);

  for (int kk = first_scalar; kk <= last_scalar; kk++)
    {
      if (is_diffusive[kk])
        {
	  MultiFab*  delta_rhs   = 0;
	  MultiFab*  alpha       = 0;
	  MultiFab** cmp_diffn   = 0;
	  MultiFab** cmp_diffnp1 = 0;

	  alpha     = new MultiFab(grids, 1, 1);
	  MultiFab::Copy(*alpha,*rock_phi,0,0,1,alpha->nGrow());

	  if (variable_scal_diff)
            {
	      Real diffTime = state[State_Type].prevTime();
	      diffusion->allocFluxBoxesLevel(cmp_diffn, 0, 1);
	      getDiffusivity(cmp_diffn, diffTime, kk, 0, 1);

	      diffTime = state[State_Type].curTime();
	      diffusion->allocFluxBoxesLevel(cmp_diffnp1, 0, 1);
	      getDiffusivity(cmp_diffnp1, diffTime, kk, 0, 1);
            }
	    
	  diffusion->diffuse_scalar(dt,kk,be_cn_theta,
				    fluxSCn,fluxSCnp1,0,delta_rhs,
				    alpha,cmp_diffn,cmp_diffnp1);

	  if (variable_scal_diff)
            {
	      diffusion->removeFluxBoxesLevel(cmp_diffn);
	      diffusion->removeFluxBoxesLevel(cmp_diffnp1);
            }
	    
	  delete delta_rhs;
	  delete alpha;

	  //
	  // Increment the viscous flux registers
	  //
	  if (do_reflux && corrector)
            {
	      FArrayBox fluxtot;

	      for (int d = 0; d < BL_SPACEDIM; d++)
                {
		  MultiFab fluxes;

		  if (level < parent->finestLevel())
		    fluxes.define((*fluxSCn[d]).boxArray(), 1, 0, Fab_allocate);

		  for (MFIter fmfi(*fluxSCn[d]); fmfi.isValid(); ++fmfi)
                    {
		      const Box& ebox = (*fluxSCn[d])[fmfi].box();

		      fluxtot.resize(ebox,1);
		      fluxtot.copy((*fluxSCn[d])[fmfi],ebox,0,ebox,0,1);
		      fluxtot.plus((*fluxSCnp1[d])[fmfi],ebox,0,0,1);

		      if (level < parent->finestLevel())
			fluxes[fmfi].copy(fluxtot);

		      if (level > 0)
			getViscFluxReg().FineAdd(fluxtot,d,fmfi.index(),0,kk,1,dt);
                    }

		  if (level < parent->finestLevel())
		    getLevel(level+1).getViscFluxReg().CrseInit(fluxes,d,0,kk,1,-dt);
                }
            }
        }
    }

  delete rho;

  diffusion->removeFluxBoxesLevel(fluxSCn);
  diffusion->removeFluxBoxesLevel(fluxSCnp1);
    
  // Make sure values on bc is correct
  Real pcTime = state[State_Type].curTime();
  FillStateBndry(pcTime,State_Type,0,ncomps);

  if (verbose > 1)
  {
    const int IOProc   = ParallelDescriptor::IOProcessorNumber();
    Real      run_time = ParallelDescriptor::second() - strt_time;

    ParallelDescriptor::ReduceRealMax(run_time,IOProc);

    if (ParallelDescriptor::IOProcessor())
        std::cout << "PorousMedia::scalar_diffusion_update(): time: " << run_time << '\n';
  }
    
  // Write out the min and max of each component of the new state
  if (corrector && verbose) check_minmax();
    
}

void
PorousMedia::diffuse_adjust_dominant(MultiFab&              Phi_new,
				     int                    sComp,
				     Real                   dt,
				     MultiFab**             fluxn,
				     MultiFab**             fluxnp1)
{
  BL_PROFILE(BL_PROFILE_THIS_NAME() + "::diffuse_adjust_dominant()");

  FArrayBox update;
  FArrayBox tmpfab;
  int nscal = 1;
  for (MFIter mfi(Phi_new); mfi.isValid(); ++mfi)
    {
      int iGrid = mfi.index();

      const Box& box = mfi.validbox();
      const int* lo    = mfi.validbox().loVect();
      const int* hi    = mfi.validbox().hiVect();

      update.resize(box,1);
      tmpfab.resize(box,1);
      tmpfab.setVal(0.);

      const int* p_lo  = (*rock_phi)[mfi].loVect();
      const int* p_hi  = (*rock_phi)[mfi].hiVect();
      const Real* pdat = (*rock_phi)[mfi].dataPtr();


      FORT_RECOMP_UPDATE(lo, hi,
			 update.dataPtr(),
			 ARLIM(update.loVect()),ARLIM(update.hiVect()),
			 pdat, ARLIM(p_lo), ARLIM(p_hi),
			 (*fluxn[0])[iGrid].dataPtr(),
			 ARLIM((*fluxn[0])[iGrid].loVect()),
			 ARLIM((*fluxn[0])[iGrid].hiVect()),
			 (*fluxn[1])[iGrid].dataPtr(),
			 ARLIM((*fluxn[1])[iGrid].loVect()),
			 ARLIM((*fluxn[1])[iGrid].hiVect()),
#if BL_SPACEDIM == 3
			 (*fluxn[2])[iGrid].dataPtr(),
			 ARLIM((*fluxn[2])[iGrid].loVect()),
			 ARLIM((*fluxn[2])[iGrid].hiVect()),
#endif
			 volume[iGrid].dataPtr(),
			 ARLIM(volume[iGrid].loVect()),ARLIM(volume[iGrid].hiVect()),
			 &nscal);

      update.mult(dt,box,0,1);
      tmpfab.plus(update,box,0,0,1);

      if (fluxnp1 != 0) 
	{
	  FORT_RECOMP_UPDATE(lo,hi,
			     update.dataPtr(),
			     ARLIM(update.loVect()),ARLIM(update.hiVect()),
			     pdat, ARLIM(p_lo), ARLIM(p_hi),
			     (*fluxnp1[0])[iGrid].dataPtr(),
			     ARLIM((*fluxnp1[0])[iGrid].loVect()),
			     ARLIM((*fluxnp1[0])[iGrid].hiVect()),
			     (*fluxnp1[1])[iGrid].dataPtr(),
			     ARLIM((*fluxnp1[1])[iGrid].loVect()),
			     ARLIM((*fluxnp1[1])[iGrid].hiVect()),
#if BL_SPACEDIM == 3
			     (*fluxnp1[2])[iGrid].dataPtr(),
			     ARLIM((*fluxnp1[2])[iGrid].loVect()),
			     ARLIM((*fluxnp1[2])[iGrid].hiVect()),
#endif
			     volume[iGrid].dataPtr(),
			     ARLIM(volume[iGrid].loVect()),ARLIM(volume[iGrid].hiVect()),
			     &nscal);

	  update.mult(dt,box,0,1);
	  tmpfab.plus(update,box,0,0,1);
	}

      tmpfab.plus(Phi_new[iGrid],box,sComp,0,1);
      Phi_new[mfi].copy(tmpfab,box,0,box,sComp,1);
    }

}

//
// This routine advects the scalars
//
void
PorousMedia::tracer_advection (MultiFab* u_macG,
                               Real dt,
                               int  fscalar,
                               int  lscalar,
                               bool reflux_on_this_call)
{
  BL_PROFILE(BL_PROFILE_THIS_NAME() + "::tracer_advection()");

  if (verbose && ParallelDescriptor::IOProcessor())
  {
    std::cout << "... advect tracers\n";
  }

  //
  // Get simulation parameters.
  //
  const Real* dx        = geom.CellSize();
  const Real  prev_time = state[State_Type].prevTime();
  const Real  cur_time  = state[State_Type].curTime();
  int nscal             = ntracers;
    
  //
  // Get the viscous terms.
  //
  MultiFab visc_terms(grids,nscal,1);
  visc_terms.setVal(0);

  //
  // Set up the grid loop.
  //
  FArrayBox flux[BL_SPACEDIM], tforces;

  FArrayBox sat, satn;

  Array<int> state_bc;

  MultiFab* divu_fp = new MultiFab(grids,1,1);
  (*divu_fp).setVal(0.);

  int do_new = 1; 
  setPhysBoundaryValues(State_Type,fscalar,nscal,do_new);

  MultiFab fluxes[BL_SPACEDIM];
  if (reflux_on_this_call && do_reflux && level < parent->finestLevel())
    {
      for (int i = 0; i < BL_SPACEDIM; i++)
        {
	  BoxArray ba = grids;
	  ba.surroundingNodes(i);
	  fluxes[i].define(ba, nscal, 0, Fab_allocate);
        }
    }
 
  for (FillPatchIterator S_fpi(*this,get_old_data(State_Type),HYP_GROW,
			       prev_time,State_Type,fscalar,nscal),
	 Sn_fpi(*this,get_new_data(State_Type),HYP_GROW,
		cur_time,State_Type,fscalar,nscal),
	 St_fpi(*this,get_old_data(State_Type),HYP_GROW,  
		prev_time,State_Type,0,ncomps),
	 Stn_fpi(*this,get_new_data(State_Type),HYP_GROW,  
		 cur_time,State_Type,0,ncomps);
       S_fpi.isValid() && Sn_fpi.isValid() && St_fpi.isValid() && Stn_fpi.isValid(); 
       ++S_fpi,++Sn_fpi,++St_fpi,++Stn_fpi)
    {
      dirichletTracerBC(S_fpi(),HYP_GROW,prev_time);
      dirichletTracerBC(Sn_fpi(),HYP_GROW,cur_time);
      dirichletStateBC(St_fpi(),HYP_GROW,prev_time);
      dirichletStateBC(Stn_fpi(),HYP_GROW,cur_time);

      const int i = S_fpi.index();
      getForce_Tracer(tforces,i,1,fscalar,nscal,cur_time);

      godunov->Setup_tracer(grids[i], flux[0], flux[1],
#if (BL_SPACEDIM == 3)  
			    flux[2], 	    
#endif		 
			    nscal);

      int aofs_ind  = ncomps;
      int state_ind = 0;
      int use_conserv_diff = (advectionType[state_ind] == Conservative);

      godunov->Sum_tf_divu_visc(S_fpi(),tforces,state_ind,nscal,
				visc_terms[i],state_ind,
				(*divu_fp)[i],use_conserv_diff);
      
      state_bc = getBCArray(State_Type,i,state_ind,1);

      sat.resize(BoxLib::grow(grids[i],HYP_GROW),1);
      satn.resize(BoxLib::grow(grids[i],HYP_GROW),1);
      sat.copy(St_fpi(),0,0,1);
      satn.copy(Stn_fpi(),0,0,1);
      sat.mult(1.0/density[0]);
      satn.mult(1.0/density[0]);
      godunov->AdvectTracer(grids[i], dx, dt, 
			    area[0][i], u_macG[0][i], flux[0], 
			    area[1][i], u_macG[1][i], flux[1], 
#if (BL_SPACEDIM == 3)                        
			    area[2][i], u_macG[2][i], flux[2], 
#endif
			    S_fpi(), Sn_fpi(), sat, satn, tforces,
			    (*divu_fp)[i] , state_ind,
			    (*aofs)[i]    , aofs_ind,
			    (*rock_phi)[i], 
			    use_conserv_diff,
			    state_ind,state_bc.dataPtr(),volume[i],
			    nscal);

      if (reflux_on_this_call)
	{
	  if (do_reflux)
	    {
	      if (level < parent->finestLevel())
		{
		  for (int d = 0; d < BL_SPACEDIM; d++)
		    fluxes[d][i].copy(flux[d]);
		}

	      if (level > 0)
		{
		  for (int d = 0; d < BL_SPACEDIM; d++)
		    advflux_reg->FineAdd(flux[d],d,i,0,fscalar,nscal,dt);
		}
	    }
	}
    }

  delete divu_fp;

  if (do_reflux && level < parent->finestLevel() && reflux_on_this_call)
    {
      for (int d = 0; d < BL_SPACEDIM; d++)
	getAdvFluxReg(level+1).CrseInit(fluxes[d],d,0,fscalar,nscal,-dt);
    }

  int corrector = 1;
  tracer_advection_update (dt, fscalar, lscalar, corrector);
}

DistributionMapping
PorousMedia::getFuncCountDM (const BoxArray& bxba, int ngrow)
{
  //
  // Sometimes "mf" is the valid region of the State.
  // Sometimes it's the region covered by AuxBoundaryData.
  // When ngrow>0 were doing AuxBoundaryData with nGrow()==ngrow.
  // Taken from LMC/HeatTransfer.cpp
  //
  BL_PROFILE(BL_PROFILE_THIS_NAME() + "::getFuncCountDM()");

  DistributionMapping rr;
  rr.RoundRobinProcessorMap(bxba.size(),ParallelDescriptor::NProcs());

  MultiFab fctmpnew;
  fctmpnew.define(bxba, 1, 0, rr, Fab_allocate);
  fctmpnew.setVal(1);

  if (ngrow == 0)
    {
      //
      // Working on valid region of state.
      //
      fctmpnew.copy(get_new_data(FuncCount_Type));  // Parallel copy.
    }
  else
    {
      //
      // Can't directly use a parallel copy from FuncCount_Type to fctmpnew.
      //
      MultiFab& FC = get_new_data(FuncCount_Type);

      BoxArray ba = FC.boxArray();
      ba.grow(ngrow);
      MultiFab grownFC(ba, 1, 0);
      grownFC.setVal(1);
                
      for (MFIter mfi(FC); mfi.isValid(); ++mfi)
	grownFC[mfi].copy(FC[mfi]);

      fctmpnew.copy(grownFC);  // Parallel copy.
    }

  int count = 0;
  Array<long> vwrk(bxba.size());
  for (MFIter mfi(fctmpnew); mfi.isValid(); ++mfi)
    vwrk[count++] = static_cast<long>(fctmpnew[mfi].sum(0));

  fctmpnew.clear();

#if BL_USE_MPI
  const int IOProc = ParallelDescriptor::IOProcessorNumber();

  Array<int> nmtags(ParallelDescriptor::NProcs(),0);
  Array<int> offset(ParallelDescriptor::NProcs(),0);

  for (int i = 0; i < vwrk.size(); i++)
    nmtags[rr.ProcessorMap()[i]]++;

  BL_ASSERT(nmtags[ParallelDescriptor::MyProc()] == count);

  for (int i = 1; i < offset.size(); i++)
    offset[i] = offset[i-1] + nmtags[i-1];

  Array<long> vwrktmp = vwrk;

  MPI_Gatherv(vwrk.dataPtr(),
	      count,
	      ParallelDescriptor::Mpi_typemap<long>::type(),
	      vwrktmp.dataPtr(),
	      nmtags.dataPtr(),
	      offset.dataPtr(),
	      ParallelDescriptor::Mpi_typemap<long>::type(),
	      IOProc,
	      ParallelDescriptor::Communicator());

  if (ParallelDescriptor::IOProcessor())
    {
      //
      // We must now assemble vwrk in the proper order.
      //
      std::vector< std::vector<int> > table(ParallelDescriptor::NProcs());

      for (int i = 0; i < vwrk.size(); i++)
	table[rr.ProcessorMap()[i]].push_back(i);

      int idx = 0;
      for (int i = 0; i < table.size(); i++)
	for (int j = 0; j < table[i].size(); j++)
	  vwrk[table[i][j]] = vwrktmp[idx++]; 
    }
  //
  // Send the properly-ordered vwrk to all processors.
  //
  ParallelDescriptor::Bcast(vwrk.dataPtr(), vwrk.size(), IOProc);
#endif

  DistributionMapping res;
  //
  // This call doesn't invoke the MinimizeCommCosts() stuff.
  //
  res.KnapSackProcessorMap(vwrk,ParallelDescriptor::NProcs());

  return res;
}

#if defined(AMANZI)
static
BoxArray
ChemistryGrids (const MultiFab& state,
                const Amr*      parent,
                int             level)
{
    //
    // Let's chop the grids up a bit.
    //
    // We want to try and level out the chemistry work.
    //
    const int NProcs = ParallelDescriptor::NProcs();

    BoxArray ba = state.boxArray();

    bool done = false;

    for (int cnt = 1; !done; cnt *= 2)
    {
        const int ChunkSize = parent->maxGridSize(level)/cnt;

        if (ChunkSize < 16)
            //
            // Don't let grids get too small. 
            //
            break;

        IntVect chunk(D_DECL(ChunkSize,ChunkSize,ChunkSize));

        for (int j = 0; j < BL_SPACEDIM && ba.size() < 3*NProcs; j++)
        {
            chunk[j] /= 2;

            ba.maxSize(chunk);

            if (ba.size() >= 3*NProcs) done = true;
        }
    }

    return ba;
}
#endif

//
// ODE-solve for chemistry: cell-by-cell
//
void
PorousMedia::strang_chem (MultiFab&  state,
			  Real       dt,
                          int        ngrow)
{
    BL_PROFILE(BL_PROFILE_THIS_NAME() + "::strang_chem()");

    const Real strt_time = ParallelDescriptor::second();

#if defined(AMANZI)
    //
    // ngrow == 0 -> we're working on the valid region of state.
    //
    // ngrow > 0  -> we're working on aux_boundary_data_old with that many grow cells.
    //
    int tnum = 1;
#ifdef BL_USE_OMP
    tnum = omp_get_max_threads();
#endif

    BL_ASSERT(state.nComp() >= ncomps+ntracers);

    for (int ithread = 0; ithread < tnum; ithread++)
    {
        BL_ASSERT(components[ithread].minerals.size() == n_minerals);
        BL_ASSERT(components[ithread].total.size() == n_total);
        BL_ASSERT(components[ithread].free_ion.size() == n_total);
        BL_ASSERT(components[ithread].total_sorbed.size() == n_sorbed);
        BL_ASSERT(components[ithread].ion_exchange_sites.size() == 0);
    }
    //
    // Assume we are always doing funccount.
    //
    BoxArray            ba = ChemistryGrids(state, parent, level);
    DistributionMapping dm = getFuncCountDM(ba,ngrow);

    if (verbose > 1 && ParallelDescriptor::IOProcessor())
    {
        if (ngrow == 0)
            std::cout << "*** strang_chem: FABs in tmp MF covering valid region: " << ba.size() << std::endl;
        else
            std::cout << "*** strang_chem: FABs in tmp MF covering aux_boundary_data_old: " << ba.size() << std::endl;
    }
  
    MultiFab stateTemp, phiTemp, volTemp, fcnCntTemp;

    stateTemp.define(ba, state.nComp(), 0, dm, Fab_allocate);

    stateTemp.copy(state,0,0,state.nComp());  // Parallel copy.

    phiTemp.define(ba, 1, 0, dm, Fab_allocate);

    if (ngrow == 0)
    {
        phiTemp.copy(*rock_phi,0,0,1);
    }
    else
    {
        BL_ASSERT(rock_phi->nGrow() >= ngrow);

        BoxArray ba(rock_phi->boxArray());

        ba.grow(ngrow);

        MultiFab phiGrow(ba, 1, 0);

        for (MFIter mfi(*rock_phi); mfi.isValid(); ++mfi)
            phiGrow[mfi].copy((*rock_phi)[mfi],0,0,1);

        phiTemp.copy(phiGrow,0,0,1);  // Parallel copy.
    }
    //
    // This gets set by the chemistry solver.
    //
    fcnCntTemp.define(ba, 1, 0, dm, Fab_allocate);
    //
    // It's cheaper to just build a new volume than doing a parallel copy
    // from the existing one.  Additionally this also works when ngrow > 0.
    //
    volTemp.define(ba, 1, 0, dm, Fab_allocate);
    for (MFIter mfi(volTemp); mfi.isValid(); ++mfi)
        geom.GetVolume(volTemp[mfi], volTemp.boxArray(), mfi.index(), 0);
  
    for (MFIter mfi(stateTemp); mfi.isValid(); ++mfi)
    {
        FArrayBox& fab     = stateTemp[mfi];
        FArrayBox& phi_fab = phiTemp[mfi];
        FArrayBox& vol_fab = volTemp[mfi];
        FArrayBox& fct_fab = fcnCntTemp[mfi];
        const int* lo      = fab.loVect();
        const int* hi      = fab.hiVect();

#if (BL_SPACEDIM == 2)

        for (int iy=lo[1]; iy<=hi[1]; iy++)
        {
            int threadid = 0;

            amanzi::chemistry::SimpleThermoDatabase&     TheChemSolve = chemSolve[threadid];
	    amanzi::chemistry::Beaker::BeakerComponents& TheComponent = components[threadid];
            amanzi::chemistry::Beaker::BeakerParameters& TheParameter = parameters[threadid];

            for (int ix=lo[0]; ix<=hi[0]; ix++)
            {

#elif (BL_SPACEDIM == 3)

#ifdef BL_USE_OMP
#pragma omp parallel for schedule(dynamic,1) 
#endif
        for (int iz = lo[2]; iz<=hi[2]; iz++)
        {		
            int threadid = 0;
#ifdef BL_USE_OMP
            threadid = omp_get_thread_num();
#endif
            amanzi::chemistry::SimpleThermoDatabase&     TheChemSolve = chemSolve[threadid];
            amanzi::chemistry::Beaker::BeakerComponents& TheComponent = components[threadid];
            amanzi::chemistry::Beaker::BeakerParameters& TheParameter = parameters[threadid];

            for (int iy = lo[1]; iy<=hi[1]; iy++)
            {
                for (int ix = lo[0]; ix<=hi[0]; ix++)
                {
#else
#error "We only support 2 or 3-D"
#endif
                    int idx_minerals = 0, idx_sorbed = 0, idx_total = 0;

                    IntVect iv(D_DECL(ix,iy,iz));

                    bool allzero = true;

                    for (int icmp = 0; icmp < ntracers; icmp++)
                    {
                        Real               val  = fab(iv,icmp+ncomps);
                        const std::string& name = qNames[tType[icmp]];

                        allzero = allzero && (val == 0);

                        if (solid.compare(name) == 0)
                        {
                            TheComponent.minerals[idx_minerals++] = val;
                        }
                        else if (absorbed.compare(name) == 0)
                        {
                            TheComponent.total_sorbed[idx_sorbed++] = val;
                        }
                        else
                        {
                            TheComponent.total[idx_total++]= val;
                        }
                    }

                    Real sat_tmp = fab(iv,0) / density[0];

                    sat_tmp = std::min(1.e0,sat_tmp);
                    sat_tmp = std::max(0.e0,sat_tmp);

                    TheParameter.porosity   = phi_fab(iv,0);
                    TheParameter.saturation = sat_tmp;
                    TheParameter.volume     = vol_fab(iv,0);

                    if (allzero) continue;

                    amanzi::chemistry::Beaker::SolverStatus stat;
	  
                    try
                    {
                        TheChemSolve.ReactionStep(&TheComponent,TheParameter,dt);

                        stat = TheChemSolve.status();

                        fct_fab(iv,0) = use_funccount ? stat.num_rhs_evaluations : 1;
                    }
                    catch (const amanzi::chemistry::ChemistryException& geochem_error)
                    {
                        std::cout << iv << " : ";
                        for (int icmp = 0; icmp < ntracers; icmp++)
                            std::cout << fab(iv,icmp+ncomps) << ' ';
                        std::cout << std::endl;

                        BoxLib::Abort(geochem_error.what());	    
                    }
                    //
                    // After calculating the change in the tracer species,
                    // update the state variables.
                    //
                    idx_minerals = idx_sorbed = idx_total = 0;
                    //
                    //
                    //
                    for (int icmp = ncomps; icmp < ncomps+ntracers; icmp++)
                    {
                        const std::string& name = qNames[tType[icmp-ncomps]];

                        if (solid.compare(name) == 0)
                        {
                            fab(iv,icmp) = TheComponent.minerals[idx_minerals++];
                        }
                        else if (absorbed.compare(name) == 0)
                        {
                            fab(iv,icmp) = TheComponent.total_sorbed[idx_sorbed++];
                        }
                        else
                        {
                            fab(iv,icmp) = TheComponent.total[idx_total++];
                        }
                    }		
                }
            }
#if (BL_SPACEDIM == 3)
          }
#endif
	}
        phiTemp.clear();
        volTemp.clear();

        state.copy(stateTemp,ncomps,ncomps,ntracers); // Parallel copy.

        stateTemp.clear();

        MultiFab& Fcnt = get_new_data(FuncCount_Type);

        if (ngrow == 0)
        {
            Fcnt.copy(fcnCntTemp,0,0,1); // Parallel copy.

            fcnCntTemp.clear();

            state.FillBoundary();
            Fcnt.FillBoundary();

            geom.FillPeriodicBoundary(state,true);
            geom.FillPeriodicBoundary(Fcnt,true);
        }
        else
        {
            //
            // Can't directly use a parallel copy to update FuncCount_Type.
            //
            BoxArray ba = Fcnt.boxArray();
            ba.grow(ngrow);
            MultiFab grownFcnt(ba, 1, 0);
            grownFcnt.setVal(1);
                
            for (MFIter mfi(Fcnt); mfi.isValid(); ++mfi)
                grownFcnt[mfi].copy(Fcnt[mfi]);

            grownFcnt.copy(fcnCntTemp); // Parallel copy.

            fcnCntTemp.clear();

            for (MFIter mfi(grownFcnt); mfi.isValid(); ++mfi)
                Fcnt[mfi].copy(grownFcnt[mfi]);
        }

#else /* Not AMANZI */

        if (do_chem == 0)
        {
            MultiFab tmp;
            tmp.define(state.boxArray(),ncomps,0,Fab_allocate);
            tmp.copy(state,0,0,ncomps);

            for (MFIter mfi(state);mfi.isValid();++mfi)
            {
                const int* s_lo  = tmp[mfi].loVect();
                const int* s_hi  = tmp[mfi].hiVect();
                const Real* sdat = tmp[mfi].dataPtr();

                if (ncomps == 4) 
                    FORT_CHEM_DUMMY(sdat,ARLIM(s_lo),ARLIM(s_hi),&dt,&ncomps);
            }     
            state.copy(tmp,0,0,ncomps);
        }
#endif

  if (verbose > 1 && ParallelDescriptor::IOProcessor())
  {
    const int IOProc   = ParallelDescriptor::IOProcessorNumber();
    Real      run_time = ParallelDescriptor::second() - strt_time;
    ParallelDescriptor::ReduceRealMax(run_time,IOProc);

    std::cout << "PorousMedia::strang_chem time: " << run_time << '\n';
  }
}
	

void
PorousMedia::set_preferred_boundary_values (MultiFab& S,
					    int       state_index,
					    int       src_comp,
					    int       dst_comp,
					    int       num_comp,
					    Real      time) const
{
  BL_PROFILE(BL_PROFILE_THIS_NAME() + "::set_preferred_boundary_values()");

  if (state_index == State_Type)
  {
      const TimeLevel whichTime = which_time(State_Type,time);
      //
      // To get chem-advanced data instead of FP'd data at old time.
      //
      // For AMANZI the chem-advanced data are the tracers.
      //
      if (!FillPatchedOldState_ok && whichTime == AmrOldTime)
      {
          if (src_comp == ncomps && num_comp == ntracers)
          {
              aux_boundary_data_old.copyTo(S, src_comp, dst_comp, num_comp);
          }
      }
  }
}

//
// Compute capillary update.  This assumes there are only 2 phases and
// incompressible.  We only solve for component 1, and solution to 
// component 2 are deduced from component 1.
//
void
PorousMedia::scalar_capillary_update (Real      dt,
				      int       corrector,
				      MultiFab* u_mac)

{
  BL_PROFILE(BL_PROFILE_THIS_NAME() + "::scalar_capillary_update()");

  BL_ASSERT(nphases == 2);
  BL_ASSERT(have_capillary == 1);

  const Real strt_time = ParallelDescriptor::second();

  // Build single component edge-centered array of MultiFabs for fluxes
  MultiFab** fluxSCn;
  MultiFab** fluxSCnp1;
  const int nGrow = 0;
  const int nComp = 1;
  diffusion->allocFluxBoxesLevel(fluxSCn,  nGrow,nComp);
  diffusion->allocFluxBoxesLevel(fluxSCnp1,nGrow,nComp);

  int nc = 0; 
  int nd = 1;
  MultiFab*  delta_rhs = 0;
  MultiFab*  alpha     = 0;
  MultiFab** cmp_pcn   = 0;
  MultiFab** cmp_pcnp1 = 0;
  MultiFab** cmp_pcnp1_dp = 0;
  MultiFab&  S_new = get_new_data(State_Type);

  MultiFab* sat_res_mf = new MultiFab(grids,1,1);
  sat_res_mf->setVal(1.);
  for (MFIter mfi(*sat_res_mf); mfi.isValid();++mfi)
    {
      const Box& box = (*sat_res_mf)[mfi].box();
      (*sat_res_mf)[mfi].minus((*cpl_coef)[mfi],box,3,0,1);
    }
  sat_res_mf->mult(density[nc]);
  diffusion->set_rho(sat_res_mf); 

  MultiFab* S_nwt = new MultiFab(grids,1,1);
  MultiFab::Copy(*S_nwt,S_new,nc,0,nComp,1);

  alpha = new MultiFab(grids, 1, 1);
  MultiFab::Copy(*alpha,*rock_phi,0,0,1,alpha->nGrow());
  
  // Newton method.
  // initialization
  Real pcTime = state[State_Type].prevTime();
  diffusion->allocFluxBoxesLevel(cmp_pcn,0,1);
  calcCapillary(pcTime);
  calcDiffusivity_CPL(cmp_pcn,lambda_cc); 
  diffusion->diffuse_init_CPL(dt,nc,be_cn_theta,
			      fluxSCn,0,delta_rhs,
			      alpha,cmp_pcn,pcn_cc,S_nwt);
  pcTime = state[State_Type].curTime();
  FillStateBndry (pcTime,State_Type,0,ncomps);
  diffusion->allocFluxBoxesLevel(cmp_pcnp1,0,1);
  diffusion->allocFluxBoxesLevel(cmp_pcnp1_dp,0,1);
  calcCapillary(pcTime);
  calcLambda(pcTime);
  calcDiffusivity_CPL(cmp_pcnp1,lambdap1_cc);
  calcDiffusivity_CPL_dp(cmp_pcnp1_dp,lambdap1_cc,pcTime,1);

  int  max_itr_nwt = 20;
#if (BL_SPACEDIM == 3)
  Real max_err_nwt = 1e-10;
#else
  Real max_err_nwt = 1e-10;
#endif
  int  itr_nwt = 0;
  Real err_nwt = 1e10;
  Real be_theta = be_cn_theta;
  while ((itr_nwt < max_itr_nwt) && (err_nwt > max_err_nwt)) 
    {
      diffusion->diffuse_iter_CPL(dt,nc,ncomps,be_theta,
				  0,alpha,cmp_pcnp1,cmp_pcnp1_dp,
				  pcnp1_cc,S_nwt,&err_nwt);

      if (verbose > 1 && ParallelDescriptor::IOProcessor())
	std::cout << "Newton iteration " << itr_nwt 
	          << " : Error = "       << err_nwt << "\n"; 

      //scalar_adjust_constraint(0,ncomps-1);
      //FillStateBndry(pcTime,State_Type,0,ncomps);
      //calcCapillary(pcTime);
      //calcLambda(pcTime);
      calcDiffusivity_CPL(cmp_pcnp1,lambdap1_cc);
      calcDiffusivity_CPL_dp(cmp_pcnp1_dp,lambdap1_cc,pcTime,1);
      itr_nwt += 1;

      if (verbose > 1)
	check_minmax();
    }
    
  diffusion->compute_flux(nc,dt,be_cn_theta,fluxSCnp1,pcnp1_cc,cmp_pcnp1);

  if (verbose > 1 && ParallelDescriptor::IOProcessor())
    {
      if (itr_nwt < max_itr_nwt)
	std::cout << "Newton converged at iteration " << itr_nwt
		  << " with error " << err_nwt << '\n';
      else
	std::cout << "Newton failed to converged: termination error is "
		  <<  err_nwt << '\n'; 
    }

  //
  // add to phase velocity
  //
  if (u_mac != 0) {
      
    FArrayBox fluxtot;

    for (int d = 0; d < BL_SPACEDIM; d++) 
      {
	for (MFIter fmfi(*fluxSCn[d]); fmfi.isValid(); ++fmfi) 
	  {
	    const Box& ebox = (*fluxSCn[d])[fmfi].box();
	    fluxtot.resize(ebox,nComp);
	    fluxtot.copy((*fluxSCn[d])[fmfi],ebox,0,ebox,0,nComp);
	    if (no_corrector == 1)
	      fluxtot.mult(2.0);
	    else
	      fluxtot.plus((*fluxSCnp1[d])[fmfi],ebox,0,0,nComp);

	    fluxtot.mult(-1.0/density[nc]);
	    fluxtot.divide(area[d][fmfi],0,0,1);
	    u_mac[d][fmfi].plus(fluxtot,ebox,0,0,nComp);
	  }
	u_mac[d].FillBoundary();
      }
  }

  //
  // Increment the viscous flux registers
  // The fluxes are - beta \nabla p_c. We accummulate flux assuming 
  // it is on the LHS.  Thus, we need to multiply by -dt due to the sign change.
  // 

  if (do_reflux && corrector) {

    FArrayBox fluxtot;
	  
    for (int d = 0; d < BL_SPACEDIM; d++) 
      {
	MultiFab fluxes;

	if (level < parent->finestLevel())
	  fluxes.define((*fluxSCn[d]).boxArray(), ncomps, 0, Fab_allocate);

	for (MFIter fmfi(*fluxSCn[d]); fmfi.isValid(); ++fmfi)
	  {
	    // for component nc
	    const Box& ebox = (*fluxSCn[d])[fmfi].box();

	    fluxtot.resize(ebox,ncomps);
	    fluxtot.copy((*fluxSCn[d])[fmfi],ebox,0,ebox,nc,1);
	    fluxtot.plus((*fluxSCnp1[d])[fmfi],ebox,0,nc,1);

	    (*fluxSCn[d])[fmfi].mult(-density[nd]/density[nc]);
	    (*fluxSCnp1[d])[fmfi].mult(-density[nd]/density[nc]);
	    fluxtot.copy((*fluxSCn[d])[fmfi],ebox,0,ebox,nd,1);
	    fluxtot.plus((*fluxSCnp1[d])[fmfi],ebox,0,nd,1);

	    if (level < parent->finestLevel())
	      fluxes[fmfi].copy(fluxtot);

	    if (level > 0)
	      getViscFluxReg().FineAdd(fluxtot,d,fmfi.index(),0,0,ncomps,-dt);
	  }

	if (level < parent->finestLevel())
	  getLevel(level+1).getViscFluxReg().CrseInit(fluxes,d,0,0,ncomps,dt);
		  
      }
  }
    
  //     nc = 0; 
  //     MultiFab::Copy(*S_nwt,S_new,nc,0,nComp,1);
  //     diffusion->check_consistency(dt,nc,mone,be_cn_theta,
  // 				 rho_flag,0,alpha,
  // 				 cmp_pcn,cmp_pcnp1,
  // 				 pcn_cc, pcnp1_cc,
  // 				 S_nwt,&err_nwt);

  delete delta_rhs;
  delete S_nwt;
  delete sat_res_mf;
  delete alpha;

  diffusion->removeFluxBoxesLevel(cmp_pcn);
  diffusion->removeFluxBoxesLevel(cmp_pcnp1);
  diffusion->removeFluxBoxesLevel(cmp_pcnp1_dp);
	   
  diffusion->removeFluxBoxesLevel(fluxSCn);
  diffusion->removeFluxBoxesLevel(fluxSCnp1);
    
  if (verbose > 1)
    {
      const int IOProc   = ParallelDescriptor::IOProcessorNumber();
      Real      run_time = ParallelDescriptor::second() - strt_time;

      ParallelDescriptor::ReduceRealMax(run_time,IOProc);

      if (ParallelDescriptor::IOProcessor())
        std::cout << "PorousMedia::scalar_CPL_update(): time: " << run_time << '\n';
    }
  
  //
  // Write out the min and max of each component of the new state.
  //
  if (verbose > 1) check_minmax();

}

//
// Compute capillary update.  This assumes there are only 2 phases and
// incompressible.  We only solve for component 1, and solution to 
// component 2 are deduced from component 1.
//
void
PorousMedia::diff_capillary_update (Real      dt,
				    int       corrector,
				    MultiFab* u_mac)
  
{
  BL_PROFILE(BL_PROFILE_THIS_NAME() + "::diff_capillary_update()");

  BL_ASSERT(nphases == 2);
  BL_ASSERT(have_capillary == 1);

  const Real strt_time = ParallelDescriptor::second();

  // Build single component edge-centered array of MultiFabs for fluxes
  MultiFab** fluxSCn;
  MultiFab** fluxSCnp1;
  const int nGrow = 0;
  const int nComp = 1;
  diffusion->allocFluxBoxesLevel(fluxSCn,  nGrow,nComp);
  diffusion->allocFluxBoxesLevel(fluxSCnp1,nGrow,nComp);

  int nc = 0; 
  int nd = 1;
  MultiFab*  delta_rhs    = 0;
  MultiFab*  alpha        = 0;
  MultiFab** tmp          = 0;
  MultiFab** cmp_pcnp1_dp = 0;
  MultiFab*  S_nwt = 0;
  MultiFab&  S_new = get_new_data(State_Type);
  diffusion->allocFluxBoxesLevel(cmp_pcnp1_dp,0,1);

  MultiFab* sat_res_mf = new MultiFab(grids,1,1);
  sat_res_mf->setVal(1.);
  for (MFIter mfi(*sat_res_mf); mfi.isValid();++mfi)
    {
      const Box& box = (*sat_res_mf)[mfi].box();
      (*sat_res_mf)[mfi].minus((*cpl_coef)[mfi],box,3,0,1);
    }
  sat_res_mf->mult(density[nc]);
  diffusion->set_rho(sat_res_mf); 

  S_nwt = new MultiFab(grids,1,1);
  MultiFab::Copy(*S_nwt,S_new,nc,0,nComp,1);

  alpha = new MultiFab(grids, 1, 1);
  MultiFab::Copy(*alpha,*rock_phi,0,0,1,alpha->nGrow());

  tmp = new MultiFab* [BL_SPACEDIM];
  for (int d=0; d<BL_SPACEDIM; d++)
    tmp[d] = &lambda[d];
  
  MultiFab* Stmp = new MultiFab(grids,1,1);
  MultiFab::Copy(*Stmp,*pcn_cc,0,0,1,1);
  MultiFab::Add(*Stmp,*pcnp1_cc,0,0,1,1);
  (*Stmp).mult(0.5);
  
  // Newton method.
  // initialization
  diffusion->diffuse_init_CPL(dt,nc,-be_cn_theta,
			      fluxSCn,0,delta_rhs,
			      alpha,tmp,Stmp,S_nwt);

  Real pcTime = state[State_Type].prevTime();
  
  Stmp->setVal(0);
  calcCapillary(pcTime);
  calcLambda(pcTime);
  calcDiffusivity_CPL(tmp,lambda_cc);

  diffusion->diffuse_init_CPL(dt,nc,be_cn_theta,
			      fluxSCnp1,0,delta_rhs,
			      alpha,tmp,pcn_cc,Stmp);

  MultiFab::Add(*S_nwt,*Stmp,0,0,1,0);
  delete Stmp;

  for (int d=0; d<BL_SPACEDIM; d++)
    MultiFab::Add(*fluxSCn[d],*fluxSCnp1[d],0,0,1,0);

  pcTime = state[State_Type].curTime();
  calcCapillary(pcTime);
  calcLambda(pcTime);
  calcDiffusivity_CPL(tmp,lambdap1_cc);
  calcDiffusivity_CPL_dp(cmp_pcnp1_dp,lambdap1_cc,pcTime,1);

  int  max_itr_nwt = 20;
#if (BL_SPACEDIM == 3)
  Real max_err_nwt = 1e-10;
#else
  Real max_err_nwt = 1e-10;
#endif
  int  itr_nwt = 0;
  Real err_nwt = 1e10;
  Real be_theta = be_cn_theta;

  while ((itr_nwt < max_itr_nwt) && (err_nwt > max_err_nwt)) 
    {
      diffusion->diffuse_iter_CPL(dt,nc,ncomps,be_theta,
				  0,alpha,tmp,cmp_pcnp1_dp,
				  pcnp1_cc,S_nwt,&err_nwt);

      if (verbose > 1 && ParallelDescriptor::IOProcessor())
	std::cout << "Newton iteration " << itr_nwt 
	          << " : Error = "       << err_nwt << "\n"; 

      //scalar_adjust_constraint(0,ncomps-1);
      //FillStateBndry(pcTime,State_Type,0,ncomps);
      //calcCapillary(pcTime);
      //calcLambda(pcTime);
      calcDiffusivity_CPL(tmp,lambdap1_cc);
      calcDiffusivity_CPL_dp(cmp_pcnp1_dp,lambdap1_cc,pcTime,1);
      itr_nwt += 1;

      if (verbose > 1)
	check_minmax();
    }

  diffusion->compute_flux(nc,dt,be_cn_theta,fluxSCnp1,pcnp1_cc,tmp);
    
  if (verbose > 1 && ParallelDescriptor::IOProcessor())
    {
      if (itr_nwt < max_itr_nwt)
	std::cout << "Newton converged at iteration " << itr_nwt
		  << " with error " << err_nwt << '\n';
      else
	std::cout << "Newton failed to converged: termination error is "
		  <<  err_nwt << '\n'; 
    }

  //
  // add to phase velocity
  //
  if (u_mac != 0) {
      
    FArrayBox fluxtot;

    for (int d = 0; d < BL_SPACEDIM; d++) 
      {
	for (MFIter fmfi(*fluxSCn[d]); fmfi.isValid(); ++fmfi) 
	  {
	    const Box& ebox = (*fluxSCn[d])[fmfi].box();
	    fluxtot.resize(ebox,nComp);
	    fluxtot.copy((*fluxSCn[d])[fmfi],ebox,0,ebox,0,nComp);
	    if (no_corrector == 1)
	      fluxtot.mult(2.0);
	    else
	      fluxtot.plus((*fluxSCnp1[d])[fmfi],ebox,0,0,nComp);

	    fluxtot.mult(-1.0/density[nc]);
	    fluxtot.divide(area[d][fmfi],0,0,1);
	    u_mac[d][fmfi].plus(fluxtot,ebox,0,0,nComp);
	  }
	u_mac[d].FillBoundary();
      }
  }

  //
  // Increment the viscous flux registers
  // The fluxes are - beta \nabla p_c. We accummulate flux assuming 
  // it is on the LHS.  Thus, we need to multiply by -dt due to the sign change.
  // 

  if (do_reflux && corrector) {

    FArrayBox fluxtot;
	  
    for (int d = 0; d < BL_SPACEDIM; d++) 
      {
	MultiFab fluxes;
	
	if (level < parent->finestLevel())
	  fluxes.define((*fluxSCn[d]).boxArray(), ncomps, 0, Fab_allocate);
	
	for (MFIter fmfi(*fluxSCn[d]); fmfi.isValid(); ++fmfi)
	  {
	    // for component nc
	    const Box& ebox = (*fluxSCn[d])[fmfi].box();
	    
	    fluxtot.resize(ebox,ncomps);
	    fluxtot.copy((*fluxSCn[d])[fmfi],ebox,0,ebox,nc,1);
	    fluxtot.plus((*fluxSCnp1[d])[fmfi],ebox,0,nc,1);

	    (*fluxSCn[d])[fmfi].mult(-density[nd]/density[nc]);
	    (*fluxSCnp1[d])[fmfi].mult(-density[nd]/density[nc]);
	    fluxtot.copy((*fluxSCn[d])[fmfi],ebox,0,ebox,nd,1);
	    fluxtot.plus((*fluxSCnp1[d])[fmfi],ebox,0,nd,1);

	    if (level < parent->finestLevel())
	      fluxes[fmfi].copy(fluxtot);

	    if (level > 0)
	      getViscFluxReg().FineAdd(fluxtot,d,fmfi.index(),0,0,ncomps,-dt);
	  }

	if (level < parent->finestLevel())
	  getLevel(level+1).getViscFluxReg().CrseInit(fluxes,d,0,0,ncomps,dt);
		  
      }
  }

  delete delta_rhs;
  delete S_nwt;
  delete alpha;
  delete sat_res_mf;
  delete [] tmp;

  diffusion->removeFluxBoxesLevel(cmp_pcnp1_dp);
  diffusion->removeFluxBoxesLevel(fluxSCn);
  diffusion->removeFluxBoxesLevel(fluxSCnp1);
    
  if (verbose > 1)
    {
      const int IOProc   = ParallelDescriptor::IOProcessorNumber();
      Real      run_time = ParallelDescriptor::second() - strt_time;

      ParallelDescriptor::ReduceRealMax(run_time,IOProc);

      if (ParallelDescriptor::IOProcessor())
        std::cout << "PorousMedia::diff_CPL_update(): time: " << run_time << '\n';
    }

  //
  // Write out the min and max of each component of the new state.
  //
  if (verbose > 1) check_minmax();
}

#ifdef MG_USE_FBOXLIB
//
// Richard equation: Equilibrium solver
//
void
PorousMedia::richard_eqb_update (MultiFab* u_mac)

{
  BL_PROFILE(BL_PROFILE_THIS_NAME() + "::richards_update()");
  BL_ASSERT(nphases == 2);
  BL_ASSERT(have_capillary == 1);

  const Real strt_time = ParallelDescriptor::second();

  // Build single component edge-centered array of MultiFabs for fluxes
  MultiFab** fluxSC;
  const int nGrow = 0;
  const int nComp = 1;
  diffusion->allocFluxBoxesLevel(fluxSC,nGrow,nComp);

  int nc = 0; 
  MultiFab** cmp_pcp1    = 0;
  MultiFab** cmp_pcp1_dp = 0;
  MultiFab sat_res_mf(grids,1,1);
  sat_res_mf.setVal(1.);
  for (MFIter mfi(sat_res_mf); mfi.isValid();++mfi)
    {
      const Box& box = sat_res_mf[mfi].box();
      sat_res_mf[mfi].minus((*cpl_coef)[mfi],box,3,0,1);
    }
  //sat_res_mf.mult(density[nc]);
  diffusion->set_rho(&sat_res_mf); 

  // Compute first res_fix = \nabla v_inflow.  
  // Its value does not change.
  MultiFab res_fix(grids,1,0);
  res_fix.setVal(0.);
  calc_richard_velbc(res_fix);
 
  // Newton method.
  // initialization
  bool do_upwind = true;
  int  max_itr_nwt = 10;
  Real max_err_nwt = 1e-12;
  int  itr_nwt = 0;
  Real err_nwt = 1e10;
  Real pcTime = state[State_Type].curTime();
  MultiFab& P_new = get_new_data(Press_Type);
  FillStateBndry(pcTime,State_Type,0,ncomps);
  diffusion->allocFluxBoxesLevel(cmp_pcp1,0,1);
  diffusion->allocFluxBoxesLevel(cmp_pcp1_dp,0,3);
  calcCapillary(pcTime);
  calcLambda(pcTime);
  calc_richard_coef(cmp_pcp1,lambdap1_cc,u_mac,0,do_upwind);
  calc_richard_jac (cmp_pcp1_dp,lambdap1_cc,u_mac,pcTime,0,do_upwind);
  while ((itr_nwt < max_itr_nwt) && (err_nwt > max_err_nwt)) 
    {
      diffusion->richard_iter_eqb(nc,gravity,density,res_fix,
				  cmp_pcp1,cmp_pcp1_dp,pcnp1_cc,
				  u_mac,do_upwind,&err_nwt);      
      if (verbose > 1 && ParallelDescriptor::IOProcessor())
	std::cout << "Newton iteration " << itr_nwt 
	          << " : Error = "       << err_nwt << "\n"; 
      scalar_adjust_constraint(0,ncomps-1);
      FillStateBndry(pcTime,State_Type,0,ncomps);
      calcCapillary(pcTime);
      calcLambda(pcTime);
      MultiFab::Copy(P_new,*pcnp1_cc,0,0,1,1);
      P_new.mult(-1.0,1);
      compute_vel_phase(u_mac,0,pcTime);
      calc_richard_coef(cmp_pcp1,lambdap1_cc,u_mac,0,do_upwind);
      calc_richard_jac (cmp_pcp1_dp,lambdap1_cc,u_mac,pcTime,0,do_upwind);
      itr_nwt += 1;

      if (verbose > 1)	check_minmax();
    }
    
  diffusion->compute_flux(nc,1.0,1.0,fluxSC,pcnp1_cc,cmp_pcp1);

  if (verbose > 1 && ParallelDescriptor::IOProcessor())
    {
      if (itr_nwt < max_itr_nwt)
	std::cout << "Newton converged at iteration " << itr_nwt
		  << " with error " << err_nwt << '\n';
      else
	std::cout << "Newton failed to converged: termination error is "
		  <<  err_nwt << '\n'; 
    }

  /*
  //
  // Increment the viscous flux registers
  // The fluxes are - beta \nabla p_c. We accummulate flux assuming 
  // it is on the LHS.  Thus, we need to multiply by -dt due to the sign change.
  // 

  if (do_reflux && corrector) {

      FArrayBox fluxtot;
      for (int d = 0; d < BL_SPACEDIM; d++) 
	{
	  MultiFab fluxes;
	  
	  if (level < parent->finestLevel())
	    fluxes.define((*fluxSC[d]).boxArray(), nComp, 0, Fab_allocate);
	  
	  for (MFIter fmfi(*fluxSC[d]); fmfi.isValid(); ++fmfi)
	    {
	      // for component nc
	      const Box& ebox = (*fluxSC[d])[fmfi].box();
	      
	      fluxtot.resize(ebox,nComp);
	      fluxtot.copy((*fluxSC[d])[fmfi],ebox,0,ebox,0,1);

	      if (level < parent->finestLevel())
		fluxes[fmfi].copy(fluxtot);
	      
	      if (level > 0)
		getViscFluxReg().FineAdd(fluxtot,d,fmfi.index(),0,0,nComp,-dt);
	    }
	  
	  if (level < parent->finestLevel())
	    getLevel(level+1).getViscFluxReg().CrseInit(fluxes,d,0,0,nComp,dt);
      }
  }
  */
    
  diffusion->removeFluxBoxesLevel(cmp_pcp1);
  diffusion->removeFluxBoxesLevel(cmp_pcp1_dp);
  diffusion->removeFluxBoxesLevel(fluxSC);
    
  if (verbose > 1)
    {
      const int IOProc   = ParallelDescriptor::IOProcessorNumber();
      Real      run_time = ParallelDescriptor::second() - strt_time;

      ParallelDescriptor::ReduceRealMax(run_time,IOProc);

      if (ParallelDescriptor::IOProcessor())
        std::cout << "PorousMedia::richard_update(): time: " 
		  << run_time << '\n';
    }
  //
  // Write out the min and max of each component of the new state.
  //
  if (verbose > 1) check_minmax();

}

//
// Richard equation: Time-dependent solver.  Only doing a first-order implicit scheme
//
void
PorousMedia::richard_scalar_update (Real dt, int& total_nwt_iter, MultiFab* u_mac)

{
  BL_PROFILE(BL_PROFILE_THIS_NAME() + "::richards_update()");
  BL_ASSERT(have_capillary == 1);

  const Real strt_time = ParallelDescriptor::second();

  // Build single component edge-centered array of MultiFabs for fluxes
  MultiFab** fluxSC;
  const int nGrow = 0;
  const int nComp = 1;
  diffusion->allocFluxBoxesLevel(fluxSC,nGrow,nComp);

  int nc = 0; 
  MultiFab** cmp_pcp1    = 0;
  MultiFab** cmp_pcp1_dp = 0;
  MultiFab sat_res_mf(grids,1,1);
  sat_res_mf.setVal(1.);
  for (MFIter mfi(sat_res_mf); mfi.isValid();++mfi)
    {
      const Box& box = sat_res_mf[mfi].box();
      sat_res_mf[mfi].minus((*cpl_coef)[mfi],box,3,0,1);
    }
  diffusion->set_rho(&sat_res_mf);

  bool do_n = true;

  MultiFab& S_new = get_new_data(State_Type);
  MultiFab* alpha = new MultiFab(grids,1,1);
  MultiFab* dalpha = 0;
  if (!do_n) dalpha = new MultiFab(grids,1,1);
  MultiFab::Copy(*alpha,*rock_phi,0,0,1,alpha->nGrow());

  // Compute first res_fix = -\phi * n^k + dt*\nabla v_inflow.  
  // Its value does not change.
  MultiFab res_fix(grids,1,0);
  MultiFab::Copy(res_fix,S_new,nc,0,1,0);
  for (MFIter mfi(res_fix); mfi.isValid(); ++mfi)
    res_fix[mfi].mult((*alpha)[mfi],mfi.validbox(),0,0,1);
  res_fix.mult(-1.0);
  calc_richard_velbc(res_fix,dt*density[0]);
  // Newton method.
  // initialization
  bool do_upwind = true;
  int  max_itr_nwt = 20;
  Real max_err_nwt = 1e-12;
  int  itr_nwt = 0;
  Real err_nwt = 1e10;
  Real pcTime = state[State_Type].curTime();
  MultiFab& P_new = get_new_data(Press_Type);
  FillStateBndry(pcTime,State_Type,0,ncomps);
  diffusion->allocFluxBoxesLevel(cmp_pcp1,0,1);
  diffusion->allocFluxBoxesLevel(cmp_pcp1_dp,0,3);

  calcCapillary(pcTime);
  calcLambda(pcTime);
  compute_vel_phase(u_mac,0,pcTime);
  calc_richard_coef(cmp_pcp1,lambdap1_cc,u_mac,0,do_upwind);
  calc_richard_jac (cmp_pcp1_dp,lambdap1_cc,u_mac,pcTime,0,do_upwind,do_n);
  if (!do_n)
      calc_richard_alpha(dalpha,pcTime);

  while ((itr_nwt < max_itr_nwt) && (err_nwt > max_err_nwt)) 
    {
      if (do_n)
	diffusion->richard_iter(dt,nc,gravity,density,res_fix,
				alpha,cmp_pcp1,cmp_pcp1_dp,
				pcnp1_cc,u_mac,do_upwind,&err_nwt);      
      else
	diffusion->richard_iter_p(dt,nc,gravity,density,res_fix,
				  alpha,dalpha,cmp_pcp1,cmp_pcp1_dp,
				  pcnp1_cc,u_mac,do_upwind,&err_nwt);  
    
      if (verbose > 1 && ParallelDescriptor::IOProcessor())
	std::cout << "Newton iteration " << itr_nwt 
	          << " : Error = "       << err_nwt << "\n"; 
      if (model != model_list["richard"])
	scalar_adjust_constraint(0,ncomps-1);
      FillStateBndry(pcTime,State_Type,0,ncomps);
      calcCapillary(pcTime);
      calcLambda(pcTime);
      MultiFab::Copy(P_new,*pcnp1_cc,0,0,1,1);
      P_new.mult(-1.0,1);
      compute_vel_phase(u_mac,0,pcTime);
      calc_richard_coef(cmp_pcp1,lambdap1_cc,u_mac,0,do_upwind);
      calc_richard_jac (cmp_pcp1_dp,lambdap1_cc,u_mac,pcTime,0,do_upwind,do_n);
      if (!do_n)
	calc_richard_alpha(dalpha,pcTime);
      itr_nwt += 1;

      if (verbose > 1)	check_minmax();
    }
  total_nwt_iter = itr_nwt;
  diffusion->richard_flux(nc,-1.0,gravity,density,fluxSC,pcnp1_cc,cmp_pcp1);

  if (verbose > 1 && ParallelDescriptor::IOProcessor())
    {
      if (itr_nwt < max_itr_nwt)
	std::cout << "Newton converged at iteration " << itr_nwt
		  << " with error " << err_nwt << '\n';
      else
	std::cout << "Newton failed to converged: termination error is "
		  <<  err_nwt << '\n'; 
    }
  
  //
  // Increment the viscous flux registers
  // The fluxes are - beta \nabla p_c. We accummulate flux assuming 
  // it is on the LHS.  Thus, we need to multiply by -dt.
  // 
  if (do_reflux) 
    {

      FArrayBox fluxtot;
      for (int d = 0; d < BL_SPACEDIM; d++) 
	{
	  MultiFab fluxes;
	  
	  if (level < parent->finestLevel())
	    fluxes.define((*fluxSC[d]).boxArray(), nComp, 0, Fab_allocate);
	  
	  for (MFIter fmfi(*fluxSC[d]); fmfi.isValid(); ++fmfi)
	    {
	      // for component nc
	      const Box& ebox = (*fluxSC[d])[fmfi].box();
	      
	      fluxtot.resize(ebox,nComp);
	      fluxtot.copy((*fluxSC[d])[fmfi],ebox,0,ebox,0,1);
	      
	      if (level < parent->finestLevel())
		fluxes[fmfi].copy(fluxtot);
	      
	      if (level > 0)
		getViscFluxReg().FineAdd(fluxtot,d,fmfi.index(),0,0,nComp,-dt);
	    }
	  
	  if (level < parent->finestLevel())
	    getLevel(level+1).getViscFluxReg().CrseInit(fluxes,d,0,0,nComp,dt);
      }
  }
      
  delete alpha;
  if (dalpha) delete dalpha;
  diffusion->removeFluxBoxesLevel(cmp_pcp1);
  diffusion->removeFluxBoxesLevel(cmp_pcp1_dp);
  diffusion->removeFluxBoxesLevel(fluxSC);
    
  if (verbose > 1)
    {
      const int IOProc   = ParallelDescriptor::IOProcessorNumber();
      Real      run_time = ParallelDescriptor::second() - strt_time;

      ParallelDescriptor::ReduceRealMax(run_time,IOProc);

      if (ParallelDescriptor::IOProcessor())
        std::cout << "PorousMedia::richard_update(): time: " 
		  << run_time << '\n';
    }
  //
  // Write out the min and max of each component of the new state.
  //
  if (verbose > 1) check_minmax();

}

//
// Richard equation: Time-dependent solver.  Only doing a first-order implicit scheme
//
void
PorousMedia::richard_composite_update (Real dt, int& total_nwt_iter)

{
  BL_PROFILE(BL_PROFILE_THIS_NAME() + "::richards_composite_update()");
  BL_ASSERT(have_capillary == 1);

  const Real strt_time = ParallelDescriptor::second();

  int nlevs = parent->finestLevel() - level + 1;
  int nc = 0;

  // Create a nlevs-level array for the coefficients

  PArray <MultiFab> alpha(nlevs,PArrayManage);
  PArray <MultiFab> res_fix(nlevs,PArrayManage);
  PArray <MultiFab> pc(nlevs,PArrayManage);
  Array < PArray <MultiFab> > cmp_pcp1(BL_SPACEDIM);
  Array < PArray <MultiFab> > cmp_pcp1_dp(BL_SPACEDIM);
    
  for (int dir=0; dir<BL_SPACEDIM; dir++)
    {
      cmp_pcp1[dir].resize(nlevs,PArrayManage);
      cmp_pcp1_dp[dir].resize(nlevs,PArrayManage);
    }

  bool do_n = true;
  bool do_upwind = true;
  int  max_itr_nwt = 20;
  Real max_err_nwt = 1e-12;
  int  itr_nwt = 0;
  Real err_nwt = 1e10;
  Real pcTime = state[State_Type].curTime();
  for (int lev=0; lev<nlevs; lev++)
    {
      PorousMedia&    fine_lev   = getLevel(lev);
      const BoxArray& fine_grids = fine_lev.boxArray();      
      MultiFab& S_lev = fine_lev.get_new_data(State_Type);
      
      alpha.set(lev,new MultiFab(fine_grids,1,1));
      MultiFab::Copy(alpha[lev],*(fine_lev.rock_phi),0,0,1,1);

      res_fix.set(lev,new MultiFab(fine_grids,1,1));
      MultiFab::Copy(res_fix[lev],S_lev,nc,0,1,0);
      for (MFIter mfi(res_fix[lev]); mfi.isValid(); ++mfi)
	res_fix[lev][mfi].mult(alpha[lev][mfi],mfi.validbox(),0,0,1);
      res_fix[lev].mult(-1.0);
      fine_lev.calc_richard_velbc(res_fix[lev],dt*density[0]);

      MultiFab* tmp_cmp_pcp1[BL_SPACEDIM];
      MultiFab* tmp_cmp_pcp1_dp[BL_SPACEDIM];
      for (int dir=0;dir<BL_SPACEDIM;dir++)
      {
	BoxArray ba = fine_grids;
	ba.surroundingNodes(dir);
	cmp_pcp1[dir].set(lev, new MultiFab(ba,1,0));
	cmp_pcp1_dp[dir].set(lev, new MultiFab(ba,3,0));
	tmp_cmp_pcp1[dir] = &cmp_pcp1[dir][lev];
	tmp_cmp_pcp1_dp[dir] = &cmp_pcp1_dp[dir][lev];
      }
      
      fine_lev.calcCapillary(pcTime);
      fine_lev.calcLambda(pcTime);

      pc.set(lev,new MultiFab(fine_grids,1,1));
      MultiFab::Copy(pc[lev],*(fine_lev.pcnp1_cc),0,0,1,1);
      fine_lev.compute_vel_phase(fine_lev.u_mac_curr,0,pcTime);
      fine_lev.calc_richard_coef(tmp_cmp_pcp1,fine_lev.lambdap1_cc,
				 fine_lev.u_mac_curr,0,do_upwind);
      fine_lev.calc_richard_jac(tmp_cmp_pcp1_dp,fine_lev.lambdap1_cc,
				fine_lev.u_mac_curr,pcTime,0,do_upwind,do_n);
    }

  while ((itr_nwt < max_itr_nwt) && (err_nwt > max_err_nwt)) 
    {
      diffusion->richard_composite_iter(dt,nlevs,nc,gravity,density,res_fix,
					alpha,cmp_pcp1,cmp_pcp1_dp,pc,
					do_upwind,&err_nwt); 

      if (verbose > 1 && ParallelDescriptor::IOProcessor())
	std::cout << "Newton iteration " << itr_nwt 
	          << " : Error = "       << err_nwt << "\n"; 

      for (int lev=0; lev<nlevs; lev++)
	{
	  PorousMedia&    fine_lev   = getLevel(lev);   
	  MultiFab& P_lev = fine_lev.get_new_data(Press_Type);

	  fine_lev.FillStateBndry(pcTime,State_Type,0,ncomps);
	  fine_lev.calcCapillary(pcTime);
	  fine_lev.calcLambda(pcTime);

	  MultiFab::Copy(P_lev,*(fine_lev.pcnp1_cc),0,0,1,1);
	  P_lev.mult(-1.0,1);

	  MultiFab* tmp_cmp_pcp1[BL_SPACEDIM];
	  MultiFab* tmp_cmp_pcp1_dp[BL_SPACEDIM];
	  for (int dir=0;dir<BL_SPACEDIM;dir++)
	    {
	      tmp_cmp_pcp1[dir] = &cmp_pcp1[dir][lev];
	      tmp_cmp_pcp1_dp[dir] = &cmp_pcp1_dp[dir][lev];
	    }
	  MultiFab::Copy(pc[lev],*(fine_lev.pcnp1_cc),0,0,1,1);
	  fine_lev.compute_vel_phase(fine_lev.u_mac_curr,0,pcTime);
	  fine_lev.calc_richard_coef(tmp_cmp_pcp1,fine_lev.lambdap1_cc,
				     fine_lev.u_mac_curr,0,do_upwind);
	  fine_lev.calc_richard_jac(tmp_cmp_pcp1_dp,fine_lev.lambdap1_cc,
				    fine_lev.u_mac_curr,pcTime,0,do_upwind,do_n);

	}

      itr_nwt += 1;

      if (verbose > 1)	check_minmax();
    }

  total_nwt_iter = itr_nwt;

  if (verbose > 1 && ParallelDescriptor::IOProcessor())
    {
      if (itr_nwt < max_itr_nwt)
	std::cout << "Newton converged at iteration " << itr_nwt
		  << " with error " << err_nwt << '\n';
      else
	std::cout << "Newton failed to converged: termination error is "
		  <<  err_nwt << '\n'; 
    }
    
  if (verbose > 1)
    {
      const int IOProc   = ParallelDescriptor::IOProcessorNumber();
      Real      run_time = ParallelDescriptor::second() - strt_time;

      ParallelDescriptor::ReduceRealMax(run_time,IOProc);

      if (ParallelDescriptor::IOProcessor())
        std::cout << "PorousMedia::richard_update(): time: " 
		  << run_time << '\n';
    }
  //
  // Write out the min and max of each component of the new state.
  //
  if (verbose > 1) check_minmax();

}
#endif

//
// Enforce the constraint sum_i s_i = 1.  This is achieved by adjusting 
// the saturation of the dominant component specified in the input.
//
void
PorousMedia::scalar_adjust_constraint (int  first_scalar,
				       int  last_scalar)
{
  MultiFab&  S_new = get_new_data(State_Type);
    
  MultiFab S_adj(grids,1,S_new.nGrow());
  MultiFab S_div(grids,1,S_new.nGrow());
  S_adj.setVal(1.0);

  for (int kk=first_scalar; kk<=last_scalar; kk++)
    {
      if (solid.compare(pNames[pType[kk]]) != 0 && 
	  kk != idx_dominant) 
	{
	  MultiFab::Copy(S_div,S_new,kk,0,1,S_div.nGrow());
	  S_div.mult(1.0/density[kk],S_div.nGrow());
	  S_adj.minus(S_div,0,1,S_adj.nGrow());
	}
    }
  S_adj.mult(density[idx_dominant],S_div.nGrow());
  MultiFab::Copy(S_new,S_adj,0,idx_dominant,1,S_new.nGrow());
  S_new.FillBoundary();
  geom.FillPeriodicBoundary(S_new,true);

  

}

//
// Tag cells for refinement
//
void
PorousMedia::errorEst (TagBoxArray& tags,
		       int         clearval,
		       int         tagval,
		       Real        time,
		       int         n_error_buf, 
		       int         ngrow)
{
  const int*  domain_lo = geom.Domain().loVect();
  const int*  domain_hi = geom.Domain().hiVect();
  const Real* dx        = geom.CellSize();
  const Real* prob_lo   = geom.ProbLo();
  const Real* prob_hi   = geom.ProbHi();
  Array<int>  itags;

  //
  // Tag cells for refinement based on routine defined in PROB_$D.F
  //
  for (int j = 0; j < err_list.size(); j++)
    {
      MultiFab* mf = derive(err_list[j].name(), time, err_list[j].nGrow());

      for (MFIter mfi(*mf); mfi.isValid(); ++mfi)
        {
	  RealBox     gridloc = RealBox(grids[mfi.index()],geom.CellSize(),geom.ProbLo());
	  itags               = tags[mfi.index()].tags();
	  int*        tptr    = itags.dataPtr();
	  const int*  tlo     = tags[mfi.index()].box().loVect();
	  const int*  thi     = tags[mfi.index()].box().hiVect();
	  const int*  lo      = mfi.validbox().loVect();
	  const int*  hi      = mfi.validbox().hiVect();
	  const Real* xlo     = gridloc.lo();
	  Real*       dat     = (*mf)[mfi].dataPtr();
	  const int*  dlo     = (*mf)[mfi].box().loVect();
	  const int*  dhi     = (*mf)[mfi].box().hiVect();
	  const int   ncomp   = (*mf)[mfi].nComp();

	  err_list[j].errFunc()(tptr, ARLIM(tlo), ARLIM(thi), &tagval,
				&clearval, dat, ARLIM(dlo), ARLIM(dhi),
				lo,hi, &ncomp, domain_lo, domain_hi,
				dx, xlo, prob_lo, &time, &level);
	  //
	  // Don't forget to set the tags in the TagBox.
	  //
	  tags[mfi.index()].tags(itags);
        }
      delete mf;
    }

  //
  // Tag cells for refinement based on permeability values
  //
  if (do_kappa_refine == 1)
    { 
      Real kpset = 1.e-6;
      Array<int> itags;
      
      for (MFIter mfi(*kappa); mfi.isValid(); ++mfi)
	{
	  const int* k_lo  = (*kappa)[mfi].loVect();
	  const int* k_hi  = (*kappa)[mfi].hiVect();
	  const Real* kdat = (*kappa)[mfi].dataPtr();

	  itags            = tags[mfi.index()].tags();
	  const int* t_lo  = tags[mfi.index()].box().loVect();
	  const int* t_hi  = tags[mfi.index()].box().hiVect();
	  const int* tdat  = itags.dataPtr();

	  const int*  lo   = mfi.validbox().loVect();
	  const int*  hi   = mfi.validbox().hiVect();
	
	  FORT_KPERROR(tdat,ARLIM(t_lo),ARLIM(t_hi),
		       kdat,ARLIM(k_lo),ARLIM(k_hi),
		       &tagval, &kpset, dx, prob_lo, prob_hi,
		       lo, hi, domain_lo, domain_hi, &level);
	
	  tags[mfi.index()].tags(itags);
	}
    }
}

Real
PorousMedia::sumDerive (const std::string& name, Real time)
{
    Real      sum = 0.0;
    MultiFab* mf  = derive(name,time,0);

    BL_ASSERT(!(mf == 0));

    BoxArray baf;

    if (level < parent->finestLevel())
    {
        baf = parent->boxArray(level+1);
        baf.coarsen(fine_ratio);
    }

    for (MFIter mfi(*mf); mfi.isValid(); ++mfi)
    {
        FArrayBox& fab = mf->get(mfi);

        if (level < parent->finestLevel())
        {
            std::vector< std::pair<int,Box> > isects = baf.intersections(grids[mfi.index()]);

            for (int ii = 0, N = isects.size(); ii < N; ii++)
            {
                fab.setVal(0,isects[ii].second,0,fab.nComp());
            }
        }

        sum += fab.sum(0);
    }

    delete mf;

    ParallelDescriptor::ReduceRealSum(sum);

    return sum;
}

Real
PorousMedia::volWgtSum (const std::string& name,
			Real           time)
{
  Real        sum     = 0;
  const Real* dx      = geom.CellSize();
  MultiFab*   mf      = derive(name,time,0);

  BoxArray baf;

  if (level < parent->finestLevel())
  {
      baf = parent->boxArray(level+1);
      baf.coarsen(fine_ratio);
  }

  for (MFIter mfi(*mf); mfi.isValid(); ++mfi)
    {
      FArrayBox& fab = (*mf)[mfi];

      if (level < parent->finestLevel())
        {
            if (level < parent->finestLevel())
            {
                std::vector< std::pair<int,Box> > isects = baf.intersections(grids[mfi.index()]);

                for (int ii = 0, N = isects.size(); ii < N; ii++)
                {
                    fab.setVal(0,isects[ii].second,0,fab.nComp());
                }
            }
        }
      Real        s;
      const Real* dat = fab.dataPtr();
      const int*  dlo = fab.loVect();
      const int*  dhi = fab.hiVect();
      const int*  lo  = grids[mfi.index()].loVect();
      const int*  hi  = grids[mfi.index()].hiVect();

      FORT_SUMMASS(dat,ARLIM(dlo),ARLIM(dhi),ARLIM(lo),ARLIM(hi),dx,&s);

      sum += s;
    }

  delete mf;

  ParallelDescriptor::ReduceRealSum(sum);

  return sum;
}

void
PorousMedia::sum_integrated_quantities ()
{
  const int finest_level = parent->finestLevel();

  Real time = state[State_Type].curTime();
  Real mass = 0.0;

  for (int lev = 0; lev <= finest_level; lev++)
    {
      PorousMedia& ns_level = getLevel(lev);
      mass += ns_level.volWgtSum("Water",time);
    }

  if (verbose > 1 && ParallelDescriptor::IOProcessor())
    {
      const int old_prec = std::cout.precision(12);
      std::cout << "TIME= " << time << " MASS= " << mass << '\n';
      std::cout.precision(old_prec);
    }
}

void
PorousMedia::setPlotVariables()
{
  AmrLevel::setPlotVariables();
}

std::string
PorousMedia::thePlotFileType () const
{
  //
  // Increment this whenever the writePlotFile() format changes.
  //
  static const std::string the_plot_file_type("PorousMedia-V1.1");

  return the_plot_file_type;
}

void
PorousMedia::writePlotFile (const std::string& dir,
			    std::ostream&  os,
			    VisMF::How     how)
{
  if ( ! Amr::Plot_Files_Output() ) return;

  int i, n;
  //
  // The list of indices of State to write to plotfile.
  // first component of pair is state_type,
  // second component of pair is component # within the state_type
  //
  std::vector<std::pair<int,int> > plot_var_map;

  int noutput = desc_lst.size();
  for (int typ = 0; typ < noutput; typ++)
    for (int comp = 0; comp < desc_lst[typ].nComp();comp++)
      if (parent->isStatePlotVar(desc_lst[typ].name(comp)) &&
	  desc_lst[typ].getType() == IndexType::TheCellType())
	plot_var_map.push_back(std::pair<int,int>(typ,comp));

  int num_derive = 0;
  std::list<std::string> derive_names;
  const std::list<DeriveRec>& dlist = derive_lst.dlist();

  for (std::list<DeriveRec>::const_iterator it = dlist.begin();
       it != dlist.end();
       ++it)
    {
      if (parent->isDerivePlotVar(it->name()))
	{
	  derive_names.push_back(it->name());
	  num_derive += it->numDerive();
	}
    }

  int n_data_items = plot_var_map.size() + num_derive;
  Real cur_time = state[State_Type].curTime();

  if (level == 0 && ParallelDescriptor::IOProcessor())
    {
      //
      // The first thing we write out is the plotfile type.
      //
      os << thePlotFileType() << '\n';

      if (n_data_items == 0)
	BoxLib::Error("Must specify at least one valid data item to plot");

      os << n_data_items << '\n';

      //
      // Names of variables -- first state, then derived
      //
      for (i =0; i < plot_var_map.size(); i++)
        {
	  int typ  = plot_var_map[i].first;
	  int comp = plot_var_map[i].second;
	  os << desc_lst[typ].name(comp) << '\n';
        }

      for (std::list<std::string>::const_iterator it = derive_names.begin();
	   it != derive_names.end();
	   ++it)
        {
	  const DeriveRec* rec = derive_lst.get(*it);
	  for (i = 0; i < rec->numDerive(); i++)
	    os << rec->variableName(i) << '\n';
        }
      os << BL_SPACEDIM << '\n';
      os << parent->cumTime() << '\n';
      int f_lev = parent->finestLevel();
      os << f_lev << '\n';
      for (i = 0; i < BL_SPACEDIM; i++)
	os << Geometry::ProbLo(i) << ' ';
      os << '\n';
      for (i = 0; i < BL_SPACEDIM; i++)
	os << Geometry::ProbHi(i) << ' ';
      os << '\n';
      for (i = 0; i < f_lev; i++)
	os << parent->refRatio(i)[0] << ' ';
      os << '\n';
      for (i = 0; i <= f_lev; i++)
	os << parent->Geom(i).Domain() << ' ';
      os << '\n';
      for (i = 0; i <= f_lev; i++)
	os << parent->levelSteps(i) << ' ';
      os << '\n';
      for (i = 0; i <= f_lev; i++)
        {
	  for (int k = 0; k < BL_SPACEDIM; k++)
	    os << parent->Geom(i).CellSize()[k] << ' ';
	  os << '\n';
        }
      os << (int) Geometry::Coord() << '\n';
      os << "0\n"; // Write bndry data.
    }
  // Build the directory to hold the MultiFab at this level.
  // The name is relative to the directory containing the Header file.
  //
  static const std::string BaseName = "/Cell";

  std::string Level = BoxLib::Concatenate("Level_", level, 1);
  //
  // Now for the full pathname of that directory.
  //
  std::string FullPath = dir;
  if (!FullPath.empty() && FullPath[FullPath.length()-1] != '/')
    FullPath += '/';
  FullPath += Level;
  //
  // Only the I/O processor makes the directory if it doesn't already exist.
  //
  if (ParallelDescriptor::IOProcessor())
    if (!BoxLib::UtilCreateDirectory(FullPath, 0755))
      BoxLib::CreateDirectoryFailed(FullPath);
  //
  // Force other processors to wait till directory is built.
  //
  ParallelDescriptor::Barrier();

  if (ParallelDescriptor::IOProcessor())
    {
      os << level << ' ' << grids.size() << ' ' << cur_time << '\n';
      os << parent->levelSteps(level) << '\n';

      for (i = 0; i < grids.size(); ++i)
        {
	  RealBox gridloc = RealBox(grids[i],geom.CellSize(),geom.ProbLo());
	  for (n = 0; n < BL_SPACEDIM; n++)
	    os << gridloc.lo(n) << ' ' << gridloc.hi(n) << '\n';
        }
      //
      // The full relative pathname of the MultiFabs at this level.
      // The name is relative to the Header file containing this name.
      // It's the name that gets written into the Header.
      //
      if (n_data_items > 0)
        {
	  std::string PathNameInHeader = Level;
	  PathNameInHeader += BaseName;
	  os << PathNameInHeader << '\n';
        }
    }

  //
  // We combine all of the multifabs -- state, derived, etc -- into one
  // multifab -- plotMF.
  // NOTE: we are assuming that each state variable has one component,
  // but a derived variable is allowed to have multiple components.
  int       cnt   = 0;
  int       ncomp = 1;
  const int nGrow = 0;
  MultiFab  plotMF(grids,n_data_items,nGrow);
  MultiFab* this_dat = 0;
  //
  // Cull data from state variables -- use no ghost cells.
  //
  for (i = 0; i < plot_var_map.size(); i++)
    {
      int typ  = plot_var_map[i].first;
      int comp = plot_var_map[i].second;
      this_dat = &state[typ].newData();
      MultiFab::Copy(plotMF,*this_dat,comp,cnt,ncomp,nGrow);
      cnt+= ncomp;
    }
  //
  // Cull data from derived variables.
  // 
  Real plot_time;

  if (derive_names.size() > 0)
    {
      for (std::list<std::string>::const_iterator it = derive_names.begin();
	   it != derive_names.end();
	   ++it) 
	{
	  plot_time = cur_time;
	  const DeriveRec* rec = derive_lst.get(*it);
	  ncomp = rec->numDerive();
	  MultiFab* derive_dat = derive(*it,plot_time,nGrow);
	  MultiFab::Copy(plotMF,*derive_dat,0,cnt,ncomp,nGrow);
	  delete derive_dat;
	  cnt += ncomp;
	}
    }
  //
  // Use the Full pathname when naming the MultiFab.
  //
  std::string TheFullPath = FullPath;
  TheFullPath += BaseName;
  VisMF::Write(plotMF,TheFullPath,how,true);
}

Real
PorousMedia::estTimeStep (MultiFab* u_mac)
{
  if (fixed_dt > 0.0)
    {
      Real factor = 1.0;

      if (!(level == 0))
        {
	  int ratio = 1;
	  for (int lev = 1; lev <= level; lev++)
            {
	      ratio *= parent->nCycle(lev);
            }
	  factor = 1.0/double(ratio);
        }

      return factor*fixed_dt;
    }

  Real estdt        = 1.0e+20;
  const Real cur_time = state[State_Type].curTime();

  if (dt_eig != 0.0)
    {
      estdt = cfl * dt_eig;
    } 
  else 
    {
      int making_new_umac = 0;
      
      // Need to define the MAC velocities in order to define the initial dt 
      if (u_mac == 0) 
	{
	  making_new_umac = 1;
	  u_mac = new MultiFab[BL_SPACEDIM];
	  for (int dir = 0; dir < BL_SPACEDIM; dir++)
	    {
	      BoxArray edge_grids(grids);
	      edge_grids.surroundingNodes(dir);
	      u_mac[dir].define(edge_grids,1,0,Fab_allocate);
	      u_mac[dir].setVal(0.);
	    }
#ifdef MG_USE_FBOXLIB
	  if (model == model_list["richard"])
	    compute_vel_phase(u_mac,0,cur_time);
	  else
#endif
	    {
	      MultiFab* RhoD;
	      RhoD  = new MultiFab[BL_SPACEDIM];
	      for (int dir = 0; dir < BL_SPACEDIM; dir++)
		{
		  BoxArray edge_grids(grids);
		  edge_grids.surroundingNodes(dir);
		  RhoD[dir].define(edge_grids,1,0,Fab_allocate);
		  RhoD[dir].setVal(0.);
		}

	      initial_mac_project(u_mac,RhoD,cur_time);
	      delete [] RhoD;
	    }
	}

      predictDT(u_mac);

      estdt = cfl*dt_eig;

      if (making_new_umac)
	delete [] u_mac;
    }

  return estdt;
}

Real
PorousMedia::initialTimeStep (MultiFab* u_mac)
{
  return init_shrink*estTimeStep(u_mac);
}

void 
PorousMedia::predictDT (MultiFab* u_macG)
{
  BL_PROFILE(BL_PROFILE_THIS_NAME() + "::predictDT()");

  const Real* dx       = geom.CellSize();
  const Real  cur_time = state[State_Type].curTime();

  dt_eig = 1.e20;

  Real eigmax[BL_SPACEDIM] = { D_DECL(0,0,0) };
  for (FillPatchIterator S_fpi(*this,get_new_data(State_Type),GEOM_GROW,
			       cur_time,State_Type,0,ncomps);
       S_fpi.isValid();
       ++S_fpi)
    {

      dirichletStateBC(S_fpi(),GEOM_GROW,cur_time);

      const int i = S_fpi.index();

      Array<int> state_bc;
      state_bc = getBCArray(State_Type,i,0,1);

      Real eigmax_m[BL_SPACEDIM] = {D_DECL(0,0,0)};
      

      if (model == model_list["single-phase"])
	{
	  godunov->esteig_lin (grids[i], u_macG[0][i], u_macG[1][i],
#if (BL_SPACEDIM == 3)    
			       u_macG[2][i],
#endif
			       (*rock_phi)[i], eigmax_m);
	}
      else if (model == model_list["two-phase"])
	{
	  const int n_kr_coef = kr_coef->nComp();
	  if (do_cpl_advect)
	    {
	      godunov->esteig_cpl (grids[i], dx,
				   u_macG[0][i],kpedge[0][i],
				   u_macG[1][i],kpedge[1][i],
#if (BL_SPACEDIM == 3)    
				   u_macG[2][i],kpedge[2][i],
#endif
				   S_fpi(), (*pcnp1_cc)[i],
				   (*rock_phi)[i], 
				   (*kr_coef)[i], n_kr_coef,
				   state_bc.dataPtr(),eigmax_m);
	    }
	  else
	    godunov->esteig (grids[i], dx,
			     u_macG[0][i],kpedge[0][i],
			     u_macG[1][i],kpedge[1][i],
#if (BL_SPACEDIM == 3)    
			     u_macG[2][i],kpedge[2][i],
#endif
			     S_fpi(),(*rock_phi)[i], 
			     (*kr_coef)[i], n_kr_coef,
			     state_bc.dataPtr(),eigmax_m);
	}
    
      if (ntracers > 0)
	{
	  godunov->esteig_trc (grids[i], u_macG[0][i], u_macG[1][i],
#if (BL_SPACEDIM == 3)    
			       u_macG[2][i],
#endif
			       S_fpi(),1,(*rock_phi)[i] ,eigmax_m);
	}

      for (int dir = 0; dir < BL_SPACEDIM; dir++)
	{
	  eigmax[dir] = std::max(eigmax[dir],eigmax_m[dir]);
	  dt_eig = std::min(dt_eig,dx[dir]/eigmax_m[dir]);
	}
    }
  
  ParallelDescriptor::ReduceRealMin(dt_eig);

  if (verbose > 1)
    {
      const int IOProc   = ParallelDescriptor::IOProcessorNumber();
      ParallelDescriptor::ReduceRealMax(&eigmax[0], BL_SPACEDIM, IOProc);

      if (ParallelDescriptor::IOProcessor())
	{
	  for (int dir = 0; dir < BL_SPACEDIM; dir++)
	    std::cout << "Max Eig in dir " << dir << " = " << eigmax[dir] << '\n';
	  std::cout << "Max timestep = " << dt_eig << '\n';
	}
    }
}

void
PorousMedia::computeNewDt (int                   finest_level,
                           int                   sub_cycle,
                           Array<int>&           n_cycle,
                           const Array<IntVect>& ref_ratio,
                           Array<Real>&          dt_min,
                           Array<Real>&          dt_level,
                           Real                  stop_time,
                           int                   post_regrid_flag)
{
  //
  // We are at the end of a coarse grid timecycle.
  // Compute the timesteps for the next iteration.
  //
  if (level > 0) return;

  const int max_level = parent->maxLevel();

  n_cycle[0] = 1;
  for (int i = 1; i <= max_level; i++)
    {
      n_cycle[i] = sub_cycle ? parent->MaxRefRatio(i-1) : 1;
    }

  Real dt_0     = 1.0e20;
  int  n_factor = 1;
    
  for (int i = 0; i <= finest_level; i++)
    {
      PorousMedia* pm = dynamic_cast<PorousMedia*>(&parent->getLevel(i));
      dt_min[i] = std::min(dt_min[i],getLevel(i).estTimeStep(pm->u_mac_curr));
    }

  if (fixed_dt <= 0.0) 
    {
      if (post_regrid_flag == 1)
	{
          //
          // Limit dt's by pre-regrid dt
          //
          for (int i = 0; i <= finest_level; i++)
	    {
              dt_min[i] = std::min(dt_min[i],dt_level[i]);
	    }
	}
      else
	{
          //
          // Limit dt's by change_max * old dt
          //
          for (int i = 0; i <= finest_level; i++)
	    {
              dt_min[i] = std::min(dt_min[i],change_max*dt_level[i]);
	    }
	}
    }

  //
  // Find the minimum over all levels
  //
  for (int i = 0; i <= finest_level; i++)
    {
      n_factor *= n_cycle[i];
      dt_0      = std::min(dt_0,n_factor*dt_min[i]);
    }

  //
  // Limit dt's by the value of stop_time.
  //
  const Real eps      = 0.0001*dt_0;
  const Real cur_time = state[State_Type].curTime();
  if (stop_time >= 0.0)
    {
      if ((cur_time + dt_0) > (stop_time - eps))
	dt_0 = stop_time - cur_time;
    }
  //
  // Adjust the time step to be able to output checkpoints at specific times.
  //
  const Real check_per = parent->checkPer();
  if (check_per > 0.0)
    {
      int a = int((cur_time + eps ) / check_per);
      int b = int((cur_time + dt_0) / check_per);
      if (a != b)
	dt_0 = b * check_per - cur_time;
    }
  //
  // Adjust the time step to be able to output plot files at specific times.
  //
  const Real plot_per = parent->plotPer();
  if (plot_per > 0.0)
    {
      int a = int((cur_time + eps ) / plot_per);
      int b = int((cur_time + dt_0) / plot_per);
      if (a != b)
	dt_0 = b * plot_per - cur_time;
    }

  n_factor = 1;
  for (int i = 0; i <= max_level; i++)
    {
      n_factor   *= n_cycle[i];
      dt_level[i] = dt_0/( (Real)n_factor );
    }
}

void
PorousMedia::computeInitialDt (int                   finest_level,
                               int                   sub_cycle,
                               Array<int>&           n_cycle,
                               const Array<IntVect>& ref_ratio,
                               Array<Real>&          dt_level, 
                               Real                  stop_time)
{
  //
  // Grids have been constructed, compute dt for all levels.
  //
  if (level > 0)
    return;

  if (verbose && ParallelDescriptor::IOProcessor())
    std::cout << "... computing dt at level 0 only in computeInitialDt\n";

  const int max_level = parent->maxLevel();

  n_cycle[0] = 1;
  for (int i = 1; i <= max_level; i++)
    {
      n_cycle[i] = sub_cycle ? parent->MaxRefRatio(i-1) : 1;
    }

  Real dt_0    = 1.0e100;
  int n_factor = 1;
  for (int i = 0; i <= finest_level; i++)
    {
     
      const PorousMedia* pm = dynamic_cast<const PorousMedia*>(&parent->getLevel(i));
      dt_level[i] = getLevel(i).initialTimeStep(pm->u_mac_curr);
      n_factor   *= n_cycle[i];
      dt_0        = std::min(dt_0,n_factor*dt_level[i]);
    }

  if (stop_time >= 0.0)
    {
      const Real eps      = 0.0001*dt_0;
      const Real cur_time = state[State_Type].curTime();
      if ((cur_time + dt_0) > (stop_time - eps))
	dt_0 = stop_time - cur_time;
    }

  n_factor = 1;
  for (int i = 0; i <= max_level; i++)
    {
      n_factor   *= n_cycle[i];
      dt_level[i] = dt_0/( (Real)n_factor );
    }
}

//
// This function estimates the initial timesteping used by the model.
//

void
PorousMedia::post_init_estDT (Real&        dt_init,
                              Array<int>&  nc_save,
                              Array<Real>& dt_save,
                              Real         stop_time)
{
  const Real strt_time    = state[State_Type].curTime();
  const int  finest_level = parent->finestLevel();

  if (verbose && ParallelDescriptor::IOProcessor())
    std::cout << "... computing dt at all levels in post_init_estDT\n";

  dt_init = 1.0e+100;

  int  n_factor;

  // Create a temporary data structure for this solve -- this u_mac just
  //   used to compute dt.

  MultiFab* umac = 0;
  for (int k = 0; k <= finest_level; k++)
    {
      nc_save[k] = parent->nCycle(k);
      dt_save[k] = getLevel(k).initialTimeStep(umac);

      n_factor   = 1;
      for (int m = finest_level; m > k; m--) 
	n_factor *= parent->nCycle(m);
      dt_init    = std::min( dt_init, dt_save[k]/((Real) n_factor) );
    }
 
  Array<Real> dt_level(finest_level+1,dt_init);
  Array<int>  n_cycle(finest_level+1,1);

  Real dt0 = dt_save[0];
  n_factor = 1;
  for (int k = 0; k <= finest_level; k++)
    {
      n_factor *= nc_save[k];
      dt0       = std::min(dt0,n_factor*dt_save[k]);
    }

  if (stop_time >= 0.0)
    {
      const Real eps = 0.0001*dt0;
      if ((strt_time + dt0) > (stop_time - eps))
	dt0 = stop_time - strt_time;
    }

  n_factor = 1;
  for (int k = 0; k <= finest_level; k++)
    {
      n_factor  *= nc_save[k];
      dt_save[k] = dt0/( (Real) n_factor);
    }

  //
  // Hack.
  //
  parent->setDtLevel(dt_level);
  parent->setNCycle(n_cycle);
  for (int k = 0; k <= finest_level; k++)
    {
      getLevel(k).setTimeLevel(strt_time,dt_init,dt_init);
    }
}

//
// Fills in amrLevel okToContinue.
//

int
PorousMedia::okToContinue ()
{
  return (level > 0) ? true : (parent->dtLevel(0) > dt_cutoff);
}

//
// THE MAIN HOOKS INTO AMR AND AMRLEVEL
//

//
// Integration cycle on fine level grids is complete .
// post_timestep() is responsible for syncing levels together.
//
// The registers used for level syncing are initialized in the
// coarse level advance and incremented in the fine level advance.
// These quantities are described in comments above advance_setup.
//

void
PorousMedia::post_timestep (int crse_iteration)
{
  BL_PROFILE(BL_PROFILE_THIS_NAME() + "::post_timestep()");

  const int finest_level = parent->finestLevel();

  if (level < finest_level)
    {
      //avgDown();
      
      if (do_reflux) 
	{
	  reflux();
#ifdef MG_USE_FBOXLIB
	  if (model == model_list["richard"])
	    richard_sync();
	  else
#endif
	    mac_sync();
	}
    }

  //
  // Test for conservation.
  //
  if (level==0 && sum_interval>0 && 
      parent->levelSteps(0)%sum_interval == 0)
    sum_integrated_quantities();
    
  //
  // Print final solutions
  //
  if (level == 0)
    {      
      for (int lev = 0; lev <= finest_level; lev++)
	{
	  if (verbose && ParallelDescriptor::IOProcessor())
	    std::cout << "Final solutions at level = " 
		      << lev << '\n';

	  getLevel(lev).check_minmax(); 

	}
    }

  //
  // Compute observations
  //
  if (level == 0)
    {
      Observation::setAmrPtr(parent);
      Real prev_time = state[State_Type].prevTime();
      Real curr_time = state[State_Type].curTime();
      for (int i=0; i<observation_array.size(); ++i)
          observation_array[i].process(prev_time, curr_time);
    }

  if  ( (parent->cumTime() >=  stop_time || 
         parent->levelSteps(0) >= max_step)
        && ParallelDescriptor::IOProcessor())
  {
      if (verbose)
	{
	  for (int i=0; i<observation_array.size(); ++i)
	    {
                const std::map<int,Real> vals = observation_array[i].vals;
                for (std::map<int,Real>::const_iterator it=vals.begin();it!=vals.end(); ++it) 
                {
                    int j = it->first;
                    std::cout << i << " " << observation_array[i].name << " " 
                              << j << " " << observation_array[i].times[j] << " "
                              << it->second << std::endl;
                }
            }
	}
      
      std::ofstream out;
      out.open(obs_outputfile.c_str(),std::ios::out);
      out.precision(16);
      out.setf(std::ios::scientific);
      for (int i=0; i<observation_array.size(); ++i)
      {
          const std::map<int,Real> vals = observation_array[i].vals;
          for (std::map<int,Real>::const_iterator it=vals.begin();it!=vals.end(); ++it) 
          {
              int j = it->first;
              out << i << " " << observation_array[i].name << " " 
                  << j << " "  << observation_array[i].times[j] << " "
                  << it->second << std::endl;
          }
      }
      out.close();
    }

  old_intersect_new          = grids;
  is_first_step_after_regrid = false;

}

//
// Build any additional data structures after restart.
//
void PorousMedia::post_restart()
{
  if (level == 0)
    {
      Observation::setAmrPtr(parent);
      Real prev_time = state[State_Type].prevTime();
      Real curr_time = state[State_Type].curTime();
      for (int i=0; i<observation_array.size(); ++i)
          observation_array[i].process(prev_time, curr_time);
    }
}

//
// Build any additional data structures after regrid.
//
void
PorousMedia::post_regrid (int lbase,
                          int new_finest)
{
  BL_PROFILE(BL_PROFILE_THIS_NAME() + "::post_regrid()");
  //if (level > lbase)
  {
    //
    // Alloc MultiFab to hold rock quantities
    //
    if (kpedge   == 0) {
      kpedge = new MultiFab[BL_SPACEDIM];
      for (int dir = 0; dir < BL_SPACEDIM; dir++)
	{
	  BoxArray edge_grids(grids);
	  edge_grids.surroundingNodes(dir).grow(1);
	  kpedge[dir].define(edge_grids,1,0,Fab_allocate);
	}
    }	      
  }
}

void 
PorousMedia::init_rock_properties ()
{

  //
  // Determine rock properties.
  //
  const Real* dx = geom.CellSize();
  const int* domain_hi = geom.Domain().hiVect();

  const int max_level = parent->maxLevel();
  const Geometry& fgeom  = parent->Geom(max_level);

  int fratio = fine_ratio[0];
  int twoexp = 1;
  int ng_twoexp = 1;
  for (int ii = 0; ii<max_level; ii++) 
    {
      if (ii >= level) twoexp *= parent->refRatio(ii)[0];
      ng_twoexp *= parent->refRatio(ii)[0];
    }	
  ng_twoexp = ng_twoexp*3;

  int curr_grid_size = parent->maxGridSize(level);
  int new_grid_size  = 4;
  if (twoexp < curr_grid_size)
    new_grid_size  = curr_grid_size/twoexp;


  // permeability
  
  if (permeability_from_fine)
    {
      BoxArray tba(grids);
      tba.maxSize(new_grid_size);
      MultiFab tkappa(tba,1,3);
      tkappa.setVal(1.e40);
    
      MultiFab* tkpedge;
      tkpedge = new MultiFab[BL_SPACEDIM];
      for (int dir = 0; dir < BL_SPACEDIM; dir++)
	{
	  BoxArray tbe(tba);
	  tbe.surroundingNodes(dir).grow(1);
	  tkpedge[dir].define(tbe,1,0,Fab_allocate);
	  tkpedge[dir].setVal(1.e40);
	}

      BoxArray ba(tkappa.size());
      BoxArray ba2(tkappa.size());
      for (int i = 0; i < ba.size(); i++)
	{
	  Box bx = tkappa.box(i);
	  bx.refine(twoexp);
	  ba.set(i,bx);

	  bx.grow(ng_twoexp);
	  ba2.set(i,bx);
	}

      MultiFab mftmp(ba2,BL_SPACEDIM,0);
      mftmp.copy(*kappadata); 

      // mfbig has same CPU distribution as kappa
      MultiFab mfbig_kappa(ba,BL_SPACEDIM,ng_twoexp); 
      for (MFIter mfi(mftmp); mfi.isValid(); ++mfi)
	mfbig_kappa[mfi].copy(mftmp[mfi]);
      mftmp.clear();
      mfbig_kappa.FillBoundary();
      fgeom.FillPeriodicBoundary(mfbig_kappa,true);

      for (MFIter mfi(tkappa); mfi.isValid(); ++mfi)
	{
	  const int* lo    = mfi.validbox().loVect();
	  const int* hi    = mfi.validbox().hiVect();

	  const int* k_lo  = tkappa[mfi].loVect();
	  const int* k_hi  = tkappa[mfi].hiVect();
	  const Real* kdat = tkappa[mfi].dataPtr();

	  const int* kx_lo  = tkpedge[0][mfi].loVect();
	  const int* kx_hi  = tkpedge[0][mfi].hiVect();
	  const Real* kxdat = tkpedge[0][mfi].dataPtr();

	  const int* ky_lo  = tkpedge[1][mfi].loVect();
	  const int* ky_hi  = tkpedge[1][mfi].hiVect();
	  const Real* kydat = tkpedge[1][mfi].dataPtr();

#if(BL_SPACEDIM==3)
	  const int* kz_lo  = tkpedge[2][mfi].loVect();
	  const int* kz_hi  = tkpedge[2][mfi].hiVect();
	  const Real* kzdat = tkpedge[2][mfi].dataPtr();
#endif

	  const int* mf_lo  = mfbig_kappa[mfi].loVect();
	  const int* mf_hi  = mfbig_kappa[mfi].hiVect();
	  const Real* mfdat = mfbig_kappa[mfi].dataPtr();

	  FORT_INITKAPPA2(mfdat,ARLIM(mf_lo),ARLIM(mf_hi),
			  kdat,ARLIM(k_lo),ARLIM(k_hi),
			  kxdat,ARLIM(kx_lo),ARLIM(kx_hi),
			  kydat,ARLIM(ky_lo),ARLIM(ky_hi),
#if(BL_SPACEDIM==3)
			  kzdat,ARLIM(kz_lo),ARLIM(kz_hi),
#endif		      
			  lo,hi,&level,&max_level, &fratio);
	}

      mfbig_kappa.clear();

      for (int dir = 0; dir < BL_SPACEDIM; dir++)
	kpedge[dir].copy(tkpedge[dir]);
      delete [] tkpedge;

      BoxArray tba2(tkappa.boxArray());
      tba2.grow(3);
      MultiFab tmpgrow(tba2,1,0);
    
      for (MFIter mfi(tkappa); mfi.isValid(); ++mfi)
	tmpgrow[mfi].copy(tkappa[mfi]);

      tkappa.clear();

      tba2 = kappa->boxArray();
      tba2.grow(3);
      MultiFab tmpgrow2(tba2,1,0);

      tmpgrow2.copy(tmpgrow);
      tmpgrow.clear();

      for (MFIter mfi(tmpgrow2); mfi.isValid(); ++mfi)
	(*kappa)[mfi].copy(tmpgrow2[mfi]);
    }

  else 
    {
      int nlayer = rock_array.size();
      Array<Real> kappaval_x(nlayer), kappaval_y(nlayer), kappaval_z(nlayer);
      int mediumtype = 0;
      for (int i=0;i<nlayer;i++)
	{
	  kappaval_x[i] = rock_array[i].permeability[0];
	  kappaval_y[i] = rock_array[i].permeability[1];
#if(BL_SPACEDIM==3)     
	  kappaval_z[i] = rock_array[i].permeability[2];
#endif
	}

      for (MFIter mfi(*kappa); mfi.isValid(); ++mfi)
	{
	  const int* lo    = mfi.validbox().loVect();
	  const int* hi    = mfi.validbox().hiVect();
	  
	  const int* k_lo  = (*kappa)[mfi].loVect();
	  const int* k_hi  = (*kappa)[mfi].hiVect();
	  const Real* kdat = (*kappa)[mfi].dataPtr();
	  
	  const int* kx_lo  = kpedge[0][mfi].loVect();
	  const int* kx_hi  = kpedge[0][mfi].hiVect();
	  const Real* kxdat = kpedge[0][mfi].dataPtr();

	  const int* ky_lo  = kpedge[1][mfi].loVect();
	  const int* ky_hi  = kpedge[1][mfi].hiVect();
	  const Real* kydat = kpedge[1][mfi].dataPtr();

#if(BL_SPACEDIM==3)
	  const int* kz_lo  = kpedge[2][mfi].loVect();
	  const int* kz_hi  = kpedge[2][mfi].hiVect();
	  const Real* kzdat = kpedge[2][mfi].dataPtr();
#endif
	  FORT_INITKAPPA(kdat,ARLIM(k_lo),ARLIM(k_hi),
			 kxdat,ARLIM(kx_lo),ARLIM(kx_hi),
			 kydat,ARLIM(ky_lo),ARLIM(ky_hi),
#if(BL_SPACEDIM==3)
			 kzdat,ARLIM(kz_lo),ARLIM(kz_hi),
#endif		      
			 lo,hi,dx,geom.ProbHi(),
			 &level,&max_level,&mediumtype,
			 kappaval_x.dataPtr(), kappaval_y.dataPtr(),
#if (BL_SPACEDIM==3)
			 kappaval_z.dataPtr(),
#endif
			 &nlayer, &fratio);

      }
    
    }
  kappa->FillBoundary();
  (*kpedge).FillBoundary();
   
  // porosity

  if (porosity_from_fine) 
    {      
      BoxArray tba(grids);
      tba.maxSize(new_grid_size);
      MultiFab trock_phi(tba,1,3);
      trock_phi.setVal(1.e40);

      BoxArray ba(trock_phi.size());
      BoxArray ba2(trock_phi.size());
      for (int i = 0; i < ba.size(); i++)
	{
	  Box bx = trock_phi.box(i);
	  bx.refine(twoexp);
	  ba.set(i,bx);
	  bx.grow(ng_twoexp);
	  ba2.set(i,bx);
	}

      MultiFab mftmp(ba2,1,0);      
      mftmp.copy(*phidata);     
      
      // mfbig has same CPU distribution as phi
      MultiFab mfbig_phi(ba,1,ng_twoexp);
      for (MFIter mfi(mftmp); mfi.isValid(); ++mfi)
	mfbig_phi[mfi].copy(mftmp[mfi]);
      mftmp.clear();
      mfbig_phi.FillBoundary();
      fgeom.FillPeriodicBoundary(mfbig_phi,true);

      for (MFIter mfi(trock_phi); mfi.isValid(); ++mfi)
	{
	  const int* lo    = mfi.validbox().loVect();
	  const int* hi    = mfi.validbox().hiVect();

	  const int* p_lo  = trock_phi[mfi].loVect();
	  const int* p_hi  = trock_phi[mfi].hiVect();
	  const Real* pdat = trock_phi[mfi].dataPtr();
	  
	  const int*  mfp_lo = mfbig_phi[mfi].loVect();
	  const int*  mfp_hi = mfbig_phi[mfi].hiVect();
	  const Real* mfpdat = mfbig_phi[mfi].dataPtr();
	  
	  FORT_INITPHI2 (mfpdat, ARLIM(mfp_lo), ARLIM(mfp_hi),
			 pdat,ARLIM(p_lo),ARLIM(p_hi),
			 lo,hi,&level,&max_level, &fratio);
	}
      mfbig_phi.clear();

      BoxArray tba2(trock_phi.boxArray());
      tba2.grow(3);
      MultiFab tmpgrow(tba2,1,0);
      
      for (MFIter mfi(trock_phi); mfi.isValid(); ++mfi)
	tmpgrow[mfi].copy(trock_phi[mfi]);
      
      trock_phi.clear();

      tba2 = rock_phi->boxArray();
      tba2.grow(3);
      MultiFab tmpgrow2(tba2,1,0);

      tmpgrow2.copy(tmpgrow);
      tmpgrow.clear();

      for (MFIter mfi(tmpgrow2); mfi.isValid(); ++mfi)
	(*rock_phi)[mfi].copy(tmpgrow2[mfi]);
    }
  else
    { 
      int porosity_type = 0;
      (*rock_phi).setVal(rock_array[0].porosity);

      if (porosity_type != 0)
	{
	  int porosity_nlayer = rock_array.size();
	  Array<Real> porosity_val(porosity_nlayer);
	  for (int i=0;i<porosity_nlayer;i++)
	    porosity_val[i]=rock_array[i].porosity;

	  for (MFIter mfi(*rock_phi); mfi.isValid(); ++mfi)
	    {
	      const int* p_lo  = (*rock_phi)[mfi].loVect();
	      const int* p_hi  = (*rock_phi)[mfi].hiVect();
	      const Real* pdat = (*rock_phi)[mfi].dataPtr();
	      
	      FORT_INITPHI (pdat,ARLIM(p_lo),ARLIM(p_hi),
			    domain_hi, dx, &porosity_type,
			    porosity_val.dataPtr(),&porosity_nlayer);
	    }
	}
    }
  rock_phi->FillBoundary();
  
  if (model != model_list["single-phase"] || model != model_list["single-phase-solid"])
    {
      bool do_fine_average = true;
      // relative permeability
      FArrayBox tmpfab;
      Real dxf[BL_SPACEDIM];
      for (int i = 0; i<BL_SPACEDIM; i++)
	dxf[i] = dx[i]/twoexp;
      int n_kr_coef = kr_coef->nComp();
      for (MFIter mfi(*kr_coef); mfi.isValid(); ++mfi)
	{
	  if (do_fine_average)
	    {
	      // build data on finest grid
	      Box bx = (*kr_coef)[mfi].box();
	      bx.refine(twoexp);
	      tmpfab.resize(bx,n_kr_coef);
	      tmpfab.setVal(0.);

	      for (int i=0; i<rock_array.size(); i++)
		rock_array[i].set_constant_krval(tmpfab,region_array,dxf);

	      // average onto coarse grid
	      const int* p_lo  = (*kr_coef)[mfi].loVect();
	      const int* p_hi  = (*kr_coef)[mfi].hiVect();
	      const Real* pdat = (*kr_coef)[mfi].dataPtr();
	  
	      const int*  mfp_lo = tmpfab.loVect();
	      const int*  mfp_hi = tmpfab.hiVect();
	      const Real* mfpdat = tmpfab.dataPtr();
	      
	      FORT_INITKR (mfpdat, ARLIM(mfp_lo), ARLIM(mfp_hi),
			   pdat,ARLIM(p_lo),ARLIM(p_hi),&n_kr_coef,
			   &level, &max_level, &fratio);
	    }
	  else
	    {
	      for (int i=0; i<rock_array.size(); i++)
		rock_array[i].set_constant_krval((*kr_coef)[mfi],region_array,dx);
	    }
	}
      // capillary pressure
      int n_cpl_coef = cpl_coef->nComp();
      for (MFIter mfi(*cpl_coef); mfi.isValid(); ++mfi)
	{
	  if (do_fine_average)
	    {
	      // build data on finest grid
	      Box bx = (*cpl_coef)[mfi].box();
	      bx.refine(twoexp);
	      tmpfab.resize(bx,n_cpl_coef);
	      tmpfab.setVal(0.);

	      for (int i=0; i<rock_array.size(); i++)
		rock_array[i].set_constant_cplval(tmpfab,region_array,dxf);
	      
	      // average onto coarse grid
	      const int* p_lo  = (*cpl_coef)[mfi].loVect();
	      const int* p_hi  = (*cpl_coef)[mfi].hiVect();
	      const Real* pdat = (*cpl_coef)[mfi].dataPtr();
	      
	      const int*  mfp_lo = tmpfab.loVect();
	      const int*  mfp_hi = tmpfab.hiVect();
	      const Real* mfpdat = tmpfab.dataPtr();
	  
	      FORT_INITKR (mfpdat, ARLIM(mfp_lo), ARLIM(mfp_hi),
			   pdat,ARLIM(p_lo),ARLIM(p_hi),&n_cpl_coef,
			   &level,&max_level, &fratio);
	    }
	  else
	    {
	      for (int i=0; i<rock_array.size(); i++)
		rock_array[i].set_constant_cplval((*cpl_coef)[mfi],region_array,dx);
	    }
	}
    }
}

//
// Ensure state, and pressure are consistent.
//

void
PorousMedia::post_init (Real stop_time)
{
  BL_PROFILE(BL_PROFILE_THIS_NAME() + "::post_init()");

  if (level > 0)
    //
    // Nothing to sync up at level > 0.
    //
    return;

  const int   finest_level = parent->finestLevel();
  Real        dt_init      = 0.;
  Array<Real> dt_save(finest_level+1);
  Array<int>  nc_save(finest_level+1);
  //
  // Ensure state is consistent, i.e. velocity field is non-divergent,
  // Coarse levels are fine level averages, pressure is zero.
  // Call initial_mac_project in order to get a good initial dt.
  //
  post_init_state();
  //
  // Estimate the initial timestepping.
  //
  post_init_estDT(dt_init, nc_save, dt_save, stop_time);
    
  const Real strt_time       = state[State_Type].curTime();
  for (int k = 0; k <= finest_level; k++)
    getLevel(k).setTimeLevel(strt_time,dt_save[k],dt_save[k]);

  parent->setDtLevel(dt_save);
  parent->setNCycle(nc_save);

  //
  // Compute the initial estimate of conservation.
  //
  if (sum_interval > 0)
    sum_integrated_quantities();

  if (level == 0)
    {
      Observation::setAmrPtr(parent);
      Real prev_time = state[State_Type].prevTime();
      Real curr_time = state[State_Type].curTime();
      for (int i=0; i<observation_array.size(); ++i)
          observation_array[i].process(prev_time, curr_time);
    }
}

//
// MULTILEVEL SYNC FUNCTIONS
//


//
// This function ensures that the state is initially consistent
// with respect to the divergence condition and fields are initially consistent
//

void
PorousMedia::post_init_state ()
{
  BL_PROFILE(BL_PROFILE_THIS_NAME() + "::post_init_state()");

  const int finest_level = parent->finestLevel();
  PorousMedia::initial_step = true;

  //
  // Average scalar and pressure data down from finer levels
  // so that conserved data is consistant between levels.
  //
  for (int k = finest_level-1; k>= 0; k--)
    {
      getLevel(k).avgDown();
    }
}

//
// Compute an initial MAC velocity in order to get a good first dt
//
void
PorousMedia::initial_mac_project (MultiFab* u_mac, MultiFab* RhoD, Real time)
{
  mac_project(u_mac,RhoD,time);

}

//
// Helper function for PorousMedia::SyncInterp().
//

static
void
set_bc_new (int*            bc_new,
            int             n,
            int             src_comp,
            const int*      clo,
            const int*      chi,
            const int*      cdomlo,
            const int*      cdomhi,
            const BoxArray& cgrids,
            int**           bc_orig_qty)
            
{
  for (int dir = 0; dir < BL_SPACEDIM; dir++)
    {
      int bc_index = (n+src_comp)*(2*BL_SPACEDIM) + dir;
      bc_new[bc_index]             = INT_DIR;
      bc_new[bc_index+BL_SPACEDIM] = INT_DIR;
 
      if (clo[dir] < cdomlo[dir] || chi[dir] > cdomhi[dir])
        {
	  for (int crse = 0; crse < cgrids.size(); crse++)
            {
	      const int* c_lo = cgrids[crse].loVect();
	      const int* c_hi = cgrids[crse].hiVect();

	      if (clo[dir] < cdomlo[dir] && c_lo[dir] == cdomlo[dir])
		bc_new[bc_index] = bc_orig_qty[crse][bc_index];
	      if (chi[dir] > cdomhi[dir] && c_hi[dir] == cdomhi[dir])
		bc_new[bc_index+BL_SPACEDIM] = bc_orig_qty[crse][bc_index+BL_SPACEDIM]; 
            }
        }
    }
}

//
// Interpolate A cell centered Sync correction from a
// coarse level (c_lev) to a fine level (f_lev).
//
// This routine interpolates the num_comp components of CrseSync
// (starting at src_comp) and either increments or puts the result into
// the num_comp components of FineSync (starting at dest_comp)
// The components of bc_orig_qty corespond to the quantities of CrseSync.
//

void
PorousMedia::SyncInterp (MultiFab&      CrseSync,
			 int            c_lev,
			 MultiFab&      FineSync,
			 int            f_lev,
			 IntVect&       ratio,
			 int            src_comp,
			 int            dest_comp,
			 int            num_comp,
			 int            increment,
			 Real           dt_clev, 
			 int**          bc_orig_qty,
			 SyncInterpType which_interp,
			 int            state_comp)
{
  BL_ASSERT(which_interp >= 0 && which_interp <= 5);

  Interpolater* interpolater = 0;

  switch (which_interp)
    {
    case PC_T:           interpolater = &pc_interp;           break;
    case CellCons_T:     interpolater = &cell_cons_interp;    break;
    case CellConsLin_T:  interpolater = &lincc_interp;        break;
    case CellConsProt_T: interpolater = &protected_interp;    break;
    default:
      BoxLib::Abort("PorousMedia::SyncInterp(): how did this happen");
    }

  PorousMedia&   fine_level  = getLevel(f_lev);
  const BoxArray& fgrids     = fine_level.boxArray();
  const Geometry& fgeom      = parent->Geom(f_lev);
  const BoxArray& cgrids     = getLevel(c_lev).boxArray();
  const Geometry& cgeom      = parent->Geom(c_lev);
  const Real*     dx_crse    = cgeom.CellSize();
  Box             cdomain    = BoxLib::coarsen(fgeom.Domain(),ratio);
  const int*      cdomlo     = cdomain.loVect();
  const int*      cdomhi     = cdomain.hiVect();
  int*            bc_new     = new int[2*BL_SPACEDIM*(src_comp+num_comp)];

  BoxArray cdataBA(fgrids.size());

  for (int i = 0; i < fgrids.size(); i++)
    cdataBA.set(i,interpolater->CoarseBox(fgrids[i],ratio));
  //
  // Note: The boxes in cdataBA may NOT be disjoint !!!
  //
  MultiFab cdataMF(cdataBA,num_comp,0);

  cdataMF.setVal(0);

  cdataMF.copy(CrseSync, src_comp, 0, num_comp);
  //
  // Set physical boundary conditions in cdataMF.
  //
  for (MFIter mfi(cdataMF); mfi.isValid(); ++mfi)
    {
      int         i       = mfi.index();
      RealBox     gridloc = RealBox(fine_level.boxArray()[i],
				    fine_level.Geom().CellSize(),
				    fine_level.Geom().ProbLo());
      FArrayBox&  cdata   = cdataMF[mfi];
      const int*  clo     = cdata.loVect();
      const int*  chi     = cdata.hiVect();
      const Real* xlo     = gridloc.lo();

      for (int n = 0; n < num_comp; n++)
        {
	  set_bc_new(bc_new,n,src_comp,clo,chi,cdomlo,cdomhi,cgrids,bc_orig_qty);

	  FORT_FILCC(cdata.dataPtr(n), ARLIM(clo), ARLIM(chi),
		     cdomlo, cdomhi, dx_crse, xlo,
		     &(bc_new[2*BL_SPACEDIM*(n+src_comp)]));
        }
    }
  cgeom.FillPeriodicBoundary(cdataMF, 0, num_comp);
  //
  // Interpolate from cdataMF to fdata and update FineSync.
  // Note that FineSync and cdataMF will have the same distribution
  // since the length of their BoxArrays are equal.
  //
  FArrayBox    fdata;
  Array<BCRec> bc_interp(num_comp);

  MultiFab* fine_stateMF = 0;
  if (interpolater == &protected_interp)
    fine_stateMF = &(getLevel(f_lev).get_new_data(State_Type));

  for (MFIter mfi(cdataMF); mfi.isValid(); ++mfi)
    {
      int        i     = mfi.index();
      FArrayBox& cdata = cdataMF[mfi];
      const int* clo   = cdata.loVect();
      const int* chi   = cdata.hiVect();

      fdata.resize(fgrids[i], num_comp);
      //
      // Set the boundary condition array for interpolation.
      //
      for (int n = 0; n < num_comp; n++)
        {
	  set_bc_new(bc_new,n,src_comp,clo,chi,cdomlo,cdomhi,cgrids,bc_orig_qty);
        }

      for (int n = 0; n < num_comp; n++)
        {
	  for (int dir = 0; dir < BL_SPACEDIM; dir++)
            {
	      int bc_index = (n+src_comp)*(2*BL_SPACEDIM) + dir;
	      bc_interp[n].setLo(dir,bc_new[bc_index]);
	      bc_interp[n].setHi(dir,bc_new[bc_index+BL_SPACEDIM]);
            }
        }

      interpolater->interp(cdata,0,fdata,0,num_comp,fgrids[i],ratio,
			   cgeom,fgeom,bc_interp,src_comp,State_Type);

      if (increment)
        {
	  fdata.mult(dt_clev);

	  if (interpolater == &protected_interp) {

	    cdata.mult(dt_clev);
	    FArrayBox& fine_state = (*fine_stateMF)[i];
	    interpolater->protect(cdata,0,fdata,0,fine_state,state_comp,
				  num_comp,fgrids[i],ratio,
				  cgeom,fgeom,bc_interp);
	    Real dt_clev_inv = 1./dt_clev;
	    cdata.mult(dt_clev_inv);

	  }
            
	  FineSync[i].plus(fdata,0,dest_comp,num_comp);
        }
      else
        {
	  FineSync[i].copy(fdata,0,dest_comp,num_comp);
        }
    }

  delete [] bc_new;
}

//
// Interpolate sync pressure correction to a finer level.
//

void
PorousMedia::SyncProjInterp (MultiFab& phi,
			     int       c_lev,
			     MultiFab& P_new,
			     MultiFab& P_old,
			     int       f_lev,
			     IntVect&  ratio,
			     bool      first_crse_step_after_initial_iters,
			     Real      cur_crse_pres_time,
			     Real      prev_crse_pres_time)
{
  const Geometry& fgeom   = parent->Geom(f_lev);
  const BoxArray& P_grids = P_new.boxArray();
  const Geometry& cgeom   = parent->Geom(c_lev);

  BoxArray crse_ba(P_grids.size());

  for (int i = 0; i < P_grids.size(); i++)
    {
      crse_ba.set(i,node_bilinear_interp.CoarseBox(P_grids[i],ratio));
    }

  Array<BCRec> bc(BL_SPACEDIM);
  MultiFab     crse_phi(crse_ba,1,0);

  crse_phi.setVal(1.e200);
  crse_phi.copy(phi,0,0,1);

  FArrayBox     fine_phi;
  PorousMedia& fine_lev            = getLevel(f_lev);
  const Real    cur_fine_pres_time  = fine_lev.state[Press_Type].curTime();
  const Real    prev_fine_pres_time = fine_lev.state[Press_Type].prevTime();

  if (state[Press_Type].descriptor()->timeType() == 
      StateDescriptor::Point && first_crse_step_after_initial_iters)
    {
      const Real time_since_zero  = cur_crse_pres_time - prev_crse_pres_time;
      const Real dt_to_prev_time  = prev_fine_pres_time - prev_crse_pres_time;
      const Real dt_to_cur_time   = cur_fine_pres_time - prev_crse_pres_time;
      const Real cur_mult_factor  = dt_to_cur_time / time_since_zero;
      const Real prev_mult_factor = dt_to_prev_time / dt_to_cur_time;

      for (MFIter mfi(crse_phi); mfi.isValid(); ++mfi)
        {
	  fine_phi.resize(P_grids[mfi.index()],1);
	  fine_phi.setVal(1.e200);
	  node_bilinear_interp.interp(crse_phi[mfi],0,fine_phi,0,1,
				      fine_phi.box(),ratio,cgeom,fgeom,bc,
				      0,Press_Type);
	  fine_phi.mult(cur_mult_factor);
	  P_new[mfi.index()].plus(fine_phi);
	  fine_phi.mult(prev_mult_factor);
	  P_old[mfi.index()].plus(fine_phi);
        }
    }
  else 
    {
      for (MFIter mfi(crse_phi); mfi.isValid(); ++mfi)
        {
	  fine_phi.resize(P_grids[mfi.index()],1);
	  fine_phi.setVal(1.e200);
	  node_bilinear_interp.interp(crse_phi[mfi],0,fine_phi,0,1,
				      fine_phi.box(),ratio,cgeom,fgeom,bc,
				      0,Press_Type);
	  P_new[mfi.index()].plus(fine_phi);
	  P_old[mfi.index()].plus(fine_phi);

        }
    }
}

//
// Averages a multifab of fine data down onto a multifab of coarse data.
//
// This should be an Amrlevel or Multifab function
//
void
PorousMedia::avgDown (MultiFab* s_crse,
		      int c_lev,
		      MultiFab* s_fine, 
		      int f_lev) 
{
    PorousMedia&   fine_lev = getLevel(f_lev);
    PorousMedia&   crse_lev = getLevel(c_lev);
    const BoxArray& fgrids  = fine_lev.grids;
    MultiFab&       fvolume = fine_lev.volume;
    const BoxArray& cgrids  = crse_lev.grids;
    MultiFab&       cvolume = crse_lev.volume;
    IntVect         ratio   = parent->refRatio(c_lev);

    int nc = (*s_crse).nComp();
    avgDown(cgrids,fgrids,*s_crse,*s_fine,cvolume,fvolume,c_lev,f_lev,0,nc,ratio);
}

void
PorousMedia::avgDown (const BoxArray& cgrids,
		      const BoxArray& fgrids,
		      MultiFab&       S_crse,
		      MultiFab&       S_fine,
		      MultiFab&       cvolume,
		      MultiFab&       fvolume,
		      int             c_level,
		      int             f_level,
		      int             scomp,
		      int             ncomp,
		      const IntVect&  fratio)
{
  BL_ASSERT(cgrids == S_crse.boxArray());
  BL_ASSERT(fgrids == S_fine.boxArray());
  BL_ASSERT(cvolume.boxArray() == cgrids);
  BL_ASSERT(fvolume.boxArray() == fgrids);
  BL_ASSERT(S_crse.nComp() == S_fine.nComp());
  BL_ASSERT(fvolume.nComp() == 1 && cvolume.nComp() == 1);

  BL_PROFILE(BL_PROFILE_THIS_NAME() + "::avgDown()");
  //
  // Coarsen() the fine stuff on processors owning the fine data.
  //
  BoxArray crse_S_fine_BA(fgrids.size());

  for (int i = 0; i < fgrids.size(); ++i)
    {
      crse_S_fine_BA.set(i,BoxLib::coarsen(fgrids[i],fratio));
    }

  MultiFab crse_S_fine(crse_S_fine_BA,ncomp,0);
  MultiFab crse_fvolume(crse_S_fine_BA,1,0);

  crse_fvolume.copy(cvolume);

  for (MFIter mfi(S_fine); mfi.isValid(); ++mfi)
    {
      const int i = mfi.index();

      avgDown(S_fine[i],crse_S_fine[i],fvolume[i],crse_fvolume[i],
	      f_level,c_level,crse_S_fine_BA[i],scomp,ncomp,fratio);
    }

  S_crse.copy(crse_S_fine,0,scomp,ncomp);
}

//
// Average fine down to coarse in the ovlp intersection.
//

void
PorousMedia::avgDown (const FArrayBox& fine_fab,
		      const FArrayBox& crse_fab, 
		      const FArrayBox& fine_vol,
		      const FArrayBox& crse_vol,
		      int              f_level,
		      int              c_level,
		      const Box&       ovlp,
		      int              scomp,
		      int              ncomp,
		      const IntVect&   fratio)
{
  avgDown_doit(fine_fab,crse_fab,fine_vol,crse_vol,
	       f_level,c_level,ovlp,scomp,ncomp,fratio);
}



//
// Actually average the data down (this is static)
//

void
PorousMedia::avgDown_doit (const FArrayBox& fine_fab,
			   const FArrayBox& crse_fab, 
			   const FArrayBox& fine_vol,
			   const FArrayBox& crse_vol,
			   int              f_level,
			   int              c_level,
			   const Box&       ovlp,
			   int              scomp,
			   int              ncomp,
			   const IntVect&   fratio)
{
  //
  //  NOTE: We copy from component scomp of the fine fab into component 0 of the crse fab
  //        because the crse fab is a temporary which was made starting at comp 0, it is
  //        not the actual state data.
  //
  const int*  ovlo   = ovlp.loVect();
  const int*  ovhi   = ovlp.hiVect();
  const int*  flo    = fine_fab.loVect();
  const int*  fhi    = fine_fab.hiVect();
  const Real* f_dat  = fine_fab.dataPtr(scomp);
  const int*  fvlo   = fine_vol.loVect();
  const int*  fvhi   = fine_vol.hiVect();
  const Real* fv_dat = fine_vol.dataPtr();
  const int*  clo    = crse_fab.loVect();
  const int*  chi    = crse_fab.hiVect();
  const Real* c_dat  = crse_fab.dataPtr();
  const int*  cvlo   = crse_vol.loVect();
  const int*  cvhi   = crse_vol.hiVect();
  const Real* cv_dat = crse_vol.dataPtr();

  FORT_AVGDOWN(c_dat,ARLIM(clo),ARLIM(chi),&ncomp,
	       f_dat,ARLIM(flo),ARLIM(fhi),
	       cv_dat,ARLIM(cvlo),ARLIM(cvhi),
	       fv_dat,ARLIM(fvlo),ARLIM(fvhi),
	       ovlo,ovhi,fratio.getVect());
}

static
void
SyncMacAcrossPeriodicEdges (MultiFab&       u_mac_crse_in_dir,
                            const MultiFab& crse_src,
                            const Geometry& cgeom,
                            int             dir,
                            int             nc)
{
  if (cgeom.isPeriodic(dir))
    {
      const Box cdmn = BoxLib::surroundingNodes(cgeom.Domain(),dir);

      const int N = 2;
      const int L = cdmn.length(dir) - 1;

      Box sides[N] = {cdmn,cdmn};

      sides[0].shift(dir, +L); // The hi end.
      sides[1].shift(dir, -L); // The lo end.

      const IntVect ZeroVector(D_DECL(0,0,0));

      IntVect shifts[N] = {ZeroVector,ZeroVector};

      shifts[0][dir] = -L; // How to shift hi -> lo
      shifts[1][dir] = +L; // How to shift lo -> hi

      for (int which = 0; which < N; ++which)
        {
	  Array<int> pmap;

	  BoxList bl(cdmn.ixType());

	  std::vector< std::pair<int,Box> > isects;

	  isects = crse_src.boxArray().intersections(sides[which]);

	  for (int i = 0; i < isects.size(); i++)
            {
	      const Box bx = crse_src.boxArray()[isects[i].first] & cdmn;

	      if (bx.ok())
                {
		  bl.push_back(bx);
		  pmap.push_back(crse_src.DistributionMap()[isects[i].first]);
                }
            }

	  if (!bl.isEmpty())
            {
	      pmap.push_back(ParallelDescriptor::MyProc()); // The sentinel.
	      MultiFab mf;
	      mf.define(BoxArray(bl), nc, 0, DistributionMapping(pmap), Fab_allocate);
	      mf.copy(crse_src);
	      mf.shift(shifts[which]);
	      u_mac_crse_in_dir.copy(mf);
            }
        }
    }
}

//
// Average edged values down a level
//
void
PorousMedia::SyncEAvgDown (PArray<MultiFab> u_mac_crse,
			   PArray<MultiFab> u_mac_fine,
			   int c_lev)
{
  BL_PROFILE(BL_PROFILE_THIS_NAME() + "::SyncEAvgDown()");

  const Geometry& cgeom = parent->Geom(c_lev);
  IntVect         ratio = parent->refRatio(c_lev);
  int             nc    = u_mac_fine[0].nComp();
  
  for (int n = 0; n < u_mac_fine.size(); ++n)
    {
      //
      // crse_src & fine_src must have same parallel distribution.
      // We'll use the KnapSack distribution for the fine_src_ba.
      // Since fine_src_ba should contain more points, this'll lead
      // to a better distribution.
      //
      BoxArray fine_src_ba = u_mac_fine[n].boxArray();
      BoxArray crse_src_ba = BoxArray(fine_src_ba.size());

      for (int i=0; i<fine_src_ba.size();++i)
	{
	  crse_src_ba.set(i,Box(fine_src_ba[i]).coarsen(ratio));
	  fine_src_ba.set(i,Box(crse_src_ba[i]).refine(ratio));
	}

      std::vector<long> wgts(fine_src_ba.size());
    
      for (unsigned int i = 0; i < wgts.size(); i++)
	wgts[i] = fine_src_ba[i].numPts();
	
      DistributionMapping dm;
      dm.KnapSackProcessorMap(wgts,ParallelDescriptor::NProcs());

      MultiFab crse_src,  fine_src; 
    
      crse_src.define(crse_src_ba, nc, 0, dm, Fab_allocate);
      fine_src.define(fine_src_ba, nc, 0, dm, Fab_allocate);
    
      crse_src.setVal(1.e200);
      fine_src.setVal(1.e200);
	
      fine_src.copy(u_mac_fine[n]);
    
      for (MFIter mfi(crse_src); mfi.isValid(); ++mfi)
	{
	  const int  nComp = nc;
	  const Box& box   = crse_src[mfi].box();
	  const int* rat   = ratio.getVect();
	  FORT_EDGE_AVGDOWN(box.loVect(), box.hiVect(), &nComp, rat, &n,
			    crse_src[mfi].dataPtr(),
			    ARLIM(crse_src[mfi].loVect()),
			    ARLIM(crse_src[mfi].hiVect()),
			    fine_src[mfi].dataPtr(),
			    ARLIM(fine_src[mfi].loVect()),
			    ARLIM(fine_src[mfi].hiVect()));
	}

      fine_src.clear();
    
      u_mac_crse[n].copy(crse_src);

      SyncMacAcrossPeriodicEdges(u_mac_crse[n], crse_src, cgeom, n, nc);

    }
}

void
PorousMedia::SyncEAvgDown (MultiFab* u_mac_crse,
			   int c_lev,
			   MultiFab* u_mac_fine, 
			   int f_lev) 
{
  BL_PROFILE(BL_PROFILE_THIS_NAME() + "::SyncEAvgDown()");

  BL_ASSERT(f_lev > 0);

  const Geometry& cgeom      = parent->Geom(c_lev);
  const BoxArray& fgrids     = getLevel(f_lev).grids;
  IntVect    ratio           = IntVect::TheUnitVector();
  ratio                     *= parent->refRatio(c_lev);
  BoxArray f_bnd_ba = fgrids;
  BoxArray c_bnd_ba = BoxArray(f_bnd_ba.size());

  int nc = u_mac_fine[0].nComp();

  for (int i = 0; i < f_bnd_ba.size(); ++i)
    {
      c_bnd_ba.set(i,Box(f_bnd_ba[i]).coarsen(ratio));
      f_bnd_ba.set(i,Box(c_bnd_ba[i]).refine(ratio));
    }

  for (int n = 0; n < BL_SPACEDIM; ++n)
    {
      //
      // crse_src & fine_src must have same parallel distribution.
      // We'll use the KnapSack distribution for the fine_src_ba.
      // Since fine_src_ba should contain more points, this'll lead
      // to a better distribution.
      //
      BoxArray crse_src_ba(c_bnd_ba);
      BoxArray fine_src_ba(f_bnd_ba);

      crse_src_ba.surroundingNodes(n);
      fine_src_ba.surroundingNodes(n);

      std::vector<long> wgts(fine_src_ba.size());

      for (unsigned int i = 0; i < wgts.size(); i++)
	{
	  wgts[i] = fine_src_ba[i].numPts();
	}
      DistributionMapping dm;
      //
      // This call doesn't invoke the MinimizeCommCosts() stuff.
      // There's very little to gain with these types of coverings
      // of trying to use SFC or anything else.
      // This also guarantees that these DMs won't be put into the
      // cache, as it's not representative of that used for more
      // usual MultiFabs.
      //
      dm.KnapSackProcessorMap(wgts,ParallelDescriptor::NProcs());

      MultiFab crse_src,  fine_src; 

      crse_src.define(crse_src_ba, nc, 0, dm, Fab_allocate);
      fine_src.define(fine_src_ba, nc, 0, dm, Fab_allocate);
	    
      crse_src.setVal(1.e200);
      fine_src.setVal(1.e200);
	
      fine_src.copy(u_mac_fine[n]);
        
      for (MFIter mfi(crse_src); mfi.isValid(); ++mfi)
	{
	  const int  nComp = nc;
	  const Box& box   = crse_src[mfi].box();
	  const int* rat   = ratio.getVect();
	  FORT_EDGE_AVGDOWN(box.loVect(), box.hiVect(), &nComp, rat, &n,
			    crse_src[mfi].dataPtr(),
			    ARLIM(crse_src[mfi].loVect()),
			    ARLIM(crse_src[mfi].hiVect()),
			    fine_src[mfi].dataPtr(),
			    ARLIM(fine_src[mfi].loVect()),
			    ARLIM(fine_src[mfi].hiVect()));
	}
      fine_src.clear();

      u_mac_crse[n].copy(crse_src);

      SyncMacAcrossPeriodicEdges(u_mac_crse[n], crse_src, cgeom, n, nc);

    }
}

void
PorousMedia::SyncEAvgDown (MultiFab* u_mac_crse[],
			   int c_lev,
			   MultiFab* u_mac_fine[], 
			   int f_lev) 
{
  BL_PROFILE(BL_PROFILE_THIS_NAME() + "::SyncEAvgDown()");

  BL_ASSERT(f_lev > 0);

  const Geometry& cgeom      = parent->Geom(c_lev);
  const BoxArray& fgrids     = getLevel(f_lev).grids;
  IntVect    ratio           = IntVect::TheUnitVector();
  ratio                     *= parent->refRatio(c_lev);
  BoxArray f_bnd_ba = fgrids;
  BoxArray c_bnd_ba = BoxArray(f_bnd_ba.size());

  int nc = (*u_mac_fine[0]).nComp();

  for (int i = 0; i < f_bnd_ba.size(); ++i)
    {
      c_bnd_ba.set(i,Box(f_bnd_ba[i]).coarsen(ratio));
      f_bnd_ba.set(i,Box(c_bnd_ba[i]).refine(ratio));
    }

  for (int n = 0; n < BL_SPACEDIM; ++n)
    {
      //
      // crse_src & fine_src must have same parallel distribution.
      // We'll use the KnapSack distribution for the fine_src_ba.
      // Since fine_src_ba should contain more points, this'll lead
      // to a better distribution.
      //
      BoxArray crse_src_ba(c_bnd_ba);
      BoxArray fine_src_ba(f_bnd_ba);

      crse_src_ba.surroundingNodes(n);
      fine_src_ba.surroundingNodes(n);

      std::vector<long> wgts(fine_src_ba.size());

      for (unsigned int i = 0; i < wgts.size(); i++)
	{
	  wgts[i] = fine_src_ba[i].numPts();
	}
      DistributionMapping dm;
      //
      // This call doesn't invoke the MinimizeCommCosts() stuff.
      // There's very little to gain with these types of coverings
      // of trying to use SFC or anything else.
      // This also guarantees that these DMs won't be put into the
      // cache, as it's not representative of that used for more
      // usual MultiFabs.
      //
      dm.KnapSackProcessorMap(wgts,ParallelDescriptor::NProcs());

      MultiFab crse_src,  fine_src; 

      crse_src.define(crse_src_ba, nc, 0, dm, Fab_allocate);
      fine_src.define(fine_src_ba, nc, 0, dm, Fab_allocate);
	    
      crse_src.setVal(1.e200);
      fine_src.setVal(1.e200);
	
      fine_src.copy(*u_mac_fine[n]);
        
      for (MFIter mfi(crse_src); mfi.isValid(); ++mfi)
	{
	  const int  nComp = nc;
	  const Box& box   = crse_src[mfi].box();
	  const int* rat   = ratio.getVect();
	  FORT_EDGE_AVGDOWN(box.loVect(), box.hiVect(), &nComp, rat, &n,
			    crse_src[mfi].dataPtr(),
			    ARLIM(crse_src[mfi].loVect()),
			    ARLIM(crse_src[mfi].hiVect()),
			    fine_src[mfi].dataPtr(),
			    ARLIM(fine_src[mfi].loVect()),
			    ARLIM(fine_src[mfi].hiVect()));
	}

      fine_src.clear();

      u_mac_crse[n]->copy(crse_src);

      SyncMacAcrossPeriodicEdges(*u_mac_crse[n], crse_src, cgeom, n, nc);

    }
}

//
// The Mac Sync correction function
//
void
PorousMedia::mac_sync ()
{
  BL_PROFILE(BL_PROFILE_THIS_NAME() + "::mac_sync()");

  const int  numscal   = ncomps; 
  const Real prev_time = state[State_Type].prevTime();
  const Real curr_time = state[State_Type].curTime();
  const Real dt        = parent->dtLevel(level);
  const BCRec& p_bc    = desc_lst[Press_Type].getBC(0);
        
  //
  // Compute the u_mac for the correction.
  //
  MultiFab* p_corr = new MultiFab(grids,1,1);
  for (int i=0; i < BL_SPACEDIM; i++)
    u_corr[i].setVal(0.);
  create_lambda(curr_time); 
  mac_projector->mac_sync_solve(level,p_bc,lambda,p_corr,u_corr,fine_ratio);
  
  //
  // Assign rock_phi to alpha
  //
  MultiFab* alpha = new MultiFab(grids, 1, 1);
  MultiFab::Copy(*alpha,*rock_phi,0,0,1,alpha->nGrow());
  //
  // Update coarse grid state by adding correction from mac_sync solve
  // the correction is the advective tendency of the new velocities.
  //
  mac_projector->mac_sync_compute(level,u_macG_curr,u_corr,
				  Ssync,lambda,rock_phi,kappa,
				  lambda_cc,dlambda_cc,kr_coef,
				  kpedge,p_corr,
				  level > 0 ? &getAdvFluxReg(level) : 0,
				  advectionType, prev_time, dt,
				  ncomps,be_cn_theta);
  //
  // average onto cell center
  //
  umac_edge_to_cen(u_corr,Vcr_Type);

  //
  // The following used to be done in mac_sync_compute.  Ssync is
  //   the source for a rate of change to rock_phi*S over the time step, so
  //   Ssync*dt is the source to the actual sync amount.
  //
  MultiFab& S_new = get_new_data(State_Type);

  if (verbose > 1)
    {
      Real tmp = (*Ssync).norm2(0);
      if (ParallelDescriptor::IOProcessor())
	std::cout << "SSYNC NORM  AFTER = " << tmp << '\n';
      Ssync->mult(-dt,Ssync->nGrow());
    
      MultiFab::Copy(S_new,*Ssync,0,ncomps+ntracers+1,1,1);
    }
    
  //
  // Diffusion solve for Ssync
  //    
  bool any_diffusive = false;
  for (int kk  = 0; kk < ncomps; kk++)
    if (is_diffusive[kk])
      any_diffusive = true;
    
  if (any_diffusive)
    {
      MultiFab tmp(grids,1,1);
      MultiFab** fluxSC  = 0;
      diffusion->allocFluxBoxesLevel(fluxSC,0,1);
      
      tmp.setVal(0.);
      for (int i=0; i < BL_SPACEDIM; ++i){
	(*fluxSC[i]).setVal(0.);
      }

      //
      // Set up rho function for diffusive solve
      //
      MultiFab* rho = new MultiFab(grids,1,1);
      MultiFab::Copy(*rho,S_new,0,0,1,1);
      for (int kk = 1; kk<ncomps; kk++)
	{
	  if (solid.compare(pNames[pType[kk]]) != 0) 
	    MultiFab::Add(*rho,S_new,kk,0,1,1);
	}
      diffusion->set_rho(rho);
      delete rho;
      
      for (int kk = 0; kk<ncomps; kk++)
	{
	  if (is_diffusive[kk])
	    {
	      MultiFab** cmp_diffn=0;
	  
	      if (variable_scal_diff)
		{
		  Real diffTime = state[State_Type].curTime();
		  diffusion->allocFluxBoxesLevel(cmp_diffn, 0, 1);
		  getDiffusivity(cmp_diffn, diffTime,kk,0,1);
		}
	      diffusion->diffuse_Ssync(Ssync,kk,dt,be_cn_theta,
				       fluxSC,0,cmp_diffn,alpha);
	      if (variable_scal_diff)
		diffusion->removeFluxBoxesLevel(cmp_diffn);

	      if (level > 0)
		{
		  for (int d = 0; d < BL_SPACEDIM; d++)
		    {
		      Real mult = dt;
		      MultiFab& fluxSCd = *fluxSC[d];
		      for (MFIter fmfi(fluxSCd); fmfi.isValid(); ++fmfi)
			getViscFluxReg().FineAdd(fluxSCd[fmfi],d,
						 fmfi.index(),
						 0,kk,1,mult);
		    }
		}
	    }
	}
      diffusion->removeFluxBoxesLevel(fluxSC);
    }
    
  // 
  // Capillary-solve.  Since capillary function is nonlinear, we cannot
  // do a simple capillary-diffuse solve for Ssync.  A full nonlinear
  // parabolic solve is needed to determine the new solution at the end of 
  // coarse timestep.  
  //
  if  (have_capillary)
    {
      const int nGrow = 0;
      const int nComp = 1;
      MultiFab** fluxSC    = 0;
      MultiFab** fluxSCp1  = 0;
      diffusion->allocFluxBoxesLevel(fluxSC,  nGrow,nComp);
      diffusion->allocFluxBoxesLevel(fluxSCp1,nGrow,nComp);
      
      int nc = 0; 
      int nd = 1;
      MultiFab*  delta_rhs = 0;
      MultiFab** cmp_pcn   = 0;
      MultiFab** cmp_pcnp1 = 0;
      MultiFab** cmp_pcnp1_dp = 0;
      MultiFab*  S_nwt = 0;
      MultiFab&  S_new = get_new_data(State_Type);

      MultiFab* sat_res_mf = new MultiFab(grids,1,1);
      sat_res_mf->setVal(1.);
      for (MFIter mfi(*sat_res_mf); mfi.isValid();++mfi)
	{
	  const Box& box = (*sat_res_mf)[mfi].box();
	  (*sat_res_mf)[mfi].minus((*cpl_coef)[mfi],box,3,0,1);
	}
      sat_res_mf->mult(density[nc]);
      diffusion->set_rho(sat_res_mf); 

      MultiFab S_tmp(grids,ncomps,1);
      MultiFab::Copy(S_tmp,S_new,0,0,ncomps,1);

      S_nwt = new MultiFab(grids,1,1);
      MultiFab::Copy(*S_nwt,S_new,nc,0,nComp,1);
      
      delta_rhs = new MultiFab(grids,1,1);
      MultiFab::Copy(*delta_rhs,*Ssync,nc,0,nComp,1);

      //
      // Newton iteration
      //

      // initialization
      Real pcTime = state[State_Type].prevTime();
      diffusion->allocFluxBoxesLevel(cmp_pcn,0,1);
      calcCapillary(pcTime);
      calcDiffusivity_CPL(cmp_pcn,lambda_cc); 
 
      pcTime = state[State_Type].curTime();
      FillStateBndry (pcTime,State_Type,0,ncomps);
      diffusion->allocFluxBoxesLevel(cmp_pcnp1,0,1);
      diffusion->allocFluxBoxesLevel(cmp_pcnp1_dp,0,1);
      calcCapillary(pcTime);
      calcLambda(pcTime);
      calcDiffusivity_CPL(cmp_pcnp1,lambdap1_cc);
      calcDiffusivity_CPL_dp(cmp_pcnp1_dp,lambdap1_cc,pcTime,1);
      
      int  max_itr_nwt = 20;
#if (BL_SPACEDIM == 3)
      Real max_err_nwt = 1e-10;
#else
      Real max_err_nwt = 1e-10;
#endif
 
      int  itr_nwt = 0;
      Real err_nwt = 1e10;
      diffusion->diffuse_init_CPL(dt,nc,be_cn_theta,
				  fluxSC,0,delta_rhs,
				  alpha,cmp_pcn,pcn_cc,S_nwt);
      while ((itr_nwt < max_itr_nwt) && (err_nwt > max_err_nwt)) 
	{
	  diffusion->diffuse_iter_CPL(dt,nc,ncomps,be_cn_theta,
				      0,alpha,cmp_pcnp1,cmp_pcnp1_dp,
				      pcnp1_cc,S_nwt,&err_nwt);

	  if (verbose > 1 && ParallelDescriptor::IOProcessor())
	    std::cout << "Newton iteration " << itr_nwt 
		      << " : Error = "       << err_nwt << "\n"; 

	  scalar_adjust_constraint(0,ncomps-1);
	  FillStateBndry (pcTime,State_Type,0,ncomps);
	  calcCapillary(pcTime);
	  calcLambda(pcTime);
	  calcDiffusivity_CPL(cmp_pcnp1,lambdap1_cc);
	  calcDiffusivity_CPL_dp(cmp_pcnp1_dp,lambdap1_cc,pcTime,1);
	  itr_nwt += 1;	  

	  if (verbose > 1) 
	    check_minmax();
	}

      diffusion->compute_flux(nc,dt,be_cn_theta,fluxSCp1,pcnp1_cc,cmp_pcnp1);

      if (verbose > 1 && ParallelDescriptor::IOProcessor())
	{
	  if (itr_nwt < max_itr_nwt)
	    std::cout << "Newton converged at iteration " << itr_nwt
		      << " with error " << err_nwt << '\n';
	  else
	    std::cout << "Newton failed to converged: termination error is "
		      <<  err_nwt << '\n'; 
	}
      
      if (level > 0)
	{
	  for (int d = 0; d < BL_SPACEDIM; d++)
	    {
	      Real mult = -dt;
	      MultiFab& fluxSCd = *fluxSCp1[d];
	      for (MFIter fmfi(fluxSCd); fmfi.isValid(); ++fmfi)
		getViscFluxReg().FineAdd(fluxSCd[fmfi],d,
					 fmfi.index(),
					 0,nc,1,mult);
	  
	      fluxSCd.mult(-density[nd]/density[nc]);
	      for (MFIter fmfi(fluxSCd); fmfi.isValid(); ++fmfi)
		getViscFluxReg().FineAdd(fluxSCd[fmfi],d,
					 fmfi.index(),
					 0,nd,1,mult);
	    }
	}
      
      // Determine the corrector after capillary-solve
      for (MFIter mfi(*S_nwt); mfi.isValid();++mfi)
	{
	  const Box& box = mfi.validbox();
	  (*Ssync)[mfi].copy(S_new[mfi],box,0,box,0,ncomps);
	  (*Ssync)[mfi].minus(S_tmp[mfi],box,0,0,ncomps);
	}
	
      
  
      delete delta_rhs;
      delete S_nwt;
      delete sat_res_mf;
      
      diffusion->removeFluxBoxesLevel(fluxSC);
      diffusion->removeFluxBoxesLevel(fluxSCp1);
      diffusion->removeFluxBoxesLevel(cmp_pcn);
      diffusion->removeFluxBoxesLevel(cmp_pcnp1);
      diffusion->removeFluxBoxesLevel(cmp_pcnp1_dp);
    }
    
  delete p_corr;
  delete alpha;

  //
  // Add the sync correction to the state.
  //
  if (have_capillary == 0 && !any_diffusive)
    {
      for (MFIter mfi(*Ssync); mfi.isValid(); ++mfi) 
	{
	  for (int nc = 0; nc < ncomps; nc++)
	    (*Ssync)[mfi].divide((*rock_phi)[mfi],0,nc,1);
	}
    }
    
  if (have_capillary == 0)
    {
      for (MFIter mfi(S_new); mfi.isValid(); ++mfi)
	S_new[mfi].plus((*Ssync)[mfi],mfi.validbox(),
			0,0,numscal);
    }
    
  if (idx_dominant > -1)
    scalar_adjust_constraint(0,ncomps-1);
      
  //
  // Get boundary conditions.
  //
  Array<int*>         sync_bc(grids.size());
  Array< Array<int> > sync_bc_array(grids.size());
      
  for (int i = 0; i < grids.size(); i++)
    {
      sync_bc_array[i] = getBCArray(State_Type,i,0,numscal);
      sync_bc[i]       = sync_bc_array[i].dataPtr();
    }

  //
  // Interpolate the sync correction to the finer levels.
  //
  IntVect    ratio = IntVect::TheUnitVector();
  const Real mult  = 1.0;
  for (int lev = level+1; lev <= parent->finestLevel(); lev++)
    {
      ratio                     *= parent->refRatio(lev-1);
      PorousMedia&     fine_lev  = getLevel(lev);
      const BoxArray& fine_grids = fine_lev.boxArray();
      MultiFab sync_incr(fine_grids,numscal,0);
      sync_incr.setVal(0.0);
      
      SyncInterp(*Ssync,level,sync_incr,lev,ratio,0,0,
		 numscal,1,mult,sync_bc.dataPtr());
      
      MultiFab& S_new = fine_lev.get_new_data(State_Type);
      for (MFIter mfi(S_new); mfi.isValid(); ++mfi)
	S_new[mfi].plus(sync_incr[mfi],fine_grids[mfi.index()],
			0,0,numscal);
    }
}

//
// The Mac Sync correction function
//
#ifdef MG_USE_FBOXLIB
void
PorousMedia::richard_sync ()
{
  BL_PROFILE(BL_PROFILE_THIS_NAME() + "::richard_sync()");

  BL_ASSERT(have_capillary);

  const Real dt = parent->dtLevel(level);
       
  //
  //   Ssync is the source for a rate of change to rock_phi*S over the time step, so
  //   Ssync*dt*density[0] is the source to the actual sync amount.
  //

  if (verbose > 1)
    {
      Real tmp = (*Ssync).norm2(0);
      if (ParallelDescriptor::IOProcessor())
	std::cout << "SSYNC NORM  AFTER = " << tmp << '\n';
    }

  // 
  // Capillary-solve.  Since capillary function is nonlinear, we cannot
  // do a simple capillary-diffuse solve for Ssync.  A full nonlinear
  // parabolic solve is needed to determine the new solution at the end of 
  // coarse timestep.  
  //

  // Build single component edge-centered array of MultiFabs for fluxes
  MultiFab** fluxSC;
  const int nGrow = 0;
  const int nComp = 1;
  diffusion->allocFluxBoxesLevel(fluxSC,nGrow,nComp);
  
  int nc = 0; 
  MultiFab** cmp_pcp1    = 0;
  MultiFab** cmp_pcp1_dp = 0;
  MultiFab sat_res_mf(grids,1,1);
  sat_res_mf.setVal(1.);
  for (MFIter mfi(sat_res_mf); mfi.isValid();++mfi)
    {
      const Box& box = sat_res_mf[mfi].box();
      sat_res_mf[mfi].minus((*cpl_coef)[mfi],box,3,0,1);
    }
  diffusion->set_rho(&sat_res_mf); 

  bool do_n = true;
  bool sync_n = true;

  MultiFab& S_new  = get_new_data(State_Type);
  MultiFab& S_old  = get_old_data(State_Type);
  MultiFab& P_new  = get_new_data(Press_Type);
  MultiFab* alpha  = new MultiFab(grids,1,1);
  MultiFab* dalpha = 0;
  MultiFab Tmp(grids,1,1);
  
  if (sync_n)
    MultiFab::Copy(Tmp,S_new,0,0,1,1);
  else
    MultiFab::Copy(Tmp,P_new,0,0,1,1);
  MultiFab::Copy(*alpha,*rock_phi,0,0,1,alpha->nGrow());
  
  if (!do_n) dalpha = new MultiFab(grids,1,1);
      
  // Compute first res_fix = -\phi * n^k + dt*\nabla v_inflow.  
  // Its value does not change.
  MultiFab res_fix(grids,1,0);
  MultiFab::Copy(res_fix,S_old,nc,0,1,0);
  for (MFIter mfi(res_fix); mfi.isValid(); ++mfi)
    res_fix[mfi].mult((*alpha)[mfi],mfi.validbox(),0,0,1);
  res_fix.mult(-1.0);
  Ssync->mult(-dt*density[0]);
  MultiFab::Add(res_fix,*Ssync,nc,0,1,0);
  calc_richard_velbc(res_fix,dt*density[0]);
  // Newton method.
  // initialization
  bool do_upwind = true;
  int  max_itr_nwt = 20;
  Real max_err_nwt = 1e-12;
  int  itr_nwt = 0;
  Real err_nwt = 1e10;
  Real pcTime = state[State_Type].curTime();
  FillStateBndry(pcTime,State_Type,0,ncomps);
  diffusion->allocFluxBoxesLevel(cmp_pcp1,0,1);
  diffusion->allocFluxBoxesLevel(cmp_pcp1_dp,0,3);
  
  calcCapillary(pcTime);
  calcLambda(pcTime);
  calc_richard_coef(cmp_pcp1,lambdap1_cc,u_mac_curr,0,do_upwind);
  calc_richard_jac (cmp_pcp1_dp,lambdap1_cc,u_mac_curr,pcTime,0,do_upwind,do_n);

  
  if (!do_n) calc_richard_alpha(dalpha,pcTime);

  while ((itr_nwt < max_itr_nwt) && (err_nwt > max_err_nwt)) 
    {
      if (do_n)
	diffusion->richard_iter(dt,nc,gravity,density,res_fix,
				alpha,cmp_pcp1,cmp_pcp1_dp,
				pcnp1_cc,u_mac_curr,do_upwind,&err_nwt);      
      else
	diffusion->richard_iter_p(dt,nc,gravity,density,res_fix,
				  alpha,dalpha,cmp_pcp1,cmp_pcp1_dp,
				  pcnp1_cc,u_mac_curr,do_upwind,&err_nwt);  

      if (verbose > 1 && ParallelDescriptor::IOProcessor())
	std::cout << "Newton iteration " << itr_nwt 
		  << " : Error = "       << err_nwt << "\n"; 
      if (model != model_list["richard"])
	scalar_adjust_constraint(0,ncomps-1);

      FillStateBndry(pcTime,State_Type,0,ncomps);
      calcCapillary(pcTime);
      calcLambda(pcTime);
      MultiFab::Copy(P_new,*pcnp1_cc,0,0,1,1);
      P_new.mult(-1.0,1);
      compute_vel_phase(u_mac_curr,0,pcTime);
      calc_richard_coef(cmp_pcp1,lambdap1_cc,u_mac_curr,0,do_upwind);
      calc_richard_jac (cmp_pcp1_dp,lambdap1_cc,u_mac_curr,pcTime,0,do_upwind,do_n);
      if (!do_n) calc_richard_alpha(dalpha,pcTime);
      itr_nwt += 1;

      if (verbose > 1)  check_minmax();
    }
  diffusion->richard_flux(nc,-1.0,gravity,density,fluxSC,pcnp1_cc,cmp_pcp1);

  if (verbose > 1 && ParallelDescriptor::IOProcessor())
    {
      if (itr_nwt < max_itr_nwt)
	std::cout << "Newton converged at iteration " << itr_nwt
		  << " with error " << err_nwt << '\n';
      else
	std::cout << "Newton failed to converged: termination error is "
		  <<  err_nwt << '\n'; 
    }
  
  if (level > 0) 
    {
      for (int d = 0; d < BL_SPACEDIM; d++) 
	{
	  for (MFIter fmfi(*fluxSC[d]); fmfi.isValid(); ++fmfi)
	    getViscFluxReg().FineAdd((*fluxSC[d])[fmfi],d,fmfi.index(),0,nc,nComp,-dt);
	}
    }
  
  // Determine the corrector after capillary-solve
  for (MFIter mfi(*Ssync); mfi.isValid();++mfi)
    {
      const Box& box = mfi.validbox();
      if (sync_n)
	{
	  (*Ssync)[mfi].copy(S_new[mfi],box,0,box,0,ncomps);
	  (*Ssync)[mfi].minus(Tmp[mfi],box,0,0,ncomps);
	}
      else
	{
	  (*Ssync)[mfi].copy(P_new[mfi],box,0,box,0,1);
	  (*Ssync)[mfi].minus(Tmp[mfi],box,0,0,1);
	}
    }

  MultiFab::Copy(S_new,*Ssync,0,ncomps+ntracers,1,0);

  delete alpha;
  if (dalpha) delete dalpha;
  diffusion->removeFluxBoxesLevel(cmp_pcp1);
  diffusion->removeFluxBoxesLevel(cmp_pcp1_dp);
  diffusion->removeFluxBoxesLevel(fluxSC);
  
  //
  // Get boundary conditions.
  //
  Array<int*>         sync_bc(grids.size());
  Array< Array<int> > sync_bc_array(grids.size());
      
  for (int i = 0; i < grids.size(); i++)
    {
      sync_bc_array[i] = getBCArray(Press_Type,i,0,1);
      sync_bc[i]       = sync_bc_array[i].dataPtr();
    }

  //
  // Interpolate the sync correction to the finer levels.
  //
  IntVect    ratio = IntVect::TheUnitVector();
  const Real mult  = 1.0;
  for (int lev = level+1; lev <= parent->finestLevel(); lev++)
    {
      ratio                     *= parent->refRatio(lev-1);
      PorousMedia&     fine_lev  = getLevel(lev);
      const BoxArray& fine_grids = fine_lev.boxArray();
      MultiFab sync_incr(fine_grids,1,0);
      sync_incr.setVal(0.0);
      
      SyncInterp(*Ssync,level,sync_incr,lev,ratio,0,0,
		 1,1,mult,sync_bc.dataPtr());

      MultiFab& S_new = fine_lev.get_new_data(Press_Type);
      if (sync_n)
	{
	  for (MFIter mfi(S_new); mfi.isValid(); ++mfi)
	    S_new[mfi].plus(sync_incr[mfi],fine_grids[mfi.index()],
			      0,0,1);
	}
      else
	{	    
	  MultiFab& P_new = fine_lev.get_new_data(Press_Type);
	  for (MFIter mfi(P_new); mfi.isValid(); ++mfi)
	    P_new[mfi].plus(sync_incr[mfi],fine_grids[mfi.index()],
			    0,0,1);
	  MultiFab P_tmp(fine_grids,1,0);
	  MultiFab::Copy(P_tmp,P_new,0,0,1,0);
	  P_tmp.mult(-1.0);
	  fine_lev.calcInvCapillary(sync_incr,P_tmp);
	  MultiFab::Copy(S_new,sync_incr,0,0,1,0);
	}
    }
}
#endif

//
// The reflux function
//
void
PorousMedia::reflux ()
{
  if (level == parent->finestLevel())
    return;

  BL_PROFILE(BL_PROFILE_THIS_NAME() + "::reflux()");

  BL_ASSERT(do_reflux);
  //
  // First do refluxing step.
  //
  FluxRegister& fr_adv  = getAdvFluxReg(level+1);
  FluxRegister& fr_visc = getViscFluxReg(level+1);
  Real          dt_crse = parent->dtLevel(level);
  Real          scale   = 1.0/dt_crse;

  fr_visc.Reflux(*Ssync,volume,scale,0,0,NUM_SCALARS,geom);
  fr_adv.Reflux (*Ssync,volume,scale,0,0,NUM_SCALARS,geom);
  //
  // This is necessary in order to zero out the contribution to any
  // coarse grid cells which underlie fine grid cells.
  //
  BoxArray baf = getLevel(level+1).boxArray();

  baf.coarsen(fine_ratio);

  for (MFIter mfi(*Ssync); mfi.isValid(); ++mfi)
  {
      BL_ASSERT(grids[mfi.index()] == mfi.validbox());

      std::vector< std::pair<int,Box> > isects = baf.intersections(mfi.validbox());

      for (int i = 0, N = isects.size(); i < N; i++)
      {
          (*Ssync)[mfi.index()].setVal(0,isects[i].second,0,NUM_SCALARS);
      }
  }
}

//
// Average fine information from the complete set of state types to coarse.
//

void
PorousMedia::avgDown ()
{
  if (level == parent->finestLevel())
    return;

  PorousMedia&   fine_lev = getLevel(level+1);
  const BoxArray& fgrids  = fine_lev.grids;
  MultiFab&       fvolume = fine_lev.volume;
  //
  // Average down the state at the new time.
  //
  MultiFab& S_crse = get_new_data(State_Type);
  MultiFab& S_fine = fine_lev.get_new_data(State_Type);
  avgDown(grids,fgrids,S_crse,S_fine,volume,fvolume,level,level+1,0,S_crse.nComp(),fine_ratio);

  //
  // Average down the pressure at the new time.
  //
  MultiFab& P_crse = get_new_data(Press_Type);
  MultiFab& P_fine = fine_lev.get_new_data(Press_Type);
  avgDown(grids,fgrids,P_crse,P_fine,volume,fvolume,level,level+1,0,1,fine_ratio);

  //
  // Average down the cell-centered velocity at the new time.
  //
  //MultiFab& U_crse = get_new_data(Vel_Type);
  //MultiFab& U_fine = fine_lev.get_new_data(Vel_Type);
  //avgDown(grids,fgrids,U_crse,U_fine,volume,fvolume,level,level+1,0,BL_SPACEDIM,fine_ratio);

  if (do_reflux && u_macG_curr != 0)
    SyncEAvgDown(u_macG_curr,level,fine_lev.u_macG_curr,level+1);

  //
  // Average down the cell-centered velocity at the new time.
  //
#ifdef AMANZI
  if (do_chem > -1)
    {
      MultiFab& FC_crse = get_new_data(FuncCount_Type);
      MultiFab& FC_fine = fine_lev.get_new_data(FuncCount_Type);
      avgDown(grids,fgrids,FC_crse,FC_fine,volume,fvolume,
	      level,level+1,0,1,fine_ratio);
    }
#endif
}

//
// ACCESS FUNCTIONS FOLLOW
//

//
// Virtual access function for getting the advective flux out of the
// advection routines for diagnostics and refluxing.
//

void
PorousMedia::pullFluxes (int        i,
                         int        start_ind,
                         int        ncomp,
                         FArrayBox& xflux,
                         FArrayBox& yflux,
#if (BL_SPACEDIM == 3)
                         FArrayBox& zflux,
#endif
                         Real       dt)
{
  //
  // Add fluxes into the refluxing counters.
  //
  if (do_reflux)
    {
      if (level < parent->finestLevel())
        {
	  FluxRegister& fr = getAdvFluxReg(level+1);
	  fr.CrseInit(xflux,xflux.box(),0,0,start_ind,ncomp,-dt);
	  fr.CrseInit(yflux,yflux.box(),1,0,start_ind,ncomp,-dt);
#if (BL_SPACEDIM == 3)                              
	  fr.CrseInit(zflux,zflux.box(),2,0,start_ind,ncomp,-dt);
#endif
        }
      if (level > 0)
        {
	  advflux_reg->FineAdd(xflux,0,i,0,start_ind,ncomp,dt);
	  advflux_reg->FineAdd(yflux,1,i,0,start_ind,ncomp,dt);
#if (BL_SPACEDIM == 3)                                
	  advflux_reg->FineAdd(zflux,2,i,0,start_ind,ncomp,dt);
#endif
        }
    }
}

//
// Virtual access function for getting the forcing terms for the 
// pressure and scalars.  
//
void
PorousMedia::getForce (FArrayBox& force,
		       int        gridno,
		       int        ngrow,
		       int        scomp,
		       int        ncomp,
		       const Real time,
		       int        do_rho_scale)
{      

  force.resize(BoxLib::grow(grids[gridno],ngrow),ncomp);

  force.setVal(0);
  if (do_source_term)
    { 
      const Real* dx       = geom.CellSize();

      for (int i = 0; i< source_array.size(); i++)
	if (!source_array[i].var_type.compare("comp"))
	    source_array[i].setVal(force, region_array, dx); 
      
      if (do_rho_scale)
	{
	  for (int i = 0; i< ncomps; i++)
	    force.mult(1.0/density[i],i);
	}
    }
}

//
// Virtual access function for getting the forcing terms for the 
// tracers.  
//
void
PorousMedia::getForce_Tracer (FArrayBox& force,
			      int        gridno,
			      int        ngrow,
			      int        scomp,
			      int        ncomp,
			      const Real time)
{      
  force.resize(BoxLib::grow(grids[gridno],ngrow),ncomp);

  force.setVal(0.);
  if (do_source_term)
    {   
      const Real* dx = geom.CellSize();
      for (int i = 0; i< source_array.size(); i++)
	if (!source_array[i].var_type.compare("tracer"))
	  source_array[i].setVal(force, region_array, dx); 
    }
}

//
// Fills ghost cells of states.
//
void
PorousMedia::FillStateBndry (Real time,
                             int  state_idx,
                             int  src_comp, 
                             int  ncomp) 
{
  MultiFab& S = get_data(state_idx,time);

  if (S.nGrow() == 0)
    return;

  for (FillPatchIterator fpi(*this,S,S.nGrow(),time,state_idx,src_comp,ncomp);
       fpi.isValid();
       ++fpi)
    {
      //
      // Fill all ghost cells interior & exterior to valid region.
      //
      BoxList boxes = BoxLib::boxDiff(fpi().box(),grids[fpi.index()]);
      for (BoxList::iterator bli = boxes.begin(); bli != boxes.end(); ++bli)
        {
	  S[fpi.index()].copy(fpi(),*bli,0,*bli,src_comp,ncomp);
        }
    }
  dirichletStateBC(time);
}


void 
PorousMedia::getViscTerms (MultiFab& visc_terms,
                           int       src_comp, 
                           int       ncomp,
                           Real      time)
{
  // 
  // Initialize all viscous terms to zero
  //
  const int nGrow = visc_terms.nGrow();
  visc_terms.setVal(0,0,ncomp,nGrow);

  //
  // Get Scalar Diffusive Terms
  //
  const int first_scal = src_comp;
  const int num_scal   = ncomp;

  if (num_scal > 0)

    {
      for (int icomp = first_scal; icomp < first_scal+num_scal; icomp++)
	{
	  if (is_diffusive[icomp])
	    {
	      MultiFab** cmp_diffn = 0;
	  
	      if (variable_scal_diff)
		{
		  diffusion->allocFluxBoxesLevel(cmp_diffn, 0, 1);
		  getDiffusivity(cmp_diffn, time, icomp, 0, 1);
		}
	      diffusion->getViscTerms(visc_terms,src_comp,icomp,
				      time,0,cmp_diffn);
	      if (variable_scal_diff)
		{
		  diffusion->removeFluxBoxesLevel(cmp_diffn);
		}	  
	    }
	}

      //
      // Get Capillary Diffusive Terms at time n
      //
      if (have_capillary)
	{
	  int nc = 0;
	  MultiFab** cmp_pcn = 0;
	  diffusion->allocFluxBoxesLevel(cmp_pcn,0,1);

	  calcCapillary(time);
	  calcDiffusivity_CPL(cmp_pcn,lambda_cc);

	  // multiply by kedge
	  for (int dir = 0; dir < BL_SPACEDIM; dir++)
	    {
	      for (MFIter mfi(*cmp_pcn[dir]); mfi.isValid(); ++mfi)
		(*cmp_pcn[dir])[mfi].mult(kpedge[dir][mfi],0,0,1);
	      (*cmp_pcn[dir]).FillBoundary();
	    }

	  diffusion->getCplViscTerms(visc_terms,nc,time,density.dataPtr(),0,
				     cmp_pcn,pcn_cc);
	  diffusion->removeFluxBoxesLevel(cmp_pcn);
	}	
    }

  //
  // Ensure consistent grow cells
  //    
  if (nGrow > 0)
    {
      for (MFIter mfi(visc_terms); mfi.isValid(); ++mfi)
        {
	  FArrayBox& vt  = visc_terms[mfi];
	  const Box& box = mfi.validbox();
	  FORT_VISCEXTRAP(vt.dataPtr(),ARLIM(vt.loVect()),ARLIM(vt.hiVect()),
			  box.loVect(),box.hiVect(),&ncomp);
        }
      visc_terms.FillBoundary(0,ncomp);
      //
      // Note: this is a special periodic fill in that we want to
      // preserve the extrapolated grow values when periodic --
      // usually we preserve only valid data.  The scheme relies on
      // the fact that there is good data in the "non-periodic" grow cells.
      // ("good" data produced via VISCEXTRAP above)
      //
      geom.FillPeriodicBoundary(visc_terms,0,ncomp,true);
    }
}

//
// Functions for calculating the variable viscosity and diffusivity.
// These default to setting the variable viscosity and diffusivity arrays
// to the values in visc_coef and diff_coef.  These functions would
// need to be replaced in any class derived from PorousMedia that
// wants variable coefficients.
//

void 
PorousMedia::calcDiffusivity (const Real time, 
			      const int  src_comp, 
			      const int  ncomp)
{
  //
  // NOTE:  The component numbers passed into PorousMedia::calcDiffusivity
  //        correspond to the components in the state.  
  //

  BL_PROFILE(BL_PROFILE_THIS_NAME() + "::calcDiffusivity()");

  MultiFab& S = get_data(State_Type,time);
  //
  // Select time level to work with (N or N+1)
  //
  const TimeLevel whichTime = which_time(State_Type,time);
    
  BL_ASSERT(whichTime == AmrOldTime || whichTime == AmrNewTime);
    
  // diffn_cc and diffnp1_cc are in PorousMedia class.
  MultiFab* diff_cc         = (whichTime == AmrOldTime) ? diffn_cc : diffnp1_cc;
  const int nGrow           = 1;

  Array<Real> const_diff_coef(ncomp);
  for (int i=0;i<ncomp;i++)
    const_diff_coef[i] = visc_coef[i];
    
  //
  // Calculate diffusivity
  //
  for (FillPatchIterator fpi(*this,S,nGrow,time,State_Type,0,ncomp);
       fpi.isValid();
       ++fpi)
    {

      dirichletStateBC(fpi(),nGrow,time);

      const int idx   = fpi.index();
      const Box box   = BoxLib::grow(grids[idx],nGrow);
      const int vflag = -1;

      FArrayBox&  Sfab  = fpi();
      const Real* ndat  = Sfab.dataPtr(); 
      const int*  n_lo  = Sfab.loVect();
      const int*  n_hi  = Sfab.hiVect();

      const Real* ddat  = (*diff_cc)[fpi].dataPtr(); 
      const int*  d_lo  = (*diff_cc)[fpi].loVect();
      const int*  d_hi  = (*diff_cc)[fpi].hiVect();

      const Real* pdat  = (*rock_phi)[fpi].dataPtr();
      const int*  p_lo  = (*rock_phi)[fpi].loVect();
      const int*  p_hi  = (*rock_phi)[fpi].hiVect();

      BL_ASSERT(box == fpi().box());
      FORT_SPECTEMPVISC(box.loVect(),box.hiVect(),
			ndat, ARLIM(n_lo), ARLIM(n_hi),
			ddat, ARLIM(d_lo), ARLIM(d_hi),
			pdat, ARLIM(p_lo),ARLIM(p_hi),
			const_diff_coef.dataPtr(), &ncomp, &vflag);
    }
}

void 
PorousMedia::getDiffusivity (MultiFab*  diffusivity[BL_SPACEDIM],
			     const Real time,
			     const int  state_comp,
			     const int  dst_comp,
			     const int  ncomp)
{
  BL_PROFILE(BL_PROFILE_THIS_NAME() + "::getDiffusivity()");

  //
  // Pick correct diffusivity component
  //
  int diff_comp = state_comp;

  //
  // Select time level to work with (N or N+1)
  //   
  const TimeLevel whichTime = which_time(State_Type,time);
    
  BL_ASSERT(whichTime == AmrOldTime || whichTime == AmrNewTime);

  MultiFab* diff_cc  = (whichTime == AmrOldTime) ? diffn_cc : diffnp1_cc;

  //
  // Fill edge-centered diffusivities based on diffn_cc or diffnp1_cc
  //
  for (int dir = 0; dir < BL_SPACEDIM; dir++)
    {
      for (MFIter ecMfi(*diffusivity[dir]); ecMfi.isValid(); ++ecMfi)
        {
	  center_to_edge_plain((*diff_cc)[ecMfi],(*diffusivity[dir])[ecMfi],
			       diff_comp,dst_comp,ncomp);
        }
    }
}

void 
PorousMedia::calcDiffusivity_CPL (MultiFab*  diffusivity[BL_SPACEDIM],
				  const Real time)
{

  BL_PROFILE(BL_PROFILE_THIS_NAME() + "::calcDiffusivity_CPL()");
  const TimeLevel whichTime = which_time(State_Type,time);
  BL_ASSERT(whichTime == AmrOldTime || whichTime == AmrNewTime);    
  MultiFab* lcc = (whichTime == AmrOldTime) ? lambda_cc : lambdap1_cc;
  calcDiffusivity_CPL(diffusivity,lcc);
}

void 
PorousMedia::calcDiffusivity_CPL (MultiFab*        diffusivity[BL_SPACEDIM],
				  const MultiFab*  lbd_cc)
{
  BL_PROFILE(BL_PROFILE_THIS_NAME() + "::calcDiffusivity_CPL()");   

  const int*  domlo    = geom.Domain().loVect();
  const int*  domhi    = geom.Domain().hiVect();
  const int   ncomp    = (*diffusivity[0]).nComp();
  for (MFIter mfi(*lbd_cc); mfi.isValid(); ++mfi)
    {
      const int idx   = mfi.index();
      const int* lo   = mfi.validbox().loVect();
      const int* hi   = mfi.validbox().hiVect();

      const int* lbd_lo  = (*lbd_cc)[idx].loVect();
      const int* lbd_hi  = (*lbd_cc)[idx].hiVect();
      const Real* lbddat = (*lbd_cc)[idx].dataPtr();

      const int* dfx_lo  = (*diffusivity[0])[idx].loVect();
      const int* dfx_hi  = (*diffusivity[0])[idx].hiVect();
      const Real* dfxdat = (*diffusivity[0])[idx].dataPtr();

      const int* dfy_lo  = (*diffusivity[1])[idx].loVect();
      const int* dfy_hi  = (*diffusivity[1])[idx].hiVect();
      const Real* dfydat = (*diffusivity[1])[idx].dataPtr();

#if(BL_SPACEDIM==3)
      const int* dfz_lo  = (*diffusivity[2])[idx].loVect();
      const int* dfz_hi  = (*diffusivity[2])[idx].hiVect();
      const Real* dfzdat = (*diffusivity[2])[idx].dataPtr();
#endif
      Array<int> bc;
      bc = getBCArray(State_Type,idx,0,1);
      FORT_GETDIFFUSE_CPL(lbddat, ARLIM(lbd_lo), ARLIM(lbd_hi),
			  dfxdat, ARLIM(dfx_lo), ARLIM(dfx_hi),
			  dfydat, ARLIM(dfy_lo), ARLIM(dfy_hi),
#if(BL_SPACEDIM==3)
			  dfzdat, ARLIM(dfz_lo), ARLIM(dfz_hi),
#endif
			  lo,hi,domlo,domhi,bc.dataPtr(),&ncomp);
    }
  // multiply by kedge
  for (int dir = 0; dir < BL_SPACEDIM; dir++)
    {
      for (MFIter ecMfi(*diffusivity[dir]); ecMfi.isValid(); ++ecMfi)
        {
	  (*diffusivity[dir])[ecMfi].mult(kpedge[dir][ecMfi],0,0,1);
        }
      (*diffusivity[dir]).FillBoundary();
    }  
}

void 
PorousMedia::calcDiffusivity_CPL_dp (MultiFab* diffusivity[BL_SPACEDIM],
				     const MultiFab* lbd_cc,
				     const Real time,
				     const int ncomp)
{
  BL_PROFILE(BL_PROFILE_THIS_NAME() + "::calcDiffusivity_CPL_dp()");

  MultiFab& S = get_data(State_Type,time);
  const int nGrow = 1;    

  const int*  domlo    = geom.Domain().loVect();
  const int*  domhi    = geom.Domain().hiVect();
  const int n_cpl_coef = cpl_coef->nComp(); 

  // Calculate diffusivity with the dp/ds term.
  for (FillPatchIterator fpi(*this,S,nGrow,time,State_Type,0,ncomps);
       fpi.isValid();
       ++fpi)
    {
      dirichletStateBC(fpi(),nGrow,time);

      const int idx   = fpi.index();
      const Box box   = BoxLib::grow(grids[idx],nGrow);

      BL_ASSERT(box == fpi().box());

      FArrayBox Htmp(box,1);
      Htmp.setVal(0.);
      const Real* hdat = Htmp.dataPtr();

      const Real* ndat = fpi().dataPtr(); 
      const int*  n_lo = fpi().loVect();
      const int*  n_hi = fpi().hiVect();

      const Real* lbddat = (*lbd_cc)[fpi].dataPtr();
      const int* lbd_lo  = (*lbd_cc)[fpi].loVect();
      const int* lbd_hi  = (*lbd_cc)[fpi].hiVect();	

      const Real* pdat   = (*rock_phi)[fpi].dataPtr();
      const int* p_lo    = (*rock_phi)[fpi].loVect();
      const int* p_hi    = (*rock_phi)[fpi].hiVect();

      const Real* kdat   = (*kappa)[fpi].dataPtr();
      const int* k_lo    = (*kappa)[fpi].loVect();
      const int* k_hi    = (*kappa)[fpi].hiVect();

      const int* lo      = fpi.validbox().loVect();
      const int* hi      = fpi.validbox().hiVect();

      const int* dfx_lo  = (*diffusivity[0])[idx].loVect();
      const int* dfx_hi  = (*diffusivity[0])[idx].hiVect();
      const Real* dfxdat = (*diffusivity[0])[idx].dataPtr();

      const int* dfy_lo  = (*diffusivity[1])[idx].loVect();
      const int* dfy_hi  = (*diffusivity[1])[idx].hiVect();
      const Real* dfydat = (*diffusivity[1])[idx].dataPtr();

#if(BL_SPACEDIM==3)
      const int* dfz_lo  = (*diffusivity[2])[idx].loVect();
      const int* dfz_hi  = (*diffusivity[2])[idx].hiVect();
      const Real* dfzdat = (*diffusivity[2])[idx].dataPtr();
#endif

      const Real* cpdat  = (*cpl_coef)[fpi].dataPtr(); 
      const int*  cp_lo  = (*cpl_coef)[fpi].loVect();
      const int*  cp_hi  = (*cpl_coef)[fpi].hiVect();

      Array<int> bc;
      bc = getBCArray(State_Type,idx,0,1);

      FORT_GETDIFFUSE_CPL_dp(ndat, hdat, ARLIM(n_lo), ARLIM(n_hi),
			     lbddat, ARLIM(lbd_lo), ARLIM(lbd_hi),
			     dfxdat, ARLIM(dfx_lo), ARLIM(dfx_hi),
			     dfydat, ARLIM(dfy_lo), ARLIM(dfy_hi),
#if(BL_SPACEDIM==3)
			     dfzdat, ARLIM(dfz_lo), ARLIM(dfz_hi),
#endif
			     pdat, ARLIM(p_lo), ARLIM(p_hi),
			     kdat, ARLIM(k_lo), ARLIM(k_hi),
			     cpdat, ARLIM(cp_lo), ARLIM(cp_hi),
			     &n_cpl_coef,
			     lo, hi, domlo, domhi,
			     bc.dataPtr(), &ncomp);
    }
    
  // multiply by kedge
  for (int dir = 0; dir < BL_SPACEDIM; dir++)
    {
      for (MFIter ecMfi(*diffusivity[dir]); ecMfi.isValid(); ++ecMfi)
        {
	  (*diffusivity[dir])[ecMfi].mult(kpedge[dir][ecMfi],0,0,1);
        }
      (*diffusivity[dir]).FillBoundary();
    }
}

#ifdef MG_USE_FBOXLIB
void 
PorousMedia::calc_richard_coef (MultiFab*        diffusivity[BL_SPACEDIM],
				const MultiFab*  lbd_cc,
				const MultiFab*  umac,
				const int        nc,
				const bool       do_upwind)
{

  BL_PROFILE(BL_PROFILE_THIS_NAME() + "::calc_richard_coef()");

  const int*  domlo    = geom.Domain().loVect();
  const int*  domhi    = geom.Domain().hiVect();
  const int ncp1 = nc + 1;

  // Calculate diffusivity for the richard's equation
  for (MFIter mfi(*lbd_cc); mfi.isValid(); ++mfi)
    {

      const int idx   = mfi.index();
      const int* lo     = mfi.validbox().loVect();
      const int* hi     = mfi.validbox().hiVect();
      
      const int* lbd_lo  = (*lbd_cc)[idx].loVect();
      const int* lbd_hi  = (*lbd_cc)[idx].hiVect();
      const Real* lbddat = (*lbd_cc)[idx].dataPtr();

      const int* ux_lo   = umac[0][idx].loVect();
      const int* ux_hi   = umac[0][idx].hiVect();
      const Real* uxdat  = umac[0][idx].dataPtr();

      const int* uy_lo   = umac[1][idx].loVect();
      const int* uy_hi   = umac[1][idx].hiVect();
      const Real* uydat  = umac[1][idx].dataPtr();

      const int* dfx_lo  = (*diffusivity[0])[idx].loVect();
      const int* dfx_hi  = (*diffusivity[0])[idx].hiVect();
      const Real* dfxdat = (*diffusivity[0])[idx].dataPtr();

      const int* dfy_lo  = (*diffusivity[1])[idx].loVect();
      const int* dfy_hi  = (*diffusivity[1])[idx].hiVect();
      const Real* dfydat = (*diffusivity[1])[idx].dataPtr();

#if(BL_SPACEDIM==3)      
      const int* uz_lo   = umac[2][idx].loVect();
      const int* uz_hi   = umac[2][idx].hiVect();
      const Real* uzdat  = umac[2][idx].dataPtr();

      const int* dfz_lo  = (*diffusivity[2])[idx].loVect();
      const int* dfz_hi  = (*diffusivity[2])[idx].hiVect();
      const Real* dfzdat = (*diffusivity[2])[idx].dataPtr();
#endif

      Array<int> bc;
      bc = getBCArray(State_Type,idx,0,1);

      FORT_RICHARD_COEF(lbddat, ARLIM(lbd_lo), ARLIM(lbd_hi),
			dfxdat, ARLIM(dfx_lo), ARLIM(dfx_hi),
			dfydat, ARLIM(dfy_lo), ARLIM(dfy_hi),
#if(BL_SPACEDIM==3)
			dfzdat, ARLIM(dfz_lo), ARLIM(dfz_hi),
#endif
			uxdat, ARLIM(ux_lo), ARLIM(ux_hi),
			uydat, ARLIM(uy_lo), ARLIM(uy_hi),
#if(BL_SPACEDIM==3)
			uzdat, ARLIM(uz_lo), ARLIM(uz_hi),
#endif
			lo,hi,domlo,domhi,bc.dataPtr(),
			rinflow_bc_lo.dataPtr(),rinflow_bc_hi.dataPtr(), 
			&ncp1,&do_upwind);

    }

  // multiply by kedge
  for (int dir = 0; dir < BL_SPACEDIM; dir++)
    {
      for (MFIter ecMfi(*diffusivity[dir]); ecMfi.isValid(); ++ecMfi)
	  (*diffusivity[dir])[ecMfi].mult(kpedge[dir][ecMfi],0,0,1);
      (*diffusivity[dir]).FillBoundary();
    }
}

void 
PorousMedia::calc_richard_jac (MultiFab*       diffusivity[BL_SPACEDIM],
			       const MultiFab* lbd_cc, 
			       const MultiFab* umac,
			       const Real      time,
			       const int       nc,
			       const bool      do_upwind,
			       const bool      do_n)
{
  BL_PROFILE(BL_PROFILE_THIS_NAME() + "::calc_richard_jac()");

  MultiFab& S = get_data(State_Type,time);
  const int nGrow = 1;    
  //
  // Select time level to work with (N or N+1)
  //
  const TimeLevel whichTime = which_time(State_Type,time);
  BL_ASSERT(whichTime == AmrOldTime || whichTime == AmrNewTime);
  MultiFab* pc_cc = (whichTime == AmrOldTime) ? pcn_cc : pcnp1_cc;

  const Real* dx   = geom.CellSize();
  const int*  domlo    = geom.Domain().loVect();
  const int*  domhi    = geom.Domain().hiVect();
  const int n_cpl_coef = cpl_coef->nComp(); 
  const int n_kr_coef  = kr_coef->nComp(); 
  bool do_analytic_jac = false;//true;
  for (FillPatchIterator fpi(*this,S,nGrow,time,State_Type,0,ncomps);
       fpi.isValid();
       ++fpi)
    {
      dirichletStateBC(fpi(),nGrow,time);

      const int idx   = fpi.index();
      const Box box   = BoxLib::grow(grids[idx],nGrow);

      BL_ASSERT(box == fpi().box());

      const Real* ndat = fpi().dataPtr(); 
      const int*  n_lo = fpi().loVect();
      const int*  n_hi = fpi().hiVect();

      const Real* lbddat = (*lbd_cc)[fpi].dataPtr();
      const int* lbd_lo  = (*lbd_cc)[fpi].loVect();
      const int* lbd_hi  = (*lbd_cc)[fpi].hiVect();

      const Real* pcdat  = (*pc_cc)[fpi].dataPtr(); 
      const int*  pc_lo  = (*pc_cc)[fpi].loVect();
      const int*  pc_hi  = (*pc_cc)[fpi].hiVect();

      const Real* pdat   = (*rock_phi)[fpi].dataPtr();
      const int* p_lo    = (*rock_phi)[fpi].loVect();
      const int* p_hi    = (*rock_phi)[fpi].hiVect();

      const Real* kdat   = (*kappa)[fpi].dataPtr();
      const int* k_lo    = (*kappa)[fpi].loVect();
      const int* k_hi    = (*kappa)[fpi].hiVect();

      const int* lo      = fpi.validbox().loVect();
      const int* hi      = fpi.validbox().hiVect();

      const int* ux_lo   = umac[0][idx].loVect();
      const int* ux_hi   = umac[0][idx].hiVect();
      const Real* uxdat  = umac[0][idx].dataPtr();

      const int* uy_lo   = umac[1][idx].loVect();
      const int* uy_hi   = umac[1][idx].hiVect();
      const Real* uydat  = umac[1][idx].dataPtr();

      const int* dfx_lo  = (*diffusivity[0])[idx].loVect();
      const int* dfx_hi  = (*diffusivity[0])[idx].hiVect();
      const Real* dfxdat = (*diffusivity[0])[idx].dataPtr();

      const int* dfy_lo  = (*diffusivity[1])[idx].loVect();
      const int* dfy_hi  = (*diffusivity[1])[idx].hiVect();
      const Real* dfydat = (*diffusivity[1])[idx].dataPtr();

      const int* kpx_lo  = kpedge[0][idx].loVect();
      const int* kpx_hi  = kpedge[0][idx].hiVect();
      const Real* kpxdat = kpedge[0][idx].dataPtr();

      const int* kpy_lo  = kpedge[1][idx].loVect();
      const int* kpy_hi  = kpedge[1][idx].hiVect();
      const Real* kpydat = kpedge[1][idx].dataPtr();

#if(BL_SPACEDIM==3)    
      const int* uz_lo   = umac[2][idx].loVect();
      const int* uz_hi   = umac[2][idx].hiVect();
      const Real* uzdat  = umac[2][idx].dataPtr();

      const int* dfz_lo  = (*diffusivity[2])[idx].loVect();
      const int* dfz_hi  = (*diffusivity[2])[idx].hiVect();
      const Real* dfzdat = (*diffusivity[2])[idx].dataPtr();

      const int* kpz_lo  = kpedge[2][idx].loVect();
      const int* kpz_hi  = kpedge[2][idx].hiVect();
      const Real* kpzdat = kpedge[2][idx].dataPtr();
#endif 
      const Real* krdat  = (*kr_coef)[fpi].dataPtr(); 
      const int*  kr_lo  = (*kr_coef)[fpi].loVect();
      const int*  kr_hi  = (*kr_coef)[fpi].hiVect();
      const Real* cpdat  = (*cpl_coef)[fpi].dataPtr(); 
      const int*  cp_lo  = (*cpl_coef)[fpi].loVect();
      const int*  cp_hi  = (*cpl_coef)[fpi].hiVect();

      Array<int> bc;
      bc = getBCArray(Press_Type,idx,0,1);

      if (do_analytic_jac) 
	FORT_RICHARD_AJAC(ndat,   ARLIM(n_lo), ARLIM(n_hi),
			      dfxdat, ARLIM(dfx_lo), ARLIM(dfx_hi),
			      dfydat, ARLIM(dfy_lo), ARLIM(dfy_hi),
#if(BL_SPACEDIM==3)
			      dfzdat, ARLIM(dfz_lo), ARLIM(dfz_hi),
#endif	
			      uxdat, ARLIM(ux_lo), ARLIM(ux_hi),
			      uydat, ARLIM(uy_lo), ARLIM(uy_hi),
#if(BL_SPACEDIM==3)
			      uzdat, ARLIM(uz_lo), ARLIM(uz_hi),
#endif
			      kpxdat, ARLIM(kpx_lo), ARLIM(kpx_hi),
			      kpydat, ARLIM(kpy_lo), ARLIM(kpy_hi),
#if(BL_SPACEDIM==3)
			      kpzdat, ARLIM(kpz_lo), ARLIM(kpz_hi),
#endif
			      lbddat, ARLIM(lbd_lo), ARLIM(lbd_hi),
			      pcdat, ARLIM(pc_lo), ARLIM(pc_hi),
			      pdat, ARLIM(p_lo), ARLIM(p_hi),
			      kdat, ARLIM(k_lo), ARLIM(k_hi),
			      krdat, ARLIM(kr_lo), ARLIM(kr_hi), &n_kr_coef,
			      cpdat, ARLIM(cp_lo), ARLIM(cp_hi), &n_cpl_coef,
			      lo, hi, domlo, domhi, dx, bc.dataPtr(), 
			      rinflow_bc_lo.dataPtr(),rinflow_bc_hi.dataPtr(), 
			      &do_upwind);
      else
	{
	  Real deps = 1.e-8;
	  if (do_n)
	    FORT_RICHARD_NJAC(ndat,   ARLIM(n_lo), ARLIM(n_hi),
			      dfxdat, ARLIM(dfx_lo), ARLIM(dfx_hi),
			      dfydat, ARLIM(dfy_lo), ARLIM(dfy_hi),
#if(BL_SPACEDIM==3)
			      dfzdat, ARLIM(dfz_lo), ARLIM(dfz_hi),
#endif	
			      uxdat, ARLIM(ux_lo), ARLIM(ux_hi),
			      uydat, ARLIM(uy_lo), ARLIM(uy_hi),
#if(BL_SPACEDIM==3)
			      uzdat, ARLIM(uz_lo), ARLIM(uz_hi),
#endif
			      kpxdat, ARLIM(kpx_lo), ARLIM(kpx_hi),
			      kpydat, ARLIM(kpy_lo), ARLIM(kpy_hi),
#if(BL_SPACEDIM==3)
			      kpzdat, ARLIM(kpz_lo), ARLIM(kpz_hi),
#endif
			      lbddat, ARLIM(lbd_lo), ARLIM(lbd_hi),
			      pcdat, ARLIM(pc_lo), ARLIM(pc_hi),
			      pdat, ARLIM(p_lo), ARLIM(p_hi),
			      kdat, ARLIM(k_lo), ARLIM(k_hi),
			      krdat, ARLIM(kr_lo), ARLIM(kr_hi), &n_kr_coef,
			      cpdat, ARLIM(cp_lo), ARLIM(cp_hi), &n_cpl_coef,
			      lo, hi, domlo, domhi, dx, bc.dataPtr(), 
			      rinflow_bc_lo.dataPtr(),rinflow_bc_hi.dataPtr(), 
			      &deps, &do_upwind);
	  else
	    FORT_RICHARD_NJAC2(dfxdat, ARLIM(dfx_lo), ARLIM(dfx_hi),
			       dfydat, ARLIM(dfy_lo), ARLIM(dfy_hi),
#if(BL_SPACEDIM==3)
			       dfzdat, ARLIM(dfz_lo), ARLIM(dfz_hi),
#endif	
			       uxdat, ARLIM(ux_lo), ARLIM(ux_hi),
			       uydat, ARLIM(uy_lo), ARLIM(uy_hi),
#if(BL_SPACEDIM==3)
			       uzdat, ARLIM(uz_lo), ARLIM(uz_hi),
#endif
			       kpxdat, ARLIM(kpx_lo), ARLIM(kpx_hi),
			       kpydat, ARLIM(kpy_lo), ARLIM(kpy_hi),
#if(BL_SPACEDIM==3)
			       kpzdat, ARLIM(kpz_lo), ARLIM(kpz_hi),
#endif
			       lbddat, ARLIM(lbd_lo), ARLIM(lbd_hi),
			       pcdat, ARLIM(pc_lo), ARLIM(pc_hi),
			       pdat, ARLIM(p_lo), ARLIM(p_hi),
			       kdat, ARLIM(k_lo), ARLIM(k_hi),
			       krdat, ARLIM(kr_lo), ARLIM(kr_hi), &n_kr_coef,
			       cpdat, ARLIM(cp_lo), ARLIM(cp_hi), &n_cpl_coef,
			       lo, hi, domlo, domhi, dx, bc.dataPtr(), 
			       rinflow_bc_lo.dataPtr(),rinflow_bc_hi.dataPtr(), 
			       &deps, &do_upwind);
	}
    }
}

void 
PorousMedia::calc_richard_alpha (MultiFab*     alpha,
				 const Real    time)
{
  BL_PROFILE(BL_PROFILE_THIS_NAME() + "::calc_richard_alpha()");

  MultiFab& S = get_data(State_Type,time);
  const int nGrow = 1;    

  const int n_cpl_coef = cpl_coef->nComp(); 
  for (FillPatchIterator fpi(*this,S,nGrow,time,State_Type,0,ncomps);
       fpi.isValid();
       ++fpi)
    {
      dirichletStateBC(fpi(),nGrow,time);

      const int idx   = fpi.index();
      const Box box   = BoxLib::grow(grids[idx],nGrow);

      BL_ASSERT(box == fpi().box());

      const Real* ndat = fpi().dataPtr(); 
      const int*  n_lo = fpi().loVect();
      const int*  n_hi = fpi().hiVect();

      const Real* adat = (*alpha)[fpi].dataPtr();
      const int*  a_lo = (*alpha)[fpi].loVect();
      const int*  a_hi = (*alpha)[fpi].hiVect();

      const Real* pdat   = (*rock_phi)[fpi].dataPtr();
      const int* p_lo    = (*rock_phi)[fpi].loVect();
      const int* p_hi    = (*rock_phi)[fpi].hiVect();

      const Real* kdat   = (*kappa)[fpi].dataPtr();
      const int* k_lo    = (*kappa)[fpi].loVect();
      const int* k_hi    = (*kappa)[fpi].hiVect();

      const int* lo      = fpi.validbox().loVect();
      const int* hi      = fpi.validbox().hiVect();

      const Real* cpdat  = (*cpl_coef)[fpi].dataPtr(); 
      const int*  cp_lo  = (*cpl_coef)[fpi].loVect();
      const int*  cp_hi  = (*cpl_coef)[fpi].hiVect();

			  
      FORT_RICHARD_ALPHA(adat, ARLIM(a_lo), ARLIM(a_hi),
			 ndat, ARLIM(n_lo), ARLIM(n_hi),
			 pdat, ARLIM(p_lo), ARLIM(p_hi),
			 kdat, ARLIM(k_lo), ARLIM(k_hi),
			 cpdat, ARLIM(cp_lo), ARLIM(cp_hi),
			 &n_cpl_coef, lo, hi);
    }
}

void 
PorousMedia::calc_richard_velbc (MultiFab& res, const Real dt)  
{ 
  //
  // Add boundary condition to residual
  //
 
  const int* domlo = geom.Domain().loVect(); 
  const int* domhi = geom.Domain().hiVect();
  const Real* dx   = geom.CellSize();

  for (MFIter mfi(res); mfi.isValid(); ++mfi)
    {
      const int* lo = mfi.validbox().loVect();
      const int* hi = mfi.validbox().hiVect();
	
      FArrayBox& rg       = res[mfi];     
      DEF_LIMITS (rg,rg_dat,rglo,rghi);
	
      FORT_RICHARD_VELBC (rg_dat, ARLIM(rglo), ARLIM(rghi),
			  lo,hi,domlo,domhi,dx,
			  inflow_bc_lo.dataPtr(),inflow_bc_hi.dataPtr(),
			  inflow_vel_lo.dataPtr(), inflow_vel_hi.dataPtr(), &dt);
    }
}
#endif

void 
PorousMedia::calcCapillary (const Real time)
{
  //
  // Calculate the capillary pressure.  
  //
  MultiFab& S = get_data(State_Type,time);
  FillStateBndry(time,State_Type,0,ncomps);
  //
  // Select time level to work with (N or N+1)
  //
  const TimeLevel whichTime = which_time(State_Type,time);
  BL_ASSERT(whichTime == AmrOldTime || whichTime == AmrNewTime);
  MultiFab* pc_cc = (whichTime == AmrOldTime) ? pcn_cc : pcnp1_cc;

  const int nGrow = 1;
  const int n_cpl_coef = cpl_coef->nComp();
  for (FillPatchIterator fpi(*this,S,nGrow,time,State_Type,0,ncomps);
       fpi.isValid();
       ++fpi)
    {
      dirichletStateBC(fpi(),nGrow,time);
      const int idx   = fpi.index(); 
      const Box box   = BoxLib::grow(grids[idx],nGrow);
      BL_ASSERT(box == fpi().box());

      const int* lo  = grids[idx].loVect();
      const int* hi  = grids[idx].hiVect();

      const FArrayBox& Sfab = fpi();
      const Real* ndat  = Sfab.dataPtr(); 
      const int*  n_lo  = Sfab.loVect();
      const int*  n_hi  = Sfab.hiVect();

      const Real* ddat  = (*pc_cc)[fpi].dataPtr(); 
      const int*  d_lo  = (*pc_cc)[fpi].loVect();
      const int*  d_hi  = (*pc_cc)[fpi].hiVect();

      const Real* pdat = (*rock_phi)[fpi].dataPtr();
      const int* p_lo  = (*rock_phi)[fpi].loVect();
      const int* p_hi  = (*rock_phi)[fpi].hiVect();

      const Real* kdat = (*kappa)[fpi].dataPtr();
      const int* k_lo  = (*kappa)[fpi].loVect();
      const int* k_hi  = (*kappa)[fpi].hiVect();

      const Real* cpdat  = (*cpl_coef)[fpi].dataPtr(); 
      const int*  cp_lo  = (*cpl_coef)[fpi].loVect();
      const int*  cp_hi  = (*cpl_coef)[fpi].hiVect();

      Array<int> s_bc;
      s_bc = getBCArray(State_Type,idx,0,1);
      FORT_MK_CPL( ddat, ARLIM(d_lo), ARLIM(d_hi),
		   ndat, ARLIM(n_lo), ARLIM(n_hi),
		   pdat, ARLIM(p_lo), ARLIM(p_hi),
		   kdat, ARLIM(k_lo), ARLIM(k_hi),
		   cpdat, ARLIM(cp_lo), ARLIM(cp_hi),
		   &n_cpl_coef, lo, hi, s_bc.dataPtr());
    }
 pc_cc->FillBoundary();
}

void 
PorousMedia::calcCapillary (MultiFab* pc,
			    const MultiFab& S)
{
  //
  // Calculate the capillary pressure for a given state.  
  //
  BL_ASSERT(S.nGrow() >=1); // Assumes that boundary cells have been properly filled
  BL_ASSERT(pc->nGrow() >= 0); // Fill boundary cells (in F)
  const int n_cpl_coef = cpl_coef->nComp();
  for (MFIter mfi(S); mfi.isValid(); ++mfi) 
    {
      const int idx  = mfi.index();
      const int* lo  = grids[idx].loVect();
      const int* hi  = grids[idx].hiVect();

      const FArrayBox& Sfab = S[mfi];
      const Real* ndat  = Sfab.dataPtr(); 
      const int*  n_lo  = Sfab.loVect();
      const int*  n_hi  = Sfab.hiVect();

      const Real* ddat  = (*pc)[mfi].dataPtr(); 
      const int*  d_lo  = (*pc)[mfi].loVect();
      const int*  d_hi  = (*pc)[mfi].hiVect();

      const Real* pdat = (*rock_phi)[mfi].dataPtr();
      const int* p_lo  = (*rock_phi)[mfi].loVect();
      const int* p_hi  = (*rock_phi)[mfi].hiVect();

      const Real* kdat = (*kappa)[mfi].dataPtr();
      const int* k_lo  = (*kappa)[mfi].loVect();
      const int* k_hi  = (*kappa)[mfi].hiVect();

      const Real* cpdat  = (*cpl_coef)[mfi].dataPtr(); 
      const int*  cp_lo  = (*cpl_coef)[mfi].loVect();
      const int*  cp_hi  = (*cpl_coef)[mfi].hiVect();

      Array<int> s_bc;
      s_bc = getBCArray(State_Type,idx,0,1);

      FORT_MK_CPL( ddat, ARLIM(d_lo), ARLIM(d_hi),
		   ndat, ARLIM(n_lo), ARLIM(n_hi),
		   pdat, ARLIM(p_lo), ARLIM(p_hi),
		   kdat, ARLIM(k_lo), ARLIM(k_hi),
		   cpdat, ARLIM(cp_lo), ARLIM(cp_hi),
		   &n_cpl_coef, lo, hi, s_bc.dataPtr());
    }
  pc->FillBoundary();
}

void 
PorousMedia::calcInvCapillary (const Real time)
{
  //
  // Calculate the capillary pressure.  
  //
  MultiFab& S = get_data(State_Type,time);
  //
  // Select time level to work with (N or N+1)
  //
  const TimeLevel whichTime = which_time(State_Type,time);
    
  BL_ASSERT(whichTime == AmrOldTime || whichTime == AmrNewTime);

  //
  // pcn_cc and pcnp1_cc are in PorousMedia class.
  //
  MultiFab* pc_cc = (whichTime == AmrOldTime) ? pcn_cc : pcnp1_cc;
  //
  // Calculate inverse capillary pressure
  //    
  const int n_cpl_coef = cpl_coef->nComp();

  for (MFIter mfi(*pc_cc); mfi.isValid(); ++mfi)
    {
      FArrayBox& Sfab   = S[mfi];
      const Real* ndat  = Sfab.dataPtr(); 
      const int*  n_lo  = Sfab.loVect();
      const int*  n_hi  = Sfab.hiVect();

      const Real* ddat  = (*pc_cc)[mfi].dataPtr(); 
      const int*  d_lo  = (*pc_cc)[mfi].loVect();
      const int*  d_hi  = (*pc_cc)[mfi].hiVect();

      const Real* pdat = (*rock_phi)[mfi].dataPtr();
      const int* p_lo  = (*rock_phi)[mfi].loVect();
      const int* p_hi  = (*rock_phi)[mfi].hiVect();

      const Real* kdat = (*kappa)[mfi].dataPtr();
      const int* k_lo  = (*kappa)[mfi].loVect();
      const int* k_hi  = (*kappa)[mfi].hiVect();

      const Real* cpdat  = (*cpl_coef)[mfi].dataPtr(); 
      const int*  cp_lo  = (*cpl_coef)[mfi].loVect();
      const int*  cp_hi  = (*cpl_coef)[mfi].hiVect();

      FORT_MK_INV_CPL( ddat, ARLIM(d_lo), ARLIM(d_hi),
		       ndat, ARLIM(n_lo), ARLIM(n_hi),
		       pdat, ARLIM(p_lo), ARLIM(p_hi),
		       kdat, ARLIM(k_lo), ARLIM(k_hi),
		       cpdat, ARLIM(cp_lo), ARLIM(cp_hi),
		       &n_cpl_coef);
    }
}

void 
PorousMedia::calcInvCapillary (MultiFab& S,
			       const MultiFab& pc)
{
  //
  // Calculate inverse capillary pressure
  //    
  const int n_cpl_coef = cpl_coef->nComp();
  for (MFIter mfi(S); mfi.isValid(); ++mfi)
    {

      FArrayBox& Sfab   = S[mfi];
      const Real* ndat  = Sfab.dataPtr(); 
      const int*  n_lo  = Sfab.loVect();
      const int*  n_hi  = Sfab.hiVect();

      const Real* ddat  = pc[mfi].dataPtr(); 
      const int*  d_lo  = pc[mfi].loVect();
      const int*  d_hi  = pc[mfi].hiVect();

      const Real* pdat = (*rock_phi)[mfi].dataPtr();
      const int* p_lo  = (*rock_phi)[mfi].loVect();
      const int* p_hi  = (*rock_phi)[mfi].hiVect();

      const Real* kdat = (*kappa)[mfi].dataPtr();
      const int* k_lo  = (*kappa)[mfi].loVect();
      const int* k_hi  = (*kappa)[mfi].hiVect();

      const Real* cpdat  = (*cpl_coef)[mfi].dataPtr(); 
      const int*  cp_lo  = (*cpl_coef)[mfi].loVect();
      const int*  cp_hi  = (*cpl_coef)[mfi].hiVect();

      FORT_MK_INV_CPL( ddat, ARLIM(d_lo), ARLIM(d_hi),
		       ndat, ARLIM(n_lo), ARLIM(n_hi),
		       pdat, ARLIM(p_lo), ARLIM(p_hi),
		       kdat, ARLIM(k_lo), ARLIM(k_hi),
		       cpdat, ARLIM(cp_lo), ARLIM(cp_hi),
		       &n_cpl_coef);
      
    }
}

void 
PorousMedia::smooth_pc (MultiFab* pc)
{
  //
  // Calculate the capillary pressure for a given state.  
  //
  const int n_cpl_coef = cpl_coef->nComp();
  for (MFIter mfi(*pc); mfi.isValid(); ++mfi) 
    {
      const int idx  = mfi.index();
      const int* lo  = grids[idx].loVect();
      const int* hi  = grids[idx].hiVect();

      const Real* ddat  = (*pc)[mfi].dataPtr(); 
      const int*  d_lo  = (*pc)[mfi].loVect();
      const int*  d_hi  = (*pc)[mfi].hiVect();

      const Real* cpdat  = (*cpl_coef)[mfi].dataPtr(); 
      const int*  cp_lo  = (*cpl_coef)[mfi].loVect();
      const int*  cp_hi  = (*cpl_coef)[mfi].hiVect();

      FORT_SMOOTH_CPL( ddat, ARLIM(d_lo), ARLIM(d_hi),
		       cpdat, ARLIM(cp_lo), ARLIM(cp_hi),
		       &n_cpl_coef, lo, hi);
    }
  pc->FillBoundary();
}


void 
PorousMedia::calcLambda (const Real time, MultiFab* lbd_cc)
{
  //
  // Calculate the lambda values at cell-center. 
  //
  MultiFab& S = get_data(State_Type,time);
  FillStateBndry(time,State_Type,0,ncomps);
  MultiFab* lcc;
  if (lbd_cc == 0)
    {
      const TimeLevel whichTime = which_time(State_Type,time);
      BL_ASSERT(whichTime == AmrOldTime || whichTime == AmrNewTime);
      lcc = (whichTime == AmrOldTime) ? lambda_cc : lambdap1_cc;
    }
  else
    lcc = lbd_cc;  

  const int nGrow = 1;
  const int n_kr_coef = kr_coef->nComp();  
  for (FillPatchIterator fpi(*this,S,nGrow,time,State_Type,0,ncomps);
       fpi.isValid();
       ++fpi)
    {
      dirichletStateBC(fpi(),nGrow,time);
      const int idx   = fpi.index();
      const Box box   = BoxLib::grow(grids[idx],nGrow);
      BL_ASSERT(box == fpi().box());

      const FArrayBox& Sfab = fpi();
      const Real* ndat  = Sfab.dataPtr(); 
      const int*  n_lo  = Sfab.loVect();
      const int*  n_hi  = Sfab.hiVect();

      const Real* ddat  = (*lcc)[fpi].dataPtr(); 
      const int*  d_lo  = (*lcc)[fpi].loVect();
      const int*  d_hi  = (*lcc)[fpi].hiVect();

      const Real* krdat  = (*kr_coef)[fpi].dataPtr(); 
      const int*  kr_lo  = (*kr_coef)[fpi].loVect();
      const int*  kr_hi  = (*kr_coef)[fpi].hiVect();
	
      FORT_MK_LAMBDA( ddat, ARLIM(d_lo), ARLIM(d_hi),
		      ndat, ARLIM(n_lo), ARLIM(n_hi),
		      krdat, ARLIM(kr_lo),ARLIM(kr_hi),
		      &n_kr_coef);
    }
  lcc->FillBoundary();
}

void 
PorousMedia::calcLambda (MultiFab* lbd, const MultiFab& S)
{
  //
  // Calculate the lambda values at cell-center. 
  //   
  const int n_kr_coef = kr_coef->nComp();
  for (MFIter mfi(S); mfi.isValid(); ++mfi)
    {
      const FArrayBox& Sfab = S[mfi];
      const Real* ndat  = Sfab.dataPtr(); 
      const int*  n_lo  = Sfab.loVect();
      const int*  n_hi  = Sfab.hiVect();

      const Real* ddat  = (*lbd)[mfi].dataPtr(); 
      const int*  d_lo  = (*lbd)[mfi].loVect();
      const int*  d_hi  = (*lbd)[mfi].hiVect();

      const Real* krdat  = (*kr_coef)[mfi].dataPtr(); 
      const int*  kr_lo  = (*kr_coef)[mfi].loVect();
      const int*  kr_hi  = (*kr_coef)[mfi].hiVect();
	
      FORT_MK_LAMBDA( ddat, ARLIM(d_lo), ARLIM(d_hi),
		      ndat, ARLIM(n_lo), ARLIM(n_hi),
		      krdat, ARLIM(kr_lo),ARLIM(kr_hi),
		      &n_kr_coef);
    }
  lbd->FillBoundary();
}

void 
PorousMedia::calcDLambda (const Real time, MultiFab* dlbd_cc)
{
  //
  // Calculate the lambda values at cell-center. 
  //

  MultiFab& S = get_data(State_Type,time);

  MultiFab* dlcc;
  if (dlbd_cc == 0)
    dlcc = dlambda_cc;
  else
    dlcc = dlbd_cc;

  const int nGrow = 1;    
  const int n_kr_coef = kr_coef->nComp();
  for (FillPatchIterator fpi(*this,S,nGrow,time,State_Type,0,ncomps);
       fpi.isValid();
       ++fpi)
    {
      dirichletStateBC(fpi(),nGrow,time);

      const int idx   = fpi.index();
      const Box box   = BoxLib::grow(grids[idx],nGrow);

      BL_ASSERT(box == fpi().box());

      FArrayBox& Sfab   = fpi();
      const Real* ndat  = Sfab.dataPtr(); 
      const int*  n_lo  = Sfab.loVect();
      const int*  n_hi  = Sfab.hiVect();

      const Real* ddat  = (*dlcc)[fpi].dataPtr(); 
      const int*  d_lo  = (*dlcc)[fpi].loVect();
      const int*  d_hi  = (*dlcc)[fpi].hiVect();

      const Real* krdat  = (*kr_coef)[fpi].dataPtr(); 
      const int*  kr_lo  = (*kr_coef)[fpi].loVect();
      const int*  kr_hi  = (*kr_coef)[fpi].hiVect();

      FORT_MK_DLAMBDA( ddat, ARLIM(d_lo), ARLIM(d_hi),
		       ndat, ARLIM(n_lo), ARLIM(n_hi), 
		       krdat, ARLIM(kr_lo),ARLIM(kr_hi),
		       &n_kr_coef);
    }

  (*dlcc).FillBoundary();
    
}

void
PorousMedia::set_overdetermined_boundary_cells (Real time)
{
  BL_PROFILE(BL_PROFILE_THIS_NAME() + "::set_overdetermined_boundary_cells()");

}


void
PorousMedia::center_to_edge_plain (const FArrayBox& ccfab,
				   FArrayBox&       ecfab,
				   int              sComp,
				   int              dComp,
				   int              nComp)
{
  BL_PROFILE(BL_PROFILE_THIS_NAME() + "::center_to_edge_plain()");

  //
  // This routine fills an edge-centered FAB from a cell-centered FAB.
  // It assumes that the data in all cells of the cell-centered FAB is
  // valid and totally ignores any concept of boundary conditions.  
  // It is assummed that the cell-centered FAB fully contains the 
  // edge-centered FAB.  If anything special needs to be done at boundaries, 
  // a varient of this routine needs to be written.  See 
  // HeatTransfer::center_to_edge_fancy().
  //
  const Box&      ccbox = ccfab.box();
  const Box&      ecbox = ecfab.box();
  const IndexType ixt   = ecbox.ixType();
  //
  // Get direction for interpolation to edges
  //
  int dir = -1;
  for (int d = 0; d < BL_SPACEDIM; d++)
    if (ixt.test(d))
      dir = d;
  //
  // Miscellanious checks
  //
  BL_ASSERT(!(ixt.cellCentered()) && !(ixt.nodeCentered()));
  BL_ASSERT(BoxLib::grow(ccbox,-BoxLib::BASISV(dir)).contains(BoxLib::enclosedCells(ecbox)));
  BL_ASSERT(sComp+nComp <= ccfab.nComp() && dComp+nComp <= ecfab.nComp());
  //
  // Shift cell-centered data to edges
  //
  Box fillBox = ccbox; 
  for (int d = 0; d < BL_SPACEDIM; d++)
    if (d != dir)
      fillBox.setRange(d, ecbox.smallEnd(d), ecbox.length(d));
    
  const int isharm = def_harm_avg_cen2edge;
  FORT_CEN2EDG(fillBox.loVect(), fillBox.hiVect(),
	       ARLIM(ccfab.loVect()), ARLIM(ccfab.hiVect()),
	       ccfab.dataPtr(sComp),
	       ARLIM(ecfab.loVect()), ARLIM(ecfab.hiVect()),
	       ecfab.dataPtr(dComp),
	       &nComp, &dir, &isharm);
}

// ===================
// Boundary Conditions
// ===================

void
PorousMedia::getDirichletFaces (Array<Orientation>& Faces,
				const int           comp_Type,
				const BCRec&        _bc)
{
  Faces.resize(0);
  for (int idir = 0; idir < BL_SPACEDIM; idir++)
    {
      if ((comp_Type == Press_Type && _bc.lo(idir) == EXT_DIR) ||
	  (comp_Type == State_Type && _bc.lo(idir) == EXT_DIR))
        {
	  const int len = Faces.size();
	  Faces.resize(len+1);
	  Faces.set(len,Orientation(idir,Orientation::low));
        }
      if ((comp_Type == Press_Type && _bc.hi(idir) == EXT_DIR) ||
	  (comp_Type == State_Type && _bc.hi(idir) == EXT_DIR))
        {
	  const int len = Faces.size();
	  Faces.resize(len+1);
	  Faces.set(len,Orientation(idir,Orientation::high));
        }
    }
}

bool
PorousMedia::grids_on_side_of_domain (const BoxArray&    _grids,
				      const Box&         _domain,
				      const Orientation& _Face) 
{
    const int idir = _Face.coordDir();

    if (_Face.isLow())
      {
        for (int igrid = 0; igrid < _grids.size(); igrid++)
	  { 
            if (_grids[igrid].smallEnd(idir) == _domain.smallEnd(idir))
	      return true;
	  }
      }
  
    if (_Face.isHigh())
      {
        for (int igrid = 0; igrid < _grids.size(); igrid++)
	  {
            if (_grids[igrid].bigEnd(idir) == _domain.bigEnd(idir))
	      return true;
	  }
      }

    return false;
}


void
PorousMedia::dirichletStateBC (const Real time)
{
  Array<Orientation> Faces;
  const BCRec& bc = desc_lst[State_Type].getBC(0);
  getDirichletFaces(Faces,State_Type,bc);

  BL_ASSERT(bc_array.size() >= Faces.size());

  if (Faces.size()>0)
    {
      const Box& domain = geom.Domain();

      BoxList  ccBoxList;

      IntVect ratio = IntVect::TheUnitVector();
      for (int lev = level+1; lev <= parent->finestLevel(); lev++)
	ratio *= parent->refRatio(lev-1);
    
      for (int iface = 0; iface < Faces.size(); iface++)
	{
	  if (grids_on_side_of_domain(grids,domain,Faces[iface])) 
	    {
      	      Box ccBndBox  = BoxLib::adjCell(domain,Faces[iface],1);
	      if (ccBndBox.ok())
		ccBoxList.push_back(ccBndBox);
	    }
	}
      

      if (!ccBoxList.isEmpty())
	{      
	  MultiFab& S = get_data(State_Type,time); 

	  const Real* dx   = geom.CellSize();
	  const int* domlo  = domain.loVect();
	  const int* domhi  = domain.hiVect();

	  BoxArray  ccBoxArray( ccBoxList);

	  FArrayBox sdat;
	  FArrayBox cdat;
	  for ( int iface = 0; iface < ccBoxList.size(); ++iface) 
	    {
	      sdat.resize(ccBoxArray[iface], ncomps);
	      sdat.setVal(0.0);
	      
	      int face  = int(Faces[iface]);
      
	      for (Array<BCData>::iterator it = bc_array.begin(); 
		   it < bc_array.end(); it++)
		{
		  if ((*it).type == bc_list["file"]) 
		    {
		      std::cerr << "Initialization of boundary condition based on "
				<< "a file has not been implemented yet.\n";
		      BoxLib::Abort("PorousMedia::dirichletStateBC()");
		    }
		  else if ((*it).type == bc_list["scalar"] || 
			   (*it).type == bc_list["zero_total_velocity"]) 
		    {
		      for (Array<int>::iterator jt = (*it).region.begin(); 
			   jt < (*it).region.end(); jt++)
			region_array[*jt]->setVal(sdat,(*it).param,
						  dx,0,0,ncomps);
		    }
		  else if ((*it).type == bc_list["hydrostatic"])
		    {
		      const int inDir = Faces[iface].coordDir();
		      if (inDir != BL_SPACEDIM - 1)
			{
			  const int n_cpl_coef = cpl_coef->nComp();
			  cdat.resize(ccBoxArray[iface],n_cpl_coef);

			  for (MFIter mfi(*cpl_coef); mfi.isValid(); ++mfi)
			    {
			      Box ovlp = (*cpl_coef)[mfi].box() & cdat.box();
			      if (ovlp.ok()) 
				cdat.copy((*cpl_coef)[mfi],ovlp,0,ovlp,0,
					  n_cpl_coef);
			    }
			  DEF_LIMITS(sdat,s_ptr,s_lo,s_hi);
			  DEF_CLIMITS(cdat,c_ptr,c_lo,c_hi);
			  Real wt_loc = wt_lo;
			  if (Faces[iface].faceDir() == Orientation::high)
			    wt_loc = wt_hi;

			  FORT_HYDRO(s_ptr, ARLIM(s_lo),ARLIM(s_hi), 
				     density.dataPtr(), &ncomps, 
				     c_ptr, ARLIM(c_lo),ARLIM(c_hi), 
				     &n_cpl_coef, dx, &wt_loc, &gravity);
			}
		    }
		}
	      
	      for (MFIter mfi(S); mfi.isValid(); ++mfi)
		{
		  Box ovlp = S[mfi].box() & sdat.box();
		  if (ovlp.ok()) 
		    {
		      S[mfi].copy(sdat,ovlp,0,ovlp,0,ncomps);
		      
		      if (S.nGrow() > 1)
			{
			  DEF_LIMITS(S[mfi],s_ptr,s_lo,s_hi);
			  FORT_PATCH_GHOST(s_ptr,ARLIM(s_lo),ARLIM(s_hi),
					   &ncomps,&face,domlo,domhi);
			}
		    }
		}
	      S.FillBoundary();
	    }
	}
    }
}

void
PorousMedia::dirichletStateBC (FArrayBox& fab, const int ngrow, const Real time)
{
  Array<Orientation> Faces;
  const BCRec& bc = desc_lst[State_Type].getBC(0);
  getDirichletFaces(Faces,State_Type,bc);

  BL_ASSERT(bc_array.size() >= Faces.size());
  if (Faces.size()>0)
    {
      const Box& domain = geom.Domain();
      BoxList  ccBoxList;
      IntVect ratio = IntVect::TheUnitVector();
      for (int lev = level+1; lev <= parent->finestLevel(); lev++)
	ratio *= parent->refRatio(lev-1);    
      for (int iface = 0; iface < Faces.size(); iface++)
	{
	  if (grids_on_side_of_domain(grids,domain,Faces[iface])) 
	    {
      	      Box ccBndBox  = BoxLib::adjCell(domain,Faces[iface],1);
	      for (int dir = 0; dir<BL_SPACEDIM; dir++)
		{
		  if (dir != Faces[iface].coordDir())
		    {
		      ccBndBox.growLo(dir,1);
		      ccBndBox.growHi(dir,1);
		    }
		}
	      const Box valid_ccBndBox = ccBndBox & fab.box();
	      if (valid_ccBndBox.ok())
		ccBoxList.push_back(valid_ccBndBox);
	    }
	}

      if (!ccBoxList.isEmpty())
	{  
	  const Real* dx   = geom.CellSize();
	  const int* domlo  = domain.loVect();
	  const int* domhi  = domain.hiVect();

	  BoxArray  ccBoxArray( ccBoxList);
	  FArrayBox sdat;
	  FArrayBox cdat;
	  for ( int iface = 0; iface < ccBoxList.size(); ++iface) 
	    {
	      sdat.resize(ccBoxArray[iface], ncomps);
	      sdat.setVal(0.0);
	      
	      int face  = int(Faces[iface]);

	      for (Array<BCData>::iterator it = bc_array.begin(); 
		   it < bc_array.end(); it++)
		{
		  if ((*it).type == bc_list["file"]) 
		    {
		      std::cerr << "Initialization of boundary condition based on "
				<< "a file has not been implemented yet.\n";
		      BoxLib::Abort("PorousMedia::dirichletStateBC()");
		    }
		  else if ((*it).type == bc_list["scalar"] || 
			   (*it).type == bc_list["zero_total_velocity"]) 
		    {
		      for (Array<int>::iterator jt = (*it).region.begin(); 
			   jt < (*it).region.end(); jt++)
			{
			  region_array[*jt]->setVal(sdat,(*it).param,
						    dx,0,0,ncomps);
			}
		    }
		  else if ((*it).type == bc_list["hydrostatic"])
		    {
		      const int inDir = Faces[iface].coordDir();
		      if (inDir != BL_SPACEDIM - 1)
			{
			  const int n_cpl_coef = cpl_coef->nComp();
			  cdat.resize(ccBoxArray[iface],n_cpl_coef);

			  for (MFIter mfi(*cpl_coef); mfi.isValid(); ++mfi)
			    {
			      Box ovlp = (*cpl_coef)[mfi].box() & cdat.box();
			      if (ovlp.ok()) 
				cdat.copy((*cpl_coef)[mfi],ovlp,0,ovlp,0,
					  n_cpl_coef);
			    }
			  DEF_LIMITS(sdat,s_ptr,s_lo,s_hi);
			  DEF_CLIMITS(cdat,c_ptr,c_lo,c_hi);
			  Real wt_loc = wt_lo;
			  if (Faces[iface].faceDir() == Orientation::high)
			    wt_loc = wt_hi;

			  FORT_HYDRO(s_ptr, ARLIM(s_lo),ARLIM(s_hi), 
				     density.dataPtr(), &ncomps, 
				     c_ptr, ARLIM(c_lo),ARLIM(c_hi), 
				     &n_cpl_coef, dx, &wt_loc, &gravity);
			}
		    }
		}
	      
	      Box ovlp = fab.box() & sdat.box();
	      fab.copy(sdat,ovlp,0,ovlp,0,ncomps);
	      if (ngrow > 1)
		{
		  DEF_LIMITS(fab,s_ptr,s_lo,s_hi);
		  FORT_PATCH_GHOST(s_ptr,ARLIM(s_lo),ARLIM(s_hi),
				   &ncomps,&face,domlo,domhi);
		}
	    }
	}
    }
  
}

void
PorousMedia::dirichletTracerBC (FArrayBox& fab, const int ngrow, const Real time)
{
  Array<Orientation> Faces;
  const BCRec& bc = desc_lst[State_Type].getBC(0);
  getDirichletFaces(Faces,State_Type,bc);

  if (Faces.size()>0)
    {
      const Box& domain = geom.Domain();

      BoxList  ccBoxList;

      IntVect ratio = IntVect::TheUnitVector();
      for (int lev = level+1; lev <= parent->finestLevel(); lev++)
	ratio *= parent->refRatio(lev-1);
    
      for (int iface = 0; iface < Faces.size(); iface++)
	{
	  if (grids_on_side_of_domain(grids,domain,Faces[iface])) 
	    {
      	      Box ccBndBox  = BoxLib::adjCell(domain,Faces[iface],1);
	      for (int dir = 0; dir<BL_SPACEDIM; dir++)
		{
		  if (dir != Faces[iface].coordDir())
		    {
		      ccBndBox.growLo(dir,1);
		      ccBndBox.growHi(dir,1);
		    }
		}
	      const Box valid_ccBndBox = ccBndBox & fab.box();
	      if (valid_ccBndBox.ok())
		ccBoxList.push_back(valid_ccBndBox);
	    }
	}
      if (!ccBoxList.isEmpty())
	{  
	  const Real* dx   = geom.CellSize();
	  const int* domlo  = domain.loVect();
	  const int* domhi  = domain.hiVect();

	  BoxArray  ccBoxArray( ccBoxList);

	  FArrayBox sdat;
	  FArrayBox cdat;
	  for ( int iface = 0; iface < ccBoxList.size(); ++iface) 
	    {
	      sdat.resize(ccBoxArray[iface], ntracers);
	      sdat.setVal(0.0);
	      
	      int face  = int(Faces[iface]);

	      for (Array<BCData>::iterator it = tbc_array.begin(); 
		   it < tbc_array.end(); it++)
		{
		  if ((*it).type == bc_list["file"]) 
		    {
		      std::cerr << "Initialization of boundary condition based on "
				<< "a file has not been implemented yet.\n";
		      BoxLib::Abort("PorousMedia::dirichletTracerBC()");
		    }
		  else if ((*it).type == bc_list["scalar"]) 
		    {
		      for (Array<int>::iterator jt = (*it).region.begin(); 
			   jt < (*it).region.end(); jt++)
			region_array[*jt]->setVal(sdat,(*it).param,
						  dx,0,0,ntracers);
		    }
		}
	      Box ovlp = fab.box() & sdat.box();
	      fab.copy(sdat,ovlp,0,ovlp,0,ntracers);
	    
	      if (ngrow > 1)
		{
		  DEF_LIMITS(fab,s_ptr,s_lo,s_hi);
		  FORT_PATCH_GHOST(s_ptr,ARLIM(s_lo),ARLIM(s_hi),
				   &ntracers,&face,domlo,domhi);
		}
	    }
	}
    }
}

MultiFab*
PorousMedia::derive (const std::string& name,
                     Real               time,
                     int                ngrow)
{
    BL_ASSERT(ngrow >= 0);
    
    const DeriveRec* rec = derive_lst.get(name);
    BoxArray dstBA(grids);
    MultiFab* mf = new MultiFab(dstBA, rec->numDerive(), ngrow);
    int dcomp = 0;
    derive(name,time,*mf,dcomp);
    return mf;

}

void
PorousMedia::derive (const std::string& name,
                     Real               time,
                     MultiFab&          mf,
                     int                dcomp)
{
    const DeriveRec* rec = derive_lst.get(name);

    if (name=="MaterialID") {
        
        BL_ASSERT(dcomp < mf.nComp());

        const int ngrow = mf.nGrow();
        
        BoxArray dstBA(mf.boxArray());
        BL_ASSERT(rec->deriveType() == dstBA[0].ixType());

        const Real* dx = geom.CellSize();

        mf.setVal(-1,dcomp,1,ngrow);
        for (MFIter mfi(mf); mfi.isValid(); ++mfi)
        {
            FArrayBox& fab = mf[mfi];
            for (int i=0; i<rock_array.size(); ++i) {
                const Array<int>& rock_regions = rock_array[i].region;
                for (int j=0; j<rock_regions.size(); ++j) {
                    int regionIdx = rock_regions[j];
                    Real val = (Real)(regionIdx);
                    region_array[regionIdx]->setVal(fab,val,dcomp,dx,0);
                }
            }
        }
    }
    else if (name=="Capillary_Pressure") {
        
        if (have_capillary)
        {
            const BoxArray& BA = mf.boxArray();
            BL_ASSERT(rec->deriveType() == BA[0].ixType());

            int ngrow = 1;
            MultiFab S(BA,ncomps,ngrow);
            FillPatchIterator fpi(*this,S,ngrow,time,State_Type,0,ncomps);
            for ( ; fpi.isValid(); ++fpi)
            {
                S[fpi].copy(fpi(),0,0,ncomps);
            }
            
            int ncomp = rec->numDerive();
            MultiFab tmpmf(BA,ncomp,1);
            calcCapillary(&tmpmf,S);
            MultiFab::Copy(mf,tmpmf,0,dcomp,ncomp,0);
        }
        else {
            BoxLib::Abort("PorousMedia::derive: cannot derive Capillary Pressure");
        }
    }
    else if (name=="Volumetric_Water_Content") {
        
        // Note, assumes one comp per phase
        int scomp = -1;
        for (int i=0; i<cNames.size(); ++i) {
            if (cNames[i] == "Water") {
                if (pNames[i] != "Aqueous") {
                    BoxLib::Abort("No Water in the Aqueous phase");
                }
                scomp = i;
            }
        }

        if (scomp>=0)
        {
            const BoxArray& BA = mf.boxArray();
            BL_ASSERT(rec->deriveType() == BA[0].ixType());
            int ngrow = mf.nGrow();
            BL_ASSERT(mf.nGrow()<=3); // rock_phi only has this many

            int ncomp = 1; // Just water
            BL_ASSERT(rec->numDerive()==ncomp);
            FillPatchIterator fpi(*this,mf,ngrow,time,State_Type,scomp,ncomp);
            for ( ; fpi.isValid(); ++fpi)
            {
                mf[fpi].copy(fpi(),0,dcomp,ncomp);
                mf[fpi].mult((*rock_phi)[fpi],0,dcomp,ncomp);
            }
        }            
        else {
            BoxLib::Abort("PorousMedia::derive: cannot derive Volumetric_Water_Content");
        }
    }
    else if (name=="Aqueous_Saturation") {

        // Sum all components in the Aqueous phase
        // FIXME: Assumes one comp per phase
        int scomp = -1;
        int naq = 0;
        for (int ip=0; ip<pNames.size(); ++ip) {
            if (pNames[ip] == "Aqueous") {
                scomp = ip;
                naq++;
            }
        }

        if (naq==1)
        {
            const BoxArray& BA = mf.boxArray();
            BL_ASSERT(rec->deriveType() == BA[0].ixType());
            int ngrow = mf.nGrow();
            BL_ASSERT(mf.nGrow()<=1); // state only has this many

            int ncomp = 1; // Just aqueous
            BL_ASSERT(rec->numDerive()==ncomp);
            FillPatchIterator fpi(*this,mf,ngrow,time,State_Type,scomp,ncomp);
            for ( ; fpi.isValid(); ++fpi)
            {
                mf[fpi].copy(fpi(),0,dcomp,ncomp);
            }
        }            
        else {
            BoxLib::Abort("PorousMedia::derive: no support for more than one Aqueous component");
        }
    }
    else if (name=="Aqueous_Pressure") {

        // The pressure field is the Aqueous pressure in atm
        // (assumes nphase==1,2) 
        int ncomp = 1;
        int ngrow = mf.nGrow();
        AmrLevel::derive("pressure",time,mf,dcomp);
        mf.mult(BL_ONEATM,dcomp,ncomp,ngrow);
        mf.plus(BL_ONEATM,dcomp,ncomp,ngrow);
    }
    else if (name=="Porosity") {
        
        const BoxArray& BA = mf.boxArray();
        BL_ASSERT(rec->deriveType() == BA[0].ixType());
        int ngrow = mf.nGrow();
        int ncomp = 1; // just porosity
        BL_ASSERT(rec->numDerive()==ncomp);
        BL_ASSERT(mf.nGrow()<=3); // rock_phi only has this many
        MultiFab::Copy(mf,*rock_phi,0,dcomp,ncomp,ngrow);

    } else {
        
        AmrLevel::derive(name,time,mf,dcomp);
    }
}

void
PorousMedia::manual_tags_placement (TagBoxArray&    tags,
				    Array<IntVect>& bf_lev)
{
  //
  // Tag inflow and outflow faces for refinement
  // 
  Array<Orientation> Faces;
  const BCRec& p_bc = desc_lst[Press_Type].getBC(0);
  getDirichletFaces(Faces,Press_Type,p_bc);

  if (Faces.size()>0)
    {
      for (int j =0; j<4; ++j)
	{
	  for (int i=0; i<Faces.size(); ++i)
	    {
	      const Orientation& Face = Faces[i];
	      const int oDir = Face.coordDir();
	      const Box& crse_domain = BoxLib::coarsen(geom.Domain(),bf_lev[level]);
	      const int mult = (Face.isLow() ? +1 : -1);

	      
	      // Refine entire boundary if new boxes within grid_tol
	      // from outflow
        
	      const int grid_tol = 2;
	      Box flowBox = Box(BoxLib::adjCell(crse_domain,Face,grid_tol));
	      flowBox.shift(oDir,mult*grid_tol);
	      

	      // Only refine if there are already tagged cells in the region
	      
	      bool hasTags = false;
	      for (MFIter tbi(tags); !hasTags && tbi.isValid(); ++tbi)
		if (tags[tbi].numTags(flowBox) > 0) hasTags = true;

	      ParallelDescriptor::ReduceBoolOr(hasTags);
	      
	      // hack to make sure inlet is always refined.
	      if (hasTags)
		tags.setVal(BoxArray(&flowBox,1),TagBox::SET);
	    }
	}
    }	
}

void
PorousMedia::create_umac_grown (MultiFab* u_mac, MultiFab* u_macG)
{

  // This complicated copy handles the periodic boundary condition properly.

  BL_PROFILE(BL_PROFILE_THIS_NAME() + "::create_umac_grown1()");
  BL_ASSERT(level==0);
	    
  for (int n = 0; n < BL_SPACEDIM; ++n)
    {
      MultiFab u_ghost(u_mac[n].boxArray(),1,1);
      u_ghost.setVal(1.e40);
      u_ghost.copy(u_mac[n]);
      u_ghost.FillBoundary();
      geom.FillPeriodicBoundary(u_ghost);
      for (MFIter mfi(u_macG[n]); mfi.isValid(); ++mfi)
	{
	  u_macG[n][mfi].copy(u_ghost[mfi]);
	}
    }
}

void
PorousMedia::create_umac_grown (MultiFab* u_mac, 
				PArray<MultiFab>& u_mac_crse, 
				MultiFab* u_macG) 
{
  BL_PROFILE(BL_PROFILE_THIS_NAME() + "::create_umac_grown2()");

  BL_ASSERT(level>0);

  const BoxArray& fgrids = grids;
  BoxList         bl     = BoxLib::GetBndryCells(fgrids,1);

  BoxArray f_bnd_ba(bl);

  bl.clear();

  BoxArray c_bnd_ba = BoxArray(f_bnd_ba.size());

  for (int i = 0; i < f_bnd_ba.size(); ++i)
    {
      c_bnd_ba.set(i,Box(f_bnd_ba[i]).coarsen(crse_ratio));
      f_bnd_ba.set(i,Box(c_bnd_ba[i]).refine(crse_ratio));
    }

  for (int n = 0; n < BL_SPACEDIM; ++n)
    {
      //
      // crse_src & fine_src must have same parallel distribution.
      // We'll use the KnapSack distribution for the fine_src_ba.
      // Since fine_src_ba should contain more points, this'll lead
      // to a better distribution.
      //
      BoxArray crse_src_ba(c_bnd_ba);
      BoxArray fine_src_ba(f_bnd_ba);

      crse_src_ba.surroundingNodes(n);
      fine_src_ba.surroundingNodes(n);

      std::vector<long> wgts(fine_src_ba.size());

      for (unsigned int i = 0; i < wgts.size(); i++)
	{
	  wgts[i] = fine_src_ba[i].numPts();
	}
      DistributionMapping dm;
      //
      // This call doesn't invoke the MinimizeCommCosts() stuff.
      // There's very little to gain with these types of coverings
      // of trying to use SFC or anything else.
      // This also guarantees that these DMs won't be put into the
      // cache, as it's not representative of that used for more
      // usual MultiFabs.
      //
      dm.KnapSackProcessorMap(wgts,ParallelDescriptor::NProcs());

      MultiFab crse_src,  fine_src; 

      crse_src.define(crse_src_ba, 1, 0, dm, Fab_allocate);
      fine_src.define(fine_src_ba, 1, 0, dm, Fab_allocate);
	    
      crse_src.setVal(1.e200);
      fine_src.setVal(1.e200);
	
      //
      // We want to fill crse_src from lower level u_mac including u_mac's grow cells.
      // Gotta do it in steps since parallel copy only does valid region.
      //
      const MultiFab& u_macLL = u_mac_crse[n];
	  
      BoxArray edge_grids = u_macLL.boxArray();
      edge_grids.grow(1);
      
      MultiFab u_macC(edge_grids,1,0);
      
      for (MFIter mfi(u_macLL); mfi.isValid(); ++mfi)
	u_macC[mfi].copy(u_macLL[mfi]);

      crse_src.copy(u_macC);
      
      for (MFIter mfi(crse_src); mfi.isValid(); ++mfi)
	{
	  const int  nComp = 1;
	  const Box& box   = crse_src[mfi].box();
	  const int* rat   = crse_ratio.getVect();
	  FORT_PC_CF_EDGE_INTERP(box.loVect(), box.hiVect(), &nComp, rat, &n,
				 crse_src[mfi].dataPtr(),
				 ARLIM(crse_src[mfi].loVect()),
				 ARLIM(crse_src[mfi].hiVect()),
				 fine_src[mfi].dataPtr(),
				 ARLIM(fine_src[mfi].loVect()),
				 ARLIM(fine_src[mfi].hiVect()));
	}
      crse_src.clear();
      //
      // Replace pc-interpd fine data with preferred u_mac data at
      // this level u_mac valid only on surrounding faces of valid
      // region - this op will not fill grow region.
      //
      fine_src.copy(u_mac[n]);

      for (MFIter mfi(fine_src); mfi.isValid(); ++mfi)
	{
	  //
	  // Interpolate unfilled grow cells using best data from
	  // surrounding faces of valid region, and pc-interpd data
	  // on fine edges overlaying coarse edges.
	  //
	  const int  nComp = 1;
	  const Box& fbox  = fine_src[mfi.index()].box(); 
	  const int* rat   = crse_ratio.getVect();
	  FORT_EDGE_INTERP(fbox.loVect(), fbox.hiVect(), &nComp, rat, &n,
			   fine_src[mfi].dataPtr(),
			   ARLIM(fine_src[mfi].loVect()),
			   ARLIM(fine_src[mfi].hiVect()));
	  
	}

      // This complicated copy handles the periodic boundary condition properly.
      MultiFab u_ghost(u_mac[n].boxArray(),1,1);
      u_ghost.setVal(1.e40);
      u_ghost.copy(u_mac[n]);     
      u_ghost.FillBoundary();
      geom.FillPeriodicBoundary(u_ghost);
      for (MFIter mfi(u_macG[n]); mfi.isValid(); ++mfi)
	{
	  u_macG[n][mfi].copy(u_ghost[mfi]);
	}
      u_macG[n].copy(fine_src);
    }
}

void
PorousMedia::GetCrseUmac(PArray<MultiFab>& u_mac_crse,
                         Real              time          ) const
{
  BL_ASSERT(level>0);
  BL_ASSERT(u_mac_crse.size() == BL_SPACEDIM);

  const PorousMedia* pm = dynamic_cast<const PorousMedia*>(&parent->getLevel(level-1));

  Real t_old = pm->state[State_Type].prevTime();
  Real t_new = pm->state[State_Type].curTime(); 
  Real alpha = (time - t_old)/(t_new - t_old);
  const Geometry& cgeom  = parent->Geom(level-1);
  for (int i=0; i<BL_SPACEDIM; ++i)
    {
      BL_ASSERT(!u_mac_crse.defined(i));
      const BoxArray eba = BoxArray(pm->boxArray()).surroundingNodes(i);
   
      u_mac_crse.set(i,new MultiFab(eba, 1, 1));

      // This complicated copy is to ensure we copy the boundary
      // data of the coarse grid to ensure periodic boundary
      // condition is correct.
      BoxArray edge_grids = u_mac_crse[i].boxArray();
      edge_grids.grow(1);
      MultiFab u_macC(edge_grids,1,0);
      MultiFab u_macD(edge_grids,1,0);
      MultiFab u_macE(eba,1,1);
      for (MFIter mfi(u_mac_crse[i]); mfi.isValid(); ++mfi)
	{
	  u_macC[mfi].copy(pm->u_macG_prev[i][mfi]);
	  Real omalpha = 1.0 - alpha;
	  u_macC[mfi].mult(omalpha);
 
	  u_macD[mfi].copy(pm->u_macG_curr[i][mfi]);
	  u_macD[mfi].mult(alpha);
	}
      for (MFIter mfi(u_macC); mfi.isValid(); ++mfi)
	{
	  u_mac_crse[i][mfi].copy(u_macC[mfi]);
	  u_macE[mfi].copy(u_macD[mfi]);
	}
      MultiFab::Add(u_mac_crse[i],u_macE,0,0,1,1);

      //        FArrayBox UmacCrseTemp;
      //         for (MFIter mfi(u_mac_crse[i]); mfi.isValid(); ++mfi)
      //         {
      //             UmacCrseTemp.resize(mfi.validbox(),1);

      //             UmacCrseTemp.copy(pm->u_macG_prev[i][mfi]);
      //             Real omalpha = 1.0 - alpha;
      //             UmacCrseTemp.mult(omalpha);

      //             u_mac_crse[i][mfi].copy(pm->u_macG_curr[i][mfi]);
      //             u_mac_crse[i][mfi].mult(alpha);
      //             u_mac_crse[i][mfi].plus(UmacCrseTemp);
      //         }
      u_mac_crse[i].FillBoundary();
      cgeom.FillPeriodicBoundary(u_mac_crse[i],false);
    }
}

void
PorousMedia::GetCrsePressure (MultiFab& phi_crse,
                              Real      time      ) const
{
  if (level==0) return;

  const PorousMedia* pm = dynamic_cast<const PorousMedia*>(&parent->getLevel(level-1));
    
  Real t_old = pm->state[Press_Type].prevTime();
  Real t_new = pm->state[Press_Type].curTime();
  Real alpha = (time - t_old)/(t_new - t_old);
  const Geometry& cgeom  = parent->Geom(level-1);
    
  phi_crse.clear();
  phi_crse.define(pm->boxArray(), 1, 1, Fab_allocate); 

  // BUT NOTE we don't trust phi's ghost cells.
  FArrayBox PhiCrseTemp;

  if (std::fabs(time-t_new)<1.e-10 ) {
    const MultiFab& P_crse_new = pm->get_new_data(Press_Type);
    //MultiFab::Copy(phi_crse,P_crse_new,0,0,1,1);
    for (MFIter mfi(phi_crse); mfi.isValid(); ++mfi)
      phi_crse[mfi].copy(P_crse_new[mfi]);
      
  } 
  else if (std::fabs(time- t_old)<1.e-10) 
    {
      const MultiFab& P_crse_old = pm->get_old_data(Press_Type);
      //MultiFab::Copy(phi_crse,P_crse_old,0,0,1,1);
      for (MFIter mfi(phi_crse); mfi.isValid(); ++mfi)
	phi_crse[mfi].copy(P_crse_old[mfi]);
    
    } 
  else 
    {
      const MultiFab& P_crse_old = pm->get_old_data(Press_Type);
      const MultiFab& P_crse_new = pm->get_new_data(Press_Type);
      for (MFIter mfi(phi_crse); mfi.isValid(); ++mfi)
	{
	  PhiCrseTemp.resize(phi_crse[mfi].box(),1);

	  PhiCrseTemp.copy(P_crse_old[mfi]);
	  Real omalpha = 1.0 - alpha;
	  PhiCrseTemp.mult(omalpha);

	  phi_crse[mfi].copy(P_crse_new[mfi]);
	  phi_crse[mfi].mult(alpha);
	  phi_crse[mfi].plus(PhiCrseTemp);
	 
	}
    }

  phi_crse.FillBoundary();
  cgeom.FillPeriodicBoundary(phi_crse,true);
}

// ============
// IO Functions
// ============

void
PorousMedia::fill_from_plotfile (MultiFab&          mf,
                                 int                dcomp,
                                 const std::string& pltfile,
                                 const std::string& varname)
{
  const Real strt_time = ParallelDescriptor::second();

  if (pltfile.empty())
    BoxLib::Abort("fill_from_plotfile(): pltfile not specified");

  if (varname.empty())
    BoxLib::Abort("fill_from_plotfile(): varname not specified");

  if (verbose && ParallelDescriptor::IOProcessor())
    std::cout << "fill_from_plotfile(): reading data from: " << pltfile << '\n';

  DataServices::SetBatchMode();
  Amrvis::FileType fileType(Amrvis::NEWPLT);
  DataServices dataServices(pltfile, fileType);

  if (!dataServices.AmrDataOk())
    //
    // This calls ParallelDescriptor::EndParallel() and exit()
    //
    DataServices::Dispatch(DataServices::ExitRequest, NULL);
    
  AmrData&           amrData   = dataServices.AmrDataRef();
  Array<std::string> plotnames = amrData.PlotVarNames();

  if (amrData.FinestLevel() < level)
    BoxLib::Abort("fill_from_plotfile(): not enough levels in plotfile");

  if (amrData.ProbDomain()[level] != Domain())
    BoxLib::Abort("fill_from_plotfile(): problem domains do not match");

  int idx = -1;
  for (int i = 0; i < plotnames.size(); ++i)
    if (plotnames[i] == varname) idx = i;

  if (idx == -1)
    {
      std::string msg = "fill_from_plotfile(): could not find '";
      msg += varname;
      msg += "' in the plotfile";
      BoxLib::Abort(msg.c_str());
    }

  amrData.FillVar(mf, level, varname, dcomp);
  amrData.FlushGrids(idx);

  if (verbose && ParallelDescriptor::IOProcessor())
    std::cout << "fill_from_plotfile(): finished init from plotfile" << '\n';

  if (verbose)
  {
    const int IOProc   = ParallelDescriptor::IOProcessorNumber();
    Real      run_time = ParallelDescriptor::second() - strt_time;

    ParallelDescriptor::ReduceRealMax(run_time,IOProc);

    if (ParallelDescriptor::IOProcessor())
        std::cout << "PorousMedia::fill_from_plotfile(): lev: "
                  << level
                  << ", time: " << run_time << '\n';
  }
}

void
PorousMedia::checkPoint (const std::string& dir,
                         std::ostream&  os,
                         VisMF::How     how,
                         bool           dump_old)
{
  AmrLevel::checkPoint(dir,os,how,dump_old);

  std::string Level = BoxLib::Concatenate("Level_", level, 1);
  std::string uxfile = "/umac_x";
  std::string uyfile = "/umac_y";
  std::string FullPath = dir;
  if (!FullPath.empty() && FullPath[FullPath.length()-1] != '/')
    {
      FullPath += '/';
    }
  FullPath += Level;
  uxfile = FullPath + uxfile;
  uyfile = FullPath + uyfile;
  VisMF::Write(u_mac_curr[0], uxfile);
  VisMF::Write(u_mac_curr[1], uyfile);

  std::string utxfile = "/umact_x";
  std::string utyfile = "/umact_y";
  utxfile = FullPath + utxfile;
  utyfile = FullPath + utyfile;
  VisMF::Write(u_macG_trac[0], utxfile);
  VisMF::Write(u_macG_trac[1], utyfile);

#if (BL_SPACEDIM == 3)
  std::string uzfile = "/umac_z";
  uzfile = FullPath + uzfile;
  VisMF::Write(u_mac_curr[2], uzfile);
  std::string utzfile = "/umact_z";
  utzfile = FullPath + utzfile;
  VisMF::Write(u_macG_trac[2], utzfile);
#endif 

#ifdef MG_USE_FBOXLIB
  if (model != model_list["richard"])
    {
      std::string rxfile = "/rhs_RhoD_x";
      std::string ryfile = "/rhs_RhoD_y";
      rxfile = FullPath + rxfile;
      ryfile = FullPath + ryfile;
      VisMF::Write(rhs_RhoD[0], rxfile);
      VisMF::Write(rhs_RhoD[1], ryfile);
#if (BL_SPACEDIM == 3)
      std::string rzfile = "/rhs_RhoD_z";
      rzfile = FullPath + rzfile;
      VisMF::Write(rhs_RhoD[2], rzfile);
#endif 
    }
#endif

  os << dt_eig << '\n';
}

// =================
// Utility functions
// =================

void 
PorousMedia::check_sum()
{
  // gathering some statistics of the solutions.

  Real minmax[2] = {1,1};

  MultiFab& S_new = get_new_data(State_Type);
  FArrayBox tmp,tmp2;

  for (MFIter mfi(S_new);mfi.isValid();++mfi) 
    {
      tmp.resize(mfi.validbox(),1);
      tmp2.resize(mfi.validbox(),1);
      tmp.setVal(0);
      tmp2.setVal(0);
    
      for (int kk=0; kk < ncomps; kk++)
	{
	  if (solid.compare(pNames[pType[kk]]) != 0) {
	    tmp2.copy(S_new[mfi],mfi.validbox(),kk,mfi.validbox(),0,1);
	    tmp2.mult(1.0/density[kk]);
	    tmp.plus(tmp2,mfi.validbox(),0,0,1);
	  }
	}
      minmax[0] = std::min(minmax[0],tmp.min(mfi.validbox(),0));
      minmax[1] = std::max(minmax[1],tmp.max(mfi.validbox(),0));
    }
    
  const int IOProc = ParallelDescriptor::IOProcessorNumber();

  ParallelDescriptor::ReduceRealMax(&minmax[0],2,IOProc);

  if (verbose && ParallelDescriptor::IOProcessor())
    {
      std::cout << "   SUM SATURATION MAX/MIN = " 
		<< minmax[1] << ' ' << minmax[0] << '\n';
    }
}

void 
PorousMedia::check_minmax()
{
  MultiFab* rho;
  MultiFab& S_new = get_new_data(State_Type);
  
  rho = new MultiFab(grids,1,0);
  MultiFab::Copy(*rho,S_new,0,0,1,0);

  for (int kk = 1; kk<ncomps; kk++)
    {
      if (solid.compare(pNames[pType[kk]]) != 0) 
	MultiFab::Add(*rho,S_new,kk,0,1,0);
    }
 
  Array<Real> smin(ncomps,1.e20), smax(ncomps,-1.e20);

  for (int kk = 0; kk < ncomps; kk++)
    {
      for (MFIter mfi(S_new); mfi.isValid(); ++mfi)
	{
	  smax[kk] = std::max(smax[kk],S_new[mfi].max(mfi.validbox(),kk));
	  smin[kk] = std::min(smin[kk],S_new[mfi].min(mfi.validbox(),kk));
	}
    }
  const int IOProc = ParallelDescriptor::IOProcessorNumber();

  ParallelDescriptor::ReduceRealMax(smax.dataPtr(), ncomps, IOProc);
  ParallelDescriptor::ReduceRealMin(smin.dataPtr(), ncomps, IOProc);
  
  if (verbose && ParallelDescriptor::IOProcessor())
    {
      for (int kk = 0; kk < ncomps; kk++)
	{
	  std::cout << "   SNEW MAX/MIN OF COMP " << kk
		    << ' ' << smax[kk] << "  " << smin[kk] << '\n';
	}
    }

  Real rhomaxmin[2] = {-1.e20,+1.e20};
  for (MFIter mfi(*rho); mfi.isValid(); ++mfi)
    {
      rhomaxmin[0] = std::max(rhomaxmin[0],(*rho)[mfi].max(mfi.validbox(),0));
      rhomaxmin[1] = std::min(rhomaxmin[1],(*rho)[mfi].min(mfi.validbox(),0));
    }

  ParallelDescriptor::ReduceRealMax(&rhomaxmin[0], 1, IOProc);
  ParallelDescriptor::ReduceRealMin(&rhomaxmin[1], 1, IOProc);

  if (verbose && ParallelDescriptor::IOProcessor())
    {  
      std::cout << "   RHO MAX/MIN "
		<< ' ' << rhomaxmin[0] << "  " << rhomaxmin[1] << '\n';
    }

  delete rho;
}

void 
PorousMedia::check_minmax(int fscalar, int lscalar)
{
  MultiFab& S_new = get_new_data(State_Type);
  
  const int nscal = lscalar - fscalar + 1;

  Array<Real> smin(nscal,1.e20), smax(nscal,-1.e20);

  for (int kk = 0; kk < nscal; kk++)
    {
      for (MFIter mfi(S_new); mfi.isValid(); ++mfi)
	{
            smax[kk] = std::max(smax[kk], S_new[mfi].max(mfi.validbox(),fscalar+kk));
            smin[kk] = std::min(smin[kk], S_new[mfi].min(mfi.validbox(),fscalar+kk));
	}
    }
  const int IOProc = ParallelDescriptor::IOProcessorNumber();
  ParallelDescriptor::ReduceRealMax(smax.dataPtr(), nscal, IOProc);
  ParallelDescriptor::ReduceRealMin(smin.dataPtr(), nscal, IOProc);
  
  if (verbose && ParallelDescriptor::IOProcessor())
    {
        for (int kk = 0; kk < nscal; kk++)
	{
	  std::cout << "   SNEW MAX/MIN OF COMP "
                    << fscalar+kk
		    << ' ' << smax[kk] 
		    << ' ' << smin[kk] << '\n';
	}
    }
}

void 
PorousMedia::check_minmax(MultiFab& mf)
{
  const int ncomp = mf.nComp();
  Array<Real> smin(ncomp,1.e20), smax(ncomp,-1.e20);

  for (int kk = 0; kk < ncomp; kk++)
    {
      for (MFIter mfi(mf); mfi.isValid(); ++mfi)
	{
	  smax[kk] = std::max(smax[kk],mf[mfi].max(mfi.validbox(),kk));
	  smin[kk] = std::min(smin[kk],mf[mfi].min(mfi.validbox(),kk));
	}
    }
  const int IOProc = ParallelDescriptor::IOProcessorNumber();

  ParallelDescriptor::ReduceRealMax(smax.dataPtr(), ncomp, IOProc);
  ParallelDescriptor::ReduceRealMin(smin.dataPtr(), ncomp, IOProc);
  
  if (verbose && ParallelDescriptor::IOProcessor())
    {
      for (int kk = 0; kk < ncomp; kk++)
	{
	  std::cout << " MAX/MIN OF MF " << kk
		    << ' ' << smax[kk] << "  " << smin[kk] << '\n';
	}
    }
}

void 
PorousMedia::check_minmax(MultiFab* u_mac)
{
  //
  // Write out the min and max of the MAC velocities.
  //
  Real umax[BL_SPACEDIM] = {D_DECL(-1.e20,-1.e20,-1.e20)};
  Real umin[BL_SPACEDIM] = {D_DECL(+1.e20,+1.e20,+1.e20)};

  for (MFIter mfi(u_mac[0]); mfi.isValid(); ++mfi)
    {
      const int i = mfi.index();

      umax[0] = std::max(umax[0],u_mac[0][i].max(u_mac[0].boxArray()[i]));
      umin[0] = std::min(umin[0],u_mac[0][i].min(u_mac[0].boxArray()[i]));
      umax[1] = std::max(umax[1],u_mac[1][i].max(u_mac[1].boxArray()[i]));
      umin[1] = std::min(umin[1],u_mac[1][i].min(u_mac[1].boxArray()[i]));
#if(BL_SPACEDIM == 3)
      umax[2] = std::max(umax[2],u_mac[2][i].max(u_mac[2].boxArray()[i]));
      umin[2] = std::min(umin[2],u_mac[2][i].min(u_mac[2].boxArray()[i]));
#endif
    }

  const int IOProc = ParallelDescriptor::IOProcessorNumber();

  ParallelDescriptor::ReduceRealMax(&umax[0], BL_SPACEDIM, IOProc);
  ParallelDescriptor::ReduceRealMin(&umin[0], BL_SPACEDIM, IOProc);

  if (verbose && ParallelDescriptor::IOProcessor())
  {
      D_TERM(std::cout << "   UMAC MAX/MIN  " << umax[0] << "  " << umin[0] << '\n';,
             std::cout << "   VMAC MAX/MIN  " << umax[1] << "  " << umin[1] << '\n';,
             std::cout << "   WMAC MAX/MIN  " << umax[2] << "  " << umin[2] << '\n';);
  }
}

void
PorousMedia::umac_edge_to_cen(MultiFab* u_mac, int idx_type)
{
  // average velocity onto cell center
  MultiFab&  U_cor  = get_new_data(idx_type);
  for (MFIter mfi(U_cor); mfi.isValid(); ++mfi)
    {
      const int* lo     = mfi.validbox().loVect();
      const int* hi     = mfi.validbox().hiVect();
    
      const int* u_lo   = U_cor[mfi].loVect();
      const int* u_hi   = U_cor[mfi].hiVect();
      const Real* udat  = U_cor[mfi].dataPtr();
	  
      const int* um_lo  = (u_mac[0])[mfi].loVect();
      const int* um_hi  = (u_mac[0])[mfi].hiVect();
      const Real* umdat = (u_mac[0])[mfi].dataPtr();
	
      const int* vm_lo  = (u_mac[1])[mfi].loVect();
      const int* vm_hi  = (u_mac[1])[mfi].hiVect();
      const Real* vmdat = (u_mac[1])[mfi].dataPtr();
	
#if (BL_SPACEDIM == 3)
      const int* wm_lo  = (u_mac[2])[mfi].loVect();
      const int* wm_hi  = (u_mac[2])[mfi].hiVect();
      const Real* wmdat = (u_mac[2])[mfi].dataPtr();
#endif

      FORT_AVG_UMAC(umdat,ARLIM(um_lo),ARLIM(um_hi),
		    vmdat,ARLIM(vm_lo),ARLIM(vm_hi),
#if (BL_SPACEDIM == 3)
		    wmdat,ARLIM(wm_lo),ARLIM(wm_hi),
#endif
		    udat ,ARLIM( u_lo),ARLIM( u_hi),lo,hi);       
    }
}

void
PorousMedia::umac_cpy_edge_to_cen(MultiFab* u_mac, int idx_type, int ishift)
{
  // average velocity onto cell center
  MultiFab&  U_cor  = get_new_data(idx_type);
  for (MFIter mfi(U_cor); mfi.isValid(); ++mfi)
    {
      const int* lo     = mfi.validbox().loVect();
      const int* hi     = mfi.validbox().hiVect();
    
      const int* u_lo   = U_cor[mfi].loVect();
      const int* u_hi   = U_cor[mfi].hiVect();
      const Real* udat  = U_cor[mfi].dataPtr();
	  
      const int* um_lo  = (u_mac[0])[mfi].loVect();
      const int* um_hi  = (u_mac[0])[mfi].hiVect();
      const Real* umdat = (u_mac[0])[mfi].dataPtr();
	
      const int* vm_lo  = (u_mac[1])[mfi].loVect();
      const int* vm_hi  = (u_mac[1])[mfi].hiVect();
      const Real* vmdat = (u_mac[1])[mfi].dataPtr();
	
#if (BL_SPACEDIM == 3)
      const int* wm_lo  = (u_mac[2])[mfi].loVect();
      const int* wm_hi  = (u_mac[2])[mfi].hiVect();
      const Real* wmdat = (u_mac[2])[mfi].dataPtr();
#endif

      FORT_CPY_UMAC(umdat,ARLIM(um_lo),ARLIM(um_hi),
		    vmdat,ARLIM(vm_lo),ARLIM(vm_hi),
#if (BL_SPACEDIM == 3)
		    wmdat,ARLIM(wm_lo),ARLIM(wm_hi),
#endif
		    udat ,ARLIM( u_lo),ARLIM( u_hi),lo,hi, &ishift); 
    }
}

void
PorousMedia::compute_divu (MultiFab& soln,
			   MultiFab* umac)
{
  //
  // This compute the divergence of umac
  //

  const Real* dx   = geom.CellSize();

  for (MFIter fpi(soln); fpi.isValid(); ++fpi)
    {
      const int i = fpi.index();
      const int* lo = fpi.validbox().loVect();
      const int* hi = fpi.validbox().hiVect();

      const Real* sdat = soln[i].dataPtr();
      const int* s_lo  = soln[i].loVect();
      const int* s_hi  = soln[i].hiVect();
    
      const Real* uxdat = umac[0][i].dataPtr();
      const int*  uxlo  = umac[0][i].loVect();
      const int*  uxhi  = umac[0][i].hiVect();

      const Real* uydat = umac[1][i].dataPtr();
      const int*  uylo  = umac[1][i].loVect();
      const int*  uyhi  = umac[1][i].hiVect();

#if (BL_SPACEDIM == 3)
      const Real* uzdat = umac[2][i].dataPtr();
      const int*  uzlo  = umac[2][i].loVect();
      const int*  uzhi  = umac[2][i].hiVect();
#endif

      FORT_DIV_UMAC (sdat, ARLIM(s_lo),ARLIM(s_hi),
		     uxdat,ARLIM(uxlo),ARLIM(uxhi),
		     uydat,ARLIM(uylo),ARLIM(uyhi),
#if (BL_SPACEDIM == 3)
		     uzdat,ARLIM(uzlo),ARLIM(uzhi),
#endif
		     lo,hi,dx);
    }
}
