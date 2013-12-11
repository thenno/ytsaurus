#pragma once

#include "public.h"

namespace NYT {
namespace NRpc {

////////////////////////////////////////////////////////////////////////////////

//! Creates a channel that routes all requests to the local RPC server.
IChannelPtr CreateLocalChannel(IRpcServerPtr server);

////////////////////////////////////////////////////////////////////////////////

} // namespace NRpc
} // namespace NYT
