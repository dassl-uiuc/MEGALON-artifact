#include "config.h"

#include <algorithm>
#include <iomanip>

#include "absl/log/check.h"
#include "absl/log/log.h"
#include "constants.h"
#include "helper.h"
#include "yaml-cpp/yaml.h"

namespace rackobj {

using std::stoull;
using std::string;

static const char *kConfigPathEnvVar = "RACKOBJ_CONFIG";
static const char *kDefaultConfigPath = "/opt/rackobj/config";

RackOBJConfig::RackOBJConfig() {
#ifdef NDEBUG
    LOG(INFO) << "Initializing configuration (NDEBUG)...";
#else
    LOG(INFO) << "Initializing configuration...";
#endif

    std::filesystem::path config_path;
    if (const char *path = std::getenv(kConfigPathEnvVar)) {
        config_path = path;
    } else {
        LOG(INFO) << std::quoted(kConfigPathEnvVar) << " environment variable not specified. "
                  << "Defaulting to config at " << std::quoted(kDefaultConfigPath);
        config_path = kDefaultConfigPath;
    }

    if (!std::filesystem::exists(config_path)) {
        LOG(FATAL) << "Configuration file at path " << config_path << " does not exist.";
    }

    YAML::Node config = YAML::LoadFile(config_path);
    if (config["mount_directory"]) {
        mount_directory_ = config["mount_directory"].as<string>();
        DLOG(INFO) << "Mounting FS at " << mount_directory_;
    } else {
        LOG(FATAL) << "\"mount_directory\" not specified in configuration.";
    }

    std::stringstream ss;
    bool contains_reqd_fields = config["id"] && config["base_address"] && config["slots"] && config["host_address"];
    if (!contains_reqd_fields) {
        LOG(FATAL) << "Server config must specify \"id\", "
                      "\"base_address\", \"host_address\", "
                      "and \"slots\"";
    }

    num_slots_ = config["slots"].as<size_t>();

    YAML::Node excluded_files = config["excluded_files"];
    if (excluded_files && excluded_files.IsSequence()) {
        for (auto it = excluded_files.begin(); it != excluded_files.end(); ++it) {
            excluded_files_.push_back(it->as<string>());
        }
    }

    YAML::Node excluded_directories = config["excluded_directories"];
    if (excluded_directories && excluded_directories.IsSequence()) {
        for (auto it = excluded_directories.begin(); it != excluded_directories.end(); ++it) {
            excluded_directories_.push_back(it->as<string>());
        }
    }

    if (config["scr_size"]) {
        scr_size_ = convert_mem_size(config["scr_size"].as<string>());
    } else {
        scr_size_ = DEFAULT_SCR_REGION_SIZE;
        LOG(WARNING) << "scr_size not specified, default to " << DEFAULT_SCR_REGION_SIZE;
    }

    if (config["logical_scr_size"]) {
        logical_scr_size_ = convert_mem_size(config["logical_scr_size"].as<string>());
    } else {
        logical_scr_size_ = DEFAULT_LOGICAL_SCR_SIZE;
        LOG(WARNING) << "logical_scr_size_ not specified, default to " << DEFAULT_LOGICAL_SCR_SIZE;
    }

    if (config["ncr_size"]) {
        ncr_size_ = convert_mem_size(config["ncr_size"].as<string>());
    } else {
        ncr_size_ = DEFAULT_NCR_REGION_SIZE;
        LOG(WARNING) << "ncr_size not specified, default to " << DEFAULT_NCR_REGION_SIZE;
    }

    if (config["local_size"]) {
        local_size_ = convert_mem_size(config["local_size"].as<string>());
    } else {
        local_size_ = DEFAULT_LOCAL_REGION_SIZE;
        LOG(WARNING) << "local_size not specified, default to " << DEFAULT_LOCAL_REGION_SIZE;
    }

    if (config["replicate"]) {
        replicate_ = config["replicate"].as<bool>();
    } else {
        replicate_ = false;
        LOG(WARNING) << "replicate not specified, default to false";
    }

    if (config["flush"]) {
        flush_ = config["flush"].as<bool>();
    } else {
        flush_ = false;
        LOG(WARNING) << "flush not specified, default to false";
    }

    if (config["move_to_share"]) {
        move_to_share_ = config["move_to_share"].as<bool>();
    } else {
        move_to_share_ = false;
        LOG(WARNING) << "move_to_share not specified, default to false";
    }

    if (config["evict"]) {
        evict_ = config["evict"].as<bool>();
    } else {
        evict_ = false;
        LOG(WARNING) << "evict not specified, default to false";
    }

    CHECK(config["key_space"]);
    key_space_ = config["key_space"].as<size_t>();
}

bool RackOBJConfig::IsPathExcluded(const std::filesystem::path &path) const {
    if (std::find(excluded_files_.begin(), excluded_files_.end(), path) != excluded_files_.end()) {
        DLOG(INFO) << path << " is excluded from RackOBJ";
        return true;
    }

    string path_str = path.string();
    for (const auto &dir : excluded_directories_) {
        if (path_str.find(dir) == 0) {
            DLOG(INFO) << path << " is excluded from RackOBJ";
            return true;
        }
    }

    return false;
}

}  // namespace rackobj