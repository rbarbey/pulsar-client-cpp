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
#ifndef LIB_PRODUCERIMPL_H_
#define LIB_PRODUCERIMPL_H_

#include <boost/optional.hpp>
#include <memory>

#include "Future.h"
#include "HandlerBase.h"
// In MSVC and macOS, the value type of STL container cannot be forward declared
#if defined(_MSC_VER) || defined(__APPLE__)
#include "OpSendMsg.h"
#endif
#include "PendingFailures.h"
#include "PeriodicTask.h"
#include "ProducerImplBase.h"

namespace pulsar {

class BatchMessageContainerBase;
class ClientImpl;
using ClientImplPtr = std::shared_ptr<ClientImpl>;
using DeadlineTimerPtr = std::shared_ptr<boost::asio::deadline_timer>;
class MessageCrypto;
using MessageCryptoPtr = std::shared_ptr<MessageCrypto>;
class ProducerImpl;
using ProducerImplWeakPtr = std::weak_ptr<ProducerImpl>;
class ProducerStatsBase;
using ProducerStatsBasePtr = std::shared_ptr<ProducerStatsBase>;
struct ResponseData;
class ProducerImpl;
using ProducerImplPtr = std::shared_ptr<ProducerImpl>;

class PulsarFriend;

class Producer;
class MemoryLimitController;
class Semaphore;
class TopicName;
struct OpSendMsg;

namespace proto {
class MessageMetadata;
}  // namespace proto

class ProducerImpl : public HandlerBase,
                     public std::enable_shared_from_this<ProducerImpl>,
                     public ProducerImplBase {
   public:
    ProducerImpl(ClientImplPtr client, const TopicName& topic,
                 const ProducerConfiguration& producerConfiguration,
                 const ProducerInterceptorsPtr& interceptors, int32_t partition = -1);
    ~ProducerImpl();

    // overrided methods from ProducerImplBase
    const std::string& getProducerName() const override;
    int64_t getLastSequenceId() const override;
    const std::string& getSchemaVersion() const override;
    void sendAsync(const Message& msg, SendCallback callback) override;
    void closeAsync(CloseCallback callback) override;
    void start() override;
    void shutdown() override;
    bool isClosed() override;
    const std::string& getTopic() const override;
    Future<Result, ProducerImplBaseWeakPtr> getProducerCreatedFuture() override;
    void triggerFlush() override;
    void flushAsync(FlushCallback callback) override;
    bool isConnected() const override;
    uint64_t getNumberOfConnectedProducer() override;
    bool isStarted() const;

    bool removeCorruptMessage(uint64_t sequenceId);

    bool ackReceived(uint64_t sequenceId, MessageId& messageId);

    virtual void disconnectProducer();

    uint64_t getProducerId() const;

    int32_t partition() const noexcept { return partition_; }

    static int getNumOfChunks(uint32_t size, uint32_t maxMessageSize);

    // NOTE: this method is introduced into `enable_shared_from_this` since C++17
    ProducerImplWeakPtr weak_from_this() noexcept;

   protected:
    ProducerStatsBasePtr producerStatsBasePtr_;

    void setMessageMetadata(const Message& msg, const uint64_t& sequenceId, const uint32_t& uncompressedSize);

    void sendMessage(std::unique_ptr<OpSendMsg> opSendMsg);

    void startSendTimeoutTimer();

    friend class PulsarFriend;

    friend class Producer;

    friend class BatchMessageContainerBase;
    friend class BatchMessageContainer;

    // overrided methods from HandlerBase
    void beforeConnectionChange(ClientConnection& connection) override;
    void connectionOpened(const ClientConnectionPtr& connection) override;
    void connectionFailed(Result result) override;
    HandlerBaseWeakPtr get_weak_from_this() override { return shared_from_this(); }
    const std::string& getName() const override { return producerStr_; }

   private:
    void printStats();

    void handleCreateProducer(const ClientConnectionPtr& cnx, Result result,
                              const ResponseData& responseData);

    void resendMessages(ClientConnectionPtr cnx);

    void refreshEncryptionKey(const boost::system::error_code& ec);
    bool encryptMessage(proto::MessageMetadata& metadata, SharedBuffer& payload,
                        SharedBuffer& encryptedPayload);

    void sendAsyncWithStatsUpdate(const Message& msg, SendCallback&& callback);

    /**
     * Reserve a spot in the messages queue before acquiring the ProducerImpl mutex. When the queue is full,
     * this call will block until a spot is available if blockIfQueueIsFull is true. Otherwise, it will return
     * ResultProducerQueueIsFull immediately.
     *
     * It also checks whether the memory could reach the limit after `payloadSize` is added. If so, this call
     * will block until enough memory could be retained.
     */
    Result canEnqueueRequest(uint32_t payloadSize);

    void releaseSemaphore(uint32_t payloadSize);
    void releaseSemaphoreForSendOp(const OpSendMsg& op);

    void cancelTimers() noexcept;

    bool isValidProducerState(const SendCallback& callback) const;
    bool canAddToBatch(const Message& msg) const;

    typedef std::unique_lock<std::mutex> Lock;

    ProducerConfiguration conf_;

    std::unique_ptr<Semaphore> semaphore_;
    std::list<std::unique_ptr<OpSendMsg>> pendingMessagesQueue_;

    const int32_t partition_;  // -1 if topic is non-partitioned
    std::string producerName_;
    bool userProvidedProducerName_;
    std::string producerStr_;
    uint64_t producerId_;
    int64_t msgSequenceGenerator_;

    std::unique_ptr<BatchMessageContainerBase> batchMessageContainer_;
    DeadlineTimerPtr batchTimer_;
    PendingFailures batchMessageAndSend(const FlushCallback& flushCallback = nullptr);

    volatile int64_t lastSequenceIdPublished_;
    std::string schemaVersion_;

    DeadlineTimerPtr sendTimer_;
    void handleSendTimeout(const boost::system::error_code& err);
    using DurationType = typename boost::asio::deadline_timer::duration_type;
    void asyncWaitSendTimeout(DurationType expiryTime);

    Promise<Result, ProducerImplBaseWeakPtr> producerCreatedPromise_;

    struct PendingCallbacks;
    decltype(pendingMessagesQueue_) getPendingCallbacksWhenFailed();
    decltype(pendingMessagesQueue_) getPendingCallbacksWhenFailedWithLock();

    void failPendingMessages(Result result, bool withLock);

    MessageCryptoPtr msgCrypto_;
    PeriodicTask dataKeyRefreshTask_;

    MemoryLimitController& memoryLimitController_;
    const bool chunkingEnabled_;
    boost::optional<uint64_t> topicEpoch;

    ProducerInterceptorsPtr interceptors_;
};

struct ProducerImplCmp {
    bool operator()(const ProducerImplPtr& a, const ProducerImplPtr& b) const;
};

} /* namespace pulsar */

#endif /* LIB_PRODUCERIMPL_H_ */
