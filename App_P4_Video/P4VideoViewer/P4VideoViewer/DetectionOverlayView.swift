import SwiftUI

/// Draws bounding boxes and labels from AI detections on top of the video.
///
/// Coordinates arrive in source frame space (1280x720) and are scaled to
/// match the aspect-fit video display using the same transform.
struct DetectionOverlayView: View {
  let detections: [BboxUDPClient.Detection]
  let sourceWidth: CGFloat = 1280
  let sourceHeight: CGFloat = 720

  var body: some View {
    GeometryReader { geo in
      let fitted = aspectFitRect(
        source: CGSize(width: sourceWidth, height: sourceHeight),
        in: geo.size
      )
      let scaleX = fitted.width / sourceWidth
      let scaleY = fitted.height / sourceHeight

      ForEach(detections) { det in
        let x = fitted.minX + CGFloat(det.x1) * scaleX
        let y = fitted.minY + CGFloat(det.y1) * scaleY
        let w = CGFloat(det.x2 - det.x1) * scaleX
        let h = CGFloat(det.y2 - det.y1) * scaleY
        let color = colorForLabel(det.label)

        // Bounding box rectangle
        Rectangle()
          .stroke(color, lineWidth: 2)
          .frame(width: max(w, 1), height: max(h, 1))
          .position(x: x + w / 2, y: y + h / 2)

        // Label above box
        Text("\(det.label) \(det.confidence)%")
          .font(.system(size: 11, weight: .semibold))
          .foregroundColor(.white)
          .padding(.horizontal, 4)
          .padding(.vertical, 1)
          .background(color.opacity(0.75))
          .cornerRadius(3)
          .position(x: x + w / 2, y: max(y - 10, 6))
      }
    }
    .allowsHitTesting(false)
  }

  /// Compute the aspect-fit rectangle for the video inside the display bounds.
  private func aspectFitRect(source: CGSize, in bounds: CGSize) -> CGRect {
    let scale = min(bounds.width / source.width, bounds.height / source.height)
    let w = source.width * scale
    let h = source.height * scale
    return CGRect(
      x: (bounds.width - w) / 2,
      y: (bounds.height - h) / 2,
      width: w,
      height: h
    )
  }

  /// Color coding per detection class.
  private func colorForLabel(_ label: String) -> Color {
    switch label {
    case "face": return .green
    case "person": return .blue
    case "cat": return .orange
    case "dog": return .brown
    case "hand": return .yellow
    default: return .purple
    }
  }
}
