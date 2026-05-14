// Bambu Bridge — E2E fixture loader (phase 12).

#include "E2EFixtureLoader.hpp"

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

namespace Slic3r {
namespace bridge {
namespace e2e {

namespace {

std::string fixture_dir() {
    if (const char* envv = std::getenv("BAMBU_BRIDGE_E2E_FIXTURE_DIR");
        envv && *envv) {
        return envv;
    }
#ifdef BAMBU_BRIDGE_E2E_FIXTURE_DIR
    return BAMBU_BRIDGE_E2E_FIXTURE_DIR;
#else
    return "./fixtures";
#endif
}

// Tiny hand-rolled JSON extractor: finds `"key":` and returns the value
// as a trimmed string. Handles "string", true/false, and integers. We
// avoid pulling nlohmann::json into the test build since the bridge's
// vendored copy is PRIVATE and the test executable only sees the
// public include path.
std::string find_json(const std::string& blob, const std::string& key) {
    const std::string needle = "\"" + key + "\"";
    const auto p = blob.find(needle);
    if (p == std::string::npos) return {};
    auto q = blob.find(':', p + needle.size());
    if (q == std::string::npos) return {};
    ++q;
    while (q < blob.size() && std::isspace(static_cast<unsigned char>(blob[q]))) ++q;
    if (q >= blob.size()) return {};
    if (blob[q] == '"') {
        ++q;
        std::string out;
        while (q < blob.size() && blob[q] != '"') { out.push_back(blob[q++]); }
        return out;
    }
    std::string out;
    while (q < blob.size() && blob[q] != ',' && blob[q] != '}' && !std::isspace(static_cast<unsigned char>(blob[q]))) {
        out.push_back(blob[q++]);
    }
    return out;
}

} // namespace

FixtureMetadata load_cube_5mm_fixture() {
    FixtureMetadata md;
    namespace fs = std::filesystem;

    const fs::path dir   = fixture_dir();
    const fs::path mf    = dir / "cube_5mm.3mf";
    const fs::path meta  = dir / "cube_5mm.3mf.metadata.json";

    md.fixture_path = mf.string();

    std::ifstream in(meta);
    if (!in) {
        // Sidecar missing — assume stub.
        md.is_stub = true;
        md.notes   = "no metadata sidecar; assuming stub";
        return md;
    }
    std::stringstream ss;
    ss << in.rdbuf();
    const std::string blob = ss.str();

    const std::string plate = find_json(blob, "plate_idx");
    const std::string stub  = find_json(blob, "is_stub");
    const std::string notes = find_json(blob, "notes");

    if (!plate.empty()) md.plate_idx = std::atoi(plate.c_str());
    if (!stub.empty())  md.is_stub   = (stub == "true" || stub == "1");
    md.notes = notes;
    return md;
}

} // namespace e2e
} // namespace bridge
} // namespace Slic3r
