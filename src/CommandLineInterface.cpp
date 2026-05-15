#include "CommandLineInterface.hpp"

#include "Controller.hpp"
#include "ZfsDriver.hpp"
#include "RepositoryStructure.hpp"
#include "Node.hpp"

#include <diffmonger/util/PasswordBuffer.hpp>
#include <diffmonger/util/parser/Parser.hpp>
#include <diffmonger/util/array.hpp>

#include <sodium/crypto_pwhash.h>

#include <string>
#include <span>
#include <vector>
#include <functional>
#include <charconv>
#include <iostream>

namespace diffmonger {

namespace {

static constexpr Uuid currentRepositoryStructureFormatUuid{
    make_array_cast<std::byte>(0x8d,0x76,0xc7,0x4f,0x60,0x6f,0x7a,0xdd,
                               0x0a,0x29,0xef,0xd4,0xc2,0x86,0x69,0xc1)};

using BooleanParameter = parameter::ParameterHelper<Parameter::Defaulted,
                                                    bool,
                                                    parameter::Scalar,
                                                    parameter::Boolean>;
using RequiredBooleanParameter = parameter::ParameterHelper<Parameter::Required,
                                                            bool,
                                                            parameter::Scalar,
                                                            parameter::Boolean>;

struct DiffmongerNode
{
    Node process(size_t const x) const
    {
        return Node{ .value = x };
    }

    auto describe() const { return "Diffmonger node identifier"; }
};

struct FdOrFile
{
    FdOwner process(std::string_view scalar) const
    {
        std::string_view const prefix = "file://";
        int fd;
        auto const r = std::from_chars(scalar.begin(), scalar.end(), fd);
        if ((r.ec == std::errc{}) && (r.ptr == scalar.end()))
            return FdOwner{fd};
        else if (scalar.starts_with(prefix))
        {
            std::filesystem::path const path(scalar.substr(prefix.length()));
            return FdOwner::from_syscall(open(path.native().c_str(), O_RDONLY),
                                         "open", path.native().c_str());
        } else
            throw std::runtime_error(std::string("Unrecognised fd or path: ").append(scalar));
    }

    auto describe() const { return "fd in form \"INT\" or path in form \"file://PATH\""; }
};

auto span_exc_front(std::vector<std::string> const &xs)
{
    if (xs.empty())
        throw std::invalid_argument("span_exc_front: empty argument");
    return std::vector<std::string_view>{xs.begin() + 1, xs.end()};
}

using OptionalFdOrFile = parameter::ParameterHelper<Parameter::Optional,
                                                    FdOwner,
                                                    parameter::Scalar,
                                                    FdOrFile>;

using OptionalNodeParameter = parameter::ParameterHelper<Parameter::Optional,
                                                         Node,
                                                         parameter::Scalar,
                                                         parameter::Integer<int64_t>,
                                                         parameter::NonNegative,
                                                         DiffmongerNode>;

using OptionalExistingFileParameter = parameter::ParameterHelper<Parameter::Optional,
                                                                 std::filesystem::path,
                                                                 parameter::Scalar,
                                                                 parameter::ExistingFile>;


struct Main : RootCommand
{
    struct PasswordParameter
    {
        static auto constexpr additionalPassphraseName = "additional-passphrase-fd";
        BooleanParameter &disablePassphrase;
        OptionalFdOrFile &additionalPassphraseFd;

