import SwiftUI
import UniformTypeIdentifiers

struct DecompressView: View {
    @EnvironmentObject var appState: AppState
    @State private var isTargeted = false
    @State private var errorMessage: String? = nil
    @State private var successMessage: String? = nil
    
    // File state
    @State private var loadedFilePath: String? = nil
    @State private var detectedTypeName: String = ""
    @State private var originalSize: UInt64 = 0
    @State private var compressedSize: UInt64 = 0
    @State private var selectedFormat: String = ""
    @State private var availableFormats: [String] = []
    @State private var dataCategory: DataCategory = .unknown
    @State private var isLoadingFile = false
    @State private var isExtracting = false
    @State private var videoExportNote: String? = nil
    
    enum DataCategory {
        case image, text, audio, video, binary, unknown
    }
    
    var body: some View {
        VStack(alignment: .leading, spacing: 24) {
            HStack(spacing: 8) {
                Image(systemName: "arrow.up.doc")
                    .font(.system(size: 15, weight: .semibold))
                    .foregroundColor(VaultTheme.emberOrange)
                
                Text("Decompress Stream")
                    .font(.system(size: 18, weight: .semibold))
                    .foregroundColor(VaultTheme.textPrimary)
            }
            .padding(.bottom, 4)
            
            Text("Drop a .qtsq file to decode the compressed stream back into raw media.")
                .font(.system(size: 13))
                .foregroundColor(VaultTheme.textSecondary)
            
            // Drop zone
            VStack(spacing: 12) {
                Image(systemName: "arrow.up.doc")
                    .font(.system(size: 32))
                    .foregroundColor(isTargeted ? VaultTheme.emberOrange : VaultTheme.textSecondary)
                
                Text(isLoadingFile ? "Reading stream…" : (loadedFilePath == nil ? "Drop .qtsq file here or click to browse" : "File loaded — choose target format below"))
                    .font(.system(size: 13, weight: .medium))
                    .foregroundColor(isTargeted ? VaultTheme.emberOrange : VaultTheme.textPrimary)
            }
            .frame(maxWidth: .infinity)
            .padding(.vertical, 44)
            .background(isTargeted ? VaultTheme.emberOrangeBg : VaultTheme.cardWhite)
            .cornerRadius(10)
            .overlay(
                RoundedRectangle(cornerRadius: 10, style: .continuous)
                    .stroke(isTargeted ? VaultTheme.emberOrange : VaultTheme.border, style: StrokeStyle(lineWidth: 1.5, dash: [6, 4]))
            )
            .shadow(color: VaultTheme.cardShadowColor, radius: 6, y: 2)
            .onDrop(of: ["public.file-url"], isTargeted: $isTargeted) { providers in
                if let provider = providers.first(where: { $0.canLoadObject(ofClass: URL.self) }) {
                    _ = provider.loadObject(ofClass: URL.self) { url, _ in
                        if let url = url, url.pathExtension.lowercased() == "qtsq" {
                            DispatchQueue.main.async {
                                loadFile(path: url.path)
                            }
                        }
                    }
                    return true
                }
                return false
            }
            
            // Browse button
            HStack {
                Spacer()
                Button(action: browseAction) {
                    Text("Browse...")
                        .font(.system(size: 15, weight: .semibold))
                        .foregroundColor(.white)
                        .padding(.horizontal, 32)
                        .padding(.vertical, 14)
                        .background(Color.teal)
                        .cornerRadius(24)
                        .shadow(color: Color.teal.opacity(0.3), radius: 8, x: 0, y: 4)
                }
                .buttonStyle(PlainButtonStyle())
                Spacer()
            }
            
            // Info card + format picker (shown after file is loaded)
            if loadedFilePath != nil {
                VStack(alignment: .leading, spacing: 16) {
                    // Info row
                    HStack(spacing: 20) {
                        InfoChip(label: "TYPE", value: detectedTypeName)
                        InfoChip(label: "ORIGINAL", value: formatBytes(originalSize))
                        InfoChip(label: "COMPRESSED", value: formatBytes(compressedSize))
                    }
                    
                    // Format picker
                    HStack(spacing: 16) {
                        Text("Output Format:")
                            .font(.system(size: 14, weight: .semibold))
                        
                        Picker("", selection: $selectedFormat) {
                            ForEach(availableFormats, id: \.self) { fmt in
                                Text(fmt.uppercased()).tag(fmt)
                            }
                        }
                        .pickerStyle(MenuPickerStyle())
                        .frame(width: 140)
                        
                        Spacer()
                        
                        Button(action: extractAction) {
                            HStack(spacing: 6) {
                                if isExtracting {
                                    ProgressView()
                                        .progressViewStyle(CircularProgressViewStyle(tint: .white))
                                        .controlSize(.small)
                                } else {
                                    Image(systemName: "square.and.arrow.down")
                                }
                                Text(isExtracting ? "Extracting…" : "Extract")
                            }
                            .font(.system(size: 14, weight: .semibold))
                            .foregroundColor(.white)
                            .padding(.horizontal, 24)
                            .padding(.vertical, 10)
                            .background(Color.blue)
                            .cornerRadius(20)
                            .shadow(color: Color.blue.opacity(0.3), radius: 6, y: 3)
                        }
                        .buttonStyle(PlainButtonStyle())
                        .disabled(isExtracting)
                    }
                }
                .padding(20)
                .background(.regularMaterial)
                .cornerRadius(16)
                .overlay(RoundedRectangle(cornerRadius: 16).stroke(Color.primary.opacity(0.05), lineWidth: 1))
            }
            
            if let success = successMessage {
                Text(success)
                    .foregroundColor(.white)
                    .padding(12)
                    .background(Color.green.opacity(0.8))
                    .cornerRadius(8)
                    .frame(maxWidth: .infinity, alignment: .center)
            }
            
            if let error = errorMessage {
                Text(error)
                    .foregroundColor(.white)
                    .padding(12)
                    .background(Color.red.opacity(0.8))
                    .cornerRadius(8)
                    .frame(maxWidth: .infinity, alignment: .center)
            }

            if let note = videoExportNote {
                Text(note)
                    .foregroundColor(.white)
                    .padding(12)
                    .background(Color.green.opacity(0.75))
                    .cornerRadius(8)
                    .frame(maxWidth: .infinity, alignment: .center)
            }
            
            Spacer()
        }
        .padding(40)
        .frame(maxWidth: .infinity, maxHeight: .infinity)
        .background(Color.clear)
    }
    
