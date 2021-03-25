#include "client.h"

int main() {
  gpr_set_log_verbosity(GPR_LOG_SEVERITY_DEBUG);

  std::unique_ptr<ClientImpl> clientImpl = make_unique<ClientImpl>();

  auto* rpcThread = new std::thread(std::bind(
              &ClientImpl::ServeRpc,clientImpl.get(),"localhost:543210"));
  auto* eventThread = new std::thread(std::bind(
              &ClientImpl::ServeEvents,clientImpl.get()));

  std::string text;
  while (true) {
      while(std::getline(std::cin, text, '\n')) {
          if (!clientImpl->SendUpstream(text)) {
              std::cout << "Quitting." << std::endl;
              break;
          }
      }
  }

  rpcThread->join();
  eventThread->join();
}

