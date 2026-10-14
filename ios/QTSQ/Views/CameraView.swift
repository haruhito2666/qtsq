import SwiftUI
import AVFoundation
import UIKit

struct CameraView: View {
    @EnvironmentObject var engine: QtsqEngine
    var onCaptured: (URL) -> Void
    var onClose: () -> Void

    @State private var isCapturing = false
    @State private var capturedImage: UIImage?
    @State private var showPreview = false

    var body: some View {
        ZStack {
            // Camera preview
            CameraPreviewView()
                .ignoresSafeArea()

            VStack {
                // Top bar
                HStack {
                    Button(action: onClose) {
                        Image(systemName: "xmark")
                            .font(.system(size: QIconSize.lg, weight: .medium))
                            .foregroundColor(.white)
                            .frame(width: 44, height: 44)
                            .background(Color.black.opacity(0.4))
                            .clipShape(Circle())
                    }
                    Spacer()
                    Text("Camera")
                        .font(QFont.title())
                        .foregroundColor(.white)
                    Spacer()
                    Color.clear.frame(width: 44, height: 44) // Balance
                }
                .padding(.horizontal, QSpacing.xl)
                .padding(.top, QSpacing.xxl)

                Spacer()

                // Capture button
                VStack(spacing: QSpacing.lg) {
                    Text("Direct to .qtsq")
                        .font(QFont.bodySmall())
                        .foregroundColor(.white.opacity(0.8))

                    Button(action: capturePhoto) {
                        ZStack {
                            Circle()
                                .stroke(Color.white, lineWidth: 4)
                                .frame(width: 80, height: 80)
                            Circle()
                                .fill(isCapturing ? QColor.accentPrimary : Color.white)
                                .frame(width: 68, height: 68)
                            if isCapturing {
                                ProgressView()
                                    .tint(.white)
                                    .scaleEffect(1.2)
                            }
                        }
                    }
                    .disabled(isCapturing)
                }
                .padding(.bottom, 60)
            }
        }
        .background(Color.black)
    }

    private func capturePhoto() {
        isCapturing = true

        // Simulate capture — in a real implementation, this would use AVCaptureSession
        // For now, create a test pattern image and compress it
        DispatchQueue.global(qos: .userInitiated).async {
            let w = 640
            let h = 480
            var rgbData = Data(count: w * h * 3)
            rgbData.withUnsafeMutableBytes { buf in
                guard let ptr = buf.baseAddress?.assumingMemoryBound(to: UInt8.self) else { return }
                for y in 0..<h {
                    for x in 0..<w {
                        let idx = (y * w + x) * 3
                        ptr[idx] = UInt8(x % 256)
                        ptr[idx + 1] = UInt8(y % 256)
                        ptr[idx + 2] = UInt8((x + y) % 256)
                    }
                }
            }

            if let compressed = engine.compressImage(pixels: rgbData, width: w, height: h, channels: 3) {
                let dir = FileManager.default.urls(for: .documentDirectory, in: .userDomainMask).first!
                let outURL = dir.appendingPathComponent("QTSQ_\(Int(Date().timeIntervalSince1970)).qtsq")
                try? compressed.write(to: outURL)

                DispatchQueue.main.async {
                    isCapturing = false
                    onCaptured(outURL)
                }
            } else {
                DispatchQueue.main.async {
                    isCapturing = false
                }
            }
        }
    }
}

// MARK: - Camera Preview (AVCaptureSession wrapper)

struct CameraPreviewView: UIViewRepresentable {
    func makeUIView(context: Context) -> UIView {
        let view = UIView(frame: .zero)
        view.backgroundColor = .black

        // Set up the camera session
        let session = AVCaptureSession()
        session.sessionPreset = .photo

        if let device = AVCaptureDevice.default(.builtInWideAngleCamera, for: .video, position: .back),
           let input = try? AVCaptureDeviceInput(device: device) {
            session.addInput(input)

            let previewLayer = AVCaptureVideoPreviewLayer(session: session)
            previewLayer.videoGravity = .resizeAspectFill
            view.layer.addSublayer(previewLayer)

            DispatchQueue.global(qos: .userInitiated).async {
                session.startRunning()
            }

            // Store session reference
            context.coordinator.session = session
            context.coordinator.previewLayer = previewLayer
        }

        return view
    }

    func updateUIView(_ uiView: UIView, context: Context) {
        context.coordinator.previewLayer?.frame = uiView.bounds
    }

    func makeCoordinator() -> Coordinator {
        Coordinator()
    }

    class Coordinator {
        var session: AVCaptureSession?
        var previewLayer: AVCaptureVideoPreviewLayer?
    }
}
