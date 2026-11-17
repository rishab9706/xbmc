# FindLibplacebo.cmake - Find the libplacebo library
#
# This module defines:
#  LIBPLACEBO_FOUND - True if libplacebo was found
#  LIBPLACEBO_INCLUDE_DIRS - Include directories for libplacebo
#  LIBPLACEBO_LIBRARIES - Libraries to link against
#  LIBPLACEBO_VERSION - Version of libplacebo
#  LIBPLACEBO_API_VERSION - API version of libplacebo
#  ${APP_NAME_LC}::Libplacebo - Target for linking

# Try to find via pkg-config first (libplacebo doesn't provide CMake config)
find_package(PkgConfig QUIET)
if(PKG_CONFIG_FOUND)
  pkg_check_modules(PC_LIBPLACEBO QUIET libplacebo IMPORTED_TARGET)
endif()

if(PC_LIBPLACEBO_FOUND)
  set(LIBPLACEBO_FOUND TRUE)
  set(LIBPLACEBO_INCLUDE_DIRS ${PC_LIBPLACEBO_INCLUDE_DIRS})
  set(LIBPLACEBO_LIBRARIES ${PC_LIBPLACEBO_LIBRARIES})
  set(LIBPLACEBO_VERSION ${PC_LIBPLACEBO_VERSION})
  # Extract API version (middle number of version)
  string(REGEX MATCH "[0-9]+\\.([0-9]+)\\.[0-9]+" _ "${PC_LIBPLACEBO_VERSION}")
  set(LIBPLACEBO_API_VERSION ${CMAKE_MATCH_1})
else()
  # Fallback to CMake config (if libplacebo ever provides one)
  find_package(libplacebo QUIET)
  if(libplacebo_FOUND)
    set(LIBPLACEBO_FOUND TRUE)
    set(LIBPLACEBO_INCLUDE_DIRS ${libplacebo_INCLUDE_DIRS})
    set(LIBPLACEBO_LIBRARIES ${libplacebo_LIBRARIES})
    set(LIBPLACEBO_VERSION ${libplacebo_VERSION})
    string(REGEX MATCH "[0-9]+\\.([0-9]+)\\.[0-9]+" _ "${libplacebo_VERSION}")
    set(LIBPLACEBO_API_VERSION ${CMAKE_MATCH_1})
  endif()
endif()

# Create target if found
if(LIBPLACEBO_FOUND)
  # APP_NAME_LC might not be set in find_package scope, use CMAKE_PROJECT_NAME as fallback
  if(NOT APP_NAME_LC)
    set(APP_NAME_LC ${CMAKE_PROJECT_NAME})
  endif()

  if(NOT TARGET ${APP_NAME_LC}::Libplacebo)
    if(TARGET PkgConfig::placebo)
      # Use the PkgConfig imported target, wrap it to add compile definitions
      add_library(${APP_NAME_LC}::Libplacebo INTERFACE IMPORTED)
      set_target_properties(${APP_NAME_LC}::Libplacebo PROPERTIES
        INTERFACE_LINK_LIBRARIES "PkgConfig::placebo"
        INTERFACE_COMPILE_DEFINITIONS "PL_STATIC;HAS_PLACEBO"
      )
    elseif(TARGET libplacebo::libplacebo)
      # Use the CMake config target if available, wrap it to add compile definitions
      add_library(${APP_NAME_LC}::Libplacebo INTERFACE IMPORTED)
      set_target_properties(${APP_NAME_LC}::Libplacebo PROPERTIES
        INTERFACE_LINK_LIBRARIES "libplacebo::libplacebo"
        INTERFACE_COMPILE_DEFINITIONS "PL_STATIC;HAS_PLACEBO"
      )
    else()
      # Fallback: create custom INTERFACE IMPORTED target
      add_library(${APP_NAME_LC}::Libplacebo INTERFACE IMPORTED)
      set_target_properties(${APP_NAME_LC}::Libplacebo PROPERTIES
        INTERFACE_INCLUDE_DIRECTORIES "${LIBPLACEBO_INCLUDE_DIRS}"
        INTERFACE_LINK_LIBRARIES "${LIBPLACEBO_LIBRARIES}"
        INTERFACE_COMPILE_DEFINITIONS "PL_STATIC;HAS_PLACEBO"
      )
    endif()
    add_library(LIBRARY::Libplacebo ALIAS ${APP_NAME_LC}::Libplacebo)
  endif()

  # Handle version check if required
  if(LIBPLACEBO_VERSION AND LIBPLACEBO_VER AND LIBPLACEBO_VERSION VERSION_LESS "${LIBPLACEBO_VER}")
    if(ENABLE_INTERNAL_LIBPLACEBO)
      message(WARNING "Found libplacebo version ${LIBPLACEBO_VERSION} but required ${LIBPLACEBO_VER}. Building internal libplacebo instead.")
      set(LIBPLACEBO_FOUND FALSE)
    else()
      message(FATAL_ERROR "Found libplacebo version ${LIBPLACEBO_VERSION} but required ${LIBPLACEBO_VER}.")
    endif()
  endif()

  if(LIBPLACEBO_FIND_REQUIRED AND NOT LIBPLACEBO_FOUND)
    message(FATAL_ERROR "Libplacebo libraries were not found.")
  endif()

  mark_as_advanced(LIBPLACEBO_INCLUDE_DIRS LIBPLACEBO_LIBRARIES)
endif()

include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(Libplacebo
  REQUIRED_VARS LIBPLACEBO_LIBRARIES LIBPLACEBO_INCLUDE_DIRS
  VERSION_VAR LIBPLACEBO_VERSION
)