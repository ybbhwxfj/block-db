#pragma once

#include "common/block_exception.h"
#include "common/buffer.h"
#include "common/endian.h"
#include "common/message.h"
#include "proto/hello.pb.h"
#include "proto/raft.pb.h"
#include <boost/functional/hash.hpp>

using namespace boost::endian;

const static uint16_t MSG_HDR_MAGIC_NUMBER1 = 1988;
const static uint16_t MSG_HDR_MAGIC_NUMBER2 = 2021;

extern std::mutex __mutex;
extern std::atomic<uint64_t> _sequence;
extern std::unordered_map<uint64_t, std::vector<int8_t>> __map;

inline uint64_t get_sequence() { return ++_sequence; }

inline void buf_insert(uint64_t hash, std::vector<int8_t> buf) {
  std::scoped_lock l(__mutex);
  __map.insert(std::make_pair(hash, buf));
}

inline void find(uint64_t hash, std::vector<int8_t> buf) {
  std::scoped_lock l(__mutex);
  std::vector<std::vector<int8_t>> v;
  auto p = __map.find(hash);
  if (p != __map.end()) {
    if (p->second.size() == buf.size()) {
      for (size_t i = 0; i < p->second.size(); i++) {
        if (p->second[i] != buf[i]) {
          BOOST_LOG_TRIVIAL(error) << "assert false";
        }
      }
    } else {
      assert(false);
    }
    //__map.erase(p);
  } else {
    BOOST_ASSERT(false);
  }
}

class msg_hdr {
private:
  uint16_buf_t magic1_;
  uint16_buf_t magic2_;
  uint16_buf_t type_;
  uint64_buf_t length_;
  uint64_buf_t hash_;

public:
  msg_hdr() : magic1_(MSG_HDR_MAGIC_NUMBER1), magic2_(MSG_HDR_MAGIC_NUMBER2) {}

  void set_type(message_type mt) { type_ = uint16_t(mt); }

  message_type type() const { return message_type(type_.value()); }

  void set_length(uint64_t size) { length_ = size; }
  uint64_t length() const { return length_.value(); }

  void set_hash(uint64_t hash) {
    hash_ = hash;
  }
  uint64_t hash() const { return hash_.value(); }

  bool check_magic() {
    uint16_t m1 = magic1_.value();
    uint16_t m2 = magic2_.value();
    return (m1 == MSG_HDR_MAGIC_NUMBER1 || m2 == MSG_HDR_MAGIC_NUMBER2);
  }
};
typedef std::function<void(msg_hdr &)> fn_msg_hdr;

const uint32_t MSG_HDR_SIZE = sizeof(msg_hdr);

template<class PBMSG>
result<void> pb_body_to_buf(byte_buffer &buffer,
                            PBMSG &msg) {

  size_t body_size = msg.ByteSizeLong();
  if (MESSAGE_BUFFER_SIZE < body_size) {
    buffer.resize(buffer.get_wpos() + body_size);
  }

  if (buffer.get_wsize() < body_size) {
    return outcome::failure(EC::EC_INSUFFICIENT_SPACE);
  }
  BOOST_ASSERT(buffer.size() >= body_size + buffer.get_wpos());
  bool ok = msg.SerializeToArray(buffer.wbegin(),
                                 buffer.get_wsize());
  if (!ok) {
    return outcome::failure(EC::EC_INSUFFICIENT_SPACE);
  }
  buffer.set_wpos(body_size);
  return outcome::success();
}

template<class PBMSG>
result<void> pb_msg_to_buf(byte_buffer &buffer, message_type id,
                           PBMSG &msg,
                           fn_msg_hdr fn = nullptr) {

  size_t body_size = msg.ByteSizeLong();
  if (MESSAGE_BUFFER_SIZE < sizeof(msg_hdr) + body_size) {
    buffer.resize(sizeof(msg_hdr) + body_size + buffer.get_wpos());
  }

  if (buffer.get_wsize() < sizeof(msg_hdr)) {
    // insufficient space for message header
    return outcome::failure(EC::EC_INSUFFICIENT_SPACE);
  }

  if (buffer.get_wsize() < body_size + sizeof(msg_hdr)) {
    return outcome::failure(EC::EC_INSUFFICIENT_SPACE);
  }
  BOOST_ASSERT(buffer.size() >= body_size + sizeof(msg_hdr) + buffer.get_wpos());
  bool ok = msg.SerializeToArray(buffer.wbegin() + sizeof(msg_hdr),
                                 buffer.get_wsize());
  if (!ok) {
    return outcome::failure(EC::EC_INSUFFICIENT_SPACE);
  }

  // second, switch to header write position, and write message header
  uint64_t wpos = buffer.get_wpos();
  msg_hdr header;
  header.set_type(id);
  header.set_length(body_size);
  uint64_t hash =
      boost::hash_range(buffer.wbegin() + sizeof(msg_hdr),
                        buffer.wbegin() + sizeof(msg_hdr) + body_size);
  //BOOST_LOG_TRIVIAL(info) << "send message hash " << hash;
  header.set_hash(hash);
  if (fn) {
    fn(header);
  }

  result<void> write_header_result = buffer.write(&header, sizeof(header));
  if (!write_header_result) {
    buffer.set_wpos(wpos);
    return write_header_result;
  }
  // set the buffer write position

  buffer.set_wpos(wpos + sizeof(msg_hdr) + body_size);
  return outcome::success();
}

inline result<msg_hdr> buf_to_hdr(byte_buffer &buffer) {
  if (buffer.get_rsize() < sizeof(msg_hdr)) {
    return outcome::failure(EC::EC_INSUFFICIENT_SPACE);
  }
  uint32_t rpos = buffer.get_rpos();
  BOOST_ASSERT(rpos == buffer.get_rpos());
  msg_hdr header;
  result<void> read_header_result = buffer.read(&header, sizeof(header));
  if (!read_header_result) {
    buffer.set_rpos(rpos);
    return read_header_result.error();
  }
  if (!header.check_magic()) {
    buffer.set_rpos(rpos);
    return outcome::failure(EC::EC_MESSAGE_MAGIC_ERROR);
  }
  return outcome::success(header);
}

template<class PBMSG>
result<void> buf_to_pb(byte_buffer &buffer, PBMSG &msg) {
  bool ok = msg.ParseFromArray(buffer.rbegin(), buffer.get_rsize());
  if (!ok) {
    return outcome::failure(EC::EC_INSUFFICIENT_SPACE);
  }
  return outcome::success();
}

template<class PBMSG> void to_pb(byte_buffer &buffer, PBMSG &msg) {
  bool ok = msg.ParseFromArray(buffer.rbegin(), buffer.get_rsize());
  if (!ok) {
    throw block_exception(EC::EC_INSUFFICIENT_SPACE);
  }
}