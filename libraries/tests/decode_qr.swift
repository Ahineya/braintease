// Independent scanner for the rendered terminal image; macOS test tool only.
import Foundation
import CoreImage

let context = CIContext(options: [.useSoftwareRenderer: true])
guard let detector = CIDetector(ofType: CIDetectorTypeQRCode, context: context,
                              options: [CIDetectorAccuracy: CIDetectorAccuracyHigh]),
      let image = CIImage(contentsOf: URL(fileURLWithPath: CommandLine.arguments[1])) else {
    fputs("QR decoder could not open image\n", stderr)
    exit(1)
}
let values = detector.features(in: image).compactMap { ($0 as? CIQRCodeFeature)?.messageString }
guard values.count == 1, values[0] == CommandLine.arguments[2] else {
    fputs("QR decoder mismatch: \(values)\n", stderr)
    exit(1)
}
print("Independent QR scan matched payload")
