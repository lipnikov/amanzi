
#include <fstream>
#include <iomanip>

#include <GSLibInt.H>
#include <RockManager.H>
#include <RockManager_F.H>
#include <WritePlotfile.H>

#include <Utility.H>
#include <ParmParse.H>

static std::string CapillaryPressureName    = "capillary_pressure";
static std::string PorosityName             = "porosity";
static std::string PermeabilityName         = "permeability";
static std::string RelativePermeabilityName = "relative_permeability";

static std::string CP_model_None = "None";
static std::string CP_model_vG = "VanGenuchten";
static std::string CP_model_BC = "BrooksCorey";

static std::string Kr_model_None = "None";
static std::string Kr_model_Mualem = "Mualem";
static std::string Kr_model_Burdine = "Burdine";

static std::string Kr_model_vG_Mualem  = CP_model_vG + "_" + Kr_model_Mualem;
static std::string Kr_model_vG_Burdine = CP_model_vG + "_" + Kr_model_Burdine;
static std::string Kr_model_BC_Mualem  = CP_model_BC + "_" + Kr_model_Mualem;
static std::string Kr_model_BC_Burdine = CP_model_BC + "_" + Kr_model_Burdine;

static int MAX_CPL_PARAMS = 7; // Must be set to accommodate the model with the most parameters
static int CPL_MODEL_ID = 0;

static int VG_M                   = 1;
static int VG_ALPHA               = 2;
static int VG_SR                  = 3;
static int VG_ELL                 = 4;
static int VG_KR_MODEL_ID         = 5;
static int VG_KR_SMOOTHING_MAX_PC = 6;

static int BC_LAMBDA              = 1;
static int BC_ALPHA               = 2;
static int BC_SR                  = 3;
static int BC_ELL                 = 4;
static int BC_KR_MODEL_ID         = 5;
static int BC_KR_SMOOTHING_MAX_PC = 6;

static Real Kr_ell_vG_Mualem_DEF = 0.5;
static Real Kr_ell_vG_Burdine_DEF = 2.0;

static Real Kr_smoothing_max_pcap_DEF = -1;
static Real Kr_smoothing_min_seff_DEF = 2;

// Interpolators
static int NUM_INIT_INTERP_EVAL_PTS_DEF = 5001;
static Real pc_at_Sr = 1.e11;

static int crse_init_factor = 32;
static int max_grid_fine_gen = 32;

static std::string MaterialPlotFileVersion = "MaterialPlotFile-V1.0";

static int Rock_Mgr_ID_ctr=0;
static std::vector<RockManager*> Rock_Mgr_Ptrs;
static std::vector<std::pair<bool,Real> > Kr_smoothing_min_seff; // Bool says whether value needs to be updated

RockManager::RockManager(const RegionManager*   _region_manager,
                         const Array<Geometry>& geomArray,
                         const Array<IntVect>&  refRatio,
                         int                    _nGrow)
  : region_manager(_region_manager), nGrow(_nGrow), interps_built(false),
    dataServices_phi(0) 
{
  Initialize(geomArray,refRatio);
  BuildInterpolators();

  rock_mgr_ID = Rock_Mgr_ID_ctr++;
  Rock_Mgr_Ptrs.resize(rock_mgr_ID+1); Rock_Mgr_Ptrs[rock_mgr_ID] = this;
}

extern "C" {
  void ROCK_MANAGER_PCAP(int* rockMgrID, const Real* saturation, int* matID, Real* time, Real* capillaryPressure, int* npts) {
    Rock_Mgr_Ptrs[*rockMgrID]->CapillaryPressure(saturation,matID,*time,capillaryPressure,*npts);
  }

  void ROCK_MANAGER_INVPCAP(int* rockMgrID, const Real* capillaryPressure, int* matID, Real* time, Real* saturation, int* npts) {
    Rock_Mgr_Ptrs[*rockMgrID]->InverseCapillaryPressure(capillaryPressure,matID,*time,saturation,*npts);
  }

  void ROCK_MANAGER_RELPERM(int* rockMgrID, const Real* saturation, int* matID, Real* time, Real* kappa, int* npts) {
    Rock_Mgr_Ptrs[*rockMgrID]->RelativePermeability(saturation,matID,*time,kappa,*npts);
  }

  void ROCK_MANAGER_DSDPCAP(int* rockMgrID, const Real* saturation, int* matID, Real* time, Real* dsdpc, int* npts) {
    Rock_Mgr_Ptrs[*rockMgrID]->DInverseCapillaryPressure(saturation,matID,*time,dsdpc,*npts);
  }

  void ROCK_MANAGER_RESIDSAT(int* rockMgrID, int* matID, Real* time, Real* Sr, int* npts) {
    Rock_Mgr_Ptrs[*rockMgrID]->ResidualSaturation(matID,*time,Sr,*npts);
  }
}

int
RockManager::NComp(const std::string& property_name) const
{
  int nc = 0;
  if (materialFiller->CanDerive(property_name)) {
    nc = materialFiller->NComp(property_name);
  }
  return nc;
}

static
void
EnsureFolderExists(const std::string& full_path)
{
  // Find folder name first, and ensure folder exists
  // FIXME: Will fail on Windows
  const std::vector<std::string>& tokens = BoxLib::Tokenize(full_path,"/");
  std::string dir = (full_path[0] == '/' ? "/" : "");
  for (int i=0; i<tokens.size()-1; ++i) {
    dir += tokens[i];
    if (i<tokens.size()-2) dir += "/";
  }
  
  if(!BoxLib::FileExists(dir)) {
    if ( ! BoxLib::UtilCreateDirectory(dir, 0755)) {
      BoxLib::CreateDirectoryFailed(dir);
    }
  }
}

void
RockManager::BuildInterpolators()
{
  CP_s_interps.resize(rock.size(),PArrayManage);

  int nComp = materialFiller->NComp(CapillaryPressureName);
  static IntVect iv(D_DECL(0,0,0));
  static Box bx(iv,iv);
  FArrayBox pc_params(bx,nComp);
  int level=0; //not really used
  int dComp=0;
  Real time = 0;

  for (int n=0; n<rock.size(); ++n) {
    int Npts = WRM_plot_file[n].first;
    if (ParallelDescriptor::IOProcessor() && Npts > 0) {
      Array<Real> s(Npts);
      Array<Real> pc(Npts);
      Array<int> mat(Npts,n);

      pc[0] = pc_at_Sr;
      InverseCapillaryPressure(pc.dataPtr(),&n,time,&(s[0]),1);

      Real ds = 1 - s[0];
      for (int i=1; i<s.size(); ++i) {
        s[i] = std::max(s[0], std::min(1.0, s[0] + ds*Real(i)/(Npts - 1)));
      }
      CapillaryPressure(s.dataPtr(),mat.dataPtr(),time,pc.dataPtr(),Npts);

      Array<Real> kr(s.size());
      RelativePermeability(s.dataPtr(),mat.dataPtr(),time,kr.dataPtr(),Npts);

      const std::string& file = WRM_plot_file[n].second;
      std::cout << "Writing WRM data for material \"" << rock[n].Name()
                << "\" to file \"" << file << "\"" << std::endl;

      EnsureFolderExists(file);
      std::ofstream osf; osf.open(file.c_str());
      osf << std::setprecision(15);
      for (int i=1; i<s.size(); ++i) {
        osf << s[i] << " " << pc[i] << " " << kr[i] << std::endl;
      }
      osf.close();
    }
  }
#if 0
  for (int n=0; n<rock.size(); ++n) {

    Array<Real> s(NUM_INIT_INTERP_EVAL_PTS_DEF);
    int Npts = s.size();
    Array<Real> pc(Npts);
    Array<int> mat(Npts,n);

    pc[0] = pc_at_Sr;
    InverseCapillaryPressure(pc.dataPtr(),&n,time,&(s[0]),1);

    Real ds = 1 - s[0];
    for (int i=1; i<s.size(); ++i) {
      s[i] = s[0] + ds*Real(i)/(NUM_INIT_INTERP_EVAL_PTS_DEF - 1);
    }
    CapillaryPressure(s.dataPtr(),mat.dataPtr(),time,pc.dataPtr(),Npts);
    CP_s_interps.set(n, new MonotCubicInterpolator(std::vector<Real>(s),std::vector<Real>(pc)));

    Array<Real> kr(s.size());
    RelativePermeability(s.dataPtr(),mat.dataPtr(),time,kr.dataPtr(),Npts);
    Kr_s_interps.set(n, new MonotCubicInterpolator(std::vector<Real>(s),std::vector<Real>(kr)));
  }
#endif
  interps_built = true;
}

