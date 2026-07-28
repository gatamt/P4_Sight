import CoreImage
import SwiftUI

/// Fullscreen video view — no menu, no controls, just video + small status overlay.
struct ContentView: View {
  @EnvironmentObject private var appState: AppState
  @StateObject private var viewModel = VideoViewModel()
  @Environment(\.scenePhase) private var scenePhase

  var body: some View {
    ZStack {
      Color.black.ignoresSafeArea()

      if let frame = viewModel.currentFrame {
        GeometryReader { geo in
          Image(decorative: frame, scale: 1.0)
            .resizable()
            .aspectRatio(contentMode: .fit)
            .frame(width: geo.size.width, height: geo.size.height)
        }
      }

      // AI bounding box overlay
      DetectionOverlayView(detections: viewModel.currentDetections)

      VStack {
        Spacer()
        HStack {
          statusBadge
          Spacer()
        }
        .padding(.horizontal, 16)
        .padding(.bottom, 8)
      }
    }
    .statusBarHidden(true)
    .onAppear {
      viewModel.start(appState: appState)
    }
    .onDisappear {
      viewModel.stop()
    }
    .onChange(of: scenePhase) { newPhase in
      switch newPhase {
      case .active:
        viewModel.handleForeground()
      case .background:
        viewModel.handleBackground()
      case .inactive:
        break  // Transient state (app switcher, Control Center) — ignore
      @unknown default:
        break
      }
    }
  }

  private var statusBadge: some View {
    VStack(alignment: .leading, spacing: 2) {
      Text(appState.connectionState.rawValue)
        .font(.caption2)
        .fontWeight(.medium)

      if !appState.statusMessage.isEmpty {
        Text(appState.statusMessage)
          .font(.caption2)
      }

      if let error = appState.lastError {
        Text(error)
          .font(.caption2)
          .foregroundColor(.red)
      }

      if appState.connectionState == .streaming {
        Text("Frames: \(appState.frameCount)")
          .font(.caption2)
        Text("Detections: \(appState.detectionCount)")
          .font(.caption2)
      }
    }
    .foregroundColor(.white)
    .padding(6)
    .background(.black.opacity(0.5))
    .cornerRadius(6)
  }
}

/// Coordinates the discovery -> UDP -> decode -> display pipeline.
@MainActor
final class VideoViewModel: ObservableObject {
  @Published var currentFrame: CGImage?
  @Published var currentDetections: [BboxUDPClient.Detection] = []

  private let discovery = BonjourDiscoveryManager()
  private let hotspotProbe = HotspotProbeDiscoveryManager()
  private let udpClient = UDPVideoClient()
  private let bboxClient = BboxUDPClient()
  private let decoder = HardwareH264Decoder()
  private let ciContext = CIContext()
  private var appState: AppState?
  private var isRunning = false
  private var didResolveEndpoint = false
  /// Track the state before pause so we know whether to resume.
  private var stateBeforePause: AppState.ConnectionState?

  func start(appState: AppState) {
    guard !isRunning else { return }
    isRunning = true
    self.appState = appState
    didResolveEndpoint = false

    /* Wire up the pipeline: discovery -> UDP -> decode -> display */
    discovery.onServiceResolved = { [weak self] host, port in
      guard let self else { return }
      Task { @MainActor in
        self.connectToResolvedHost(host: host, port: port, source: "bonjour")
      }
    }

    discovery.onServiceLost = { [weak self] in
      guard let self else { return }
      Task { @MainActor in
        self.appState?.updateStatus(.reconnecting)
        self.didResolveEndpoint = false
        self.udpClient.disconnect()
        self.bboxClient.stopListening()
        self.decoder.stop()
        self.currentFrame = nil
        self.currentDetections = []
        self.discovery.startBrowsing()
        self.hotspotProbe.startProbing(port: 3334)
      }
    }

    hotspotProbe.onProbeUpdate = { [weak self] message in
      guard let self else { return }
      Task { @MainActor in
        guard !self.didResolveEndpoint else { return }
        self.appState?.updateStatus(.probing, message: message)
      }
    }

    hotspotProbe.onHostDetected = { [weak self] host, port in
      guard let self else { return }
      Task { @MainActor in
        self.connectToResolvedHost(host: host, port: port, source: "probe")
      }
    }

    udpClient.onFrameReady = { [weak self] h264Data, isKeyframe in
      guard let self else { return }
      Task { @MainActor in
        if self.appState?.connectionState == .registering {
          self.appState?.updateStatus(.waitingForKeyframe)
        }
        if isKeyframe && self.appState?.connectionState == .waitingForKeyframe {
          self.appState?.updateStatus(.streaming)
        }
        self.appState?.frameCount += 1
      }
      self.decoder.decode(h264Data: h264Data, isKeyframe: isKeyframe)
    }

    udpClient.onRegistered = { [weak self] in
      guard let self else { return }
      Task { @MainActor in
        if self.appState?.connectionState == .registering {
          self.appState?.updateStatus(.waitingForKeyframe)
        }
      }
    }

    /* Wire up bbox listener */
    bboxClient.onDetectionsReceived = { [weak self] bboxFrame in
      guard let self else { return }
      Task { @MainActor in
        self.currentDetections = bboxFrame.detections
        self.appState?.detectionCount += UInt64(bboxFrame.detections.count)
      }
    }

    decoder.onFrameDecoded = { [weak self] ciImage in
      guard let self else { return }
      let cgImage = self.ciContext.createCGImage(ciImage, from: ciImage.extent)
      Task { @MainActor in
        self.currentFrame = cgImage
      }
    }

    appState.updateStatus(.discovering)
    discovery.startBrowsing()
    hotspotProbe.startProbing(port: 3334)

    /* Start bbox listener immediately — firmware sends when AI detects */
    bboxClient.startListening(port: 3335)
  }

  func stop() {
    isRunning = false
    discovery.stopBrowsing()
    hotspotProbe.stopProbing()
    udpClient.disconnect()
    bboxClient.stopListening()
    decoder.stop()
    currentDetections = []
  }

  /// App went to background — pause streaming, invalidate decoder session.
  func handleBackground() {
    guard isRunning, didResolveEndpoint else { return }
    stateBeforePause = appState?.connectionState
    appState?.updateStatus(.paused)
    udpClient.pause()
    decoder.stop()
    currentDetections = []
  }

  /// App returned to foreground — resume streaming (decoder recreates on next IDR).
  func handleForeground() {
    guard isRunning, didResolveEndpoint else { return }
    guard appState?.connectionState == .paused else { return }
    appState?.updateStatus(.waitingForKeyframe, message: "Resuming...")
    udpClient.resume()
  }

  private func connectToResolvedHost(host: String, port: UInt16, source: String) {
    guard !didResolveEndpoint else { return }

    didResolveEndpoint = true
    appState?.updateStatus(.registering, message: "\(source): \(host):\(port)")
    appState?.resolvedHost = host
    appState?.resolvedPort = port

    discovery.stopBrowsing()
    hotspotProbe.stopProbing()
    udpClient.connect(host: host, port: port)
  }
}
