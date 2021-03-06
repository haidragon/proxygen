/*
 *  Copyright (c) 2019-present, Facebook, Inc.
 *  All rights reserved.
 *
 *  This source code is licensed under the BSD-style license found in the
 *  LICENSE file in the root directory of this source tree. An additional grant
 *  of patent rights can be found in the PATENTS file in the same directory.
 *
 */
#include <proxygen/lib/http/session/HQUpstreamSession.h>
#include <folly/io/async/EventBaseManager.h>
#include <proxygen/lib/http/codec/HQControlCodec.h>
#include <proxygen/lib/http/codec/HQStreamCodec.h>
#include <proxygen/lib/http/codec/HTTP1xCodec.h>
#include <proxygen/lib/http/codec/test/TestUtils.h>
#include <proxygen/lib/http/session/test/HQSessionMocks.h>
#include <proxygen/lib/http/session/test/HQSessionTestCommon.h>
#include <proxygen/lib/http/session/test/HTTPSessionMocks.h>
#include <proxygen/lib/http/session/test/HTTPTransactionMocks.h>
#include <proxygen/lib/http/session/test/MockQuicSocketDriver.h>
#include <proxygen/lib/http/session/test/TestUtils.h>
#include <quic/api/test/MockQuicSocket.h>
#include <wangle/acceptor/ConnectionManager.h>

#include <folly/futures/Future.h>
#include <folly/portability/GTest.h>

using namespace proxygen;
using namespace proxygen::hq;
using namespace quic;
using namespace folly;
using namespace testing;
using namespace std::chrono;
using std::unique_ptr;

constexpr quic::StreamId kQPACKEncoderIngressStreamId = 7;
constexpr quic::StreamId kQPACKDecoderEgressStreamId = 10;

class HQUpstreamSessionTest : public HQSessionTest {
 public:
  HQUpstreamSessionTest()
      : HQSessionTest(proxygen::TransportDirection::UPSTREAM) {
  }

 protected:
  std::pair<HTTPCodec::StreamID, std::unique_ptr<HTTPCodec>> makeCodec(
      HTTPCodec::StreamID id) {
    if (IS_HQ) {
      return {id,
              std::make_unique<hq::HQStreamCodec>(
                  id,
                  TransportDirection::DOWNSTREAM,
                  qpackCodec_,
                  encoderWriteBuf_,
                  decoderWriteBuf_,
                  [] { return std::numeric_limits<uint64_t>::max(); },
                  egressSettings_,
                  ingressSettings_,
                  false)};
    } else {
      auto codec =
          std::make_unique<HTTP1xCodec>(TransportDirection::DOWNSTREAM, true);
      // When the codec is created, need to fake the request
      FakeHTTPCodecCallback cb;
      codec->setCallback(&cb);
      codec->onIngress(*folly::IOBuf::copyBuffer("GET / HTTP/1.1\r\n\r\n"));
      return {1, std::move(codec)};
    }
  }

  void sendResponse(quic::StreamId id,
                    const HTTPMessage& resp,
                    std::unique_ptr<folly::IOBuf> body = nullptr,
                    bool eom = true) {
    auto c = makeCodec(id);
    auto res =
        streams_.emplace(std::piecewise_construct,
                         std::forward_as_tuple(id),
                         std::forward_as_tuple(c.first, std::move(c.second)));
    auto& stream = res.first->second;
    stream.readEOF = eom;
    stream.codec->generateHeader(
        stream.buf, stream.codecId, resp, body == nullptr ? eom : false);
    if (body && body->computeChainDataLength() > 0) {
      stream.codec->generateBody(
          stream.buf, stream.codecId, std::move(body), folly::none, eom);
    }
  }

