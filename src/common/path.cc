#include "path.h"

#include "absl/log/check.h"

namespace rackobj {

RackOBJPath::RackOBJPath(const RackOBJConfig& cfg, const std::filesystem::path& path) {
    logical_path_ = path;
    if (logical_path_.is_relative()) {
        logical_path_ = std::filesystem::current_path() / logical_path_;
    }
    DCHECK(logical_path_.is_absolute());
    real_path_ = cfg.GetMountDirectory() / logical_path_.relative_path();
}

}  // namespace rackobj