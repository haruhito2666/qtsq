import SwiftUI
import AVFoundation
import UniformTypeIdentifiers

// The standard audio path is MDCT2. Its bitrate is not a user setting:
// the encoder matches the source file's own bitrate (minus the container
// envelope), so importing a 3.4 MB MP3 produces a ~3.4 MB QTSQ file
// instead of inflating it to a fixed 320 kbps tier. Stereo is the
// default; a mono source stays mono.
//
// Photon Audio is the future spatial codec (positioned against Dolby
// Atmos). It is listed as a format choice but disabled until that work
// lands. The old Photon HQ (Gabor atoms) and spectral dictionary paths
// remain decode-only for legacy files: HQ expanded a 3.4 MB recording to
// 95.1 MB, and the lossless spectral dictionary expanded the same file
// to 16.1 MB because an already-compressed MP3 has no redundancy left.
enum AudioCompressionMethod: CaseIterable, Hashable {
    case standard       // MDCT2, bitrate auto-matched to the source
    case photon         // spatial codec — coming later

    var isAvailable: Bool { self == .standard }

    var title: String {
        switch self {
        case .standard: return "Standard"
        case .photon:   return "Photon"
        }
    }

    var subtitle: String {
        switch self {
        case .standard: return "MP3-size audio · bitrate matched to your file"
        case .photon:   return "Spatial audio · coming later"
        }
    }

    var icon: String {
        switch self {
        case .standard: return "waveform.path"
        case .photon:   return "atom"
        }
    }

    var color: Color {
        switch self {
        case .standard: return .orange
        case .photon:   return .purple
        }
    }

    /// Inverse of MDCT2's quality→bitrate anchors
    /// (48/96/128/192/320 kbps at quality 0/0.25/0.5/0.85/1.0). The
    /// logger is log-linear, so this returns the quality that makes the
    /// encoder target `kbps`. Clamped just above 48 kbps because quality
    /// 0 means "lossless" to the capture API.
    static func quality(forKbps kbps: Double) -> Float {
        let k = min(320.0, max(49.0, kbps))
        let anchors: [(q: Double, kbps: Double)] = [
            (0.0, 48.0), (0.25, 96.0), (0.5, 128.0),
            (0.85, 192.0), (1.0, 320.0),
        ]
        for i in 1..<anchors.count {
            let a = anchors[i - 1], b = anchors[i]
            if k <= b.kbps {
                let t = log2(k / a.kbps) / log2(b.kbps / a.kbps)
                return Float(a.q + t * (b.q - a.q))
            }
        }
        return 1.0
    }
}

enum ImageQualityMode {
    case hdSplat
    case hdTriangle
    case hdHybrid
    
    case compactSplat
    case compactTriangle
    case compactHybrid
    
    case losslessSplat
    case losslessTriangle
    case losslessHybrid
    
    case pixel  // True pixel encoding — same output size as JPEG
}

enum VideoQualityMode {
    case continuous4DGS  // 4D Gaussian Splat — velocity-based continuous
    case pixelLossless   // Pixel path, bit-exact (quality = 0)
    case pixelHigh       // Pixel lossy, high quality
    case pixelMedium     // Pixel lossy, balanced size
    case pixelLow        // Pixel lossy, smallest

    var isPixel: Bool {
        switch self {
        case .continuous4DGS: return false
        default: return true
        }
    }

    /// ctx->quality: 0 = lossless; >0 = custom QTC DCT lossy (0..1).
    var quality: Float {
        switch self {
        case .continuous4DGS: return 0
        case .pixelLossless:  return 0
        case .pixelHigh:      return 0.70
        case .pixelMedium:    return 0.40
        case .pixelLow:       return 0.20
        }
    }

    var label: String {
        switch self {
        case .continuous4DGS: return "4DGS"
        case .pixelLossless:  return "Pixel Lossless"
        case .pixelHigh:      return "Pixel High"
        case .pixelMedium:    return "Pixel Medium"
        case .pixelLow:       return "Pixel Low"
        }
    }
}

struct CompressView: View {
    @EnvironmentObject var appState: AppState
    @State private var isTargeted = false
    @State private var errorMessage: String? = nil
    @State private var successMessage: String? = nil
    @State private var isCompressing = false
    @State private var encodeStatus = "Encoding stream to vault…"
    @State private var showAudioMethodPicker = false
    @State private var pendingAudioPath: String? = nil
    @State private var showImageQualityPicker = false
    @State private var pendingImagePath: String? = nil
    @State private var showVideoQualityPicker = false
    @State private var pendingVideoPath: String? = nil
    // Folder/multi-file container quality
    @State private var showFolderQualityPicker = false
    @State private var pendingFolderPaths: [String]? = nil
    @State private var pendingFolderIsFolder = false // true = single folder, false = multi-file
    @State private var pendingFolderHasImages = false
    @State private var pendingFolderHasAudio = false
    @State private var pendingFolderHasVideo = false
    // Phase 7: the container codec is a Settings preference (qtc default).
    @AppStorage("vault.codec") private var codec: VaultCodec = .qtc
    
    var body: some View {
        ZStack {
            VStack(alignment: .leading, spacing: 24) {
                HStack(spacing: 8) {
                    Image(systemName: "archivebox")
                        .font(.system(size: 15, weight: .semibold))
                        .foregroundColor(VaultTheme.emberOrange)
                    
                    Text("Compress to Vault")
                        .font(.system(size: 18, weight: .semibold))
                        .foregroundColor(VaultTheme.textPrimary)
                }
                .padding(.bottom, 4)
                
                Text("Drop any image, video clip, audio track, document corpus, or directory to encode into .qtsq format.")
                    .font(.system(size: 13))
                    .foregroundColor(VaultTheme.textSecondary)
                
                VStack(spacing: 14) {
                    if isCompressing {
                        ProgressView()
                            .scaleEffect(1.2)
                        Text(encodeStatus)
                            .font(.system(size: 13, weight: .semibold))
                            .foregroundColor(VaultTheme.emberOrange)
                    } else {
                        Image(systemName: "arrow.down.doc")
                            .font(.system(size: 36))
                            .foregroundColor(isTargeted ? VaultTheme.emberOrange : VaultTheme.textSecondary)
                        
                        Text("Drop file or folder here, or click to browse")
                            .font(.system(size: 13, weight: .medium))
                            .foregroundColor(isTargeted ? VaultTheme.emberOrange : VaultTheme.textPrimary)
                    }
                }
                .frame(maxWidth: .infinity, maxHeight: .infinity)
                .background(isTargeted ? VaultTheme.emberOrangeBg : VaultTheme.cardWhite)
                .cornerRadius(10)
                .overlay(
                    RoundedRectangle(cornerRadius: 10, style: .continuous)
                        .stroke(isTargeted ? VaultTheme.emberOrange : VaultTheme.border, style: StrokeStyle(lineWidth: 1.5, dash: [6, 4]))
                )
                .shadow(color: VaultTheme.cardShadowColor, radius: 6, y: 2)
                .frame(height: 220)
                .onDrop(of: ["public.file-url"], isTargeted: $isTargeted) { providers in
                    // Collect ALL dropped URLs
                    var urls: [URL] = []
                    let group = DispatchGroup()
                    for provider in providers where provider.canLoadObject(ofClass: URL.self) {
                        group.enter()
                        _ = provider.loadObject(ofClass: URL.self) { url, _ in
                            if let url = url {
                                urls.append(url)
                            }
                            group.leave()
                        }
                    }
                    group.notify(queue: .main) {
                        if urls.count == 1 {
                            // Single item: check if folder or file
                            var isDir: ObjCBool = false
                            FileManager.default.fileExists(atPath: urls[0].path, isDirectory: &isDir)
                            if isDir.boolValue {
                                // Folder → container
                                self.compressFolder(path: urls[0].path)
                            } else {
                                // Single file → normal compress
                                self.handleFile(path: urls[0].path)
                            }
                        } else if urls.count > 1 {
                            // Multiple items → container
                            self.compressMultipleFiles(paths: urls.map { $0.path })
                        }
                    }
                    return true
                }
                
                HStack {
                    Spacer()
                    Button(action: browseAction) {
                        Text("Browse...")
                            .font(.system(size: 15, weight: .semibold))
                            .foregroundColor(.white)
                            .padding(.horizontal, 32)
                            .padding(.vertical, 14)
                            .background(VaultTheme.emberOrange)
                            .cornerRadius(24)
                            .shadow(color: VaultTheme.emberOrange.opacity(0.3), radius: 8, x: 0, y: 4)
                    }
                    .buttonStyle(PlainButtonStyle())
                    .disabled(isCompressing)
                    Spacer()
                }
                
                // Phase 7: the codec is chosen in Settings (qtc by default).
                HStack(spacing: 6) {
                    Image(systemName: "dial.medium")
                        .font(.system(size: 11))
                    Text("Codec: \(codec.rawValue) · change in Settings")
                        .font(.system(size: 11, weight: .medium))
                }
                .foregroundColor(.secondary)
                
                if let success = successMessage {
                    HStack(spacing: 8) {
                        Image(systemName: "checkmark.circle.fill")
                        Text(success)
                            .font(.system(size: 12, weight: .medium))
                    }
                    .foregroundColor(VaultTheme.integrityGreen)
                    .padding(12)
                    .frame(maxWidth: .infinity, alignment: .center)
                    .background(VaultTheme.integrityGreenBg)
                    .cornerRadius(8)
                    .overlay(RoundedRectangle(cornerRadius: 8).stroke(VaultTheme.integrityGreen.opacity(0.3), lineWidth: 1))
                }
                
                if let error = errorMessage {
                    HStack(spacing: 8) {
                        Image(systemName: "exclamationmark.triangle.fill")
                        Text(error)
                            .font(.system(size: 12, weight: .medium))
                    }
                    .foregroundColor(VaultTheme.dangerRed)
                    .padding(12)
                    .frame(maxWidth: .infinity, alignment: .center)
                    .background(VaultTheme.dangerRedBg)
                    .cornerRadius(8)
                    .overlay(RoundedRectangle(cornerRadius: 8).stroke(VaultTheme.dangerRed.opacity(0.3), lineWidth: 1))
                }
                
                Spacer()
            }
            .padding(40)
            
            // Sheet anchors
            Color.clear
                .sheet(isPresented: $showAudioMethodPicker) {
                    AudioMethodPickerSheet(onSelect: { method in
                        showAudioMethodPicker = false
                        if let path = pendingAudioPath {
                            compressFile(path: path, audioMethod: method, imageQuality: nil, videoQuality: nil)
                        }
                    }, onCancel: {
                        showAudioMethodPicker = false
                        pendingAudioPath = nil
                    })
                }
            
            Color.clear
                .sheet(isPresented: $showImageQualityPicker) {
                    ImageQualityPickerSheet(onSelect: { quality in
                        showImageQualityPicker = false
                        if let path = pendingImagePath {
                            compressFile(path: path, audioMethod: nil, imageQuality: quality, videoQuality: nil)
                        }
                    }, onCancel: {
                        showImageQualityPicker = false
                        pendingImagePath = nil
                    })
                }
            
            Color.clear
                .sheet(isPresented: $showVideoQualityPicker) {
                    VideoQualityPickerSheet(onSelect: { quality in
                        showVideoQualityPicker = false
                        if let path = pendingVideoPath {
                            compressFile(path: path, audioMethod: nil, imageQuality: nil, videoQuality: quality)
                        }
                    }, onCancel: {
                        showVideoQualityPicker = false
                        pendingVideoPath = nil
                    })
                }
            
            Color.clear
                .sheet(isPresented: $showFolderQualityPicker) {
                    FolderQualityPickerSheet(
                        hasImages: pendingFolderHasImages,
                        hasAudio: pendingFolderHasAudio,
                        hasVideo: pendingFolderHasVideo,
                        onSelect: { imgQ, audioM, videoQ in
                            showFolderQualityPicker = false
                            if let paths = pendingFolderPaths {
                                if pendingFolderIsFolder {
                                    compressFolderWithQuality(path: paths[0], imageQuality: imgQ, audioMethod: audioM, videoQuality: videoQ)
                                } else {
                                    compressMultipleFilesWithQuality(paths: paths, imageQuality: imgQ, audioMethod: audioM, videoQuality: videoQ)
                                }
                            }
                        },
                        onCancel: {
                            showFolderQualityPicker = false
                            pendingFolderPaths = nil
                        }
                    )
                }
        }
        .frame(maxWidth: .infinity, maxHeight: .infinity)
        .background(Color.clear)
    }
    
