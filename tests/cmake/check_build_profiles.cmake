if(NOT DEFINED WAYDISPLAY_SOURCE_DIR)
    message(FATAL_ERROR "WAYDISPLAY_SOURCE_DIR is required")
endif()
if(NOT DEFINED WAYDISPLAY_TEST_ROOT)
    message(FATAL_ERROR "WAYDISPLAY_TEST_ROOT is required")
endif()
if(NOT DEFINED WAYDISPLAY_C_COMPILER OR
   NOT DEFINED WAYDISPLAY_CXX_COMPILER)
    message(FATAL_ERROR "GCC compiler paths are required")
endif()

function(waydisplay_require_text text pattern description)
    string(FIND "${text}" "${pattern}" index)
    if(index EQUAL -1)
        message(FATAL_ERROR
            "${description}: expected '${pattern}' in compile_commands.json")
    endif()
endfunction()

function(waydisplay_reject_text text pattern description)
    string(FIND "${text}" "${pattern}" index)
    if(NOT index EQUAL -1)
        message(FATAL_ERROR
            "${description}: unexpected '${pattern}' in compile_commands.json")
    endif()
endfunction()

function(waydisplay_configure_profile profile output_var)
    set(build_suffix "")
    set(log_level_argument)
    if(ARGC GREATER 2)
        string(TOLOWER "${ARGV2}" log_level_suffix)
        set(build_suffix "-log-${log_level_suffix}")
        set(log_level_argument "-DWAYDISPLAY_LOG_LEVEL=${ARGV2}")
    endif()

    set(build_dir "${WAYDISPLAY_TEST_ROOT}/${profile}${build_suffix}")
    file(REMOVE_RECURSE "${build_dir}")

    execute_process(
        COMMAND "${CMAKE_COMMAND}"
            -S "${WAYDISPLAY_SOURCE_DIR}"
            -B "${build_dir}"
            -G "Unix Makefiles"
            "-DCMAKE_BUILD_TYPE=${profile}"
            "-DCMAKE_C_COMPILER=${WAYDISPLAY_C_COMPILER}"
            "-DCMAKE_CXX_COMPILER=${WAYDISPLAY_CXX_COMPILER}"
            -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
            "-DWAYDISPLAY_PGO_DATA_DIR=${WAYDISPLAY_TEST_ROOT}/pgo-data"
            -DWAYDISPLAY_BUILD_CLIENT_SDL=OFF
            -DWAYDISPLAY_BUILD_WLROOTS_SERVER=OFF
            -DWAYDISPLAY_BUILD_TESTS=OFF
            -DWAYDISPLAY_ENABLE_AUDIO=OFF
            -DWAYDISPLAY_ENABLE_H264_SERVER_ENCODER=OFF
            -DWAYDISPLAY_ENABLE_H265_SERVER_ENCODER=OFF
            -DWAYDISPLAY_ENABLE_H264_CLIENT_DECODER=OFF
            -DWAYDISPLAY_ENABLE_H265_CLIENT_DECODER=OFF
            -DWAYDISPLAY_ENABLE_VAAPI_CLIENT_DECODER=OFF
            ${log_level_argument}
        RESULT_VARIABLE result
        OUTPUT_VARIABLE stdout
        ERROR_VARIABLE stderr
    )
    if(NOT result EQUAL 0)
        message(FATAL_ERROR
            "${profile} configure failed (${result})\n${stdout}\n${stderr}")
    endif()

    set(commands_file "${build_dir}/compile_commands.json")
    if(NOT EXISTS "${commands_file}")
        message(FATAL_ERROR
            "${profile} did not generate ${commands_file}")
    endif()
    file(READ "${commands_file}" commands)
    set(${output_var} "${commands}" PARENT_SCOPE)
endfunction()

file(REMOVE_RECURSE "${WAYDISPLAY_TEST_ROOT}")
file(MAKE_DIRECTORY "${WAYDISPLAY_TEST_ROOT}/pgo-data")

waydisplay_configure_profile(Debug debug_commands)
waydisplay_require_text("${debug_commands}" "-O0" "Debug optimization")
waydisplay_require_text("${debug_commands}" "-g3" "Debug symbols")
waydisplay_require_text("${debug_commands}" "-ggdb" "GDB symbols")
waydisplay_require_text("${debug_commands}" "-fno-omit-frame-pointer" "Debug frame pointers")
waydisplay_require_text("${debug_commands}" "WAYDISPLAY_LOG_LEVEL=4" "Debug log level")
waydisplay_reject_text("${debug_commands}" "-fprofile-generate=" "Debug PGO instrumentation")

waydisplay_configure_profile(Release release_commands)
waydisplay_require_text("${release_commands}" "-O3" "Release optimization")
waydisplay_require_text("${release_commands}" "-flto" "Release LTO")
waydisplay_require_text("${release_commands}" "WAYDISPLAY_LOG_LEVEL=2" "Release log level")
waydisplay_require_text("${release_commands}" "-DNDEBUG" "Release assertions")
waydisplay_reject_text("${release_commands}" "-march=native" "Release portability")
waydisplay_reject_text("${release_commands}" "-fprofile-use=" "Release PGO")

waydisplay_configure_profile(Release release_stats_commands STATS)
waydisplay_require_text("${release_stats_commands}" "WAYDISPLAY_LOG_LEVEL=3" "Stats log level override")

waydisplay_configure_profile(Profile profile_commands)
waydisplay_require_text("${profile_commands}" "-O3" "Profile optimization")
waydisplay_require_text("${profile_commands}" "-flto" "Profile LTO")
waydisplay_require_text("${profile_commands}" "-march=native" "Profile architecture")
waydisplay_require_text("${profile_commands}" "-fprofile-generate=" "Profile instrumentation")
waydisplay_require_text("${profile_commands}" "-fprofile-update=atomic" "Profile thread safety")
waydisplay_require_text("${profile_commands}" "WAYDISPLAY_LOG_LEVEL=2" "Profile log level")

# Native remains a fully optimized local build without profile data.
waydisplay_configure_profile(Native native_without_pgo_commands)
waydisplay_require_text("${native_without_pgo_commands}" "-O3" "Native optimization without PGO")
waydisplay_require_text("${native_without_pgo_commands}" "-flto" "Native LTO without PGO")
waydisplay_require_text("${native_without_pgo_commands}" "-march=native" "Native architecture without PGO")
waydisplay_reject_text("${native_without_pgo_commands}" "-fprofile-use=" "Native absent PGO data")

# Native enables profile consumption only when data is present. A marker with
# the same suffix is sufficient for this configure-only contract test.
file(WRITE "${WAYDISPLAY_TEST_ROOT}/pgo-data/profile-marker.gcda" "profile")
waydisplay_configure_profile(Native native_commands)
waydisplay_require_text("${native_commands}" "-O3" "Native optimization")
waydisplay_require_text("${native_commands}" "-flto" "Native LTO")
waydisplay_require_text("${native_commands}" "-march=native" "Native architecture")
waydisplay_require_text("${native_commands}" "-fprofile-use=" "Native PGO use")
waydisplay_require_text("${native_commands}" "-fprofile-correction" "Native PGO correction")
waydisplay_require_text("${native_commands}" "WAYDISPLAY_LOG_LEVEL=2" "Native log level")
