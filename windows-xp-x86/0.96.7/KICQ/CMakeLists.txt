# PMLICQ target: Miranda NG stable v0.96.7 (ebe074e).
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
    message(WARNING "PMLICQ: Miranda NG checkout is not at an exact release tag; build against v0.96.7 (ebe074e)")
  endif()
else()
  message(STATUS "PMLICQ: unable to determine Miranda NG revision; make sure SDK/source is v0.96.7")
endif()

file(GLOB SOURCES "src/*.h" "src/*.cpp" "res/*.rc")
set(TARGET KICQ)
include(${CMAKE_SOURCE_DIR}/cmake/plugin.cmake)
target_link_libraries(${TARGET} ws2_32.lib)
add_subdirectory(proto_kicq)

# Use UTF-8 source encoding for Russian X-status names in both x86 and x64.
if(MSVC)
  target_compile_options(${TARGET} PRIVATE /utf-8)
endif()
