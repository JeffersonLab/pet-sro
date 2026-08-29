#----------------------------------------------------------------
# Generated CMake target import file for configuration "Release".
#----------------------------------------------------------------

# Commands may need to know the format version.
set(CMAKE_IMPORT_FILE_VERSION 1)

# Import target "shmem_writer::shmem_writer" for configuration "Release"
set_property(TARGET shmem_writer::shmem_writer APPEND PROPERTY IMPORTED_CONFIGURATIONS RELEASE)
set_target_properties(shmem_writer::shmem_writer PROPERTIES
  IMPORTED_LOCATION_RELEASE "${_IMPORT_PREFIX}/lib/libshmem_writer.so"
  IMPORTED_SONAME_RELEASE "libshmem_writer.so"
  )

list(APPEND _IMPORT_CHECK_TARGETS shmem_writer::shmem_writer )
list(APPEND _IMPORT_CHECK_FILES_FOR_shmem_writer::shmem_writer "${_IMPORT_PREFIX}/lib/libshmem_writer.so" )

# Commands beyond this point should not need to know the version.
set(CMAKE_IMPORT_FILE_VERSION)