    // MARK: - Helpers
    
    private func formatBytes(_ bytes: UInt64) -> String {
        if bytes < 1024 { return "\(bytes) B" }
        if bytes < 1048576 { return String(format: "%.1f KB", Double(bytes) / 1024.0) }
        return String(format: "%.1f MB", Double(bytes) / 1048576.0)
    }
    
    private func browseAction() {
        let panel = NSOpenPanel()
        panel.allowsMultipleSelection = false
        panel.allowedContentTypes = [UTType(filenameExtension: "qtsq")!]
        if panel.runModal() == .OK, let url = panel.url {
            loadFile(path: url.path)
        }
    }
    
    // MARK: - Step 1: Load metadata
    
    private func loadFile(path: String) {
        guard !isLoadingFile else { return }
        self.errorMessage = nil
        self.videoExportNote = nil
        self.successMessage = nil
        self.loadedFilePath = nil
        self.isLoadingFile = true
        
        // qtsq_read pulls the whole compressed stream into memory — run it
        // off the main thread so dropping a large .qtsq never stalls the UI.
        DispatchQueue.global(qos: .userInitiated).async {
            var ctx = qtsq_context_t()
            qtsq_init(&ctx)
            defer { qtsq_free(&ctx) }
            
            let readRes = path.withCString { cPath -> Int32 in
                return qtsq_read(&ctx, cPath)
            }
            
            var error: String? = nil
            var typeName = ""
            var origSize: UInt64 = 0
            var compSize: UInt64 = 0
            var category = DataCategory.unknown
            var formats: [String] = []
            var defaultFormat = ""
            
            if readRes != QTSQ_OK {
                error = String(cString: qtsq_error_string(readRes))
            } else if qtsq_is_encrypted(&ctx) != 0 {
                error = "File is encrypted. Open in Viewer to decrypt it first."
            } else {
                let type = qtsq_data_type_t(UInt32(ctx.header.data_type))
                typeName = String(cString: qtsq_type_name(type))
                origSize = ctx.header.original_size
                compSize = ctx.header.singularity_size
                
                // Determine category and available formats
                if qtsq_type_is_image(type) != 0 {
                    category = .image
                    formats = ["png", "jpeg", "tiff", "bmp", "raw"]
                    defaultFormat = "png"
                } else if qtsq_type_is_text(type) != 0 {
                    category = .text
                    if type == QTSQ_TYPE_JSON {
                        formats = ["json", "txt"]
                        defaultFormat = "json"
                    } else if type == QTSQ_TYPE_CSV {
                        formats = ["csv", "txt"]
                        defaultFormat = "csv"
                    } else {
                        formats = ["txt", "json", "csv"]
                        defaultFormat = "txt"
                    }
                } else if qtsq_type_is_audio(type) != 0 {
                    category = .audio
                    formats = ["wav", "raw"]
                    defaultFormat = "wav"
                } else if type == QTSQ_TYPE_VIDEO_FRAMES {
                    category = .video
                    formats = ["png-frames", "wav"]
                    defaultFormat = "png-frames"
                } else {
                    category = .binary
                    formats = ["bin", "dat"]
                    defaultFormat = "bin"
                }
            }
            
            DispatchQueue.main.async {
                isLoadingFile = false
                if let error = error {
                    errorMessage = error
                    return
                }
                detectedTypeName = typeName
                originalSize = origSize
                compressedSize = compSize
                dataCategory = category
                availableFormats = formats
                selectedFormat = defaultFormat
                loadedFilePath = path
            }
        }
    }
    
