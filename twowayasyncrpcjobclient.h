/*
*
* Copyright 2016, Google Inc.
* All rights reserved.
*/

#ifndef _twowayasyncrpcjobclient_h_
#define _twowayasyncrpcjobclient_h_

#include "msgx/asyncrpcjob.h"
#include <string>
#include <thread>
#include "boost/variant.hpp"

using EventProcessor = std::function<void(bool)>;

struct EventInfo
{
    // The function to be called for processing incoming event
    EventProcessor* eventProcessor;
    bool ok;
};


template<typename StubType, typename UpstreamDataType, typename DownstreamDataType>
struct TwoWayAsyncRpcJobClientHandlers :
    public AsyncRpcJobHandlersClient<StubType, UpstreamDataType, DownstreamDataType>
{
public:
    // Async client-side interface for bi-directional streaming, where the outgoing
    // message stream going to the server has messages of type UpstreamDataType, and the
    // incoming message stream coming from the server has messages of type DownstreamDataType.
    using ReaderWriter = grpc::ClientAsyncReaderWriter<UpstreamDataType,DownstreamDataType>;

    using PrepareAsyncRPC = std::function<std::unique_ptr<ReaderWriter>(
            grpc::ClientContext*,
            grpc::CompletionQueue*)>;

    // Creates an RPC object, returning
    // an instance to store in "call" but does not actually start the RPC
    // Because we are using the asynchronous API, we need to hold on to
    // the "call" instance in order to get updates on the ongoing RPC.
    PrepareAsyncRPC mPrepareAsyncRPC;
};

template<typename StubType, typename UpstreamDataType, typename DownstreamDataType>
class TwoWayAsyncRpcJobClient : public AsyncRpcJob
{
    using _ThisRpcTypeJobHandlers = TwoWayAsyncRpcJobClientHandlers<StubType, UpstreamDataType, DownstreamDataType>;

    public:

    TwoWayAsyncRpcJobClient(
            StubType* stub,
            grpc::CompletionQueue* cq,
            _ThisRpcTypeJobHandlers jobHandlers) :
        m_Stub(stub)
        , m_CQ(cq)
        , m_Handlers(jobHandlers)
        , m_ClientStreamingDone(false) {

            mOnConnect = std::bind(&TwoWayAsyncRpcJobClient::OnConnect, this, std::placeholders::_1);
            mOnRead = std::bind(&TwoWayAsyncRpcJobClient::OnRead, this, std::placeholders::_1);
            mOnWrite = std::bind(&TwoWayAsyncRpcJobClient::OnWrite, this, std::placeholders::_1);
            mOnFinish = std::bind(&TwoWayAsyncRpcJobClient::OnFinish, this, std::placeholders::_1);
            mOnDone = std::bind(&AsyncRpcJob::OnDone, this, std::placeholders::_1);

            //inform the application of the entities it can use to respond to the rpc
            m_SendUpstream = std::bind(&TwoWayAsyncRpcJobClient::SendUpstream, this, std::placeholders::_1);
            jobHandlers.rpcJobContextHandler(m_Stub, this, &m_ClientContext, m_SendUpstream);

            // creates an RPC object, returning
            // an instance to store in "call" but does not actually start the RPC
            // Because we are using the asynchronous API, we need to hold on to
            // the "call" instance in order to get updates on the ongoing RPC.
            AsyncRpcJob::OperStarted(AsyncRpcJob::OPER_TYPE_START_CALL);
            m_RW = m_Handlers.mPrepareAsyncRPC(&m_ClientContext, m_CQ);

            // StartCall initiates the RPC call
            m_RW->StartCall(&mOnConnect);
        }

    private:

    bool SendUpstream(const UpstreamDataType* data)
    {
        if (data == nullptr && !m_ClientStreamingDone)
        {
            // Wait for client to finish the all the requests. If you want to cancel, use ClientContext::TryCancel.
            GPR_ASSERT(false);
            return false;
        }

        if (data != nullptr)
        {
            // We need to make a copy of the data because we need
            // to maintain it until we get a completion notification.
            m_UpstreamDataQue.push_back(*data);

            if (!AsyncWriteInProgress())
            {
                doSend();
            }
        }
        else
        {
            m_ClientStreamingDone = true;

            // Kick the async op if our state machine is not going to
            // be kicked from the completion queue
            if (!AsyncWriteInProgress())
            {
                doFinish();
            }
        }

        return true;
    }

    void OnConnect(bool ok)
    {
        if (AsyncRpcJob::OperFinished(AsyncRpcJob::OPER_TYPE_START_CALL))
        {
            if (ok)
            {
                AsyncRpcJob::OperStarted(AsyncRpcJob::OPER_TYPE_READ);
                m_RW->Read(&m_DownstreamData, &mOnRead);
            }
        }
    }

    void OnRead(bool ok)
    {
        if (AsyncRpcJob::OperFinished(AsyncRpcJob::OPER_TYPE_READ))
        {
            if (ok)
            {
                // inform application that a downstream data has arrived
                m_Handlers.ProcessDownstreamHandler(m_Stub, this, &m_DownstreamData);

                // queue up another read operation for this rpc
                AsyncRpcJob::OperStarted(AsyncRpcJob::OPER_TYPE_READ);
                m_RW->Read(&m_DownstreamData, &mOnRead);
            }
            else
            {
                m_ClientStreamingDone = true;
                m_Handlers.ProcessDownstreamHandler(m_Stub, this, nullptr);
            }
        }
    }

    void OnWrite(bool ok)
    {
        if (AsyncRpcJob::OperFinished(AsyncRpcJob::OPER_TYPE_WRITE))
        {
            // Get rid of the message that just finished.
            m_UpstreamDataQue.pop_front();

            if (ok)
            {
                if (!m_UpstreamDataQue.empty())
                {
                    // If we have more messages waiting to be sent, send them.
                    doSend();
                }
                else if (m_ClientStreamingDone)
                {
                    // Previous write completed and we did not have any pending write.
                    // If the application indicated a done operation, finish the rpc processing.
                    doFinish();
                }
            }
        }
    }

    void OnFinish(bool ok)
    {
        AsyncRpcJob::OperFinished(AsyncRpcJob::OPER_TYPE_FINISH);
    }

    void Done() override
    {
        //TODO
    }

    void doSend()
    {
        AsyncRpcJob::OperStarted(AsyncRpcJob::OPER_TYPE_WRITE);

        m_RW->Write(m_UpstreamDataQue.front(), &mOnWrite);
    }

    void doFinish()
    {
        AsyncRpcJob::OperStarted(AsyncRpcJob::OPER_TYPE_FINISH);

        grpc::Status status;
        m_RW->Finish(&status, &mOnFinish);
    }

    private:

    StubType* m_Stub;
    grpc::CompletionQueue* m_CQ;
    unique_ptr<typename _ThisRpcTypeJobHandlers::ReaderWriter> m_RW;
    grpc::ClientContext m_ClientContext;

    UpstreamDataType m_UpstreamData;
    DownstreamDataType m_DownstreamData;

    _ThisRpcTypeJobHandlers m_Handlers;

    typename _ThisRpcTypeJobHandlers::_SendUpstreamHandler m_SendUpstream;

    EventProcessor mOnConnect;
    EventProcessor mOnRead;
    EventProcessor mOnWrite;
    EventProcessor mOnFinish;
    EventProcessor mOnDone;


    std::list<UpstreamDataType>     m_UpstreamDataQue;
    bool                            m_ClientStreamingDone;
};


#endif  //_twowayasyncrpcjobclient_h_
