#pragma once

#include "public.h"

#include <ytlib/misc/ref.h>

#include <ytlib/bus/public.h>

#include <ytlib/rpc/rpc.pb.h>

namespace NYT {
namespace NRpc {

////////////////////////////////////////////////////////////////////////////////

//! Represents an RPC request at server-side.
struct IServiceContext
    : public virtual TRefCounted
{
    //! Returns the message that contains the request being handled.
    virtual NBus::IMessagePtr GetRequestMessage() const = 0;

    //! Returns the id of the request.
    /*!
     *  These ids are assigned by the client to distinguish between responses.
     *  The server should not rely on their uniqueness.
     *  
     *  #NullRequestId is a possible value.
     */
    virtual const TRequestId& GetRequestId() const = 0;

    //! Returns the instant when the request was first issued by the client, if known.
    virtual TNullable<TInstant> GetRequestStartTime() const = 0;

    //! Returns the instant when the current retry of request was issued by the client, if known.
    virtual TNullable<TInstant> GetRetryStartTime() const = 0;

    //! Returns request priority for reordering purposes.
    virtual i64 GetPriority() const = 0;

    //! Returns the requested path.
    virtual const Stroka& GetPath() const = 0;

    //! Returns the requested verb.
    virtual const Stroka& GetVerb() const = 0;

    //! Returns True if the request if one-way, i.e. replying to it is not possible.
    virtual bool IsOneWay() const = 0;

    //! Returns True if the request was already replied.
    virtual bool IsReplied() const = 0;

    //! Signals that the request processing is complete and sends reply to the client.
    virtual void Reply(const TError& error) = 0;

    //! Parses the message and forwards to the client.
    virtual void Reply(NBus::IMessagePtr message) = 0;

    //! Returns the error that was previously set by #Reply.
    /*!
     *  Calling #GetError before #Reply is forbidden.
     */
    virtual const TError& GetError() const = 0;

    //! Returns the request body.
    virtual TSharedRef GetRequestBody() const = 0;

    //! Returns the response body.
    virtual TSharedRef GetResponseBody() = 0;

    //! Sets the response body.
    virtual void SetResponseBody(const TSharedRef& responseBody) = 0;

    //! Returns a vector of request attachments.
    virtual std::vector<TSharedRef>& RequestAttachments() = 0;

    //! Returns request attributes.
    virtual NYTree::IAttributeDictionary& RequestAttributes() = 0;

    //! Returns a vector of response attachments.
    virtual std::vector<TSharedRef>& ResponseAttachments() = 0;

    //! Returns response attributes.
    virtual NYTree::IAttributeDictionary& ResponseAttributes() = 0;

    //! Sets and immediately logs the request logging info.
    virtual void SetRequestInfo(const Stroka& info) = 0;

    //! Returns the previously set request logging info.
    virtual Stroka GetRequestInfo() const = 0;

    //! Sets the response logging info. This info will be logged when the context is replied.
    virtual void SetResponseInfo(const Stroka& info) = 0;

    //! Returns the currently set response logging info.
    virtual Stroka GetResponseInfo() = 0;

    //! Wraps the given action into an exception guard that logs the exception and replies.
    virtual TClosure Wrap(const TClosure& action) = 0;


    // Extension methods.
    void SetRequestInfo(const char* format, ...);
    void SetResponseInfo(const char* format, ...);

};

////////////////////////////////////////////////////////////////////////////////

//! Represents an abstract service registered within TServer.
/*!
 *  \note All methods be be implemented as thread-safe.
 */
struct IService
    : public virtual TRefCounted
{
    //! Applies a new configuration.
    virtual void Configure(NYTree::INodePtr config) = 0;

    //! Returns the name of the service.
    virtual Stroka GetServiceName() const = 0;

    //! Handles incoming request.
    virtual void OnRequest(
        const NProto::TRequestHeader& header,
        NBus::IMessagePtr message,
        NBus::IBusPtr replyBus) = 0;
};

////////////////////////////////////////////////////////////////////////////////

} // namespace NRpc
} // namespace NYT
