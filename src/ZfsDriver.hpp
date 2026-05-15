#ifndef DIFFMONGER_ZFS_DRIVER_HPP
#define DIFFMONGER_ZFS_DRIVER_HPP

#include "BackendDriver.hpp"
#include "zfs.hpp"

#include <optional>

namespace diffmonger {

class ZfsBackendDriver : public BackendDriver
{
public:
    struct RepositoryParams
    {
        bool raw = false;
    };

    struct DriverArguments
    {
        std::vector<std::string> zfsCommand = { "zfs" };
        std::string datasetName{};
        std::optional<std::vector<std::string>> receive0_args{};
        std::optional<std::vector<std::string>> receiven_args{};
    };

    static std::unique_ptr<BackendDriver::Factory> createFactory(
        RepositoryParams, DriverArguments);
};

}
#endif
