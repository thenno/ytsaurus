#pragma once

#include <core/misc/blob.h>

#include <core/logging/log.h>

namespace NYT {
namespace NPipes {
namespace NDetail {

////////////////////////////////////////////////////////////////////////////////

class TNonblockingWriter
{
public:
    // TODO(babenko): Owns this fd?
    explicit TNonblockingWriter(int fd);
    ~TNonblockingWriter();

    TErrorOr<size_t> Write(const char* data, size_t size);

    TError Close();

    bool IsClosed() const;

private:
    int FD_;

    bool Closed_;

    NLog::TLogger Logger;
};

////////////////////////////////////////////////////////////////////////////////

} // namespace NDetail
} // namespace NPipes
} // namespace NYT
