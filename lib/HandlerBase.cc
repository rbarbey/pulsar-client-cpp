/**
 * Licensed to the Apache Software Foundation (ASF) under one
 * or more contributor license agreements.  See the NOTICE file
 * distributed with this work for additional information
 * regarding copyright ownership.  The ASF licenses this file
 * to you under the Apache License, Version 2.0 (the
 * "License"); you may not use this file except in compliance
 * with the License.  You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing,
 * software distributed under the License is distributed on an
 * "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
 * KIND, either express or implied.  See the License for the
 * specific language governing permissions and limitations
 * under the License.
 */
#include "HandlerBase.h"

#include "ClientConnection.h"
#include "ClientImpl.h"
#include "ExecutorService.h"
#include "LogUtils.h"
#include "TimeUtils.h"

DECLARE_LOG_OBJECT()

namespace pulsar {

HandlerBase::HandlerBase(const ClientImplPtr& client, const std::string& topic, const Backoff& backoff)
    : client_(client),
      topic_(std::make_shared<std::string>(topic)),
      executor_(client->getIOExecutorProvider()->get()),
      mutex_(),
      creationTimestamp_(TimeUtils::now()),
      operationTimeut_(seconds(client->conf().getOperationTimeoutSeconds())),
      state_(NotStarted),
      backoff_(backoff),
      epoch_(0),
      timer_(executor_->createDeadlineTimer()),
      reconnectionPending_(false) {}

HandlerBase::~HandlerBase() { timer_->cancel(); }

void HandlerBase::start() {
    // guard against concurrent state changes such as closing
    State state = NotStarted;
    if (state_.compare_exchange_strong(state, Pending)) {
        grabCnx();
    }
}

ClientConnectionWeakPtr HandlerBase::getCnx() const {
    Lock lock(connectionMutex_);
    return connection_;
}

void HandlerBase::setCnx(const ClientConnectionPtr& cnx) {
    Lock lock(connectionMutex_);
    auto previousCnx = connection_.lock();
    if (previousCnx) {
        beforeConnectionChange(*previousCnx);
    }
    connection_ = cnx;
}

void HandlerBase::grabCnx() {
    if (getCnx().lock()) {
        LOG_INFO(getName() << "Ignoring reconnection request since we're already connected");
        return;
    }

    bool expectedState = false;
    if (!reconnectionPending_.compare_exchange_strong(expectedState, true)) {
        LOG_DEBUG(getName() << "Ignoring reconnection attempt since there's already a pending reconnection");
        return;
    }

    LOG_INFO(getName() << "Getting connection from pool");
    ClientImplPtr client = client_.lock();
    Future<Result, ClientConnectionWeakPtr> future = client->getConnection(*topic_);
    future.addListener(std::bind(&HandlerBase::handleNewConnection, std::placeholders::_1,
                                 std::placeholders::_2, get_weak_from_this()));
}

void HandlerBase::handleNewConnection(Result result, ClientConnectionWeakPtr connection,
                                      HandlerBaseWeakPtr weakHandler) {
    HandlerBasePtr handler = weakHandler.lock();
    if (!handler) {
        LOG_DEBUG("HandlerBase Weak reference is not valid anymore");
        return;
    }

    handler->reconnectionPending_ = false;

    if (result == ResultOk) {
        ClientConnectionPtr conn = connection.lock();
        if (conn) {
            LOG_DEBUG(handler->getName() << "Connected to broker: " << conn->cnxString());
            handler->connectionOpened(conn);
            return;
        }
        // TODO - look deeper into why the connection is null while the result is ResultOk
        LOG_INFO(handler->getName() << "ClientConnectionPtr is no longer valid");
    }
    handler->connectionFailed(result);
    scheduleReconnection(handler);
}

void HandlerBase::handleDisconnection(Result result, ClientConnectionWeakPtr connection,
                                      HandlerBaseWeakPtr weakHandler) {
    HandlerBasePtr handler = weakHandler.lock();
    if (!handler) {
        LOG_DEBUG("HandlerBase Weak reference is not valid anymore");
        return;
    }

    State state = handler->state_;

    ClientConnectionPtr currentConnection = handler->getCnx().lock();
    if (currentConnection && connection.lock().get() != currentConnection.get()) {
        LOG_WARN(handler->getName()
                 << "Ignoring connection closed since we are already attached to a newer connection");
        return;
    }

    handler->resetCnx();

    if (result == ResultRetryable) {
        scheduleReconnection(handler);
        return;
    }

    switch (state) {
        case Pending:
        case Ready:
            scheduleReconnection(handler);
            break;

        case NotStarted:
        case Closing:
        case Closed:
        case Producer_Fenced:
        case Failed:
            LOG_DEBUG(handler->getName()
                      << "Ignoring connection closed event since the handler is not used anymore");
            break;
    }
}

void HandlerBase::scheduleReconnection(HandlerBasePtr handler) {
    const auto state = handler->state_.load();

    if (state == Pending || state == Ready) {
        TimeDuration delay = handler->backoff_.next();

        LOG_INFO(handler->getName() << "Schedule reconnection in " << (delay.total_milliseconds() / 1000.0)
                                    << " s");
        handler->timer_->expires_from_now(delay);
        // passing shared_ptr here since time_ will get destroyed, so tasks will be cancelled
        // so we will not run into the case where grabCnx is invoked on out of scope handler
        handler->timer_->async_wait(std::bind(&HandlerBase::handleTimeout, std::placeholders::_1, handler));
    }
}

void HandlerBase::handleTimeout(const boost::system::error_code& ec, HandlerBasePtr handler) {
    if (ec) {
        LOG_DEBUG(handler->getName() << "Ignoring timer cancelled event, code[" << ec << "]");
        return;
    } else {
        handler->epoch_++;
        handler->grabCnx();
    }
}

Result HandlerBase::convertToTimeoutIfNecessary(Result result, ptime startTimestamp) const {
    if (result == ResultRetryable && (TimeUtils::now() - startTimestamp >= operationTimeut_)) {
        return ResultTimeout;
    } else {
        return result;
    }
}

}  // namespace pulsar
