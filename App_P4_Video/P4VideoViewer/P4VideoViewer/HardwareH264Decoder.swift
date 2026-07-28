import AVFoundation
import CoreImage
import Foundation
import VideoToolbox
import os

/// Hardware H.264 decoder using VideoToolbox on iPhone.
/// Decodes H.264 NALUs from the P4 encoder into displayable CIImage frames.
final class HardwareH264Decoder: @unchecked Sendable {
  private let logger = Logger(subsystem: "com.gata.p4videoviewer", category: "decoder")

  private var formatDescription: CMVideoFormatDescription?
  private var decompressionSession: VTDecompressionSession?
  private var spsData: Data?
  private var ppsData: Data?

  /// Called with each decoded frame (CIImage) on an arbitrary thread.
  var onFrameDecoded: ((_ image: CIImage) -> Void)?

  private var decodeFailures: UInt64 = 0

  /// Feed a complete H.264 frame (one or more NALUs) for decoding.
  func decode(h264Data: Data, isKeyframe: Bool) {
    /* Extract SPS/PPS from keyframes to create format description */
    if isKeyframe {
      extractParameterSets(from: h264Data)
    }

    guard formatDescription != nil else {
      return /* Wait for first keyframe with SPS/PPS */
    }

    /* Convert Annex-B start codes to AVCC length-prefixed format */
    guard let avccData = annexBToAVCC(h264Data) else {
      return
    }

    /* Create sample buffer and decode */
    var blockBuffer: CMBlockBuffer?
    avccData.withUnsafeBytes { rawBuf in
      guard let baseAddr = rawBuf.baseAddress else { return }
      CMBlockBufferCreateWithMemoryBlock(
        allocator: kCFAllocatorDefault,
        memoryBlock: UnsafeMutableRawPointer(mutating: baseAddr),
        blockLength: avccData.count,
        blockAllocator: kCFAllocatorNull,
        customBlockSource: nil,
        offsetToData: 0,
        dataLength: avccData.count,
        flags: 0,
        blockBufferOut: &blockBuffer
      )
    }

    guard let block = blockBuffer else { return }

    var sampleBuffer: CMSampleBuffer?
    var sampleSize = avccData.count
    CMSampleBufferCreateReady(
      allocator: kCFAllocatorDefault,
      dataBuffer: block,
      formatDescription: formatDescription,
      sampleCount: 1,
      sampleTimingEntryCount: 0,
      sampleTimingArray: nil,
      sampleSizeEntryCount: 1,
      sampleSizeArray: &sampleSize,
      sampleBufferOut: &sampleBuffer
    )

    guard let sample = sampleBuffer else { return }

    ensureSession()
    guard let session = decompressionSession else { return }

    var flagsOut = VTDecodeInfoFlags()
    let status = VTDecompressionSessionDecodeFrame(
      session,
      sampleBuffer: sample,
      flags: [._EnableAsynchronousDecompression],
      frameRefcon: nil,
      infoFlagsOut: &flagsOut
    )

    if status != noErr {
      decodeFailures += 1
      if decodeFailures % 50 == 1 {
        logger.error("Decode failed: \(status) (failures: \(self.decodeFailures))")
      }
    }
  }

  func stop() {
    if let session = decompressionSession {
      VTDecompressionSessionInvalidate(session)
      decompressionSession = nil
    }
    formatDescription = nil
    spsData = nil
    ppsData = nil
  }

  // MARK: - Private

  /// Find and extract SPS and PPS NAL units from Annex-B bytestream.
  private func extractParameterSets(from data: Data) {
    let nalus = findNALUnits(in: data)
    for nalu in nalus {
      let naluType = nalu[0] & 0x1F
      if naluType == 7 { /* SPS */
        spsData = nalu
      } else if naluType == 8 { /* PPS */
        ppsData = nalu
      }
    }

    if let sps = spsData, let pps = ppsData, formatDescription == nil {
      createFormatDescription(sps: sps, pps: pps)
    }
  }