    private func browseAction() {
        let panel = NSOpenPanel()
        panel.allowsMultipleSelection = true
        panel.canChooseDirectories = true
        if panel.runModal() == .OK {
            let urls = panel.urls
            if urls.count == 1 {
                var isDir: ObjCBool = false
                FileManager.default.fileExists(atPath: urls[0].path, isDirectory: &isDir)
                if isDir.boolValue {
                    self.compressFolder(path: urls[0].path)
                } else {
                    self.handleFile(path: urls[0].path)
                }
            } else if urls.count > 1 {
                self.compressMultipleFiles(paths: urls.map { $0.path })
            }
        }
    }
    
    private func handleFile(path: String) {
        let ext = URL(fileURLWithPath: path).pathExtension.lowercased()
        // Dropping an existing .qtsq should open it, not re-wrap it as generic
        // data (which produced a nested, unplayable blob).
        if ext == "qtsq" {
            appState.openFile(path: path)
            return
        }
        let audioExts = Set(["mp3", "wav", "aac", "m4a", "flac", "ogg", "wma", "aiff", "aif"])
        let imageExts = Set(["jpg", "jpeg", "png", "bmp", "tiff", "tif", "gif", "heic", "webp", "ico"])
        
        let isAudio = audioExts.contains(ext)
        let isImage = imageExts.contains(ext)
        
        // Use UTType to detect any video format naturally supported by macOS
        var isVideo = false
        if let type = UTType(filenameExtension: ext) {
            isVideo = type.conforms(to: .movie) || type.conforms(to: .video) || type.conforms(to: .audiovisualContent)
        }
        if !isVideo {
            let videoExts = Set(["mp4", "mov", "webm", "mkv", "avi", "m4v", "flv", "wmv", "3gp"])
            isVideo = videoExts.contains(ext)
        }
        
        if isAudio {
            pendingAudioPath = path
            showAudioMethodPicker = true
        } else if isImage {
            pendingImagePath = path
            showImageQualityPicker = true
        } else if isVideo {
            pendingVideoPath = path
            showVideoQualityPicker = true
        } else {
            compressFile(path: path, audioMethod: nil, imageQuality: nil, videoQuality: nil)
        }
    }
    
