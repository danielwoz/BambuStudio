// Bambu Bridge — default upload sink stub (phase 7).

#include "NullUploadSink.hpp"

#include <cstdio>

namespace Slic3r {
namespace bridge {
namespace router {

server::UploadResult NullUploadSink::deliver(server::UploadJob job) {
    server::UploadResult r;
    r.ok            = false;
    r.error_message =
        "NullUploadSink does not forward uploads; install a "
        "LanUploadSink or CloudUploadSink to deliver to a printer.";
    return r;
}

} // namespace router
} // namespace bridge
} // namespace Slic3r