void
RockManager::BuildGSLibFile(Real                   avg,
                            Real                   std,
                            const std::string&     gslib_param_file,
                            const std::string&     gslib_data_file,
                            const Array<Real>&     gslib_file_shift,
                            const Array<Geometry>& geom_array,
                            const Array<IntVect>&  ref_ratio,
                            int                    num_grow,
                            int                    max_grid_size_fine_gen,
                            Property::CoarsenRule& crule,
                            const std::string&     varname)
{
  int nLev = geom_array.size();
  int finest_level = nLev - 1;
  const Geometry& geom = geom_array[finest_level];
  Box stat_box = Box(geom.Domain());

  if(!BoxLib::FileExists(gslib_param_file)) {
    const std::string& str = "GSLib parameter file: \"" + gslib_param_file + "\" does not exist";
    BoxLib::Abort(str.c_str());
  }

  // Original interface supports layered structure, we disable that for now
  Array<Real> avgVals(1,avg);
  Array<int> n_cell(BL_SPACEDIM);
  const Geometry& geom0 = geom_array[0];
  for (int d=0; d<BL_SPACEDIM; ++d) {
    n_cell[d] = geom0.Domain().length(d);
  }

  // Find cummulative refinement ratio
  int twoexp = 1;
  for (int i = 1; i<nLev; i++) {
    twoexp *= ref_ratio[i-1][0];  // FIXME: Assumes uniform refinement
  }

  PArray<MultiFab> stat(nLev,PArrayManage);
  BoxArray stat_ba(stat_box);
  stat_ba.maxSize(max_grid_size_fine_gen);
  int ng_cum = num_grow * twoexp;
  stat.set(finest_level, new MultiFab(stat_ba,1,ng_cum));

  const Array<Real> prob_lo(geom0.ProbLo(),BL_SPACEDIM);
  const Array<Real> prob_hi(geom0.ProbHi(),BL_SPACEDIM);
  
  GSLibInt::rdpGaussianSim(avgVals,std,n_cell,prob_lo,prob_hi,twoexp,stat[finest_level],
                           crse_init_factor,max_grid_size_fine_gen,ng_cum,gslib_param_file);

  for (int lev=finest_level-1; lev>=0; --lev) {
    int ltwoexp = 1;
    for (int i = 1; i<lev; i++) {
      ltwoexp *= ref_ratio[i-1][0];  // FIXME: Assumes uniform refinement
    }

    const Box& domain = geom_array[lev].Domain();
    BoxArray ba(domain);
    ba.maxSize(max_grid_size_fine_gen / ref_ratio[lev][0]); // FIXME: Assumes uniform refinement
    stat.set(lev, new MultiFab(ba,1,num_grow*ltwoexp));

    BoxArray baf = BoxArray(ba).refine(ref_ratio[lev]);
    MultiFab fine(baf,1,stat[lev].nGrow()*ref_ratio[lev][0]);// FIXME: Assumes uniform refinement
    BoxArray bafg = BoxArray(baf).grow(fine.nGrow());
    MultiFab fineg(bafg,1,0);
    fineg.copy(stat[lev+1]); // parallel copy
    for (MFIter mfi(fine); mfi.isValid(); ++mfi) {
      fine[mfi].copy(fineg[mfi]);
    }
    fineg.clear();

    for (MFIter mfi(fine); mfi.isValid(); ++mfi) {
      const FArrayBox& finefab = fine[mfi];
      FArrayBox& crsefab = stat[lev][mfi];
      const Box& cbox = crsefab.box();
      if ( !(finefab.box().contains(Box(cbox).refine(ref_ratio[lev])) ) ) {
        std::cout << "c,f: " << cbox << " " << finefab.box() << std::endl;
        BoxLib::Abort();
      }
      MatFiller::CoarsenData(fine[mfi],0,stat[lev][mfi],cbox,0,1,ref_ratio[lev],crule);
    }
  }

  EnsureFolderExists(gslib_data_file);
  
  Array<MultiFab*> data(nLev);
  Array<Box> prob_domain(nLev);
  Array<Array<Real> > dx_level(nLev,Array<Real>(BL_SPACEDIM));
  Array<int> int_ref(nLev-1);
  for (int lev=0; lev<nLev; ++lev) {
    data[lev] = &(stat[lev]);
    prob_domain[lev] = geom_array[lev].Domain();
    for (int d=0; d<BL_SPACEDIM; ++d) {
      dx_level[lev][d] = geom_array[lev].CellSize(d);
    }
    if (lev<finest_level) {
      int_ref[lev] = ref_ratio[lev][0];
    }
  }
  Real time=0;
  bool verbose=false;
  Array<Real> vfeps(BL_SPACEDIM,0);
  Array<int> level_steps(nLev,0);
  Array<std::string> varnames(1,varname);
  bool is_cart_grid = false;
  WritePlotfile(MaterialPlotFileVersion,data,time,geom0.ProbLo(),geom0.ProbHi(),int_ref,prob_domain,
                dx_level,geom0.Coord(),gslib_data_file,varnames,verbose,is_cart_grid,vfeps.dataPtr(),
                level_steps.dataPtr());
  ParallelDescriptor::Barrier(); // Wait until everyone finished to avoid reading before completely written
}

// FIXME: Replace with call to MatFiller::FillCellsOutsideDomain
void
RockManager::FillBoundary(Real      time,
                          int       level,
                          MultiFab& mf,
                          int       dComp,
                          int       nComp,
                          int       nGrow) const
{
  const Geometry& geom = materialFiller->Geom(level);

  if (nGrow>0) {
    const Box& domain = geom.Domain();
    for (MFIter mfi(mf); mfi.isValid(); ++mfi) {
      FArrayBox& fab = mf[mfi];
      const Box& vbox = mfi.validbox();
      Box gbox = Box(vbox).grow(nGrow);
      if ( !(domain.contains(gbox)) ) {
        for (int d=0; d<BL_SPACEDIM; ++d) {
          Box adjCellLo = BoxLib::adjCellLo(vbox,d,1);
          Box intCellLo = Box(adjCellLo).shift(d,1);
          for (int i=0; i<nGrow; ++i) {
            Box ladjCellLo = Box(adjCellLo).shift(d,-i);
            for (int dd=0; dd<d; ++dd) {
              ladjCellLo.grow(dd,nGrow);
            }
            Box lintCellLo = Box(ladjCellLo).shift(d,i+1);
            fab.copy(fab,lintCellLo,dComp,ladjCellLo,dComp,1);
          }

          Box adjCellHi = BoxLib::adjCellHi(vbox,d,1);
          Box intCellHi = Box(adjCellHi).shift(d,-1);
          for (int i=0; i<nGrow; ++i) {
            Box ladjCellHi = Box(adjCellHi).shift(d,i);
            for (int dd=0; dd<d; ++dd) {
              ladjCellHi.grow(dd,nGrow);
            }
            Box lintCellHi = Box(ladjCellHi).shift(d,-i-1);
            fab.copy(fab,lintCellHi,dComp,ladjCellHi,dComp,1);
          }
        }
      }
    }
    bool local = false;
    bool corner=true;
    mf.FillBoundary(dComp,nComp,false,!corner);
    materialFiller->Geom(level).FillPeriodicBoundary(mf,dComp,nComp,corner,local);
  }
}

void
RockManager::Porosity(Real      time,
                      int       level,
                      MultiFab& porosity,
                      int       dComp,
                      int       nGrow) const
{
  bool ignore_mixed = true;
  materialFiller->SetProperty(time,level,porosity,PorosityName,dComp,nGrow,0,ignore_mixed);
  int nComp = NComp(PorosityName);
  Array<Real> dummy(nComp);
  for (int i=0; i<rock.size(); ++i) {
    const Property* prop = rock[i].Prop(PorosityName);
    const GSLibProperty* gslib_prop = dynamic_cast<const GSLibProperty*>(prop);
    if (gslib_prop != 0) {
      const AmrData* this_const_amrData = gslib_prop->GetAmrData();
      AmrData* this_amrData = const_cast<AmrData*>(this_const_amrData);

      iMultiFab id(porosity.boxArray(),1,0);
      materialFiller->SetMaterialID(level,id,0,ignore_mixed);
      MultiFab vals(porosity.boxArray(),nComp,0);
      this_amrData->FillVar(vals,level,PorosityName,dComp);
      for (MFIter mfi(porosity); mfi.isValid(); ++mfi) {
        Box bx = Box(mfi.validbox()).grow(nGrow);
        const FArrayBox& vfab = vals[mfi];
        const IArrayBox& idfab = id[mfi];
        FArrayBox& mfab = porosity[mfi];
        FORT_FILLPMAT (mfab.dataPtr(), ARLIM(mfab.loVect()),  ARLIM(mfab.hiVect()),
                       idfab.dataPtr(), ARLIM(idfab.loVect()), ARLIM(idfab.hiVect()),
                       vfab.dataPtr(),  ARLIM(vfab.loVect()),  ARLIM(idfab.hiVect()),
                       &i, bx.loVect(), bx.hiVect(), &nComp);        
      }
    }
  }
  FillBoundary(time,level,porosity,dComp,1,nGrow);
}

