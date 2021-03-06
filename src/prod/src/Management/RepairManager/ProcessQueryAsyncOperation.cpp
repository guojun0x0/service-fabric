// ------------------------------------------------------------
// Copyright (c) Microsoft Corporation.  All rights reserved.
// Licensed under the MIT License (MIT). See License.txt in the repo root for license information.
// ------------------------------------------------------------

#include "stdafx.h"

using namespace Common;
using namespace Management::RepairManager;
using namespace Query;
using namespace ServiceModel;
using namespace ClientServerTransport;

StringLiteral const TraceComponent("ProcessQueryAsyncOperation");

void ProcessQueryAsyncOperation::OnStart(AsyncOperationSPtr const& thisSPtr)
{
    MakeRequest(thisSPtr);
}

ErrorCode ProcessQueryAsyncOperation::End(Common::AsyncOperationSPtr const & asyncOperation, __out Transport::MessageUPtr &reply)
{
    auto casted = AsyncOperation::End<ProcessQueryAsyncOperation>(asyncOperation);
    if (casted->Error.IsSuccess())
    {
        if (casted->reply_)
        {
            reply = move(casted->reply_);
        }
    }

    return casted->Error;
}

void ProcessQueryAsyncOperation::MakeRequest(AsyncOperationSPtr const &thisSPtr)
{
    Store::ReplicaActivityId replicaActivityId(owner_.PartitionedReplicaId, activityId_);

    WriteInfo(TraceComponent, "{0} received query request {1} ({2})",
        replicaActivityId,
        queryName_,
        queryArgs_);

    auto error = owner_.ProcessQuery(queryName_, queryArgs_, activityId_, reply_);

    if (ClientRequestAsyncOperation::IsRetryable(error))
    {
        TimeSpan retryDelay = owner_.GetRandomizedOperationRetryDelay(error);
        {
            AcquireExclusiveLock lock(timerLock_);
            if (!thisSPtr->IsCompleted)   
            {
                WriteNoise(
                    TraceComponent,
                    "{0} retrying query due to error {1}",
                    replicaActivityId,
                    error);

                retryTimer_ = Timer::Create(TimerTagDefault, [this, thisSPtr](TimerSPtr const & timer)
                    { 
                        timer->Cancel();
                        this->MakeRequest(thisSPtr);
                    },
                    false); // dont allow concurrency
                retryTimer_->Change(retryDelay);
            }
        }
    }
    else
    {
        TryComplete(thisSPtr, error);
    }
}

void ProcessQueryAsyncOperation::OnTimeout(AsyncOperationSPtr const & thisSPtr)
{
    UNREFERENCED_PARAMETER(thisSPtr);

    this->CancelRetryTimer();
}

void ProcessQueryAsyncOperation::OnCancel()
{
    this->CancelRetryTimer();
}

void ProcessQueryAsyncOperation::OnCompleted()
{
    TimedAsyncOperation::OnCompleted();

    this->CancelRetryTimer();
}

void ProcessQueryAsyncOperation::CancelRetryTimer()
{
    TimerSPtr snap;
    {
        AcquireExclusiveLock lock(timerLock_); 
        snap = retryTimer_;
    }

    if (snap)
    {
        snap->Cancel();
    }
}
