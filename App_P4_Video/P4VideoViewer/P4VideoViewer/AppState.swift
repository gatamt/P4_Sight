import Combine
import Foundation
import os

/// Central app state — drives UI and coordinates discovery -> connection -> video flow.
@MainActor
final class AppState: ObservableObject {
  enum ConnectionState: String {
    case discovering = "Searching for P4..."
    case probing = "Probing hotspot..."
    case resolving = "Resolving service..."
    case registering = "Sending VID0..."
    case waitingForKeyframe = "Waiting for keyframe..."
    case streaming = "Streaming"
    case paused = "Paused"
    case reconnecting = "Reconnecting..."
    case error = "Error"
  }

  @Published var connectionState: ConnectionState = .discovering
  @Published var statusMessage: String = ""
  @Published var frameCount: UInt64 = 0
  @Published var detectionCount: UInt64 = 0
  @Published var lastError: String?

  let logger = Logger(subsystem: "com.gata.p4videoviewer", category: "app")

  /// Resolved P4 host and port from Bonjour.
  @Published var resolvedHost: String?
  @Published var resolvedPort: UInt16?

  func updateStatus(_ state: ConnectionState, message: String = "") {
    connectionState = state
    statusMessage = message
    logger.info("State: \(state.rawValue) \(message)")
  }

  func logError(_ message: String) {
    lastError = message
    logger.error("\(message)")
  }
}
