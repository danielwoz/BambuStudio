// Bambu Bridge — shared upload spool helper.

#include "UploadSpool.hpp"

#include <cerrno>
#include <cstdlib>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <vector>

namespace Slic3r {
namespace bridge {
namespace router {

std::string spool_upload_to_tempfile(const server::UploadJob& job) {
    const char* tmpdir = std::getenv("TMPDIR");
    std::string tmpl = (tmpdir && *tmpdir ? tmpdir : "/tmp");
    tmpl += "/bridge-upload-XXXXXX";

    std::vector<char> buf(tmpl.begin(), tmpl.end());
    buf.push_back('\0');

    int fd = ::mkstemp(buf.data());
    if (fd < 0) return {};
    ::fchmod(fd, 0600);

    const auto* src = job.content.data();
    std::size_t remaining = job.content.size();
    while (remaining > 0) {
        ssize_t w = ::write(fd, src, remaining);
        if (w < 0) {
            if (errno == EINTR) continue;
            ::close(fd);
            ::unlink(buf.data());
            return {};
        }
        src       += w;
        remaining -= static_cast<std::size_t>(w);
    }
    ::close(fd);
    return std::string(buf.data());
}

} // namespace router
} // namespace bridge
} // namespace Slic3r