void
RockManager::Initialize(const Array<Geometry>& geomArray,
                        const Array<IntVect>&  refRatio)
{
  is_saturated = false;
  is_diffusive = false;
  tensor_diffusion = false;

  static int CP_cnt = 0;
  CP_models[CP_model_None] = CP_cnt++;
  CP_models[CP_model_vG] = CP_cnt++;
  CP_models[CP_model_BC] = CP_cnt++;

  static int Kr_cnt = 0;
  Kr_models[Kr_model_None] = Kr_cnt++;
  Kr_models[Kr_model_vG_Mualem] = Kr_cnt++;
  Kr_models[Kr_model_vG_Burdine] = Kr_cnt++;
  Kr_models[Kr_model_BC_Mualem] = Kr_cnt++;
  Kr_models[Kr_model_BC_Burdine] = Kr_cnt++;

  ParmParse pp("rock");
  int nrock = pp.countval("rock");
  if (nrock <= 0) {
    BoxLib::Abort("At least one rock type must be defined.");
  }
  Array<std::string> r_names;  pp.getarr("rock",r_names,0,nrock);

  rock.clear();
  rock.resize(nrock,PArrayManage);
  Array<std::string> material_regions;

  // Scan rock for properties that must be defined for all
  //   if defined for one. 
  bool user_specified_molecular_diffusion_coefficient = false;
  bool user_specified_dispersivity = false;
  bool user_specified_tortuosity = false;
  bool user_specified_specific_storage = false;
  for (int i = 0; i<nrock; i++) {
    const std::string& rname = r_names[i];
    const std::string prefix("rock." + rname);
    ParmParse ppr(prefix.c_str());
    user_specified_molecular_diffusion_coefficient = ppr.countval("molecular_diffusion.val");
    user_specified_tortuosity = ppr.countval("tortuosity.val");
    user_specified_dispersivity = ppr.countval("dispersivity.alphaL");
    user_specified_specific_storage = ppr.countval("specific_storage.val");
  }

  if (is_saturated) {
    user_specified_specific_storage = true; // Will use default if not specified
  }

  bool enable_diffusion = is_diffusive 
    && ( user_specified_molecular_diffusion_coefficient || user_specified_dispersivity);
  
  bool enable_tensor_diffusion = enable_diffusion && user_specified_dispersivity;

  // setup static database for smoothing interval
  Kr_smoothing_min_seff.resize(nrock,std::pair<bool,Real>(true,Kr_smoothing_min_seff_DEF));

  // set up static database for WRM plot files
  WRM_plot_file.resize(nrock,std::pair<int,std::string>(0,""));

  for (int i = 0; i<nrock; i++) {

    const std::string& rname = r_names[i];
    const std::string prefix("rock." + rname);
    ParmParse ppr(prefix.c_str());

    bool generate_porosity_gslib_file = false;
        
    static Property::CoarsenRule arith_crsn = Property::Arithmetic;
    static Property::CoarsenRule harm_crsn = Property::ComponentHarmonic;
    static Property::RefineRule pc_refine = Property::PiecewiseConstant;

    Real rdensity = -1; // ppr.get("density",rdensity); // not actually used anywhere

    Real rDmolec = 0;
    Property* Dmolec_func = 0;
    if (user_specified_molecular_diffusion_coefficient) {
      ppr.query("molecular_diffusion.val",rDmolec);
      std::string Dmolec_str = "molecular_diffusion_coefficient";
      Dmolec_func = new ConstantProperty(Dmolec_str,rDmolec,harm_crsn,pc_refine);
    }

    Array<Real> rDispersivity(2,0);
    Property* Dispersivity_func = 0;
    if (user_specified_dispersivity) {
      ppr.query("dispersivity.alphaL",rDispersivity[0]);
      ppr.query("dispersivity.alphaT",rDispersivity[1]);
      std::string Dispersivity_str = "dispersivity";
      Dispersivity_func = new ConstantProperty(Dispersivity_str,rDispersivity,harm_crsn,pc_refine);
    }

    Real rTortuosity = 1;
    Property* Tortuosity_func = 0;
    if (user_specified_tortuosity) {
      ppr.query("tortuosity.val",rTortuosity);
      std::string Tortuosity_str = "tortuosity";
      Tortuosity_func = new ConstantProperty(Tortuosity_str,rTortuosity,harm_crsn,pc_refine);
    }

    Real rSpecificStorage = 0;
    Property* SpecificStorage_func = 0;
    if (user_specified_specific_storage) {
      ppr.query("specific_storage.val",rSpecificStorage);
      std::string SpecificStorage_str = "specific_storage";
      SpecificStorage_func = new ConstantProperty(SpecificStorage_str,rSpecificStorage,arith_crsn,pc_refine);
    }

    Property* phi_func = 0;
    Array<Real> rpvals(1), rptimes;
    Array<std::string> rpforms;
    const std::string porosity_prefix(prefix+"."+PorosityName);
    ParmParse pprp(porosity_prefix.c_str());


    std::string PorosityValsName = "vals";
    std::string PorosityValName = "val";
    std::string PorosityStdName = "std";
    std::string PorosityTimesName = "times";
    std::string PorosityFormsName = "forms";
    std::string PorosityDistName = "distribution_type";
    std::string PorosityGSParamFileName = "gslib_param_file";
    std::string PorosityGSDataFileName = "gslib_data_file";
    std::string PorosityGSFileShiftName = "gslib_file_shift";

    std::string phi_dist = "uniform"; pprp.query(PorosityDistName.c_str(),phi_dist);

    if (phi_dist != "uniform" && phi_dist!="gslib") {
      BoxLib::Abort(std::string("Unrecognized distribution_type for rock: \""+rname).c_str());
    }
    if (phi_dist == "uniform") {
      if (pprp.countval(PorosityValsName.c_str()) > 0) {
        pprp.getarr(PorosityValsName.c_str(),rpvals,0,pprp.countval(PorosityValsName.c_str()));
        int nrpvals = rpvals.size();
        if (nrpvals>1) {
          pprp.getarr(PorosityTimesName.c_str(),rptimes,0,nrpvals);
          pprp.getarr(PorosityFormsName.c_str(),rpforms,0,nrpvals-1);
          TabularFunction pft(rptimes,rpvals,rpforms);
          phi_func = new TabularInTimeProperty(PorosityName.c_str(),pft,arith_crsn,pc_refine);
        }
        else {
          phi_func = new ConstantProperty(PorosityName,rpvals[0],arith_crsn,pc_refine);
        }
      } else if (pprp.countval(PorosityValName.c_str()) == 1) {
        rpvals.resize(1); pprp.get(PorosityValName.c_str(),rpvals[0]);
        phi_func = new ConstantProperty(PorosityName,rpvals[0],arith_crsn,pc_refine);
      } else {
        BoxLib::Abort("No valid porosity values given for uniform distribution");
      }
    }
    else {
      // phi_dist == gslib
      std::string gslib_param_file, gslib_data_file;
      generate_porosity_gslib_file = (pprp.countval(PorosityGSParamFileName.c_str()) != 0);
      if (pprp.countval(PorosityGSDataFileName.c_str()) == 0) {
        pprp.get(PorosityGSParamFileName.c_str(),gslib_param_file);
        gslib_data_file="porosity.gslib";
      }
      else {
        pprp.query(PorosityGSParamFileName.c_str(),gslib_param_file);
        pprp.get(PorosityGSDataFileName.c_str(),gslib_data_file);
      }

      Array<Real> gslib_file_shift(BL_SPACEDIM,0);
      pprp.queryarr(PorosityGSFileShiftName.c_str(),gslib_file_shift,0,BL_SPACEDIM);

      int num = amrData_phi.count(gslib_data_file);
      if (num == 0) {
        if (generate_porosity_gslib_file) {
          Real porosity_avg; pprp.get(PorosityValName.c_str(),porosity_avg);
          Real porosity_std; pprp.get(PorosityStdName.c_str(),porosity_std);
          BuildGSLibFile(porosity_avg, porosity_std, gslib_param_file, gslib_data_file,
                         gslib_file_shift, geomArray, refRatio, nGrow, max_grid_fine_gen,
                         arith_crsn,PorosityName);
        }
        
        DataServices::SetBatchMode();
        Amrvis::FileType fileType(Amrvis::NEWPLT);
        dataServices_phi = new DataServices(gslib_data_file, fileType);
        if (!dataServices_phi->AmrDataOk())
          DataServices::Dispatch(DataServices::ExitRequest, NULL);    
        amrData_phi[gslib_data_file] = &(dataServices_phi->AmrDataRef());
      }
      else {
        BoxLib::Abort("Cannot assign the same GSLib data file to more than one material");
      }
      phi_func = new GSLibProperty(PorosityName,0,amrData_phi[gslib_data_file],arith_crsn,pc_refine);
    }

    Property* kappa_func = 0;
    Array<Real> rvpvals(1), rhpvals(1), rh1pvals(1), rvptimes(1), rhptimes(1), rh1ptimes(1);
    Array<std::string> rvpforms, rhpforms, rh1pforms;

    Array<Real> rperm_in(BL_SPACEDIM);
    if (ppr.countval(PermeabilityName.c_str())) {
      ppr.getarr(PermeabilityName.c_str(),rperm_in,0,BL_SPACEDIM);
      rhpvals[0] = rperm_in[0];
      rvpvals[0] = rperm_in[BL_SPACEDIM-1];
#if BL_SPACEDIM==3
      rh1pvals[0] = rperm_in[0];
#endif
    }
    else {

      std::string PermeabilityVertValName = PermeabilityName+".vertical.vals";
      std::string PermeabilityVertTimesName = PermeabilityName+".vertical.times";
      std::string PermeabilityVertFormsName = PermeabilityName+".vertical.forms";

      int nrvpvals = ppr.countval(PermeabilityVertValName.c_str());
      if (nrvpvals>0) {
        ppr.getarr(PermeabilityVertValName.c_str(),rvpvals,0,nrvpvals);
        if (nrvpvals>1) {
          ppr.getarr(PermeabilityVertTimesName.c_str(),rvptimes,0,nrvpvals);
          ppr.getarr(PermeabilityVertFormsName.c_str(),rvpforms,0,nrvpvals-1);
        }
      } else {
        BoxLib::Abort(std::string("No vertical permeability function specified for rock: \""+rname).c_str());
      }

      std::string PermeabilityHoriValName = PermeabilityName+".horizontal.vals";
      std::string PermeabilityHoriTimesName = PermeabilityName+".horizontal.times";
      std::string PermeabilityHoriFormsName = PermeabilityName+".horizontal.forms";
      int nrhpvals = ppr.countval(PermeabilityHoriValName.c_str());
      if (nrhpvals>0) {
        ppr.getarr(PermeabilityHoriValName.c_str(),rhpvals,0,nrhpvals);
        if (nrhpvals>1) {
          ppr.getarr(PermeabilityHoriTimesName.c_str(),rhptimes,0,nrhpvals);
          ppr.getarr(PermeabilityHoriFormsName.c_str(),rhpforms,0,nrhpvals-1);
        }

      } else {
        BoxLib::Abort(std::string("No horizontal permeability function specified for rock: \""+rname).c_str());
      }

#if BL_SPACEDIM==3
      std::string PermeabilityHori1ValName = PermeabilityName+".horizontal1.vals";
      std::string PermeabilityHori1TimesName = PermeabilityName+".horizontal1.times";
      std::string PermeabilityHori1FormsName = PermeabilityName+".horizontal1.forms";
      int nrh1pvals = ppr.countval(PermeabilityHori1ValName.c_str());
      if (nrh1pvals>0) {
        ppr.getarr(PermeabilityHori1ValName.c_str(),rh1pvals,0,nrh1pvals);
        if (nrh1pvals>1) {
          ppr.getarr(PermeabilityHori1TimesName.c_str(),rh1ptimes,0,nrh1pvals);
          ppr.getarr(PermeabilityHori1FormsName.c_str(),rh1pforms,0,nrh1pvals-1);
        }

      } else {
        BoxLib::Abort(std::string("No horizontal1 permeability function specified for rock: \""+rname).c_str());
      }
#endif
    }

    // The permeability is specified in mDa.  
    // This needs to be multiplied with 1e-10 to be consistent 
    // with the other units in the code.  What this means is that
    // we will be evaluating the darcy velocity as:
    //
    //  u_Darcy [m/s] = ( kappa [X . mD] / mu [Pa.s] ).Grad(p) [atm/m]
    //
    // where X is the factor necessary to have this formula be dimensionally
    // consistent.  X here is 1.e-10, and can be combined with kappa for the 
    // the moment because no other derived quantities depend directly on the 
    // value of kappa  (NOTE: We will have to know that this is done however
    // if kappa is used as a diagnostic or in some way for a derived quantity).
    //
    for (int j=0; j<rvpvals.size(); ++j) {
      rvpvals[j] *= 1.e-10;
    }
    for (int j=0; j<rhpvals.size(); ++j) {
      rhpvals[j] *= 1.e-10;
    }
#if BL_SPACEDIM==3
    for (int j=0; j<rh1pvals.size(); ++j) {
      rh1pvals[j] *= 1.e-10;
    }
#endif

    if (rvpvals.size()>1 || rhpvals.size()>1) {
      Array<TabularFunction> pft(BL_SPACEDIM);
      pft[0] = TabularFunction(rhptimes,rhpvals,rhpforms);
      pft[BL_SPACEDIM-1] = TabularFunction(rvptimes,rvpvals,rvpforms);
#if BL_SPACEDIM==3
      pft[1] = TabularFunction(rh1ptimes,rh1pvals,rh1pforms);
#endif
      kappa_func = new TabularInTimeProperty(PermeabilityName,pft,harm_crsn,pc_refine);
    }
    else {
      Array<Real> vals(BL_SPACEDIM);
      vals[0] = rhpvals[0]; vals[BL_SPACEDIM-1] = rvpvals[0];
#if BL_SPACEDIM==3
      vals[1] = rh1pvals[0];
#endif
      kappa_func = new ConstantProperty(PermeabilityName,vals,harm_crsn,pc_refine);
    }

    // Set old-style values
    Array<Real> rpermeability(BL_SPACEDIM,rvpvals[0]);
    for (int j=0;j<BL_SPACEDIM-1;j++) rpermeability[j] = rhpvals[0];


    // capillary pressure: include cpl_coef, residual_saturation, sigma
    const std::string cpl_prefix(prefix+".cpl");
    ParmParse pp_cpl(cpl_prefix.c_str());
    std::string cpl_model; pp_cpl.get("type",cpl_model); 
    std::map<std::string,int>::const_iterator it = CP_models.find(cpl_model);
    int rcplType = -1;
    int rKrType = -1;

    Array<Real> rcplParam(MAX_CPL_PARAMS);

    if (it != CP_models.end()) {
      rcplType = it->second;

      bool is_vG = Is_CP_model_XX(rcplType,CP_model_vG);
      bool is_BC = Is_CP_model_XX(rcplType,CP_model_BC);

      if (rcplType == CP_models[CP_model_None]) {
        rKrType = Kr_models[Kr_model_None];
      }
      else if (is_vG || is_BC)
      {
        Real m, lambda;
        if (is_vG) {
          pp_cpl.get("m",m);
          if (m <= 0) {
            if (ParallelDescriptor::IOProcessor()) {
              std::cerr << "Invalid m (= " << m << " ) for Capillary Pressure model in material: \"" << rname << "\"" << std::endl;
            } BoxLib::Abort();
          }
        } else {
          pp_cpl.get("lambda",lambda);
          if (lambda <= 0) {
            if (ParallelDescriptor::IOProcessor()) {
              std::cerr << "Invalid lambda (= " << lambda << " ) for Capillary Pressure model in material: \"" << rname << "\"" << std::endl;
            } BoxLib::Abort();
          }
        }          
        Real Sr; pp_cpl.get("Sr",Sr);
        if (Sr < 0 || Sr > 1) {
          if (ParallelDescriptor::IOProcessor()) {
            std::cerr << "Invalid Sr (= " << Sr << " ) for Capillary Pressure model in material: \"" << rname << "\"" << std::endl;
          } BoxLib::Abort();
        }

        Real alpha; pp_cpl.get("alpha",alpha);
        if (alpha < 0 ) {
          if (ParallelDescriptor::IOProcessor()) {
            std::cerr << "Invalid alpha (= " << m << " ) for Capillary Pressure model in material: \"" << rname << "\"" << std::endl;
          } BoxLib::Abort();
        }

        std::string Kr_model; ppr.get("Kr_model", Kr_model);
        std::string Kr_full_model_name = cpl_model + "_" + Kr_model;
        std::map<std::string,int>::const_iterator itKr = Kr_models.find(Kr_full_model_name);
        if (it != Kr_models.end()) {
          rKrType = itKr->second;
        }
        else {
          if (ParallelDescriptor::IOProcessor()) {
            std::cerr << "Invalid Kr model (= \"" << Kr_model
                      << "\") for Relative Permeability with Capillary Pressure model (\"" << cpl_model
                      << "\") in material: \"" << rname << "\""<< std::endl;
          } BoxLib::Abort();
        }

        // Get the ell value (optional for vG+{Mualem,Burdine}, required for all others
        Real Kr_ell;
        if (Is_Kr_model_XX(rKrType,Kr_model_vG_Mualem)) {
          Kr_ell = Kr_ell_vG_Mualem_DEF; ppr.query("Kr_ell",Kr_ell);
        }
        else if (Is_Kr_model_XX(rKrType,Kr_model_vG_Burdine)) {
          Kr_ell = Kr_ell_vG_Burdine_DEF; ppr.query("Kr_ell",Kr_ell);
        }
        else {
          ppr.get("Kr_ell",Kr_ell);
        }

        Real Kr_smoothing_max_pcap = Kr_smoothing_max_pcap_DEF;
        ppr.query("Kr_smoothing_max_pcap",Kr_smoothing_max_pcap);

        if (ppr.countval("WRM_plot_file")>0) {
          ppr.get("WRM_plot_file",WRM_plot_file[i].second);

          WRM_plot_file[i].first = NUM_INIT_INTERP_EVAL_PTS_DEF;
          ppr.query("WRM_plot_file_num_pts",WRM_plot_file[i].first);
        }

        // Finally, load array of Real numbers for this model
        rcplParam[CPL_MODEL_ID] = (Real)rcplType;
        if (is_vG) {
          rcplParam[VG_M]     = m;
          rcplParam[VG_ALPHA] = alpha;
          rcplParam[VG_SR]    = Sr;
          rcplParam[VG_ELL]   = Kr_ell;
          rcplParam[VG_KR_MODEL_ID]  = (Real)rKrType;
          rcplParam[VG_KR_SMOOTHING_MAX_PC] = Kr_smoothing_max_pcap;
        }
        else {
          rcplParam[BC_LAMBDA] = lambda;
          rcplParam[BC_ALPHA]  = alpha;
          rcplParam[BC_SR]     = Sr;
          rcplParam[BC_ELL]    = Kr_ell;
          rcplParam[BC_KR_MODEL_ID]  = (Real)rKrType;
          rcplParam[BC_KR_SMOOTHING_MAX_PC] = Kr_smoothing_max_pcap;
        }
      }
      else {
        if (ParallelDescriptor::IOProcessor()) {
          std::cerr << "Unknown capillary pressure (" << cpl_model << ") model for " << rname << std::endl;
        }
      }
    }

    Property* cpl_func = new ConstantProperty(CapillaryPressureName,rcplParam,arith_crsn,pc_refine);

    Array<std::string> region_names;
    ppr.getarr("regions",region_names,0,ppr.countval("regions"));
    Array<const Region*> rregions = region_manager->RegionPtrArray(region_names);
    for (int j=0; j<region_names.size(); ++j) {
      material_regions.push_back(region_names[j]);
    }

    std::vector<Property*> properties;
    if (phi_func) {
      properties.push_back(phi_func);
    }
    properties.push_back(kappa_func);
    if (Dmolec_func) {
      properties.push_back(Dmolec_func);
    }
    if (Dispersivity_func) {
      properties.push_back(Dispersivity_func);
    }
    if (Tortuosity_func) {
      properties.push_back(Tortuosity_func);
    }
    if (SpecificStorage_func) {
      properties.push_back(SpecificStorage_func);
    }
    properties.push_back(cpl_func);
    rock.set(i,new Material(rname,rregions,properties));
    delete phi_func;
    delete kappa_func;
    delete Dmolec_func;
    delete Tortuosity_func;
    delete Dispersivity_func;
    delete SpecificStorage_func;
    delete cpl_func;
  }

#if 0
  // Read rock parameters associated with chemistry
  using_sorption = false;
  aux_chem_variables.clear();

  if (do_tracer_chemistry>0)
  {
    ParmParse ppm("mineral");
    nminerals = ppm.countval("minerals");
    minerals.resize(nminerals);
    if (nminerals>0) {
      ppm.getarr("minerals",minerals,0,nminerals);
    }

    ParmParse pps("sorption_site");
    nsorption_sites = pps.countval("sorption_sites");
    sorption_sites.resize(nsorption_sites);
    if (nsorption_sites>0) {
      pps.getarr("sorption_sites",sorption_sites,0,nsorption_sites);
    }

    ICParmPair sorption_isotherm_options;
    sorption_isotherm_options[          "Kd"] = 0;
    sorption_isotherm_options[  "Langmuir_b"] = 0;
    sorption_isotherm_options["Freundlich_n"] = 1;
	
    for (int k=0; k<tNames.size(); ++k) {
      for (ICParmPair::const_iterator it=sorption_isotherm_options.begin();
           it!=sorption_isotherm_options.end(); ++it) {
        const std::string& str = it->first;
        bool found = false;
        for (int i=0; i<nrock; ++i) {
          const std::string prefix("rock."+r_names[i]+".Sorption_Isotherms."+tNames[k]);
          ParmParse pprs(prefix.c_str());
          if (pprs.countval(str.c_str())) {
            pprs.get(str.c_str(),sorption_isotherm_ics[r_names[i]][tNames[k]][str]);
            found = true;
          }
        }
	    
        if (found) {
          using_sorption = true;
          nsorption_isotherms = ntracers;
          for (int i=0; i<nrock; ++i) {
            if (sorption_isotherm_ics[r_names[i]][tNames[k]].count(str) == 0) {
              sorption_isotherm_ics[r_names[i]][tNames[k]][str] = it->second; // set to default value
            }
          }
          const std::string label = str+"_"+tNames[k];
          if (aux_chem_variables.find(label) == aux_chem_variables.end()) {
            sorption_isotherm_label_map[tNames[k]][str] = aux_chem_variables.size();
            aux_chem_variables[label]=aux_chem_variables.size()-1;
          }
        }
      }
    }

    ICParmPair cation_exchange_options;
    cation_exchange_options["Cation_Exchange_Capacity"] = 0;
    {
      for (ICParmPair::const_iterator it=cation_exchange_options.begin(); it!=cation_exchange_options.end(); ++it) {
        const std::string& str = it->first;
        bool found = false;
        for (int i=0; i<nrock; ++i) {
          const std::string prefix("rock."+r_names[i]);
          ParmParse pprs(prefix.c_str());
          if (pprs.countval(str.c_str())) {
            pprs.get(str.c_str(),cation_exchange_ics[r_names[i]]);
            found = true;
          }
        }
	    
        if (found) {
          using_sorption = true;
          ncation_exchange = 1;
          for (int i=0; i<nrock; ++i) {
            if (cation_exchange_ics.count(r_names[i]) == 0) {
              cation_exchange_ics[r_names[i]] = it->second; // set to default value
            }
          }

          const std::string label = str;
          if (aux_chem_variables.find(label) == aux_chem_variables.end())  {
            cation_exchange_label_map[str] = aux_chem_variables.size();
            aux_chem_variables[label]=aux_chem_variables.size()-1;
          }
          //std::cout << "****************** cation_exchange_ics[" << r_names[i] << "] = " 
          //	  << cation_exchange_ics[r_names[i]] << std::endl;
        }
      }
    }

    ICParmPair mineralogy_options;
    mineralogy_options[      "Volume_Fraction"] = 0;
    mineralogy_options["Specific_Surface_Area"] = 0;
    for (int k=0; k<minerals.size(); ++k) {
      for (ICParmPair::const_iterator it=mineralogy_options.begin(); it!=mineralogy_options.end(); ++it) {
        const std::string& str = it->first;
        bool found = false;
        for (int i=0; i<nrock; ++i) {
          const std::string prefix("rock."+r_names[i]+".mineralogy."+minerals[k]);
          ParmParse pprs(prefix.c_str());
          if (pprs.countval(str.c_str())) {
            pprs.get(str.c_str(),mineralogy_ics[r_names[i]][minerals[k]][str]);
            found = true;
          }
        }
	    
        if (found) {
          using_sorption = true;
          for (int i=0; i<nrock; ++i) {
            if (mineralogy_ics[r_names[i]][minerals[k]].count(str) == 0) {
              mineralogy_ics[r_names[i]][minerals[k]][str] = it->second; // set to default value
            }
          }
          //std::cout << "****************** mineralogy_ics[" << r_names[i] << "][" << minerals[k] 
          //	  << "][" << str << "] = " << mineralogy_ics[r_names[i]][minerals[k]][str] 
          //	  << std::endl;

          const std::string label = str+"_"+minerals[k];
          if (aux_chem_variables.find(label) == aux_chem_variables.end()) {
            mineralogy_label_map[minerals[k]][str] = aux_chem_variables.size();
            aux_chem_variables[label]=aux_chem_variables.size()-1;
          }
        }
      }
    }

    ICParmPair complexation_options;
    complexation_options["Site_Density"] = 0;
    for (int k=0; k<sorption_sites.size(); ++k) {
      for (ICParmPair::const_iterator it=complexation_options.begin(); it!=complexation_options.end(); ++it) {
        const std::string& str = it->first;
        bool found = false;
        for (int i=0; i<nrock; ++i) {
          const std::string prefix("rock."+r_names[i]+".Surface_Complexation_Sites."+sorption_sites[k]);
          ParmParse pprs(prefix.c_str());
          if (pprs.countval(str.c_str())) {
            pprs.get(str.c_str(),surface_complexation_ics[r_names[i]][sorption_sites[k]][str]);
            found = true;
          }
        }
	    
        if (found) {
          using_sorption = true;
          for (int i=0; i<nrock; ++i) {
            if (surface_complexation_ics[r_names[i]][sorption_sites[k]].count(str) == 0) {
              surface_complexation_ics[r_names[i]][sorption_sites[k]][str] = it->second; // set to default value
            }
          }
          //std::cout << "****************** surface_complexation_ics[" << r_names[i] << "][" << sorption_sites[k] 
          //	  << "][" << str << "] = " << sorption_isotherm_ics[r_names[i]][sorption_sites[k]][str] 
          //	  << std::endl;
	      
          const std::string label = str+"_"+sorption_sites[k];
          if (aux_chem_variables.find(label) == aux_chem_variables.end()) {
            surface_complexation_label_map[sorption_sites[k]][str] = aux_chem_variables.size();
            aux_chem_variables[label]=aux_chem_variables.size()-1;
          }
        }
      }
    }

    if (using_sorption) 
    {
      ICParmPair sorption_chem_options; // these are domain-wide, specified per solute
      sorption_chem_options["Total_Sorbed"] = 1.e-40;
      for (int k=0; k<tNames.size(); ++k) {
        for (ICParmPair::const_iterator it=sorption_chem_options.begin(); it!=sorption_chem_options.end(); ++it) {
          const std::string& str = it->first;
          const std::string prefix("tracer."+tNames[k]+".Initial_Condition."+str);
          ParmParse pprs(prefix.c_str());
          sorption_chem_ics[tNames[k]][str] = it->second; // set to default value
          pprs.query(str.c_str(),sorption_chem_ics[tNames[k]][str]);                      
          //std::cout << "****************** sorption_chem_ics[" << tNames[k] 
          //              << "][" << str << "] = " << sorption_chem_ics[tNames[k]][str] << std::endl;
          const std::string label = str+"_"+tNames[k];
          if (aux_chem_variables.find(label) == aux_chem_variables.end()){
            sorption_chem_label_map[tNames[k]][str] = aux_chem_variables.size();
            aux_chem_variables[label]=aux_chem_variables.size()-1;
          }
        }
      }
    }
  }
#endif

  materialFiller = new MatFiller(geomArray,refRatio,rock);
}

