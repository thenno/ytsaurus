#include "stdafx.h"
#include "bootstrap.h"
#include "config.h"
#include "private.h"
#include "job.h"
#include "job_manager.h"
#include "supervisor_service.h"
#include "environment.h"
#include "environment_manager.h"
#include "unsafe_environment.h"
#include "scheduler_connector.h"
#include "slot.h"

#include <ytlib/rpc/server.h>

#include <server/cell_node/bootstrap.h>
#include <server/cell_node/config.h>

#include <server/job_proxy/config.h>

#include <server/chunk_holder/bootstrap.h>
#include <server/chunk_holder/config.h>
#include <server/chunk_holder/chunk_cache.h>

#ifdef _unix_
#include <sys/types.h>
#include <sys/stat.h>
#endif

namespace NYT {
namespace NExecAgent {

using namespace NRpc;
using namespace NCellNode;

////////////////////////////////////////////////////////////////////////////////

static NLog::TLogger& SILENT_UNUSED Logger = ExecAgentLogger;

////////////////////////////////////////////////////////////////////////////////

TBootstrap::TBootstrap(
    TExecAgentConfigPtr config,
    NCellNode::TBootstrap* nodeBootstrap)
    : Config(config)
    , NodeBootstrap(nodeBootstrap)
{
    YASSERT(config);
    YASSERT(nodeBootstrap);
}

TBootstrap::~TBootstrap()
{ }

void TBootstrap::Initialize()
{
    JobProxyConfig = New<NJobProxy::TJobProxyConfig>();
    
    JobProxyConfig->MemoryWatchdogPeriod = Config->MemoryWatchdogPeriod;
    
    JobProxyConfig->Logging = Config->JobProxyLogging;
    
    JobProxyConfig->MemoryLimitMultiplier = Config->MemoryLimitMultiplier;
    
    JobProxyConfig->SandboxName = SandboxName;
    JobProxyConfig->AddressResolver = NodeBootstrap->GetConfig()->AddressResolver;
    JobProxyConfig->SupervisorConnection = New<NBus::TTcpBusClientConfig>();
    JobProxyConfig->SupervisorConnection->Address = NodeBootstrap->GetLocalDescriptor().Address;
    JobProxyConfig->SupervisorRpcTimeout = Config->SupervisorRpcTimeout;
    JobProxyConfig->MasterRpcTimeout = NodeBootstrap->GetConfig()->Masters->RpcTimeout;
    // TODO(babenko): consider making this priority configurable
    JobProxyConfig->SupervisorConnection->Priority = 6;

    JobControlEnabled = false;

#if defined(_unix_) && !defined(_darwin_)
    if (Config->EnforceJobControl) {
        uid_t ruid, euid, suid;
        YCHECK(getresuid(&ruid, &euid, &suid) == 0);
        if (suid == 0) {
            JobControlEnabled = true;
        }
        umask(0000);
    }
#endif

    if (!JobControlEnabled) {
        if (Config->EnforceJobControl) {
            LOG_FATAL("Job control disabled, please run as root");
        } else {
            LOG_WARNING("Job control disabled, cannot kill jobs and use memory limits watcher");
        }
    }

    JobManager = New<TJobManager>(Config->JobManager, this);
    JobManager->Initialize();

    auto supervisorService = New<TSupervisorService>(this);
    NodeBootstrap->GetRpcServer()->RegisterService(supervisorService);

    EnvironmentManager = New<TEnvironmentManager>(Config->EnvironmentManager);
    EnvironmentManager->Register("unsafe", CreateUnsafeEnvironmentBuilder());

    SchedulerConnector = New<TSchedulerConnector>(Config->SchedulerConnector, this);
    SchedulerConnector->Start();
}

bool TBootstrap::IsJobControlEnabled() const
{
    return JobControlEnabled;
}

TExecAgentConfigPtr TBootstrap::GetConfig() const
{
    return Config;
}

IInvokerPtr TBootstrap::GetControlInvoker() const
{
    return NodeBootstrap->GetControlInvoker();
}

IChannelPtr TBootstrap::GetMasterChannel() const
{
    return NodeBootstrap->GetMasterChannel();
}

IChannelPtr TBootstrap::GetSchedulerChannel() const
{
    return NodeBootstrap->GetSchedulerChannel();
}

const NNodeTrackerClient::TNodeDescriptor& TBootstrap::GetLocalDescriptor() const
{
    return NodeBootstrap->GetLocalDescriptor();
}

TJobManagerPtr TBootstrap::GetJobManager() const
{
    return JobManager;
}

TEnvironmentManagerPtr TBootstrap::GetEnvironmentManager() const
{
    return EnvironmentManager;
}

NChunkHolder::TChunkCachePtr TBootstrap::GetChunkCache() const
{
    return NodeBootstrap->GetChunkHolderBootstrap()->GetChunkCache();
}

NJobProxy::TJobProxyConfigPtr TBootstrap::GetJobProxyConfig() const
{
    return JobProxyConfig;
}

NCellNode::TNodeMemoryTracker& TBootstrap::GetMemoryUsageTracker()
{
    return NodeBootstrap->GetMemoryUsageTracker();
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NExecAgent
} // namespace NYT
