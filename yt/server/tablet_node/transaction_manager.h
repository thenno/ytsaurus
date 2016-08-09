#pragma once

#include "public.h"

#include <yt/server/cell_node/public.h>

#include <yt/server/hive/transaction_manager.h>

#include <yt/server/hydra/composite_automaton.h>
#include <yt/server/hydra/entity_map.h>

#include <yt/ytlib/tablet_client/tablet_service.pb.h>

#include <yt/core/actions/signal.h>

#include <yt/core/ytree/public.h>
#include <yt/server/hive/helpers.h>

namespace NYT {
namespace NTabletNode {

////////////////////////////////////////////////////////////////////////////////

class TTransactionManager
    : public NHiveServer::ITransactionManager
{
public:
    //! Raised when a new transaction is started.
    DECLARE_SIGNAL(void(TTransaction*), TransactionStarted);

    //! Raised when a transaction is prepared.
    DECLARE_SIGNAL(void(TTransaction*), TransactionPrepared);

    //! Raised when a transaction is committed.
    DECLARE_SIGNAL(void(TTransaction*), TransactionCommitted);

    //! Raised when a transaction is aborted.
    DECLARE_SIGNAL(void(TTransaction*), TransactionAborted);

    //! Raised on epoch finish for each transaction (both persistent and transient)
    //! to help all dependent subsystems to reset their transient transaction-related
    //! state.
    DECLARE_SIGNAL(void(TTransaction*), TransactionTransientReset);

public:
    TTransactionManager(
        TTransactionManagerConfigPtr config,
        TTabletSlotPtr slot,
        NCellNode::TBootstrap* bootstrap);

    ~TTransactionManager();

    //! Finds transaction by id.
    //! If it does not exist then creates a new transaction
    //! (either persistent or transient, depending on #transient).
    TTransaction* GetOrCreateTransaction(
        const TTransactionId& transactionId,
        TTimestamp startTimestamp,
        TDuration timeout,
        bool transient);

    //! Finds a transaction by id.
    //! If a persistent instance is found, just returns it.
    //! If a transient instance is found, makes is persistent and returns it.
    //! Fails if no transaction is found.
    TTransaction* MakeTransactionPersistent(const TTransactionId& transactionId);

    //! Returns the full list of transactions, including transient and persistent.
    std::vector<TTransaction*> GetTransactions();

    void RegisterPrepareActionHandler(const NHiveServer::TTransactionPrepareActionHandlerDescriptor& descriptor);
    void RegisterCommitActionHandler(const NHiveServer::TTransactionCommitActionHandlerDescriptor& descriptor);
    void RegisterAbortActionHandler(const NHiveServer::TTransactionAbortActionHandlerDescriptor& descriptor);

    NYTree::IYPathServicePtr GetOrchidService();

private:
    class TImpl;
    const TIntrusivePtr<TImpl> Impl_;

    /// ITransactionManager overrides.
    virtual void PrepareTransactionCommit(
        const TTransactionId& transactionId,
        bool persistent) override;
    virtual void PrepareTransactionAbort(
        const TTransactionId& transactionId,
        bool force) override;
    virtual void CommitTransaction(
        const TTransactionId& transactionId,
        TTimestamp commitTimestamp) override;
    virtual void AbortTransaction(
        const TTransactionId& transactionId,
        bool force) override;
    virtual void PingTransaction(
        const TTransactionId& transactionId,
        bool pingAncestors) override;
    virtual void RegisterAction(
        const TTransactionId& transactionId,
        const TTransactionActionData& data) override;
};

DEFINE_REFCOUNTED_TYPE(TTransactionManager)

////////////////////////////////////////////////////////////////////////////////

} // namespace NTabletNode
} // namespace NYT