RockManager::~RockManager()
{
  delete materialFiller;
}

bool
RockManager::CanDerive(const std::string& property_name) const
{
  return materialFiller->CanDerive(property_name);
}

bool
RockManager::GetProperty(Real               time,
                         int                level,
                         MultiFab&          mf,
                         const std::string& pname,
                         int                dComp,
                         int                nGrow,
                         void*              ctx,
                         bool               ignore_mixed) const
{
  return materialFiller->SetProperty(time,level,mf,pname,dComp,nGrow,ctx,ignore_mixed);
}

void
RockManager::GetMaterialID(int level, iMultiFab& mf, int nGrow, bool ignore_mixed) const
{
  return materialFiller->SetMaterialID(level,mf,nGrow,ignore_mixed);
}


static Real vgMKr(Real seff, Real m, Real mI, Real ell) {
  return std::pow(seff, ell) * std::pow(1-std::pow(1-std::pow(seff,mI),m),2);
}

static Real vgBKr(Real seff, Real m, Real mI, Real ell) {
  return std::pow(seff, ell) * ( 1 - std::pow(1-std::pow(seff,mI),m) );
}

static Real vgPc(Real seff, Real mI, Real nI, Real alphaI) {
  return alphaI * std::pow( std::pow(seff,-mI) - 1, nI);
}