        PasswordParameter(Command &command)
            : disablePassphrase(
                command.emplaceParameter<BooleanParameter>(
                    Parameter::Defaulted{{"false"}},
                    "no-passphrase",
                    std::string()
                    .append(
                        "Disable requesting a keyboard-entry passphrase. "
                        "Presumably the ")
                    .append(additionalPassphraseName)
                    .append(" parameter will be used to enter the passphrase instead."))),
              additionalPassphraseFd(
                  command.emplaceParameter<OptionalFdOrFile>(
                      "additional-passphrase-fd",
                      "An fd or a path to read an additional passphrase from. "
                      "The contents is trivially appended to the back of the  "
                      "normal passphrase without a separator. "
                      "The contents can be arbitrary binary data, include null bytes. "
                      "This parameter exists to enable \"what you have\" security; "
                      "the input may, for example, come from a smartcard reader, "
                      "in which case, "
                      "a wrapper script will likely be needed to interface with the reader "
                      "and prepare an open fd to be inherited when exec*()ing diffmonger. "
                      "Note that diffmonger creates a user-data/ directory that can "
                      "be used to store a salt to process with the smartcard, if needed. "
                      "Note that diffmonger only calls read() and close() on the given fd "
                      "and does not call write() to clear the memory. "
                      "If the fd was created by "
                      "memfd_create() and it is sought to clear the memory after use, "
                      "then it is the caller's responsibility to do this."))
              /*
            "a high-entropy passphrase can be physically written on a post-it note and stuck "
            "to the monitor; an attacker needs both the primary passphrase and the "
            "additional passphrase. "
            "This does nothing to defend against targeted local attacks, "
            "but it makes remote-only attacks far more difficult. "
            "It also makes distant future attacks "
            "(i.e., attacks made in the distant future against your historic "
            "archives of your data) "
            "far harder."
            "significantly reducing your surface area to attack. "
            "It is intended that in addition to writing the passphrase on a postit note, "
            "While password splitting
            "While the post-it note doesn't
            "As a convenience, a file path can also be given in the form \"file:///<PATH>\". "
            "In either case, the entire file, up to EOF, is read and used as the passphrase; "
            "ensure there is no trailing whitespace!");
              */
        {}

        // Should only be called once; consumes additionalPassword's fd.
        std::unique_ptr<PasswordBuffer> readPassword(
            bool const check = false,
            std::string_view const prompt = "Password: ",
            std::string_view const check_prompt = "Password (confirm): ")
        {
            // TODO: Use pinentry(1) for this; note that pinentry supports SETREPEAT
            // for confirming the password.
            // Looks relatively straightforward;
            // assuan_pipe_connect() launches pinentry,
            // assuan_transact() sets options and gets output.
            auto ttyPassword =
                [&]
                {
                    if (disablePassphrase.get())
                        return std::make_unique<PasswordBuffer>(0);

                    auto password = PasswordBuffer::readPasswordFromTty(prompt);

                    if (check)
                    {
                        auto password_check =
                            PasswordBuffer::readPasswordFromTty(check_prompt);
                        if (std::string_view(password->getSpan()) !=
                            std::string_view(password_check->getSpan()))
                            throw std::runtime_error("Passwords do not match");
                    }

                    return password;
                }();

            std::optional<FdOwner> &maybeFd = additionalPassphraseFd.get();

            if (!maybeFd)
                return ttyPassword;

            auto const additionalPassword =
                PasswordBuffer::readuntil(std::move(maybeFd.value()), {});

            // Reduces risk of accidental useless encryption.
            if (additionalPassword->getSize() == 0)
                throw std::runtime_error("Additional password was given but is empty; "
                                         "this isn't supported");

            auto tmp = std::make_unique<PasswordBuffer>(
                ttyPassword->getSize() + additionalPassword->getSize());
            memcpy(tmp->getWritePointer(), ttyPassword->getPointer(), ttyPassword->getSize());
            memcpy(tmp->getWritePointer() + ttyPassword->getSize(),
                   additionalPassword->getPointer(),
                   additionalPassword->getSize());

            // Reduces risk of accidental useless encryption.
            if (tmp->getSize() == 0)
                throw std::runtime_error("Zero-length password is not supported; "
                                         "use unencrypted storage instead");

            return tmp;
        }

        template <typename ...Params>
        std::unique_ptr<PasswordBuffer>
        maybeReadPassword(RepositoryParams const &repositoryParams, Params &&...params)
        {
            if (repositoryParams.encryption)
                return readPassword(std::forward<Params>(params)...);
            else
                return nullptr;
        }
    };

    template <typename Base>
    struct DriverRequiringCommandHelper;

    class DriverRequiringCommand
    {
    public:
        std::unique_ptr<RepositoryStructure> repositoryStructure;
        std::unique_ptr<RepositoryParams> repositoryParams;

        virtual Main &getMain() = 0;
        virtual Command &asCommand() = 0;

        std::unique_ptr<BackendDriver::Factory> createBackendDriverFactory() const
        {
            if (!backendDriverFactoryFunctor)
                throw std::runtime_error("Could not create driver");
            else
                return backendDriverFactoryFunctor();
        }

