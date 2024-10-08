/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <chrono>
#include <memory>

#include "squangle/mysql_client/ConnectOperation.h"
#include "squangle/mysql_client/Connection.h"
#include "squangle/mysql_client/Flags.h"
#include "squangle/mysql_client/MysqlHandler.h"

namespace facebook::common::mysql_client {

using namespace std::chrono_literals;

ConnectOperationImpl::ConnectOperationImpl(
    MysqlClientBase* mysql_client,
    std::shared_ptr<const ConnectionKey> conn_key)
    : OperationImpl(std::make_unique<OperationImpl::OwnedConnection>(
          mysql_client->createConnection(conn_key))),
      conn_key_(std::dynamic_pointer_cast<const MysqlConnectionKey>(conn_key)),
      flags_(CLIENT_MULTI_STATEMENTS),
      active_in_client_(true),
      tcp_timeout_handler_(mysql_client->getEventBase(), this) {
  DCHECK(conn_key_); // The connection key is a MySQL connection key
  mysql_client->activeConnectionAdded(conn_key_);
}

void ConnectOperationImpl::setConnectionOptions(
    const ConnectionOptions& conn_opts) {
  setTimeout(conn_opts.getTimeout());
  setDefaultQueryTimeout(conn_opts.getQueryTimeout());
  setAttributes(conn_opts.getAttributes());
  setConnectAttempts(conn_opts.getConnectAttempts());
  if (conn_opts.getDscp().has_value()) {
    setDscp(*conn_opts.getDscp());
  }
  setTotalTimeout(conn_opts.getTotalTimeout());
  setCompression(conn_opts.getCompression());
  auto provider = conn_opts.getSSLOptionsProvider();
  if (conn_opts.getConnectTcpTimeout()) {
    setTcpTimeout(*conn_opts.getConnectTcpTimeout());
  }
  if (conn_opts.getSniServerName()) {
    setSniServerName(*conn_opts.getSniServerName());
  }
  if (provider) {
    setSSLOptionsProvider(std::move(provider));
  }
  if (conn_opts.getCertValidationCallback()) {
    setCertValidationCallback(
        conn_opts.getCertValidationCallback(),
        conn_opts.getCertValidationContext(),
        conn_opts.isOpPtrAsValidationContext());
  }
}

const ConnectionOptions& ConnectOperationImpl::getConnectionOptions() const {
  return conn_options_;
}

void ConnectOperationImpl::setDefaultQueryTimeout(Duration t) {
  CHECK_THROW(
      state() == OperationState::Unstarted, db::OperationStateException);
  conn_options_.setQueryTimeout(t);
}

void ConnectOperationImpl::setSniServerName(const std::string& sni_servername) {
  CHECK_THROW(
      state() == OperationState::Unstarted, db::OperationStateException);
  conn_options_.setSniServerName(sni_servername);
}

void ConnectOperationImpl::enableResetConnBeforeClose() {
  conn_options_.enableResetConnBeforeClose();
}

void ConnectOperationImpl::enableDelayedResetConn() {
  conn_options_.enableDelayedResetConn();
}

void ConnectOperationImpl::enableChangeUser() {
  conn_options_.enableChangeUser();
}

void ConnectOperationImpl::setCertValidationCallback(
    CertValidatorCallback callback,
    const void* context,
    bool opPtrAsContext) {
  CHECK_THROW(
      state() == OperationState::Unstarted, db::OperationStateException);
  conn_options_.setCertValidationCallback(
      std::move(callback), context, opPtrAsContext);
}

void ConnectOperationImpl::setTimeout(Duration timeout) {
  conn_options_.setTimeout(timeout);
  OperationImpl::setTimeout(timeout);
}

void ConnectOperationImpl::setTcpTimeout(Duration timeout) {
  conn_options_.setConnectTcpTimeout(timeout);
}

void ConnectOperationImpl::setTotalTimeout(Duration total_timeout) {
  conn_options_.setTotalTimeout(total_timeout);
  OperationImpl::setTimeout(min(getTimeout(), total_timeout));
}
void ConnectOperationImpl::setConnectAttempts(uint32_t max_attempts) {
  CHECK_THROW(
      state() == OperationState::Unstarted, db::OperationStateException);
  conn_options_.setConnectAttempts(max_attempts);
}

void ConnectOperationImpl::setDscp(uint8_t dscp) {
  CHECK_THROW(
      state() == OperationState::Unstarted, db::OperationStateException);
  conn_options_.setDscp(dscp);
}

void ConnectOperationImpl::setKillOnQueryTimeout(bool killOnQueryTimeout) {
  CHECK_THROW(
      state() == OperationState::Unstarted, db::OperationStateException);
  killOnQueryTimeout_ = killOnQueryTimeout;
}
void ConnectOperationImpl::setSSLOptionsProviderBase(
    std::unique_ptr<SSLOptionsProviderBase> /*ssl_options_provider*/) {
  CHECK_THROW(
      state() == OperationState::Unstarted, db::OperationStateException);
  LOG(ERROR) << "Using deprecated function";
}
void ConnectOperationImpl::setSSLOptionsProvider(
    std::shared_ptr<SSLOptionsProviderBase> ssl_options_provider) {
  CHECK_THROW(
      state() == OperationState::Unstarted, db::OperationStateException);
  conn_options_.setSSLOptionsProvider(ssl_options_provider);
}

bool ConnectOperationImpl::shouldCompleteOperation(OperationResult result) {
  // Cancelled doesn't really get to this point, the Operation is forced to
  // complete by Operation, adding this check here just-in-case.
  if (attempts_made_ >= conn_options_.getConnectAttempts() ||
      result == OperationResult::Cancelled) {
    return true;
  }

  return hasOpElapsed(conn_options_.getTotalTimeout() + 1ms);
}

void ConnectOperationImpl::attemptFailed(OperationResult result) {
  ++attempts_made_;
  if (shouldCompleteOperation(result)) {
    completeOperation(result);
    return;
  }

  // We need to update duration_ here because the logging function needs it.
  setDuration();
  logConnectCompleted(result);

  tcp_timeout_handler_.cancelTimeout();

  unregisterHandler();
  cancelTimeout();
  conn().close();

  // Adjust timeout
  Duration timeout_attempt_based = conn_options_.getTimeout() + opElapsed();
  setTimeoutInternal(
      min(timeout_attempt_based, conn_options_.getTotalTimeout()));
  specializedRun();
}

void ConnectOperationImpl::attemptSucceeded(OperationResult result) {
  ++attempts_made_;
  completeOperation(result);
}

void ConnectOperationImpl::specializedRunImpl() {
  if (attempts_made_ == 0) {
    conn().initialize();
  } else {
    conn().initMysqlOnly();
  }
  removeClientReference();

  conn().setConnectAttributes(getAttributes());

  if (const auto& optCompressionLib = getCompression()) {
    conn().setCompression(*optCompressionLib);
  }

  conn_options_.withPossibleSSLOptionsProvider([&](auto& provider) {
    if (conn().setSSLOptionsProvider(provider) && connection_context_) {
      connection_context_->isSslConnection = true;
    }
  });

  // Set sni field for ssl connection
  if (const auto& optSniServerName = conn_options_.getSniServerName()) {
    conn().setSniServerName(*optSniServerName);
  }

  if (const auto& optDscp = conn_options_.getDscp()) {
    if (!conn().setDscp(*optDscp)) {
      LOG(WARNING) << fmt::format(
          "Failed to set DSCP {} for MySQL Client", *optDscp);
    }
  }

  if (conn_options_.getCertValidationCallback()) {
    conn().setCertValidatorCallback(mysqlCertValidator, &getOp());
  }

  // If the tcp timeout value is not set in conn options, use the default value
  auto timeout = std::chrono::duration_cast<Millis>(
      conn_options_.getConnectTcpTimeout().value_or(
          Duration(FLAGS_async_mysql_connect_tcp_timeout_micros)));
  // Set the connect timeout in mysql options and also on tcp_timeout_handler if
  // event base is set. Sync implmenation of MysqlClientBase may not have it
  // set. If the timeout is set to 0, skip setting any timeout
  if (timeout.count() != 0) {
    conn().setConnectTimeout(timeout);
    if (isEventBaseSet()) {
      tcp_timeout_handler_.scheduleTimeout(timeout.count());
    }
  }

  // connect is immediately "ready" to do one loop
  actionable();
}

void ConnectOperationImpl::specializedRun() {
  if (!conn().runInThread([&]() { specializedRunImpl(); })) {
    completeOperationInner(OperationResult::Failed);
  }
}

ConnectOperationImpl::~ConnectOperationImpl() {
  removeClientReference();
}

/*static*/ std::unique_ptr<ConnectOperationImpl> ConnectOperationImpl::create(
    MysqlClientBase* mysql_client,
    std::shared_ptr<const ConnectionKey> conn_key) {
  // We must do this unusual behavior (with `new`) instead of std::make_unique
  // because we don't want the constructor for ConnectOperationImpl to be
  // public. Without a public constructor there is no standard way of allowing
  // std::make_unique to call the constructor - i.e. no way to mark
  // std::make_unique as a friend.  So we have to do this weirdness.
  return std::unique_ptr<ConnectOperationImpl>(
      new ConnectOperationImpl(mysql_client, std::move(conn_key)));
}

void ConnectOperationImpl::actionable() {
  DCHECK(isInEventBaseThread());

  folly::stop_watch<Duration> sw;
  auto guard = folly::makeGuard([&]() { logThreadBlockTime(sw); });

  auto& handler = conn().client().getMysqlHandler();
  // MYSQL* mysql = conn()->mysql();
  const auto usingUnixSocket = !conn_key_->unixSocketPath().empty();

  auto status = handler.tryConnect(
      conn().getInternalConnection(), conn_options_, conn_key_, flags_);

  if (status == ERROR) {
    getOp().snapshotMysqlErrors(conn().getErrno(), conn().getErrorMessage());
    guard.dismiss();
    attemptFailed(OperationResult::Failed);
  } else {
    if ((isDoneWithTcpHandShake() || usingUnixSocket) &&
        tcp_timeout_handler_.isScheduled()) {
      // cancel tcp connect timeout
      tcp_timeout_handler_.cancelTimeout();
    }

    auto fd = conn().getSocketDescriptor();
    if (fd <= 0) {
      LOG(ERROR) << "Unexpected invalid socket descriptor on completed, "
                 << (status == DONE ? "errorless" : "pending")
                 << " connect.  fd=" << fd;
      getOp().setAsyncClientError(
          static_cast<uint16_t>(SquangleErrno::SQ_INITIALIZATION_FAILED),
          "mysql_get_socket_descriptor returned an invalid descriptor");
      guard.dismiss();
      attemptFailed(OperationResult::Failed);
    } else if (status == DONE) {
      auto socket = folly::NetworkSocket::fromFd(fd);
      changeHandlerFD(socket);
      conn().mysqlConnection()->setConnectionContext(connection_context_);
      conn().mysqlConnection()->connectionOpened();
      guard.dismiss();
      attemptSucceeded(OperationResult::Succeeded);
    } else {
      changeHandlerFD(folly::NetworkSocket::fromFd(fd));
      waitForActionable();
    }
  }
}

bool ConnectOperationImpl::isDoneWithTcpHandShake() {
  return conn().isDoneWithTcpHandShake();
}

void ConnectOperationImpl::specializedTimeoutTriggered() {
  timeoutHandler(false);
}

void ConnectOperationImpl::tcpConnectTimeoutTriggered() {
  if (!isDoneWithTcpHandShake()) {
    timeoutHandler(true);
  }
  // else  do nothing since we have made progress
}

void ConnectOperationImpl::timeoutHandler(
    bool isTcpTimeout,
    bool isPoolConnection) {
  auto deltaMs = opElapsedMs();

  auto cbDelayUs = client().callbackDelayMicrosAvg();
  bool stalled = (cbDelayUs >= kCallbackDelayStallThresholdUs);

  // Overall the message looks like this:
  //   [<errno>](Mysql Client) Connect[Pool] to <host>:<port> timed out
  //   [at stage <connect_stage>] (took Nms, timeout was Nms)
  //   [(CLIENT_OVERLOADED: cb delay Nms, N active conns)] [TcpTimeout:N]
  std::vector<std::string> parts;
  parts.push_back(fmt::format(
      "[{}]({})Connect{} to {}:{} timed out",
      static_cast<uint16_t>(
          stalled ? SquangleErrno::SQ_ERRNO_CONN_TIMEOUT_LOOP_STALLED
                  : SquangleErrno::SQ_ERRNO_CONN_TIMEOUT),
      kErrorPrefix,
      isPoolConnection ? "Pool" : "",
      conn_key_->host(),
      conn_key_->port()));
  if (!isPoolConnection) {
    parts.push_back(fmt::format("at stage {}", conn().getConnectStageName()));
  }

  parts.push_back(timeoutMessage(deltaMs));
  if (stalled) {
    parts.push_back(threadOverloadMessage(cbDelayUs));
  }
  parts.push_back(fmt::format("(TcpTimeout:{})", (isTcpTimeout ? 1 : 0)));

  getOp().setAsyncClientError(CR_SERVER_LOST, folly::join(" ", parts));
  attemptFailed(OperationResult::TimedOut);
}

void ConnectOperationImpl::logConnectCompleted(OperationResult result) {
  // If the connection wasn't initialized, it's because the operation
  // was cancelled before anything started, so we don't do the logs
  if (!conn().hasInitialized()) {
    return;
  }
  auto* context = connection_context_.get();
  if (result == OperationResult::Succeeded) {
    withOptionalConnectionContext(
        [&](auto& context) { context.sslVersion = conn().getTlsVersion(); });
    client().logConnectionSuccess(
        db::CommonLoggingData(
            getOp().getOperationType(),
            elapsed(),
            getTimeout(),
            getMaxThreadBlockTime(),
            getTotalThreadBlockTime()),
        conn().getKey(),
        context);
  } else {
    db::FailureReason reason = db::FailureReason::DATABASE_ERROR;
    if (result == OperationResult::TimedOut) {
      reason = db::FailureReason::TIMEOUT;
    } else if (result == OperationResult::Cancelled) {
      reason = db::FailureReason::CANCELLED;
    }
    client().logConnectionFailure(
        db::CommonLoggingData(
            getOp().getOperationType(),
            elapsed(),
            getTimeout(),
            getMaxThreadBlockTime(),
            getTotalThreadBlockTime()),
        reason,
        conn().getKey(),
        mysql_errno(),
        mysql_error(),
        context);
  }
}

void ConnectOperationImpl::maybeStoreSSLSession() {
  // If connection was successful
  if (result() != OperationResult::Succeeded || !conn().hasInitialized()) {
    return;
  }

  // if there is an ssl provider set
  conn_options_.withPossibleSSLOptionsProvider([&](auto& provider) {
    if (conn().storeSession(provider)) {
      if (connection_context_) {
        connection_context_->sslSessionReused = true;
      }
      client().stats()->incrReusedSSLSessions();
    }
  });
}

void ConnectOperationImpl::specializedCompleteOperation() {
  // Pass the callbacks to the Connection now that we are done with them
  conn().setCallbacks(std::move(callbacks_));

  // Operations that don't directly initiate a new TLS conneciton
  // shouldn't update the TLS session because it can propagate the
  // session object from a connection created usisn one client cert
  // to an SSL provider initialized with a different cert.
  if (getOp().getOperationType() == db::OperationType::Connect) {
    maybeStoreSSLSession();
  }

  // Can only log this on successful connections because unsuccessful
  // ones call mysql_close_free inside libmysql
  if (result() == OperationResult::Succeeded && conn().ok() &&
      connection_context_) {
    connection_context_->endpointVersion = conn().serverInfo();
  }

  // Cancel tcp timeout
  tcp_timeout_handler_.cancelTimeout();

  logConnectCompleted(result());

  // If connection_initialized_ is false the only way to complete the
  // operation is by cancellation
  DCHECK(conn().hasInitialized() || result() == OperationResult::Cancelled);

  conn().setConnectionOptions(conn_options_);
  conn().setKillOnQueryTimeout(getKillOnQueryTimeout());
  conn().setConnectionContext(connection_context_);

  conn().notify();

  if (connect_callback_) {
    connect_callback_(op());
    // Release callback since no other callbacks will be made
    connect_callback_ = nullptr;
  }
  // In case this operation didn't even get the chance to run, we still need
  // to remove the reference it added to the async client
  removeClientReference();
}

ConnectOperation& ConnectOperationImpl::op() const {
  DCHECK(op_ && dynamic_cast<ConnectOperation*>(op_) != nullptr);
  return *(ConnectOperation*)op_;
}

void ConnectOperation::mustSucceed() {
  run().wait();
  if (!ok()) {
    throw db::RequiredOperationFailedException(
        "Connect failed: " + mysql_error());
  }
}

void ConnectOperationImpl::removeClientReference() {
  if (active_in_client_) {
    // It's safe to call the client since we still have a ref counting
    // it won't die before it goes to 0
    active_in_client_ = false;
    client().activeConnectionRemoved(conn_key_);
  }
}

int ConnectOperationImpl::mysqlCertValidator(
    X509* server_cert,
    const void* context,
    const char** errptr) {
  ConnectOperation* self =
      reinterpret_cast<ConnectOperation*>(const_cast<void*>(context));
  CHECK_NOTNULL(self);

  // Hold a shared pointer to the Operation object while running the callback
  auto weak_self = self->weak_from_this();
  auto guard = weak_self.lock();
  if (guard == nullptr) {
    LOG(ERROR) << "ConnectOperation object " << self
               << " is already deallocated";
    return 0;
  }

  const CertValidatorCallback callback =
      self->getConnectionOptions().getCertValidationCallback();
  CHECK(callback);
  const void* callbackContext =
      self->getConnectionOptions().isOpPtrAsValidationContext()
      ? self
      : self->getConnectionOptions().getCertValidationContext();
  folly::StringPiece errorMessage;

  // "libmysql" expects this callback to return "0" if the cert validation was
  // successful, and return "1" if validation failed.
  int result = callback(server_cert, callbackContext, errorMessage) ? 0 : 1;
  if (!errorMessage.empty()) {
    *errptr = errorMessage.data();
  }
  return result;
}

/*static*/
std::shared_ptr<ConnectOperation> ConnectOperation::create(
    std::unique_ptr<ConnectOperationImpl> impl) {
  // We must do this unusual behavior (with `new`) instead of std::make_shared
  // because we don't want the constructor for ConnectOperation to be public.
  // Without a public constructor there is no standard way of allowing
  // std::make_shared to call the constructor - i.e. no way to mark
  // std::make_shared as a friend.  So we have to do this weirdness.
  return std::shared_ptr<ConnectOperation>(
      new ConnectOperation(std::move(impl)));
}

} // namespace facebook::common::mysql_client