    private func compressToContext(path: String, audioMethod: AudioCompressionMethod?, imageQuality: ImageQualityMode?, videoQuality: VideoQualityMode?, ctx: inout qtsq_context_t, sourceFileSize: inout Int) -> Int32 {
        let ext = URL(fileURLWithPath: path).pathExtension.lowercased()
        let audioExts = Set(["mp3", "wav", "aac", "m4a", "flac", "ogg", "wma", "aiff", "aif"])
        let imageExts = Set(["jpg", "jpeg", "png", "bmp", "tiff", "tif", "gif", "heic", "webp", "ico"])
        
        let isAudio = audioExts.contains(ext)
        let isImage = imageExts.contains(ext)
        var isVideo = false
        if let type = UTType(filenameExtension: ext) {
            isVideo = type.conforms(to: .movie) || type.conforms(to: .video) || type.conforms(to: .audiovisualContent)
        }
        if !isVideo {
            let videoExts = Set(["mp4", "mov", "webm", "mkv", "avi", "m4v", "flv", "wmv", "3gp"])
            isVideo = videoExts.contains(ext)
        }
        
        // Video and audio only need the byte count here; audio is decoded
        // through AVAudioFile below. Loading a multi-hundred-MB WAV into
        // Data first doubled peak memory for no reason.
        var data = Data()
        if isVideo || isAudio {
            if let attrs = try? FileManager.default.attributesOfItem(atPath: path),
               let sz = attrs[.size] as? Int, sz > 0 {
                sourceFileSize = sz
            } else {
                DispatchQueue.main.async {
                    if self.errorMessage == nil { self.errorMessage = "Failed to open file: \(path)" }
                }
                return -1
            }
        } else {
            guard let fileData = try? Data(contentsOf: URL(fileURLWithPath: path)) else {
                DispatchQueue.main.async {
                    if self.errorMessage == nil { self.errorMessage = "Failed to open file: \(path)" }
                }
                return -1 // QTSQ_ERR_IO
            }
            data = fileData
            sourceFileSize = data.count
        }
        var result: Int32 = 0 // QTSQ_OK
        qtsq_init(&ctx)
        // Phase 7: qtc is the default codec (set by qtsq_init). The Settings
        // preference can opt out to legacy zlib. qtc runs on plaintext below
        // sanitize/encrypt, and decode sniffs the stream magic.
        if codec == .zlib {
            ctx.codec = UInt8(QTSQ_CODEC_ZLIB)
        }
        // Pixel video quality: 0 = lossless; >0 enables custom spatial quant.
        if let vq = videoQuality, vq.isPixel {
            ctx.quality = vq.quality
        }
        

        
        if isAudio {
            // ─── AUDIO PATH ─── Decode to PCM, then compress with chosen method
            do {
                let audioFile = try AVAudioFile(forReading: URL(fileURLWithPath: path))
                let format = audioFile.processingFormat
                // AVAudioFrameCount is 32-bit; a longer file would wrap and
                // silently encode a truncated prefix.
                guard format.sampleRate > 0,
                      format.sampleRate <= Double(UInt32.max),
                      format.channelCount > 0,
                      audioFile.length > 0,
                      audioFile.length <= Int64(UInt32.max) else {
                    DispatchQueue.main.async {
                        if self.errorMessage == nil {
                            self.errorMessage = "Audio file is empty, invalid, or too long."
                        }
                    }
                    result = QTSQ_ERR_TOO_LARGE
                    throw NSError(domain: "QTSQ", code: -1)
                }
                let frameCount = AVAudioFrameCount(audioFile.length)
                // Match the encoder's 4 GiB working-set budget before
                // allocating a whole-file PCM buffer. The float path needs
                // about 16 bytes per interleaved stereo sample (20 for
                // mono); 17/21 leaves margin while keeping the rejection
                // here instead of surfacing a generic codec size error.
                let estimatedChannels = min(Int(format.channelCount), 2)
                let bytesPerSample = estimatedChannels == 1 ? 21 : 17
                let budgetSamples = (4 << 30) / bytesPerSample
                guard Int(frameCount) * estimatedChannels <= budgetSamples else {
                    DispatchQueue.main.async {
                        if self.errorMessage == nil {
                            self.errorMessage = "Audio is too long for the standard encoder (about 45 minutes at 44.1 kHz stereo; longer for mono)."
                        }
                    }
                    result = QTSQ_ERR_TOO_LARGE
                    throw NSError(domain: "QTSQ", code: -1)
                }
                guard let pcmBuffer = AVAudioPCMBuffer(pcmFormat: format, frameCapacity: frameCount) else {
                    DispatchQueue.main.async {
                        if self.errorMessage == nil {
                            self.errorMessage = "Not enough memory to decode this audio file."
                        }
                    }
                    result = QTSQ_ERR_ALLOC
                    throw NSError(domain: "QTSQ", code: -1)
                }
                try audioFile.read(into: pcmBuffer)
                
                let channels = Int(format.channelCount)
                let sampleRate = UInt32(format.sampleRate)
                let frames = Int(pcmBuffer.frameLength)
                guard frames > 0 else {
                    result = QTSQ_ERR_TOO_SMALL
                    throw NSError(domain: "QTSQ", code: -1)
                }
                
                // Choose compression method. Standard is the only working
                // path today; Photon is a visible placeholder for the
                // spatial codec that lands later.
                let method = audioMethod ?? .standard
                if method == .photon {
                    DispatchQueue.main.async {
                        if self.errorMessage == nil {
                            self.errorMessage = "Photon audio is coming later. Use Standard for now."
                        }
                    }
                    result = QTSQ_ERR_FORMAT
                    throw NSError(domain: "QTSQ", code: -1)
                }

                // Match the source's own average bitrate instead of asking
                // the user to pick a tier. The 5 KB allowance covers the
                // QTSQ header, TOC and security block, so the finished
                // .qtsq lands at or just under the input file's size.
                // Lossless sources (WAV/FLAC) have a huge nominal bitrate
                // and simply cap at the best standard tier.
                let duration = format.sampleRate > 0
                    ? Double(frames) / format.sampleRate : 0
                let usableBytes = max(0.0, Double(sourceFileSize) - 5_000.0)
                let detectedKbps = (duration > 0 && usableBytes > 0)
                    ? usableBytes * 8.0 / duration / 1000.0
                    : 49.0

                // MDCT2 perceptual coder (qtc AUDIO_FLOAT lens).
                // ctx.quality is derived from that detected bitrate; the
                // qtc codec is forced because the legacy zlib stream has
                // no lossy mode. Standard audio is stereo, but a mono
                // source stays mono: duplicating it into a fake stereo
                // pair costs the same bytes for less real content.
                // Multi-channel sources use front L/R here; a caller that
                // wants a proper 5.1 downmix should convert to stereo
                // before handing it to QTSQ.
                ctx.codec = UInt8(QTSQ_CODEC_QTC)
                ctx.quality = AudioCompressionMethod.quality(forKbps: detectedKbps)

                let reqChannels = min(channels, 2)
                let totalReqSamples = frames * reqChannels
                // qtc's float lens caps at 2^30 samples. Check here too so
                // an absurd input produces an error instead of a wrapped
                // count or an unbounded allocation.
                guard totalReqSamples > 0, totalReqSamples <= (1 << 30),
                      let pcmRaw = malloc(totalReqSamples * MemoryLayout<Float32>.size) else {
                    DispatchQueue.main.async {
                        if self.errorMessage == nil {
                            self.errorMessage = "Audio is too long or there is not enough memory to encode it."
                        }
                    }
                    result = QTSQ_ERR_ALLOC
                    throw NSError(domain: "QTSQ", code: -1)
                }
                defer { free(pcmRaw) }
                let pcmFloat = pcmRaw.bindMemory(to: Float32.self,
                                                 capacity: totalReqSamples)

                if let floatData = pcmBuffer.floatChannelData {
                    for frame in 0..<frames {
                        for ch in 0..<reqChannels {
                            // Keep the C boundary clean: NaN becomes
                            // silence, infinities and out-of-range values
                            // clip to full scale.
                            var sample = floatData[ch][frame]
                            if !sample.isFinite { sample = 0 }
                            else if sample > 1.0 { sample = 1.0 }
                            else if sample < -1.0 { sample = -1.0 }
                            pcmFloat[frame * reqChannels + ch] = sample
                        }
                    }
                }

                result = qtsq_compress_audio_float(
                    &ctx, pcmFloat,
                    totalReqSamples,
                    sampleRate, UInt8(reqChannels))
                
                if result == QTSQ_OK {
                    ctx.header.original_size = UInt64(sourceFileSize)
                }
            } catch {
                if result == QTSQ_OK { result = QTSQ_ERR_FORMAT }
            }
        } else if isImage,
                  let image = NSImage(contentsOf: URL(fileURLWithPath: path)),
                  let cgImage = image.cgImage(forProposedRect: nil, context: nil, hints: nil) {
            // ─── IMAGE PATH ─── Use Gravitational Field Compression
            // Draw the CGImage straight into an 8-bit sRGB pixel buffer.
            // (The previous NSImage → tiffRepresentation → NSBitmapImageRep
            // round-trip re-encoded and re-decoded the same pixels in memory
            // only to read the dimensions — the pixels always came from the
            // CGImage draw below.)
            let width = cgImage.width
            let height = cgImage.height
            let channels: UInt8 = 3
            
            let rgbaBytesPerRow = width * 4
            let rgbaSize = height * rgbaBytesPerRow
            let rgbaBuffer = UnsafeMutablePointer<UInt8>.allocate(capacity: rgbaSize)
            
            if let colorSpace = CGColorSpace(name: CGColorSpace.sRGB),
               let cgContext = CGContext(
                data: rgbaBuffer,
                width: width,
                height: height,
                bitsPerComponent: 8,
                bytesPerRow: rgbaBytesPerRow,
                space: colorSpace,
                bitmapInfo: CGImageAlphaInfo.noneSkipLast.rawValue
               ) {
                
                cgContext.draw(cgImage, in: CGRect(x: 0, y: 0, width: width, height: height))
                
                let rgbSize = width * height * 3
                let rgbBuffer = UnsafeMutablePointer<UInt8>.allocate(capacity: rgbSize)
                for i in 0..<(width * height) {
                    rgbBuffer[i * 3]     = rgbaBuffer[i * 4]
                    rgbBuffer[i * 3 + 1] = rgbaBuffer[i * 4 + 1]
                    rgbBuffer[i * 3 + 2] = rgbaBuffer[i * 4 + 2]
                }
                
                switch imageQuality {
                case .hdSplat:
                    result = qtsq_compress_splat_hd(&ctx, rgbBuffer, UInt32(width), UInt32(height), channels, Int(sourceFileSize))
                case .hdTriangle:
                    result = qtsq_compress_triangle_adaptive(&ctx, rgbBuffer, UInt32(width), UInt32(height), channels, Int(sourceFileSize), 3, 15.0)
                case .hdHybrid:
                    result = qtsq_compress_gravitational_hd(&ctx, rgbBuffer, UInt32(width), UInt32(height), channels, Int(sourceFileSize))
                
                case .compactSplat:
                    result = qtsq_compress_splat_compact(&ctx, rgbBuffer, UInt32(width), UInt32(height), channels, Int(sourceFileSize))
                case .compactTriangle:
                    result = qtsq_compress_triangle_adaptive(&ctx, rgbBuffer, UInt32(width), UInt32(height), channels, Int(sourceFileSize), 2, 25.0)
                case .compactHybrid:
                    result = qtsq_compress_gravitational_hd(&ctx, rgbBuffer, UInt32(width), UInt32(height), channels, Int(sourceFileSize))
                
                case .losslessSplat:
                    result = qtsq_compress_splat_lossless(&ctx, rgbBuffer, UInt32(width), UInt32(height), channels, Int(sourceFileSize))
                case .losslessTriangle:
                    result = qtsq_compress_triangle_adaptive(&ctx, rgbBuffer, UInt32(width), UInt32(height), channels, Int(sourceFileSize), 4, 3.0)
                case .losslessHybrid:
                    result = qtsq_compress_gravitational_hd(&ctx, rgbBuffer, UInt32(width), UInt32(height), channels, Int(sourceFileSize))
                
                case .pixel:
                    // True pixel encoding — pass through raw JPEG/PNG for 100% fidelity and identical size
                    var isPrecompressed = false
                    if data.count > 4 {
                        let bytes = [UInt8](data.prefix(4))
                        if (bytes[0] == 0xFF && bytes[1] == 0xD8) || (bytes[0] == 0x89 && bytes[1] == 0x50 && bytes[2] == 0x4E && bytes[3] == 0x47) {
                            isPrecompressed = true
                        }
                    }
                    
                    if isPrecompressed {
                        data.withUnsafeBytes { ptr in
                            if let base = ptr.bindMemory(to: UInt8.self).baseAddress {
                                result = qtsq_compress_pixel_passthrough(&ctx, base, data.count)
                            }
                        }
                    } else {
                        result = qtsq_compress_pixel(&ctx, rgbBuffer, UInt32(width), UInt32(height), channels, QTSQ_PIXEL_COMPRESS_ZLIB)
                    }
                
                default:
                    result = qtsq_compress_gravitational(&ctx, rgbBuffer, UInt32(width), UInt32(height), channels, Int(sourceFileSize))
                }
                
                // ─── MeshNet V3: Vertex Position + Color Shift Optimizer ───
                if result == QTSQ_OK && MeshNetV3Service.shared.isAvailable {
                    print("🐜 MeshNet V3: Running Vertex AI enhancement...")
                    
                    let success = MeshNetV3Service.shared.optimizeMesh(
                        ctx: &ctx,
                        originalRGB: rgbBuffer,
                        width: width,
                        height: height
                    )
                    
                    if success {
                        print("✅ MeshNet V3: Vertex topology & colors optimized!")
                    } else {
                        print("⚠️ MeshNet V3: Model inference failed — using default vertices")
                    }
                }
                
                rgbBuffer.deallocate()
            } else {
                result = QTSQ_ERR_FORMAT
            }
            
            rgbaBuffer.deallocate()
            
            if result == QTSQ_OK {
                ctx.header.original_size = UInt64(sourceFileSize)
            }
        } else if isVideo {
            // ─── VIDEO PATH ───
            // AVFoundation first (fast, hardware). Many files (VP9/AV1/webm
            // variants, some camera RAW MOV) demux but cannot decode to BGRA
            // on this Mac — AVAssetReader then yields 0 frames while still
            // reporting "success". Never accept an empty encode: fall back
            // to FFmpeg, which handles essentially every format.
            do {
                var usedFFmpeg = false
                let asset = AVURLAsset(url: URL(fileURLWithPath: path))
                let semaphore = DispatchSemaphore(value: 0)
                var videoTrack: AVAssetTrack? = nil
                Task {
                    videoTrack = try? await asset.loadTracks(withMediaType: .video).first
                    semaphore.signal()
                }
                _ = semaphore.wait(timeout: .distantFuture)

                if let track = videoTrack {
                    let reader = try AVAssetReader(asset: asset)
                    let outputSettings: [String: Any] = [
                        kCVPixelBufferPixelFormatTypeKey as String: kCVPixelFormatType_32BGRA
                    ]
                    let trackOutput = AVAssetReaderTrackOutput(track: track, outputSettings: outputSettings)
                    reader.add(trackOutput)
                    let readerStarted = reader.startReading()
                    // VIDEO: Do NOT use QTSQ_FLAG_UPSCALE — that's for single images only.
                    var flags: Int32 = 0
                    if videoQuality?.isPixel == true {
                        flags = Int32(QTSQ_VIDEO_PIXEL)
                    } else {
                        flags = Int32(QTSQ_VIDEO_CONTINUOUS)
                    }

                    // Load track properties using modern async API
                    var trackSize = CGSize(width: 1920, height: 1080)
                    var trackFPS: Float = 24
                    var durationSec: Double = 0
                    let propSem = DispatchSemaphore(value: 0)
                    Task {
                        if let sz = try? await track.load(.naturalSize) { trackSize = sz }
                        if let fps = try? await track.load(.nominalFrameRate) { trackFPS = fps }
                        durationSec = CMTimeGetSeconds(try await asset.load(.duration))
                        propSem.signal()
                    }
                    propSem.wait()
                    let width = Int32(trackSize.width)
                    let height = Int32(trackSize.height)
                    // Round (don't truncate): 23.976 → 24, not 23.
                    let fpsInt = trackFPS > 0 ? Int32((Double(trackFPS) + 0.5).rounded()) : 24
                    // Rate-control budget ≈ source bytes per frame so lossy
                    // mode can land near the original file size.
                    let frameEst = max(1, Int(durationSec * Double(max(fpsInt, 1))))
                    let payload = max(sourceFileSize / frameEst, 256)

                    var enc: OpaquePointer? = nil
                    if readerStarted {
                        enc = qtsq_video_encoder_create(&ctx, UInt32(width), UInt32(height), 3, UInt32(fpsInt), flags, payload)
                    }

                    var framesAdded = 0
                    if let encoder = enc {
                        let rgbSize = Int(width * height * 3)
                        let rgbBuffer = UnsafeMutablePointer<UInt8>.allocate(capacity: rgbSize)
                        defer { rgbBuffer.deallocate() }

                        while readerStarted,
                              let sampleBuffer = trackOutput.copyNextSampleBuffer(),
                              reader.status == .reading {
                            guard let imageBuffer = CMSampleBufferGetImageBuffer(sampleBuffer) else { continue }
                            CVPixelBufferLockBaseAddress(imageBuffer, .readOnly)
                            defer { CVPixelBufferUnlockBaseAddress(imageBuffer, .readOnly) }
                            guard let ptr = CVPixelBufferGetBaseAddress(imageBuffer) else { continue }
                            let bytesPerRow = CVPixelBufferGetBytesPerRow(imageBuffer)
                            let bgraData = ptr.bindMemory(to: UInt8.self, capacity: Int(height) * bytesPerRow)

                            if let metal = qtsq_metal_global(),
                               qtsq_metal_bgra_to_rgb(metal, bgraData, rgbBuffer, UInt32(width), UInt32(height), UInt32(bytesPerRow)) == 0 {
                                // GPU path
                            } else {
                                // Tight unsafe BGRA→RGB. Nested Swift loops with
                                // bounds checks were minutes at 4K when metallib
                                // failed to load from the .app bundle.
                                let w = Int(width)
                                let h = Int(height)
                                bgraData.withMemoryRebound(to: UInt8.self, capacity: h * bytesPerRow) { src in
                                    rgbBuffer.withMemoryRebound(to: UInt8.self, capacity: w * h * 3) { dst in
                                        var di = 0
                                        var si = 0
                                        for _ in 0..<h {
                                            var x = 0
                                            var s = si
                                            while x < w {
                                                dst[di]     = src[s + 2] // R
                                                dst[di + 1] = src[s + 1] // G
                                                dst[di + 2] = src[s]     // B
                                                di += 3
                                                s += 4
                                                x += 1
                                            }
                                            si += bytesPerRow
                                        }
                                    }
                                }
                            }
                            if qtsq_video_encoder_add_frame(encoder, rgbBuffer) == 0 {
                                framesAdded += 1
                                if framesAdded % 24 == 0 {
                                    let n = framesAdded
                                    DispatchQueue.main.async {
                                        self.encodeStatus = "Encoding… \(n) frames"
                                    }
                                }
                            }
                        }

                        // Keep any frames we actually got. Discarding a partial
                        // AV read and re-running FFmpeg doubled wall time on
                        // long 4K clips ("still processing" after minutes).
                        // Only fall back when AV produced nothing (VP9 etc.).
                        if framesAdded > 0 {
                            attachGravitationalAudio(to: encoder, sourcePath: path)
                            result = qtsq_video_encoder_finish(encoder)
                            if result == 0 {
                                ctx.header.original_size = UInt64(sourceFileSize)
                            }
                        } else {
                            usedFFmpeg = true
                        }
                        qtsq_video_encoder_free(encoder)
                    } else {
                        usedFFmpeg = true
                    }

                    if usedFFmpeg {
                        // Reset ctx (encoder_create may have stamped schema/flags)
                        qtsq_init(&ctx)
                        if self.codec == .zlib { ctx.codec = UInt8(QTSQ_CODEC_ZLIB) }
                        if let vq = videoQuality, vq.isPixel { ctx.quality = vq.quality }
                        result = try self.processWithFFmpeg(path: path, videoQuality: videoQuality, ctx: &ctx, sourceFileSize: sourceFileSize)
                    }
                } else {
                    // No video track via AVFoundation → FFmpeg
                    result = try self.processWithFFmpeg(path: path, videoQuality: videoQuality, ctx: &ctx, sourceFileSize: sourceFileSize)
                }

            } catch {
                // Any AVFoundation throw → try FFmpeg before giving up
                do {
                    qtsq_init(&ctx)
                    if self.codec == .zlib { ctx.codec = UInt8(QTSQ_CODEC_ZLIB) }
                    if let vq = videoQuality, vq.isPixel { ctx.quality = vq.quality }
                    result = try self.processWithFFmpeg(path: path, videoQuality: videoQuality, ctx: &ctx, sourceFileSize: sourceFileSize)
                } catch {
                    result = QTSQ_ERR_FORMAT
                    DispatchQueue.main.async {
                        if self.errorMessage == nil {
                            self.errorMessage = error.localizedDescription
                        }
                    }
                }
            }
        } else {
            // ─── DETECT TEXT / DOCUMENT FILES ───
            let textExts = Set(["txt", "md", "markdown", "rst", "log",
                                "html", "htm", "xml", "svg", "yaml", "yml", "toml", "ini", "cfg",
                                "c", "h", "cpp", "hpp", "m", "mm", "swift", "rs", "go",
                                "py", "rb", "js", "ts", "jsx", "tsx", "vue", "svelte",
                                "java", "kt", "kts", "cs", "dart", "lua", "sh", "bash", "zsh",
                                "css", "scss", "less", "sql", "r", "pl", "php",
                                "makefile", "cmake", "dockerfile", "gitignore"])
            let jsonExts = Set(["json", "geojson", "jsonl"])
            let csvExts = Set(["csv", "tsv"])
            
            if jsonExts.contains(ext) {
                // ─── JSON PATH ─── Dictionary compression with minification
                data.withUnsafeBytes { buffer in
                    guard let ptr = buffer.bindMemory(to: Int8.self).baseAddress else {
                        result = QTSQ_ERR_NULL
                        return
                    }
                    result = qtsq_compress_json(&ctx, ptr, data.count)
                    if result == QTSQ_OK {
                        ctx.header.original_size = UInt64(sourceFileSize)
                    }
                }
            } else if csvExts.contains(ext) {
                // ─── CSV PATH ─── Dictionary compression
                data.withUnsafeBytes { buffer in
                    guard let ptr = buffer.bindMemory(to: Int8.self).baseAddress else {
                        result = QTSQ_ERR_NULL
                        return
                    }
                    result = qtsq_compress_csv(&ctx, ptr, data.count)
                    if result == QTSQ_OK {
                        ctx.header.original_size = UInt64(sourceFileSize)
                    }
                }
            } else if textExts.contains(ext) {
                // ─── TEXT PATH ─── Dictionary + ZLib compression
                data.withUnsafeBytes { buffer in
                    guard let ptr = buffer.bindMemory(to: Int8.self).baseAddress else {
                        result = QTSQ_ERR_NULL
                        return
                    }
                    result = qtsq_compress_text(&ctx, ptr, data.count)
                    if result == QTSQ_OK {
                        ctx.header.original_size = UInt64(sourceFileSize)
                    }
                }
            } else {
                // ─── GENERIC PATH ─── Raw compression with type detection
                data.withUnsafeBytes { buffer in
                    guard let ptr = buffer.bindMemory(to: UInt8.self).baseAddress else {
                        result = QTSQ_ERR_NULL
                        return
                    }
                    let mutPtr = UnsafeMutablePointer(mutating: ptr)
                    result = qtsq_compress_raw(&ctx, mutPtr, data.count, 0)
                    
                    if result == QTSQ_OK {
                        ctx.header.original_size = UInt64(sourceFileSize)
                        let detected = qtsq_detect_type(ptr, data.count)
                        ctx.header.data_type = UInt8(detected.rawValue)
                    }
                }
            }
        }
        return result
    }

