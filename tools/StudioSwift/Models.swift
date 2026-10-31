import Foundation
import SwiftUI
import AppKit

// MARK: - Vault Design Tokens (Signal meets Linear meets Swiss Bank Vault)

public struct VaultTheme {
    // Backgrounds & Base Surfaces
    public static let paperWhite = Color(red: 0.980, green: 0.980, blue: 0.972) // #FAFAF8
    public static let cardWhite = Color.white                                   // #FFFFFF
    public static let sidebarBg = Color(red: 0.965, green: 0.965, blue: 0.957) // Warm off-white
    public static let subpanelBg = Color(red: 0.95, green: 0.95, blue: 0.94)
    
    // Borders & Separators (Strict 6% black)
    public static let border = Color.black.opacity(0.06)
    public static let borderHover = Color.black.opacity(0.12)
    public static let separator = Color.black.opacity(0.05)
    
    // Core Accent Hierarchy
    public static let emberOrange = Color(red: 1.0, green: 0.549, blue: 0.0)      // #FF8C00 Signature primary
    public static let emberOrangeBg = Color(red: 1.0, green: 0.549, blue: 0.0).opacity(0.08)
    
    public static let provenanceTeal = Color(red: 0.051, green: 0.463, blue: 0.431) // #0D766E Deep Teal (provenance only)
    public static let provenanceTealBg = Color(red: 0.051, green: 0.463, blue: 0.431).opacity(0.08)
    
    public static let integrityGreen = Color(red: 0.086, green: 0.643, blue: 0.290) // #16A34A Clean Green (integrity only)
    public static let integrityGreenBg = Color(red: 0.086, green: 0.643, blue: 0.290).opacity(0.08)
    
    public static let dangerRed = Color(red: 0.863, green: 0.149, blue: 0.149)       // #DC2626 Warning/danger
    public static let dangerRedBg = Color(red: 0.863, green: 0.149, blue: 0.149).opacity(0.08)
    
    // Typography Colors
    public static let textPrimary = Color.black.opacity(0.88)
    public static let textSecondary = Color.black.opacity(0.50)
    public static let textTertiary = Color.black.opacity(0.35)
    
    // Shadows
    public static let cardShadowColor = Color.black.opacity(0.03)
}

// MARK: - View Modifiers

public struct VaultCardModifier: ViewModifier {
    var cornerRadius: CGFloat = 10
    var hasShadow: Bool = true
    
    public func body(content: Content) -> some View {
        content
            .background(VaultTheme.cardWhite)
            .cornerRadius(cornerRadius)
            .overlay(
                RoundedRectangle(cornerRadius: cornerRadius, style: .continuous)
                    .stroke(VaultTheme.border, lineWidth: 1)
            )
            .shadow(color: hasShadow ? VaultTheme.cardShadowColor : .clear, radius: 6, x: 0, y: 2)
    }
}

public struct VaultGhostButtonModifier: ViewModifier {
    var isSelected: Bool = false
    
    public func body(content: Content) -> some View {
        content
            .font(.system(size: 12, weight: .medium))
            .foregroundColor(isSelected ? VaultTheme.emberOrange : VaultTheme.textPrimary)
            .padding(.horizontal, 14)
            .padding(.vertical, 6)
            .background(isSelected ? VaultTheme.emberOrangeBg : VaultTheme.cardWhite)
            .cornerRadius(8)
            .overlay(
                RoundedRectangle(cornerRadius: 8, style: .continuous)
                    .stroke(isSelected ? VaultTheme.emberOrange.opacity(0.4) : VaultTheme.border, lineWidth: 1)
            )
            .shadow(color: VaultTheme.cardShadowColor, radius: 3, x: 0, y: 1)
    }
}

extension View {
    public func vaultCard(cornerRadius: CGFloat = 10, hasShadow: Bool = true) -> some View {
        self.modifier(VaultCardModifier(cornerRadius: cornerRadius, hasShadow: hasShadow))
    }
    
    public func vaultGhostButton(isSelected: Bool = false) -> some View {
        self.modifier(VaultGhostButtonModifier(isSelected: isSelected))
    }
}

// MARK: - Brand Ring Mark Vector Component

public struct BrandRingMark: View {
    var size: CGFloat = 80
    var lineWidth: CGFloat = 4.5
    
