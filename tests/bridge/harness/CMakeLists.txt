# Bambu Bridge — harness library.
#
# Static library shared by every Harness*Test binary. Holds the
# ShimRecorder (process-wide JSONL writer) and the trace schema headers.
# The recorder itself is OFF at process start; tests opt in via
# enable_from_env() / enable(path).
#
# This library is only configured when BAMBU_BRIDGE_HARNESS=ON. The
# production bambu_bridge static library has zero dependency on it.

add_library(bambu_bridge_harness STATIC
    ShimRecorder.cpp
    ShimRecorder.hpp
    TraceLine.hpp
    TraceComparator.cpp
    TraceComparator.hpp
)

target_include_directories(bambu_bridge_harness
    PUBLIC
        # tests/bridge — for `#include "harness/ShimRecorder.hpp"`.
        ${CMAKE_CURRENT_SOURCE_DIR}/..
        # bambu_bridge/ — for `#include "third_party/nlohmann/json.hpp"`
        # (matches the path the production bridge code uses; harness
        # tests reuse the vendored json header).
        ${CMAKE_CURRENT_SOURCE_DIR}/../../../src/bambu_bridge
)

target_compile_definitions(bambu_bridge_harness PUBLIC
    BAMBU_BRIDGE_HARNESS_ENABLE=1
)

set_target_properties(bambu_bridge_harness PROPERTIES
    CXX_STANDARD 17
    CXX_STANDARD_REQUIRED ON
)

if (TARGET Threads::Threads)
    target_link_libraries(bambu_bridge_harness PUBLIC Threads::Threads)
endif ()

# Standalone trace comparator binary. Takes two JSONL paths on argv;
# returns 0 / 1 / 2 / 77 per TraceComparator::compare_files's contract.
add_executable(trace_comparator
    trace_comparator_main.cpp
)
target_link_libraries(trace_comparator PRIVATE bambu_bridge_harness)
set_target_properties(trace_comparator PROPERTIES
    CXX_STANDARD 17
    CXX_STANDARD_REQUIRED ON
)