static Real vgPcInv(Real pc, Real m, Real n, Real alpha) {
  return std::pow(1 + std::pow(alpha*pc,n),-m);
}

static Real bcMKr(Real seff, Real lambda, Real ell) {
  return std::pow(seff,ell+2+2/lambda);
}

static Real bcBKr(Real seff, Real lambda, Real ell) {
  return std::pow(seff,ell+1+2/lambda);
}

static Real bcPc(Real seff, Real lambdaI, Real alphaI) {
  return alphaI * std::pow(seff,-lambdaI);
}

static Real bcPcInv(Real pc, Real lambda, Real alpha) {
  return std::pow(alpha*pc,-lambda);
}

// Capillary Pressure (given Saturation)
void
RockManager::CapillaryPressure(const Real* saturation, int* matID, Real time, Real* capillaryPressure, int npts) const
{
  Array<Array<int> > mat_pts = SortPtsByMaterial(matID,npts);

  // Make temp structure to interact with Property interface
  int nComp = materialFiller->NComp(CapillaryPressureName);
  Array<Real> pc_params(nComp);

  for (int j=0; j<rock.size(); ++j) {

    int N = mat_pts[j].size();

    if (N>0) {
      const Property* p = rock[j].Prop(CapillaryPressureName); BL_ASSERT(p!=0);
      p->Evaluate(time,pc_params);
      bool is_vG = Is_CP_model_XX(pc_params[CPL_MODEL_ID],CP_model_vG);
      bool is_BC = Is_CP_model_XX(pc_params[CPL_MODEL_ID],CP_model_BC);

      if (is_vG) {
        Real m      = pc_params[VG_M];
        Real alpha  = pc_params[VG_ALPHA];
        Real Sr     = pc_params[VG_SR];

        Real mI     = 1/m;
        Real omSrI  = 1/(1-Sr);
        Real alphaI = 1/alpha;
        Real n      = 1/(1-m);
        Real nI     = 1-m;

        for (int i=0; i<N; ++i) {
          int idx = mat_pts[j][i];
          Real s = std::min(1.0, std::max(0.0, saturation[idx]));        
          Real seff = (s - Sr)*omSrI;
          capillaryPressure[idx] = vgPc(seff,mI,nI,alphaI);
        }
      }
      else if (is_BC) {
        Real lambda   = pc_params[BC_LAMBDA];
        Real alpha    = pc_params[BC_ALPHA];
        Real Sr       = pc_params[BC_SR];

        Real lambdaI  = 1/lambda;
        Real alphaI   = 1/alpha;
        Real omSrI    = 1/(1-Sr);

        for (int i=0; i<N; ++i) {
          int idx = mat_pts[j][i];
          Real s = std::min(1.0, std::max(0.0, saturation[idx]));        
          Real seff = (s - Sr)*omSrI;
          capillaryPressure[idx] = bcPc(seff,lambdaI,alphaI);
        }
      }
      else {
        if (ParallelDescriptor::IOProcessor()) {
          std::cerr << "Invalid Capillary Presure model " << std::endl;
        } BoxLib::Abort();
      }
    }
  }
}
 
