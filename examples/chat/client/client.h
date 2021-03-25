#include "msgx/asyncrpcjob.h"
#include "msgx/twowayasyncrpcjobclient.h"

#include "msgexchange.pb.h"
#include "msgexchange.grpc.pb.h"
#include <condition_variable>
#include <boost/uuid/uuid.hpp>            // uuid class
#include <boost/uuid/uuid_generators.hpp> // generators
#include <boost/uuid/uuid_io.hpp>         // streaming operators etc.

class ClientImpl final
{
    public:
        ClientImpl() {
        }

        ~ClientImpl() {
            // Always shutdown the completion queue after the server.
            m_CQ->Shutdown();
        }

        // This must be run as a thread routine.
        void ServeRpc(const std::string& address) {

            auto channel = grpc::CreateChannel(
                    address, grpc::InsecureChannelCredentials());

            std::cout << "client connected to " << address << std::endl;

            m_Stub = msgexchange::msgxservice::NewStub(channel);

            m_CQ = std::make_unique<grpc::CompletionQueue>();

            // Create Two Way Async RPC Job before processing completion queue
            createTwoWayAsyncRpcJobClient();

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
            std::function<bool(msgexchange::Msg*)> sendFunc;
            grpc::ClientContext* clientContext;
        };

        std::unordered_map<AsyncRpcJob*, RpcJobHandler> m_RpcJobHandlers;
        void SetMessageContext(
                msgexchange::msgxservice::Stub* stub,
                AsyncRpcJob* job,
                grpc::ClientContext* clientContext,
                std::function<bool(const msgexchange::Msg*)> _SendUpstreamFunc) {
            RpcJobHandler jh;
            jh.sendFunc = _SendUpstreamFunc;
            jh.clientContext = clientContext;

            m_RpcJobHandlers[job] = jh;
        }

        void DownstreamDataHandler(
                msgexchange::msgxservice::Stub* stub,
                AsyncRpcJob* job,
                const msgexchange::Msg* msg) {

            if (msg) {
                if (msg->has_data()) {
                    std::cout << "<< " << msg->data().bytes(0) << std::endl;
                }else if (msg->has_status()) {
                    //std::cout << "uuid: " << msg->uuid() << " result: " <<
                    //msg->status().result() << std::endl;
                }
            }
            else {
                m_RpcJobHandlers[job].sendFunc(nullptr);
            }
        }

        bool SendUpstream(const std::string& message) {
            if (m_RpcJobHandlers.empty()) {
                std::cout << "no rpc job handlers crated yet\n";
                return false;
            }

            auto it = m_RpcJobHandlers.begin();
            RpcJobHandler& jh = it->second;

            msgexchange::Msg msg;

            msg.set_uuid(boost::uuids::to_string(boost::uuids::random_generator()()));

            auto* data = msg.mutable_data();

            data->add_bytes(message);

            jh.sendFunc(&msg);

            return true;
        }

        void RpcJobDone(
                msgexchange::msgxservice::Stub* stub,
                AsyncRpcJob* job,
                bool rpcCancelled) {
            m_RpcJobHandlers.erase(job);
            delete job;
        }

        void createTwoWayAsyncRpcJobClient() {
            TwoWayAsyncRpcJobClientHandlers<msgexchange::msgxservice::Stub, msgexchange::Msg, msgexchange::Msg> jh;
            jh.rpcJobContextHandler = std::bind(
                    &ClientImpl::SetMessageContext,
                    this,
                    std::placeholders::_1,
                    std::placeholders::_2,
                    std::placeholders::_3,
                    std::placeholders::_4
                    );

            jh.rpcJobDoneHandler = std::bind(
            &ClientImpl::RpcJobDone,
            this,
            std::placeholders::_1,
            std::placeholders::_2,
            std::placeholders::_3
            );


            jh.mPrepareAsyncRPC = std::bind(
                    &msgexchange::msgxservice::Stub::PrepareAsyncMessage,
                    m_Stub.get(),
                    std::placeholders::_1,
                    std::placeholders::_2
                    );

            jh.ProcessDownstreamHandler = std::bind(
            &ClientImpl::DownstreamDataHandler,
            this,
            std::placeholders::_1,
            std::placeholders::_2,
            std::placeholders::_3
            );

            new TwoWayAsyncRpcJobClient<msgexchange::msgxservice::Stub, msgexchange::Msg, msgexchange::Msg>(m_Stub.get(), m_CQ.get(), jh);

            std::cout << "created twowayasyncrpcjobclient\n";
        }

    private:

        std::unique_ptr<msgexchange::msgxservice::Stub> m_Stub;
        std::unique_ptr<std::list<EventInfo>>           m_RpcEventInfoList;
        std::mutex                                      m_Mutex;
        std::unique_ptr<grpc::CompletionQueue>          m_CQ;
        std::condition_variable                         m_CV;
        bool                                            m_EventsLoaded;
};

