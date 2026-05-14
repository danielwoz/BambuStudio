# Bambu Bridge — harness mocks library.
#
# Drives the in-process MockPlugin (subclass of
# BambuNetworkingPluginHandle) and MockBambuSource (subclass of
# BambuSourceHandle). Each Harness*Test binary links this library to
# inject per-printer FSM behaviour without bringing up the real
# proprietary plugins.

add_library(bambu_bridge_mocks STATIC
    MockPlugin.cpp
    MockPlugin.hpp
    MockBambuSource.cpp
    MockBambuSource.hpp
    PrinterFsm.hpp
    PrinterModel.hpp
    A1Printer.hpp
    H2SPrinter.hpp
    H2DPrinter.hpp
)

target_include_directories(bambu_bridge_mocks
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}
        # bambu_bridge — for BambuNetworkingPluginHandle.hpp etc.
        ${CMAKE_CURRENT_SOURCE_DIR}/../../../src/bambu_bridge
)

target_link_libraries(bambu_bridge_mocks PUBLIC
    bambu_bridge
)

set_target_properties(bambu_bridge_mocks PROPERTIES
    CXX_STANDARD 17
    CXX_STANDARD_REQUIRED ON
)
