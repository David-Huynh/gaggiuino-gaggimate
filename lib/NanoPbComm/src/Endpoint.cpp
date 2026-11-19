#include "Endpoint.h"
#include <cstring>
#if defined(ARDUINO_ARCH_STM32)
#define ESP_LOGE(tag, fmt, ...) ((void)0)
#define ESP_LOGW(tag, fmt, ...) ((void)0)
#else
#include <esp_log.h>
#endif
#if !defined(ARDUINO_ARCH_STM32)
#include <esp_system.h>
#endif
#include <pb_decode.h>
#include <pb_encode.h>

static constexpr const char *ENDPOINT_TAG = "Endpoint";

Endpoint::Endpoint(Transport &transport) : _transport(transport) {
    _mutex = xSemaphoreCreateRecursiveMutex();
    _rxQueue = xQueueCreate(RX_QUEUE_DEPTH, sizeof(DispatchEvent));
    if (_mutex == nullptr || _rxQueue == nullptr)
        ESP_LOGE(ENDPOINT_TAG, "Failed to allocate endpoint resources (out of memory)");
}

Endpoint::~Endpoint() {
    // Detach first so the transport can't invoke our (this-capturing) callbacks
    // while/after we tear down the task, queue, and mutex.
    _transport.onData(nullptr);
    _transport.onConnectionChange(nullptr);
    if (_dispatchTask)
        vTaskDelete(_dispatchTask);
    if (_rxQueue)
        vQueueDelete(_rxQueue);
    if (_mutex)
        vSemaphoreDelete(_mutex);
}

void Endpoint::begin() {
    if (_mutex == nullptr || _rxQueue == nullptr) {
        ESP_LOGE(ENDPOINT_TAG, "Endpoint resources missing; not starting (comms disabled)");
        return;
    }
    _transport.onData([this](const uint8_t *data, size_t length) { handleData(data, length); });
    _transport.onConnectionChange([this](bool connected) { handleConnection(connected); });
    if (_dispatchTask == nullptr &&
        xTaskCreate(dispatchTaskFn, "GmDispatch", DISPATCH_STACK, this, 1, &_dispatchTask) != pdPASS) {
        _dispatchTask = nullptr;
        ESP_LOGE(ENDPOINT_TAG, "Failed to create dispatch task; inbound messages will not be processed");
    }
}

void Endpoint::dispatchTaskFn(void *arg) {
    auto *self = static_cast<Endpoint *>(arg);
    DispatchEvent event;
    for (;;) {
        if (xQueueReceive(self->_rxQueue, &event, portMAX_DELAY) != pdTRUE)
            continue;
        self->processDispatchEvent(event);
    }
}

void Endpoint::dispatch(const gm::Payload &payload) {
    const pb_size_t which = payload.which_content;
    if (which < HANDLER_SLOTS && _handlers[which])
        _handlers[which](payload);
}

void Endpoint::processDispatchEvent(DispatchEvent const &event) {
    if (event.generation != _dispatchGeneration.load()) return;
    if (event.isConnection) {
        _applicationConnected.store(event.connected);
        if (_connHandler) _connHandler(event.connected);
    } else dispatch(event.payload);
}

void Endpoint::loop() { pump(); }

bool Endpoint::isIdle() {
    lock();
    const bool idle = _queue.empty() && !_inFlight;
    unlock();
    return idle;
}

void Endpoint::on(pb_size_t which, Handler handler) {
    if (which < HANDLER_SLOTS)
        _handlers[which] = std::move(handler);
}

void Endpoint::send(const gm::Payload &payload) { send(payload, gm_proto::defaultPriority(payload.which_content)); }

void Endpoint::send(const gm::Payload &payload, uint8_t priority) {
    lock();
    const bool queued = _queue.upsert(gm_proto::coalescingKey(payload), priority, payload);
    unlock();
    if (!queued)
        ESP_LOGW(ENDPOINT_TAG, "Outbound queue full, dropped payload %u", static_cast<unsigned>(payload.which_content));
    requestPump();
}