        void on_enter()
        {
            auto &main = getMain();

            repositoryStructure =
                std::make_unique<RepositoryStructure>(main.repositoryPath.get());

            // Responsible for calling setBackendDriverFactoryFunctor().
            repositoryParams = createRepositoryParams(*repositoryStructure, this);

            if (!backendDriverFactoryFunctor || !repositoryParams)
                throw std::runtime_error("Could not process initargs");
        }

        /*
         * Called via createRepositoryParams().
         * The caller is likely to have added Parameters to asCommand()
         * that are captured by reference in the given functor.
         * This is safe.
         * the Parameters get owned by Command, which is destroyed _after_
         * the functor (which is owned by DriverRequiringCommand)
         * by virtue of Command being the earlier
         * base inherited from by DriverRequiringCommandHelper
         * (noting that DriverRequiringCommand's destructor is private,
         * with DriverRequiringCommandHelper as a friend, to ensure instances
         * can only exist via DriverRequiringCommandHelper).
         *
         * Awkward but safe. This awkwardness is ultimately necessary as we're
         * operating without temporal locality; setBackendDriverFactoryFunctor()
         * gets called first, and then at some point later, the whole of Main
         * gets parse()-ed; it's not until parse() runs that the functor is invoked.
         * The only way around the awkwardness would be to use shared_ptr...
         */
        template <typename F>
        void setBackendDriverFactoryFunctor(F &&f)
        {
            backendDriverFactoryFunctor = std::forward<F>(f);
        }

    private:
        std::function<std::unique_ptr<BackendDriver::Factory>()> backendDriverFactoryFunctor;
        template <typename Base>
        friend struct DriverRequiringCommandHelper;
        ~DriverRequiringCommand() {}
    };

    // Important: Base must be destroyed _after_ DriverRequiringCommand;
    // see comments in DriverRequiringCommand::setBackendDriverFactoryFunctor().
    template <typename Base>
    struct DriverRequiringCommandHelper : Base, DriverRequiringCommand
    {
        using Base::Base;

        void on_enter() final
        {
            DriverRequiringCommand::on_enter();
        }

        Command &asCommand() final { return *this; }
    };

    using RepositoryPath = parameter::ParameterHelper<
        Parameter::Defaulted,
        std::filesystem::path,
        parameter::Scalar>;
    RepositoryPath &repositoryPath = emplaceParameter<RepositoryPath>(
        Parameter::Defaulted{{"."}}, "R", "Path to diffmonger repository.");





    std::string_view getSelfDocumentation() const override
    { return "Diffmonger parent command. A subcommand is required."; }

    struct InitBase : SubCommand<Main>
    {
        struct DriverSubCommandBase : SubCommand<InitBase>
        {
            using SubCommand::SubCommand;

            virtual void extendCommand(DriverRequiringCommand &driverRequiringCommand) const = 0;

            void execute() final
            {
                auto &modeVariant = getParent().modeVariant;

                if (auto *retrospectiveMode = std::get_if<RetrospectiveMode>(&modeVariant))
                {
                    if (retrospectiveMode->commandToExtend)
                        extendCommand(*retrospectiveMode->commandToExtend);
                } else if (auto *executionMode = std::get_if<ExecutionMode>(&modeVariant))
                {
                    InitBase &init = getParent();
                    Main &main = init.getParent();

                    diffmonger::init_repository(main.repositoryPath.get(),
                                                currentRepositoryStructureFormatUuid,
                                                executionMode->args);
                } else throw std::invalid_argument("InitBase: modeVariant");
            }
        };

        struct RetrospectiveMode
        {
            // If given, then the lifetime of the DriverRequiringCommand must
            // exceed the lifetime of *this.
            DriverRequiringCommand *commandToExtend = nullptr;
            Uuid repositoryStructureFormatUuid;
        };

        struct ExecutionMode
        {
            std::vector<std::string> args;
        };

        using ModeVariant = std::variant<RetrospectiveMode, ExecutionMode>;
        ModeVariant modeVariant;

        InitBase(auto const &parent, ModeVariant modeVariant)
            : SubCommand(parent, "init"),
              modeVariant(std::move(modeVariant))
        {}

        std::unique_ptr<RepositoryParams> repositoryParams;
    } &init;

    struct ShowInit final : SubCommand<Main>
    {
        BooleanParameter &nullsep = emplaceParameter<BooleanParameter>(
            Parameter::Defaulted{{"false"}},
            "nullsep",
            "If true, then outputs null separated values. Otherwise, "
            "outputs as space separated quoted values.");

