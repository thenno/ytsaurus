#include "stdafx.h"
#include "format.h"

namespace NYT {
namespace NChunkClient {

////////////////////////////////////////////////////////////////////////////////

const ui64 TChunkMetaHeader::ExpectedSignature = 0x313030484d435459ull; // YTCMH001

////////////////////////////////////////////////////////////////////////////////

} // namespace NChunkClient
} // namespace NYT