void Endpoint::sendBatch(const gm::Payload *payloads, size_t count) {
    if (payloads == nullptr || count == 0)
        return;
    lock();
    bool queued = true;
    for (size_t i = 0; i < count; i++)
        queued &= _queue.upsert(gm_proto::coalescingKey(payloads[i]), gm_proto::defaultPriority(payloads[i].which_content),
                                payloads[i]);
    unlock();
    if (!queued)
        ESP_LOGW(ENDPOINT_TAG, "Outbound queue full, dropped part of a batch");
    requestPump();
}

void Endpoint::sendUrgentBatch(const gm::Payload *payloads, size_t count) {
    if (payloads == nullptr || count == 0)
        return;
    const size_t frameCount = count > MAX_PAYLOADS_PER_FRAME ? MAX_PAYLOADS_PER_FRAME : count;

    lock();

    // The transport preserves write order. Abandoning the previous frame means
    // it cannot be retransmitted after this one; the receiver also rejects any
    // late lower frame id as a duplicate. Remove queued values for the same
    // control keys so stale state cannot follow the urgent transition.
    for (pb_size_t i = 0; i < _inFlightPayloadCount; i++) {
        const uint16_t oldKey = gm_proto::coalescingKey(_inFlightPayloads[i]);
        bool superseded = false;
        for (size_t j = 0; j < count; j++) {
            if (gm_proto::coalescingKey(payloads[j]) == oldKey) {
                superseded = true;
                break;
            }
        }
        if (!superseded) {
            _queue.upsert(oldKey, gm_proto::defaultPriority(_inFlightPayloads[i].which_content), _inFlightPayloads[i]);
        }
    }
    _inFlight = false;
    _inFlightPayloadCount = 0;
    _retries = 0;
    _txLen = 0;
    for (size_t i = 0; i < count; i++)
        _queue.invalidate(gm_proto::coalescingKey(payloads[i]));

    if (!_transport.isConnected() || !_sessionReady) {
        for (size_t i = 0; i < count; i++)
            _queue.upsert(gm_proto::coalescingKey(payloads[i]), gm_proto::PRIO_HIGH, payloads[i]);
        unlock();
        return;
    }

    memset(&_txFrame, 0, sizeof(_txFrame));
    _txFrame.session_id = _localSession;
    _txFrame.peer_session_id = _peerSession;
    _txFrame.id = _nextId++;
    if (_nextId == 0)
        _nextId = 1;
    _txFrame.payloads_count = static_cast<pb_size_t>(frameCount);
    for (size_t i = 0; i < frameCount; i++)
        _txFrame.payloads[i] = payloads[i];

    if (!encodeFrame(_txFrame, _txBuf, frameLimit(), &_txLen)) {
        ESP_LOGE(ENDPOINT_TAG, "Failed to encode urgent outbound frame (%u payloads)", static_cast<unsigned>(frameCount));
        for (size_t i = 0; i < count; i++)
            _queue.upsert(gm_proto::coalescingKey(payloads[i]), gm_proto::PRIO_HIGH, payloads[i]);
        unlock();
        requestPump();
        return;
    }

    for (size_t i = frameCount; i < count; i++)
        _queue.upsert(gm_proto::coalescingKey(payloads[i]), gm_proto::PRIO_HIGH, payloads[i]);
    _transport.send(_txBuf, _txLen);
    _inFlight = true;
    _inFlightPayloadCount = static_cast<pb_size_t>(frameCount);
    for (size_t i = 0; i < frameCount; i++)
        _inFlightPayloads[i] = payloads[i];
    _inFlightId = _txFrame.id;
    _sentAt = millis();
    unlock();
}

void Endpoint::sendUnreliable(const gm::Payload &payload) { sendUnreliable(&payload, 1); }

