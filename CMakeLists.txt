cmake_minimum_required(VERSION 3.25)
project(DeckHand VERSION 0.1.0 LANGUAGES C CXX)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()
option(DECKHAND_WITH_API "Build the REST/WebSocket API server into the app (needs OpenSSL)" ON)
option(DECKHAND_WITH_SQLCIPHER "Link SQLCipher (needed to import encrypted source libraries)" ON)
enable_testing()
if(MSVC)
  add_compile_options(/W4 /utf-8 /permissive- /Zc:__cplusplus /EHsc /wd4100 /wd4244 /wd4267 /wd4127)
else()
  add_compile_options(-Wall -Wextra)
endif()
if(WIN32)
  # Windows 10 and later. NOMINMAX / LEAN_AND_MEAN keep <windows.h> from breaking std::min and the winsock order.
  add_compile_definitions(_WIN32_WINNT=0x0A00 WINVER=0x0A00 NTDDI_VERSION=0x0A000000 WIN32_LEAN_AND_MEAN NOMINMAX
    _USE_MATH_DEFINES _CRT_SECURE_NO_WARNINGS)
  enable_language(RC)
  if(MINGW)
    add_compile_definitions(__USE_MINGW_ANSI_STDIO=1)  # C99 printf/snprintf (%zu, %lld) instead of the old msvcrt ones
  endif()
endif()

# ---- third party -----------------------------------------------------------
add_library(dh_sqlite INTERFACE)
if(NOT APPLE AND NOT WIN32)
  find_package(PkgConfig REQUIRED)
endif()
if(DECKHAND_WITH_SQLCIPHER AND APPLE)
  set(SQLCIPHER_ROOT "/opt/homebrew/opt/sqlcipher" CACHE PATH "SQLCipher prefix")
  set(OPENSSL_CRYPTO "/opt/homebrew/opt/openssl@4/lib/libcrypto.a" CACHE FILEPATH "libcrypto used by SQLCipher")
  target_include_directories(dh_sqlite SYSTEM INTERFACE ${SQLCIPHER_ROOT}/include/sqlcipher)
  target_compile_definitions(dh_sqlite INTERFACE SQLITE_HAS_CODEC DECKHAND_WITH_SQLCIPHER=1)
  target_link_libraries(dh_sqlite INTERFACE ${SQLCIPHER_ROOT}/lib/libsqlcipher.a ${OPENSSL_CRYPTO} z)
elseif(DECKHAND_WITH_SQLCIPHER AND WIN32)
  # Windows: SQLCipher built against OpenSSL (scripts/windows/Dockerfile for MinGW, vcpkg's `sqlcipher` port for MSVC).
  # Headers are looked up as <prefix>/include/sqlcipher/sqlite3.h so a plain SQLite header is never picked up.
  find_package(OpenSSL REQUIRED)
  set(_dh_prefixes ${CMAKE_PREFIX_PATH} ${CMAKE_FIND_ROOT_PATH} ${SQLCIPHER_ROOT})
  find_path(SQLCIPHER_INCLUDE_DIR sqlite3.h HINTS ${_dh_prefixes} PATH_SUFFIXES include/sqlcipher NO_DEFAULT_PATH)
  find_library(SQLCIPHER_LIBRARY NAMES sqlcipher libsqlcipher HINTS ${_dh_prefixes} PATH_SUFFIXES lib NO_DEFAULT_PATH)
  if(NOT SQLCIPHER_INCLUDE_DIR OR NOT SQLCIPHER_LIBRARY)
    message(FATAL_ERROR "SQLCipher not found: set CMAKE_PREFIX_PATH (or SQLCIPHER_ROOT) to a prefix with include/sqlcipher/sqlite3.h and lib/sqlcipher")
  endif()
  target_include_directories(dh_sqlite SYSTEM INTERFACE ${SQLCIPHER_INCLUDE_DIR})
  target_compile_definitions(dh_sqlite INTERFACE SQLITE_HAS_CODEC DECKHAND_WITH_SQLCIPHER=1)
  # An imported target, so that CMake keeps libsqlcipher before the libcrypto it needs on the static link line.
  add_library(dh_sqlcipher_lib STATIC IMPORTED)
  set_target_properties(dh_sqlcipher_lib PROPERTIES IMPORTED_LOCATION "${SQLCIPHER_LIBRARY}"
    INTERFACE_LINK_LIBRARIES "OpenSSL::Crypto;crypt32;ws2_32;advapi32;user32;gdi32")
  target_link_libraries(dh_sqlite INTERFACE dh_sqlcipher_lib)
