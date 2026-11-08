file(GLOB SOURCES "src/*.h" "src/*.cpp" "res/*.rc")
set(TARGET KICQ)
include(${CMAKE_SOURCE_DIR}/cmake/plugin.cmake)
target_link_libraries(${TARGET} ws2_32.lib)
add_subdirectory(proto_kicq)

# Preserve Unicode literals in X-status menu, regardless of Windows ANSI code page.
if(MSVC)
  target_compile_options(${TARGET} PRIVATE /utf-8)
endif()
