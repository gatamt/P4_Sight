import Foundation
import Network
import os

/// UDP video client — handles VID0/BEAT/PAWS/GONE protocol and reassembles chunked H.264 frames.
final class UDPVideoClient: @unchecked Sendable {
  private let logger = Logger(subsystem: "com.gata.p4videoviewer", category: "udp")

  /// 28-byte chunk header — must match firmware h264_chunk_header_t exactly.
  struct H264Header {
    let frameId: UInt32
    let width: UInt16
    let height: UInt16
    let timestamp: UInt32
    let totalLen: UInt32
    let chunkIdx: UInt16
    let chunkCount: UInt16
    let frameType: UInt32  // 1 = keyframe
    let reserved: UInt32

    static let size = 28

    init?(data: Data) {
      guard data.count >= Self.size else { return nil }
      var offset = 0
      func read<T: FixedWidthInteger>() -> T {
        let value = data.withUnsafeBytes {
          $0.loadUnaligned(fromByteOffset: offset, as: T.self)
        }
        offset += MemoryLayout<T>.size
        return T(littleEndian: value)
      }
      frameId = read()
      width = read()
      height = read()
      timestamp = read()
      totalLen = read()
      chunkIdx = read()
      chunkCount = read()
      frameType = read()
      reserved = read()
    }

    var isKeyframe: Bool { frameType == 1 }
  }

  // MARK: - Connection state

  private var connection: NWConnection?
  private let queue = DispatchQueue(label: "com.gata.udpvideo", qos: .userInteractive)
  private var heartbeatTimer: DispatchSourceTimer?
  private var registrationAttempts: UInt32 = 0
  private var isRegistered = false

  // MARK: - Frame reassembly state

  private var currentFrameId: UInt32 = UInt32.max
  private var frameBuffer = Data()
  private var receivedChunks = Set<UInt16>()
  private var expectedChunkCount: UInt16 = 0
  private var currentIsKeyframe = false

  // MARK: - Statistics

  private var totalPackets: UInt64 = 0
  private var totalFrames: UInt64 = 0
  private var assemblyTimeouts: UInt64 = 0

  // MARK: - Callbacks

  /// Called when a complete H.264 frame is reassembled.
  var onFrameReady: ((_ h264Data: Data, _ isKeyframe: Bool) -> Void)?

  /// Called when VACK is received (registration confirmed by firmware).
  var onRegistered: (() -> Void)?

  // MARK: - Connection lifecycle

  func connect(host: String, port: UInt16) {
    disconnect()

    let endpoint = NWEndpoint.hostPort(
      host: NWEndpoint.Host(host),
      port: NWEndpoint.Port(rawValue: port)!
    )
    let params = NWParameters.udp
    let conn = NWConnection(to: endpoint, using: params)
    self.connection = conn

    conn.stateUpdateHandler = { [weak self] state in
      guard let self else { return }
      switch state {
      case .ready:
        self.logger.info("UDP connected to \(host):\(port)")
        self.startVID0RegistrationLoop()
        self.receiveLoop()
      case .failed(let error):
        self.logger.error("UDP connection failed: \(error.localizedDescription)")
      case .cancelled:
        self.logger.info("UDP connection cancelled")
      default:
        break
      }
    }

    conn.start(queue: queue)
  }

  func disconnect() {
    sendControl("GONE")
    stopHeartbeat()
    isRegistered = false
    registrationAttempts = 0
    connection?.cancel()
    connection = nil
  }

  /// Pause streaming — sends PAWS and stops heartbeat.
  func pause() {
    sendControl("PAWS")
    stopHeartbeat()
    logger.info("Sent PAWS — heartbeat stopped")
  }

  /// Resume streaming — re-sends VID0 to handle firmware IDLE timeout.
  func resume() {
    guard connection != nil else { return }
    /* Always send VID0 on resume — firmware may have timed out to IDLE
       while app was backgrounded, and BEAT is ignored in IDLE state. */
    sendControl("VID0")
    startHeartbeatTimer()
    logger.info("Sent VID0 — resuming stream")
  }

  // MARK: - Control messages

  /// Send a 4-byte ASCII control message to the firmware.
  private func sendControl(_ msg: String) {
    guard let conn = connection, let data = msg.data(using: .ascii) else { return }
    conn.send(
      content: data,
      completion: .contentProcessed { [weak self] error in
        if let error {
          self?.logger.error("\(msg) send failed: \(error.localizedDescription)")
        }
      })
  }