  void SetUp() override {
    folly::EventBaseManager::get()->clearEventBase();
    quic::QuicSocket::TransportInfo transportInfo = {
        .srtt = std::chrono::microseconds(100),
        .rttvar = std::chrono::microseconds(0),
        .writableBytes = 0,
        .congestionWindow = 1500,
        .packetsRetransmitted = 0,
        .timeoutBasedLoss = 0,
        .pto = std::chrono::microseconds(0),
        .bytesSent = 0,
        .bytesRecvd = 0,
        .ptoCount = 0,
        .totalPTOCount = 0};
    EXPECT_CALL(*socketDriver_->getSocket(), getTransportInfo())
        .WillRepeatedly(Return(transportInfo));
    localAddress_.setFromIpPort("0.0.0.0", 0);
    peerAddress_.setFromIpPort("127.0.0.0", 443);
    EXPECT_CALL(*socketDriver_->getSocket(), getLocalAddress())
        .WillRepeatedly(ReturnRef(localAddress_));

    EXPECT_CALL(*socketDriver_->getSocket(), getPeerAddress())
        .WillRepeatedly(ReturnRef(peerAddress_));
    EXPECT_CALL(*socketDriver_->getSocket(), getAppProtocol())
        .WillRepeatedly(Return(getProtocolString()));
    HTTPSession::setDefaultWriteBufferLimit(65536);
    HTTP2PriorityQueue::setNodeLifetime(std::chrono::milliseconds(2));
    dynamic_cast<HQUpstreamSession*>(hqSession_)
        ->setConnectCallback(&connectCb_);

    EXPECT_CALL(connectCb_, connectSuccess());

    hqSession_->onTransportReady();

    createControlStreams();

    flushAndLoop();
    if (IS_HQ) {
      EXPECT_EQ(httpCallbacks_.settings, 1);
    }
  }

  void TearDown() override {
    if (!IS_H1Q_FB_V1) {
      // With control streams we may need an extra loop for proper shutdown
      if (!socketDriver_->isClosed()) {
        // Send the first GOAWAY with MAX_STREAM_ID immediately
        sendGoaway(quic::kEightByteLimit);
        // Schedule the second GOAWAY with the last seen stream ID, after some
        // delay
        sendGoaway(socketDriver_->getMaxStreamId(), milliseconds(50));
      }
      eventBase_.loopOnce();
    }
  }

  void sendGoaway(quic::StreamId lastStreamId,
                  milliseconds delay = milliseconds(0)) {
    folly::IOBufQueue writeBuf{folly::IOBufQueue::cacheChainLength()};
    egressControlCodec_->generateGoaway(
        writeBuf, lastStreamId, ErrorCode::NO_ERROR);
    socketDriver_->addReadEvent(connControlStreamId_, writeBuf.move(), delay);
  }

  std::unique_ptr<StrictMock<MockHTTPHandler>> openTransaction(
      bool expectStartPaused = false) {
    // Returns a mock handler with txn_ field set in it
    auto handler = std::make_unique<StrictMock<MockHTTPHandler>>();
    handler->expectTransaction();
    if (expectStartPaused) {
      handler->expectEgressPaused();
    }
    auto txn = hqSession_->newTransaction(handler.get());
    EXPECT_EQ(txn, handler->txn_);
    return handler;
  }

  void flushAndLoop(
      bool eof = false,
      milliseconds eofDelay = milliseconds(0),
      milliseconds initialDelay = milliseconds(0),
      std::function<void()> extraEventsFn = std::function<void()>()) {
    flush(eof, eofDelay, initialDelay, extraEventsFn);
    CHECK(eventBase_.loop());
  }

  void flushAndLoopN(
      uint64_t n,
      bool eof = false,
      milliseconds eofDelay = milliseconds(0),
      milliseconds initialDelay = milliseconds(0),
      std::function<void()> extraEventsFn = std::function<void()>()) {
    flush(eof, eofDelay, initialDelay, extraEventsFn);
    for (uint64_t i = 0; i < n; i++) {
      eventBase_.loopOnce();
    }
  }

  bool flush(bool eof = false,
             milliseconds eofDelay = milliseconds(0),
             milliseconds initialDelay = milliseconds(0),
             std::function<void()> extraEventsFn = std::function<void()>()) {
    bool done = true;
    if (!encoderWriteBuf_.empty()) {
      socketDriver_->addReadEvent(kQPACKEncoderIngressStreamId,
                                  encoderWriteBuf_.move(),
                                  milliseconds(0));
    }
    for (auto& stream : streams_) {
      if (socketDriver_->isStreamIdle(stream.first)) {
        continue;
      }
      if (stream.second.buf.chainLength() > 0) {
        socketDriver_->addReadEvent(
            stream.first, stream.second.buf.move(), initialDelay);
        done = false;
      }
      // EOM -> stream EOF
      if (stream.second.readEOF) {
        socketDriver_->addReadEOF(stream.first, eofDelay);
        done = false;
      }
    }
    if (extraEventsFn) {
      extraEventsFn();
    }
    if (eof || eofDelay.count() > 0) {
      /*  wonkiness.  Should somehow close the connection?
       * socketDriver_->addReadEOF(1, eofDelay);
       */
    }
    return done;
  }

