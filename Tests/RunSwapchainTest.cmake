execute_process(
    COMMAND "${APP}" --swapchain-test "${SCENARIO}"
    RESULT_VARIABLE result
    OUTPUT_VARIABLE output
    ERROR_VARIABLE errors
    TIMEOUT 45
)
message("${output}${errors}")
if("${output}${errors}" MATCHES "Validation Error|ERROR: Validation|VUID-|SYNC-HAZARD")
    message(FATAL_ERROR "Vulkan validation reported a problem")
endif()
if(SCENARIO STREQUAL "create-failure" AND NOT errors MATCHES "Failed to create Swapchain:")
    message(FATAL_ERROR "Swapchain creation fault was not reached")
endif()
if(SCENARIO STREQUAL "view-failure" AND NOT errors MATCHES "Failed to create Swapchain ImageView 2:")
    message(FATAL_ERROR "Second ImageView creation fault was not reached")
endif()
if(SCENARIO STREQUAL "asset-drop" AND
   (NOT errors MATCHES "broken[.]glb:" OR NOT output MATCHES "Win32 file drop, failure recovery and UTF-8 paths verified"))
    message(FATAL_ERROR "File drop coverage did not complete")
endif()
if(result STREQUAL "77")
    message("Unsupported surface configuration")
elseif(NOT result STREQUAL "0")
    message(FATAL_ERROR "Recovery scenario exited with ${result}")
endif()
