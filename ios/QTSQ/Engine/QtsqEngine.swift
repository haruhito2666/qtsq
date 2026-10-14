import Foundation
import UIKit

// MARK: - QTSQ Data Types

enum QtsqDataType: Int {
    case raw         = 0x00
    case imageGray   = 0x10
    case imageRGB    = 0x11
    case imageRGBA   = 0x12
    case audioMono   = 0x20
    case audioStereo = 0x21
    case audioMulti  = 0x22
    case videoFrames = 0x30
    case textASCII   = 0x40
    case textUTF8    = 0x41
    case tableInt    = 0x50
    case tableFloat  = 0x51
    case json        = 0x52
    case csv         = 0x53

    var isImage: Bool { rawValue >= 0x10 && rawValue <= 0x12 }
    var isAudio: Bool { rawValue >= 0x20 && rawValue <= 0x22 }
    var isText: Bool { rawValue >= 0x40 && rawValue <= 0x53 }

    var label: String {
        switch self {
        case .raw: return "Raw Binary"
        case .imageGray: return "Grayscale Image"
        case .imageRGB: return "RGB Image"
        case .imageRGBA: return "RGBA Image"
        case .audioMono: return "Mono Audio"
        case .audioStereo: return "Stereo Audio"
        case .audioMulti: return "Multi-Channel Audio"
        case .videoFrames: return "Video"
        case .textASCII: return "ASCII Text"
        case .textUTF8: return "UTF-8 Text"
        case .tableInt: return "Integer Table"
        case .tableFloat: return "Float Table"
        case .json: return "JSON Data"
        case .csv: return "CSV Data"
        }
    }
}

// MARK: - Decompression Result

struct QtsqResult {
    let data: Data
    let type: QtsqDataType
    let originalSize: Int
    let width: Int
    let height: Int
    let audioSampleRate: Int
    let encoding: String
    let vertexCount: Int
    let faceCount: Int
    let splatCount: Int
}

// MARK: - Engine Wrapper

final class QtsqEngine: ObservableObject {
    @Published var useGPU: Bool = true

    // MARK: - Compress Raw Bytes

    func compressBytes(_ data: Data, type: String, password: String? = nil) -> Data? {
        var ctx = qtsq_context_t()
        guard qtsq_init(&ctx) == 0 else { return nil }
        defer { qtsq_free(&ctx) }

        let typeHint: qtsq_data_type_t = {
            switch type {
            case "image": return QTSQ_TYPE_IMAGE_RGB
            case "audio": return QTSQ_TYPE_AUDIO_MONO
            default:      return QTSQ_TYPE_RAW
            }
        }()

        let result = data.withUnsafeBytes { buf -> Int32 in
            guard let ptr = buf.baseAddress?.assumingMemoryBound(to: UInt8.self) else { return -1 }
            let res = qtsq_compress_raw(&ctx, ptr, buf.count, 0)
            if res == 0 {
                ctx.header.data_type = UInt8(typeHint.rawValue)
            }
            return res
        }
        guard result == 0 else { return nil }

        if let pw = password, !pw.isEmpty {
            guard qtsq_encrypt(&ctx, pw) == 0 else { return nil }
        }

        return writeContextToData(&ctx)
    }

    // MARK: - Compress Image

    func compressImage(pixels: Data, width: Int, height: Int, channels: Int, password: String? = nil) -> Data? {
        var ctx = qtsq_context_t()
        guard qtsq_init(&ctx) == 0 else { return nil }
        defer { qtsq_free(&ctx) }

        let result = pixels.withUnsafeBytes { buf -> Int32 in
            guard let ptr = buf.baseAddress?.assumingMemoryBound(to: UInt8.self) else { return -1 }
            return qtsq_compress_gravitational_hd(&ctx, ptr, UInt32(width), UInt32(height), UInt8(channels), 0)
        }
        guard result == 0 else { return nil }

        if let pw = password, !pw.isEmpty {
            guard qtsq_encrypt(&ctx, pw) == 0 else { return nil }
        }

        return writeContextToData(&ctx)
    }

    // MARK: - Compress Image Upscale

    func compressImageUpscale(pixels: Data, width: Int, height: Int, channels: Int, password: String? = nil, sourceSize: Int = 0) -> Data? {
        var ctx = qtsq_context_t()
        guard qtsq_init(&ctx) == 0 else { return nil }
        defer { qtsq_free(&ctx) }

        let result = pixels.withUnsafeBytes { buf -> Int32 in
            guard let ptr = buf.baseAddress?.assumingMemoryBound(to: UInt8.self) else { return -1 }
            return qtsq_compress_gravitational_upscale(&ctx, ptr, UInt32(width), UInt32(height), UInt8(channels), sourceSize)
        }
        guard result == 0 else { return nil }

        if let pw = password, !pw.isEmpty {
            guard qtsq_encrypt(&ctx, pw) == 0 else { return nil }
        }

        return writeContextToData(&ctx)
    }

