#pragma once

#include "public.h"
#include "process.h"

#include <yt/yt/core/misc/ref.h>

namespace NYT {

////////////////////////////////////////////////////////////////////////////////

struct TSubprocessResult
{
    TSharedRef Output;
    TSharedRef Error;
    TError Status;
};

////////////////////////////////////////////////////////////////////////////////

class TSubprocess
{
public:
    explicit TSubprocess(const TString& path, bool copyEnv = true);

    static TSubprocess CreateCurrentProcessSpawner();

    void AddArgument(TStringBuf arg);
    void AddArguments(std::initializer_list<TStringBuf> args);

    TSubprocessResult Execute(const TSharedRef& input = TSharedRef::MakeEmpty());
    void Kill(int signal);

    TString GetCommandLine() const;

    TProcessBasePtr GetProcess() const;

private:
    const TProcessBasePtr Process_;
};

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT
