#include "stdafx.h"

#include <ytlib/misc/guid.h>

#include <contrib/testing/framework.h>

namespace NYT {

////////////////////////////////////////////////////////////////////////////////

TEST(TGuidTest, SerializationToProto)
{
    TGuid guid = TGuid::Create();
    NProto::TGuid protoGuid = guid.ToProto();
    TGuid deserializedGuid = TGuid::FromProto(protoGuid);
    EXPECT_EQ(guid, deserializedGuid);
}


////////////////////////////////////////////////////////////////////////////////

} // namespace NYT
