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
#include "ProducerImpl.h"

#include <pulsar/MessageIdBuilder.h>

#include <boost/date_time/posix_time/posix_time.hpp>

#include "BatchMessageContainer.h"
#include "BatchMessageKeyBasedContainer.h"
#include "ClientConnection.h"
#include "ClientImpl.h"
#include "Commands.h"
#include "CompressionCodec.h"
#include "ExecutorService.h"
#include "LogUtils.h"
#include "MemoryLimitController.h"
#include "MessageCrypto.h"
#include "MessageImpl.h"
#include "OpSendMsg.h"
#include "ProducerConfigurationImpl.h"
#include "PulsarApi.pb.h"
#include "Semaphore.h"
#include "TimeUtils.h"
#include "TopicName.h"
#include "stats/ProducerStatsDisabled.h"
#include "stats/ProducerStatsImpl.h"

namespace pulsar {
DECLARE_LOG_OBJECT()

ProducerImpl::ProducerImpl(ClientImplPtr client, const TopicName& topicName,
                           const ProducerConfiguration& conf, const ProducerInterceptorsPtr& interceptors,
                           int32_t partition)
    : HandlerBase(client, (partition < 0) ? topicName.toString() : topicName.getTopicPartitionName(partition),
                  Backoff(milliseconds(client->getClientConfig().getInitialBackoffIntervalMs()),
                          milliseconds(client->getClientConfig().getMaxBackoffIntervalMs()),
                          milliseconds(std::max(100, conf.getSendTimeout() - 100)))),
      conf_(conf),
      semaphore_(),
      partition_(partition),
      producerName_(conf_.getProducerName()),
      userProvidedProducerName_(false),
      producerStr_("[" + *topic_ + ", " + producerName_ + "] "),
      producerId_(client->newProducerId()),
      msgSequenceGenerator_(0),
      batchTimer_(executor_->createDeadlineTimer()),
      sendTimer_(executor_->createDeadlineTimer()),
      dataKeyRefreshTask_(*executor_, 4 * 60 * 60 * 1000),
      memoryLimitController_(client->getMemoryLimitController()),
      chunkingEnabled_(conf_.isChunkingEnabled() && topicName.isPersistent() && !conf_.getBatchingEnabled()),
      interceptors_(interceptors) {
    LOG_DEBUG("ProducerName - " << producerName_ << " Created producer on topic " << topic_
                                << " id: " << producerId_);

    int64_t initialSequenceId = conf.getInitialSequenceId();
    lastSequenceIdPublished_ = initialSequenceId;
    msgSequenceGenerator_ = initialSequenceId + 1;

    if (!producerName_.empty()) {
        userProvidedProducerName_ = true;
    }

    if (conf.getMaxPendingMessages() > 0) {
        semaphore_ = std::unique_ptr<Semaphore>(new Semaphore(conf_.getMaxPendingMessages()));
    }

    unsigned int statsIntervalInSeconds = client->getClientConfig().getStatsIntervalInSeconds();
    if (statsIntervalInSeconds) {
        producerStatsBasePtr_ =
            std::make_shared<ProducerStatsImpl>(producerStr_, executor_, statsIntervalInSeconds);
    } else {
        producerStatsBasePtr_ = std::make_shared<ProducerStatsDisabled>();
    }
    producerStatsBasePtr_->start();

    if (conf_.isEncryptionEnabled()) {
        std::ostringstream logCtxStream;
        logCtxStream << "[" << topic_ << ", " << producerName_ << ", " << producerId_ << "]";
        std::string logCtx = logCtxStream.str();
        msgCrypto_ = std::make_shared<MessageCrypto>(logCtx, true);
        msgCrypto_->addPublicKeyCipher(conf_.getEncryptionKeys(), conf_.getCryptoKeyReader());
    }

    if (conf_.getBatchingEnabled()) {
        switch (conf_.getBatchingType()) {
            case ProducerConfiguration::DefaultBatching:
                batchMessageContainer_.reset(new BatchMessageContainer(*this));
                break;
            case ProducerConfiguration::KeyBasedBatching:
                batchMessageContainer_.reset(new BatchMessageKeyBasedContainer(*this));
                break;
            default:  // never reached here
                LOG_ERROR("Unknown batching type: " << conf_.getBatchingType());
                return;
        }
    }
}

ProducerImpl::~ProducerImpl() {
    LOG_DEBUG(getName() << "~ProducerImpl");
    shutdown();
    printStats();
    if (state_ == Ready || state_ == Pending) {
        LOG_WARN(getName() << "Destroyed producer which was not properly closed");
    }
}

const std::string& ProducerImpl::getTopic() const { return *topic_; }

const std::string& ProducerImpl::getProducerName() const { return producerName_; }

int64_t ProducerImpl::getLastSequenceId() const { return lastSequenceIdPublished_; }

const std::string& ProducerImpl::getSchemaVersion() const { return schemaVersion_; }

void ProducerImpl::beforeConnectionChange(ClientConnection& connection) {
    connection.removeProducer(producerId_);
}

void ProducerImpl::connectionOpened(const ClientConnectionPtr& cnx) {
    if (state_ == Closed) {
        LOG_DEBUG(getName() << "connectionOpened : Producer is already closed");
        return;
    }

    ClientImplPtr client = client_.lock();
    int requestId = client->newRequestId();

    SharedBuffer cmd = Commands::newProducer(*topic_, producerId_, producerName_, requestId,
                                             conf_.getProperties(), conf_.getSchema(), epoch_,
                                             userProvidedProducerName_, conf_.isEncryptionEnabled(),
                                             static_cast<proto::ProducerAccessMode>(conf_.getAccessMode()),
                                             topicEpoch, conf_.impl_->initialSubscriptionName);
    cnx->sendRequestWithId(cmd, requestId)
        .addListener(std::bind(&ProducerImpl::handleCreateProducer, shared_from_this(), cnx,
                               std::placeholders::_1, std::placeholders::_2));
}

void ProducerImpl::connectionFailed(Result result) {
    // Keep a reference to ensure object is kept alive
    ProducerImplPtr ptr = shared_from_this();

    if (conf_.getLazyStartPartitionedProducers() && conf_.getAccessMode() == ProducerConfiguration::Shared) {
        // if producers are lazy, then they should always try to restart
        // so don't change the state and allow reconnections
        return;
    } else if (producerCreatedPromise_.setFailed(result)) {
        state_ = Failed;
    }
}

void ProducerImpl::handleCreateProducer(const ClientConnectionPtr& cnx, Result result,
                                        const ResponseData& responseData) {
    Lock lock(mutex_);

    LOG_DEBUG(getName() << "ProducerImpl::handleCreateProducer res: " << strResult(result));

    // make sure we're still in the Pending/Ready state, closeAsync could have been invoked
    // while waiting for this response if using lazy producers
    const auto state = state_.load();
    if (state != Ready && state != Pending) {
        LOG_DEBUG("Producer created response received but producer already closed");
        failPendingMessages(ResultAlreadyClosed, false);
        if (result == ResultOk || result == ResultTimeout) {
            auto client = client_.lock();
            if (client) {
                int requestId = client->newRequestId();
                cnx->sendRequestWithId(Commands::newCloseProducer(producerId_, requestId), requestId);
            }
        }
        if (!producerCreatedPromise_.isComplete()) {
            lock.unlock();
            producerCreatedPromise_.setFailed(ResultAlreadyClosed);
        }
        return;
    }

    if (result == ResultOk) {
        // We are now reconnected to broker and clear to send messages. Re-send all pending messages and
        // set the cnx pointer so that new messages will be sent immediately
        LOG_INFO(getName() << "Created producer on broker " << cnx->cnxString());

        cnx->registerProducer(producerId_, shared_from_this());
        producerName_ = responseData.producerName;
        schemaVersion_ = responseData.schemaVersion;
        producerStr_ = "[" + *topic_ + ", " + producerName_ + "] ";
        topicEpoch = responseData.topicEpoch;

        if (lastSequenceIdPublished_ == -1 && conf_.getInitialSequenceId() == -1) {
            lastSequenceIdPublished_ = responseData.lastSequenceId;
            msgSequenceGenerator_ = lastSequenceIdPublished_ + 1;
        }
        resendMessages(cnx);
        setCnx(cnx);
        state_ = Ready;
        backoff_.reset();

        if (conf_.isEncryptionEnabled()) {
            auto weakSelf = weak_from_this();
            dataKeyRefreshTask_.setCallback([this, weakSelf](const PeriodicTask::ErrorCode& ec) {
                auto self = weakSelf.lock();
                if (!self) {
                    return;
                }
                if (ec) {
                    LOG_ERROR("DataKeyRefresh timer failed: " << ec.message());
                    return;
                }
                msgCrypto_->addPublicKeyCipher(conf_.getEncryptionKeys(), conf_.getCryptoKeyReader());
            });
        }

        // if the producer is lazy the send timeout timer is already running
        if (!(conf_.getLazyStartPartitionedProducers() &&
              conf_.getAccessMode() == ProducerConfiguration::Shared)) {
            startSendTimeoutTimer();
        }

        lock.unlock();
        producerCreatedPromise_.setValue(shared_from_this());

    } else {
        // Producer creation failed
        if (result == ResultTimeout) {
            // Creating the producer has timed out. We need to ensure the broker closes the producer
            // in case it was indeed created, otherwise it might prevent new create producer operation,
            // since we are not closing the connection
            auto client = client_.lock();
            if (client) {
                int requestId = client->newRequestId();
                cnx->sendRequestWithId(Commands::newCloseProducer(producerId_, requestId), requestId);
            }
        }

        if (result == ResultProducerFenced) {
            state_ = Producer_Fenced;
            failPendingMessages(result, false);
            auto client = client_.lock();
            if (client) {
                client->cleanupProducer(this);
            }
            lock.unlock();
            producerCreatedPromise_.setFailed(result);
        } else if (producerCreatedPromise_.isComplete()) {
            if (result == ResultProducerBlockedQuotaExceededException) {
                LOG_WARN(getName() << "Backlog is exceeded on topic. Sending exception to producer");
                failPendingMessages(ResultProducerBlockedQuotaExceededException, false);
            } else if (result == ResultProducerBlockedQuotaExceededError) {
                LOG_WARN(getName() << "Producer is blocked on creation because backlog is exceeded on topic");
            }

            // Producer had already been initially created, we need to retry connecting in any case
            LOG_WARN(getName() << "Failed to reconnect producer: " << strResult(result));
            scheduleReconnection(shared_from_this());
        } else {
            // Producer was not yet created, retry to connect to broker if it's possible
            result = convertToTimeoutIfNecessary(result, creationTimestamp_);
            if (result == ResultRetryable) {
                LOG_WARN(getName() << "Temporary error in creating producer: " << strResult(result));
                scheduleReconnection(shared_from_this());
            } else {
                LOG_ERROR(getName() << "Failed to create producer: " << strResult(result));
                failPendingMessages(result, false);
                state_ = Failed;
                lock.unlock();
                producerCreatedPromise_.setFailed(result);
            }
        }
    }
}

auto ProducerImpl::getPendingCallbacksWhenFailed() -> decltype(pendingMessagesQueue_) {
    decltype(pendingMessagesQueue_) pendingMessages;
    LOG_DEBUG(getName() << "# messages in pending queue : " << pendingMessagesQueue_.size());

    pendingMessages.swap(pendingMessagesQueue_);
    for (const auto& op : pendingMessages) {
        releaseSemaphoreForSendOp(*op);
    }

    if (!batchMessageContainer_ || batchMessageContainer_->isEmpty()) {
        return pendingMessages;
    }

    auto handleOp = [this, &pendingMessages](std::unique_ptr<OpSendMsg>&& op) {
        releaseSemaphoreForSendOp(*op);
        if (op->result == ResultOk) {
            pendingMessages.emplace_back(std::move(op));
        }
    };

    if (batchMessageContainer_->hasMultiOpSendMsgs()) {
        auto opSendMsgs = batchMessageContainer_->createOpSendMsgs();
        for (auto&& op : opSendMsgs) {
            handleOp(std::move(op));
        }
    } else {
        handleOp(batchMessageContainer_->createOpSendMsg());
    }
    return pendingMessages;
}

auto ProducerImpl::getPendingCallbacksWhenFailedWithLock() -> decltype(pendingMessagesQueue_) {
    Lock lock(mutex_);
    return getPendingCallbacksWhenFailed();
}

void ProducerImpl::failPendingMessages(Result result, bool withLock) {
    auto opSendMsgs = withLock ? getPendingCallbacksWhenFailedWithLock() : getPendingCallbacksWhenFailed();
    for (const auto& op : opSendMsgs) {
        op->complete(result, {});
    }
}

void ProducerImpl::resendMessages(ClientConnectionPtr cnx) {
    if (pendingMessagesQueue_.empty()) {
        return;
    }

    LOG_DEBUG(getName() << "Re-Sending " << pendingMessagesQueue_.size() << " messages to server");

    for (const auto& op : pendingMessagesQueue_) {
        LOG_DEBUG(getName() << "Re-Sending " << op->sendArgs->sequenceId);
        cnx->sendMessage(op->sendArgs);
    }
}

void ProducerImpl::setMessageMetadata(const Message& msg, const uint64_t& sequenceId,
                                      const uint32_t& uncompressedSize) {
    // Call this function after acquiring the mutex_
    proto::MessageMetadata& msgMetadata = msg.impl_->metadata;
    msgMetadata.set_producer_name(producerName_);
    msgMetadata.set_publish_time(TimeUtils::currentTimeMillis());
    msgMetadata.set_sequence_id(sequenceId);
    if (conf_.getCompressionType() != CompressionNone) {
        msgMetadata.set_compression(static_cast<proto::CompressionType>(conf_.getCompressionType()));
        msgMetadata.set_uncompressed_size(uncompressedSize);
    }
    if (!this->getSchemaVersion().empty()) {
        msgMetadata.set_schema_version(this->getSchemaVersion());
    }
}

void ProducerImpl::flushAsync(FlushCallback callback) {
    if (state_ != Ready) {
        callback(ResultAlreadyClosed);
        return;
    }
    if (batchMessageContainer_) {
        Lock lock(mutex_);
        auto failures = batchMessageAndSend(callback);
        if (!pendingMessagesQueue_.empty()) {
            auto& opSendMsg = pendingMessagesQueue_.back();
            lock.unlock();
            failures.complete();
            opSendMsg->addTrackerCallback(callback);
        } else {
            lock.unlock();
            failures.complete();
            callback(ResultOk);
        }
    } else {
        Lock lock(mutex_);
        if (!pendingMessagesQueue_.empty()) {
            auto& opSendMsg = pendingMessagesQueue_.back();
            lock.unlock();
            opSendMsg->addTrackerCallback(callback);
        } else {
            lock.unlock();
            callback(ResultOk);
        }
    }
}

void ProducerImpl::triggerFlush() {
    if (batchMessageContainer_) {
        if (state_ == Ready) {
            Lock lock(mutex_);
            auto failures = batchMessageAndSend();
            lock.unlock();
            failures.complete();
        }
    }
}

bool ProducerImpl::isValidProducerState(const SendCallback& callback) const {
    const auto state = state_.load();
    switch (state) {
        case HandlerBase::Ready:
            // OK
        case HandlerBase::Pending:
            // We are OK to queue the messages on the client, it will be sent to the broker once we get the
            // connection
            return true;
        case HandlerBase::Closing:
        case HandlerBase::Closed:
            callback(ResultAlreadyClosed, {});
            return false;
        case HandlerBase::Producer_Fenced:
            callback(ResultProducerFenced, {});
            return false;
        case HandlerBase::NotStarted:
        case HandlerBase::Failed:
        default:
            callback(ResultNotConnected, {});
            return false;
    }
}

bool ProducerImpl::canAddToBatch(const Message& msg) const {
    // If a message has a delayed delivery time, we'll always send it individually
    return batchMessageContainer_.get() && !msg.impl_->metadata.has_deliver_at_time();
}

static SharedBuffer applyCompression(const SharedBuffer& uncompressedPayload,
                                     CompressionType compressionType) {
    return CompressionCodecProvider::getCodec(compressionType).encode(uncompressedPayload);
}

void ProducerImpl::sendAsync(const Message& msg, SendCallback callback) {
    producerStatsBasePtr_->messageSent(msg);

    Producer producer = Producer(shared_from_this());
    auto interceptorMessage = interceptors_->beforeSend(producer, msg);

    const auto now = boost::posix_time::microsec_clock::universal_time();
    auto self = shared_from_this();
    sendAsyncWithStatsUpdate(interceptorMessage, [this, self, now, callback, producer, interceptorMessage](
                                                     Result result, const MessageId& messageId) {
        producerStatsBasePtr_->messageReceived(result, now);

        interceptors_->onSendAcknowledgement(producer, result, interceptorMessage, messageId);

        if (callback) {
            callback(result, messageId);
        }
    });
}

void ProducerImpl::sendAsyncWithStatsUpdate(const Message& msg, SendCallback&& callback) {
    if (!isValidProducerState(callback)) {
        return;
    }

    // Convert the payload before sending the message.
    msg.impl_->convertKeyValueToPayload(conf_.getSchema());
    const auto& uncompressedPayload = msg.impl_->payload;
    const uint32_t uncompressedSize = uncompressedPayload.readableBytes();
    const auto result = canEnqueueRequest(uncompressedSize);
    if (result != ResultOk) {
        // If queue is full sending the batch immediately, no point waiting till batchMessagetimeout
        if (batchMessageContainer_) {
            LOG_DEBUG(getName() << " - sending batch message immediately");
            Lock lock(mutex_);
            auto failures = batchMessageAndSend();
            lock.unlock();
            failures.complete();
        }

        callback(result, {});
        return;
    }

    // We have already reserved a spot, so if we need to early return for failed result, we should release the
    // semaphore and memory first.
    const auto handleFailedResult = [this, uncompressedSize, callback](Result result) {
        releaseSemaphore(uncompressedSize);  // it releases the memory as well
        callback(result, {});
    };

    auto& msgMetadata = msg.impl_->metadata;
    const bool compressed = !canAddToBatch(msg);
    const auto payload =
        compressed ? applyCompression(uncompressedPayload, conf_.getCompressionType()) : uncompressedPayload;
    const auto compressedSize = static_cast<uint32_t>(payload.readableBytes());
    const auto maxMessageSize = static_cast<uint32_t>(ClientConnection::getMaxMessageSize());

    if (!msgMetadata.has_replicated_from() && msgMetadata.has_producer_name()) {
        handleFailedResult(ResultInvalidMessage);
        return;
    }

    Lock lock(mutex_);
    uint64_t sequenceId;
    if (!msgMetadata.has_sequence_id()) {
        sequenceId = msgSequenceGenerator_++;
    } else {
        sequenceId = msgMetadata.sequence_id();
    }
    setMessageMetadata(msg, sequenceId, uncompressedSize);

    auto payloadChunkSize = maxMessageSize;
    int totalChunks;
    if (!compressed || !chunkingEnabled_) {
        totalChunks = 1;
    } else {
        const auto metadataSize = static_cast<uint32_t>(msgMetadata.ByteSizeLong());
        if (metadataSize >= maxMessageSize) {
            LOG_WARN(getName() << " - metadata size " << metadataSize << " cannot exceed " << maxMessageSize
                               << " bytes");
            handleFailedResult(ResultMessageTooBig);
            return;
        }
        payloadChunkSize = maxMessageSize - metadataSize;
        totalChunks = getNumOfChunks(compressedSize, payloadChunkSize);
    }

    // Each chunk should be sent individually, so try to acquire extra permits for chunks.
    for (int i = 0; i < (totalChunks - 1); i++) {
        const auto result = canEnqueueRequest(0);  // size is 0 because the memory has already reserved
        if (result != ResultOk) {
            handleFailedResult(result);
            return;
        }
    }

    if (canAddToBatch(msg)) {
        // Batching is enabled and the message is not delayed
        if (!batchMessageContainer_->hasEnoughSpace(msg)) {
            batchMessageAndSend().complete();
        }
        bool isFirstMessage = batchMessageContainer_->isFirstMessageToAdd(msg);
        bool isFull = batchMessageContainer_->add(msg, callback);
        if (isFirstMessage) {
            batchTimer_->expires_from_now(
                boost::posix_time::milliseconds(conf_.getBatchingMaxPublishDelayMs()));
            auto weakSelf = weak_from_this();
            batchTimer_->async_wait([this, weakSelf](const boost::system::error_code& ec) {
                auto self = weakSelf.lock();
                if (!self) {
                    return;
                }
                if (ec) {
                    LOG_DEBUG(getName() << " Ignoring timer cancelled event, code[" << ec << "]");
                    return;
                }
                LOG_DEBUG(getName() << " - Batch Message Timer expired");

                // ignore if the producer is already closing/closed
                const auto state = state_.load();
                if (state == Pending || state == Ready) {
                    Lock lock(mutex_);
                    auto failures = batchMessageAndSend();
                    lock.unlock();
                    failures.complete();
                }
            });
        }

        if (isFull) {
            auto failures = batchMessageAndSend();
            lock.unlock();
            failures.complete();
        }
    } else {
        const bool sendChunks = (totalChunks > 1);
        if (sendChunks) {
            msgMetadata.set_uuid(producerName_ + "-" + std::to_string(sequenceId));
            msgMetadata.set_num_chunks_from_msg(totalChunks);
            msgMetadata.set_total_chunk_msg_size(compressedSize);
        }

        auto chunkMessageId = totalChunks > 1 ? std::make_shared<ChunkMessageIdImpl>() : nullptr;

        int beginIndex = 0;
        for (int chunkId = 0; chunkId < totalChunks; chunkId++) {
            if (sendChunks) {
                msgMetadata.set_chunk_id(chunkId);
            }
            const uint32_t endIndex = std::min(compressedSize, beginIndex + payloadChunkSize);
            auto chunkedPayload = payload.slice(beginIndex, endIndex - beginIndex);
            beginIndex = endIndex;

            SharedBuffer encryptedPayload;
            if (!encryptMessage(msgMetadata, chunkedPayload, encryptedPayload)) {
                handleFailedResult(ResultCryptoError);
                return;
            }

            auto op = OpSendMsg::create(msgMetadata, 1, uncompressedSize, conf_.getSendTimeout(),
                                        (chunkId == totalChunks - 1) ? callback : nullptr, chunkMessageId,
                                        producerId_, encryptedPayload);

            if (!chunkingEnabled_) {
                const uint32_t msgMetadataSize = op->sendArgs->metadata.ByteSizeLong();
                const uint32_t payloadSize = op->sendArgs->payload.readableBytes();
                const uint32_t msgHeadersAndPayloadSize = msgMetadataSize + payloadSize;
                if (msgHeadersAndPayloadSize > maxMessageSize) {
                    lock.unlock();
                    releaseSemaphoreForSendOp(*op);
                    LOG_WARN(getName()
                             << " - compressed Message size " << msgHeadersAndPayloadSize << " cannot exceed "
                             << maxMessageSize << " bytes unless chunking is enabled");
                    handleFailedResult(ResultMessageTooBig);
                    return;
                }
            }

            sendMessage(std::move(op));
        }
    }
}

int ProducerImpl::getNumOfChunks(uint32_t size, uint32_t maxMessageSize) {
    if (size >= maxMessageSize && maxMessageSize != 0) {
        return size / maxMessageSize + ((size % maxMessageSize == 0) ? 0 : 1);
    }
    return 1;
}

Result ProducerImpl::canEnqueueRequest(uint32_t payloadSize) {
    if (conf_.getBlockIfQueueFull()) {
        if (semaphore_ && !semaphore_->acquire()) {
            return ResultInterrupted;
        }
        if (!memoryLimitController_.reserveMemory(payloadSize)) {
            return ResultInterrupted;
        }
        return ResultOk;
    } else {
        if (semaphore_ && !semaphore_->tryAcquire()) {
            return ResultProducerQueueIsFull;
        }
        if (!memoryLimitController_.tryReserveMemory(payloadSize)) {
            if (semaphore_) {
                semaphore_->release(1);
            }

            return ResultMemoryBufferIsFull;
        }

        return ResultOk;
    }
}

void ProducerImpl::releaseSemaphore(uint32_t payloadSize) {
    if (semaphore_) {
        semaphore_->release();
    }

    memoryLimitController_.releaseMemory(payloadSize);
}

void ProducerImpl::releaseSemaphoreForSendOp(const OpSendMsg& op) {
    if (semaphore_) {
        semaphore_->release(op.messagesCount);
    }

    memoryLimitController_.releaseMemory(op.messagesSize);
}

// It must be called while `mutex_` is acquired
PendingFailures ProducerImpl::batchMessageAndSend(const FlushCallback& flushCallback) {
    PendingFailures failures;
    LOG_DEBUG("batchMessageAndSend " << *batchMessageContainer_);
    batchTimer_->cancel();
    if (batchMessageContainer_->isEmpty()) {
        return failures;
    }

    auto handleOp = [this, &failures](std::unique_ptr<OpSendMsg>&& op) {
        if (op->result == ResultOk) {
            sendMessage(std::move(op));
        } else {
            LOG_ERROR("batchMessageAndSend | Failed to createOpSendMsg: " << op->result);
            releaseSemaphoreForSendOp(*op);
            auto rawOpPtr = op.release();
            failures.add([rawOpPtr] {
                std::unique_ptr<OpSendMsg> op{rawOpPtr};
                op->complete(op->result, {});
            });
        }
    };

    if (batchMessageContainer_->hasMultiOpSendMsgs()) {
        auto opSendMsgs = batchMessageContainer_->createOpSendMsgs(flushCallback);
        for (auto&& op : opSendMsgs) {
            handleOp(std::move(op));
        }
    } else {
        handleOp(batchMessageContainer_->createOpSendMsg(flushCallback));
    }
    return failures;
}

// Precondition -
// a. we have a reserved spot on the queue
// b. call this function after acquiring the ProducerImpl mutex_
void ProducerImpl::sendMessage(std::unique_ptr<OpSendMsg> opSendMsg) {
    const auto sequenceId = opSendMsg->sendArgs->sequenceId;
    LOG_DEBUG("Inserting data to pendingMessagesQueue_");
    auto args = opSendMsg->sendArgs;
    pendingMessagesQueue_.emplace_back(std::move(opSendMsg));

    ClientConnectionPtr cnx = getCnx().lock();
    if (cnx) {
        // If we do have a connection, the message is sent immediately, otherwise
        // we'll try again once a new connection is established
        LOG_DEBUG(getName() << "Sending msg immediately - seq: " << sequenceId);
        cnx->sendMessage(args);
    } else {
        LOG_DEBUG(getName() << "Connection is not ready - seq: " << sequenceId);
    }
}

void ProducerImpl::printStats() {
    if (batchMessageContainer_) {
        LOG_INFO("Producer - " << producerStr_ << ", [batchMessageContainer = " << *batchMessageContainer_
                               << "]");
    } else {
        LOG_INFO("Producer - " << producerStr_ << ", [batching  = off]");
    }
}

void ProducerImpl::closeAsync(CloseCallback originalCallback) {
    auto callback = [this, originalCallback](Result result) {
        if (result == ResultOk) {
            LOG_INFO(getName() << "Closed producer " << producerId_);
            shutdown();
        } else {
            LOG_ERROR(getName() << "Failed to close producer: " << strResult(result));
        }
        if (originalCallback) {
            originalCallback(result);
        }
    };

    Lock lock(mutex_);

    // if the producer was never started then there is nothing to clean up
    State expectedState = NotStarted;
    if (state_.compare_exchange_strong(expectedState, Closed)) {
        callback(ResultOk);
        return;
    }

    cancelTimers();

    if (semaphore_) {
        semaphore_->close();
    }

    // ensure any remaining send callbacks are called before calling the close callback
    failPendingMessages(ResultAlreadyClosed, false);

    // TODO  maybe we need a loop here to implement CAS for a condition,
    // just like Java's `getAndUpdate` method on an atomic variable
    const auto state = state_.load();
    if (state != Ready && state != Pending) {
        callback(ResultAlreadyClosed);

        return;
    }
    LOG_INFO(getName() << "Closing producer for topic " << topic_);
    state_ = Closing;

    ClientConnectionPtr cnx = getCnx().lock();
    if (!cnx) {
        callback(ResultOk);
        return;
    }

    // Detach the producer from the connection to avoid sending any other
    // message from the producer
    resetCnx();

    ClientImplPtr client = client_.lock();
    if (!client) {
        callback(ResultOk);
        return;
    }

    int requestId = client->newRequestId();
    auto self = shared_from_this();
    cnx->sendRequestWithId(Commands::newCloseProducer(producerId_, requestId), requestId)
        .addListener([self, callback](Result result, const ResponseData&) { callback(result); });
}

Future<Result, ProducerImplBaseWeakPtr> ProducerImpl::getProducerCreatedFuture() {
    return producerCreatedPromise_.getFuture();
}

uint64_t ProducerImpl::getProducerId() const { return producerId_; }

void ProducerImpl::handleSendTimeout(const boost::system::error_code& err) {
    const auto state = state_.load();
    if (state != Pending && state != Ready) {
        return;
    }
    Lock lock(mutex_);

    if (err == boost::asio::error::operation_aborted) {
        LOG_DEBUG(getName() << "Timer cancelled: " << err.message());
        return;
    } else if (err) {
        LOG_ERROR(getName() << "Timer error: " << err.message());
        return;
    }

    decltype(pendingMessagesQueue_) pendingMessages;
    if (pendingMessagesQueue_.empty()) {
        // If there are no pending messages, reset the timeout to the configured value.
        LOG_DEBUG(getName() << "Producer timeout triggered on empty pending message queue");
        asyncWaitSendTimeout(milliseconds(conf_.getSendTimeout()));
    } else {
        // If there is at least one message, calculate the diff between the message timeout and
        // the current time.
        time_duration diff = pendingMessagesQueue_.front()->timeout - TimeUtils::now();
        if (diff.total_milliseconds() <= 0) {
            // The diff is less than or equal to zero, meaning that the message has been expired.
            LOG_DEBUG(getName() << "Timer expired. Calling timeout callbacks.");
            pendingMessages = getPendingCallbacksWhenFailed();
            // Since the pending queue is cleared now, set timer to expire after configured value.
            asyncWaitSendTimeout(milliseconds(conf_.getSendTimeout()));
        } else {
            // The diff is greater than zero, set the timeout to the diff value
            LOG_DEBUG(getName() << "Timer hasn't expired yet, setting new timeout " << diff);
            asyncWaitSendTimeout(diff);
        }
    }

    lock.unlock();
    for (const auto& op : pendingMessages) {
        op->complete(ResultTimeout, {});
    }
}

bool ProducerImpl::removeCorruptMessage(uint64_t sequenceId) {
    Lock lock(mutex_);
    if (pendingMessagesQueue_.empty()) {
        LOG_DEBUG(getName() << " -- SequenceId - " << sequenceId << "]"  //
                            << "Got send failure for expired message, ignoring it.");
        return true;
    }

    std::unique_ptr<OpSendMsg> op{std::move(pendingMessagesQueue_.front().release())};
    uint64_t expectedSequenceId = op->sendArgs->sequenceId;
    if (sequenceId > expectedSequenceId) {
        LOG_WARN(getName() << "Got ack failure for msg " << sequenceId                //
                           << " expecting: " << expectedSequenceId << " queue size="  //
                           << pendingMessagesQueue_.size() << " producer: " << producerId_);
        return false;
    } else if (sequenceId < expectedSequenceId) {
        LOG_DEBUG(getName() << "Corrupt message is already timed out. Ignoring msg " << sequenceId);
        return true;
    } else {
        LOG_DEBUG(getName() << "Remove corrupt message from queue " << sequenceId);
        pendingMessagesQueue_.pop_front();
        lock.unlock();
        try {
            // to protect from client callback exception
            op->complete(ResultChecksumError, {});
        } catch (const std::exception& e) {
            LOG_ERROR(getName() << "Exception thrown from callback " << e.what());
        }
        releaseSemaphoreForSendOp(*op);
        return true;
    }
}

bool ProducerImpl::ackReceived(uint64_t sequenceId, MessageId& rawMessageId) {
    auto messageId = MessageIdBuilder::from(rawMessageId).partition(partition_).build();
    Lock lock(mutex_);

    if (pendingMessagesQueue_.empty()) {
        LOG_DEBUG(getName() << " -- SequenceId - " << sequenceId << "]"  //
                            << " -- MessageId - " << messageId << "]"
                            << "Got an SEND_ACK for expired message, ignoring it.");
        return true;
    }

    const auto& op = *pendingMessagesQueue_.front();
    if (op.result != ResultOk) {
        LOG_ERROR("Unexpected OpSendMsg whose result is " << op.result << " for " << sequenceId << " and "
                                                          << rawMessageId);
        return false;
    }

    uint64_t expectedSequenceId = op.sendArgs->sequenceId;
    if (sequenceId > expectedSequenceId) {
        LOG_WARN(getName() << "Got ack for msg " << sequenceId                        //
                           << " expecting: " << expectedSequenceId << " queue size="  //
                           << pendingMessagesQueue_.size() << " producer: " << producerId_);
        return false;
    } else if (sequenceId < expectedSequenceId) {
        // Ignoring the ack since it's referring to a message that has already timed out.
        LOG_DEBUG(getName() << "Got ack for timed out msg " << sequenceId  //
                            << " -- MessageId - " << messageId << " last-seq: " << expectedSequenceId
                            << " producer: " << producerId_);
        return true;
    }

    // Message was persisted correctly
    LOG_DEBUG(getName() << "Received ack for msg " << sequenceId);

    if (op.chunkedMessageId) {
        // Handling the chunk message id.
        if (op.chunkId == 0) {
            op.chunkedMessageId->setFirstChunkMessageId(messageId);
        } else if (op.chunkId == op.numChunks - 1) {
            op.chunkedMessageId->setLastChunkMessageId(messageId);
            messageId = op.chunkedMessageId->build();
        }
    }

    releaseSemaphoreForSendOp(op);
    lastSequenceIdPublished_ = sequenceId + op.messagesCount - 1;

    std::unique_ptr<OpSendMsg> opSendMsg{pendingMessagesQueue_.front().release()};
    pendingMessagesQueue_.pop_front();

    lock.unlock();
    try {
        opSendMsg->complete(ResultOk, messageId);
    } catch (const std::exception& e) {
        LOG_ERROR(getName() << "Exception thrown from callback " << e.what());
    }
    return true;
}

bool ProducerImpl::encryptMessage(proto::MessageMetadata& metadata, SharedBuffer& payload,
                                  SharedBuffer& encryptedPayload) {
    if (!conf_.isEncryptionEnabled() || msgCrypto_ == NULL) {
        encryptedPayload = payload;
        return true;
    }

    return msgCrypto_->encrypt(conf_.getEncryptionKeys(), conf_.getCryptoKeyReader(), metadata, payload,
                               encryptedPayload);
}

void ProducerImpl::disconnectProducer() {
    LOG_DEBUG("Broker notification of Closed producer: " << producerId_);
    resetCnx();
    scheduleReconnection(shared_from_this());
}

void ProducerImpl::start() {
    HandlerBase::start();

    if (conf_.getLazyStartPartitionedProducers() && conf_.getAccessMode() == ProducerConfiguration::Shared) {
        // we need to kick it off now as it is possible that the connection may take
        // longer than sendTimeout to connect
        startSendTimeoutTimer();
    }
}

void ProducerImpl::shutdown() {
    resetCnx();
    interceptors_->close();
    auto client = client_.lock();
    if (client) {
        client->cleanupProducer(this);
    }
    cancelTimers();
    producerCreatedPromise_.setFailed(ResultAlreadyClosed);
    state_ = Closed;
}

void ProducerImpl::cancelTimers() noexcept {
    dataKeyRefreshTask_.stop();
    boost::system::error_code ec;
    batchTimer_->cancel(ec);
    sendTimer_->cancel(ec);
}

bool ProducerImplCmp::operator()(const ProducerImplPtr& a, const ProducerImplPtr& b) const {
    return a->getProducerId() < b->getProducerId();
}

bool ProducerImpl::isClosed() { return state_ == Closed; }

bool ProducerImpl::isConnected() const { return !getCnx().expired() && state_ == Ready; }

uint64_t ProducerImpl::getNumberOfConnectedProducer() { return isConnected() ? 1 : 0; }

bool ProducerImpl::isStarted() const { return state_ != NotStarted; }
void ProducerImpl::startSendTimeoutTimer() {
    if (conf_.getSendTimeout() > 0) {
        asyncWaitSendTimeout(milliseconds(conf_.getSendTimeout()));
    }
}

void ProducerImpl::asyncWaitSendTimeout(DurationType expiryTime) {
    sendTimer_->expires_from_now(expiryTime);

    auto weakSelf = weak_from_this();
    sendTimer_->async_wait([weakSelf](const boost::system::error_code& err) {
        auto self = weakSelf.lock();
        if (self) {
            std::static_pointer_cast<ProducerImpl>(self)->handleSendTimeout(err);
        }
    });
}

ProducerImplWeakPtr ProducerImpl::weak_from_this() noexcept { return shared_from_this(); }

}  // namespace pulsar
/* namespace pulsar */
