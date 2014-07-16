#pragma once

#include "common.h"

#include <core/ytree/public.h>

namespace NYT {
namespace NLog {

////////////////////////////////////////////////////////////////////////////////

class TLogManager
{
public:
    TLogManager();

    static TLogManager* Get();

    void Configure(NYTree::INodePtr node);
    void Configure(const Stroka& fileName, const NYPath::TYPath& path);

    void AfterFork();

    void Initialize() const;

    void Shutdown();

    int GetVersion() const;
    ELogLevel GetMinLevel(const Stroka& category) const;

    void Enqueue(TLogEvent&& event);

    void Reopen();

private:
    class TImpl;
    mutable TIntrusivePtr<TImpl> Impl_;
};

////////////////////////////////////////////////////////////////////////////////

} // namespace NLog
} // namespace NYT

template <>
struct TSingletonTraits<NYT::NLog::TLogManager>
{
    enum
    {
        Priority = 2048
    };
};
