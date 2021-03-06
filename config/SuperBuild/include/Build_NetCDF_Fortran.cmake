#  -*- mode: cmake -*-

#
# Build TPL: NetCDF-fortran
# 

# --- Define all the directories and common external project flags
define_external_project_args(NetCDF_Fortran TARGET netcdf-fortran
                             DEPENDS ${MPI_PROJECT} NetCDF)

# add version version to the autogenerated tpl_versions.h file
amanzi_tpl_version_write(FILENAME ${TPL_VERSIONS_INCLUDE_FILE}
  PREFIX NetCDF_Fortran
  VERSION ${NetCDF_Fortran_VERSION_MAJOR} ${NetCDF_Fortran_VERSION_MINOR} ${NetCDF_Fortran_VERSION_PATCH})
  
#set(cpp_flags_list -I${NetCDF_prefix_dir}/include)

set(cpp_flags_list -I${TPL_INSTALL_PREFIX}/include)
set(ld_flags_list -L${TPL_INSTALL_PREFIX}/lib)
list(REMOVE_DUPLICATES cpp_flags_list)
list(REMOVE_DUPLICATES ld_flags_list)
build_whitespace_string(netcdf_fortran_cppflags ${cpp_flags_list})
build_whitespace_string(netcdf_fortran_ldflags ${ld_flags_list})

# --- Add external project build and tie to the ZLIB build target
# Default build produces shared libraries
ExternalProject_Add(${NetCDF_Fortran_BUILD_TARGET}
	            DEPENDS ${NetCDF_Fortran_PACKAGE_DEPENDS} # Package dependency target
	            TMP_DIR ${NetCDF_Fortran_tmp_dir}         # Temporary files directory
	            STAMP_DIR ${NetCDF_Fortran_stamp_dir}     # Timestamp and log directory
	            # -- Downloads
	            DOWNLOAD_DIR ${TPL_DOWNLOAD_DIR}
	            URL ${NetCDF_Fortran_URL}                 # URL may be a web site OR a local file
	            URL_MD5 ${NetCDF_Fortran_MD5_SUM}         # md5sum of the archive file
	            # -- Configure
	            SOURCE_DIR ${NetCDF_Fortran_source_dir}   # Source directory
	            CONFIGURE_COMMAND 
                        <SOURCE_DIR>/configure
                        --prefix=<INSTALL_DIR>
                        --disable-fortran-compiler-check
                        --enable-shared=${BUILD_SHARED_LIBS}
                        --enable-static=${BUILD_STATIC_LIBS}
		        CC=${CMAKE_C_COMPILER}
		        FC=${CMAKE_Fortran_COMPILER}
                        FCFLAGS=${Amanzi_COMMON_FCFLAGS}
                        CPPFLAGS=${netcdf_fortran_cppflags}
                        LDFLAGS=${netcdf_fortran_ldflags}
                        LD_LIBRARY_PATH=${TPL_INSTALL_PREFIX}/lib:$ENV{LD_LIBRARY_PATH}
	            # -- Build
	            BINARY_DIR ${NetCDF_Fortran_build_dir}    # Build directory 
	            BUILD_COMMAND $(MAKE)                     # $(MAKE) enables parallel builds through make
	            BUILD_IN_SOURCE ${NetCDF_Fortran_BUILD_IN_SOURCE}
	            # -- Install
	            INSTALL_DIR ${TPL_INSTALL_PREFIX}
	            # -- Output control
	            ${NetCDF_Fortran_logging_args})

