#include "execution_context.h"
#include "execution_stack.h"

#ifdef CXXABIv1
    #include <cxxabi.h>
#endif // CXXABIv1

////////////////////////////////////////////////////////////////////////////////

namespace NYT::NConcurrency {

////////////////////////////////////////////////////////////////////////////////

TExecutionContext::TExecutionContext(
    TExecutionStack* stack,
    ITrampoLine* trampoline)
    : ContContext_({
        trampoline,
        TArrayRef(static_cast<char*>(stack->GetStack()), stack->GetSize())})
{
#ifdef CXXABIv1
    static_assert(
        sizeof(__cxxabiv1::__cxa_eh_globals) == TExecutionContext::EHSize,
        "Size mismatch of __cxa_eh_globals structure");
#endif
}

void TExecutionContext::SwitchTo(TExecutionContext* target)
{
#ifdef CXXABIv1
    auto* eh = __cxxabiv1::__cxa_get_globals();
    ::memcpy(EH_.data(), eh, EHSize);
    ::memcpy(eh, target->EH_.data(), EHSize);
#endif
    ContContext_.SwitchTo(&target->ContContext_);
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NTY::NConcurrency

