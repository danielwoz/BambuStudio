// Bambu Bridge — trace_comparator CLI (harness).
//
// Thin argv wrapper around TraceComparator::compare_files. Used by the
// CI replay-vs-fixture workflow and engineers comparing two locally
// captured traces.

#include <cstdio>
#include <string>

#include "TraceComparator.hpp"

int main(int argc, char** argv) {
    if (argc != 3) {
        std::fprintf(stderr,
                     "usage: %s <trace_a.jsonl> <trace_b.jsonl>\n"
                     "  exit codes: 0 match, 1 mismatch, 2 parse-error, "
                     "77 input missing\n",
                     argv[0]);
        return 2;
    }
    return Slic3r::bridge::harness::compare_files(argv[1], argv[2]);
}