void
RockManager::CapillaryPressure(const MultiFab&  saturation,
                               const iMultiFab& matID,
                               Real             time,
                               MultiFab&        capillaryPressure,
                               int              sComp,
                               int              dComp,
                               int              nGrow) const
{
  BL_ASSERT(saturation.boxArray() == capillaryPressure.boxArray());
  BL_ASSERT(sComp < saturation.nComp()  && dComp < capillaryPressure.nComp());
  BL_ASSERT(nGrow <= saturation.nGrow() && nGrow <= capillaryPressure.nGrow());
  FArrayBox s, pc;
  IArrayBox id;
  for (MFIter mfi(saturation); mfi.isValid(); ++mfi) {
    Box box = Box(mfi.validbox()).grow(nGrow);
    s.resize(box,1); s.copy(saturation[mfi],box,sComp,box,0,1);
    id.resize(box,1); id.copy(matID[mfi],box,0,box,0,1);
    pc.resize(box,1);
    CapillaryPressure(s.dataPtr(),id.dataPtr(),time,pc.dataPtr(),box.numPts());
    capillaryPressure[mfi].copy(pc,box,0,box,dComp,1);
  }
}

Array<Array<int> >
RockManager::SortPtsByMaterial(int* matID, int npts) const
{
  Array<Array<int> > mat_pts(rock.size());
  for (int i=0; i<npts; ++i) {
    mat_pts[matID[i]].push_back(i);
  }
  return mat_pts;
}