    private func compressFile(path: String, audioMethod: AudioCompressionMethod?, imageQuality: ImageQualityMode?, videoQuality: VideoQualityMode?) {
        self.errorMessage = nil
        self.successMessage = nil
        self.isCompressing = true
        self.encodeStatus = "Encoding stream to vault…"
        
        let ext = URL(fileURLWithPath: path).pathExtension.lowercased()
        
        DispatchQueue.global(qos: .userInitiated).async {
            var ctx = qtsq_context_t()
            var sourceFileSize = 0
            let result = self.compressToContext(path: path, audioMethod: audioMethod, imageQuality: imageQuality, videoQuality: videoQuality, ctx: &ctx, sourceFileSize: &sourceFileSize)
            
            DispatchQueue.main.async {
                self.isCompressing = false
                
                if result != 0 {
                    if self.errorMessage == nil {
                        self.errorMessage = String(cString: qtsq_error_string(result))
                    }
                    qtsq_free(&ctx)
                    return
                }
                
                let save = NSSavePanel()
                save.allowedContentTypes = [UTType(filenameExtension: "qtsq")!]
                let base = URL(fileURLWithPath: path).deletingPathExtension().lastPathComponent
                save.nameFieldStringValue = base + ".qtsq"
                
                if save.runModal() == .OK, let saveUrl = save.url {
                    let writeResult = saveUrl.path.withCString { cPath -> Int32 in
                        if !qtsqPromptAndEncrypt(&ctx) { return -1 }
                        return qtsq_write(&ctx, cPath)
                    }
                    
                    if writeResult == 0 {
                        var compressedSize: UInt64 = 0
                        if let attrs = try? FileManager.default.attributesOfItem(atPath: saveUrl.path),
                           let sz = attrs[FileAttributeKey.size] as? UInt64 {
                            compressedSize = sz
                        }
                        
                        let origStr = self.formatSize(UInt64(sourceFileSize))
                        let compStr = self.formatSize(compressedSize)
                        let lossyExts = Set(["jpg", "jpeg", "webp"])
                        if imageQuality == .hdSplat && lossyExts.contains(ext) {
                            self.successMessage = "✨ Improved: \(origStr) → \(compStr) · \(self.codec.rawValue)"
                        } else {
                            let ratio = compressedSize > 0 ? Double(sourceFileSize) / Double(compressedSize) : 0
                            self.successMessage = "✅ \(origStr) → \(compStr) (\(String(format: "%.1f", ratio)):1) · \(self.codec.rawValue)"
                        }
                        self.appState.openFile(path: saveUrl.path)
                    } else {
                        self.errorMessage = "Failed to write .qtsq file."
                    }
                }
                
                qtsq_free(&ctx)
            }
        }
    }

    private func formatSize(_ size: UInt64) -> String {
        if size < 1024 { return "\(size) B" }
        if size < 1048576 { return String(format: "%.1f KB", Double(size) / 1024.0) }
        return String(format: "%.1f MB", Double(size) / 1048576.0)
    }
    
    // ─── Container: Compress Folder ───
    
    private func compressFolder(path: String) {
        // Scan folder for media types
        let (hasImg, hasAud, hasVid) = scanFolderForMediaTypes(path: path)
        
        if hasImg || hasAud || hasVid {
            pendingFolderPaths = [path]
            pendingFolderIsFolder = true
            pendingFolderHasImages = hasImg
            pendingFolderHasAudio = hasAud
            pendingFolderHasVideo = hasVid
            showFolderQualityPicker = true
        } else {
            // No media files, compress directly
            compressFolderWithQuality(path: path, imageQuality: nil, audioMethod: nil, videoQuality: nil)
        }
    }
    
    private func compressFolderWithQuality(path: String, imageQuality: ImageQualityMode?, audioMethod: AudioCompressionMethod?, videoQuality: VideoQualityMode?) {
        self.errorMessage = nil
        self.successMessage = nil
        self.isCompressing = true
        self.encodeStatus = "Encoding stream to vault…"
        
        DispatchQueue.global(qos: .userInitiated).async {
            var ctx = qtsq_context_t()
            var result = qtsq_container_create(&ctx)
            
            if result == 0 {
                let fm = FileManager.default
                if let enumerator = fm.enumerator(atPath: path) {
                    while let file = enumerator.nextObject() as? String {
                        if file.hasPrefix(".") { continue }
                        let fullPath = URL(fileURLWithPath: path).appendingPathComponent(file).path
                        var isDir: ObjCBool = false
                        if fm.fileExists(atPath: fullPath, isDirectory: &isDir) {
                            if isDir.boolValue { continue }
                            let ext = URL(fileURLWithPath: file).pathExtension.lowercased()
                            if ext == "qtsq" || ext == "qtsw" { continue }
                            
                            var streamCtx = qtsq_context_t()
                            var srcSize = 0
                            let sRes = self.compressToContext(path: fullPath, audioMethod: audioMethod, imageQuality: imageQuality, videoQuality: videoQuality, ctx: &streamCtx, sourceFileSize: &srcSize)
                            
                            if sRes == 0 {
                                qtsq_container_add_stream(&ctx, &streamCtx, file)
                            }
                            qtsq_free(&streamCtx)
                        }
                    }
                }
            }
            if result == 0 {
                result = qtsq_container_pack(&ctx)
            }
            
            var count: UInt32 = 0
            if result == 0 {
                qtsq_container_get_count(&ctx, &count)
            }
            
            DispatchQueue.main.async {
                self.isCompressing = false
                
                if result != 0 {
                    if self.errorMessage == nil { self.errorMessage = String(cString: qtsq_error_string(result)) }
                    qtsq_free(&ctx)
                    return
                }
                
                let save = NSSavePanel()
                save.allowedContentTypes = [UTType(filenameExtension: "qtsq")!]
                let folderName = URL(fileURLWithPath: path).lastPathComponent
                save.nameFieldStringValue = folderName + ".qtsq"
                
                if save.runModal() == .OK, let saveUrl = save.url {
                    let writeResult = saveUrl.path.withCString { cPath -> Int32 in
                        if !qtsqPromptAndEncrypt(&ctx) { return -1 }
                        return qtsq_write(&ctx, cPath)
                    }
                    if writeResult == 0 {
                        var compressedSize: UInt64 = 0
                        if let attrs = try? FileManager.default.attributesOfItem(atPath: saveUrl.path),
                           let sz = attrs[FileAttributeKey.size] as? UInt64 {
                            compressedSize = sz
                        }
                        self.successMessage = "📦 Container: \(count) streams → \(self.formatSize(compressedSize)) · \(self.codec.rawValue)"
                        self.appState.openFile(path: saveUrl.path)
                    } else {
                        self.errorMessage = "Failed to write container."
                    }
                }
                qtsq_free(&ctx)
            }
        }
    }
    
