#pragma once

#include <yt/core/misc/public.h>

namespace NYT {

////////////////////////////////////////////////////////////////////////////////

namespace NProto {

class TWorkloadDescriptor;

} // namespace NProto

////////////////////////////////////////////////////////////////////////////////

//! The type of the workload.
//! Order is given by |GetBasicPriority| function and not by concrete enum values.
/*!
 *  This is the most fine-grained categorization available.
 *  Most subsystems will map EWorkloadCategory to their own coarser categories.
 *
 *  NB: This enum is serializable, so please keep values intact or advance
 *  protocol version where appropriate.
 */
DEFINE_ENUM(EWorkloadCategory,
    ((Idle)                    (0))
    ((SystemReplication)       (1))
    ((SystemRepair)            (2))
    ((SystemTabletCompaction)  (6))
    ((SystemTabletPartitioning)(7))
    ((SystemTabletPreload)     (8))
    ((UserBatch)               (3))
    ((UserRealtime)            (4))
    ((SystemRealtime)          (5))
);

struct TWorkloadDescriptor;

DECLARE_REFCOUNTED_CLASS(TWorkloadConfig)

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT
