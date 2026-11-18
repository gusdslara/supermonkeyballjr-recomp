cmake_minimum_required(VERSION 3.16)
project(smbjr C)
if(WIN32)
	enable_language(RC)
endif()

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

# ---------------------------------------------------------------- SDL3
set(SDL_SHARED OFF CACHE BOOL "" FORCE)
set(SDL_STATIC ON CACHE BOOL "" FORCE)
set(SDL_TEST_LIBRARY OFF CACHE BOOL "" FORCE)
set(SDL_TESTS OFF CACHE BOOL "" FORCE)
set(SDL_EXAMPLES OFF CACHE BOOL "" FORCE)
if(ANDROID)
	# SDLActivity loads libSDL3.so + libmain.so
	set(SDL_SHARED ON CACHE BOOL "" FORCE)
	set(SDL_STATIC OFF CACHE BOOL "" FORCE)
endif()
add_subdirectory(externals/SDL EXCLUDE_FROM_ALL)

# ---------------------------------------------------------------- mGBA (libmgba only)
set(M_CORE_GBA ON CACHE BOOL "" FORCE)
set(M_CORE_GB OFF CACHE BOOL "" FORCE)
set(ENABLE_SCRIPTING OFF CACHE BOOL "" FORCE)
set(ENABLE_DEBUGGERS OFF CACHE BOOL "" FORCE)
set(ENABLE_GDB_STUB OFF CACHE BOOL "" FORCE)
set(USE_ZLIB OFF CACHE BOOL "" FORCE)
set(USE_PNG OFF CACHE BOOL "" FORCE)
set(USE_LIBZIP OFF CACHE BOOL "" FORCE)
set(USE_MINIZIP OFF CACHE BOOL "" FORCE)
set(USE_LZMA OFF CACHE BOOL "" FORCE)
set(USE_ELF OFF CACHE BOOL "" FORCE)
set(USE_SQLITE3 OFF CACHE BOOL "" FORCE)
set(USE_LUA OFF CACHE BOOL "" FORCE)
set(USE_JSON_C OFF CACHE BOOL "" FORCE)
set(USE_FFMPEG OFF CACHE BOOL "" FORCE)
set(USE_EDITLINE OFF CACHE BOOL "" FORCE)
set(USE_DISCORD_RPC OFF CACHE BOOL "" FORCE)
set(USE_EPOXY OFF CACHE BOOL "" FORCE)
set(BUILD_GL OFF CACHE BOOL "" FORCE)
set(BUILD_GLES2 OFF CACHE BOOL "" FORCE)
set(BUILD_GLES3 OFF CACHE BOOL "" FORCE)
set(LIBMGBA_ONLY ON CACHE BOOL "" FORCE)
if(EMSCRIPTEN)
	# Single-threaded build (works on any static host, no COOP/COEP headers); _GNU_SOURCE exposes PATH_MAX.
	add_compile_definitions(_GNU_SOURCE DISABLE_THREADING)
	# mGBA forces USE_PTHREADS on every UNIX target; patches/ turns that off for Emscripten.
	set(MGBA_PATCH ${CMAKE_CURRENT_SOURCE_DIR}/patches/mgba-emscripten-no-pthreads.patch)
	execute_process(COMMAND git apply --reverse --check ${MGBA_PATCH}
		WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/externals/mgba RESULT_VARIABLE MGBA_PATCHED OUTPUT_QUIET ERROR_QUIET)
	if(NOT MGBA_PATCHED EQUAL 0)
		execute_process(COMMAND git apply ${MGBA_PATCH}
			WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/externals/mgba RESULT_VARIABLE MGBA_APPLY)
		if(NOT MGBA_APPLY EQUAL 0)
			message(FATAL_ERROR "Could not apply ${MGBA_PATCH}")
		endif()
	endif()
endif()
add_subdirectory(externals/mgba EXCLUDE_FROM_ALL)

# ---------------------------------------------------------------- game shell
set(SMB_SRC
	src/main.c
	src/emu.c
	src/input.c
	src/view.c
	src/hud.c
	src/sprites.c
	src/recomp/recomp.c
)