    // ─── Container: Compress Multiple Files ───
    
    private func compressMultipleFiles(paths: [String]) {
        // Scan all paths for media types
        var hasImg = false, hasAud = false, hasVid = false
        let imageExts = Set(["jpg", "jpeg", "png", "bmp", "tiff", "tif", "gif", "heic", "webp", "ico"])
        let audioExts = Set(["mp3", "wav", "aac", "m4a", "flac", "ogg", "wma", "aiff", "aif"])
        let videoExts = Set(["mp4", "mov", "webm", "mkv", "avi", "m4v", "flv", "wmv", "3gp"])
        
        for p in paths {
            var isDir: ObjCBool = false
            FileManager.default.fileExists(atPath: p, isDirectory: &isDir)
            if isDir.boolValue {
                let (i, a, v) = scanFolderForMediaTypes(path: p)
                hasImg = hasImg || i; hasAud = hasAud || a; hasVid = hasVid || v
            } else {
                let ext = URL(fileURLWithPath: p).pathExtension.lowercased()
                if imageExts.contains(ext) { hasImg = true }
                if audioExts.contains(ext) { hasAud = true }
                if videoExts.contains(ext) { hasVid = true }
            }
        }
        
        if hasImg || hasAud || hasVid {
            pendingFolderPaths = paths
            pendingFolderIsFolder = false
            pendingFolderHasImages = hasImg
            pendingFolderHasAudio = hasAud
            pendingFolderHasVideo = hasVid
            showFolderQualityPicker = true
        } else {
            compressMultipleFilesWithQuality(paths: paths, imageQuality: nil, audioMethod: nil, videoQuality: nil)
        }
    }
    
    private func compressMultipleFilesWithQuality(paths: [String], imageQuality: ImageQualityMode?, audioMethod: AudioCompressionMethod?, videoQuality: VideoQualityMode?) {
        self.errorMessage = nil
        self.successMessage = nil
        self.isCompressing = true
        self.encodeStatus = "Encoding stream to vault…"
        
        DispatchQueue.global(qos: .userInitiated).async {
            var ctx = qtsq_context_t()
            var result = qtsq_container_create(&ctx)
            
            if result == 0 {
                let fm = FileManager.default
                for path in paths {
                    var isDir: ObjCBool = false
                    fm.fileExists(atPath: path, isDirectory: &isDir)
                    
                    if isDir.boolValue {
                        if let enumerator = fm.enumerator(atPath: path) {
                            let folderName = URL(fileURLWithPath: path).lastPathComponent
                            while let file = enumerator.nextObject() as? String {
                                if file.hasPrefix(".") { continue }
                                let fullPath = URL(fileURLWithPath: path).appendingPathComponent(file).path
                                var isSubDir: ObjCBool = false
                                if fm.fileExists(atPath: fullPath, isDirectory: &isSubDir) && !isSubDir.boolValue {
                                    let ext = URL(fileURLWithPath: file).pathExtension.lowercased()
                                    if ext == "qtsq" || ext == "qtsw" { continue }
                                    
                                    var streamCtx = qtsq_context_t()
                                    var srcSize = 0
                                    let sRes = self.compressToContext(path: fullPath, audioMethod: audioMethod, imageQuality: imageQuality, videoQuality: videoQuality, ctx: &streamCtx, sourceFileSize: &srcSize)
                                    
                                    if sRes == 0 {
                                        let relName = folderName + "/" + file
                                        qtsq_container_add_stream(&ctx, &streamCtx, relName)
                                    }
                                    qtsq_free(&streamCtx)
                                }
                            }
                        }
                    } else {
                        let filename = URL(fileURLWithPath: path).lastPathComponent
                        let ext = URL(fileURLWithPath: filename).pathExtension.lowercased()
                        if ext == "qtsq" || ext == "qtsw" { continue }
                        
                        var streamCtx = qtsq_context_t()
                        var srcSize = 0
                        let sRes = self.compressToContext(path: path, audioMethod: audioMethod, imageQuality: imageQuality, videoQuality: videoQuality, ctx: &streamCtx, sourceFileSize: &srcSize)
                        
                        if sRes == 0 {
                            qtsq_container_add_stream(&ctx, &streamCtx, filename)
                        }
                        qtsq_free(&streamCtx)
                    }
                }
            }
            if result == 0 {
                result = qtsq_container_pack(&ctx)
            }
            
            var count: UInt32 = 0
            if result == 0 {
                qtsq_container_get_count(&ctx, &count)
            }
            
            DispatchQueue.main.async {
                self.isCompressing = false
                
                if result != 0 {
                    if self.errorMessage == nil { self.errorMessage = String(cString: qtsq_error_string(result)) }
                    qtsq_free(&ctx)
                    return
                }
                
                let save = NSSavePanel()
                save.allowedContentTypes = [UTType(filenameExtension: "qtsq")!]
                save.nameFieldStringValue = "bundle.qtsq"
                
                if save.runModal() == .OK, let saveUrl = save.url {
                    let writeResult = saveUrl.path.withCString { cPath -> Int32 in
                        if !qtsqPromptAndEncrypt(&ctx) { return -1 }
                        return qtsq_write(&ctx, cPath)
                    }
                    if writeResult == 0 {
                        var compressedSize: UInt64 = 0
                        if let attrs = try? FileManager.default.attributesOfItem(atPath: saveUrl.path),
                           let sz = attrs[FileAttributeKey.size] as? UInt64 {
                            compressedSize = sz
                        }
                        self.successMessage = "📦 Container: \(count) streams → \(self.formatSize(compressedSize)) · \(self.codec.rawValue)"
                        self.appState.openFile(path: saveUrl.path)
                    } else {
                        self.errorMessage = "Failed to write container."
                    }
                }
                qtsq_free(&ctx)
            }
        }
    }
    
    private func processWithFFmpeg(path: String, videoQuality: VideoQualityMode?, ctx: inout qtsq_context_t, sourceFileSize: Int = 0) throws -> Int32 {
        let fm = FileManager.default
        let possiblePaths = ["/opt/homebrew/bin/ffmpeg", "/usr/local/bin/ffmpeg", "/usr/bin/ffmpeg"]
        guard let ffmpeg = possiblePaths.first(where: { fm.fileExists(atPath: $0) }) else {
            throw NSError(domain: "QTSQ", code: -2, userInfo: [NSLocalizedDescriptionKey: "AVFoundation cannot decode this video and FFmpeg is not installed.\n\nInstall FFmpeg (brew install ffmpeg) to compress .webm, .mkv, VP9/AV1, and camera RAW formats."])
        }
        let ffprobe = ffmpeg.replacingOccurrences(of: "ffmpeg", with: "ffprobe")
        if !fm.fileExists(atPath: ffprobe) {
             throw NSError(domain: "QTSQ", code: -2, userInfo: [NSLocalizedDescriptionKey: "ffprobe is missing."])
        }
        
        // 1. Get width, height, fps using ffprobe
        let probeTask = Process()
        probeTask.executableURL = URL(fileURLWithPath: ffprobe)
        probeTask.arguments = ["-v", "error", "-select_streams", "v:0", "-show_entries", "stream=width,height,r_frame_rate", "-of", "csv=s=x:p=0", path]
        let probePipe = Pipe()
        probeTask.standardOutput = probePipe
        try probeTask.run()
        let probeData = probePipe.fileHandleForReading.readDataToEndOfFile()
        probeTask.waitUntilExit()
        
        guard probeTask.terminationStatus == 0,
              let output = String(data: probeData, encoding: .utf8)?.trimmingCharacters(in: .whitespacesAndNewlines),
              let firstLine = output.components(separatedBy: "\n").first else {
            throw NSError(domain: "QTSQ", code: -3, userInfo: [NSLocalizedDescriptionKey: "FFprobe failed to read video metadata."])
        }
        
        // Output format: e.g "1920x1080x24/1"
        let parts = firstLine.components(separatedBy: "x")
        guard parts.count >= 3,
              let width = Int32(parts[0]),
              let height = Int32(parts[1]) else {
            throw NSError(domain: "QTSQ", code: -4, userInfo: [NSLocalizedDescriptionKey: "Invalid ffprobe output: \(output)"])
        }
        
        var fps: Int32 = 24
        let fpsParts = parts[2].components(separatedBy: "/")
        if fpsParts.count == 2, let num = Double(fpsParts[0]), let den = Double(fpsParts[1]), den > 0 {
            let actualFps = Int32(round(num / den))
            fps = actualFps > 0 ? actualFps : 24
        } else if let parsedFps = Int32(parts[2]), parsedFps > 0 {
            fps = parsedFps
        }

        // Duration + declared frame count for per-frame size budget.
        var durationSec: Double = 0
        var nbFrames: Int = 0
        let durTask = Process()
        durTask.executableURL = URL(fileURLWithPath: ffprobe)
        durTask.arguments = ["-v", "error", "-show_entries", "format=duration:stream=nb_frames",
                             "-of", "default=noprint_wrappers=1", path]
        let durPipe = Pipe()
        durTask.standardOutput = durPipe
        if (try? durTask.run()) != nil {
            let durData = durPipe.fileHandleForReading.readDataToEndOfFile()
            durTask.waitUntilExit()
            if let s = String(data: durData, encoding: .utf8) {
                for line in s.split(separator: "\n") {
                    if line.hasPrefix("duration="), let d = Double(line.dropFirst("duration=".count)) {
                        durationSec = d
                    } else if line.hasPrefix("nb_frames="), let n = Int(line.dropFirst("nb_frames=".count)) {
                        nbFrames = n
                    }
                }
            }
        }

        // Set QTSQ encoder flags
        var flags: Int32 = 0
        if videoQuality?.isPixel == true { flags = Int32(QTSQ_VIDEO_PIXEL) }
        else { flags = Int32(QTSQ_VIDEO_CONTINUOUS) }

        // Prefer source-size budget when available (nearly match input size).
        let frameEstimate = max(1, nbFrames > 0 ? nbFrames : Int(durationSec * Double(max(fps, 1))))
        let targetPayload = sourceFileSize > 0
            ? max(sourceFileSize / frameEstimate, 256)
            : (Int(width) * Int(height) * 4) / 10
        guard let enc = qtsq_video_encoder_create(&ctx, UInt32(width), UInt32(height), 3, UInt32(fps), flags, targetPayload) else {
            throw NSError(domain: "QTSQ", code: -5, userInfo: [NSLocalizedDescriptionKey: "Video encoder creation failed inside FFmpeg fallback."])
        }
        // Free on every exit path (throw after create leaked ~frame_count × bitstream before).
        var encFinished = false
        defer {
            if !encFinished { qtsq_video_encoder_free(enc) }
        }

        // 2. Stream first video stream as raw rgb24 (-map 0:v:0 handles
        //    files with audio/data streams and multiple video streams).
        let ffmpegTask = Process()
        ffmpegTask.executableURL = URL(fileURLWithPath: ffmpeg)
        ffmpegTask.arguments = [
            "-v", "error", "-i", path, "-map", "0:v:0",
            "-f", "image2pipe", "-vcodec", "rawvideo", "-pix_fmt", "rgb24",
            "-s", "\(width)x\(height)", "-"
        ]

        let videoPipe = Pipe()
        ffmpegTask.standardOutput = videoPipe
        // Capture stderr so we can surface decode failures instead of writing 0 frames.
        // MUST drain concurrently: if the 64 KB stderr pipe fills while we are
        // blocked on stdout, ffmpeg deadlocks and the UI spins forever.
        let errPipe = Pipe()
        ffmpegTask.standardError = errPipe
        try ffmpegTask.run()

        var errData = Data()
        let errLock = NSLock()
        let errGroup = DispatchGroup()
        errGroup.enter()
        DispatchQueue.global(qos: .utility).async {
            while true {
                let chunk = errPipe.fileHandleForReading.availableData
                if chunk.isEmpty { break }
                errLock.lock()
                errData.append(chunk)
                errLock.unlock()
            }
            errGroup.leave()
        }

        let frameSize = Int(width * height * 3) // rgb24 is 3 contiguous channels
        let handle = videoPipe.fileHandleForReading

        let rgbBuffer = UnsafeMutablePointer<UInt8>.allocate(capacity: frameSize)
        defer { rgbBuffer.deallocate() }

        var framesAdded = 0
        while true {
            var frameData = Data()
            while frameData.count < frameSize {
                let chunk = handle.readData(ofLength: frameSize - frameData.count)
                if chunk.isEmpty { break }
                frameData.append(chunk)
            }
            if frameData.count < frameSize { break }

            frameData.withUnsafeBytes { buffer in
                if let ptr = buffer.bindMemory(to: UInt8.self).baseAddress {
                    rgbBuffer.initialize(from: ptr, count: frameSize)
                    if qtsq_video_encoder_add_frame(enc, rgbBuffer) == 0 {
                        framesAdded += 1
                        if framesAdded % 24 == 0 {
                            let n = framesAdded
                            DispatchQueue.main.async {
                                self.encodeStatus = "Encoding… \(n) frames (FFmpeg)"
                            }
                        }
                    }
                }
            }
        }

        ffmpegTask.waitUntilExit()
        errGroup.wait()
        errLock.lock()
        let errMsg = String(data: errData, encoding: .utf8)?.trimmingCharacters(in: .whitespacesAndNewlines) ?? ""
        errLock.unlock()

        // Never ship an empty encode — that's how we produced 4.7 KB "videos".
        if framesAdded == 0 {
            qtsq_video_encoder_free(enc)
            encFinished = true
            let detail = errMsg.isEmpty ? "FFmpeg produced no video frames." : errMsg
            throw NSError(domain: "QTSQ", code: -3, userInfo: [NSLocalizedDescriptionKey: "Failed to decode video frames.\n\(detail)"])
        }

        // Classic Gravitational audio track (no Photon, no Opus).
        attachGravitationalAudio(to: enc, sourcePath: path)

        let res = qtsq_video_encoder_finish(enc)
        qtsq_video_encoder_free(enc)
        encFinished = true
        if res == 0 && sourceFileSize > 0 {
            ctx.header.original_size = UInt64(sourceFileSize)
        }
        return res
    }
}