void Endpoint::sendUnreliable(const gm::Payload *payloads, size_t count) {
    if (payloads == nullptr || count == 0 || !_transport.isConnected() || !_applicationConnected.load())
        return;
    if (count > MAX_PAYLOADS_PER_FRAME)
        count = MAX_PAYLOADS_PER_FRAME;

    lock();
    // id == 0 => the peer will not ACK and we never retransmit. Build into the
    // dedicated _unrelBuf so an in-flight reliable frame (_txBuf) is untouched.
    memset(&_txFrame, 0, sizeof(_txFrame));
    _txFrame.id = 0;
    _txFrame.ack = 0;
    _txFrame.session_id = _localSession;
    _txFrame.peer_session_id = _peerSession;
    _txFrame.payloads_count = static_cast<pb_size_t>(count);
    for (size_t i = 0; i < count; i++)
        _txFrame.payloads[i] = payloads[i];
    size_t len = 0;
    if (encodeFrame(_txFrame, _unrelBuf, frameLimit(), &len))
        _transport.send(_unrelBuf, len);
    unlock();
}

size_t Endpoint::frameLimit() const {
    const size_t limit = _transport.maxDatagram();
    return limit < BUFFER_SIZE ? limit : BUFFER_SIZE;
}

bool Endpoint::encodeFrame(const gm::Frame &frame, uint8_t *buf, size_t bufSize, size_t *outLen) {
    pb_ostream_t os = pb_ostream_from_buffer(buf, bufSize);
    if (!pb_encode(&os, &gaggimate_Frame_msg, &frame))
        return false;
    *outLen = os.bytes_written;
    return true;
}

void Endpoint::sendAck(uint32_t id) {
    gm::Frame frame = gaggimate_Frame_init_zero;
    frame.session_id = _localSession;
    frame.peer_session_id = _peerSession;
    frame.id = 0; // ACKs are never themselves acknowledged
    frame.ack = id;
    frame.payloads_count = 0;
    uint8_t buf[32];
    size_t len = 0;
    if (encodeFrame(frame, buf, sizeof(buf), &len))
        _transport.send(buf, len);
}

// A fixed timeout shorter than the real round trip resends every frame that did arrive and overloads a slow link.
unsigned long Endpoint::ackTimeoutLocked() const {
    unsigned long timeout = ACK_TIMEOUT_MIN_MS;
    if (_rttValid && _smoothedRttMs * ACK_TIMEOUT_RTT_FACTOR > timeout)
        timeout = _smoothedRttMs * ACK_TIMEOUT_RTT_FACTOR;
    timeout <<= _timeoutShift;
    return timeout > ACK_TIMEOUT_MAX_MS ? ACK_TIMEOUT_MAX_MS : timeout;
}

// With a pump task registered the caller only wakes it; otherwise pump on the calling thread as before.
void Endpoint::requestPump() {
    if (_pumpTask != nullptr)
        xTaskNotifyGive(_pumpTask);
    else
        pump();
}

void Endpoint::pump() {
    if (!_transport.isConnected())
        return;

    lock();
    const bool dropped = pumpLocked();
    unlock();
    if (dropped && _sendFailedHandler)
        _sendFailedHandler(); // mutex released, so the handler may call send()
}

