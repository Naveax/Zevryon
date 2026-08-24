# Legacy include point retained so the existing CMake include chain stays stable.
# Zevryon no longer ships a macOS/CoreText backend.
if(APPLE)
  message(FATAL_ERROR "Zevryon does not support macOS; supported desktop targets are Windows and Linux")
endif()

include("${CMAKE_CURRENT_LIST_DIR}/massivedoc_generation.cmake")
include("${CMAKE_CURRENT_LIST_DIR}/massivedoc_trigram.cmake")
include("${CMAKE_CURRENT_LIST_DIR}/frame_budget_scheduler.cmake")
include("${CMAKE_CURRENT_LIST_DIR}/foreground_layout_handoff.cmake")
include("${CMAKE_CURRENT_LIST_DIR}/process_memory_pressure.cmake")
include("${CMAKE_CURRENT_LIST_DIR}/font_resource_sfnt.cmake")
include("${CMAKE_CURRENT_LIST_DIR}/harfbuzz_shaping.cmake")