    public init(size: CGFloat = 80, lineWidth: CGFloat = 4.5) {
        self.size = size
        self.lineWidth = lineWidth
    }
    
    public var body: some View {
        ZStack {
            // Background subtle glow / ring
            Circle()
                .stroke(
                    LinearGradient(
                        colors: [
                            VaultTheme.emberOrange.opacity(0.95),
                            VaultTheme.emberOrange.opacity(0.35),
                            VaultTheme.emberOrange.opacity(0.9)
                        ],
                        startPoint: .topLeading,
                        endPoint: .bottomTrailing
                    ),
                    lineWidth: lineWidth
                )
                .frame(width: size, height: size)
                .rotationEffect(.degrees(-25))
            
            // Overlapping inner fluid dynamic circle segment
            Circle()
                .trim(from: 0.15, to: 0.85)
                .stroke(
                    VaultTheme.emberOrange,
                    style: StrokeStyle(lineWidth: lineWidth * 0.85, lineCap: .round)
                )
                .frame(width: size * 0.82, height: size * 0.82)
                .rotationEffect(.degrees(45))
        }
        .frame(width: size + 8, height: size + 8)
    }
}

// MARK: - Intent Chips (Creative vs Provenance)

public struct IntentChip: View {
    public enum IntentType {
        case creative
        case provenance
    }
    
    public let type: IntentType
    public var isSelected: Bool = true
    
    public init(type: IntentType, isSelected: Bool = true) {
        self.type = type
        self.isSelected = isSelected
    }
    
    public var body: some View {
        HStack(spacing: 5) {
            Circle()
                .fill(type == .creative ? VaultTheme.emberOrange : VaultTheme.provenanceTeal)
                .frame(width: 5, height: 5)
            
            Text(type == .creative ? "Creative" : "Provenance")
                .font(.system(size: 11, weight: .semibold))
                .foregroundColor(type == .creative ? VaultTheme.emberOrange : VaultTheme.provenanceTeal)
        }
        .padding(.horizontal, 10)
        .padding(.vertical, 4)
        .background(
            type == .creative ? VaultTheme.emberOrangeBg : VaultTheme.provenanceTealBg
        )
        .cornerRadius(6)
        .overlay(
            RoundedRectangle(cornerRadius: 6, style: .continuous)
                .stroke(
                    (type == .creative ? VaultTheme.emberOrange : VaultTheme.provenanceTeal).opacity(0.35),
                    lineWidth: 1
                )
        )
    }
}

enum PanelType: Hashable {
    case welcome
    case viewer(UUID) // Associated with a file ID
    case info(UUID)
    case encrypt(UUID)
    case health(UUID)
    case compress
    case decompress
    case diff
    case settings
}

class QTSQFile: Identifiable, ObservableObject {
    let id = UUID()
    let path: String
    let name: String
    
    var ctx: qtsq_context_t
    var data: Data?

    /// Temp file path used when decompressed text is too large to keep in RAM.
    /// The data property will be an OS-managed memory-mapped slice of this file.
    private var tempFileURL: URL?

    @Published var isLoaded: Bool = false
    @Published var isEncrypted: Bool = false
    /// True when the user has decrypted the file for viewing in this session.
    /// The file on disk stays encrypted; only the in-memory copy is readable.
    @Published var isUnlocked: Bool = false
    /// Plaintext working copy used while unlocked. Viewers that decode lazily
    /// (spectral dictionary / Photon HQ audio, video) read from this instead
    /// of the sealed ctx. nil when locked.
    var viewCtx: qtsq_context_t?
    /// Holds this file's share of the app-wide memory budget while a
    /// decrypted working copy exists. Released in freeViewCtx/deinit.
    fileprivate var copyToken: QTSQMemoryBudget.Token?
    /// True when the file is encrypted to this device's key (saved without a
    /// password) and can be opened without one.
    var isDeviceEncrypted: Bool {
        ctx.has_security != 0 && ctx.security.encryption_type == 2
    }
    @Published var errorMessage: String? = nil
    @Published var isSpectralDictionary: Bool = false
    @Published var isVideo: Bool = false
    @Published var isContainer: Bool = false