// Returns true when the in-flight frame ran out of retries and was dropped.
bool Endpoint::pumpLocked() {
    const unsigned long now = millis();
    if (!_sessionReady) {
        if (now - _lastHelloMs >= HELLO_INTERVAL_MS) sendHello();
        return false;
    }
    bool dropped = false;

    if (_inFlight) {
        if (now - _sentAt < ackTimeoutLocked())
            return false; // still waiting for ACK
        if (_retries >= MAX_RETRIES) {
            // Give up and free the slot; the send-failed handler lets the application re-send its state.
            _inFlight = false;
            dropped = true;
            _inFlightPayloadCount = 0;
        } else {
            _transport.send(_txBuf, _txLen);
            _sentAt = now;
            _retries++;
            _retransmits++;
            if (_timeoutShift < ACK_TIMEOUT_MAX_SHIFT)
                _timeoutShift++; // stays raised for the next frames until one is ACKed cleanly
            return false;
        }
    }

    // Idle: drain the highest-priority entries into one frame.
    if (_queue.empty())
        return dropped;

    memset(&_txFrame, 0, sizeof(_txFrame));
    pb_size_t count = 0;
    while (count < MAX_PAYLOADS_PER_FRAME) {
        auto entry = _queue.pop();
        if (!entry)
            break;
        _txFrame.payloads[count++] = entry->payload;
    }
    _txFrame.payloads_count = count;
    _txFrame.ack = 0;
    _txFrame.session_id = _localSession;
    _txFrame.peer_session_id = _peerSession;
    _txFrame.id = _nextId++;
    if (_nextId == 0)
        _nextId = 1;

    // Over the transport limit: push the lowest-priority payloads back for the next frame.
    bool encoded;
    while (!(encoded = encodeFrame(_txFrame, _txBuf, frameLimit(), &_txLen)) && count > 1) {
        const gm::Payload &last = _txFrame.payloads[--count];
        _queue.upsert(gm_proto::coalescingKey(last), gm_proto::defaultPriority(last.which_content), last);
        _txFrame.payloads_count = count;
    }
    if (!encoded) {
        // An individual payload over the current link limit must not starve the controls behind it.
        ESP_LOGE(ENDPOINT_TAG, "Outbound payload exceeds datagram budget");
        return true;
    }

    _transport.send(_txBuf, _txLen);
    _inFlight = true;
    _inFlightPayloadCount = count;
    for (pb_size_t i = 0; i < count; i++)
        _inFlightPayloads[i] = _txFrame.payloads[i];
    _inFlightId = _txFrame.id;
    _sentAt = now;
    _retries = 0;
    return dropped;
}

void Endpoint::handleData(const uint8_t *data, size_t length) {
    memset(&_rxFrame, 0, sizeof(_rxFrame));
    pb_istream_t is = pb_istream_from_buffer(data, length);
    if (!pb_decode(&is, &gaggimate_Frame_msg, &_rxFrame)) {
        ESP_LOGW(ENDPOINT_TAG, "Failed to decode frame (%u bytes): %s", static_cast<unsigned>(length), PB_GET_ERROR(&is));
        return;
    }

    // A physical UART link can stay alive while one board reboots. Sequence
    // counters alone cannot identify that event or bind an old ACK to a boot.
    const bool hello = _rxFrame.id == 0 && _rxFrame.ack == 0 && _rxFrame.payloads_count == 0;
    lock();
    const uint32_t source = _rxFrame.session_id;
    const uint32_t destination = _rxFrame.peer_session_id;
    if (source == 0) {
        // Legacy INFO is read-only and still useful for an OTA mismatch screen.
        if (_rxQueue) for (pb_size_t i = 0; i < _rxFrame.payloads_count; ++i) {
            if (_rxFrame.payloads[i].which_content != gaggimate_Payload_system_info_tag) continue;
            DispatchEvent event;
            event.generation = _dispatchGeneration.load();
            event.payload = _rxFrame.payloads[i];
            xQueueSend(_rxQueue, &event, 0);
        }
        unlock();
        return;
    }
    if ((destination != 0 && destination != _localSession) ||
        (source != _peerSession && (!hello || (_sessionReady && destination != 0)))) {
        sendHello();
        unlock();
        return;
    }
    bool reply = false;
    if (hello && destination == 0) {
        if (_sessionReady || (_peerSession != 0 && _peerSession != source)) {
            _localSession = freshSessionId();
            resetSession();
            queueConnection(false);
        }
        _peerSession = source;
        reply = true;
    } else if (hello && source != _peerSession) {
        _peerSession = source;
        reply = true;
    }
    if (destination == _localSession && source == _peerSession && !_sessionReady) {
        _sessionReady = true;
        queueConnection(true);
        reply = true;
    }
    if (reply) sendHello();
    const bool ready = _sessionReady && source == _peerSession && destination == _localSession;
    unlock();
    if (!ready || hello) return;

    const uint32_t id = _rxFrame.id;
    const uint32_t ack = _rxFrame.ack;

    bool duplicate = false;
    lock();
    if (ack != 0 && _inFlight && ack == _inFlightId) {
        // Sample RTT only when the frame was ACKed without a retransmit -- after
        // a retransmit we can't tell which copy this ACK answers (Karn's rule).
        if (_retries == 0) {
            const uint32_t rtt = static_cast<uint32_t>(millis() - _sentAt);
            _lastRttMs = rtt;
            // Rise fast, fall slow: the round trip jumps ~5x when the link relaxes to its idle interval after a process.
            if (!_rttValid || rtt > _smoothedRttMs)
                _smoothedRttMs = _rttValid ? (_smoothedRttMs + rtt) / 2 : rtt;
            else
                _smoothedRttMs = (_smoothedRttMs * 7 + rtt) / 8;
            _rttValid = true;
            _timeoutShift = 0; // a clean sample: the estimate is trustworthy again
        }
        _inFlight = false;
        _inFlightPayloadCount = 0;
    }
    if (id != 0 && id <= _lastRxId)
        duplicate = true; // retransmit of an already-processed frame
    unlock();

    if (id != 0 && duplicate) {
        sendAck(id); // peer's previous ACK was lost; re-ack without re-processing
        requestPump();
        return;
    }

    // Hand the payloads to the dispatch task rather than running handlers on the
    // transport (BLE) thread. Only ACK + advance the de-dup cursor once every
    // payload is safely queued; otherwise leave the frame un-ACKed so the sender
    // retransmits once the dispatch task has caught up (back-pressure).
    bool accepted = true;
    const pb_size_t n = _rxFrame.payloads_count;
    if (n > 0) {
        if (static_cast<pb_size_t>(uxQueueSpacesAvailable(_rxQueue)) < n) {
            accepted = false;
        } else {
            for (pb_size_t i = 0; i < n; i++) {
                DispatchEvent event;
                event.generation = _dispatchGeneration.load();
                event.payload = _rxFrame.payloads[i];
                xQueueSend(_rxQueue, &event, 0);
            }
        }
    }

    if (accepted) {
        if (id != 0) {
            lock();
            _lastRxId = id;
            unlock();
            sendAck(id);
        }
    }

    // A received ACK may have freed the in-flight slot; send the next frame now.
    requestPump();
}

