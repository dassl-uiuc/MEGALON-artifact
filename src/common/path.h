#pragma once

#include <filesystem>

#include "config.h"

namespace rackobj {

class RackOBJPath {
public:
    RackOBJPath(const RackOBJConfig& cfg, const std::filesystem::path& path);

    /**
     * @brief Get the Logical Path object. The logical path is the path that the
     * user sees and interacts with. It is the path that is passed to the system
     * calls.
     */
    inline const std::filesystem::path& GetLogicalPath() const { return logical_path_; }

    /**
     * @brief Get the Real Path object. The real path is the path that the
     * server uses to interact with the file system. It is the path that is
     * passed to the file system calls.
     */
    inline const std::filesystem::path& GetRealPath() const { return real_path_; }

    inline std::filesystem::path GetFilename() const { return logical_path_.filename(); }

    inline std::filesystem::path GetLogicalParentDirectory() const {
        if (logical_path_.has_filename()) {
            return logical_path_.parent_path();
        } else {
            return logical_path_.parent_path().parent_path();
        }
    }

    inline std::filesystem::path GetRealParentDirectory() const {
        if (real_path_.has_filename()) {
            return real_path_.parent_path();
        } else {
            return real_path_.parent_path().parent_path();
        }
    }

    std::filesystem::path::iterator lbegin() { return logical_path_.begin(); }

    std::filesystem::path::iterator lend() { return logical_path_.end(); }

    std::filesystem::path::iterator rbegin() { return real_path_.begin(); }

    std::filesystem::path::iterator rend() { return real_path_.end(); }

private:
    std::filesystem::path real_path_;
    std::filesystem::path logical_path_;
};

}  // namespace rackobj