    // Actual file sizes from disk
    var sourceFileSize: UInt64 = 0
    var qtsqFileSize: UInt64 = 0
    
    /// True when the file is a single Triangle-strategy image that can be edited
    var isEditableMesh: Bool {
        ctx.header.strategy == 0x07 && !isContainer && !isVideo && !isEncrypted && !isSpectralDictionary
    }
    
    init(path: String) {
        self.path = path
        self.name = URL(fileURLWithPath: path).lastPathComponent
        self.ctx = qtsq_context_t()
        qtsq_init(&self.ctx)
        
        // Read actual .qtsq file size from disk
        if let attrs = try? FileManager.default.attributesOfItem(atPath: path),
           let sz = attrs[.size] as? UInt64 {
            self.qtsqFileSize = sz
        }
        
        readAndDecompress()
    }
    
    deinit {
        qtsq_free(&self.ctx)
        freeViewCtx()
        cleanupTempFile()
    }

    private func cleanupTempFile() {
        if let url = tempFileURL {
            try? FileManager.default.removeItem(at: url)
            tempFileURL = nil
        }
    }
    
    func readAndDecompress() {
        DispatchQueue.global(qos: .userInitiated).async { [self] in
            var localCtx = self.ctx
            let result = qtsq_read(&localCtx, path)
            self.ctx = localCtx
            
            if result != QTSQ_OK {
                DispatchQueue.main.async {
                    self.errorMessage = String(cString: qtsq_error_string(result))
                }
                return
            }
            
            // Use original_size from header as source file size
            // (this represents the raw uncompressed data size)
            self.sourceFileSize = self.ctx.header.original_size
            
            self.isEncrypted = qtsq_is_encrypted(&self.ctx) != 0

            /* Device-encrypted files (saved without a password) unlock
             * silently with this Mac's key — zero-friction open. */
            var deviceUnlocked = false
            if self.isEncrypted,
               self.isDeviceEncrypted,
               DeviceKeyStore.shared.isAvailable {
                if DeviceKeyStore.shared.decryptWithDeviceKey(&self.ctx) == QTSQ_OK {
                    deviceUnlocked = true
                    DispatchQueue.main.async { self.isEncrypted = false }
                }
            }
            
            // Detect Spectral Dictionary audio — custom player handles decode
            // Check encoding string from schema — most reliable detection
            let encodingStr = withUnsafePointer(to: self.ctx.schema.encoding) { ptr -> String in
                return ptr.withMemoryRebound(to: CChar.self, capacity: 64) { cStr in
                    return String(cString: cStr)
                }
            }
            let type = qtsq_data_type_t(UInt32(self.ctx.header.data_type))
            let isAudio = type == QTSQ_TYPE_AUDIO_MONO || type == QTSQ_TYPE_AUDIO_STEREO || type == QTSQ_TYPE_AUDIO_MULTI
            let isSpecDict = encodingStr == "SPEC_DICTIONARY" || encodingStr == "SPECTRAL_PEAKS" || encodingStr == "FULL_SPECTRUM" || encodingStr == "LOSSLESS_DELTA" || self.ctx.header.strategy == 0x03 /* FOURIER */ || self.ctx.header.strategy == 0x08 /* PHOTON */
            /* MDCT2 lossy stereo capture: PROCEDURAL + sample_depth 32 +
             * audio dtype (qtsq_compress_audio_float). Without this the
             * generic "decompress to file.data" branch would run and the
             * user would get raw float bytes instead of the audio player. */
            let isMdct2Float = isAudio && self.ctx.header.strategy == 0x01 /* PROCEDURAL */ && self.ctx.header.sample_depth == 32
            let isStruct = isAudio && (isSpecDict || isMdct2Float)
            let isVid = type == QTSQ_TYPE_VIDEO_FRAMES
            let isCont = type == QTSQ_TYPE_CONTAINER
            
            if isStruct || isVid || isCont {
                DispatchQueue.main.async {
                    if isStruct {
                        self.isSpectralDictionary = true
                    }
                    if isVid {
                        self.isVideo = true
                    }
                    if isCont {
                        self.isContainer = true
                    }
                    self.isLoaded = true
                }
                return
            }
            
            if !self.isEncrypted || deviceUnlocked {
                self.performDecompress()
            }
        }
    }
    