    // MARK: - Step 2: Extract
    
    private func extractAction() {
        guard let path = loadedFilePath, !isExtracting else { return }
        self.errorMessage = nil
        self.videoExportNote = nil
        self.successMessage = nil
        self.isExtracting = true
        
        let format = selectedFormat
        let category = dataCategory
        
        // Read + decompress + format conversion is the heavy part (full
        // decode of the stream) — run it off the main thread and only come
        // back to main for the save panel.
        DispatchQueue.global(qos: .userInitiated).async {
            var ctx = qtsq_context_t()
            qtsq_init(&ctx)
            
            var error: String? = nil
            var outputData = Data()
            
            let readRes = path.withCString { cPath -> Int32 in
                return qtsq_read(&ctx, cPath)
            }
            if readRes != QTSQ_OK {
                error = String(cString: qtsq_error_string(readRes))
            } else {
                var rawData: UnsafeMutablePointer<UInt8>? = nil
                var rawSize: Int = 0
                
                if category == .video {
                    // A frame sequence (or a standalone audio track) is not a
                    // single file, so these export directly to a location the
                    // user picks instead of going through the save panel.
                    DispatchQueue.main.async {
                        let baseName = URL(fileURLWithPath: path).deletingPathExtension().lastPathComponent
                        if format == "wav" {
                            if !videoHasAudio(ctx: ctx) {
                                isExtracting = false
                                errorMessage = "This video has no audio track."
                                return
                            }
                            let panel = NSSavePanel()
                            panel.nameFieldStringValue = "\(baseName)_audio.wav"
                            guard panel.runModal() == .OK, let dest = panel.url else {
                                isExtracting = false
                                return
                            }
                            if exportVideoAudio(ctx: ctx, to: dest) {
                                isExtracting = false
                                videoExportNote = "Audio written to \(dest.path)"
                            } else {
                                isExtracting = false
                                errorMessage = "Audio extraction failed."
                            }
                        } else {
                            let panel = NSOpenPanel()
                            panel.canChooseDirectories = true
                            panel.canChooseFiles = false
                            panel.allowsMultipleSelection = false
                            panel.prompt = "Choose Folder"
                            guard panel.runModal() == .OK, let dir = panel.url else {
                                isExtracting = false
                                return
                            }
                            let count = exportVideoFrames(ctx: ctx, toDirectory: dir, baseName: baseName)
                            isExtracting = false
                            if count > 0 {
                                videoExportNote = "\(count) PNG frames written to \(dir.path)"
                            } else {
                                errorMessage = "No frames could be decoded."
                            }
                        }
                    }
                    qtsq_free(&ctx)
                    return
                }

                let res = qtsq_decompress(&ctx, &rawData, &rawSize)
                if res != QTSQ_OK {
                    error = String(cString: qtsq_error_string(res))
                } else if let dataPtr = rawData, rawSize > 0 {
                    // Convert raw data to the selected format
                    switch category {
                    case .image:
                        outputData = convertImage(dataPtr: dataPtr, rawSize: rawSize, ctx: ctx, format: format)
                    case .audio:
                        if format == "wav" {
                            // Check if the C decoder already returned a complete WAV file
                            // (both gravitational and dictionary decoders embed WAV headers)
                            let isAlreadyWav = rawSize >= 4 &&
                                dataPtr[0] == 0x52 && dataPtr[1] == 0x49 &&  // "RI"
                                dataPtr[2] == 0x46 && dataPtr[3] == 0x46     // "FF"
                            if isAlreadyWav {
                                outputData = Data(bytes: dataPtr, count: rawSize)
                            } else if ctx.header.strategy == 0x01 && ctx.header.sample_depth == 32 {
                                /* MDCT2 capture: the generic decoder hands back
                                 * interleaved float32, not int16. Wrapping it as
                                 * 16-bit PCM would label 4-byte samples as 2-byte
                                 * ones — a WAV at double speed and half length. */
                                outputData = wrapFloat32InWav(dataPtr: dataPtr, rawSize: rawSize, ctx: ctx)
                            } else {
                                outputData = wrapInWav(dataPtr: dataPtr, rawSize: rawSize, ctx: ctx)
                            }
                        } else {
                            outputData = Data(bytes: dataPtr, count: rawSize)
                        }
                    case .text, .binary, .unknown, .video:
                        outputData = Data(bytes: dataPtr, count: rawSize)
                    }
                    
                    free(dataPtr)
                } else {
                    error = "Decompression yielded empty data."
                }
            }
            
            qtsq_free(&ctx)
            
            DispatchQueue.main.async {
                guard error == nil else {
                    isExtracting = false
                    errorMessage = error
                    return
                }
                
                // Save dialog (must run on main)
                let baseName = URL(fileURLWithPath: path).deletingPathExtension().lastPathComponent
                let save = NSSavePanel()
                save.nameFieldStringValue = "\(baseName)_extracted.\(format)"
                
                guard save.runModal() == .OK, let saveUrl = save.url else {
                    isExtracting = false
                    return
                }
                
                // Write off-main too — the extracted payload can be large.
                DispatchQueue.global(qos: .userInitiated).async {
                    do {
                        try outputData.write(to: saveUrl)
                        let written = outputData.count
                        DispatchQueue.main.async {
                            isExtracting = false
                            successMessage = "✅ Extracted \(formatBytes(UInt64(written))) as .\(format.uppercased())"
                        }
                    } catch {
                        DispatchQueue.main.async {
                            isExtracting = false
                            errorMessage = "Failed to write file to disk."
                        }
                    }
                }
            }
        }
    }
    