elseif(DECKHAND_WITH_SQLCIPHER)
  # Linux: the distribution's SQLCipher (libsqlcipher-dev), found through pkg-config.
  pkg_check_modules(SQLCIPHER REQUIRED IMPORTED_TARGET sqlcipher)
  target_compile_definitions(dh_sqlite INTERFACE SQLITE_HAS_CODEC DECKHAND_WITH_SQLCIPHER=1)
  target_link_libraries(dh_sqlite INTERFACE PkgConfig::SQLCIPHER)
else()
  find_package(SQLite3 REQUIRED)
  target_link_libraries(dh_sqlite INTERFACE SQLite::SQLite3)
endif()

add_library(dh_miniaudio STATIC third_party/miniaudio/miniaudio_impl.c)
target_include_directories(dh_miniaudio SYSTEM PUBLIC third_party/miniaudio)
target_compile_definitions(dh_miniaudio PUBLIC MA_NO_DECODING MA_NO_ENCODING MA_NO_GENERATION
  MA_NO_RESOURCE_MANAGER MA_NO_NODE_GRAPH MA_NO_ENGINE)
target_compile_options(dh_miniaudio PRIVATE $<IF:$<C_COMPILER_ID:MSVC>,/w,-w>)
if(APPLE)
  target_compile_definitions(dh_miniaudio PUBLIC MA_NO_RUNTIME_LINKING)
  target_link_libraries(dh_miniaudio PUBLIC "-framework CoreFoundation" "-framework CoreAudio" "-framework AudioToolbox")
elseif(WIN32)
  # Windows: WASAPI (shared mode) through miniaudio; its COM and mmdevice entry points are resolved at run time.
  target_link_libraries(dh_miniaudio PUBLIC ole32 winmm)
else()
  # Linux: leave runtime linking on. miniaudio then dlopen()s PulseAudio, ALSA and JACK when the output
  # opens and picks the first that works, so the binary needs none of them at build or load time.
  find_package(Threads REQUIRED)
  target_link_libraries(dh_miniaudio PUBLIC Threads::Threads ${CMAKE_DL_LIBS} m)
endif()

# ---- schema embedded from the single source file ----------------------------
file(READ src/library/schema.sql DH_SCHEMA_SQL)
file(WRITE ${CMAKE_BINARY_DIR}/gen/deckhand_schema.inc "R\"DHSQL(${DH_SCHEMA_SQL})DHSQL\"")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS src/library/schema.sql)