    func performDecompress() {
        DispatchQueue.global(qos: .userInitiated).async { [self] in
            var rawData: UnsafeMutablePointer<UInt8>? = nil
            var rawSize: Int = 0
            // Decompress directly — do NOT shallow-copy ctx (pointer aliasing corrupts singularity)
            let res = qtsq_decompress(&self.ctx, &rawData, &rawSize)
            guard res == QTSQ_OK, let dataPtr = rawData, rawSize > 0 else {
                DispatchQueue.main.async {
                    self.errorMessage = String(cString: qtsq_error_string(res))
                }
                return
            }

            // ── Large text files: write to temp, mmap, free the C buffer ──────
            // Keeps only accessed pages in RAM instead of pinning the full
            // decompressed corpus (which can be multiple GBs).
            let isText = qtsq_type_is_text(qtsq_data_type_t(UInt32(self.ctx.header.data_type))) != 0
            let largeCutoff = 8_000_000  // 8 MB threshold

            if isText && rawSize > largeCutoff {
                let tmpURL = URL(fileURLWithPath: NSTemporaryDirectory())
                    .appendingPathComponent("qtsq_\(self.id.uuidString).tmp")

                // Write raw bytes to temp file (sequential, very fast)
                let writeOK = FileManager.default.createFile(
                    atPath: tmpURL.path, contents: nil)
                if writeOK, let fh = FileHandle(forWritingAtPath: tmpURL.path) {
                    // Write in 16 MB chunks to bound peak stack/heap pressure
                    let chunk = 16 * 1024 * 1024
                    var written = 0
                    while written < rawSize {
                        let toWrite = min(chunk, rawSize - written)
                        fh.write(Data(bytesNoCopy: dataPtr.advanced(by: written),
                                      count: toWrite, deallocator: .none))
                        written += toWrite
                    }
                    fh.closeFile()
                    free(dataPtr)  // ← C buffer freed immediately; only temp file remains

                    // Memory-map the temp file — OS evicts pages not in use
                    if let mapped = try? Data(contentsOf: tmpURL, options: [.mappedRead, .alwaysMapped]) {
                        DispatchQueue.main.async {
                            self.tempFileURL = tmpURL
                            self.data = mapped
                            self.isLoaded = true
                        }
                        return
                    }
                }
                // Fallback: mmap failed, keep in RAM (better than crashing)
                free(dataPtr)
                DispatchQueue.main.async {
                    self.errorMessage = "Could not create temp file for large text corpus."
                }
                return
            }

            // ── Small file: zero-copy wrap — C buffer is the Data's backing store ──
            DispatchQueue.main.async {
                self.data = Data(bytesNoCopy: dataPtr, count: rawSize, deallocator: .free)
                self.isLoaded = true
            }
        }
    }
    
    /// Decrypts the file for viewing WITHOUT leaving it decrypted on disk.
    ///
    /// Sequence (all on a background queue, in this exact order):
    ///   1. vault-decrypt the in-memory context
    ///   2. decompress the now-plaintext payload into `data`
    ///   3. re-encrypt the context and write it back to disk
    ///      (the at-rest guarantee is preserved)
    ///   4. publish isUnlocked = true so the viewer renders the content
    /// Deep-copies the sealed context into an isolated working copy. Heap
    /// buffers the copy doesn't own are detached so freeing the copy can
    /// never double-free the original's memory.
    private func makeSealedCopy() -> qtsq_context_t? {
        var c = self.ctx
        c.transforms = nil; c.num_transforms = 0
        c.radiation_index = nil; c.num_radiation = 0
        c.wormholes = nil; c.num_wormholes = 0
        c.original_input = nil; c.original_input_size = 0
        guard let src = c.singularity, c.singularity_size > 0 else { return nil }
        let n = Int(c.singularity_size)
        // The sealed copy plus the decrypted copy both live in RAM. Reserve
        // both so a few large videos cannot quietly pass the budget.
        guard let token = QTSQMemoryBudget.Token(UInt64(n) * 2) else { return nil }
        self.copyToken = token
        let buf = UnsafeMutablePointer<UInt8>.allocate(capacity: n)
        buf.update(from: src, count: n)
        c.singularity = buf
        return c
    }

