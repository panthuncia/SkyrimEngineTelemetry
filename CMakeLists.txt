cmake_minimum_required(VERSION 3.25)

project(
	SkyrimEngineTelemetry
	VERSION 0.1.0
	LANGUAGES C CXX ASM_MASM
)

if("${PROJECT_SOURCE_DIR}" STREQUAL "${PROJECT_BINARY_DIR}")
	message(FATAL_ERROR "In-source builds are not supported. Use a build preset or a separate build directory.")
endif()

option(SET_ENABLE_AE "Build for Skyrim Anniversary Edition/CommonLibSSE AE offsets." ON)
set(SET_SKYRIM_DIR "" CACHE PATH "Optional Skyrim/MO2 mod root used by the deploy target and post-build auto-deploy.")
set(SET_INSTALL_DIR_FILE "${PROJECT_SOURCE_DIR}/install-prefix.txt" CACHE FILEPATH "Text file containing the install/deploy prefix.")

if(NOT SET_SKYRIM_DIR AND EXISTS "${SET_INSTALL_DIR_FILE}")
	file(READ "${SET_INSTALL_DIR_FILE}" SET_INSTALL_DIR_FROM_FILE)
	string(STRIP "${SET_INSTALL_DIR_FROM_FILE}" SET_INSTALL_DIR_FROM_FILE)
	if(SET_INSTALL_DIR_FROM_FILE)
		file(TO_CMAKE_PATH "${SET_INSTALL_DIR_FROM_FILE}" SET_INSTALL_DIR_FROM_FILE)
		set(SET_SKYRIM_DIR "${SET_INSTALL_DIR_FROM_FILE}" CACHE PATH "Optional Skyrim/MO2 mod root used by the deploy target and post-build auto-deploy." FORCE)
		message(STATUS "Using install directory from ${SET_INSTALL_DIR_FILE}: ${SET_SKYRIM_DIR}")
	endif()
endif()

# ---- Dependencies ----

# Tracy comes from the ThirdParty/tracy submodule (pinned to v0.14.0) rather than vcpkg.
# Tracy's CMake defaults TRACY_ENABLE to OFF, which compiles every zone out.
# On-demand: nothing is recorded until a profiler connects.
set(TRACY_ENABLE ON CACHE BOOL "Enable profiling" FORCE)
set(TRACY_ON_DEMAND ON CACHE BOOL "On-demand profiling" FORCE)
add_subdirectory(ThirdParty/tracy EXCLUDE_FROM_ALL)
# Satisfy find_package(Tracy CONFIG) calls (e.g. BasicTelemetry's) with the submodule target.
file(WRITE "${CMAKE_FIND_PACKAGE_REDIRECTS_DIR}/tracy-config.cmake" "# Provided by ThirdParty/tracy via add_subdirectory\n")

set(BASICTELEMETRY_BUILD_ARTIFACTS OFF CACHE BOOL "Build BasicTelemetry artifact writers" FORCE)
set(BASICTELEMETRY_BUILD_TOOLS OFF CACHE BOOL "Build BasicTelemetry tools" FORCE)
set(BASICTELEMETRY_BUILD_TESTS OFF CACHE BOOL "Build BasicTelemetry tests" FORCE)
set(BASICTELEMETRY_ENABLE_TRACY ON CACHE BOOL "Build Tracy integration" FORCE)
add_subdirectory(BasicTelemetry EXCLUDE_FROM_ALL)

set(SKYRIM_SUPPORT_AE "${SET_ENABLE_AE}" CACHE BOOL "Enable AE support in CommonLibSSE." FORCE)
add_subdirectory(CommonLibSSE EXCLUDE_FROM_ALL)

find_package(spdlog REQUIRED CONFIG)

find_path(DETOURS_INCLUDE_DIRS "detours/detours.h" REQUIRED)
set(_set_vcpkg_installed_dir "${VCPKG_INSTALLED_DIR}")
if(NOT _set_vcpkg_installed_dir)
	set(_set_vcpkg_installed_dir "${_VCPKG_INSTALLED_DIR}")
endif()
find_library(
	SET_DETOURS_RELEASE_LIBRARY
	NAMES detours
	PATHS "${_set_vcpkg_installed_dir}/${VCPKG_TARGET_TRIPLET}/lib"
	NO_DEFAULT_PATH
	REQUIRED
)
find_library(
	SET_DETOURS_DEBUG_LIBRARY
	NAMES detours
	PATHS "${_set_vcpkg_installed_dir}/${VCPKG_TARGET_TRIPLET}/debug/lib"
	NO_DEFAULT_PATH
	REQUIRED
)
add_library(SETDetours STATIC IMPORTED)
add_library(SET::Detours ALIAS SETDetours)
set_target_properties(SETDetours PROPERTIES
	IMPORTED_LOCATION "${SET_DETOURS_RELEASE_LIBRARY}"
	IMPORTED_LOCATION_DEBUG "${SET_DETOURS_DEBUG_LIBRARY}"
	IMPORTED_LOCATION_RELEASE "${SET_DETOURS_RELEASE_LIBRARY}"
	IMPORTED_LOCATION_RELWITHDEBINFO "${SET_DETOURS_RELEASE_LIBRARY}"
	IMPORTED_LOCATION_MINSIZEREL "${SET_DETOURS_RELEASE_LIBRARY}"
	INTERFACE_INCLUDE_DIRECTORIES "${DETOURS_INCLUDE_DIRS}"
)

# ---- Plugin ----