  struct ServerStream {
    ServerStream(HTTPCodec::StreamID cId, std::unique_ptr<HTTPCodec> c)
        : codecId(cId), codec(std::move(c)) {
    }
    HTTPCodec::StreamID id;
    IOBufQueue buf{IOBufQueue::cacheChainLength()};
    bool readEOF{false};
    HTTPCodec::StreamID codecId;
    std::unique_ptr<HTTPCodec> codec;
  };

  MockConnectCallback connectCb_;
  std::unordered_map<quic::StreamId, ServerStream> streams_;
  folly::IOBufQueue encoderWriteBuf_{folly::IOBufQueue::cacheChainLength()};
  folly::IOBufQueue decoderWriteBuf_{folly::IOBufQueue::cacheChainLength()};
};

// Use this test class for h1q-fb only tests
using HQUpstreamSessionTestH1q = HQUpstreamSessionTest;
// Use this test class for h1q-fb-v1 only tests
using HQUpstreamSessionTestH1qv1 = HQUpstreamSessionTest;
// Use this test class for h1q-fb-v2 only tests
using HQUpstreamSessionTestH1qv2 = HQUpstreamSessionTest;
// Use this test class for h1q-fb-v2 and hq tests
using HQUpstreamSessionTestH1qv2HQ = HQUpstreamSessionTest;
// Use this test class for hq only tests
using HQUpstreamSessionTestHQ = HQUpstreamSessionTest;

TEST_P(HQUpstreamSessionTest, SimpleGet) {
  auto handler = openTransaction();
  handler->txn_->sendHeaders(getGetRequest());
  handler->txn_->sendEOM();
  handler->expectHeaders();
  handler->expectBody();
  handler->expectEOM();
  handler->expectDetachTransaction();
  auto resp = makeResponse(200, 100);
  sendResponse(handler->txn_->getID(),
               *std::get<0>(resp),
               std::move(std::get<1>(resp)),
               true);
  flushAndLoop();
  hqSession_->closeWhenIdle();
}

TEST_P(HQUpstreamSessionTest, NoNewTransactionIfSockIsNotGood) {
  socketDriver_->sockGood_ = false;
  EXPECT_EQ(hqSession_->newTransaction(nullptr), nullptr);
  hqSession_->closeWhenIdle();
}

TEST_P(HQUpstreamSessionTest, DropConnectionWithEarlyDataFailedError) {
  auto handler = openTransaction();
  handler->txn_->sendHeaders(getGetRequest());
  handler->txn_->sendEOM();

  EXPECT_CALL(*handler, onError(_))
      .WillOnce(Invoke([](const HTTPException& error) {
        EXPECT_EQ(error.getProxygenError(), kErrorEarlyDataFailed);
        EXPECT_TRUE(std::string(error.what()).find("quic loses race") !=
                    std::string::npos);
      }));
  handler->expectDetachTransaction();
  socketDriver_->deliverConnectionError(
      {HTTP3::ErrorCode::GIVEUP_ZERO_RTT, "quic loses race"});
}

TEST_P(HQUpstreamSessionTest, ResponseTermedByFin) {
  auto handler = openTransaction();
  handler->txn_->sendHeaders(getGetRequest());
  handler->txn_->sendEOM();
  handler->expectHeaders();
  handler->expectBody();
  handler->expectEOM();
  handler->expectDetachTransaction();
  HTTPMessage resp;
  resp.setStatusCode(200);
  resp.setHTTPVersion(1, 0);
  // HTTP/1.0 response with no content-length, termed by tranport FIN
  sendResponse(handler->txn_->getID(), resp, makeBuf(100), true);
  flushAndLoop();
  hqSession_->closeWhenIdle();
}