// Inverse Capillary Pressure (Saturation given Capillary Pressure)
void
RockManager::InverseCapillaryPressure(const Real* capillaryPressure, int* matID, Real time, Real* saturation, int npts) const
{
  Array<Array<int> > mat_pts = SortPtsByMaterial(matID,npts);

  // Make temp structure to interact with Property interface
  int nComp = materialFiller->NComp(CapillaryPressureName);
  Array<Real> pc_params(nComp);

  for (int j=0; j<rock.size(); ++j) {
    const Property* p = rock[j].Prop(CapillaryPressureName); BL_ASSERT(p!=0);
    p->Evaluate(time,pc_params);
    bool is_vG = Is_CP_model_XX(pc_params[CPL_MODEL_ID],CP_model_vG);
    bool is_BC = Is_CP_model_XX(pc_params[CPL_MODEL_ID],CP_model_BC);

    if (is_vG) {
      Real m     = pc_params[VG_M];
      Real alpha = pc_params[VG_ALPHA];
      Real Sr    = pc_params[VG_SR];
      Real n     = 1./(1-m);
      for (int i=0, End=mat_pts[j].size(); i<End; ++i) {
        int idx = mat_pts[j][i];
        Real seff = (capillaryPressure[idx] <= 0  ? 1 : 
                     std::pow( std::pow(alpha*capillaryPressure[idx],n) + 1, -m));
        saturation[idx] = seff*(1 - Sr) + Sr;
      }
    }
    else if (is_BC) {
      Real mLambda = -pc_params[BC_LAMBDA];
      Real alpha   =  pc_params[BC_ALPHA];
      Real Sr      =  pc_params[BC_SR];
      
      for (int i=0, End=mat_pts[j].size(); i<End; ++i) {
        int idx = mat_pts[j][i];
        Real seff = (capillaryPressure[idx] <= 0  ? 1 : 
                     std::pow(alpha*capillaryPressure[idx],mLambda));
        saturation[idx] = seff*(1 - Sr) + Sr;
      }
    }
    else {
      if (ParallelDescriptor::IOProcessor()) {
        std::cerr << "Invalid Capillary Presure model " << std::endl;
      } BoxLib::Abort();
    }
  }
}

void
RockManager::InverseCapillaryPressure(const MultiFab&  capillaryPressure,
                                      const iMultiFab& matID,
                                      Real             time,
                                      MultiFab&        saturation,
                                      int              sComp,
                                      int              dComp,
                                      int              nGrow) const
{
  BL_ASSERT(saturation.boxArray() == capillaryPressure.boxArray());
  BL_ASSERT(dComp < saturation.nComp()  && sComp < capillaryPressure.nComp());
  BL_ASSERT(nGrow <= saturation.nGrow() && nGrow <= capillaryPressure.nGrow());
  FArrayBox s, pc;
  IArrayBox id;
  for (MFIter mfi(saturation); mfi.isValid(); ++mfi) {
    Box box = Box(mfi.validbox()).grow(nGrow);
    pc.resize(box,1); pc.copy(capillaryPressure[mfi],box,sComp,box,0,1);
    id.resize(box,1); id.copy(matID[mfi],box,0,box,0,1);
    s.resize(box,1);
    InverseCapillaryPressure(pc.dataPtr(),id.dataPtr(),time,s.dataPtr(),box.numPts());
    saturation[mfi].copy(s,box,0,box,dComp,1);
  }
}

// D (Saturation) / D(CapillaryPressure)
void
RockManager::DInverseCapillaryPressure(const Real* saturation, int* matID, Real time, Real* DsaturationDpressure, int npts) const
{
  Array<Array<int> > mat_pts = SortPtsByMaterial(matID,npts);

  // Make temp structure to interact with Property interface
  int nComp = materialFiller->NComp(CapillaryPressureName);
  Array<Real> pc_params(nComp);

  for (int j=0; j<rock.size(); ++j) {
    const Property* p = rock[j].Prop(CapillaryPressureName); BL_ASSERT(p!=0);
    p->Evaluate(time,pc_params);
    bool is_vG = Is_CP_model_XX(pc_params[CPL_MODEL_ID],CP_model_vG);
    bool is_BC = Is_CP_model_XX(pc_params[CPL_MODEL_ID],CP_model_BC);

    if (is_vG) {
      Real m     = pc_params[VG_M];
      Real alpha = pc_params[VG_ALPHA];
      Real Sr    = pc_params[VG_SR];
      Real n     = 1./(1-m);
      Real b     = -1/m;
      Real fac   = - (1 - Sr)*alpha*m*n;
      Real omSrI = 1/(1 - Sr);

      for (int i=0, End=mat_pts[j].size(); i<End; ++i) {
        int idx = mat_pts[j][i];
        Real seff = (saturation[idx] - Sr)*omSrI;
        Real sb = std::pow(seff,b);
        DsaturationDpressure[idx] = fac*std::pow(sb-1,m)*seff/sb;
      }
    }
    else if (is_BC) {
      Real lambda = pc_params[BC_LAMBDA];
      Real alpha  = pc_params[BC_ALPHA];
      Real Sr     = pc_params[BC_SR];
      Real fac    = -alpha*lambda;
      Real oplI   = 1+1/lambda;
      Real omSrI = 1/(1 - Sr);

      for (int i=0, End=mat_pts[j].size(); i<End; ++i) {
        int idx = mat_pts[j][i];
        Real seff = (saturation[idx] - Sr)*omSrI;
        DsaturationDpressure[idx] = fac*std::pow(seff,oplI);
      }
    }
  }
}

void
RockManager::DInverseCapillaryPressure(const MultiFab&  saturation,
                                       const iMultiFab& matID,
                                       Real             time,
                                       MultiFab&        DsaturationDpressure,
                                       int              sComp,
                                       int              dComp,
                                       int              nGrow) const
{
  BL_ASSERT(saturation.boxArray() == DsaturationDpressure.boxArray());
  BL_ASSERT(dComp < saturation.nComp()  && sComp < DsaturationDpressure.nComp());
  BL_ASSERT(nGrow <= saturation.nGrow() && nGrow <= DsaturationDpressure.nGrow());
  FArrayBox s, dsdp;
  IArrayBox id;
  for (MFIter mfi(saturation); mfi.isValid(); ++mfi) {
    Box box = Box(mfi.validbox()).grow(nGrow);
    s.resize(box,1); s.copy(saturation[mfi],box,sComp,box,0,1);
    id.resize(box,1); id.copy(matID[mfi],box,0,box,0,1);
    dsdp.resize(box,1);
    DInverseCapillaryPressure(s.dataPtr(),id.dataPtr(),time,dsdp.dataPtr(),box.numPts());
    DsaturationDpressure[mfi].copy(dsdp,box,0,box,dComp,1);
  }
}

static Real KrInterp(Real se, Real seth, Real m_th, Real m_int) {
  Real dels  = 1 - se;
  Real dels1 = 1 - seth;
  return 1 + dels*dels * m_int/dels1 +  dels*dels * (dels-dels1) * (m_th - 2*m_int) / (dels1*dels1);
}