# ---------------------------------------------------------------- static recompilation (phase 2)
# Point SMBJR_ROM at your own ROM to translate the game's code to C at build time. The generated
# code comes from the game, so it lives only in the build folder and is never committed.
set(SMBJR_ROM "" CACHE FILEPATH "Super Monkey Ball Jr. (USA) ROM to recompile (empty = interpreter only)")
if(SMBJR_ROM)
	find_package(Python3 REQUIRED COMPONENTS Interpreter)
	set(RC_GEN ${CMAKE_CURRENT_BINARY_DIR}/recomp)
	set(RC_SRC ${RC_GEN}/rc_table.c)
	foreach(i RANGE 0 31)
		if(i LESS 10)
			list(APPEND RC_SRC ${RC_GEN}/rc_0${i}.c)
		else()
			list(APPEND RC_SRC ${RC_GEN}/rc_${i}.c)
		endif()
	endforeach()
	set(RC_TOOL ${CMAKE_CURRENT_SOURCE_DIR}/tools/recomp)
	add_custom_command(OUTPUT ${RC_SRC} ${RC_GEN}/ops_thumb.h ${RC_GEN}/ops_arm.h
		COMMAND ${Python3_EXECUTABLE} ${RC_TOOL}/recomp.py --rom ${SMBJR_ROM}
			--mgba ${CMAKE_CURRENT_SOURCE_DIR}/externals/mgba --entries ${RC_TOOL}/entries.txt --ramcode ${RC_TOOL}/ramcode.txt --out ${RC_GEN}
		DEPENDS ${RC_TOOL}/recomp.py ${RC_TOOL}/gbarom.py ${RC_TOOL}/opnames.py ${RC_TOOL}/entries.txt
			${RC_TOOL}/ramcode.txt ${SMBJR_ROM}
			${CMAKE_CURRENT_SOURCE_DIR}/externals/mgba/src/arm/isa-thumb.c
			${CMAKE_CURRENT_SOURCE_DIR}/externals/mgba/src/arm/isa-arm.c
			${CMAKE_CURRENT_SOURCE_DIR}/externals/mgba/src/gba/hle-bios.c
		COMMENT "Recompiling Super Monkey Ball Jr. to C"
		VERBATIM)
	add_library(smbjr_recomp STATIC ${RC_SRC})
	target_include_directories(smbjr_recomp PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src/recomp ${RC_GEN})
	target_link_libraries(smbjr_recomp PRIVATE mgba)
	if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
		# Generated code is always optimized: it is the game itself.
		target_compile_options(smbjr_recomp PRIVATE -O2 -w)
	endif()
	set_target_properties(smbjr_recomp PROPERTIES POSITION_INDEPENDENT_CODE ON)
endif()

if(ANDROID)
	add_library(main SHARED ${SMB_SRC})
	set(SMB_TARGET main)
	target_link_libraries(main PRIVATE android log)
else()
	if(MINGW)
		# .exe icon. windres splits include paths at spaces, so compile the .rc without the target's include dirs.
		add_custom_command(OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/smbjr_rc.obj
			COMMAND ${CMAKE_RC_COMPILER} -O coff -i smbjr.rc -o ${CMAKE_CURRENT_BINARY_DIR}/smbjr_rc.obj
			WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/res
			DEPENDS res/smbjr.rc res/icon.ico)
		list(APPEND SMB_SRC ${CMAKE_CURRENT_BINARY_DIR}/smbjr_rc.obj)
	elseif(WIN32)
		list(APPEND SMB_SRC res/smbjr.rc)  # .exe icon
	endif()
	add_executable(smbjr ${SMB_SRC})
	set(SMB_TARGET smbjr)
	if(EMSCRIPTEN)
		set_target_properties(smbjr PROPERTIES SUFFIX ".html")
		# Relative: emcc cannot parse an @file path that contains spaces.
		file(RELATIVE_PATH EMCC_LIB_FUNCS ${CMAKE_CURRENT_BINARY_DIR} ${CMAKE_CURRENT_SOURCE_DIR}/web/emcc_lib_funcs.json)
		target_link_options(smbjr PRIVATE
			"SHELL:-s ALLOW_MEMORY_GROWTH=1"
			"SHELL:-s FORCE_FILESYSTEM=1"
			"SHELL:-s EXPORTED_RUNTIME_METHODS=['FS','IDBFS','ENV','ccall','addRunDependency','removeRunDependency']"
			"SHELL:-lidbfs.js"
			# SDL's software framebuffer (?sw=1) needs Emscripten's $Browser; a file avoids '$' escaping.
			"SHELL:-s DEFAULT_LIBRARY_FUNCS_TO_INCLUDE=@${EMCC_LIB_FUNCS}"
			"--shell-file=${CMAKE_CURRENT_SOURCE_DIR}/web/shell.html")
		# Favicon / home-screen icons next to smbjr.html (generated by android/make_icons.py).
		foreach(icon favicon.ico icon-192.png icon-512.png apple-touch-icon.png)
			add_custom_command(TARGET smbjr POST_BUILD COMMAND ${CMAKE_COMMAND} -E copy_if_different
				${CMAKE_CURRENT_SOURCE_DIR}/web/${icon} $<TARGET_FILE_DIR:smbjr>/${icon})
		endforeach()
	endif()
endif()

target_link_libraries(${SMB_TARGET} PRIVATE SDL3::SDL3 mgba)
if(SMBJR_ROM)
	target_link_libraries(${SMB_TARGET} PRIVATE smbjr_recomp)
	target_compile_definitions(${SMB_TARGET} PRIVATE SMBJR_RECOMP)
endif()
if(MINGW)
	target_link_options(${SMB_TARGET} PRIVATE -static -static-libgcc)
endif()
if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
	target_compile_options(${SMB_TARGET} PRIVATE -Wall -Wno-unused-parameter)
endif()