TEST_P(HQUpstreamSessionTest, WaitForReplaySafeCallback) {
  auto handler = openTransaction();
  StrictMock<folly::test::MockReplaySafetyCallback> cb1;
  StrictMock<folly::test::MockReplaySafetyCallback> cb2;
  StrictMock<folly::test::MockReplaySafetyCallback> cb3;

  auto sock = socketDriver_->getSocket();
  EXPECT_CALL(*sock, replaySafe()).WillRepeatedly(Return(false));
  handler->txn_->addWaitingForReplaySafety(&cb1);
  handler->txn_->addWaitingForReplaySafety(&cb2);
  handler->txn_->addWaitingForReplaySafety(&cb3);
  handler->txn_->removeWaitingForReplaySafety(&cb2);

  ON_CALL(*sock, replaySafe()).WillByDefault(Return(true));
  EXPECT_CALL(cb1, onReplaySafe_());
  EXPECT_CALL(cb3, onReplaySafe_());
  hqSession_->onReplaySafe();

  handler->expectDetachTransaction();
  handler->txn_->sendAbort();
  hqSession_->closeWhenIdle();
  eventBase_.loopOnce();
}

TEST_P(HQUpstreamSessionTest, AlreadyReplaySafe) {
  auto handler = openTransaction();

  StrictMock<folly::test::MockReplaySafetyCallback> cb;

  auto sock = socketDriver_->getSocket();
  EXPECT_CALL(*sock, replaySafe()).WillRepeatedly(Return(true));
  EXPECT_CALL(cb, onReplaySafe_());
  handler->txn_->addWaitingForReplaySafety(&cb);

  handler->expectDetachTransaction();
  handler->txn_->sendAbort();
  hqSession_->closeWhenIdle();
  eventBase_.loopOnce();
}

TEST_P(HQUpstreamSessionTest, Test100Continue) {
  InSequence enforceOrder;
  auto handler = openTransaction();
  auto req = getPostRequest(10);
  req.getHeaders().add(HTTP_HEADER_EXPECT, "100-continue");
  handler->txn_->sendHeaders(req);
  handler->txn_->sendEOM();
  handler->expectHeaders();
  handler->expectHeaders();
  handler->expectBody();
  handler->expectEOM();
  handler->expectDetachTransaction();
  sendResponse(handler->txn_->getID(), *makeResponse(100), nullptr, false);
  auto resp = makeResponse(200, 100);
  sendResponse(handler->txn_->getID(),
               *std::get<0>(resp),
               std::move(std::get<1>(resp)),
               true);
  flushAndLoop();
  hqSession_->closeWhenIdle();
}

TEST_P(HQUpstreamSessionTest, GetAddresses) {
  folly::SocketAddress localAddr("::", 65001);
  folly::SocketAddress remoteAddr("31.13.31.13", 3113);
  EXPECT_CALL(*socketDriver_->getSocket(), getLocalAddress())
      .WillRepeatedly(ReturnRef(localAddr));
  EXPECT_CALL(*socketDriver_->getSocket(), getPeerAddress())
      .WillRepeatedly(ReturnRef(remoteAddr));
  EXPECT_EQ(localAddr, hqSession_->getLocalAddress());
  EXPECT_EQ(remoteAddr, hqSession_->getPeerAddress());
  hqSession_->dropConnection();
}

TEST_P(HQUpstreamSessionTest, GetAddressesFromBase) {
  HTTPSessionBase* sessionBase = dynamic_cast<HTTPSessionBase*>(hqSession_);
  EXPECT_EQ(localAddress_, sessionBase->getLocalAddress());
  EXPECT_EQ(localAddress_, sessionBase->getLocalAddress());
  hqSession_->dropConnection();
}

TEST_P(HQUpstreamSessionTest, GetAddressesAfterDropConnection) {
  HQSession::DestructorGuard dg(hqSession_);
  hqSession_->dropConnection();
  EXPECT_EQ(localAddress_, hqSession_->getLocalAddress());
  EXPECT_EQ(peerAddress_, hqSession_->getPeerAddress());
}

TEST_P(HQUpstreamSessionTest, DropConnectionTwice) {
  HQSession::DestructorGuard dg(hqSession_);
  hqSession_->closeWhenIdle();
  hqSession_->dropConnection();
}

TEST_P(HQUpstreamSessionTest, NotifyConnectCallbackBeforeDestruct) {
  MockConnectCallback connectCb;
  dynamic_cast<HQUpstreamSession*>(hqSession_)->setConnectCallback(&connectCb);
  EXPECT_CALL(connectCb, connectError(_)).Times(1);
  socketDriver_->deliverConnectionError(
      {quic::LocalErrorCode::CONNECT_FAILED, "Peer closed"});
}