        void execute() override
        {
            ShowInitParams params = {
                .nullsep = nullsep.get()
            };

            diffmonger::show_init({getParent().repositoryPath.get()}, params);
        }

        std::string_view getSelfDocumentation() const override
        {
            return "Show the command used to init the repository.";
        }

        using SubCommand::SubCommand;
    };


    struct InitKeyPair final : SubCommand<Main>
    {
        PasswordParameter passwordParameter{*this};

        using ArgonLimit =
            parameter::ParameterHelper<Parameter::Defaulted,
                                       size_t,
                                       parameter::Scalar,
                                       parameter::Integer<size_t>>;
        ArgonLimit &memlimit =
            emplaceParameter<ArgonLimit>(
                Parameter::Defaulted{
                    {std::to_string(crypto_pwhash_MEMLIMIT_SENSITIVE)}},
                "memlimit",
                std::string("Memory limit for Argon2id"));

        void execute() override
        {
            InitKeyPairParams const params = {
                .password = passwordParameter.readPassword(true),
                .memlimit = memlimit.get()
            };

            diffmonger::init_keypair({getParent().repositoryPath.get()}, params);
        }

        std::string_view getSelfDocumentation() const override
        {
            return "Create a keypair for the repository.";
        }

        using SubCommand::SubCommand;
    };


    struct Snapshot final : DriverRequiringCommandHelper<SubCommand<Main>>
    {
        using DriverRequiringCommandHelper::DriverRequiringCommandHelper;

        BooleanParameter &pruneDataset = emplaceParameter<BooleanParameter>(
            Parameter::Defaulted{{"true"}},
            "prune-dataset",
            "If true, then old snapshot objects will be pruned from the live dataset.");

        BooleanParameter &pruneRepository = emplaceParameter<BooleanParameter>(
            Parameter::Defaulted{{"true"}},
            "prune-repository",
            "If true, then old snapshot files will be pruned from the repository.");

        BooleanParameter &fromExport = emplaceParameter<BooleanParameter>(
            Parameter::Defaulted{{"false"}},
            "from-export",
            "Set to true if importing.");

        void execute() override
        {
            SnapshotParams const snapshotParams = {
                .prune_dataset = pruneDataset.get(),
                .prune_repository = pruneRepository.get()
            };

            snapshot(*repositoryStructure,
                     *repositoryParams,
                     *createBackendDriverFactory()->create(),
                     snapshotParams);
        }

        Main &getMain() override { return getParent(); }

        std::string_view getSelfDocumentation() const override
        { return "Store a snapshot of the dataset."; }
    };


    struct SnapshotImport final : DriverRequiringCommandHelper<SubCommand<Main>>
    {
        using DriverRequiringCommandHelper::DriverRequiringCommandHelper;

        BooleanParameter &createSnapshots =
            emplaceParameter<BooleanParameter>(
                Parameter::Defaulted{{"false"}},
                "create-snapshot", "Must snapshots be created instead of imported?");

        void execute() override
        {
            SnapshotImportParams const params { .create_snapshot = createSnapshots.get() };

            snapshot_import(*repositoryStructure,
                            *repositoryParams,
                            *createBackendDriverFactory()->create(),
                            params);
        }

        Main &getMain() override { return getParent(); }

        std::string_view getSelfDocumentation() const override
        { return "Store a snapshot of the dataset."; }
    };


    struct Restore final : DriverRequiringCommandHelper<SubCommand<Main>>
    {
        using DriverRequiringCommandHelper::DriverRequiringCommandHelper;

        PasswordParameter passwordParameter{*this};

        OptionalNodeParameter &nodeParameter = emplaceParameter<OptionalNodeParameter>(
            "node", "Which node to restore");

        void execute() override
        {
            RestoreParams restoreParams = {
                .password = passwordParameter.maybeReadPassword(*repositoryParams),
                .node = nodeParameter.get()
            };

            auto backendDriverFactory = createBackendDriverFactory();

            restore(*repositoryStructure,
                    *repositoryParams,
                    *backendDriverFactory->create(),
                    restoreParams);
        }

        Main &getMain() override { return getParent(); }

        std::string_view getSelfDocumentation() const override
        { return "Destructively restore the dataset from a snapshot."; }
    };


    struct ExportRepository final : DriverRequiringCommandHelper<SubCommand<Main>>
    {
        using DriverRequiringCommandHelper::DriverRequiringCommandHelper;

