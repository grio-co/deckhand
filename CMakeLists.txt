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
add_compile_options(-Wall -Wextra)

# ---- third party -----------------------------------------------------------
add_library(dh_sqlite INTERFACE)
if(NOT APPLE)
  find_package(PkgConfig REQUIRED)
endif()
if(DECKHAND_WITH_SQLCIPHER AND APPLE)
  set(SQLCIPHER_ROOT "/opt/homebrew/opt/sqlcipher" CACHE PATH "SQLCipher prefix")
  set(OPENSSL_CRYPTO "/opt/homebrew/opt/openssl@4/lib/libcrypto.a" CACHE FILEPATH "libcrypto used by SQLCipher")
  target_include_directories(dh_sqlite SYSTEM INTERFACE ${SQLCIPHER_ROOT}/include/sqlcipher)
  target_compile_definitions(dh_sqlite INTERFACE SQLITE_HAS_CODEC DECKHAND_WITH_SQLCIPHER=1)
  target_link_libraries(dh_sqlite INTERFACE ${SQLCIPHER_ROOT}/lib/libsqlcipher.a ${OPENSSL_CRYPTO} z)
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
target_compile_options(dh_miniaudio PRIVATE -w)
if(APPLE)
  target_compile_definitions(dh_miniaudio PUBLIC MA_NO_RUNTIME_LINKING)
  target_link_libraries(dh_miniaudio PUBLIC "-framework CoreFoundation" "-framework CoreAudio" "-framework AudioToolbox")
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
dh_lib(sync      DEPS core log)
dh_lib(api       DEPS core log)
if(APPLE)
  set(OPENSSL_ROOT "/opt/homebrew/opt/openssl@4" CACHE PATH "OpenSSL prefix (TLS for the API server)")
  target_include_directories(dh_api SYSTEM PRIVATE ${OPENSSL_ROOT}/include)
  target_link_libraries(dh_api PUBLIC ${OPENSSL_ROOT}/lib/libssl.a ${OPENSSL_ROOT}/lib/libcrypto.a)
else()
  find_package(OpenSSL REQUIRED)
  target_link_libraries(dh_api PUBLIC OpenSSL::SSL OpenSSL::Crypto)
endif()
dh_lib(skin      DEPS core log i18n)
dh_lib(gfx       DEPS core log)
dh_lib(services  DEPS core log library audio export analysis)
# The API server needs OpenSSL; platforms without it (iOS) build with -DDECKHAND_WITH_API=OFF.
if(DECKHAND_WITH_API)
  dh_lib(app     DEPS core log library import audio midi gfx services api i18n skin broadcast)
  target_compile_definitions(dh_app PRIVATE DECKHAND_WITH_API=1)
else()
  dh_lib(app     DEPS core log library import audio midi gfx services i18n skin broadcast)
  target_compile_definitions(dh_app PRIVATE DECKHAND_WITH_API=0)
endif()

# ---- platform glue (C APIs shared by macOS and iOS) ---------------------------
if(APPLE)
  file(GLOB apple_srcs CONFIGURE_DEPENDS platform/apple/*.cpp)
  add_library(dh_platform STATIC ${apple_srcs})
  target_link_libraries(dh_platform PUBLIC dh_audio dh_midi
    "-framework AudioToolbox" "-framework CoreMIDI" "-framework CoreFoundation" "-framework CoreAudio")
elseif(UNIX)
  # Linux: FFmpeg (libavformat/libavcodec/libswresample, LGPL, dynamic) decodes; the ALSA sequencer does MIDI.
  pkg_check_modules(FFMPEG REQUIRED IMPORTED_TARGET libavformat libavcodec libswresample libavutil)
  pkg_check_modules(ALSA REQUIRED IMPORTED_TARGET alsa)
  file(GLOB linux_srcs CONFIGURE_DEPENDS platform/linux/*.cpp)
  add_library(dh_platform STATIC ${linux_srcs})
  target_link_libraries(dh_platform PUBLIC dh_audio dh_midi PkgConfig::FFMPEG PkgConfig::ALSA)
endif()

# ---- programs -----------------------------------------------------------------
add_executable(deckhandctl src/cli/main.cpp)
target_link_libraries(deckhandctl dh_library dh_import dh_audio dh_services dh_api dh_platform)

file(GLOB test_srcs CONFIGURE_DEPENDS tests/test_*.cpp)
foreach(t ${test_srcs})
  get_filename_component(n ${t} NAME_WE)
  add_executable(${n} ${t})
  target_link_libraries(${n} dh_library dh_import dh_export dh_analysis dh_audio dh_midi dh_gfx dh_api dh_skin dh_i18n
    dh_broadcast dh_platform)
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
    install(TARGETS deckhand deckhandctl RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
    install(DIRECTORY ${CMAKE_SOURCE_DIR}/controllers/ DESTINATION ${CMAKE_INSTALL_DATADIR}/deckhand/controllers
            FILES_MATCHING PATTERN "*.dhmap")
    install(DIRECTORY ${CMAKE_SOURCE_DIR}/resources/lang ${CMAKE_SOURCE_DIR}/resources/skins
            DESTINATION ${CMAKE_INSTALL_DATADIR}/deckhand)
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