TEST_P(HQUpstreamSessionTest, DropFromConnectError) {
  MockConnectCallback connectCb;
  HQUpstreamSession* upstreamSess =
      dynamic_cast<HQUpstreamSession*>(hqSession_);
  upstreamSess->setConnectCallback(&connectCb);
  EXPECT_CALL(connectCb, connectError(_)).WillOnce(InvokeWithoutArgs([&] {
    hqSession_->dropConnection();
  }));
  socketDriver_->addOnConnectionEndEvent(0);
  eventBase_.loop();
}

TEST_P(HQUpstreamSessionTest, NotifyReplaySafeAfterTransportReady) {
  MockConnectCallback connectCb;
  HQUpstreamSession* upstreamSess =
      dynamic_cast<HQUpstreamSession*>(hqSession_);
  upstreamSess->setConnectCallback(&connectCb);

  // onTransportReady gets called in SetUp() already

  EXPECT_CALL(connectCb, onReplaySafe());
  upstreamSess->onReplaySafe();

  upstreamSess->closeWhenIdle();
  eventBase_.loopOnce();
}

TEST_P(HQUpstreamSessionTest, OnConnectionErrorWithOpenStreams) {
  HQSession::DestructorGuard dg(hqSession_);
  auto handler = openTransaction();
  handler->expectError();
  handler->expectDetachTransaction();
  hqSession_->onConnectionError(
      std::make_pair(quic::LocalErrorCode::CONNECT_FAILED,
                     "Connect Failure with Open streams"));
  eventBase_.loop();
  EXPECT_EQ(hqSession_->getConnectionCloseReason(),
            ConnectionCloseReason::SHUTDOWN);
}

TEST_P(HQUpstreamSessionTest, OnConnectionErrorWithOpenStreamsPause) {
  HQSession::DestructorGuard dg(hqSession_);
  auto handler1 = openTransaction();
  auto handler2 = openTransaction();
  handler1->txn_->sendHeaders(getGetRequest());
  handler1->txn_->sendEOM();
  handler2->txn_->sendHeaders(getGetRequest());
  handler2->txn_->sendEOM();
  auto resp = makeResponse(200, 100);
  sendResponse(handler1->txn_->getID(),
               *std::get<0>(resp),
               std::move(std::get<1>(resp)),
               true);
  resp = makeResponse(200, 100);
  sendResponse(handler2->txn_->getID(),
               *std::get<0>(resp),
               std::move(std::get<1>(resp)),
               true);
  flush();
  eventBase_.runInLoop([&] {
    hqSession_->onConnectionError(
        std::make_pair(quic::LocalErrorCode::CONNECT_FAILED,
                       "Connect Failure with Open streams"));
  });
  handler1->expectError(
      [&](const HTTPException&) { handler2->txn_->pauseIngress(); });
  handler1->expectDetachTransaction();
  handler2->expectError();
  handler2->expectDetachTransaction();
  eventBase_.loop();
  EXPECT_EQ(hqSession_->getConnectionCloseReason(),
            ConnectionCloseReason::SHUTDOWN);
}

TEST_P(HQUpstreamSessionTestH1qv2HQ, GoawayStreamsUnacknowledged) {
  std::vector<std::unique_ptr<StrictMock<MockHTTPHandler>>> handlers;
  auto numStreams = 4;
  quic::StreamId goawayId = (numStreams * 4) / 2;
  for (auto n = 1; n <= numStreams; n++) {
    handlers.emplace_back(openTransaction());
    auto handler = handlers.back().get();
    handler->txn_->sendHeaders(getGetRequest());
    handler->txn_->sendEOM();
    EXPECT_CALL(*handler, onGoaway(testing::_)).Times(2);
    if (handler->txn_->getID() > goawayId) {
      handler->expectError([hdlr = handler](const HTTPException& err) {
        EXPECT_TRUE(err.hasProxygenError());
        EXPECT_EQ(err.getProxygenError(), kErrorStreamUnacknowledged);
        ASSERT_EQ(
            folly::to<std::string>("StreamUnacknowledged on transaction id: ",
                                   hdlr->txn_->getID()),
            std::string(err.what()));
      });
    } else {
      handler->expectHeaders();
      handler->expectBody();
      handler->expectEOM();
    }

    if (n < numStreams) {
      handler->expectDetachTransaction();
    } else {
      handler->expectDetachTransaction([&] {
        // Make sure the session can't create any more transactions.
        MockHTTPHandler handler2;
        EXPECT_EQ(hqSession_->newTransaction(&handler2), nullptr);
        // Send the responses for the acknowledged streams
        for (auto& hdlr : handlers) {
          auto id = hdlr->txn_->getID();
          if (id <= goawayId) {
            auto resp = makeResponse(200, 100);
            sendResponse(
                id, *std::get<0>(resp), std::move(std::get<1>(resp)), true);
          }
        }
        flush();
      });
    }
  }

  sendGoaway(quic::kEightByteLimit, milliseconds(50));
  sendGoaway(goawayId, milliseconds(100));
  flushAndLoop();
}