        using SnapshotImportCommand = parameter::ParameterHelper<
            Parameter::Required,
            std::vector<std::string>,
            parameter::Vector>;
        SnapshotImportCommand &snapshotImportCommand =
            emplaceParameter<SnapshotImportCommand>(
                "cmd",
                std::string("Command to run to import snapshots."));

        PasswordParameter passwordParameter{*this};

        void execute() override
        {
            ExportParams const exportParams = {
                .password = passwordParameter.maybeReadPassword(*repositoryParams),
                .importCommand = snapshotImportCommand.get()
            };

            auto backendDriverFactory = createBackendDriverFactory();

            auto const exitStatus = export_repository(*repositoryStructure,
                                                      *repositoryParams,
                                                      *backendDriverFactory->create(),
                                                      exportParams);
            if (exitStatus != ExitStatus::SUCCESS)
                ::exit(static_cast<int>(exitStatus));
        }

        Main &getMain() override { return getParent(); }

        std::string_view getSelfDocumentation() const override
        { return "Copies the source repository to the target repository."; }
    };


    struct PruneDataset final : DriverRequiringCommandHelper<SubCommand<Main>>
    {
        OptionalNodeParameter &nodeParameter =
            emplaceParameter<OptionalNodeParameter>(
                "node",
                "Node to prune as-of. If not given, "
                "then the most recent valid (non-quarantined) node is used.");

        Main &getMain() override { return getParent(); }

        void execute() override
        {
            PruneDatasetParams pruneDatasetParams {
                .node = nodeParameter.get()
            };

            prune_dataset(*repositoryStructure,
                          *repositoryParams,
                          *createBackendDriverFactory()->create(),
                          pruneDatasetParams);
        }

        std::string_view getSelfDocumentation() const override
        {
            return "Remove old snapshot objects from the live dataset.";
        }

        using DriverRequiringCommandHelper::DriverRequiringCommandHelper;
    };


    struct PruneRepository final : SubCommand<Main>
    {
        void execute() override
        {}

        std::string_view getSelfDocumentation() const override
        {
            return "Remove old files from the repository, "
                "without touching the underlying dataset.";
        }

        using SubCommand::SubCommand;
    };


    struct ListNodes final : SubCommand<Main>
    {
        BooleanParameter &showTimestamps =
            emplaceParameter<BooleanParameter>(
                Parameter::Defaulted{{"false"}},
                "show-timestamps", "Show node timestamps?");

        void execute() override
        {
            RepositoryStructure const repositoryStructure{getParent().repositoryPath.get()};
            list_nodes(repositoryStructure,
                       *getParent().createRepositoryParams(repositoryStructure),
                       ListNodesParams {
                           .show_timestamps = showTimestamps.get()
                       });
        }

        std::string_view getSelfDocumentation() const override
        {
            return "List nodes.";
        }

        using SubCommand::SubCommand;
    };

    struct Alive final : SubCommand<Main>
    {
        using RequiredNode = parameter::ParameterHelper<Parameter::Required,
                                                        Node,
                                                        parameter::Scalar,
                                                        parameter::Integer<ssize_t>,
                                                        parameter::NonNegative,
                                                        DiffmongerNode>;
        RequiredNode &n = emplaceParameter<RequiredNode>("node",
                                                         "Node.");
        void execute() override
        {
            n.get().alive_after([] (Node const i) { printf("%zu\n", i.value); });
        }

        std::string_view getSelfDocumentation() const override
        {
            return "Returns the set of nodes (in order) that will be alive after the given "
                "node is created. This command is useful for scripting.";
        }

        using SubCommand::SubCommand;
    };

    struct Help final : SubCommand<Main>
    {
        void execute() override
        {
            std::cout << documenter::document(getParent()) << "\n";
        }

        std::string_view getSelfDocumentation() const override
        {
            return "Help message.";
        }

        using SubCommand::SubCommand;
    };

    static InitBase &emplaceInitCommand(
        Main &main,
        InitBase::ModeVariant initModeVariant);

