#----------------------------------------------------------------
# Generated CMake target import file for configuration "Release".
#----------------------------------------------------------------

# Commands may need to know the format version.
set(CMAKE_IMPORT_FILE_VERSION 1)

# Import target "microcdr" for configuration "Release"
set_property(TARGET microcdr APPEND PROPERTY IMPORTED_CONFIGURATIONS RELEASE)
set_target_properties(microcdr PROPERTIES
  IMPORTED_LINK_INTERFACE_LANGUAGES_RELEASE "C"
  IMPORTED_LOCATION_RELEASE "${_IMPORT_PREFIX}/lib/libmicrocdr.a"
  )

list(APPEND _IMPORT_CHECK_TARGETS microcdr )
list(APPEND _IMPORT_CHECK_FILES_FOR_microcdr "${_IMPORT_PREFIX}/lib/libmicrocdr.a" )

# Commands beyond this point should not need to know the version.
set(CMAKE_IMPORT_FILE_VERSION)
