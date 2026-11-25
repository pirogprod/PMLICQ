# PMLICQ target: Miranda NG stable 0.96.7 (including later stable builds).
# The plugin must be compiled INSIDE the matching Miranda NG source tree.
# Git-tag verification deliberately fails on an explicitly wrong tagged checkout.
find_package(Git QUIET)
if(GIT_FOUND AND EXISTS "${CMAKE_SOURCE_DIR}/.git")
  execute_process(
    COMMAND "${GIT_EXECUTABLE}" -C "${CMAKE_SOURCE_DIR}" describe --tags --exact-match HEAD
    RESULT_VARIABLE PMLICQ_TAG_STATUS OUTPUT_VARIABLE PMLICQ_TREE_TAG
    ERROR_QUIET OUTPUT_STRIP_TRAILING_WHITESPACE)
  if(PMLICQ_TAG_STATUS EQUAL 0 AND NOT PMLICQ_TREE_TAG STREQUAL "v0.96.7")
    message(FATAL_ERROR "PMLICQ: expected Miranda NG v0.96.7, found ${PMLICQ_TREE_TAG}")
  elseif(NOT PMLICQ_TAG_STATUS EQUAL 0)
    message(WARNING "PMLICQ: non-tagged checkout; use the exact revision of the installed Miranda 0.96.7 build (e.g. 495fea6 for #28846)")
  endif()
else()
  message(STATUS "PMLICQ: unable to determine Miranda NG revision; use the SDK/source matching the exact installed 0.96.7 build")
endif()

# Native API with a modern SDK; an old SDK uses the runtime-checked #28846 bridge.
option(PMLICQ_ALLOW_LEGACY_CONTACT_API "Enable legacy API for a matching old runtime only" OFF)
if(PMLICQ_ALLOW_LEGACY_CONTACT_API)
  message(WARNING "PMLICQ: explicit legacy fallback enabled; use only with a matching legacy runtime or #28846")
endif()

file(GLOB SOURCES "src/*.h" "src/*.cpp" "res/*.rc")
set(TARGET KICQ)
include(${CMAKE_SOURCE_DIR}/cmake/plugin.cmake)
target_link_libraries(${TARGET} ws2_32.lib)
if(PMLICQ_ALLOW_LEGACY_CONTACT_API)
  target_compile_definitions(${TARGET} PRIVATE PMLICQ_ALLOW_LEGACY_CONTACT_API)
endif()
add_subdirectory(proto_kicq)

# Use UTF-8 source encoding for Russian X-status names in both x86 and x64.
if(MSVC)
  target_compile_options(${TARGET} PRIVATE /utf-8)
endif()
