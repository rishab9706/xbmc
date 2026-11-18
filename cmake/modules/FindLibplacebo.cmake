find_package(PkgConfig REQUIRED)
pkg_check_modules(PC_LIBPLACEBO REQUIRED IMPORTED_TARGET libplacebo)

set(LIBPLACEBO_FOUND TRUE)
set(LIBPLACEBO_VERSION "${PC_LIBPLACEBO_VERSION}")

string(REGEX MATCH "[0-9]+\\.([0-9]+)\\.[0-9]+" _ "${PC_LIBPLACEBO_VERSION}")
set(LIBPLACEBO_API_VERSION "${CMAKE_MATCH_1}")

if(LIBPLACEBO_API_VERSION VERSION_LESS 325)
  message(FATAL_ERROR
    "libplacebo API version ${LIBPLACEBO_API_VERSION} is too old; "
    "pl_icc_update() requires API version 325 or newer.")
endif()

if(NOT APP_NAME_LC)
  set(APP_NAME_LC "${CMAKE_PROJECT_NAME}")
endif()

if(NOT TARGET ${APP_NAME_LC}::Libplacebo)
  add_library(${APP_NAME_LC}::Libplacebo INTERFACE IMPORTED)

  set_target_properties(${APP_NAME_LC}::Libplacebo PROPERTIES
    INTERFACE_LINK_LIBRARIES "PkgConfig::PC_LIBPLACEBO"
    INTERFACE_COMPILE_DEFINITIONS "HAS_PLACEBO;PL_STATIC"
  )
endif()

add_library(LIBRARY::Libplacebo ALIAS ${APP_NAME_LC}::Libplacebo)

include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(Libplacebo
  REQUIRED_VARS LIBPLACEBO_VERSION
  VERSION_VAR LIBPLACEBO_VERSION
)