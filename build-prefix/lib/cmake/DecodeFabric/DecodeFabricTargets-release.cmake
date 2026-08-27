#----------------------------------------------------------------
# Generated CMake target import file for configuration "Release".
#----------------------------------------------------------------

# Commands may need to know the format version.
set(CMAKE_IMPORT_FILE_VERSION 1)

# Import target "DecodeFabric::DecodeFabric" for configuration "Release"
set_property(TARGET DecodeFabric::DecodeFabric APPEND PROPERTY IMPORTED_CONFIGURATIONS RELEASE)
set_target_properties(DecodeFabric::DecodeFabric PROPERTIES
  IMPORTED_LINK_INTERFACE_LANGUAGES_RELEASE "CXX"
  IMPORTED_LOCATION_RELEASE "${_IMPORT_PREFIX}/lib/decodefabric.lib"
  )

list(APPEND _cmake_import_check_targets DecodeFabric::DecodeFabric )
list(APPEND _cmake_import_check_files_for_DecodeFabric::DecodeFabric "${_IMPORT_PREFIX}/lib/decodefabric.lib" )

# Commands beyond this point should not need to know the version.
set(CMAKE_IMPORT_FILE_VERSION)