    // MARK: - Image Conversion
    
    private func convertImage(dataPtr: UnsafeMutablePointer<UInt8>, rawSize: Int, ctx: qtsq_context_t, format: String) -> Data {
        // Get dimensions from schema
        var width = Int(ctx.schema.dimensions.0)
        var height = Int(ctx.schema.dimensions.1)
        var channels = Int(ctx.schema.dimensions.2)
        
        // Fallback: read from singularity header if schema is empty
        if width == 0 || height == 0 {
            if let sing = ctx.singularity, ctx.singularity_size >= 10 {
                var w: UInt32 = 0
                var h: UInt32 = 0
                memcpy(&w, sing, 4)
                memcpy(&h, sing.advanced(by: 4), 4)
                let c = sing.advanced(by: 8).pointee
                width = Int(w)
                height = Int(h)
                channels = Int(c)
            }
        }
        
        // Final fallback: guess square
        if width == 0 || height == 0 {
            let type = qtsq_data_type_t(UInt32(ctx.header.data_type))
            if type == QTSQ_TYPE_IMAGE_GRAY { channels = 1 }
            else if type == QTSQ_TYPE_IMAGE_RGBA { channels = 4 }
            else { channels = 3 }
            if channels > 0 {
                let px = rawSize / channels
                width = Int(sqrt(Double(px)))
                if width == 0 { width = 1 }
                height = px / width
                if height == 0 { height = 1 }
            }
        }
        
        if channels == 0 { channels = 3 }
        
        // If "raw" format, just return raw pixels
        if format == "raw" {
            return Data(bytes: dataPtr, count: rawSize)
        }
        
        // Build CGImage
        let colorSpace: CGColorSpace
        var bitmapInfo: UInt32
        
        if channels == 1 {
            colorSpace = CGColorSpaceCreateDeviceGray()
            bitmapInfo = CGImageAlphaInfo.none.rawValue
        } else if channels == 4 {
            colorSpace = CGColorSpaceCreateDeviceRGB()
            bitmapInfo = CGImageAlphaInfo.premultipliedLast.rawValue
        } else {
            colorSpace = CGColorSpaceCreateDeviceRGB()
            bitmapInfo = CGImageAlphaInfo.none.rawValue
        }
        
        let provider = CGDataProvider(data: Data(bytes: dataPtr, count: rawSize) as CFData)!
        
        guard let cgImage = CGImage(
            width: width, height: height,
            bitsPerComponent: 8, bitsPerPixel: 8 * channels,
            bytesPerRow: width * channels,
            space: colorSpace,
            bitmapInfo: CGBitmapInfo(rawValue: bitmapInfo),
            provider: provider, decode: nil, shouldInterpolate: true, intent: .defaultIntent
        ) else {
            return Data(bytes: dataPtr, count: rawSize)
        }
        
        let bmp = NSBitmapImageRep(cgImage: cgImage)
        
        switch format {
        case "png":
            return bmp.representation(using: .png, properties: [:]) ?? Data(bytes: dataPtr, count: rawSize)
        case "jpeg":
            return bmp.representation(using: .jpeg, properties: [.compressionFactor: 0.9]) ?? Data(bytes: dataPtr, count: rawSize)
        case "tiff":
            return bmp.representation(using: .tiff, properties: [:]) ?? Data(bytes: dataPtr, count: rawSize)
        case "bmp":
            return bmp.representation(using: .bmp, properties: [:]) ?? Data(bytes: dataPtr, count: rawSize)
        default:
            return Data(bytes: dataPtr, count: rawSize)
        }
    }
    