TEST_P(HQUpstreamSessionTestHQ, DelayedQPACK) {
  InSequence enforceOrder;
  auto handler = openTransaction();
  handler->txn_->sendHeaders(getGetRequest());
  handler->txn_->sendEOM();
  handler->expectHeaders();
  handler->expectHeaders();
  handler->expectBody();
  handler->expectEOM();
  handler->expectDetachTransaction();
  auto cont = makeResponse(100);
  auto resp = makeResponse(200, 100);
  cont->getHeaders().add("X-FB-Debug", "jvrbfihvuvvclgvfkbkikjlcbruleekj");
  std::get<0>(resp)->getHeaders().add("X-FB-Debug",
                                      "egedljtrbullljdjjvtjkekebffefclj");
  sendResponse(handler->txn_->getID(), *cont, nullptr, false);
  sendResponse(handler->txn_->getID(),
               *std::get<0>(resp),
               std::move(std::get<1>(resp)),
               true);
  auto control = encoderWriteBuf_.move();
  flushAndLoopN(1);
  encoderWriteBuf_.append(std::move(control));
  flushAndLoop();
  hqSession_->closeWhenIdle();
}

TEST_P(HQUpstreamSessionTestHQ, DelayedQPACKTimeout) {
  InSequence enforceOrder;
  auto handler = openTransaction();
  handler->txn_->sendHeaders(getGetRequest());
  handler->txn_->sendEOM();
  handler->expectError();
  auto resp = makeResponse(200, 100);
  std::get<0>(resp)->getHeaders().add("X-FB-Debug",
                                      "egedljtrbullljdjjvtjkekebffefclj");
  sendResponse(handler->txn_->getID(),
               *std::get<0>(resp),
               std::move(std::get<1>(resp)),
               true);
  auto control = encoderWriteBuf_.move();
  handler->expectDetachTransaction([this, &control]() mutable {
    // have the header block arrive after destruction
    encoderWriteBuf_.append(std::move(control));
    eventBase_.runInLoop([this] { flush(); });
    eventBase_.runAfterDelay([this] { hqSession_->closeWhenIdle(); }, 100);
  });
  flushAndLoop();
}

TEST_P(HQUpstreamSessionTestHQ, QPACKDecoderStreamFlushed) {
  InSequence enforceOrder;
  auto handler = openTransaction();
  handler->txn_->sendHeadersWithOptionalEOM(getGetRequest(), true);
  flushAndLoopN(1);
  handler->expectDetachTransaction();
  handler->txn_->sendAbort();
  flushAndLoop();
  auto& decoderStream = socketDriver_->streams_[kQPACKDecoderEgressStreamId];
  // type byte plus cancel
  EXPECT_EQ(decoderStream.writeBuf.chainLength(), 2);

  handler = openTransaction();
  handler->txn_->sendHeadersWithOptionalEOM(getGetRequest(), true);
  handler->expectHeaders();
  handler->expectBody();
  handler->expectEOM();
  auto resp = makeResponse(200, 100);
  std::get<0>(resp)->getHeaders().add("Response", "Dynamic");
  sendResponse(handler->txn_->getID(),
               *std::get<0>(resp),
               std::move(std::get<1>(resp)),
               true);
  auto qpackData = encoderWriteBuf_.move();
  flushAndLoopN(1);
  encoderWriteBuf_.append(std::move(qpackData));
  handler->expectDetachTransaction();
  hqSession_->closeWhenIdle();
  flushAndLoop();
  // type byte plus cancel plus ack
  EXPECT_EQ(decoderStream.writeBuf.chainLength(), 3);
}

