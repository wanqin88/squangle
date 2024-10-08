/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include "squangle/mysql_client/AsyncMysqlClient.h"
#include "squangle/mysql_client/detail/MysqlConnection.h"

namespace facebook::common::mysql_client {

class SyncConnection;

class SyncMysqlClient : public MysqlClientBase {
 public:
  SyncMysqlClient() : SyncMysqlClient(nullptr) {}
  explicit SyncMysqlClient(
      std::unique_ptr<db::SquangleLoggerBase> db_logger,
      std::unique_ptr<db::DBCounterBase> db_stats =
          std::make_unique<db::SimpleDbCounter>())
      : MysqlClientBase(
            adjustLogger(std::move(db_logger)),
            std::move(db_stats)) {}

  static std::unique_ptr<db::SquangleLoggerBase> adjustLogger(
      std::unique_ptr<db::SquangleLoggerBase> logger) {
    if (logger) {
      logger->setLoggingPrefix("cpp_sync");
    }
    return logger;
  }

  db::SquangleLoggingData makeSquangleLoggingData(
      std::shared_ptr<const ConnectionKey> connKey,
      const db::ConnectionContextBase* connContext) override {
    return db::SquangleLoggingData(std::move(connKey), connContext);
  }

  // Factory method
  std::unique_ptr<Connection> createConnection(
      std::shared_ptr<const ConnectionKey> conn_key) override;

  void drain(bool /*unused*/) {}

  bool supportsLocalFiles() override {
    return true;
  }

  uint64_t getPoolsConnectionLimit() {
    // This is used by HHVM in the async client.  We don't need it here in the
    // sync client.
    return std::numeric_limits<uint64_t>::max();
  }

  static std::shared_ptr<SyncMysqlClient> defaultClient();

 protected:
  // Private methods, primarily used by Operations and its subclasses.
  template <typename Client>
  friend class ConnectionPool;

  bool runInThread(folly::Cob&& fn, bool /*wait*/) override {
    fn();
    return true;
  }

  // These functions matter more to Async client, which keeps track of
  // existing operations.
  void activeConnectionAdded(std::shared_ptr<const ConnectionKey>) override {}
  void activeConnectionRemoved(std::shared_ptr<const ConnectionKey>) override {}
  void addOperation(std::shared_ptr<Operation>) override {}
  void deferRemoveOperation(Operation*) override {}

 private:
  MysqlHandler& getMysqlHandler() override {
    return mysql_handler_;
  }

  // Sync implementation of mysql handler interface
  class SyncMysqlHandler : public MysqlHandler {
    Status tryConnect(
        const InternalConnection& conn,
        const ConnectionOptions& opts,
        std::shared_ptr<const ConnectionKey> key,
        int flags) override;

    Status runQuery(const InternalConnection& conn, std::string_view queryStmt)
        override {
      return conn.runQuery(queryStmt);
    }
    Status nextResult(const InternalConnection& conn) override {
      return conn.nextResult();
    }
    size_t getFieldCount(const InternalConnection& conn) override {
      return conn.getFieldCount();
    }
    InternalResult::FetchRowRet fetchRow(InternalResult& result) override {
      return result.fetchRow();
    }
    std::unique_ptr<InternalResult> getResult(
        const InternalConnection& conn) override {
      return conn.getResult();
    }

    Status resetConn(const InternalConnection& conn) override {
      return conn.resetConn();
    }
    Status changeUser(
        const InternalConnection& conn,
        std::shared_ptr<const ConnectionKey> conn_key) override {
      return conn.changeUser(std::move(conn_key));
    }
  } mysql_handler_;
};

// SyncConnection is a specialization of Connection to handle inline loops.
// It has its own EventBase and Operations using it will have events and
// callbacks running with this EventBase.
class SyncConnection : public Connection {
 public:
  SyncConnection(
      MysqlClientBase& client,
      std::shared_ptr<const ConnectionKey> conn_key,
      std::unique_ptr<ConnectionHolder> conn = nullptr)
      : Connection(client, std::move(conn_key), std::move(conn)) {}

  ~SyncConnection();

  // Operations call these methods as the operation becomes unblocked, as
  // callers want to wait for completion, etc.
  void notify() override {
    // Nop
  }

  void wait() const override {
    // Nop
  }

  // Called when a new operation is being started.
  void resetActionable() override {
    // Nop
  }

  bool runInThread(folly::Cob&& fn) override {
    fn();
    return true;
  }

  std::shared_ptr<MultiQueryStreamOperation> createOperation(
      std::unique_ptr<OperationImpl::ConnectionProxy> proxy,
      MultiQuery&& multi_query) override {
    auto impl = client().createFetchOperationImpl(std::move(proxy));
    return MultiQueryStreamOperation::create(
        std::move(impl), std::move(multi_query));
  }

 protected:
  std::unique_ptr<InternalConnection> createInternalConnection() override {
    return std::make_unique<detail::SyncMysqlConnection>();
  }
};

} // namespace facebook::common::mysql_client
