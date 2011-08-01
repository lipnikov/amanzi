#include "amanzi_structured_grid_simulation_driver.H"
#include "ParmParse.H"
#include "Amr.H"
#include "PorousMedia.H"

#include "ParmParseHelpers.H"

void
Structured_observations(const Array<Observation>& observation_array,
			Amanzi::ObservationData& observation_data)
{
  for (int i=0; i<observation_array.size(); ++i)
    {
      std::string label = observation_array[i].name;
      int ntimes = observation_array[i].times.size();
      std::vector<Amanzi::ObservationData::DataTriple> dt(ntimes);
      for (int j = 0; j<ntimes; ++j)
	dt[j].time = observation_array[i].times[j];
      
      int nval = observation_array[i].vals.size();
      for (int j = 0; j<nval; ++j)
	{
	  dt[j].value = observation_array[i].vals[j];
	  dt[j].is_valid = true;
	}
      
      observation_data[label] = dt;
    }	
}  

Amanzi::Simulator::ReturnType
AmanziStructuredGridSimulationDriver::Run (const MPI_Comm&               mpi_comm,
                                           const Teuchos::ParameterList& input_parameter_list,
                                           Amanzi::ObservationData&      output_observations)
{
    int argc=0;
    char** argv;

    BoxLib::Initialize(argc,argv,false,mpi_comm);

    // Retain (for now) the ability to augment the input parameters with an additional file in the ParmParse format
    if (0 && input_parameter_list.isParameter("PPfile"))
      {
	std::string PPfile = Teuchos::getParameter<std::string>(input_parameter_list, "PPfile");
	ParmParse::Initialize(argc,argv,PPfile.c_str());
      }
    BoxLib::Initialize_ParmParse(input_parameter_list);

    const Real run_strt = ParallelDescriptor::second();

    int  max_step;
    Real strt_time;
    Real stop_time;

    ParmParse pp;

    max_step  = -1;    
    strt_time =  0.0;  
    stop_time = -1.0;  

    pp.query("max_step",max_step);
    pp.query("strt_time",strt_time);
    pp.query("stop_time",stop_time);

    if (strt_time < 0.0)
        BoxLib::Abort("MUST SPECIFY a non-negative strt_time");

    if (max_step < 0 && stop_time < 0.0)
    {
        BoxLib::Abort(
            "Exiting because neither max_step nor stop_time is non-negative.");
    }


    Amr* amrptr = new Amr;

    amrptr->init(strt_time,stop_time);

    while ( amrptr->okToContinue()           &&
           (amrptr->levelSteps(0) < max_step || max_step < 0) &&
           (amrptr->cumTime() < stop_time || stop_time < 0.0) )
    {
        amrptr->coarseTimeStep(stop_time);
    }

    // Process the observations
    const Array<Observation>& observation_array = PorousMedia::TheObservationArray();

    Structured_observations(observation_array,output_observations);

    delete amrptr;

    const int IOProc   = ParallelDescriptor::IOProcessorNumber();
    Real      run_stop = ParallelDescriptor::second() - run_strt;

    ParallelDescriptor::ReduceRealMax(run_stop,IOProc);

    if (ParallelDescriptor::IOProcessor())
      {
        std::cout << "Run time = " << run_stop << std::endl;
	std::cout << "SCOMPLETED\n";
      }

    bool dump_unused_parameters = false;
    BoxLib::Finalize(dump_unused_parameters);

    return Amanzi::Simulator::SUCCESS;
}