    // MARK: - WAV Wrapper
    
    private func wrapInWav(dataPtr: UnsafeMutablePointer<UInt8>, rawSize: Int, ctx: qtsq_context_t) -> Data {
        let numChannels: UInt16 = UInt16(ctx.header.num_channels > 0 ? ctx.header.num_channels : 1)
        let sampleRate: UInt32 = ctx.schema.sample_rate > 0 ? ctx.schema.sample_rate : 44100
        let bitsPerSample: UInt16 = 16
        let blockAlign = numChannels * (bitsPerSample / 8)
        let byteRate = UInt32(sampleRate) * UInt32(blockAlign)
        let dataSize = UInt32(rawSize)
        let fileSize = 36 + dataSize
        
        var wav = Data()
        wav.append(contentsOf: "RIFF".utf8)
        wav.append(withUnsafeBytes(of: fileSize.littleEndian) { Data($0) })
        wav.append(contentsOf: "WAVE".utf8)
        wav.append(contentsOf: "fmt ".utf8)
        wav.append(withUnsafeBytes(of: UInt32(16).littleEndian) { Data($0) })   // chunk size
        wav.append(withUnsafeBytes(of: UInt16(1).littleEndian) { Data($0) })    // PCM
        wav.append(withUnsafeBytes(of: numChannels.littleEndian) { Data($0) })
        wav.append(withUnsafeBytes(of: sampleRate.littleEndian) { Data($0) })
        wav.append(withUnsafeBytes(of: byteRate.littleEndian) { Data($0) })
        wav.append(withUnsafeBytes(of: blockAlign.littleEndian) { Data($0) })
        wav.append(withUnsafeBytes(of: bitsPerSample.littleEndian) { Data($0) })
        wav.append(contentsOf: "data".utf8)
        wav.append(withUnsafeBytes(of: dataSize.littleEndian) { Data($0) })
        wav.append(Data(bytes: dataPtr, count: rawSize))
        
        return wav
    }
    