/// Extract the source's first audio stream with ffmpeg and attach it to the
/// video encoder using Classic Gravitational (1D GFC + Wendland) coding.
/// Failures are non-fatal: the video is encoded without an audio track.
private func attachGravitationalAudio(to enc: OpaquePointer?, sourcePath: String) {
    guard let enc = enc else { return }
    let ffmpegPaths = ["/opt/homebrew/bin/ffmpeg", "/usr/local/bin/ffmpeg", "/usr/bin/ffmpeg"]
    guard let ffmpeg = ffmpegPaths.first(where: { FileManager.default.fileExists(atPath: $0) }) else { return }
    let tmpWav = URL(fileURLWithPath: NSTemporaryDirectory())
        .appendingPathComponent("qtsq_vid_aud_\(UUID().uuidString).wav")
    defer { try? FileManager.default.removeItem(at: tmpWav) }

    let task = Process()
    task.executableURL = URL(fileURLWithPath: ffmpeg)
    // "-map 0:a:0?" — the trailing ? makes a missing audio stream non-fatal.
    task.arguments = ["-v", "error", "-i", sourcePath, "-map", "0:a:0?",
                      "-ac", "1", "-ar", "44100", "-c:a", "pcm_s16le",
                      "-f", "wav", tmpWav.path]
    if (try? task.run()) != nil { task.waitUntilExit() }
    guard task.terminationStatus == 0,
          let af = try? AVAudioFile(forReading: tmpWav),
          af.length > 0,
          let pcm = AVAudioPCMBuffer(pcmFormat: af.processingFormat,
                                     frameCapacity: AVAudioFrameCount(af.length))
    else { return }
    guard (try? af.read(into: pcm)) != nil, pcm.frameLength > 0,
          let channels = pcm.floatChannelData else { return }

    let fmt = af.processingFormat
    let n = Int(pcm.frameLength)
    var out = [Int16](repeating: 0, count: n)
    for ch in 0..<Int(fmt.channelCount) {
        let data = channels[ch]
        for i in 0..<n {
            let v = max(-1.0, min(1.0, data[i]))
            let s = Int16(v * 32767.0)
            out[i] = ch == 0 ? s : Int16((Int32(out[i]) + Int32(s)) / 2)
        }
    }
    out.withUnsafeBufferPointer { bp in
        _ = qtsq_video_encoder_set_audio(enc, bp.baseAddress!, UInt32(n), 1, 44100)
    }
}

// ─── Folder Media Scanner ───

private func scanFolderForMediaTypes(path: String) -> (hasImages: Bool, hasAudio: Bool, hasVideo: Bool) {
    let imageExts = Set(["jpg", "jpeg", "png", "bmp", "tiff", "tif", "gif", "heic", "webp", "ico"])
    let audioExts = Set(["mp3", "wav", "aac", "m4a", "flac", "ogg", "wma", "aiff", "aif"])
    let videoExts = Set(["mp4", "mov", "webm", "mkv", "avi", "m4v", "flv", "wmv", "3gp"])
    
    var hasImg = false, hasAud = false, hasVid = false
    
    if let enumerator = FileManager.default.enumerator(atPath: path) {
        while let file = enumerator.nextObject() as? String {
            let ext = URL(fileURLWithPath: file).pathExtension.lowercased()
            if imageExts.contains(ext) { hasImg = true }
            if audioExts.contains(ext) { hasAud = true }
            if videoExts.contains(ext) { hasVid = true }
            if hasImg && hasAud && hasVid { break }
        }
    }
    
    return (hasImg, hasAud, hasVid)
}

// ─── Folder Quality Picker Sheet (Combined) ───

struct FolderQualityPickerSheet: View {
    let hasImages: Bool
    let hasAudio: Bool
    let hasVideo: Bool
    let onSelect: (ImageQualityMode?, AudioCompressionMethod?, VideoQualityMode?) -> Void
    let onCancel: () -> Void
    
    @State private var imageQuality: ImageQualityMode = .hdSplat
    @State private var audioMethod: AudioCompressionMethod = .standard
    @State private var videoQuality: VideoQualityMode = .continuous4DGS
    
    var body: some View {
        VStack(spacing: 24) {
            // Header
            VStack(spacing: 8) {
                Image(systemName: "folder.fill.badge.gearshape")
                    .font(.system(size: 44))
                    .foregroundColor(.orange)
                
                Text("Container Quality Options")
                    .font(.system(size: 20, weight: .bold))
                
                Text("Choose compression settings for media files")
                    .font(.system(size: 13))
                    .foregroundColor(.secondary)
            }
            
            // Image quality
            if hasImages {
                VStack(alignment: .leading, spacing: 10) {
                    Label("Images", systemImage: "photo.fill")
                        .font(.system(size: 14, weight: .bold))
                        .foregroundColor(.purple)
                    
                    HStack(spacing: 12) {
                        qualityButton(
                            title: "HD", subtitle: "Best quality",
                            icon: "sparkles", color: .purple,
                            isSelected: imageQuality == .hdSplat
                        ) { imageQuality = .hdSplat }
                        
                        qualityButton(
                            title: "Compact", subtitle: "Small size",
                            icon: "arrow.down.right.circle.fill", color: .blue,
                            isSelected: imageQuality == .compactSplat
                        ) { imageQuality = .compactSplat }
                        
                        qualityButton(
                            title: "Lossless", subtitle: "Max fidelity",
                            icon: "diamond.fill", color: .orange,
                            isSelected: imageQuality == .losslessSplat
                        ) { imageQuality = .losslessSplat }
                    }
                }
                .padding(14)
                .background(Color.purple.opacity(0.05))
                .cornerRadius(14)
            }
            
            // Audio quality
            if hasAudio {
                VStack(alignment: .leading, spacing: 10) {
                    Label("Audio", systemImage: "waveform")
                        .font(.system(size: 14, weight: .bold))
                        .foregroundColor(.blue)
                    
                    HStack(spacing: 12) {
                        qualityButton(
                            title: "Standard", subtitle: "Auto · MP3 size",
                            icon: "waveform.path", color: .orange,
                            isSelected: audioMethod == .standard
                        ) { audioMethod = .standard }

                        qualityButton(
                            title: "Photon", subtitle: "Coming later",
                            icon: "atom", color: .purple,
                            isSelected: audioMethod == .photon,
                            isEnabled: false
                        ) { }
                    }
                }
                .padding(14)
                .background(Color.blue.opacity(0.05))
                .cornerRadius(14)
            }
            
            // Video quality
            if hasVideo {
                VStack(alignment: .leading, spacing: 10) {
                    Label("Video", systemImage: "film")
                        .font(.system(size: 14, weight: .bold))
                        .foregroundColor(.red)
                    
                    HStack(spacing: 12) {
                        qualityButton(
                            title: "4DGS", subtitle: "Gaussian",
                            icon: "atom", color: .pink,
                            isSelected: videoQuality == .continuous4DGS
                        ) { videoQuality = .continuous4DGS }

                        qualityButton(
                            title: "Pixel High", subtitle: "mp4-like",
                            icon: "film", color: .orange,
                            isSelected: videoQuality == .pixelHigh
                        ) { videoQuality = .pixelHigh }

                        qualityButton(
                            title: "Pixel Med", subtitle: "smaller",
                            icon: "film", color: .orange,
                            isSelected: videoQuality == .pixelMedium
                        ) { videoQuality = .pixelMedium }

                        qualityButton(
                            title: "Pixel Low", subtitle: "smallest",
                            icon: "film", color: .red,
                            isSelected: videoQuality == .pixelLow
                        ) { videoQuality = .pixelLow }

                        qualityButton(
                            title: "Pixel Lossless", subtitle: "exact",
                            icon: "film", color: .blue,
                            isSelected: videoQuality == .pixelLossless
                        ) { videoQuality = .pixelLossless }
                    }
                }
                .padding(14)
                .background(Color.red.opacity(0.05))
                .cornerRadius(14)
            }
            
            // Actions
            HStack(spacing: 16) {
                Button(action: onCancel) {
                    Text("Cancel")
                        .font(.system(size: 14, weight: .medium))
                        .foregroundColor(.secondary)
                        .padding(.horizontal, 20)
                        .padding(.vertical, 10)
                }
                .buttonStyle(PlainButtonStyle())
                
                Button(action: {
                    onSelect(
                        hasImages ? imageQuality : nil,
                        hasAudio ? audioMethod : nil,
                        hasVideo ? videoQuality : nil
                    )
                }) {
                    Text("Compress")
                        .font(.system(size: 14, weight: .bold))
                        .foregroundColor(.white)
                        .padding(.horizontal, 28)
                        .padding(.vertical, 10)
                        .background(Color.orange)
                        .cornerRadius(20)
                }
                .buttonStyle(PlainButtonStyle())
            }
        }
        .padding(32)
        .frame(width: 460)
    }
    
