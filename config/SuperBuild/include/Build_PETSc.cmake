#  -*- mode: cmake -*-

#
# Build TPL:  PETSc 
#    
# --- Define all the directories and common external project flags
define_external_project_args(PETSc TARGET petsc DEPENDS ${MPI_PROJECT} BUILD_IN_SOURCE)

# --- Download packages PETSc needs
set(petsc_url_download http://software.lanl.gov/ascem/tpls)
get_filename_component(real_download_path ${TPL_DOWNLOAD_DIR} REALPATH)
set(petsc_archive_files)
set(petsc_archive_md5sums)
set(superlu_archive_file superlu_4.2.tar.gz)
list(APPEND petsc_archive_files ${superlu_archive_file})
list(APPEND petsc_archive_md5sums 565602cf69e425874c2525f8b96e9bb1)
set(superludist_archive_file superlu_dist_2.5.tar.gz)
list(APPEND petsc_archive_files ${superludist_archive_file})
list(APPEND petsc_archive_md5sums 2194ae8f9786e396a721cf4d41045566)

if (DISABLE_EXTERNAL_DOWNLOAD)
    foreach ( _file ${petsc_archive_files} )
      if (  NOT EXISTS "${real_download_path}/${_file}" ) 
	message(FATAL_ERROR "PETSc build requires ${_file}. Not found in ${real_download_path}")
      endif()
    endforeach()
else()
    list(LENGTH petsc_archive_files num_archive_files)
    set(list_idx 0)
    while ( "${list_idx}" LESS "${num_archive_files}" )
      list(GET petsc_archive_files ${list_idx} _file)
      list(GET petsc_archive_md5sums ${list_idx} _md5sum)
      message(STATUS "Downloading ${_file} for PETSC")
      file(DOWNLOAD "${petsc_url_download}/${_file}"
	            "${real_download_path}/${_file}"
		    SHOW_PROGRESS
		    INACTIVITY_TIMEOUT 180
		    EXPECTED_MD5SUM "${_md5sum}")
      math(EXPR list_idx "${list_idx}+1") 		  
    endwhile()
endif()    

# --- Define configure parameters

# Use the common cflags, cxxflags
include(BuildWhitespaceString)
build_whitespace_string(petsc_cflags
                       ${Amanzi_COMMON_CFLAGS})

build_whitespace_string(petsc_cxxflags
                       ${Amanzi_COMMON_CXXFLAGS})
set(cpp_flag_list 
    ${Amanzi_COMMON_CFLAGS}
    ${Amanzi_COMMON_CXXFLAGS})
list(REMOVE_DUPLICATES cpp_flag_list)
build_whitespace_string(petsc_cppflags ${cpp_flags_list})

build_whitespace_string(petsc_fcflags
                       ${Amanzi_COMMON_FCFLAGS})

# Set PETSc debug flag
if ( "${CMAKE_BUILD_TYPE}" STREQUAL "Release" )
  set(petsc_debug_flag 0)
else()
  set(petsc_debug_flag 1)
endif()

# Point PETSc to the MPI build
print_variable(${MPI_PROJECT}_BUILD_TARGET)
if ( "${${MPI_PROJECT}_BUILD_TARGET}" STREQUAL "" )
  set(petsc_mpi_flags --with-mpi=1)
else()
  set(petsc_mpi_flags 
            --with-mpi=1 --with-mpi-dir=${TPL_INSTALL_PREFIX})
endif()

# BLAS options
if (BLAS_LIBRARIES) 
  build_whitespace_string(petsc_blas_libs ${BLAS_LIBRARIES})
  set(petsc_blas_option --with-blas-lib='${petsc_blas_libs}')
else()
  set(petsc_blas_option)
endif()

# LAPACK options
if ( LAPACK_LIBRARIES ) 
  build_whitespace_string(petsc_lapack_libs ${LAPACK_LIBRARIES})
  set(petsc_lapack_option --with-lapack-lib='${petsc_lapack_libs}')
else()
  set(petsc_lapack_option)
endif()

# PETSc SuperLU flags
# For now we allow PETSc to download and build this package
# It should be a separate TPL. Error with the download or
# building of this package will appear to be an error in the
# petsc-configure target. See the log files for more detailed
# information.
set(petsc_superlu_flags 
         --download-superlu_dist=${real_download_path}/${superludist_archive_file}
	 --download-parmetis
	 --download-superlu=${real_download_path}/${superlu_archive_file})

# PETSc install directory
set(petsc_install_dir ${TPL_INSTALL_PREFIX}/${PETSc_BUILD_TARGET}-${PETSc_VERSION})

# --- Add external project build 
ExternalProject_Add(${PETSc_BUILD_TARGET}
                    DEPENDS   ${PETSc_PACKAGE_DEPENDS}             # Package dependency target
                    TMP_DIR   ${PETSc_tmp_dir}                     # Temporary files directory
                    STAMP_DIR ${PETSc_stamp_dir}                   # Timestamp and log directory
                    # -- Download and URL definitions
                    DOWNLOAD_DIR ${TPL_DOWNLOAD_DIR}               # Download directory
                    URL          ${PETSc_URL}                      # URL may be a web site OR a local file
                    URL_MD5      ${PETSc_MD5_SUM}                  # md5sum of the archive file
                    # -- Configure
                    SOURCE_DIR        ${PETSc_source_dir}          # Source directory
                    CONFIGURE_COMMAND
                              <SOURCE_DIR>/configure
                                          --prefix=<INSTALL_DIR>
                                          --with-cc=${CMAKE_C_COMPILER_USE}
                                          --with-cxx=${CMAKE_CXX_COMPILER_USE}
                                          --with-fc=${CMAKE_Fortran_COMPILER_USE}
                                          --CFLAGS=${petsc_cflags}
                                          --CXXFLAGS=${petsc_cxxflags}
                                          --with-debugging=${petsc_debug_flag}
					  --with-mpi=1
                                          ${petsc_lapack_option}
                                          ${petsc_blas_option}
					  ${petsc_superlu_flags}
                    # -- Build
                    BINARY_DIR        ${PETSc_build_dir}           # Build directory 
                    BUILD_COMMAND     $(MAKE)                      # Run the CMake script to build
                    BUILD_IN_SOURCE   ${PETSc_BUILD_IN_SOURCE}     # Flag for in source builds
                    # -- Install
                    INSTALL_DIR      ${petsc_install_dir}  # Install directory, NOT in the usual directory
                    # -- Output control
                    ${PETSc_logging_args})

# --- Useful variables for other packages that depend on PETSc
set(PETSC_DIR ${petsc_install_dir})