    /// Converts interleaved float32 samples to a 16-bit PCM WAV, clamping
    /// to +/-1.0. Used for MDCT2 streams, whose decoded bytes are floats.
    private func wrapFloat32InWav(dataPtr: UnsafeMutablePointer<UInt8>, rawSize: Int, ctx: qtsq_context_t) -> Data {
        let numChannels: UInt16 = UInt16(ctx.header.num_channels > 0 ? ctx.header.num_channels : 2)
        let sampleRate: UInt32 = ctx.schema.sample_rate > 0 ? ctx.schema.sample_rate : 44100
        let frames = rawSize / MemoryLayout<Float>.size
        let bitsPerSample: UInt16 = 16
        let blockAlign = numChannels * (bitsPerSample / 8)
        let byteRate = sampleRate * UInt32(blockAlign)
        let dataSize = UInt32(frames * 2)
        let fileSize = 36 + dataSize
        
        var pcm16 = [Int16](repeating: 0, count: frames)
        dataPtr.withMemoryRebound(to: Float.self, capacity: frames) { f in
            for i in 0..<frames {
                let v = Swift.max(-1.0, Swift.min(1.0, f[i]))
                pcm16[i] = Int16(v * 32767.0)
            }
        }
        
        var wav = Data()
        wav.append(contentsOf: "RIFF".utf8)
        wav.append(withUnsafeBytes(of: fileSize.littleEndian) { Data($0) })
        wav.append(contentsOf: "WAVE".utf8)
        wav.append(contentsOf: "fmt ".utf8)
        wav.append(withUnsafeBytes(of: UInt32(16).littleEndian) { Data($0) })   // chunk size
        wav.append(withUnsafeBytes(of: UInt16(1).littleEndian) { Data($0) })    // PCM
        wav.append(withUnsafeBytes(of: numChannels.littleEndian) { Data($0) })
        wav.append(withUnsafeBytes(of: sampleRate.littleEndian) { Data($0) })
        wav.append(withUnsafeBytes(of: byteRate.littleEndian) { Data($0) })
        wav.append(withUnsafeBytes(of: blockAlign.littleEndian) { Data($0) })
        wav.append(withUnsafeBytes(of: bitsPerSample.littleEndian) { Data($0) })
        wav.append(contentsOf: "data".utf8)
        wav.append(withUnsafeBytes(of: dataSize.littleEndian) { Data($0) })
        pcm16.withUnsafeBytes { wav.append(contentsOf: $0) }
        
        return wav
    }
}

// MARK: - Info Chip

struct InfoChip: View {
    let label: String
    let value: String
    