    private func qualityButton(title: String, subtitle: String, icon: String, color: Color, isSelected: Bool, isEnabled: Bool = true, action: @escaping () -> Void) -> some View {
        Button(action: action) {
            VStack(spacing: 6) {
                Image(systemName: icon)
                    .font(.system(size: 20))
                    .foregroundColor(isSelected ? color : .secondary)
                Text(title)
                    .font(.system(size: 12, weight: .bold))
                    .foregroundColor(isSelected ? .primary : .secondary)
                Text(subtitle)
                    .font(.system(size: 9))
                    .foregroundColor(.secondary)
            }
            .frame(maxWidth: .infinity)
            .padding(10)
            .background(isSelected ? color.opacity(0.12) : Color.clear)
            .cornerRadius(10)
            .overlay(RoundedRectangle(cornerRadius: 10)
                .stroke(isSelected ? color : Color.clear, lineWidth: 2))
        }
        .buttonStyle(PlainButtonStyle())
        .disabled(!isEnabled)
        .opacity(isEnabled ? 1.0 : 0.45)
    }
}

// ─── Audio Compression Method Picker Sheet ───

struct AudioMethodPickerSheet: View {
    let onSelect: (AudioCompressionMethod) -> Void
    let onCancel: () -> Void
    
    var body: some View {
        VStack(spacing: 24) {
            // Header
            VStack(spacing: 8) {
                Image(systemName: "waveform.circle.fill")
                    .font(.system(size: 44))
                    .foregroundColor(.orange)
                
                Text("Audio Compression Method")
                    .font(.system(size: 20, weight: .bold))
                
                Text("Standard matches your file's size. Photon is coming later.")
                    .font(.system(size: 13))
                    .foregroundColor(.secondary)
            }
            
            // Two formats: the working MP3-class Standard path, and the
            // Photon spatial codec as a visible placeholder.
            VStack(spacing: 10) {
                ForEach(AudioCompressionMethod.allCases, id: \.self) { method in
                    Button(action: {
                        if method.isAvailable { onSelect(method) }
                    }) {
                        HStack(spacing: 14) {
                            Image(systemName: method.icon)
                                .font(.system(size: 22, weight: .medium))
                                .foregroundColor(method.color)
                                .frame(width: 38)
                            
                            VStack(alignment: .leading, spacing: 3) {
                                Text(method.title)
                                    .font(.system(size: 14, weight: .semibold))
                                    .foregroundColor(.primary)
                                Text(method.subtitle)
                                    .font(.system(size: 11))
                                    .foregroundColor(.secondary)
                            }
                            
                            Spacer()
                            
                            if method.isAvailable {
                                Image(systemName: "chevron.right")
                                    .foregroundColor(.secondary)
                            } else {
                                Text("SOON")
                                    .font(.system(size: 9, weight: .bold))
                                    .foregroundColor(.secondary)
                                    .padding(.horizontal, 7)
                                    .padding(.vertical, 3)
                                    .background(Color.secondary.opacity(0.1))
                                    .cornerRadius(6)
                            }
                        }
                        .padding(14)
                        .background(method.color.opacity(0.08))
                        .cornerRadius(12)
                        .overlay(RoundedRectangle(cornerRadius: 12)
                            .stroke(method.color.opacity(0.2), lineWidth: 1))
                    }
                    .buttonStyle(PlainButtonStyle())
                    .disabled(!method.isAvailable)
                    .opacity(method.isAvailable ? 1.0 : 0.5)
                }
            }
            
            // Cancel
            Button(action: onCancel) {
                Text("Cancel")
                    .font(.system(size: 13, weight: .medium))
                    .foregroundColor(.secondary)
            }
            .buttonStyle(PlainButtonStyle())
        }
        .padding(32)
        .frame(width: 420)
    }
}

struct ImageQualityPickerSheet: View {
    let onSelect: (ImageQualityMode) -> Void
    let onCancel: () -> Void
    @State private var selectedTier: String? = nil  // "hd", "compact", "lossless"
    
    var body: some View {
        VStack(spacing: 24) {
            if let tier = selectedTier {
                // ── Step 2: Choose Method (Triangle, Hybrid, Splat) ──
                VStack(spacing: 8) {
                    Image(systemName: "wand.and.stars")
                        .font(.system(size: 44))
                        .foregroundColor(.cyan)
                    Text("Choose Rendering Method")
                        .font(.system(size: 20, weight: .bold))
                    Text("\(tier.uppercased()) tier selected")
                        .font(.system(size: 13))
                        .foregroundColor(.secondary)
                }
                
                VStack(spacing: 12) {
                    
                    // Option A: Triangle Mesh (Classic)
                    Button(action: {
                        switch tier {
                        case "hd": onSelect(.hdTriangle)
                        case "compact": onSelect(.compactTriangle)
                        default: onSelect(.losslessTriangle)
                        }
                    }) {
                        HStack(spacing: 16) {
                            Image(systemName: "triangle.fill")
                                .font(.system(size: 28))
                                .foregroundColor(.purple)
                                .frame(width: 44)
                            VStack(alignment: .leading, spacing: 4) {
                                HStack(spacing: 6) {
                                    Text("Triangle Mesh")
                                        .font(.system(size: 15, weight: .semibold))
                                        .foregroundColor(.primary)
                                    Text("CLASSIC")
                                        .font(.system(size: 9, weight: .heavy))
                                        .foregroundColor(.white)
                                        .padding(.horizontal, 6)
                                        .padding(.vertical, 2)
                                        .background(Color.purple)
                                        .cornerRadius(4)
                                }
                                Text("Adaptive subdivision wireframe")
                                    .font(.system(size: 11))
                                    .foregroundColor(.secondary)
                            }
                            Spacer()
                            Image(systemName: "chevron.right")
                                .foregroundColor(.secondary)
                        }
                        .padding(16)
                        .background(Color.purple.opacity(0.08))
                        .cornerRadius(14)
                        .overlay(RoundedRectangle(cornerRadius: 14)
                            .stroke(Color.purple.opacity(0.2), lineWidth: 1))
                    }
                    .buttonStyle(PlainButtonStyle())

                    // Option B: Triangle + Splats (Hybrid)
                    Button(action: {
                        switch tier {
                        case "hd": onSelect(.hdHybrid)
                        case "compact": onSelect(.compactHybrid)
                        default: onSelect(.losslessHybrid)
                        }
                    }) {
                        HStack(spacing: 16) {
                            Image(systemName: "triangle.circle.fill")
                                .font(.system(size: 28))
                                .foregroundColor(.pink)
                                .frame(width: 44)
                            VStack(alignment: .leading, spacing: 4) {
                                HStack(spacing: 6) {
                                    Text("Triangle + Splats")
                                        .font(.system(size: 15, weight: .semibold))
                                        .foregroundColor(.primary)
                                    Text("HYBRID")
                                        .font(.system(size: 9, weight: .heavy))
                                        .foregroundColor(.white)
                                        .padding(.horizontal, 6)
                                        .padding(.vertical, 2)
                                        .background(Color.pink)
                                        .cornerRadius(4)
                                }
                                Text("Mesh structure with splatted micro-details")
                                    .font(.system(size: 11))
                                    .foregroundColor(.secondary)
                            }
                            Spacer()
                            Image(systemName: "chevron.right")
                                .foregroundColor(.secondary)
                        }
                        .padding(16)
                        .background(Color.pink.opacity(0.08))
                        .cornerRadius(14)
                        .overlay(RoundedRectangle(cornerRadius: 14)
                            .stroke(Color.pink.opacity(0.2), lineWidth: 1))
                    }
                    .buttonStyle(PlainButtonStyle())

                    // Option C: Pure Splats Only
                    Button(action: {
                        switch tier {
                        case "hd": onSelect(.hdSplat)
                        case "compact": onSelect(.compactSplat)
                        default: onSelect(.losslessSplat)
                        }
                    }) {
                        HStack(spacing: 16) {
                            Image(systemName: "circle.hexagongrid.fill")
                                .font(.system(size: 28))
                                .foregroundColor(.cyan)
                                .frame(width: 44)
                            VStack(alignment: .leading, spacing: 4) {
                                HStack(spacing: 6) {
                                    Text("Pure Splats Only")
                                        .font(.system(size: 15, weight: .semibold))
                                        .foregroundColor(.primary)
                                    Text("NEW")
                                        .font(.system(size: 9, weight: .heavy))
                                        .foregroundColor(.white)
                                        .padding(.horizontal, 6)
                                        .padding(.vertical, 2)
                                        .background(Color.cyan)
                                        .cornerRadius(4)
                                }
                                Text("Gaussian splats • No mesh • Fast decode")
                                    .font(.system(size: 11))
                                    .foregroundColor(.secondary)
                            }
                            Spacer()
                            Image(systemName: "chevron.right")
                                .foregroundColor(.secondary)
                        }
                        .padding(16)
                        .background(Color.cyan.opacity(0.08))
                        .cornerRadius(14)
                        .overlay(RoundedRectangle(cornerRadius: 14)
                            .stroke(Color.cyan.opacity(0.2), lineWidth: 1))
                    }
                    .buttonStyle(PlainButtonStyle())

                }
                
                Button(action: { selectedTier = nil }) {
                    Text("← Back")
                        .font(.system(size: 13, weight: .medium))
                        .foregroundColor(.secondary)
                }
                .buttonStyle(PlainButtonStyle())
                
            } else {
                // ── Step 1: Choose Tier (HD / Compact / Lossless) ──
                VStack(spacing: 8) {
                    Image(systemName: "photo.circle.fill")
                        .font(.system(size: 44))
                        .foregroundColor(.orange)
                    Text("Image Compression Quality")
                        .font(.system(size: 20, weight: .bold))
                    Text("Choose quality tier for this image")
                        .font(.system(size: 13))
                        .foregroundColor(.secondary)
                }
                
                VStack(spacing: 12) {
                    Button(action: { withAnimation(.easeInOut(duration: 0.2)) { selectedTier = "hd" } }) {
                        HStack(spacing: 16) {
                            Image(systemName: "sparkles")
                                .font(.system(size: 28))
                                .foregroundColor(.purple)
                                .frame(width: 44)
                            VStack(alignment: .leading, spacing: 4) {
                                Text("QTSQ HD")
                                    .font(.system(size: 15, weight: .semibold))
                                    .foregroundColor(.primary)
                                Text("Best quality • High detail • Beats JPEG/WebP")
                                    .font(.system(size: 11))
                                    .foregroundColor(.secondary)
                            }
                            Spacer()
                            Image(systemName: "chevron.right")
                                .foregroundColor(.secondary)
                        }
                        .padding(16)
                        .background(Color.purple.opacity(0.08))
                        .cornerRadius(14)
                        .overlay(RoundedRectangle(cornerRadius: 14)
                            .stroke(Color.purple.opacity(0.2), lineWidth: 1))
                    }
                    .buttonStyle(PlainButtonStyle())
                    
                    Button(action: { withAnimation(.easeInOut(duration: 0.2)) { selectedTier = "compact" } }) {
                        HStack(spacing: 16) {
                            Image(systemName: "arrow.down.right.and.arrow.up.left")
                                .font(.system(size: 28))
                                .foregroundColor(.green)
                                .frame(width: 44)
                            VStack(alignment: .leading, spacing: 4) {
                                Text("QTSQ Compact")
                                    .font(.system(size: 15, weight: .semibold))
                                    .foregroundColor(.primary)
                                Text("Smallest file size • Efficient encoding")
                                    .font(.system(size: 11))
                                    .foregroundColor(.secondary)
                            }
                            Spacer()
                            Image(systemName: "chevron.right")
                                .foregroundColor(.secondary)
                        }
                        .padding(16)
                        .background(Color.green.opacity(0.08))
                        .cornerRadius(14)
                        .overlay(RoundedRectangle(cornerRadius: 14)
                            .stroke(Color.green.opacity(0.2), lineWidth: 1))
                    }
                    .buttonStyle(PlainButtonStyle())
                    
                    Button(action: { withAnimation(.easeInOut(duration: 0.2)) { selectedTier = "lossless" } }) {
                        HStack(spacing: 16) {
                            Image(systemName: "diamond.fill")
                                .font(.system(size: 28))
                                .foregroundColor(.orange)
                                .frame(width: 44)
                            VStack(alignment: .leading, spacing: 4) {
                                HStack(spacing: 6) {
                                    Text("QTSQ Lossless")
                                        .font(.system(size: 15, weight: .semibold))
                                        .foregroundColor(.primary)
                                    Text("MAX")
                                        .font(.system(size: 9, weight: .heavy))
                                        .foregroundColor(.white)
                                        .padding(.horizontal, 6)
                                        .padding(.vertical, 2)
                                        .background(Color.orange)
                                        .cornerRadius(4)
                                }
                                Text("Maximum fidelity • Near-original quality")
                                    .font(.system(size: 11))
                                    .foregroundColor(.secondary)
                            }
                            Spacer()
                            Image(systemName: "chevron.right")
                                .foregroundColor(.secondary)
                        }
                        .padding(16)
                        .background(Color.orange.opacity(0.08))
                        .cornerRadius(14)
                        .overlay(RoundedRectangle(cornerRadius: 14)
                            .stroke(Color.orange.opacity(0.2), lineWidth: 1))
                    }
                    .buttonStyle(PlainButtonStyle())
                    
                    // ── Pixel Mode: direct select, no method sub-step ──
                    Button(action: { onSelect(.pixel) }) {
                        HStack(spacing: 16) {
                            Image(systemName: "square.grid.3x3.fill")
                                .font(.system(size: 28))
                                .foregroundColor(.blue)
                                .frame(width: 44)
                            VStack(alignment: .leading, spacing: 4) {
                                HStack(spacing: 6) {
                                    Text("QTSQ Pixel")
                                        .font(.system(size: 15, weight: .semibold))
                                        .foregroundColor(.primary)
                                    Text("JPEG SIZE")
                                        .font(.system(size: 9, weight: .heavy))
                                        .foregroundColor(.white)
                                        .padding(.horizontal, 6)
                                        .padding(.vertical, 2)
                                        .background(Color.blue)
                                        .cornerRadius(4)
                                }
                                Text("True pixels • Same file size as JPEG • Perfect quality")
                                    .font(.system(size: 11))
                                    .foregroundColor(.secondary)
                            }
                            Spacer()
                            Image(systemName: "chevron.right")
                                .foregroundColor(.secondary)
                        }
                        .padding(16)
                        .background(Color.blue.opacity(0.08))
                        .cornerRadius(14)
                        .overlay(RoundedRectangle(cornerRadius: 14)
                            .stroke(Color.blue.opacity(0.2), lineWidth: 1))
                    }
                    .buttonStyle(PlainButtonStyle())
                }
                
                Button(action: onCancel) {
                    Text("Cancel")
                        .font(.system(size: 13, weight: .medium))
                        .foregroundColor(.secondary)
                }
                .buttonStyle(PlainButtonStyle())
            }
        }
        .padding(32)
        .frame(width: 420)
        .animation(.easeInOut(duration: 0.2), value: selectedTier)
    }
}

