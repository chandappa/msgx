/*
*
* Copyright 2016, Google Inc.
* All rights reserved.
*/

#ifndef _asyncrpcjob_h_
#define _asyncrpcjob_h_

#include <iostream>
#include <functional>
#include <grpc++/grpc++.h>

using namespace std;

// A base class for various rpc types.
// With gRPC, it is necessary to keep track of pending async operations.
// Only 1 async operation can be pending at a time with an exception
// that both async read and write can be pending at the same time.
class AsyncRpcJob
{
    public:
        enum OperType
        {
            OPER_TYPE_INVALID,
            OPER_TYPE_QUEUED_REQUEST,
            OPER_TYPE_START_CALL,
            OPER_TYPE_READ,
            OPER_TYPE_WRITE,
            OPER_TYPE_FINISH
        };

        AsyncRpcJob()
            : mOperCounter(0)
              , mReadInProgress(false)
              , mWriteInProgress(false)
              , mDone(false) {

              }

        virtual ~AsyncRpcJob() {};

        void OperStarted(OperType operation) {
            ++mOperCounter;

            switch (operation)
            {
                case OPER_TYPE_READ:
                    mReadInProgress = true;
                    break;
                case OPER_TYPE_WRITE:
                    mWriteInProgress = true;

                default:
                    break;
            }
        }

        // returns true if the processing should keep going. false otherwise.
        bool OperFinished(OperType operation) {
            --mOperCounter;

            switch (operation)
            {
                case OPER_TYPE_READ:
                    mReadInProgress = false;
                    break;

                case OPER_TYPE_WRITE:
                    mWriteInProgress = false;

                default:
                    break;
            }

            // No pending async operations and gRPC library notified
            // as earlier that it is done with the rpc.
            // Finish the rpc.
            if (mOperCounter == 0 && mDone)
            {
                Done();
                return false;
            }

            return true;
        }

        bool OperInProgress() const
        {
            return mOperCounter != 0;
        }

        bool AsyncReadInProgress() const
        {
            return mReadInProgress;
        }

        bool AsyncWriteInProgress() const
        {
            return mWriteInProgress;
        }

        // Tag processor for the 'done' event of this rpc from gRPC library
        void OnDone(bool /*ok*/)
        {
            mDone = true;
            if (mOperCounter == 0)
                Done();
        }

        // Each different rpc type need to implement the specialization of action when
        // this rpc is done.
        virtual void Done() = 0;
    private:
        int32_t     mOperCounter;
        bool        mReadInProgress;
        bool        mWriteInProgress;

        // In case of an abrupt rpc ending (for example, client process exit), gRPC calls
        // OnDone prematurely even while an async operation is in progress
        // and would be notified later. An example sequence would be
        // 1. The client issues an rpc request.
        // 2. The server handles the rpc and calls Finish with response. At this point,
        //    ServerContext::IsCancelled is NOT true.
        // 3. The client process abruptly exits.
        // 4. The completion queue dispatches an OnDone tag followed by the OnFinish tag.
        //    If the application cleans up the state in OnDone, OnFinish invocation would
        //    result in undefined behavior.
        //    This actually feels like a pretty odd behavior of the gRPC library
        //    (it is most likely a result of our multi-threaded usage) so we account for
        //    that by keeping track of whether the OnDone was called earlier.
        //    As far as the application is considered, the rpc is only 'done' when no async
        //    Ops are pending.
        bool    mDone;
};

// The application code communicates with utility classes using these handlers.
template<typename ServiceType, typename UpstreamDataType, typename DownstreamDataType>
struct AsyncRpcJobHandlers
{
public:
    // typedefs. See the comments below.
    using _InvokeUpstreamDataHandler = std::function<void(ServiceType*, AsyncRpcJob*, const UpstreamDataType*)>;
    using _CreateRpcJobHandler = std::function<void()>;
    using _RpcJobDoneHandler = std::function<void(ServiceType*, AsyncRpcJob*, bool)>;

    using _SendDownstreamHandler = std::function<bool(const DownstreamDataType*)>;
    using _NewConnectionHandler = std::function<void(ServiceType*, AsyncRpcJob*, grpc::ServerContext*, _SendDownstreamHandler)>;

    // Job to Application code handlers/callbacks
    //
    // To inform the application of a new request to be processed.
    _InvokeUpstreamDataHandler InvokeUpstreamDataHandler;

    // To inform the application to create a new AsyncRpcJob of this type.
    _CreateRpcJobHandler createRpcJobHandler;

    // To inform the application that this job is done now.
    _RpcJobDoneHandler rpcJobDoneHandler;

    // Application code to job
    // To inform the application of the entities it can use to respond to the rpc request.
    _NewConnectionHandler newConnectionHandler;
};


// The application code communicates with utility classes using these handlers.
template<typename ServiceType, typename UpstreamDataType, typename DownstreamDataType>
struct AsyncRpcJobHandlersClient
{
public:
    // typedefs. See the comments below.
    using _ProcessDownstreamHandler = std::function<void(ServiceType*, AsyncRpcJob*, const UpstreamDataType*)>;
    using _RpcJobDoneHandler = std::function<void(ServiceType*, AsyncRpcJob*, bool)>;

    using _SendUpstreamHandler = std::function<bool(const UpstreamDataType*)>;
    using _RpcJobContextHandler = std::function<void(ServiceType*, AsyncRpcJob*, grpc::ClientContext*, _SendUpstreamHandler)>;

    // Job to Application code handlers/callbacks
    //
    // To inform the application of a new request to be processed.
    _ProcessDownstreamHandler ProcessDownstreamHandler;

    // To inform the application that this job is done now.
    _RpcJobDoneHandler rpcJobDoneHandler;

    // Application code to job
    // To inform the application of the entities it can use to respond to the rpc request.
    _RpcJobContextHandler rpcJobContextHandler;
};


#endif
