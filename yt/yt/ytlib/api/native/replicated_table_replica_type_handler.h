#pragma once

#include "private.h"

namespace NYT::NApi::NNative {

////////////////////////////////////////////////////////////////////////////////

ITypeHandlerPtr CreateReplicatedTableReplicaTypeHandler(TClient* client);

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NApi::NNative
