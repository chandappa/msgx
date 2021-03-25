/*
*
* Copyright 2016, Google Inc.
* All rights reserved.
*/

#ifndef _twowayasyncrpcjobserver_h_
#define _twowayasyncrpcjobserver_h_

#include "msgx/asyncrpcjob.h"
#include <string>
#include <thread>

using EventProcessor = std::function<void(bool)>;

struct EventInfo
{
    // The function to be called for processing incoming event
    EventProcessor* eventProcessor;
    bool ok;
};


template<typename ServiceType, typename UpstreamDataType, typename DownstreamDataType>
struct TwoWayAsyncRpcJobServerHandlers :
    public AsyncRpcJobHandlers<ServiceType, UpstreamDataType, DownstreamDataType>
{
public:
    using ReaderWriter = grpc::ServerAsyncReaderWriter<DownstreamDataType,UpstreamDataType>;
    using QueReqHandler = std::function<void(
            ServiceType*,
            grpc::ServerContext*,
            ReaderWriter*,
            grpc::CompletionQueue*,
            grpc::ServerCompletionQueue*, void *)>;

    // Job to Application code handlers/callbacks
    // AsyncRpcJob calls this to inform the application to queue up a
    // request for enabling rpc handling.
    QueReqHandler mQueReqHandler;
};

template<typename ServiceType, typename UpstreamDataType, typename DownstreamDataType>
class TwoWayAsyncRpcJobServer : public AsyncRpcJob
{
    using _ThisRpcTypeJobHandlers = TwoWayAsyncRpcJobServerHandlers<ServiceType, UpstreamDataType, DownstreamDataType>;

    public:

    TwoWayAsyncRpcJobServer(
            ServiceType* service,
            grpc::ServerCompletionQueue* cq,
            _ThisRpcTypeJobHandlers jobHandlers) :
        m_Service(service)
        , m_CQ(cq)
        , m_Stream(&mServerContext)
        , m_Handlers(jobHandlers)
        , m_ServerStreamingDone(false)
        , m_ClientStreamingDone(false) {

            m_OnInit = std::bind(&TwoWayAsyncRpcJobServer::OnInit, this, std::placeholders::_1);
            m_OnRead = std::bind(&TwoWayAsyncRpcJobServer::OnRead, this, std::placeholders::_1);
            m_OnWrite = std::bind(&TwoWayAsyncRpcJobServer::OnWrite, this, std::placeholders::_1);
            m_OnFinish = std::bind(&TwoWayAsyncRpcJobServer::OnFinish, this, std::placeholders::_1);
            m_OnDone = std::bind(&AsyncRpcJob::OnDone, this, std::placeholders::_1);

            // set up the completion queue to inform us when gRPC is done with this rpc.
            mServerContext.AsyncNotifyWhenDone(&m_OnDone);

            //inform the application of the entities it can use to respond to the rpc
            m_SendDownstream = std::bind(&TwoWayAsyncRpcJobServer::SendDownstream, this, std::placeholders::_1);

            // finally, issue the async request needed by gRPC to start handling this rpc.
            AsyncRpcJob::OperStarted(AsyncRpcJob::OPER_TYPE_QUEUED_REQUEST);
            m_Handlers.mQueReqHandler(m_Service, &mServerContext, &m_Stream, m_CQ, m_CQ, &m_OnInit);
        }

    private:

    bool SendDownstream(const DownstreamDataType* data)
    {
        const std::lock_guard<std::mutex> lock(m_Mutex);

        if (data == nullptr && !m_ClientStreamingDone)
        {
            // Wait for client to finish the all the requests. If you want to cancel,
            // use ServerContext::TryCancel.
            GPR_ASSERT(false);
            return false;
        }

        if (data != nullptr)
        {
            // We need to make a copy of the data because we need to maintain
            // it until we get a completion notification.
            m_DownstreamDataQue.push_back(*data);

            if (!AsyncWriteInProgress())
            {
                doSend();
            }
        }
        else
        {
            m_ServerStreamingDone = true;

            // Kick the async op if our state machine is not going to be kicked from
            // the completion queue
            if (!AsyncWriteInProgress())
            {
                doFinish();
            }
        }

        return true;
    }

    void OnInit(bool ok)
    {
        m_Handlers.newConnectionHandler(m_Service, this, &mServerContext, m_SendDownstream);

        // This is used up once a client gets connected.
        // Server needs to get ready for next client i.e one rpc job handler per client.
        m_Handlers.createRpcJobHandler();

        if (AsyncRpcJob::OperFinished(AsyncRpcJob::OPER_TYPE_QUEUED_REQUEST))
        {
            if (ok)
            {
                AsyncRpcJob::OperStarted(AsyncRpcJob::OPER_TYPE_READ);
                m_Stream.Read(&m_UpstreamData, &m_OnRead);
            }
        }
    }

    void OnRead(bool ok)
    {
        if (AsyncRpcJob::OperFinished(AsyncRpcJob::OPER_TYPE_READ))
        {
            if (ok)
            {
                // inform application that a new request has come in
                m_Handlers.InvokeUpstreamDataHandler(m_Service, this, &m_UpstreamData);

                // queue up another read operation for this rpc
                AsyncRpcJob::OperStarted(AsyncRpcJob::OPER_TYPE_READ);
                m_Stream.Read(&m_UpstreamData, &m_OnRead);
            }
            else
            {
                m_ClientStreamingDone = true;
                m_Handlers.InvokeUpstreamDataHandler(m_Service, this, nullptr);
            }
        }
    }

    void OnWrite(bool ok)
    {
        const std::lock_guard<std::mutex> lock(m_Mutex);

        if (AsyncRpcJob::OperFinished(AsyncRpcJob::OPER_TYPE_WRITE))
        {
            // Get rid of the message that just finished.
            m_DownstreamDataQue.pop_front();

            if (ok)
            {
                if (!m_DownstreamDataQue.empty())
                {
                    // If we have more messages waiting to be sent, send them.
                    doSend();
                }
                else if (m_ServerStreamingDone)
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
        m_Handlers.rpcJobDoneHandler(m_Service, this, mServerContext.IsCancelled());
    }

    void doSend()
    {
        AsyncRpcJob::OperStarted(AsyncRpcJob::OPER_TYPE_WRITE);
        m_Stream.Write(m_DownstreamDataQue.front(), &m_OnWrite);
    }

    void doFinish()
    {
        AsyncRpcJob::OperStarted(AsyncRpcJob::OPER_TYPE_FINISH);
        m_Stream.Finish(grpc::Status::OK, &m_OnFinish);
    }

    private:

    ServiceType* m_Service;
    grpc::ServerCompletionQueue* m_CQ;
    typename _ThisRpcTypeJobHandlers::ReaderWriter m_Stream;
    grpc::ServerContext mServerContext;

    UpstreamDataType m_UpstreamData;

    _ThisRpcTypeJobHandlers m_Handlers;

    typename _ThisRpcTypeJobHandlers::_SendDownstreamHandler m_SendDownstream;

    EventProcessor m_OnInit;
    EventProcessor m_OnRead;
    EventProcessor m_OnWrite;
    EventProcessor m_OnFinish;
    EventProcessor m_OnDone;


    std::list<DownstreamDataType> m_DownstreamDataQue;
    bool m_ServerStreamingDone;
    bool m_ClientStreamingDone;
    std::mutex m_Mutex;
};


#endif  //_twowayasyncrpcjobserver_h_