void
RockManager::RelativePermeability(const Real* saturation, int* matID, Real time, Real* kappa, int npts) const
{
  Array<Array<int> > mat_pts = SortPtsByMaterial(matID,npts);

  // Make temp structure to interact with Property interface
  int nComp = materialFiller->NComp(CapillaryPressureName);
  Array<Real> pc_params(nComp);

  for (int j=0; j<rock.size(); ++j) {
    const Property* p = rock[j].Prop(CapillaryPressureName); BL_ASSERT(p!=0);
    p->Evaluate(time,pc_params);
    bool is_vG = Is_CP_model_XX(pc_params[CPL_MODEL_ID],CP_model_vG);
    bool is_BC = Is_CP_model_XX(pc_params[CPL_MODEL_ID],CP_model_BC);

    if (is_vG) {

      Real m                     = pc_params[VG_M];
      Real alpha                 = pc_params[VG_ALPHA];
      Real Sr                    = pc_params[VG_SR];
      Real ell                   = pc_params[VG_ELL];
      int  Kr_model              = pc_params[VG_KR_MODEL_ID];
      Real Kr_smoothing_max_pcap = pc_params[BC_KR_SMOOTHING_MAX_PC];

      Real omSrI = 1/(1 - Sr);
      Real mI = 1/m;
      Real n = 1/(1-m);
      bool is_Mualem  = Is_Kr_model_XX(Kr_model,Kr_model_vG_Mualem);
      bool is_Burdine = Is_Kr_model_XX(Kr_model,Kr_model_vG_Burdine);

      // Find smoothing interval in terms of seff (need to evaluate here since properties may be time-dependent)
      std::pair<bool,Real>& Kr_min_seff = Kr_smoothing_min_seff[j];
      if ( (Kr_smoothing_max_pcap > 0)
           && (p->isTimeDependent() || Kr_min_seff.first) )
      {
        Kr_min_seff.second = vgPcInv(Kr_smoothing_max_pcap,m,n,alpha);
        Kr_min_seff.first = false;

        if (ParallelDescriptor::IOProcessor()) {
          std::cout << "For material \"" << rock[j].Name() << "\" seff thresh is " << Kr_min_seff.second << std::endl;
        }
      }

      if (is_Mualem) {
        Real Kr_slope_thresh, Kr_slope_interval;
        if (Kr_smoothing_max_pcap > 0) {
          Real mI = 1/m;
          Real ds = (1 - Kr_min_seff.second)*0.001;
          Real Kr_thresh1 = vgMKr(Kr_min_seff.second   ,m,mI,ell);
          Real Kr_thresh2 = vgMKr(Kr_min_seff.second+ds,m,mI,ell);
          Kr_slope_thresh = -(Kr_thresh2 - Kr_thresh1)/ds;
          Kr_slope_interval = (Kr_thresh1 - 1)/(1 - Kr_min_seff.second);
        }

        for (int i=0, End=mat_pts[j].size(); i<End; ++i) {
          int idx = mat_pts[j][i];
          Real seff = (saturation[idx] - Sr)*omSrI;
          if (seff > Kr_min_seff.second && seff < 1) {
            kappa[idx] = KrInterp(seff, Kr_min_seff.second, Kr_slope_thresh, Kr_slope_interval);
          }
          else {
            kappa[idx] = vgMKr(seff,m,mI,ell);
          }
        }
      }
      else if (is_Burdine) {
        Real Kr_slope_thresh, Kr_slope_interval;
        if (Kr_smoothing_max_pcap > 0) {
          Real mI = 1/m;
          Real ds = (1 - Kr_min_seff.second)*0.001;
          Real Kr_thresh1 = vgBKr(Kr_min_seff.second   ,m,mI,ell);
          Real Kr_thresh2 = vgBKr(Kr_min_seff.second+ds,m,mI,ell);
          Kr_slope_thresh = -(Kr_thresh2 - Kr_thresh1)/ds;
          Kr_slope_interval = (Kr_thresh1 - 1)/(1 - Kr_min_seff.second);
        }

        for (int i=0, End=mat_pts[j].size(); i<End; ++i) {
          int idx = mat_pts[j][i];
          Real seff = (saturation[idx] - Sr)*omSrI;
          if (seff > Kr_min_seff.second && seff < 1) {
            kappa[idx] = KrInterp(seff, Kr_min_seff.second, Kr_slope_thresh, Kr_slope_interval);
          }
          else {
            kappa[idx] = vgBKr(seff,m,mI,ell);
          }
        }
      }

    } else if (is_BC) {

      Real lambda = pc_params[BC_LAMBDA];
      Real alpha  = pc_params[BC_ALPHA];
      Real Sr     = pc_params[BC_SR];
      Real ell    = pc_params[BC_ELL];
      Real omSrI  = 1/(1 - Sr);

      bool is_Mualem  = Is_Kr_model_XX(pc_params[VG_KR_MODEL_ID],Kr_model_BC_Mualem);
      bool is_Burdine = Is_Kr_model_XX(pc_params[VG_KR_MODEL_ID],Kr_model_BC_Burdine);

      BL_ASSERT(is_Mualem || is_Burdine);
      Real f = (is_Mualem ? ell + 2 + 2/lambda : ell + 1 + 2/lambda);

      for (int i=0, End=mat_pts[j].size(); i<End; ++i) {
        int idx = mat_pts[j][i];
        Real seff = (saturation[idx] - Sr)*omSrI;
        kappa[idx] = std::pow(seff,f);
      }

    }
  }
}

void
RockManager::RelativePermeability(const MultiFab&  saturation,
                                  const iMultiFab& matID,
                                  Real             time,
                                  MultiFab&        kappa,
                                  int              sComp,
                                  int              dComp,
                                  int              nGrow) const
{
  BL_ASSERT(saturation.boxArray() == kappa.boxArray());
  BL_ASSERT(dComp < saturation.nComp()  && sComp < kappa.nComp());
  BL_ASSERT(nGrow <= saturation.nGrow() && nGrow <= kappa.nGrow());
  FArrayBox s, k;
  IArrayBox id;
  for (MFIter mfi(saturation); mfi.isValid(); ++mfi) {
    Box box = Box(mfi.validbox()).grow(nGrow);
    s.resize(box,1); s.copy(saturation[mfi],box,sComp,box,0,1);
    id.resize(box,1); id.copy(matID[mfi],box,0,box,0,1);
    k.resize(box,1);
    RelativePermeability(s.dataPtr(),id.dataPtr(),time,k.dataPtr(),box.numPts());
    kappa[mfi].copy(k,box,0,box,dComp,1);
  }
}


void 
RockManager::ResidualSaturation(int* matID, Real time, Real* Sr, int npts) const
{
  Array<Array<int> > mat_pts = SortPtsByMaterial(matID,npts);

  // Make temp structure to interact with Property interface
  int nComp = materialFiller->NComp(CapillaryPressureName);
  Array<Real> pc_params(nComp);

  for (int j=0; j<rock.size(); ++j) {
    const Property* p = rock[j].Prop(CapillaryPressureName); BL_ASSERT(p!=0);
    p->Evaluate(time,pc_params);
    bool is_vG = Is_CP_model_XX(pc_params[CPL_MODEL_ID],CP_model_vG);
    bool is_BC = Is_CP_model_XX(pc_params[CPL_MODEL_ID],CP_model_BC);

    if (is_vG) {
      for (int i=0, End=mat_pts[j].size(); i<End; ++i) {
        int idx = mat_pts[j][i];
        Sr[idx] = pc_params[VG_SR];
      }
    } else if (is_BC) {
      for (int i=0, End=mat_pts[j].size(); i<End; ++i) {
        int idx = mat_pts[j][i];
        Sr[idx] = pc_params[BC_SR];
      }
    }
  }
}

void
RockManager::ResidualSaturation(const iMultiFab& matID,
                                Real             time,
                                MultiFab&        Sr,
                                int              dComp,
                                int              nGrow) const
{
  BL_ASSERT(dComp < Sr.nComp());
  BL_ASSERT(nGrow <= Sr.nGrow());
  FArrayBox sr;
  IArrayBox id;
  for (MFIter mfi(Sr); mfi.isValid(); ++mfi) {
    Box box = Box(mfi.validbox()).grow(nGrow);
    id.resize(box,1); id.copy(matID[mfi],box,0,box,0,1);
    sr.resize(box,1);
    ResidualSaturation(id.dataPtr(),time,sr.dataPtr(),box.numPts());
    Sr[mfi].copy(sr,box,0,box,dComp,1);
  }
}

// Annoying set of functions necessary make [] operator of maps const
bool
RockManager::Is_CP_model_XX(int model_id, const std::string& str) const
{
  std::map<std::string,int>::const_iterator it = CP_models.find(str);
  if (it != CP_models.end()) {
    return model_id == it->second;
  }
  return false;
}

bool
RockManager::Is_Kr_model_XX(int model_id, const std::string& str) const
{
  std::map<std::string,int>::const_iterator it = Kr_models.find(str);
  if (it != Kr_models.end()) {
    return model_id == it->second;
  }
  return false;
}
