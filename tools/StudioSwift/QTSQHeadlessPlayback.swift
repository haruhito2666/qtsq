import Foundation

/// Headless playback verification for the QTSQStudio GUI path.
///
/// Reproduces, without a window or audio device, exactly what the app does
/// when the user double-clicks a vaulted audio file:
///   qtsq_read -> device-key decrypt (DeviceKeyStore) -> MDCT2 float decode
/// which is the flow SpectralAudioPlayer uses after ViewerView opens a file.
///
/// usage: qtsq_headless <file.qtsq>
/// exit 0 on success, 1 on any failure.

@main
struct QTSQHeadlessPlayback {
    static func encString(_ ctx: UnsafePointer<qtsq_context_t>) -> String {
        withUnsafePointer(to: ctx.pointee.schema.encoding) { ptr in
            ptr.withMemoryRebound(to: CChar.self, capacity: 64) { String(cString: $0) }
        }
    }

    static func main() {
        let args = CommandLine.arguments
        guard args.count == 2 else {
            FileHandle.standardError.write("usage: \(args[0]) <file.qtsq>\n".data(using: .utf8)!)
            exit(2)
        }
        let path = args[1]

        var ctx = qtsq_context_t()
        var rc = qtsq_init(&ctx)
        guard rc == QTSQ_OK else {
            print("FAIL qtsq_init: \(String(cString: qtsq_error_string(rc)))")
            exit(1)
        }

        rc = qtsq_read(&ctx, path)
        guard rc == QTSQ_OK else {
            print("FAIL qtsq_read: \(String(cString: qtsq_error_string(rc)))")
            exit(1)
        }

        let wasEncrypted = qtsq_is_encrypted(&ctx) != 0
        let deviceEncrypted = ctx.has_security != 0 && ctx.security.encryption_type == 2
        let type = qtsq_data_type_t(UInt32(ctx.header.data_type))
        let isAudio = type == QTSQ_TYPE_AUDIO_MONO || type == QTSQ_TYPE_AUDIO_STEREO
            || type == QTSQ_TYPE_AUDIO_MULTI

        print("open: encrypted=\(wasEncrypted) deviceKey=\(deviceEncrypted) " +
              "type=0x\(String(format: "%02x", ctx.header.data_type)) encoding=\(encString(&ctx))")

        if wasEncrypted {
            guard deviceEncrypted else {
                print("FAIL encrypted with a passphrase; the app would prompt (not device-unlockable)")
                exit(1)
            }
            guard DeviceKeyStore.shared.isAvailable else {
                print("FAIL device keypair unavailable")
                exit(1)
            }
            rc = DeviceKeyStore.shared.decryptWithDeviceKey(&ctx)
            guard rc == QTSQ_OK else {
                print("FAIL device decrypt: \(String(cString: qtsq_error_string(rc)))")
                exit(1)
            }
            print("decrypt: device key OK")
        }

        /* Encoding travels inside the vault envelope (ZERO_META): it is only
         * restored by the decrypt, exactly as Models.swift inspects it after
         * unlocking the file. */
        let encoding = encString(&ctx)
        let isMdct2Float = isAudio && ctx.header.strategy == 0x01 && ctx.header.sample_depth == 32
            && encoding == "RAW_FLOAT32"
        guard isMdct2Float else {
            print("FAIL not an MDCT2 float stream; the player would take another path")
            exit(1)
        }

        var pcm: UnsafeMutablePointer<Float>? = nil
        var count = 0
        var sr: UInt32 = 0
        var ch: UInt8 = 0
        rc = qtsq_decompress_audio_float(&ctx, &pcm, &count, &sr, &ch)
        guard rc == QTSQ_OK, let samples = pcm, count > 0 else {
            print("FAIL float decode: \(String(cString: qtsq_error_string(rc)))")
            exit(1)
        }

        var peak: Float = 0
        var sumSq: Double = 0
        for i in 0..<count {
            let v = samples[i]
            peak = max(peak, abs(v))
            sumSq += Double(v) * Double(v)
        }
        let rms = sqrt(sumSq / Double(count))
        let frames = count / max(1, Int(ch))
        let seconds = sr > 0 ? Double(frames) / Double(sr) : 0
        let kbps = seconds > 0 ? Double(ctx.singularity_size) * 8.0 / seconds / 1000.0 : 0

        print(String(format: "decode: %d samples, %u Hz, %u ch, %.1f s, %.0f kbps, peak %.3f, rms %.4f",
                     count, sr, ch, seconds, kbps, peak, rms))
        free(samples)
        qtsq_free(&ctx)

        guard count > 0, seconds > 1.0, peak > 0.01, rms > 0.001 else {
            print("FAIL decoded audio is empty or silent")
            exit(1)
        }
        print("PASS headless playback path")
    }
}