TEST_P(HQUpstreamSessionTestHQ, DelayedQPACKAfterReset) {
  // Stand on your head and spit wooden nickels
  // Ensure the session does not deliver input data to a transaction detached
  // earlier the same loop
  InSequence enforceOrder;
  // Send two requests
  auto handler1 = openTransaction();
  auto handler2 = openTransaction();
  handler1->txn_->sendHeadersWithOptionalEOM(getGetRequest(), true);
  handler2->txn_->sendHeadersWithOptionalEOM(getGetRequest(), true);
  // Send a response to txn1 that will block on QPACK data
  auto resp1 = makeResponse(302, 0);
  std::get<0>(resp1)->getHeaders().add("Response1", "Dynamic");
  sendResponse(handler1->txn_->getID(),
               *std::get<0>(resp1),
               std::move(std::get<1>(resp1)),
               true);
  // Save first QPACK data
  auto qpackData1 = encoderWriteBuf_.move();
  // Send response to txn2 that will block on *different* QPACK data
  auto resp2 = makeResponse(302, 0);
  std::get<0>(resp2)->getHeaders().add("Respnse2", "Dynamic");
  sendResponse(handler2->txn_->getID(),
               *std::get<0>(resp2),
               std::move(std::get<1>(resp2)),
               false);
  // Save second QPACK data
  auto qpackData2 = encoderWriteBuf_.move();

  // Abort *both* txns when txn1 gets headers.  This will leave txn2 detached
  // with pending input data in this loop.
  handler1->expectHeaders([&] {
    handler1->txn_->sendAbort();
    handler2->txn_->sendAbort();
  });

  auto streamIt1 = streams_.find(handler1->txn_->getID());
  CHECK(streamIt1 != streams_.end());
  auto streamIt2 = streams_.find(handler2->txn_->getID());
  CHECK(streamIt2 != streams_.end());
  // add all the events in the same callback, with the stream data coming
  // before the QPACK data
  std::vector<MockQuicSocketDriver::ReadEvent> events;
  events.emplace_back(handler2->txn_->getID(),
                      streamIt2->second.buf.move(),
                      streamIt2->second.readEOF,
                      folly::none,
                      false);
  events.emplace_back(handler1->txn_->getID(),
                      streamIt1->second.buf.move(),
                      streamIt1->second.readEOF,
                      folly::none,
                      false);
  events.emplace_back(kQPACKEncoderIngressStreamId,
                      std::move(qpackData1),
                      false,
                      folly::none,
                      false);
  socketDriver_->addReadEvents(std::move(events));
  handler2->expectDetachTransaction();
  handler1->expectDetachTransaction();
  eventBase_.loopOnce();
  // Add the QPACK data that would unblock txn2.  It's long gone and this
  // should be a no-op.
  socketDriver_->addReadEvent(kQPACKEncoderIngressStreamId,
                              std::move(qpackData2));
  eventBase_.loopOnce();
  hqSession_->closeWhenIdle();
}

// This test is checking two different scenarios for different protocol
//   - in HQ we already have sent SETTINGS in SetUp, so tests that multiple
//     setting frames are not allowed
//   - in h1q-fb-v2 tests that receiving even a single SETTINGS frame errors
//     out the connection
TEST_P(HQUpstreamSessionTestH1qv2HQ, ExtraSettings) {
  auto handler = openTransaction();
  handler->txn_->sendHeaders(getGetRequest());
  handler->txn_->sendEOM();
  handler->expectError();
  handler->expectDetachTransaction();

  // Need to use a new codec. Since generating settings twice is
  // forbidden
  HQControlCodec auxControlCodec_{nextUnidirectionalStreamId_,
                                  TransportDirection::DOWNSTREAM,
                                  StreamDirection::EGRESS,
                                  egressSettings_,
                                  UnidirectionalStreamType::H1Q_CONTROL};
  folly::IOBufQueue writeBuf{folly::IOBufQueue::cacheChainLength()};
  auxControlCodec_.generateSettings(writeBuf);
  socketDriver_->addReadEvent(
      connControlStreamId_, writeBuf.move(), milliseconds(0));

  flushAndLoop();

  EXPECT_EQ(*socketDriver_->streams_[kConnectionStreamId].error,
            HTTP3::ErrorCode::HTTP_UNEXPECTED_FRAME);
}

