#ifndef DIFFMONGER_CONTROLLER_HPP
#define DIFFMONGER_CONTROLLER_HPP

#include "BackendDriver.hpp"
#include "Node.hpp"
#include "RepositoryParams.hpp"
#include "RepositoryStructure.hpp"

#include <string>
#include <optional>
#include <filesystem>
#include <memory>

namespace diffmonger {

class RepositoryStructure;
class PasswordBuffer;

enum class ExitStatus : int
{
    SUCCESS = 0,
    // This indicates that partial functionality executed successfully,
    // but some expected functionality was skipped due to runtime conditions.
    // Manual user review of error messages is required to decide if the skipped
    // functionality was required.
    PARTIAL_FUNCTIONALITY = 2
};

struct SnapshotDoesNotExist : std::runtime_error
{
    using std::runtime_error::runtime_error;
};

struct PruneParams
{
};

struct SnapshotParams
{
};

struct SnapshotImportParams
{
    bool create_snapshot = false;
};

struct RestoreParams
{
    std::shared_ptr<PasswordBuffer> password;
    Node node;
    /**
     * Should nodes whose snapshots already exist in the live dataset be skipped?
     */
    bool skip_existing = true;
};

struct ExportParams
{
    std::shared_ptr<PasswordBuffer> password;
    std::vector<std::string> importCommand;
};

struct ShowInitParams
{
    // Output as null-separated values?
    bool nullsep = false;
};

struct InitKeyPairParams
{
    std::unique_ptr<PasswordBuffer> password;
    size_t memlimit;
};

struct ListNodesParams
{
    bool show_timestamps = false;
};

void init_repository(std::filesystem::path const &repositoryPath,
                     Uuid const &repositoryStructureFormatUuid,
                     std::vector<std::string> const &initargs);

void init_keypair(std::filesystem::path const &repositoryPath,
                  InitKeyPairParams const &initKeyPairParams);

void show_init(RepositoryStructure const &repositoryStructure,
              ShowInitParams const &initCmdParams);

void snapshot(RepositoryStructure &repositoryStructure,
              RepositoryParams const &repositoryParams,
              BackendDriver &backendDriver,
              SnapshotParams const &params);

void snapshot_import(RepositoryStructure &repositoryStructure,
                     RepositoryParams const &repositoryParams,
                     BackendDriver &backendDriver,
                     SnapshotImportParams const &params);

void restore(RepositoryStructure &repositoryStructure,
             RepositoryParams const &repositoryParams,
             BackendDriver &backendDriver,
             RestoreParams const &params);

void prune_repository(RepositoryStructure &repositoryStructure,
                      RepositoryParams const &params,
                      PruneParams const &pruneParams);

ExitStatus export_repository(RepositoryStructure &destinationRepositoryStructure,
                             RepositoryParams const &destinationRepositoryParams,
                             BackendDriver &backendDriver,
                             ExportParams const &copyParams);

void list_nodes(RepositoryStructure const &repositoryStructure,
                RepositoryParams const &repositoryParams,
                ListNodesParams const &listNodesParams);

}

#endif