uint32_t Endpoint::freshSessionId() {
#if defined(ARDUINO_ARCH_STM32)
    // Session identity, not authentication. Hardware timing + device identity
    // distinguish boots; a peer hello also rotates the still-running ESP's RNG
    // nonce, invalidating frames addressed to the previous session.
    static uint32_t sequence = 0;
    uint32_t value = micros() ^ HAL_GetUIDw0() ^ HAL_GetUIDw1() ^ (++sequence * 0x9e3779b9u);
#else
    uint32_t value = esp_random();
#endif
    return value == 0 ? 1 : value;
}

void Endpoint::sendHello() {
    gm::Frame frame = gaggimate_Frame_init_zero;
    frame.session_id = _localSession;
    frame.peer_session_id = _peerSession;
    uint8_t bytes[16];
    size_t length = 0;
    if (encodeFrame(frame, bytes, sizeof(bytes), &length)) _transport.send(bytes, length);
    _lastHelloMs = millis();
}

void Endpoint::queueConnection(bool connected) {
    if (!_rxQueue) return;
    DispatchEvent event;
    event.isConnection = true;
    event.connected = connected;
    event.generation = _dispatchGeneration.load();
    xQueueSend(_rxQueue, &event, 0);
}

void Endpoint::resetSession() {
    _sessionReady = false;
    _applicationConnected.store(false);
    _dispatchGeneration.fetch_add(1);
    _inFlight = false;
    _inFlightPayloadCount = 0;
    _retries = 0;
    _timeoutShift = 0;
    _txLen = 0;
    _inFlightId = 0;
    _lastRxId = 0;
    _nextId = 1;
    _rttValid = false;
    _smoothedRttMs = _lastRttMs = 0;
    _queue.clear();
    if (_rxQueue) xQueueReset(_rxQueue);
}

void Endpoint::handleConnection(bool connected) {
    lock();
    _localSession = freshSessionId();
    _peerSession = 0;
    resetSession();
    queueConnection(false);
    if (connected) sendHello();
    unlock();
}