# dh_lib(name DEPS a b ...): static library from src/<name>/*.cpp, headers in src/<name>/include
function(dh_lib name)
  cmake_parse_arguments(A "" "" "DEPS" ${ARGN})
  file(GLOB srcs CONFIGURE_DEPENDS src/${name}/*.cpp)
  add_library(dh_${name} STATIC ${srcs})
  target_include_directories(dh_${name} PUBLIC src/${name}/include)
  foreach(d ${A_DEPS})
    target_link_libraries(dh_${name} PUBLIC dh_${d})
  endforeach()
endfunction()

add_library(dh_core INTERFACE)
target_include_directories(dh_core INTERFACE src/core/include)

dh_lib(log       DEPS core)
dh_lib(i18n      DEPS core log)
dh_lib(library   DEPS core log sqlite)
target_include_directories(dh_library PRIVATE ${CMAKE_BINARY_DIR}/gen)
dh_lib(import    DEPS core log library)
dh_lib(export    DEPS core log)
dh_lib(analysis  DEPS core log)
dh_lib(audio     DEPS core log miniaudio)
dh_lib(midi      DEPS core log audio)
dh_lib(broadcast DEPS core log)
dh_lib(sync      DEPS core log library)
dh_lib(api       DEPS core log)
if(APPLE)
  set(OPENSSL_ROOT "/opt/homebrew/opt/openssl@4" CACHE PATH "OpenSSL prefix (TLS for the API server)")
  target_include_directories(dh_api SYSTEM PRIVATE ${OPENSSL_ROOT}/include)
  target_link_libraries(dh_api PUBLIC ${OPENSSL_ROOT}/lib/libssl.a ${OPENSSL_ROOT}/lib/libcrypto.a)
else()
  if(WIN32)
    set(OPENSSL_USE_STATIC_LIBS ON)
  endif()
  find_package(OpenSSL REQUIRED)
  target_link_libraries(dh_api PUBLIC OpenSSL::SSL OpenSSL::Crypto)
  if(WIN32)
    target_link_libraries(dh_api PUBLIC ws2_32 crypt32 advapi32 user32 gdi32)
  endif()
endif()
dh_lib(skin      DEPS core log i18n)
dh_lib(gfx       DEPS core log)
dh_lib(services  DEPS core log library audio export analysis)
# The API server needs OpenSSL; platforms without it (iOS) build with -DDECKHAND_WITH_API=OFF.
if(DECKHAND_WITH_API)
  dh_lib(app     DEPS core log library import audio midi gfx services api i18n skin broadcast sync)
  target_compile_definitions(dh_app PRIVATE DECKHAND_WITH_API=1)
else()
  dh_lib(app     DEPS core log library import audio midi gfx services i18n skin broadcast sync)
  target_compile_definitions(dh_app PRIVATE DECKHAND_WITH_API=0)
endif()

# ---- platform glue (C APIs shared by macOS and iOS) ---------------------------
if(APPLE)
  file(GLOB apple_srcs CONFIGURE_DEPENDS platform/apple/*.cpp)
  add_library(dh_platform STATIC ${apple_srcs})
  target_link_libraries(dh_platform PUBLIC dh_audio dh_midi
    "-framework AudioToolbox" "-framework CoreMIDI" "-framework CoreFoundation" "-framework CoreAudio")
elseif(WIN32)
  # Windows: Media Foundation decodes (plus built-in PCM WAV/AIFF readers), WinMM does MIDI.
  file(GLOB win_srcs CONFIGURE_DEPENDS platform/windows/*.cpp)
  add_library(dh_platform STATIC ${win_srcs})
  target_link_libraries(dh_platform PUBLIC dh_audio dh_midi mfplat mfreadwrite mfuuid propsys ole32 oleaut32 uuid winmm)
  # IPC (shared memory, control channel, process spawn) and the control sockets.
  target_link_libraries(dh_audio PUBLIC ws2_32 bcrypt advapi32 ole32)
  target_link_libraries(dh_broadcast PUBLIC ws2_32)
  target_link_libraries(dh_api PUBLIC ws2_32)
  # src/app uses a few POSIX names (localtime_r, chmod, ssize_t, __attribute__): map them from the build side.
  target_compile_options(dh_app PRIVATE $<IF:$<CXX_COMPILER_ID:MSVC>,/FI${CMAKE_SOURCE_DIR}/platform/windows/compat/dh_compat.h,-include${CMAKE_SOURCE_DIR}/platform/windows/compat/dh_compat.h>)
  if(MINGW)
    # Run anywhere without the MinGW runtime DLLs next to the exe; winpthread is linked whole so std::thread works.
    # libwinpthread.a carries its own VERSIONINFO resource, which would clash with ours: link a copy without it.
    execute_process(COMMAND ${CMAKE_CXX_COMPILER} -print-file-name=libwinpthread.a OUTPUT_VARIABLE _dh_pthread_a
                    OUTPUT_STRIP_TRAILING_WHITESPACE)
    set(_dh_pthread_flag -lwinpthread)
    if(EXISTS "${_dh_pthread_a}")
      set(_dh_pthread_copy ${CMAKE_BINARY_DIR}/libwinpthread_nores.a)
      file(COPY_FILE "${_dh_pthread_a}" ${_dh_pthread_copy})
      execute_process(COMMAND ${CMAKE_AR} d ${_dh_pthread_copy} version.o ERROR_QUIET)
      set(_dh_pthread_flag ${_dh_pthread_copy})
    endif()
    add_link_options(-static -static-libgcc -static-libstdc++ -Wl,--whole-archive ${_dh_pthread_flag} -Wl,--no-whole-archive)
  endif()
elseif(UNIX)
  # Linux: FFmpeg (libavformat/libavcodec/libswresample, LGPL, dynamic) decodes; the ALSA sequencer does MIDI.
  pkg_check_modules(FFMPEG REQUIRED IMPORTED_TARGET libavformat libavcodec libswresample libavutil)
  pkg_check_modules(ALSA REQUIRED IMPORTED_TARGET alsa)
  file(GLOB linux_srcs CONFIGURE_DEPENDS platform/linux/*.cpp)
  add_library(dh_platform STATIC ${linux_srcs})
  target_link_libraries(dh_platform PUBLIC dh_audio dh_midi PkgConfig::FFMPEG PkgConfig::ALSA)
endif()

# ---- programs -----------------------------------------------------------------
# The audio engine in its own process: owns the output device, recorder and controller hub so the UI can crash
# or hang without stopping the music (ARCHITECTURE.md, "Engine process").
file(GLOB enginehost_srcs CONFIGURE_DEPENDS src/enginehost/*.cpp)
# Console programs on Windows carry a UTF-8 manifest and version info (platform/windows/resources/console.rc).
if(WIN32)
  set(DH_CONSOLE_RC platform/windows/resources/console.rc)
else()
  set(DH_CONSOLE_RC "")
endif()
add_executable(deckhand-engine ${enginehost_srcs} ${DH_CONSOLE_RC})
target_link_libraries(deckhand-engine dh_midi dh_audio dh_platform dh_log)

add_executable(deckhandctl src/cli/main.cpp ${DH_CONSOLE_RC})
target_link_libraries(deckhandctl dh_library dh_import dh_audio dh_services dh_api dh_platform)

file(GLOB test_srcs CONFIGURE_DEPENDS tests/test_*.cpp)
foreach(t ${test_srcs})
  get_filename_component(n ${t} NAME_WE)
  add_executable(${n} ${t} ${DH_CONSOLE_RC})
  target_link_libraries(${n} dh_library dh_import dh_export dh_analysis dh_audio dh_midi dh_gfx dh_api dh_skin dh_i18n
    dh_broadcast dh_sync dh_platform)
  add_test(NAME ${n} COMMAND ${n} WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
endforeach()

# ---- Linux platform tests (decoder, ALSA sequencer, text rasteriser) -------------------------
if(UNIX AND NOT APPLE)
  add_executable(test_linux_decoder platform/linux/tests/decoder_test.cpp)
  target_link_libraries(test_linux_decoder dh_platform dh_audio)
  add_test(NAME test_linux_decoder COMMAND test_linux_decoder WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
  add_executable(test_linux_midi platform/linux/tests/midi_test.cpp)
  target_link_libraries(test_linux_midi dh_platform dh_midi PkgConfig::ALSA)
  add_test(NAME test_linux_midi COMMAND test_linux_midi WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
  add_executable(test_linux_engine_fallback platform/linux/tests/engine_fallback_test.cpp)
  target_link_libraries(test_linux_engine_fallback dh_audio dh_platform)
  add_test(NAME test_linux_engine_fallback COMMAND test_linux_engine_fallback WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
endif()

# ---- Linux shell (SDL3 window, OpenGL 3.3 renderer, Pango text) ---------------------------
if(UNIX AND NOT APPLE)
  option(DECKHAND_BUILD_SHELL "Build the Linux desktop shell (needs SDL3, OpenGL and Pango)" ON)
  if(DECKHAND_BUILD_SHELL)
    find_package(SDL3 CONFIG REQUIRED)
    pkg_check_modules(PANGO REQUIRED IMPORTED_TARGET pangocairo)
    file(GLOB shell_srcs CONFIGURE_DEPENDS platform/linux/shell/*.cpp)
    add_executable(deckhand ${shell_srcs})
    target_include_directories(deckhand PRIVATE src/app/include)
    target_link_libraries(deckhand dh_app dh_platform SDL3::SDL3 PkgConfig::PANGO)
    add_executable(test_linux_text platform/linux/tests/text_test.cpp platform/linux/shell/text.cpp
                   platform/linux/shell/renderer.cpp)
    target_include_directories(test_linux_text PRIVATE src/app/include)
    target_link_libraries(test_linux_text SDL3::SDL3 PkgConfig::PANGO)
    add_test(NAME test_linux_text COMMAND test_linux_text WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
    # Resources next to the binary in the share/deckhand layout (controllers/, lang/, skins/).
    set(DH_SHARE ${CMAKE_BINARY_DIR}/share/deckhand)
    add_custom_command(TARGET deckhand POST_BUILD
      COMMAND ${CMAKE_COMMAND} -E make_directory ${DH_SHARE}/controllers
      COMMAND ${CMAKE_COMMAND} -E copy_directory ${CMAKE_SOURCE_DIR}/controllers ${DH_SHARE}/controllers
      COMMAND ${CMAKE_COMMAND} -E copy_directory ${CMAKE_SOURCE_DIR}/resources/lang ${DH_SHARE}/lang
      COMMAND ${CMAKE_COMMAND} -E copy_directory ${CMAKE_SOURCE_DIR}/resources/skins ${DH_SHARE}/skins)
    include(GNUInstallDirs)
    # deckhand-engine must sit beside deckhand: the app starts it from its own folder (ARCHITECTURE.md, "Engine process").
    install(TARGETS deckhand deckhand-engine deckhandctl RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
    install(FILES ${CMAKE_SOURCE_DIR}/platform/linux/packaging/deckhand.desktop
            DESTINATION ${CMAKE_INSTALL_DATADIR}/applications)
    install(FILES ${CMAKE_SOURCE_DIR}/platform/linux/packaging/deckhand.svg
            DESTINATION ${CMAKE_INSTALL_DATADIR}/icons/hicolor/scalable/apps)
    install(DIRECTORY ${CMAKE_SOURCE_DIR}/controllers/ DESTINATION ${CMAKE_INSTALL_DATADIR}/deckhand/controllers
            FILES_MATCHING PATTERN "*.dhmap")
    install(DIRECTORY ${CMAKE_SOURCE_DIR}/resources/lang ${CMAKE_SOURCE_DIR}/resources/skins
            DESTINATION ${CMAKE_INSTALL_DATADIR}/deckhand)
  endif()
endif()

# ---- Windows platform tests and shell (Win32 window, Direct3D 11, DirectWrite) --------------------
if(WIN32)
  add_executable(test_windows_decoder platform/windows/tests/decoder_test.cpp ${DH_CONSOLE_RC})
  target_link_libraries(test_windows_decoder dh_platform dh_audio)
  add_test(NAME test_windows_decoder COMMAND test_windows_decoder WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
  add_executable(test_windows_midi platform/windows/tests/midi_test.cpp ${DH_CONSOLE_RC})
  target_link_libraries(test_windows_midi dh_platform dh_midi winmm)
  add_test(NAME test_windows_midi COMMAND test_windows_midi WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
  add_executable(test_windows_ipc platform/windows/tests/ipc_test.cpp ${DH_CONSOLE_RC})
  target_link_libraries(test_windows_ipc dh_audio dh_platform ws2_32)
  add_dependencies(test_windows_ipc deckhand-engine)  # the test starts the engine process from its own folder
  add_test(NAME test_windows_ipc COMMAND test_windows_ipc WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
  option(DECKHAND_BUILD_SHELL "Build the Windows desktop shell" ON)
  if(DECKHAND_BUILD_SHELL)
    file(GLOB shell_srcs CONFIGURE_DEPENDS platform/windows/shell/*.cpp)
    add_executable(deckhand WIN32 ${shell_srcs} platform/windows/resources/deckhand.rc)
    target_include_directories(deckhand PRIVATE src/app/include)
    target_link_libraries(deckhand dh_app dh_platform d3d11 dxgi dwrite ole32 shell32 user32 gdi32 imm32 uuid)
    if(MINGW)
      target_link_options(deckhand PRIVATE -municode)  # wWinMain entry point
    endif()
    add_executable(test_windows_text platform/windows/tests/text_test.cpp platform/windows/shell/text.cpp
                   platform/windows/shell/png.cpp ${DH_CONSOLE_RC})
    target_include_directories(test_windows_text PRIVATE src/app/include)
    target_link_libraries(test_windows_text dwrite ole32 user32 uuid)
    add_test(NAME test_windows_text COMMAND test_windows_text WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
    # Resources beside the exe in the share/deckhand layout (controllers/, lang/, skins/), as on Linux.
    set(DH_SHARE ${CMAKE_BINARY_DIR}/share/deckhand)
    add_custom_command(TARGET deckhand POST_BUILD
      COMMAND ${CMAKE_COMMAND} -E make_directory ${DH_SHARE}/controllers
      COMMAND ${CMAKE_COMMAND} -E copy_directory ${CMAKE_SOURCE_DIR}/controllers ${DH_SHARE}/controllers
      COMMAND ${CMAKE_COMMAND} -E copy_directory ${CMAKE_SOURCE_DIR}/resources/lang ${DH_SHARE}/lang
      COMMAND ${CMAKE_COMMAND} -E copy_directory ${CMAKE_SOURCE_DIR}/resources/skins ${DH_SHARE}/skins)
    # deckhand-engine must sit beside deckhand: the app starts it from its own folder.
    install(TARGETS deckhand deckhand-engine deckhandctl RUNTIME DESTINATION .)
    install(DIRECTORY ${CMAKE_SOURCE_DIR}/controllers/ DESTINATION share/deckhand/controllers FILES_MATCHING PATTERN "*.dhmap")
    install(DIRECTORY ${CMAKE_SOURCE_DIR}/resources/lang ${CMAKE_SOURCE_DIR}/resources/skins DESTINATION share/deckhand)
  endif()
endif()

# Link arguments for shells that link the core outside CMake (see scripts/).
if(APPLE)
  file(WRITE ${CMAKE_BINARY_DIR}/tls_link.txt "${OPENSSL_ROOT}/lib/libssl.a ${OPENSSL_ROOT}/lib/libcrypto.a")
  if(DECKHAND_WITH_SQLCIPHER)
    file(WRITE ${CMAKE_BINARY_DIR}/sqlite_link.txt "${SQLCIPHER_ROOT}/lib/libsqlcipher.a ${OPENSSL_CRYPTO} -lz")
  else()
    file(WRITE ${CMAKE_BINARY_DIR}/sqlite_link.txt "-lsqlite3")
  endif()
endif()
