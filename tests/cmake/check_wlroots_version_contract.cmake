# Catch partial wlroots upgrades even when the server target is disabled.
if(NOT DEFINED WAYDISPLAY_SOURCE_DIR OR NOT EXISTS "${WAYDISPLAY_SOURCE_DIR}/CMakeLists.txt")
    message(FATAL_ERROR "WAYDISPLAY_SOURCE_DIR must point at the WayDisplay checkout")
endif()

foreach(_relative_path IN ITEMS
    PKGBUILD
    cmake/WayDisplayDependencies.cmake
    cmake/WayDisplayTargets.cmake
    .github/workflows/ci.yml
)
    file(READ "${WAYDISPLAY_SOURCE_DIR}/${_relative_path}" _contents)
    if(_contents MATCHES "wlroots[- ]?0\\.19|wlroots0\\.19")
        message(FATAL_ERROR "Stale wlroots 0.19 reference in ${_relative_path}")
    endif()
endforeach()

file(READ "${WAYDISPLAY_SOURCE_DIR}/PKGBUILD" _pkgbuild)
file(READ "${WAYDISPLAY_SOURCE_DIR}/cmake/WayDisplayDependencies.cmake" _dependencies)
file(READ "${WAYDISPLAY_SOURCE_DIR}/.github/workflows/ci.yml" _ci)
foreach(_contract IN ITEMS
    "'wlroots0.20'"
    "wlroots-0.20"
)
    string(FIND "${_pkgbuild}" "${_contract}" _offset)
    if(_offset EQUAL -1)
        message(FATAL_ERROR "Missing ${_contract} in PKGBUILD")
    endif()
endforeach()
string(FIND "${_dependencies}" "IMPORTED_TARGET \"wlroots-0.20>=0.20.0\"" _offset)
if(_offset EQUAL -1)
    message(FATAL_ERROR "CMake must require wlroots-0.20 >= 0.20.0")
endif()
string(FIND "${_ci}" "pkg-config --modversion wlroots-0.20" _offset)
if(_offset EQUAL -1)
    message(FATAL_ERROR "Full-runtime CI must check the wlroots-0.20 module")
endif()