    /// Zeroes and releases the plaintext working copy.
    func freeViewCtx() {
        guard var c = viewCtx else { return }
        if let s = c.singularity, c.singularity_size > 0 {
            memset(s, 0, Int(c.singularity_size))
            s.deallocate()
        }
        c.singularity = nil
        viewCtx = nil
        copyToken = nil
    }

    func unlockForViewing(password: String) {
        DispatchQueue.global(qos: .userInitiated).async { [self] in
            /* Decrypt an isolated copy — self.ctx and the file on disk stay
             * sealed at all times. */
            guard var vctx = makeSealedCopy() else {
                DispatchQueue.main.async {
                    // makeSealedCopy returns nil both for "nothing to open"
                    // and when the memory budget is exhausted; distinguish so
                    // a large file does not look like a missing one.
                    self.errorMessage = self.copyToken == nil && self.ctx.singularity_size > 0
                        ? "Not enough memory to open this file. Budget in use: \(QTSQMemoryBudget.reservedBytes / 1_000_000) MB of \(QTSQMemoryBudget.limitDescription). Close another file and try again."
                        : "Nothing to decrypt."
                }
                return
            }

            /* Device-encrypted files unlock with this Mac's key; the entered
             * password is ignored. */
            let usedDeviceKey = self.isDeviceEncrypted && DeviceKeyStore.shared.isAvailable
            let decRes: Int32
            if usedDeviceKey {
                decRes = DeviceKeyStore.shared.decryptWithDeviceKey(&vctx)
            } else {
                decRes = password.withCString { cstr in
                    qtsq_vault_decrypt_password(&vctx, cstr)
                }
            }
            guard decRes == QTSQ_OK else {
                if let s = vctx.singularity { s.deallocate() }
                vctx.singularity = nil
                DispatchQueue.main.async {
                    self.errorMessage = "Incorrect passphrase or corrupted authentication tag."
                }
                return
            }

            /* Generic streams: decompress now into file.data */
            var decoded: Data? = nil
            var decError: String? = nil
            if !self.isSpectralDictionary && !self.isContainer && !self.isVideo {
                var rawData: UnsafeMutablePointer<UInt8>? = nil
                var rawSize: Int = 0
                let res = qtsq_decompress(&vctx, &rawData, &rawSize)
                if res == QTSQ_OK, let dataPtr = rawData, rawSize > 0 {
                    decoded = Data(bytesNoCopy: dataPtr, count: rawSize, deallocator: .free)
                } else {
                    if let dataPtr = rawData { free(dataPtr) }
                    decError = String(cString: qtsq_error_string(res))
                }
            }

            /* Publish the plaintext copy for lazy decoders (Photon HQ,
             * spectral dictionary, video) plus any decompressed data. */
            DispatchQueue.main.async {
                self.freeViewCtx()
                self.viewCtx = vctx
                if decoded != nil { self.data = decoded }
                self.isUnlocked = true
                self.isLoaded = true
                self.errorMessage = decError
            }
        }
    }

    func reload() {
        self.isLoaded = false
        self.data = nil
        self.isUnlocked = false
        self.isSpectralDictionary = false
        self.isContainer = false
        // Without this a re-read file that is no longer video keeps showing
        // the video player, because ViewerView keys off isVideo.
        self.isVideo = false
        self.errorMessage = nil
        freeViewCtx()
        cleanupTempFile()  // remove old temp file before re-reading
        qtsq_free(&self.ctx)
        qtsq_init(&self.ctx)

        // Re-read actual .qtsq file size from disk
        if let attrs = try? FileManager.default.attributesOfItem(atPath: path),
           let sz = attrs[.size] as? UInt64 {
            self.qtsqFileSize = sz
        }

        readAndDecompress()
    }
}

class AppState: ObservableObject {
    @Published var files: [QTSQFile] = []
    @Published var currentPanel: PanelType = .welcome

    init() {}

    func openFile(path: String) {
        // If file already open at this path, remove and re-read from disk
        // (the file on disk may have been overwritten by a new compression)
        if let existingIndex = files.firstIndex(where: { $0.path == path }) {
            files.remove(at: existingIndex)
        }
        
        let newFile = QTSQFile(path: path)
        files.append(newFile)
        self.currentPanel = .viewer(newFile.id)
    }
}