set(SET_SOURCES
	src/PCH.h
	src/Plugin.cpp
	src/Settings.cpp
	src/Settings.h
	src/Hooks/Detours.cpp
	src/Hooks/Detours.h
	src/Hooks/FrameHooks.cpp
	src/Hooks/FrameHooks.h
	src/Hooks/ZoneHooks.cpp
	src/Hooks/ZoneHooks.h
	src/Report/ReportWriter.cpp
	src/Report/ReportWriter.h
	src/Sampler/ModuleMap.cpp
	src/Sampler/ModuleMap.h
	src/Sampler/StackSampler.cpp
	src/Sampler/StackSampler.h
	src/Sampler/Symbolizer.cpp
	src/Sampler/Symbolizer.h
	src/Telemetry/Zone.cpp
	src/Telemetry/Zone.h
	src/Threads/ThreadHooks.cpp
	src/Threads/ThreadHooks.h
	src/Threads/ThreadRegistry.cpp
	src/Threads/ThreadRegistry.h
	src/Util/Memory.cpp
	src/Util/Memory.h
)

source_group(
	TREE "${CMAKE_CURRENT_SOURCE_DIR}"
	FILES ${SET_SOURCES}
)

# The zone hook stub is built on its own so it does not inherit the C++
# compile options that CommonLibSSE propagates (ml64 rejects them).
add_library(SETZoneHookStub OBJECT src/Hooks/ZoneHookStub.asm)
target_compile_options(SETZoneHookStub PRIVATE /Zi /W3 /WX)

add_library("${PROJECT_NAME}" SHARED ${SET_SOURCES} $<TARGET_OBJECTS:SETZoneHookStub>)

target_compile_features("${PROJECT_NAME}" PRIVATE cxx_std_23)

target_link_libraries("${PROJECT_NAME}" PRIVATE CommonLibSSE::CommonLibSSE BasicTelemetry::Tracy spdlog::spdlog SET::Detours)

target_include_directories("${PROJECT_NAME}" PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")

target_precompile_headers("${PROJECT_NAME}" PRIVATE src/PCH.h)

target_compile_definitions("${PROJECT_NAME}" PRIVATE SET_PLUGIN_VERSION_MAJOR=${PROJECT_VERSION_MAJOR} SET_PLUGIN_VERSION_MINOR=${PROJECT_VERSION_MINOR} SET_PLUGIN_VERSION_PATCH=${PROJECT_VERSION_PATCH})

set_target_properties(
	"${PROJECT_NAME}"
	PROPERTIES
		OUTPUT_NAME "${PROJECT_NAME}"
		PREFIX ""
		RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/out/$<CONFIG>/SKSE/Plugins"
		LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/out/$<CONFIG>/SKSE/Plugins"
		ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib/$<CONFIG>"
)

if(MSVC)
	# C++ only: ml64 rejects these.
	target_compile_options(
		"${PROJECT_NAME}"
		PRIVATE
			$<$<COMPILE_LANGUAGE:CXX>:/EHsc /MP /W4 /permissive- /Zc:preprocessor /Zc:__cplusplus /external:anglebrackets /external:W0 /Zi>
	)
	if(NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang")
		target_compile_options("${PROJECT_NAME}" PRIVATE $<$<COMPILE_LANGUAGE:CXX>:/WX>)
	endif()

	target_link_options("${PROJECT_NAME}" PRIVATE /DEBUG:FULL)

	target_compile_definitions("${PROJECT_NAME}" PRIVATE _AMD64_ _UNICODE UNICODE)
endif()

install(
	TARGETS "${PROJECT_NAME}"
	RUNTIME
		DESTINATION "SKSE/Plugins"
		COMPONENT Plugin
	LIBRARY
		DESTINATION "SKSE/Plugins"
		COMPONENT Plugin
)

install(
	FILES "$<TARGET_PDB_FILE:${PROJECT_NAME}>"
	DESTINATION "SKSE/Plugins"
	OPTIONAL
	COMPONENT Plugin
)

# Preserve local sampler settings, but always deploy the repository's zone
# catalog so newly added instrumentation is exercised by the next build/run.
install(
	CODE "
		set(_set_config_dir \"\$ENV{DESTDIR}\${CMAKE_INSTALL_PREFIX}/SKSE/Plugins\")
		if(NOT EXISTS \"\${_set_config_dir}/SkyrimEngineTelemetry.ini\")
			file(INSTALL DESTINATION \"\${_set_config_dir}\" TYPE FILE FILES \"${CMAKE_CURRENT_SOURCE_DIR}/data/SKSE/Plugins/SkyrimEngineTelemetry.ini\")
		endif()
	"
	COMPONENT Plugin
)

install(
	FILES "${CMAKE_CURRENT_SOURCE_DIR}/data/SKSE/Plugins/SkyrimEngineTelemetry.zones"
	DESTINATION "SKSE/Plugins"
	COMPONENT Plugin
)

if(SET_SKYRIM_DIR)
	add_custom_command(
		TARGET "${PROJECT_NAME}"
		POST_BUILD
		COMMAND "${CMAKE_COMMAND}" --install "${CMAKE_BINARY_DIR}"
			--config "$<CONFIG>"
			--prefix "${SET_SKYRIM_DIR}"
			--component Plugin
		COMMENT "Auto-deploying ${PROJECT_NAME} to ${SET_SKYRIM_DIR}"
		VERBATIM
	)

	add_custom_target(
		deploy
		COMMAND
			"${CMAKE_COMMAND}" --install "${CMAKE_BINARY_DIR}"
			--config "$<CONFIG>"
			--prefix "${SET_SKYRIM_DIR}"
			--component Plugin
		DEPENDS "${PROJECT_NAME}"
		COMMENT "Deploying ${PROJECT_NAME} to ${SET_SKYRIM_DIR}"
		VERBATIM
	)
endif()
