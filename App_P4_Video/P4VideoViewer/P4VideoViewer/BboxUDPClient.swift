import Foundation
import Network
import os

/// Byte-aligned little-endian readers (ARM64 requires aligned loads).
private func readU16LE(_ data: Data, offset: Int) -> UInt16 {
  UInt16(data[offset]) | (UInt16(data[offset + 1]) << 8)
}

private func readU32LE(_ data: Data, offset: Int) -> UInt32 {
  UInt32(data[offset])
    | (UInt32(data[offset + 1]) << 8)
    | (UInt32(data[offset + 2]) << 16)
    | (UInt32(data[offset + 3]) << 24)
}

/// Receives AI bounding box metadata from ESP32-P4 on UDP port 3335.
/// Parse-only — no heartbeat, no chunking. Fire-and-forget from firmware side.
final class BboxUDPClient: @unchecked Sendable {
  struct Detection: Identifiable {
    let id = UUID()
    let classId: UInt8
    let confidence: UInt8
    let x1: UInt16, y1: UInt16, x2: UInt16, y2: UInt16
    let label: String
  }

  struct BboxFrame {
    let frameId: UInt32
    let timestamp: UInt32
    let detections: [Detection]
  }

  var onDetectionsReceived: ((BboxFrame) -> Void)?

  private let logger = Logger(subsystem: "com.gata.p4videoviewer", category: "bbox")
  private let queue = DispatchQueue(label: "bbox-udp", qos: .userInitiated)
  private var listener: NWListener?

  /// Start listening for bbox packets on the given port.
  func startListening(port: UInt16) {
    stopListening()

    let params = NWParameters.udp
    params.allowLocalEndpointReuse = true

    guard let nwPort = NWEndpoint.Port(rawValue: port) else {
      logger.error("Invalid bbox port: \(port)")
      return
    }

    do {
      listener = try NWListener(using: params, on: nwPort)
    } catch {
      logger.error("Failed to create bbox listener: \(error)")
      return
    }

    listener?.newConnectionHandler = { [weak self] connection in
      connection.start(queue: self?.queue ?? .main)
      self?.receiveLoop(connection)
    }

    listener?.stateUpdateHandler = { [weak self] state in
      switch state {
      case .ready:
        self?.logger.info("Bbox listener ready on port \(port)")
      case .failed(let error):
        self?.logger.error("Bbox listener failed: \(error)")
      default:
        break
      }
    }

    listener?.start(queue: queue)
  }

  func stopListening() {
    listener?.cancel()
    listener = nil
  }

  // MARK: - Private

  private func receiveLoop(_ connection: NWConnection) {
    connection.receiveMessage { [weak self] data, _, _, error in
      if let error {
        self?.logger.warning("Bbox recv error: \(error)")
        return
      }
      if let data {
        self?.parseBboxPacket(data)
      }
      // Continue receiving
      self?.receiveLoop(connection)
    }
  }

  /// Parse a binary BBOX packet from the firmware.
  private func parseBboxPacket(_ data: Data) {
    // Header: "BBOX" (4) + frame_id (4) + timestamp (4) + num_detections (1) = 13 bytes
    guard data.count >= 13 else { return }

    let magic = String(data: data[0..<4], encoding: .ascii) ?? ""
    guard magic == "BBOX" else { return }

    let frameId = readU32LE(data, offset: 4)
    let timestamp = readU32LE(data, offset: 8)
    let numDetections = data[12]

    // Each detection: 26 bytes
    let detectionSize = 26
    let expectedSize = 13 + Int(numDetections) * detectionSize
    guard data.count >= expectedSize else { return }

    var detections: [Detection] = []
    detections.reserveCapacity(Int(numDetections))

    for i in 0..<Int(numDetections) {
      let o = 13 + i * detectionSize

      let classId = data[o]
      let confidence = data[o + 1]
      let x1 = readU16LE(data, offset: o + 2)
      let y1 = readU16LE(data, offset: o + 4)
      let x2 = readU16LE(data, offset: o + 6)
      let y2 = readU16LE(data, offset: o + 8)
      let labelLen = min(Int(data[o + 10]), 15)
      let labelData = data[(o + 11)..<(o + 11 + labelLen)]
      let label = String(data: labelData, encoding: .utf8) ?? "unknown"

      detections.append(
        Detection(
          classId: classId,
          confidence: confidence,
          x1: x1, y1: y1, x2: x2, y2: y2,
          label: label
        ))
    }

    let frame = BboxFrame(
      frameId: frameId,
      timestamp: timestamp,
      detections: detections
    )

    onDetectionsReceived?(frame)
  }
}
