#include "server.h"

int main(int argc, char** argv) {

  gpr_set_log_verbosity(GPR_LOG_SEVERITY_DEBUG);

  auto serverImpl = make_unique<ServerImpl>();

  auto* rpcThread = new std::thread(std::bind(
              &ServerImpl::ServeRpc,serverImpl.get(),"0.0.0.0:543210"));
  auto* eventThread = new std::thread(std::bind(
              &ServerImpl::ServeEvents,serverImpl.get()));

  std::string text;
  while (true) {
      while(std::getline(std::cin, text, '\n')) {
          if (!serverImpl->SendDownstream(text)) {
              std::cout << "Quitting." << std::endl;
              break;
          }
      }
  }

  rpcThread->join();
  eventThread->join();

  return 0;
}
