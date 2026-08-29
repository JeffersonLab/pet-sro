#----------------------------------------------------------------
# Generated CMake target import file for configuration "RelWithDebInfo".
#----------------------------------------------------------------

# Commands may need to know the format version.
set(CMAKE_IMPORT_FILE_VERSION 1)

# Import target "Ersap::ersap" for configuration "RelWithDebInfo"
set_property(TARGET Ersap::ersap APPEND PROPERTY IMPORTED_CONFIGURATIONS RELWITHDEBINFO)
set_target_properties(Ersap::ersap PROPERTIES
  IMPORTED_LOCATION_RELWITHDEBINFO "${_IMPORT_PREFIX}/lib64/libersap.so"
  IMPORTED_SONAME_RELWITHDEBINFO "libersap.so"
  )

list(APPEND _cmake_import_check_targets Ersap::ersap )
list(APPEND _cmake_import_check_files_for_Ersap::ersap "${_IMPORT_PREFIX}/lib64/libersap.so" )

# Commands beyond this point should not need to know the version.
set(CMAKE_IMPORT_FILE_VERSION)
