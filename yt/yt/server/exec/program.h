#include <yt/server/lib/user_job_executor/config.h>
#include <yt/server/lib/user_job_synchronizer_client/user_job_synchronizer.h>

#include <yt/ytlib/program/program.h>
#include <yt/ytlib/program/program_config_mixin.h>
#include <yt/ytlib/program/program_tool_mixin.h>
#include <yt/ytlib/program/helpers.h>

#include <yt/library/process/pipe.h>

#include <yt/core/logging/formatter.h>
#include <yt/core/logging/log_manager.h>

#include <yt/core/misc/proc.h>
#include <yt/core/misc/fs.h>

#include <sys/ioctl.h>

#ifdef _unix_
    #include <sys/resource.h>
#endif

namespace NYT::NExec {

using namespace NUserJobSynchronizerClient;
using namespace NUserJobExecutor;

////////////////////////////////////////////////////////////////////////////////

class TExecProgram
    : public TProgram
    , public TProgramConfigMixin<TUserJobExecutorConfig>
{
public:
    TExecProgram()
        : TProgramConfigMixin(Opts_, false)
    { }

protected:
    virtual void DoRun(const NLastGetopt::TOptsParseResult& parseResult) override
    {
        auto config = GetConfig();

        JobId_ = config->JobId;
        ExecutorStderr_ = TFile{"../executor_stderr", EOpenModeFlag::WrOnly | EOpenModeFlag::ForAppend | EOpenModeFlag::OpenAlways};

        if (HandleConfigOptions()) {
            return;
        }

        ConfigureUids();
        ConfigureCrashHandler();

        TThread::SetCurrentThreadName("ExecMain");

        // Don't start any other singleton or parse config in executor mode.
        // Explicitly shut down log manager to ensure it doesn't spoil dup-ed descriptors.
        NLogging::TLogManager::StaticShutdown();

        if (config->Uid > 0) {
            SetUid(config->Uid);
        }

        TError executorError;

        try {
            auto enableCoreDump = config->EnableCoreDump;
            struct rlimit rlimit = {
                enableCoreDump ? RLIM_INFINITY : 0,
                enableCoreDump ? RLIM_INFINITY : 0
            };

            auto rv = setrlimit(RLIMIT_CORE, &rlimit);
            if (rv) {
                THROW_ERROR_EXCEPTION("Failed to configure core dump limits")
                    << TError::FromSystem();
            }

            for (const auto& pipe : config->Pipes) {
                auto streamFd = pipe->FD;
                const auto& path = pipe->Path;

                try {
                    // Behaviour of named pipe:
                    // reader blocks on open if no writer and O_NONBLOCK is not set,
                    // writer blocks on open if no reader and O_NONBLOCK is not set.
                    auto flags = (pipe->Write ? O_WRONLY : O_RDONLY);
                    auto fd = HandleEintr(::open, path.c_str(), flags);
                    if (fd == -1) {
                        THROW_ERROR_EXCEPTION("Failed to open named pipe")
                            << TErrorAttribute("path", path)
                            << TError::FromSystem();
                    }

                    if (streamFd != fd) {
                        SafeDup2(fd, streamFd);
                        SafeClose(fd, false);
                    }
                } catch (const std::exception& ex) {
                    THROW_ERROR_EXCEPTION("Failed to prepare named pipe")
                        << TErrorAttribute("path", path)
                        << TErrorAttribute("fd", streamFd)
                        << ex;
                }
            }
        } catch (const std::exception& ex) {
            executorError = ex;
        }

        if (!executorError.IsOK()) {
            LogToStderr(Format("Failed to prepare pipes, unexpected executor error\n%v", executorError));
            Exit(4);
        }

        std::vector<char*> env;
        for (auto environment : config->Environment) {
            env.push_back(const_cast<char*>(environment.c_str()));
        }
        env.push_back(const_cast<char*>("SHELL=/bin/bash"));
        env.push_back(nullptr);

        std::vector<const char*> args;
        args.push_back("/bin/bash");

        TString command;
        if (!config->Command.empty()) {
            // :; is added avoid fork/exec (one-shot) optimization.
            command = ":; " + config->Command;
            args.push_back("-c");
            args.push_back(command.c_str());
        }
        args.push_back(nullptr);

        // We are ready to execute user code, send signal to JobProxy.
        try {
            auto jobProxyControl = CreateUserJobSynchronizerClient(config->UserJobSynchronizerConnectionConfig);
            jobProxyControl->NotifyExecutorPrepared();
        } catch (const std::exception& ex) {
            LogToStderr(Format("Unable to notify job proxy\n%v", ex.what()));
            Exit(5);
        }

        TryExecve(
            "/bin/bash",
            args.data(),
            env.data());

        LogToStderr(Format("execve failed: %v", TError::FromSystem()));
        Exit(6);
    }

    virtual void OnError(const TString& message) const noexcept override
    {
        LogToStderr(message);
    }

private:
    void LogToStderr(const TString& message) const
    {
        auto logRecord = Format("%v (JobId: %v)", message, JobId_);

        if (!ExecutorStderr_.IsOpen()) {
            Cerr << logRecord << Endl;
            return;
        }

        ExecutorStderr_.Write(logRecord.data(), logRecord.size());
        ExecutorStderr_.Flush();
    }

    mutable TFile ExecutorStderr_;
    TString JobId_;
};

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NExec
