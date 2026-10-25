cmake_minimum_required(VERSION 3.20)
project(grimnumbers LANGUAGES C CXX ASM_MASM)

if(NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(FATAL_ERROR "Grim Numbers targets x64 only - Grim Dawn's x64 folder.")
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(MSVC)
    # Scoped to C/C++: ml64 rejects /permissive- and /W4.
    add_compile_options("$<$<COMPILE_LANGUAGE:C,CXX>:/W4>"
                        "$<$<COMPILE_LANGUAGE:C,CXX>:/permissive->"
                        "$<$<COMPILE_LANGUAGE:C,CXX>:/Zi>")
    add_compile_definitions(WIN32_LEAN_AND_MEAN NOMINMAX _CRT_SECURE_NO_WARNINGS)
    add_link_options(/DEBUG)
    # Static CRT so the proxy does not drag an extra runtime into the game.
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

# ---- MinHook ------------------------------------------------------------
add_library(minhook STATIC
    third_party/minhook/src/buffer.c
    third_party/minhook/src/hook.c
    third_party/minhook/src/trampoline.c
    third_party/minhook/src/hde/hde64.c
)
target_include_directories(minhook PUBLIC third_party/minhook/include)
target_include_directories(minhook PRIVATE third_party/minhook/src)
set_property(TARGET minhook PROPERTY
    MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
if(MSVC)
    target_compile_options(minhook PRIVATE /W0)
endif()

# ---- generated pass-through export table --------------------------------
# The proxy shadows a system DLL for the whole process, so it must export
# everything that DLL does - not just what the game itself imports. The table
# is generated from the local system DLL at configure time.
set(GN_PROXY_DLL "dinput8" CACHE STRING "System DLL to shadow (without .dll)")
set(GN_GENERATED "${CMAKE_BINARY_DIR}/generated")

find_package(Python3 COMPONENTS Interpreter REQUIRED)
execute_process(
    COMMAND "${Python3_EXECUTABLE}" -I
            "${CMAKE_CURRENT_SOURCE_DIR}/tools/gen_proxy_exports.py"
            "$ENV{SystemRoot}/System32/${GN_PROXY_DLL}.dll"
            "${GN_GENERATED}"
            "${GN_PROXY_DLL}"
    RESULT_VARIABLE GN_GEN_RESULT
    OUTPUT_VARIABLE GN_GEN_OUTPUT
    ERROR_VARIABLE  GN_GEN_OUTPUT)
if(NOT GN_GEN_RESULT EQUAL 0)
    message(FATAL_ERROR "export generation failed:
${GN_GEN_OUTPUT}")
endif()
message(STATUS "${GN_GEN_OUTPUT}")

# ---- proxy DLL ----------------------------------------------------------
add_library(grimnumbers_proxy SHARED
    src/hook/dllmain.cpp
    src/hook/gd_symbols.cpp
    src/hook/hooks.cpp
    src/hook/pipe_server.cpp
    src/hook/log.cpp
    "${GN_GENERATED}/proxy_targets.cpp"
    "${GN_GENERATED}/proxy_thunks.asm"
)
target_include_directories(grimnumbers_proxy PRIVATE include src/hook)
target_link_libraries(grimnumbers_proxy PRIVATE minhook)
set_target_properties(grimnumbers_proxy PROPERTIES
    OUTPUT_NAME "${GN_PROXY_DLL}"
    PREFIX ""
)
if(MSVC)
    # LNK4222: the linker would rather the COM entry points had no ordinal. A
    # proxy wants the ordinal map to match the DLL it stands in for, so keep
    # the ordinals and silence that one warning.
    target_link_options(grimnumbers_proxy PRIVATE
        "/DEF:${GN_GENERATED}/proxy_exports.def"
        "/IGNORE:4222")
endif()

# ---- client modules -----------------------------------------------------
# Shared by the console readout and the windowed app: the stats model, the
# pipe reader, value formatting and the overlay layout parser. Neither front
# end reimplements any of it.
add_library(grimnumbers_core STATIC
    src/client/stats.cpp
    src/client/pipe_client.cpp
    src/client/fields.cpp
    src/client/layout.cpp
)
target_include_directories(grimnumbers_core PUBLIC include src/client)

# ---- console client -----------------------------------------------------
add_executable(grimnumbers_client src/client/main.cpp)
target_link_libraries(grimnumbers_client PRIVATE grimnumbers_core)

# ---- windowed app + in-game overlay ------------------------------------
# The overlay is a layered, click-through window drawn with Direct2D and
# DirectWrite - both part of Windows, so there is no third-party renderer and
# nothing is hooked inside the game process.
add_executable(grimnumbers_app WIN32
    src/client/app_main.cpp
    src/client/appwindow.cpp
    src/client/overlay.cpp
)
target_link_libraries(grimnumbers_app PRIVATE grimnumbers_core d2d1 dwrite)

# The app reads overlay.ini and elements.ini from its own directory, so seed
# the shipped defaults there - but only when they are missing, since those
# copies are meant to be edited and a build must not overwrite them.
foreach(cfg overlay elements)
    add_custom_command(TARGET grimnumbers_app POST_BUILD
        COMMAND ${CMAKE_COMMAND}
                "-DSRC=${CMAKE_CURRENT_SOURCE_DIR}/config/${cfg}.ini"
                "-DDST=$<TARGET_FILE_DIR:grimnumbers_app>/${cfg}.ini"
                -P "${CMAKE_CURRENT_SOURCE_DIR}/tools/copy_if_absent.cmake")
endforeach()

# ---- install helper ----------------------------------------------------
# Copies the proxy next to "Grim Dawn.exe". Point GD_X64_DIR at the x64 folder.
set(GD_X64_DIR "" CACHE PATH "Grim Dawn x64 directory (where Grim Dawn.exe lives)")
if(GD_X64_DIR)
    add_custom_target(install_proxy
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                $<TARGET_FILE:grimnumbers_proxy> "${GD_X64_DIR}/${GN_PROXY_DLL}.dll"
        DEPENDS grimnumbers_proxy
        COMMENT "Copying proxy ${GN_PROXY_DLL}.dll to ${GD_X64_DIR}")
endif()

# ---- smoke test --------------------------------------------------------
# Drives the real pipe server with synthetic events, no game required.
add_executable(grimnumbers_pipe_smoke
    tools/pipe_smoke.cpp
    src/hook/pipe_server.cpp
    src/hook/log.cpp
)
target_include_directories(grimnumbers_pipe_smoke PRIVATE include src/hook)

# ---- keep the build path out of the binaries ----------------------------
# By default the linker records the .pdb's absolute path inside each binary,
# so a shipped DLL would carry whatever directory it happened to be built in.
# /PDBALTPATH replaces that with the bare filename. The linker's own %_PDB%
# macro would do it, but MSBuild mangles bare % signs, so name the file
# explicitly instead.
if(MSVC)
    foreach(t grimnumbers_proxy grimnumbers_core grimnumbers_client
              grimnumbers_app grimnumbers_pipe_smoke)
        get_target_property(_type ${t} TYPE)
        if(NOT _type STREQUAL "STATIC_LIBRARY")
            target_link_options(${t} PRIVATE
                "/PDBALTPATH:$<TARGET_PDB_FILE_NAME:${t}>")
        endif()
    endforeach()
endif()
