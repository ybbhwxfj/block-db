#include "network/db_client.h"

#include <utility>

db_client::db_client(node_config conf) : conf_(std::move(conf)) {

}

bool db_client::connect() {
  ptr<tcp::socket> s(new tcp::socket(io_context_));
  tcp::resolver resolver(io_context_);
  boost::system::error_code ec;

  boost::asio::connect(
      *s, resolver.resolve(conf_.address(), std::to_string(conf_.port())), ec);
  if (ec.failed()) {
    BOOST_LOG_TRIVIAL(error) << "connect ip:" << conf_.address()
                             << " port:" << conf_.port() << " failed";
    return false;
  } else {
    cli_.reset(new client(s));
    return true;
  }
}