struct VideoQualityPickerSheet: View {
    let onSelect: (VideoQualityMode) -> Void
    let onCancel: () -> Void

    var body: some View {
        VStack(spacing: 24) {
            VStack(spacing: 8) {
                Image(systemName: "film.circle.fill")
                    .font(.system(size: 44))
                    .foregroundColor(.orange)

                Text("Video Compression Mode")
                    .font(.system(size: 20, weight: .bold))

                Text("Choose how to encode this video. 4DGS uses Gaussian splats; Pixel uses mp4-like frame prediction.")
                    .font(.system(size: 13))
                    .foregroundColor(.secondary)
                    .multilineTextAlignment(.center)
            }

            VStack(spacing: 12) {
                // Pixel High — custom QTC DCT + motion compensation (near-input size)
                Button(action: { onSelect(.pixelHigh) }) {
                    HStack(spacing: 16) {
                        ZStack {
                            Image(systemName: "film")
                                .font(.system(size: 28))
                                .foregroundStyle(
                                    LinearGradient(
                                        colors: [.orange, .red],
                                        startPoint: .topLeading,
                                        endPoint: .bottomTrailing
                                    )
                                )
                                .frame(width: 44)
                        }

                        VStack(alignment: .leading, spacing: 4) {
                            HStack(spacing: 6) {
                                Text("Pixel High")
                                    .font(.system(size: 15, weight: .semibold))
                                Text("mp4-like")
                                    .font(.system(size: 8, weight: .heavy))
                                    .foregroundColor(.white)
                                    .padding(.horizontal, 5).padding(.vertical, 2)
                                    .background(
                                        LinearGradient(
                                            colors: [.orange, .red],
                                            startPoint: .leading,
                                            endPoint: .trailing
                                        )
                                    )
                                    .cornerRadius(4)
                            }
                            Text("Custom quant lossy • Near input size • Fast")
                                .font(.system(size: 11))
                                .foregroundColor(.secondary)
                        }
                        Spacer()
                    }
                    .padding(16)
                    .background(
                        LinearGradient(
                            colors: [Color.orange.opacity(0.08), Color.red.opacity(0.06)],
                            startPoint: .leading,
                            endPoint: .trailing
                        )
                    )
                    .cornerRadius(14)
                    .overlay(
                        RoundedRectangle(cornerRadius: 14)
                            .stroke(
                                LinearGradient(
                                    colors: [.orange.opacity(0.3), .red.opacity(0.2)],
                                    startPoint: .topLeading,
                                    endPoint: .bottomTrailing
                                ),
                                lineWidth: 1
                            )
                    )
                }
                .buttonStyle(PlainButtonStyle())

                // Pixel Medium / Low / Lossless — compact row
                HStack(spacing: 10) {
                    Button(action: { onSelect(.pixelMedium) }) {
                        VStack(spacing: 4) {
                            Text("Pixel Medium")
                                .font(.system(size: 12, weight: .semibold))
                            Text("balanced size")
                                .font(.system(size: 10))
                                .foregroundColor(.secondary)
                        }
                        .frame(maxWidth: .infinity)
                        .padding(.vertical, 12)
                        .background(Color.orange.opacity(0.08))
                        .cornerRadius(10)
                        .overlay(RoundedRectangle(cornerRadius: 10)
                            .stroke(Color.orange.opacity(0.25), lineWidth: 1))
                    }
                    .buttonStyle(PlainButtonStyle())

                    Button(action: { onSelect(.pixelLow) }) {
                        VStack(spacing: 4) {
                            Text("Pixel Low")
                                .font(.system(size: 12, weight: .semibold))
                            Text("smallest")
                                .font(.system(size: 10))
                                .foregroundColor(.secondary)
                        }
                        .frame(maxWidth: .infinity)
                        .padding(.vertical, 12)
                        .background(Color.red.opacity(0.08))
                        .cornerRadius(10)
                        .overlay(RoundedRectangle(cornerRadius: 10)
                            .stroke(Color.red.opacity(0.25), lineWidth: 1))
                    }
                    .buttonStyle(PlainButtonStyle())

                    Button(action: { onSelect(.pixelLossless) }) {
                        VStack(spacing: 4) {
                            Text("Lossless")
                                .font(.system(size: 12, weight: .semibold))
                            Text("bit-exact")
                                .font(.system(size: 10))
                                .foregroundColor(.secondary)
                        }
                        .frame(maxWidth: .infinity)
                        .padding(.vertical, 12)
                        .background(Color.blue.opacity(0.08))
                        .cornerRadius(10)
                        .overlay(RoundedRectangle(cornerRadius: 10)
                            .stroke(Color.blue.opacity(0.25), lineWidth: 1))
                    }
                    .buttonStyle(PlainButtonStyle())
                }

                // 4DGS Continuous — velocity-based Gaussian splat field
                Button(action: { onSelect(.continuous4DGS) }) {
                    HStack(spacing: 16) {
                        ZStack {
                            Image(systemName: "atom")
                                .font(.system(size: 28))
                                .foregroundStyle(
                                    LinearGradient(
                                        colors: [.pink, .purple, .blue],
                                        startPoint: .topLeading,
                                        endPoint: .bottomTrailing
                                    )
                                )
                                .frame(width: 44)
                        }

                        VStack(alignment: .leading, spacing: 4) {
                            HStack(spacing: 6) {
                                Text("4DGS Continuous")
                                    .font(.system(size: 15, weight: .semibold))
                                Text("NEW")
                                    .font(.system(size: 8, weight: .heavy))
                                    .foregroundColor(.white)
                                    .padding(.horizontal, 5).padding(.vertical, 2)
                                    .background(
                                        LinearGradient(
                                            colors: [.pink, .purple],
                                            startPoint: .leading,
                                            endPoint: .trailing
                                        )
                                    )
                                    .cornerRadius(4)
                            }
                            Text("Gaussian splat velocity field • Resolution independent • Any framerate")
                                .font(.system(size: 11))
                                .foregroundColor(.secondary)
                        }
                        Spacer()
                    }
                    .padding(16)
                    .background(
                        LinearGradient(
                            colors: [Color.purple.opacity(0.08), Color.pink.opacity(0.06)],
                            startPoint: .leading,
                            endPoint: .trailing
                        )
                    )
                    .cornerRadius(14)
                    .overlay(
                        RoundedRectangle(cornerRadius: 14)
                            .stroke(
                                LinearGradient(
                                    colors: [.purple.opacity(0.3), .pink.opacity(0.2)],
                                    startPoint: .topLeading,
                                    endPoint: .bottomTrailing
                                ),
                                lineWidth: 1
                            )
                    )
                }
                .buttonStyle(PlainButtonStyle())

            }

            Button("Cancel", action: onCancel)
                .buttonStyle(PlainButtonStyle())
                .foregroundColor(.secondary)
        }
        .padding(32)
        .frame(width: 460)
    }
}