    Main(std::string name,
         InitBase::ModeVariant initModeVariant)
        : RootCommand(std::move(name)),
          init(emplaceInitCommand(*this, std::move(initModeVariant)))
    {
        Command::emplaceSubCommand<Alive>(*this, "alive");
        Command::emplaceSubCommand<ShowInit>(*this, "show-init");
        Command::emplaceSubCommand<InitKeyPair>(*this, "init-keypair");
        Command::emplaceSubCommand<Snapshot>(*this, "snapshot");
        Command::emplaceSubCommand<SnapshotImport>(*this, "snapshot-import");
        Command::emplaceSubCommand<Restore>(*this, "restore");
        Command::emplaceSubCommand<PruneDataset>(*this, "prune-dataset");
        Command::emplaceSubCommand<PruneRepository>(*this, "prune-repository");
        Command::emplaceSubCommand<ExportRepository>(*this, "export-repository");
        Command::emplaceSubCommand<Help>(*this, "help");
    }


    static std::unique_ptr<RepositoryParams>
    createRepositoryParams(RepositoryStructure const &repositoryStructure,
                           DriverRequiringCommand *commandToExtend = nullptr)
    {
        Main retrospectiveMain{
            repositoryStructure.getInitArgs().front(),
            Main::InitBase::RetrospectiveMode {
                .commandToExtend = commandToExtend,
                .repositoryStructureFormatUuid = repositoryStructure.getRepositoryStructureFormatUuid()
            }};

        Parser::parse(&retrospectiveMain,
                      span_exc_front(repositoryStructure.getInitArgs()));

        return std::move(retrospectiveMain.init.repositoryParams);
    }
};

struct InitV1 final : Main::InitBase
{
    static constexpr Uuid repositoryStructureFormatUuid{
        make_array_cast<std::byte>(0x8d,0x76,0xc7,0x4f,0x60,0x6f,0x7a,0xdd,
                                   0x0a,0x29,0xef,0xd4,0xc2,0x86,0x69,0xc1)};

    struct ZfsSubCommand final : DriverSubCommandBase
    {
        using DriverSubCommandBase::DriverSubCommandBase;

        BooleanParameter &raw =
            emplaceParameter<BooleanParameter>(
                Parameter::Defaulted{{"false"}}, "raw", "Make zfs sends raw?");

        void extendCommand(Main::DriverRequiringCommand
                           &driverRequiringCommand) const override
        {
            using Dataset = parameter::ParameterHelper<Parameter::Required,
                                                       std::string,
                                                       parameter::Scalar>;
            Dataset *dataset = &driverRequiringCommand
                .asCommand().emplaceParameter<Dataset>("dataset", "Zfs dataset");

            using ReceiveArgs =
                parameter::ParameterHelper<Parameter::Defaulted,
                                           std::vector<std::string>,
                                           parameter::Vector>;

            ReceiveArgs *receiveArgs = &driverRequiringCommand.asCommand()
                .emplaceParameter<ReceiveArgs>(
                    Parameter::Defaulted{{"-u"}},
                    "receive-args",
                    "Additional arguments that are added to the 'zfs receive' command");

            using InitialReceiveArgs =
                parameter::ParameterHelper<Parameter::Optional,
                                           std::vector<std::string>,
                                           parameter::Vector>;
            InitialReceiveArgs *initialReceiveArgs = &driverRequiringCommand.asCommand()
                .emplaceParameter<InitialReceiveArgs>(
                    "initial-receive-args",
                    std::string("Like ")
                    .append(receiveArgs->name)
                    .append(", but only applies to the first issued "
                            "'zfs receive' command. If not given, "
                            "then its value is taken from ")
                    .append(receiveArgs->name));

            driverRequiringCommand.setBackendDriverFactoryFunctor(
                // No lifetime issues here regarding the captured dataset pointer;
                // see comments in setBackendDriverFactoryFunctor().
                [dataset,
                 receiveArgs,
                 initialReceiveArgs,
                 raw=raw.get()]
                () -> std::unique_ptr<BackendDriver::Factory>
                {
                    ZfsBackendDriver::RepositoryParams const repositoryParams {
                        .raw = raw };
                    ZfsBackendDriver::DriverArguments const driverArguments {
                        .datasetName = dataset->get(),
                        .receive0_args = (
                            initialReceiveArgs->get()
                            ? initialReceiveArgs->get().value()
                            : receiveArgs->get()),
                        .receiven_args = receiveArgs->get()
                    };

                    return ZfsBackendDriver::createFactory(repositoryParams,
                                                           driverArguments);
                });
        }

        std::string_view getSelfDocumentation() const override
        {
            return "Initiate the repository with a zfs backend.";
        }
    };

