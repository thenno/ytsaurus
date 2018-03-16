#pragma once

#include "public.h"

namespace NYT {
namespace NCoreDump {

////////////////////////////////////////////////////////////////////////////////

ICoreDumperPtr CreateCoreDumper(TCoreDumperConfigPtr config);

////////////////////////////////////////////////////////////////////////////////

} // namespace NCoreDump
} // namespace NYT
