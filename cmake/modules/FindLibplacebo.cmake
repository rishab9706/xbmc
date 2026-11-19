#.rst:
# FindLibplacebo
# --------------
# Builds libplacebo from the configured git repository
#
# This will define the following target:
#
#   ${APP_NAME_LC}::Libplacebo - The libplacebo library
#   LIBRARY::Libplacebo - ALIAS target for the libplacebo library

if(NOT TARGET ${APP_NAME_LC}::${CMAKE_FIND_PACKAGE_NAME})

  include(cmake/scripts/common/ModuleHelpers.cmake)

  set(${CMAKE_FIND_PACKAGE_NAME}_MODULE_LC libplacebo)

  set(LIBPLACEBO_GIT_REPOSITORY "https://github.com/rishab9706/libplacebo.git"
      CACHE STRING "libplacebo git repository to build")

  set(LIBPLACEBO_GIT_TAG "develop"
      CACHE STRING "libplacebo git branch, tag or commit to build")

  find_package(Meson REQUIRED)
  find_package(Ninja REQUIRED)

  if(CMAKE_CROSSCOMPILING)
    generate_mesoncrossfile()

    if(EXISTS ${DEPENDS_PATH}/share/libplacebo-cross-file.meson)
      set(LIBPLACEBO_CROSS_FILE --cross-file=${DEPENDS_PATH}/share/libplacebo-cross-file.meson)
    elseif(EXISTS ${DEPENDS_PATH}/share/cross-file.meson)
      set(LIBPLACEBO_CROSS_FILE --cross-file=${DEPENDS_PATH}/share/cross-file.meson)
    endif()
  endif()

  set(LIBPLACEBO_LIBRARY
      "${DEPENDS_PATH}/lib/${CMAKE_STATIC_LIBRARY_PREFIX}placebo${CMAKE_STATIC_LIBRARY_SUFFIX}")

  include(ExternalProject)
  ExternalProject_Add(build-libplacebo
    GIT_REPOSITORY "${LIBPLACEBO_GIT_REPOSITORY}"
    GIT_TAG "${LIBPLACEBO_GIT_TAG}"
    GIT_SHALLOW TRUE
    PREFIX "${CMAKE_BINARY_DIR}/${CORE_BUILD_DIR}/build-libplacebo"
    INSTALL_DIR "${DEPENDS_PATH}"
    BUILD_IN_SOURCE 1
    BUILD_BYPRODUCTS "${LIBPLACEBO_LIBRARY}"
    CONFIGURE_COMMAND ${CMAKE_COMMAND} -E env --modify NINJA=set:${NINJA_EXECUTABLE}
                      ${MESON_EXECUTABLE} setup ./build
                      --buildtype=release
                      --default-library=static
                      --prefix=${DEPENDS_PATH}
                      --libdir=lib
                      -Dlibdovi=enabled
                      -Dopengl=enabled
                      -Ddemos=false
                      -Dtests=false
                      ${LIBPLACEBO_CROSS_FILE}
    BUILD_COMMAND ${NINJA_EXECUTABLE} -C ./build
    INSTALL_COMMAND ${NINJA_EXECUTABLE} -C ./build install
  )

  set_target_properties(build-libplacebo PROPERTIES FOLDER "External Projects")

  # libplacebo's pkg-config Requires.private dependencies. They are only linked
  # when pkg-config can find them, matching what the meson build enables.
  set(LIBPLACEBO_LINK_LIBRARIES)

  if(TARGET ${APP_NAME_LC}::LCMS2)
    list(APPEND LIBPLACEBO_LINK_LIBRARIES ${APP_NAME_LC}::LCMS2)
  endif()

  find_package(PkgConfig QUIET)
  if(PKG_CONFIG_FOUND)
    foreach(_module lcms2 dovi libunwind shaderc vulkan)
      string(TOUPPER ${_module} _module_upper)

      if(NOT (_module STREQUAL "lcms2" AND TARGET ${APP_NAME_LC}::LCMS2))
        pkg_check_modules(PC_LIBPLACEBO_${_module_upper} QUIET IMPORTED_TARGET ${_module})
        if(TARGET PkgConfig::PC_LIBPLACEBO_${_module_upper})
          list(APPEND LIBPLACEBO_LINK_LIBRARIES PkgConfig::PC_LIBPLACEBO_${_module_upper})
        endif()
      endif()
    endforeach()
  endif()

  if(TARGET Threads::Threads)
    list(APPEND LIBPLACEBO_LINK_LIBRARIES Threads::Threads)
  endif()

  list(APPEND LIBPLACEBO_LINK_LIBRARIES ${CMAKE_DL_LIBS})

  if(NOT WIN32)
    list(APPEND LIBPLACEBO_LINK_LIBRARIES m)
  endif()

  add_library(${APP_NAME_LC}::${CMAKE_FIND_PACKAGE_NAME} STATIC IMPORTED)

  set_target_properties(${APP_NAME_LC}::${CMAKE_FIND_PACKAGE_NAME} PROPERTIES
    IMPORTED_LOCATION "${LIBPLACEBO_LIBRARY}"
    INTERFACE_INCLUDE_DIRECTORIES "${DEPENDS_PATH}/include"
    INTERFACE_COMPILE_DEFINITIONS "HAS_PLACEBO;PL_STATIC"
    INTERFACE_LINK_LIBRARIES "${LIBPLACEBO_LINK_LIBRARIES}"
  )

  add_dependencies(${APP_NAME_LC}::${CMAKE_FIND_PACKAGE_NAME} build-libplacebo)

  add_library(LIBRARY::${CMAKE_FIND_PACKAGE_NAME} ALIAS ${APP_NAME_LC}::${CMAKE_FIND_PACKAGE_NAME})
endif()