    // MARK: - Compress Audio

    func compressAudio(pcm: [Int16], sampleRate: Int, channels: Int, password: String? = nil) -> Data? {
        var ctx = qtsq_context_t()
        guard qtsq_init(&ctx) == 0 else { return nil }
        defer { qtsq_free(&ctx) }

        let result = pcm.withUnsafeBufferPointer { buf -> Int32 in
            guard let ptr = buf.baseAddress else { return -1 }
            return qtsq_compress_audio(&ctx, ptr, pcm.count, UInt32(sampleRate), UInt8(channels))
        }
        guard result == 0 else { return nil }

        if let pw = password, !pw.isEmpty {
            guard qtsq_encrypt(&ctx, pw) == 0 else { return nil }
        }

        return writeContextToData(&ctx)
    }

    // MARK: - Decompress

    func decompress(_ data: Data, password: String? = nil) -> QtsqResult? {
        var ctx = qtsq_context_t()
        guard qtsq_init(&ctx) == 0 else { return nil }
        defer { qtsq_free(&ctx) }

        // Write data to temp file, then read
        let tempURL = FileManager.default.temporaryDirectory.appendingPathComponent("temp_\(UUID().uuidString).qtsq")
        do {
            try data.write(to: tempURL)
        } catch { return nil }
        defer { try? FileManager.default.removeItem(at: tempURL) }

        guard qtsq_read(&ctx, tempURL.path) == 0 else { return nil }

        // Decrypt if needed
        if let pw = password, !pw.isEmpty {
            guard qtsq_decrypt(&ctx, pw) == 0 else { return nil }
        }

        // Decompress
        var outData: UnsafeMutablePointer<UInt8>?
        var outSize: Int = 0
        guard qtsq_decompress(&ctx, &outData, &outSize) == 0, let ptr = outData else { return nil }
        defer { free(ptr) }

        let resultData = Data(bytes: ptr, count: outSize)
        let dataType = QtsqDataType(rawValue: Int(ctx.header.data_type)) ?? .raw
        let ogSize = Int(ctx.header.original_size)
        
        let width = Int(ctx.schema.dimensions.0)
        let height = Int(ctx.schema.dimensions.1)
        let sampleRate = Int(ctx.schema.sample_rate)
        
        var encodingBuffer = ctx.schema.encoding
        let encString = withUnsafePointer(to: &encodingBuffer.0) { ptr in
            String(cString: ptr)
        }
        
        // C structs don't directly expose mesh/splat counts at context level currently,
        // so we default to 0 for these metrics (they can be inferred from payload processing later)
        let vertexCnt = 0
        let faceCnt = 0
        let splatCnt = 0

        return QtsqResult(
            data: resultData,
            type: dataType,
            originalSize: ogSize,
            width: width,
            height: height,
            audioSampleRate: sampleRate,
            encoding: encString,
            vertexCount: vertexCnt,
            faceCount: faceCnt,
            splatCount: splatCnt
        )
    }

    // MARK: - Helpers

    private func writeContextToData(_ ctx: inout qtsq_context_t) -> Data? {
        let tempURL = FileManager.default.temporaryDirectory.appendingPathComponent("out_\(UUID().uuidString).qtsq")

        // Phase 1.5: encrypt before writing (fail-closed contract)
        if qtsq_is_encrypted(&ctx) == 0 {
            // Not yet encrypted — prompt for password via UIAlertController
            // In iOS, this must be called from the main thread with a view controller.
            // For programmatic use (background export), use a default password from
            // Keychain. Here we use a simple prompt approach.
            let alert = UIAlertController(title: "Encryption Required",
                                          message: "Enter a password to encrypt this file:",
                                          preferredStyle: .alert)
            alert.addTextField { tf in
                tf.isSecureTextEntry = true
                tf.placeholder = "Password"
            }
            alert.addAction(UIAlertAction(title: "Cancel", style: .cancel))
            alert.addAction(UIAlertAction(title: "Encrypt", style: .default) { _ in
                // Note: this is synchronous within the alert handler
                // For proper async flow, refactor to use a completion handler
            })

            // For non-UI contexts (background tasks), auto-encrypt with
            // a device-derived key. The proper iOS flow would use a
            // delegate/callback to get the password from the user.
            // As a fail-closed fallback: refuse to write without encryption.
            return nil
        }

        guard qtsq_write(&ctx, tempURL.path) == 0 else { return nil }
        defer { try? FileManager.default.removeItem(at: tempURL) }
        return try? Data(contentsOf: tempURL)
    }
}

// MARK: - String from C buffer

private extension String {
    init(cString buffer: UnsafePointer<CChar>, maxLength: Int) {
        let data = Data(bytes: buffer, count: maxLength)
        if let idx = data.firstIndex(of: 0) {
            self = String(data: data[..<idx], encoding: .utf8) ?? ""
        } else {
            self = String(data: data, encoding: .utf8) ?? ""
        }
    }
}