    using MinDurationBetweenSnapshots =
        parameter::ParameterHelper<Parameter::Defaulted,
                                   std::string,
                                   parameter::Scalar>;
    MinDurationBetweenSnapshots &minDurationBetweenSnapshots =
        emplaceParameter<MinDurationBetweenSnapshots>(
            Parameter::Defaulted{{"1m"}},
            "min-duration-between-snapshots",
            "Minimum duration between successive snapshots.\n"
            "Any snapshots made in excess of this duration are quarantined for such\n"
            "time until they can be replayed without exceeding the limit.\n"
            "While quarantined, the snapshots exist and can be restored,\n"
            "but will never lead to the pruning of old snapshots.\n"
            "This prevents ransomware from flooding snapshots in order to trigger\n"
            "early pruning of legitimate snapshots.");

    BooleanParameter &encryption =
        emplaceParameter<BooleanParameter>(
            Parameter::Defaulted{{"true"}},
            "encryption",
            "Set to false to disable encryption.");

    InitV1(auto const &parent, ModeVariant modeVariant)
        : InitBase(parent, std::move(modeVariant))
    {
        emplaceSubCommand<ZfsSubCommand>(*this, "zfs");
    }

    void before_subcommand(Command &) override
    {
        repositoryParams = std::make_unique<RepositoryParams>();
        repositoryParams->encryption = encryption.get();
        repositoryParams->min_seconds_between_snapshots =
            [&]
            {
                auto const x = minDurationBetweenSnapshots.get();
                size_t value;
                if (!x.empty())
                {
                    auto numend = x.data() + x.size() - 1;
                    auto const r =
                        std::from_chars(x.data(), x.data() + x.size(), value);
                    if ((r.ec != std::errc{}) || (r.ptr != numend))
                        throw std::runtime_error(
                            "Invalid value (" + x + ") for " +
                            minDurationBetweenSnapshots.name);

                    switch (*numend)
                    {
                    case 's':
                        return value; break;
                    case 'm':
                        return 60*value; break;
                    case 'h':
                        return 60*60*value; break;
                    case 'd':
                        return 60*60*24*value; break;
                    case 'w':
                        return 60*60*24*7*value; break;
                    default:
                        throw std::runtime_error(
                            "Invalid suffix for " + minDurationBetweenSnapshots.name);

                    }
                }
                throw std::runtime_error(
                    "Invalid value for " + minDurationBetweenSnapshots.name);
            }();
    }

    std::string_view getSelfDocumentation() const override
    {
        return "Creates a new repository at the path given in R.";
    }
};

Main::InitBase &Main::emplaceInitCommand(Main &main, InitBase::ModeVariant initModeVariant)
{
    if (auto const *retrospectiveMode =
        std::get_if<InitBase::RetrospectiveMode>(&initModeVariant))
    {
        if (retrospectiveMode->repositoryStructureFormatUuid
            == InitV1::repositoryStructureFormatUuid)
            return emplaceSubCommand<InitV1>(main, std::move(initModeVariant));
        else
            throw std::runtime_error("Unsupported repository parameters version");
    } else
        return emplaceSubCommand<InitV1>(main, std::move(initModeVariant));
}

} // <-- Anonymous namespace

void my_terminate() {
    if (auto eptr = std::current_exception()) {
        try {
            std::rethrow_exception(eptr);
        } catch (const std::exception& e) {
            std::cerr << "Unhandled exception: " << e.what() << '\n';
        } catch (...) {
            std::cerr << "Unhandled non-std::exception\n";
        }
    } else {
        std::cerr << "Terminate called without active exception\n";
    }

    std::abort(); // preserve default “crash” behavior for debugger
}

namespace cli_interface {
int run(int argc, char const * const * const argv)
{
    Main main(argv[0],
              Main::InitBase::ExecutionMode{
                  .args = std::vector<std::string>(argv, argv + argc) });
    try
    {
        parse(main, argc, argv);
        return 0;
    } catch (argument_error const &e)
    {
        throw argument_error(
            std::string("Argument error: ")
            .append(e.what())
            .append("\n\n")
            .append(documenter::document(main)));
    }
}
}


std::unique_ptr<RepositoryParams>
createRepositoryParams(RepositoryStructure const &repositoryStructure)
{
    return Main::createRepositoryParams(repositoryStructure);
}
} // <-- namespace diffmonger
