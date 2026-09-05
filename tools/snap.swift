// Minimal webcam stills capture for the calibration loop.
//
// Built with the system swiftc so nothing has to be installed. macOS will ask
// for camera permission the first time; that prompt is attributed to whichever
// app launched this.
//
//   ./build-snap.sh    (a bare binary will NOT get camera access — see that script)
//   ./snap out.jpg [warmupSeconds]
//
// Note: exposure and white-balance cannot be driven manually here.
// AVCaptureDevice's exposureTargetBias and related controls are iOS-only; on
// macOS the built-in camera exposes automatically and that is that. If the
// panel comes out underexposed, the fix is physical — fill more of the frame
// with it, and keep bright windows out of shot.
//
// The warm-up matters: laptop webcams need a second or two of frames before
// auto-exposure and auto-white-balance settle, and a photo taken before then
// is useless as a colour reference.

import AVFoundation
import Foundation
import CoreImage

let args = CommandLine.arguments
guard args.count >= 2 else {
    FileHandle.standardError.write("usage: snap <output.jpg> [warmupSeconds]\n".data(using: .utf8)!)
    exit(2)
}
let outPath = args[1]
let warmup = args.count >= 3 ? (Double(args[2]) ?? 2.0) : 2.0

func fail(_ msg: String) -> Never {
    FileHandle.standardError.write("snap: \(msg)\n".data(using: .utf8)!)
    exit(1)
}

// Camera permission
let sem = DispatchSemaphore(value: 0)
var granted = false
AVCaptureDevice.requestAccess(for: .video) { ok in granted = ok; sem.signal() }
if sem.wait(timeout: .now() + 30) == .timedOut { fail("timed out waiting for camera permission") }
if !granted { fail("camera access denied — grant it in System Settings > Privacy & Security > Camera") }

guard let device = AVCaptureDevice.default(for: .video) else { fail("no camera found") }

let session = AVCaptureSession()
session.sessionPreset = .photo

guard let input = try? AVCaptureDeviceInput(device: device), session.canAddInput(input) else {
    fail("cannot open camera input")
}
session.addInput(input)

let output = AVCaptureVideoDataOutput()
output.videoSettings = [kCVPixelBufferPixelFormatTypeKey as String: kCVPixelFormatType_32BGRA]
output.alwaysDiscardsLateVideoFrames = true

final class Grabber: NSObject, AVCaptureVideoDataOutputSampleBufferDelegate {
    var latest: CVPixelBuffer?
    let lock = NSLock()
    func captureOutput(_ o: AVCaptureOutput, didOutput sampleBuffer: CMSampleBuffer,
                       from c: AVCaptureConnection) {
        guard let pb = CMSampleBufferGetImageBuffer(sampleBuffer) else { return }
        lock.lock(); latest = pb; lock.unlock()
    }
}
let grabber = Grabber()
output.setSampleBufferDelegate(grabber, queue: DispatchQueue(label: "snap.capture"))
guard session.canAddOutput(output) else { fail("cannot add camera output") }
session.addOutput(output)

session.startRunning()

Thread.sleep(forTimeInterval: warmup)   // let AE/AWB settle

grabber.lock.lock()
let frame = grabber.latest
grabber.lock.unlock()
session.stopRunning()

guard let pixelBuffer = frame else { fail("no frame captured") }

let ciImage = CIImage(cvPixelBuffer: pixelBuffer)
let context = CIContext()
guard let colorSpace = CGColorSpace(name: CGColorSpace.sRGB) else { fail("no sRGB colour space") }

let url = URL(fileURLWithPath: outPath)
do {
    if outPath.lowercased().hasSuffix(".png") {
        try context.writePNGRepresentation(of: ciImage, to: url,
                                           format: .RGBA8, colorSpace: colorSpace)
    } else {
        try context.writeJPEGRepresentation(of: ciImage, to: url,
                                            colorSpace: colorSpace,
                                            options: [kCGImageDestinationLossyCompressionQuality as CIImageRepresentationOption: 0.95])
    }
} catch {
    fail("failed to write \(outPath): \(error)")
}

let w = CVPixelBufferGetWidth(pixelBuffer)
let h = CVPixelBufferGetHeight(pixelBuffer)
print("captured \(w)x\(h) -> \(outPath)")
