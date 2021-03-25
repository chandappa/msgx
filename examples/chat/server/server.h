#include "msgx/asyncrpcjob.h"
#include "msgx/twowayasyncrpcjobserver.h"

#include "msgexchange.pb.h"
#include "msgexchange.grpc.pb.h"
#include <condition_variable>

class ServerImpl final
{
    public:
        ServerImpl() {
        }

        ~ServerImpl() {
            m_Server->Shutdown();
            // Always shutdown the completion queue after the server.
            m_CQ->Shutdown();
        }

        // This must be run as a thread routine.
        void ServeRpc(const std::string& address) {
            std::string serverAddress(address);

            grpc::ServerBuilder builder;

            // Listen on the given address without any authentication mechanism.
            builder.AddListeningPort(serverAddress, grpc::InsecureServerCredentials());

            // Register "service_" as the instance through which we'll communicate with
            // clients. In this case it corresponds to an *asynchronous* service.
            builder.RegisterService(&m_AsyncService);

            // Get hold of the completion queue used for the asynchronous communication
            // with the gRPC runtime.
            m_CQ = builder.AddCompletionQueue();

            // Finally assemble the server.
            m_Server = builder.BuildAndStart();

            std::cout << "Server listening on " << serverAddress << std::endl;

            // Create Two Way Async RPC Job before processing completion queue
            createTwoWayAsyncRpcJobServer();

            // Proceed to the server's main loop.
            std::cout << "Starting to serve RPCs\n";
            while (true) {
                EventInfo eventInfo;

                // Block waiting to read the next event from the completion queue. The
                // event is uniquely identified by its tag, which in this case is the
                // memory address of a EventInfo instance.
                // The return value of Next should always be checked. This return value
                // tells us whether there is any kind of event or cq_ is shutting down.
                // TODO handle return value
                GPR_ASSERT(m_CQ->Next((void**)&eventInfo.eventProcessor, &eventInfo.ok));

                std::unique_lock<std::mutex> lock(m_Mutex);
                if (!m_RpcEventInfoList) {
                    m_RpcEventInfoList = std::make_unique<std::list<EventInfo>>();
                }
                m_RpcEventInfoList->push_back(eventInfo);
                lock.unlock();
                m_CV.notify_one();
            }
            std::cout << "Done serving RPCs\n";
        }

        // This must be run as a thread routine.
        void ServeEvents() {
            std::cout << "Starting to serve events\n";
            while (true) {
                std::unique_lock<std::mutex> lock(m_Mutex);
                m_CV.wait(lock, [&] {
                        if(m_RpcEventInfoList && !m_RpcEventInfoList->empty()) return true;
                        else return false;
                        });
                auto eiList = std::move(m_RpcEventInfoList);
                lock.unlock();

                while (!eiList->empty()) {
                    auto ei = eiList->front();
                    eiList->pop_front();
                    (*(ei.eventProcessor))(ei.ok);
                }
            }
            std::cout << "Done serving events\n";
        }

        struct RpcJobHandler {
            grpc::ServerContext* ctx;
            std::function<bool(msgexchange::Msg*)> sendUpstreamFunc;
        };

        std::unordered_map<AsyncRpcJob*, RpcJobHandler> m_RpcJobHandlers;
        void OnNewConnection(
                msgexchange::msgxservice::AsyncService* service,
                AsyncRpcJob* job,
                grpc::ServerContext* ctx,
                std::function<bool(const msgexchange::Msg*)> _sendDownstreamFunc) {

            std::cout << "new connection from: " << ctx->peer() << std::endl;

            RpcJobHandler jh;

            jh.sendUpstreamFunc = _sendDownstreamFunc;
            jh.ctx = ctx;

            m_RpcJobHandlers[job] = jh;
        }

        void UpstreamDataHandler(
                msgexchange::msgxservice::AsyncService* service,
                AsyncRpcJob* job,
                const msgexchange::Msg* msg) {

            if (msg) {
                if (msg->has_data()) {
                    std::cout << "<< " << msg->data().bytes(0) << std::endl;

                    //TODO - this is optional, acknowdgement
                    msgexchange::Msg reply;

                    reply.set_uuid(msg->uuid());

                    auto status = reply.mutable_status();
                    status->set_result(msgexchange::Status_Result_SUCCESS);

                    m_RpcJobHandlers[job].sendUpstreamFunc(&reply);

                }else if (msg->has_status()) {
                    std::cout << ": " << msg->status().result() << std::endl;
                }
            }
            else {
                m_RpcJobHandlers[job].sendUpstreamFunc(nullptr);
            }
        }

        bool SendDownstream(const std::string& message) {
            if (m_RpcJobHandlers.empty()) {
                std::cout << "no rpc job handlers crated yet\n";
                return false;
            }

            // Picking up the first one.
            // TODO - make this conditional
            auto it = m_RpcJobHandlers.begin();
            RpcJobHandler& jh = it->second;

            msgexchange::Msg msg;

            auto* data = msg.mutable_data();

            data->add_bytes(message);

            jh.sendUpstreamFunc(&msg);

            return true;
        }


        void MessageProcessingDone(
                msgexchange::msgxservice::AsyncService* service,
                AsyncRpcJob* job,
                bool rpcCancelled) {
            std::cout << "done with rpc\n";
            m_RpcJobHandlers.erase(job);
            delete job;
        }

        void createTwoWayAsyncRpcJobServer() {
            TwoWayAsyncRpcJobServerHandlers<msgexchange::msgxservice::AsyncService, msgexchange::Msg, msgexchange::Msg> jh;
            jh.newConnectionHandler = std::bind(
                    &ServerImpl::OnNewConnection,
                    this,
                    std::placeholders::_1,
                    std::placeholders::_2,
                    std::placeholders::_3,
                    std::placeholders::_4
                    );

            jh.rpcJobDoneHandler = std::bind(
            &ServerImpl::MessageProcessingDone,
            this,
            std::placeholders::_1,
            std::placeholders::_2,
            std::placeholders::_3
            );

            // This is used up once a client gets connected.
            // Server needs to get ready for next client i.e one rpc job handler per client.
            jh.createRpcJobHandler = std::bind(&ServerImpl::createTwoWayAsyncRpcJobServer, this);

            jh.mQueReqHandler = &msgexchange::msgxservice::AsyncService::RequestMessage;

            jh.InvokeUpstreamDataHandler = std::bind(
            &ServerImpl::UpstreamDataHandler,
            this,
            std::placeholders::_1,
            std::placeholders::_2,
            std::placeholders::_3
            );

            new TwoWayAsyncRpcJobServer<msgexchange::msgxservice::AsyncService, msgexchange::Msg, msgexchange::Msg>(&m_AsyncService, m_CQ.get(), jh);
        }

    private:

        std::unique_ptr<std::list<EventInfo>>           m_RpcEventInfoList;
        std::mutex                                      m_Mutex;
        std::unique_ptr<grpc::ServerCompletionQueue>    m_CQ;
        msgexchange::msgxservice::AsyncService          m_AsyncService;
        std::unique_ptr<grpc::Server>                   m_Server;
        std::condition_variable                         m_CV;
        bool                                            m_EventsLoaded;
};