    var body: some View {
        VStack(alignment: .leading, spacing: 4) {
            Text(label)
                .font(.system(size: 10, weight: .bold))
                .foregroundColor(.secondary)
            Text(value)
                .font(.system(size: 14, weight: .bold, design: .rounded))
                .foregroundColor(.primary)
        }
        .padding(.horizontal, 14)
        .padding(.vertical, 10)
        .background(.regularMaterial)
        .cornerRadius(12)
        .overlay(RoundedRectangle(cornerRadius: 12).stroke(Color.primary.opacity(0.05), lineWidth: 1))
    }
}

// ── Video export helpers ────────────────────────────────────────────
// The decoder hands back owned RGB buffers; convert each to a CGImage and
// write a PNG so exported frames open in anything, not just QTSQ Studio.
private func videoHasAudio(ctx: qtsq_context_t) -> Bool {
    var c = ctx
    guard let dec = qtsq_video_decoder_open(&c) else { return false }
    defer { qtsq_video_decoder_free(dec) }
    return qtsq_video_decoder_has_audio(dec) != 0
}

private func exportVideoFrames(ctx: qtsq_context_t,
                               toDirectory dir: URL,
                               baseName: String) -> Int {
    var c = ctx
    guard let dec = qtsq_video_decoder_open(&c) else { return 0 }
    defer { qtsq_video_decoder_free(dec) }

    let count = Int(qtsq_video_decoder_get_frame_count(dec))
    guard count > 0 else { return 0 }

    let digits = max(4, String(count).count)
    var written = 0
    for i in 0..<count {
        var px: UnsafeMutablePointer<UInt8>? = nil
        var w: UInt32 = 0, h: UInt32 = 0
        if qtsq_video_decoder_get_frame(dec, UInt32(i), &px, &w, &h) != QTSQ_OK || px == nil || w == 0 || h == 0 {
            continue
        }
        let channels = Int(ctx.schema.dimensions.2)
        let bytesPerPixel = channels >= 4 ? 4 : 3
        let bpr = Int(w) * bytesPerPixel
        guard let pxBuf = UnsafeRawPointer(px) else { continue }
        guard let provider = CGDataProvider(dataInfo: nil, data: pxBuf, size: bpr * Int(h),
                                            releaseData: { _, _, _ in }) else { continue }
        guard let cgImage = CGImage(width: Int(w), height: Int(h), bitsPerComponent: 8,
                                   bitsPerPixel: 8 * bytesPerPixel, bytesPerRow: bpr,
                                   space: CGColorSpaceCreateDeviceRGB(),
                                   bitmapInfo: bytesPerPixel == 4
                                       ? CGBitmapInfo(rawValue: CGImageAlphaInfo.noneSkipLast.rawValue)
                                       : CGBitmapInfo(rawValue: CGImageAlphaInfo.none.rawValue),
                                   provider: provider, decode: nil,
                                   shouldInterpolate: false, intent: .defaultIntent) else { continue }
        let rep = NSBitmapImageRep(cgImage: cgImage)
        if let png = rep.representation(using: .png, properties: [:]) {
            let idx = String(format: "%0\(digits)d", i)
            let dest = dir.appendingPathComponent("\(baseName)_\(idx).png")
            do {
                try png.write(to: dest)
                written += 1
            } catch {
                continue
            }
        }
    }
    return written
}

/// Write the embedded Classic Gravitational audio track as a WAV file.
private func exportVideoAudio(ctx: qtsq_context_t, to dest: URL) -> Bool {
    var c = ctx
    guard let dec = qtsq_video_decoder_open(&c) else { return false }
    defer { qtsq_video_decoder_free(dec) }

    var wav: UnsafeMutablePointer<UInt8>? = nil
    var wavSize = 0
    var rate: UInt32 = 0
    var channels: UInt8 = 0
    guard qtsq_video_decoder_get_audio(dec, &wav, &wavSize, &rate, &channels) == QTSQ_OK,
          let wav = wav, wavSize > 44 else {
        free(wav)
        return false
    }
    let data = Data(bytes: wav, count: wavSize)
    free(wav)
    return (try? data.write(to: dest)) != nil
}