  private func createFormatDescription(sps: Data, pps: Data) {
    var desc: CMVideoFormatDescription?

    let status = sps.withUnsafeBytes { spsPtr -> OSStatus in
      pps.withUnsafeBytes { ppsPtr -> OSStatus in
        let parameterSetPointers: [UnsafePointer<UInt8>] = [
          spsPtr.baseAddress!.assumingMemoryBound(to: UInt8.self),
          ppsPtr.baseAddress!.assumingMemoryBound(to: UInt8.self),
        ]
        let parameterSetSizes: [Int] = [sps.count, pps.count]

        return CMVideoFormatDescriptionCreateFromH264ParameterSets(
          allocator: kCFAllocatorDefault,
          parameterSetCount: 2,
          parameterSetPointers: parameterSetPointers,
          parameterSetSizes: parameterSetSizes,
          nalUnitHeaderLength: 4,
          formatDescriptionOut: &desc
        )
      }
    }

    if status == noErr, let desc {
      formatDescription = desc
      /* Invalidate old session so a new one is created with updated format */
      if let session = decompressionSession {
        VTDecompressionSessionInvalidate(session)
        decompressionSession = nil
      }
      logger.info("Format description created from SPS/PPS")
    } else {
      logger.error("Failed to create format description: \(status)")
    }
  }

  private func ensureSession() {
    guard decompressionSession == nil, let fmt = formatDescription else { return }

    let attrs: [String: Any] = [
      kCVPixelBufferPixelFormatTypeKey as String: kCVPixelFormatType_32BGRA
    ]

    var session: VTDecompressionSession?
    let callback = VTDecompressionOutputCallbackRecord(
      decompressionOutputCallback: { refcon, _, status, _, imageBuffer, _, _ in
        guard status == noErr, let pixelBuffer = imageBuffer else { return }
        let decoder = Unmanaged<HardwareH264Decoder>.fromOpaque(refcon!).takeUnretainedValue()
        let ciImage = CIImage(cvPixelBuffer: pixelBuffer)
        decoder.onFrameDecoded?(ciImage)
      },
      decompressionOutputRefCon: Unmanaged.passUnretained(self).toOpaque()
    )

    var callbackRecord = callback
    let status = VTDecompressionSessionCreate(
      allocator: kCFAllocatorDefault,
      formatDescription: fmt,
      decoderSpecification: nil,
      imageBufferAttributes: attrs as CFDictionary,
      outputCallback: &callbackRecord,
      decompressionSessionOut: &session
    )

    if status == noErr {
      decompressionSession = session
      logger.info("VideoToolbox decompression session created")
    } else {
      logger.error("Failed to create decompression session: \(status)")
    }
  }

  /// Find NAL unit boundaries in Annex-B data (00 00 00 01 or 00 00 01).
  private func findNALUnits(in data: Data) -> [Data] {
    var nalus: [Data] = []
    var i = 0
    let bytes = Array(data)
    var startPositions: [Int] = []

    while i < bytes.count - 3 {
      if bytes[i] == 0 && bytes[i + 1] == 0 {
        if bytes[i + 2] == 1 {
          startPositions.append(i + 3)
          i += 3
        } else if i < bytes.count - 4 && bytes[i + 2] == 0 && bytes[i + 3] == 1 {
          startPositions.append(i + 4)
          i += 4
        } else {
          i += 1
        }
      } else {
        i += 1
      }
    }

    for j in 0..<startPositions.count {
      let start = startPositions[j]
      let end =
        j + 1 < startPositions.count
        ? findStartCodeBefore(startPositions[j + 1], in: bytes)
        : bytes.count
      if start < end {
        nalus.append(Data(bytes[start..<end]))
      }
    }

    return nalus
  }

  /// Find the start code position before a given NALU start.
  private func findStartCodeBefore(_ pos: Int, in bytes: [UInt8]) -> Int {
    if pos >= 4 && bytes[pos - 4] == 0 && bytes[pos - 3] == 0
      && bytes[pos - 2] == 0 && bytes[pos - 1] == 1
    {
      return pos - 4
    }
    if pos >= 3 && bytes[pos - 3] == 0 && bytes[pos - 2] == 0
      && bytes[pos - 1] == 1
    {
      return pos - 3
    }
    return pos
  }

  /// Convert Annex-B (start code) format to AVCC (length-prefixed) format.
  private func annexBToAVCC(_ data: Data) -> Data? {
    let nalus = findNALUnits(in: data)
    guard !nalus.isEmpty else { return nil }

    var result = Data()
    for nalu in nalus {
      let naluType = nalu[0] & 0x1F
      /* Skip SPS (7) and PPS (8) from the stream — they're in format description */
      if naluType == 7 || naluType == 8 { continue }

      var length = UInt32(nalu.count).bigEndian
      result.append(Data(bytes: &length, count: 4))
      result.append(nalu)
    }

    return result.isEmpty ? nil : result
  }
}
