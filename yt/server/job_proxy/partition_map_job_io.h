#pragma once

#include "public.h"

namespace NYT {
namespace NJobProxy {

////////////////////////////////////////////////////////////////////

std::unique_ptr<IUserJobIO> CreatePartitionMapJobIO(IJobHostPtr host);

////////////////////////////////////////////////////////////////////

} // namespace NJobProxy
} // namespace NYT