using HQUpstreamSessionDeathTestH1qv2HQ = HQUpstreamSessionTestH1qv2HQ;
TEST_P(HQUpstreamSessionDeathTestH1qv2HQ, WriteExtraSettings) {
  EXPECT_EXIT(sendSettings(),
              ::testing::KilledBySignal(SIGABRT),
              "Check failed: !sentSettings_");
}

// Test Cases for which Settings are not sent in the test SetUp
using HQUpstreamSessionTestHQNoSettings = HQUpstreamSessionTest;

INSTANTIATE_TEST_CASE_P(HQUpstreamSessionTest,
                        HQUpstreamSessionTestHQNoSettings,
                        Values(TestParams({.alpn_ = "h3",
                                           .shouldSendSettings_ = false})),
                        paramsToTestName);
TEST_P(HQUpstreamSessionTestHQNoSettings, SimpleGet) {
  EXPECT_CALL(connectCb_, connectError(_)).Times(1);
  socketDriver_->deliverConnectionError(
      {quic::LocalErrorCode::CONNECT_FAILED, "Peer closed"});
}

TEST_P(HQUpstreamSessionTestHQNoSettings, GoawayBeforeSettings) {
  auto handler = openTransaction();
  handler->txn_->sendHeaders(getGetRequest());
  handler->txn_->sendEOM();
  handler->expectError();
  handler->expectDetachTransaction();

  sendGoaway(quic::kEightByteLimit);
  flushAndLoop();

  EXPECT_EQ(*socketDriver_->streams_[kConnectionStreamId].error,
            HTTP3::ErrorCode::HTTP_MISSING_SETTINGS);
}

TEST_P(HQUpstreamSessionTestH1qv1, TestConnectionClose) {
  hqSession_->drain();
  auto handler = openTransaction();
  handler->txn_->sendHeaders(getGetRequest());
  handler->txn_->sendEOM();
  handler->expectHeaders();
  handler->expectBody();
  handler->expectEOM();
  handler->expectDetachTransaction();
  auto resp = makeResponse(200, 100);
  std::get<0>(resp)->getHeaders().set(HTTP_HEADER_CONNECTION, "close");
  sendResponse(handler->txn_->getID(),
               *std::get<0>(resp),
               std::move(std::get<1>(resp)),
               true);
  hqSession_->closeWhenIdle();
  flushAndLoop();
}

/**
 * Instantiate the Parametrized test cases
 */

// Make sure all the tests keep working with all the supported protocol versions
INSTANTIATE_TEST_CASE_P(HQUpstreamSessionTest,
                        HQUpstreamSessionTest,
                        Values(TestParams({.alpn_ = "h1q-fb"}),
                               TestParams({.alpn_ = "h1q-fb-v2"}),
                               TestParams({.alpn_ = "h3"})),
                        paramsToTestName);

// Instantiate h1 only tests
INSTANTIATE_TEST_CASE_P(HQUpstreamSessionTest,
                        HQUpstreamSessionTestH1q,
                        Values(TestParams({.alpn_ = "h1q-fb"}),
                               TestParams({.alpn_ = "h1q-fb-v2"})),
                        paramsToTestName);

// Instantiate h1q-fb-v2 and hq only tests (goaway tests)
INSTANTIATE_TEST_CASE_P(HQUpstreamSessionTest,
                        HQUpstreamSessionTestH1qv2HQ,
                        Values(TestParams({.alpn_ = "h1q-fb-v2"}),
                               TestParams({.alpn_ = "h3"})),
                        paramsToTestName);

// Instantiate h1q-fb-v1 only tests
INSTANTIATE_TEST_CASE_P(HQUpstreamSessionTest,
                        HQUpstreamSessionTestH1qv1,
                        Values(TestParams({.alpn_ = "h1q-fb"})),
                        paramsToTestName);

// Instantiate h1q-fb-v2 only tests
INSTANTIATE_TEST_CASE_P(HQUpstreamSessionTest,
                        HQUpstreamSessionTestH1qv2,
                        Values(TestParams({.alpn_ = "h1q-fb-v2"})),
                        paramsToTestName);

// Instantiate hq only tests
INSTANTIATE_TEST_CASE_P(HQUpstreamSessionTest,
                        HQUpstreamSessionTestHQ,
                        Values(TestParams({.alpn_ = "h3"})),
                        paramsToTestName);