  // MARK: - VID0 registration

  private func startVID0RegistrationLoop() {
    stopHeartbeat()
    registrationAttempts = 0
    isRegistered = false

    let timer = DispatchSource.makeTimerSource(queue: queue)
    timer.schedule(deadline: .now(), repeating: 1.0)
    timer.setEventHandler { [weak self] in
      guard let self, self.connection != nil else {
        self?.stopHeartbeat()
        return
      }

      self.registrationAttempts += 1
      self.sendControl("VID0")
      self.logger.info("VID0 registration sent (attempt \(self.registrationAttempts))")

      if self.registrationAttempts >= 10 {
        self.stopHeartbeat()
      }
    }
    heartbeatTimer = timer
    timer.resume()
  }

  // MARK: - Heartbeat

  private func startHeartbeatTimer() {
    stopHeartbeat()

    let timer = DispatchSource.makeTimerSource(queue: queue)
    timer.schedule(deadline: .now() + 1.0, repeating: 1.0)
    timer.setEventHandler { [weak self] in
      self?.sendControl("BEAT")
    }
    heartbeatTimer = timer
    timer.resume()
  }

  private func stopHeartbeat() {
    heartbeatTimer?.cancel()
    heartbeatTimer = nil
  }

  // MARK: - Receive loop

  private func receiveLoop() {
    guard let conn = connection else { return }
    conn.receiveMessage { [weak self] content, _, isComplete, error in
      guard let self else { return }

      if let error {
        self.logger.error("Receive error: \(error.localizedDescription)")
        return
      }

      if let data = content {
        self.processPacket(data)
      }

      self.receiveLoop()
    }
  }

  private func processPacket(_ data: Data) {
    /* Check for 4-byte control messages (VACK) */
    if data.count == 4, let msg = String(data: data, encoding: .ascii) {
      if msg == "VACK" {
        logger.info("VACK received — registration confirmed")
        isRegistered = true
        startHeartbeatTimer()
        onRegistered?()
        return
      }
    }

    totalPackets += 1

    /* Legacy fallback: stop VID0 retries on first video packet if no VACK */
    if !isRegistered {
      isRegistered = true
      startHeartbeatTimer()
      logger.info("First video packet — switching to heartbeat (no VACK)")
      onRegistered?()
    }

    guard let header = H264Header(data: data) else {
      logger.warning("Packet too small for header: \(data.count) bytes")
      return
    }

    let payload = data.dropFirst(H264Header.size)

    /* New frame detected — reset assembly state */
    if header.frameId != currentFrameId {
      if currentFrameId != UInt32.max && receivedChunks.count < Int(expectedChunkCount) {
        assemblyTimeouts += 1
        if assemblyTimeouts % 100 == 1 {
          logger.warning(
            "Frame \(self.currentFrameId) incomplete: \(self.receivedChunks.count)/\(self.expectedChunkCount) chunks"
          )
        }
      }

      currentFrameId = header.frameId
      expectedChunkCount = header.chunkCount
      currentIsKeyframe = header.isKeyframe
      frameBuffer = Data(capacity: Int(header.totalLen))
      frameBuffer = Data(repeating: 0, count: Int(header.totalLen))
      receivedChunks.removeAll()
    }

    /* Place chunk at correct offset in frame buffer */
    let chunkOffset = Int(header.chunkIdx) * 1400
    let copyLen = min(payload.count, frameBuffer.count - chunkOffset)
    if copyLen > 0 {
      frameBuffer.replaceSubrange(
        chunkOffset..<(chunkOffset + copyLen),
        with: payload.prefix(copyLen))
    }
    receivedChunks.insert(header.chunkIdx)

    /* Check if frame is complete */
    if receivedChunks.count == Int(expectedChunkCount) {
      let trimmedData = frameBuffer.prefix(Int(header.totalLen))
      totalFrames += 1

      if totalFrames % 300 == 0 {
        logger.info(
          "Frame \(self.totalFrames): \(trimmedData.count) bytes \(self.currentIsKeyframe ? "[IDR]" : "")"
        )
      }

      onFrameReady?(Data(trimmedData), currentIsKeyframe)
    }
  }
}
