#include "concurrency/calvin_context.h"

#include <utility>
#include "common/berror.h"

#ifdef DB_TYPE_CALVIN

calvin_context::calvin_context(xid_t xid,
                               node_id_t node_id,
                               node_id_t dsb_node_id,
                               uint64_t cno,
                               ptr<tx_request> req,
                               net_service *service,
                               access_mgr *access,
                               fn_calvin_context_remove fn_remove
)
    : tx(xid),
      xid_(xid),
      node_id_(node_id),
      dsb_node_id_(dsb_node_id),
      collector_id_(req->source()),
      cno_(cno),
      ops_request_(req),
      service_(service),
      access_(access),
      log_committed_(false),
      fn_remove_(std::move(fn_remove)) {
  BOOST_ASSERT(ops_request_);
  BOOST_ASSERT(collector_id_ != 0);
  BOOST_ASSERT(ops_request_->operations_size() != 0);
}

void calvin_context::lock_acquire(EC ec, oid_t oid) {
  std::scoped_lock l(mutex_);
  trace_message_ += "lk ok" + std::to_string(oid) + ";";
  auto i = callback_.find(oid);
  if (i != callback_.end()) {
    i->second(ec);
  }
}

bool calvin_context::on_operation_done(const tx_operation &op, const tuple &tp) {
  std::scoped_lock l(mutex_);
  trace_message_ += "op " + std::to_string(op.operation_id()) + ";";
  ptr<tx_operation> o(new tx_operation(op));
  *o->mutable_tuple_row()->mutable_tuple() = tp;
  op_response_.insert(std::make_pair(o->operation_id(), o));
  send_read(op);
  trace_message_ += "rd dsb;";
  return true;
}

bool calvin_context::on_operation_committed(const tx_log &) {
  trace_message_ += "op cmt;";
  std::scoped_lock l(mutex_);

  //BOOST_ASSERT(op.log_type() == TX_CMD_RM_COMMIT);
  log_committed_ = true;
  return tx_commit();
}

bool calvin_context::tx_commit() {
  BOOST_ASSERT(ops_request_->operations_size() > 0);
  if (size_t(ops_request_->operations_size()) > op_read_.size()) {
    return false;
  }

  if (!log_committed_) {
    return false;
  }
  // respond
  // tx commit
  trace_message_ = "tx cmt;";

  // release locks ...
  for (const tx_operation &op: ops_request_->operations()) {
    lock_mode lm = op_type_to_lock_mode(op.op_type());
    access_->unlock(xid_, lm, op.tuple_row().table_id(), predicate(op.tuple_row().tuple_id()));
  }

  calvin_part_commit msg;
  msg.set_source(node_id_);
  msg.set_dest(collector_id_);
  msg.set_xid(xid_);
  auto r = service_->async_send(collector_id_, CALVIN_PART_COMMIT, msg);
  if (!r) {
    BOOST_LOG_TRIVIAL(error) << "send calvin part commit failed , " << ec2string(r.error().code());
  }
  fn_remove_(xid_);
  return true;
}

void calvin_context::add_lock_acquire_callback(oid_t oid, fn_lock_callback fn) {
  std::scoped_lock l(mutex_);
  trace_message_ += "lk " + std::to_string(oid) + ";";
  callback_[oid] = std::move(fn);
}

void calvin_context::debug_tx(std::ostream &os) const {
  os << "calvin RM " << xid_
     << " committed log: " << log_committed_
     << " ops: " << ops_request_->operations_size()
     << " respond: " << op_read_.size() << std::endl;
  os << "    trace :" << trace_message_ << std::endl;
}

void calvin_context::send_read(const tx_operation &op) {
  ccb_read_request read;
  read.set_xid(xid_);
  read.set_source(node_id_);
  read.set_dest(dsb_node_id_);
  read.set_cno(cno_);
  read.set_oid(op.operation_id());
  read.set_table_id(op.tuple_row().table_id());
  read.set_tuple_id(op.tuple_row().tuple_id());

  auto r = service_->async_send(dsb_node_id_, C2D_READ_DATA_REQ, read);
  if (not r) {
    BOOST_LOG_TRIVIAL(error) << "read data request" << std::endl;
  }
}

void calvin_context::read_response(const dsb_read_response &res) {
  std::scoped_lock l(mutex_);
  oid_t oid = res.oid();
  trace_message_ += "rd dsb res " + std::to_string(oid) + ";";
  auto i = op_response_.find(oid);
  if (i != op_response_.end()) {
    op_read_.insert(oid);
    if (res.error_code() == EC::EC_OK && res.has_tuple_row()) {
      *i->second->mutable_tuple_row() = res.tuple_row();
    }
  } else {
    BOOST_ASSERT(false);
  }

  tx_commit();
}
#endif // DB_TYPE_CALVIN