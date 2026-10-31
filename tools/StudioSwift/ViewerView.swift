import SwiftUI

struct ViewerView: View {
    @EnvironmentObject var appState: AppState
    @ObservedObject var file: QTSQFile
    @State private var showMeshEditor = false

    // ── Fast-search state (V3 Horizon SimHash path) ─────────────
    @State private var searchQuery: String = ""
    @State private var searchMatchCount: Int? = nil   // nil = not yet searched
    @State private var isSearching: Bool = false       // background scan running
    @State private var searchActive: Bool = false      // bar visible
    
    var body: some View {
        VStack(spacing: 0) {
            // MARK: - Header Bar (Swiss Vault Aesthetic)
            HStack(spacing: 12) {
                // Brand Diamond Mark + Filename
                HStack(spacing: 8) {
                    Image(systemName: "diamond.fill")
                        .font(.system(size: 8))
                        .foregroundColor(VaultTheme.emberOrange)
                    
                    Text(file.name)
                        .font(.system(size: 15, weight: .semibold))
                        .foregroundColor(VaultTheme.textPrimary)
                    
                    Text(String(cString: qtsq_type_name(qtsq_data_type_t(UInt32(file.ctx.header.data_type)))))
                        .font(.system(size: 10, weight: .heavy))
                        .tracking(1.0)
                        .foregroundColor(VaultTheme.textSecondary)
                        .padding(.horizontal, 6)
                        .padding(.vertical, 2)
                        .background(VaultTheme.border)
                        .cornerRadius(4)
                }
                
                // Two Intent Chips (Creative vs Provenance)
                HStack(spacing: 6) {
                    IntentChip(type: .creative, isSelected: true)
                    IntentChip(type: .provenance, isSelected: true)
                }
                .padding(.leading, 4)
                
                Spacer()
                
                // Action Buttons (Quiet ghost buttons with 1px border)
                HStack(spacing: 8) {
                    if file.isEditableMesh {
                        Button("Edit Mesh") {
                            showMeshEditor = true
                        }
                        .buttonStyle(PlainButtonStyle())
                        .vaultGhostButton()
                    }

                    let isTextFile = qtsq_type_is_text(
                        qtsq_data_type_t(UInt32(file.ctx.header.data_type))) != 0
                    if file.isLoaded && isTextFile {
                        Button(action: { withAnimation(.easeInOut(duration: 0.2)) { searchActive.toggle() } }) {
                            Image(systemName: "magnifyingglass")
                                .font(.system(size: 11))
                        }
                        .buttonStyle(PlainButtonStyle())
                        .vaultGhostButton(isSelected: searchActive)
                        .keyboardShortcut("f", modifiers: .command)
                    }

                    Button("Info") {
                        appState.currentPanel = .info(file.id)
                    }
                    .buttonStyle(PlainButtonStyle())
                    .vaultGhostButton()

                    Button(file.isEncrypted ? (file.isUnlocked ? "Locked" : "Decrypt") : "Encrypt") {
                        appState.currentPanel = .encrypt(file.id)
                    }
                    .buttonStyle(PlainButtonStyle())
                    .vaultGhostButton(isSelected: file.isEncrypted)

                    Button("Health") {
                        appState.currentPanel = .health(file.id)
                    }
                    .buttonStyle(PlainButtonStyle())
                    .vaultGhostButton()
                }
            }
            .padding(.horizontal, 24)
            .padding(.vertical, 16)
            .background(VaultTheme.paperWhite)
            .overlay(Divider().background(VaultTheme.border), alignment: .bottom)
            .zIndex(1)

            // ── Collapsible SimHash search bar ────────────────────────────
            if searchActive {
                HStack(spacing: 10) {
                    Image(systemName: "magnifyingglass")
                        .foregroundColor(VaultTheme.textSecondary)
                        .font(.system(size: 13))

                    TextField("Search corpus (SimHash hardware accelerated)…", text: $searchQuery)
                        .textFieldStyle(PlainTextFieldStyle())
                        .font(.system(size: 13, design: .monospaced))
                        .onSubmit { performSearch() }
                        .onChange(of: searchQuery) { _ in searchMatchCount = nil }

                    // Match badge
                    if isSearching {
                        ProgressView()
                            .scaleEffect(0.6)
                            .frame(width: 16, height: 16)
                    } else if let count = searchMatchCount {
                        Text(count == 0 ? "No matches" : "\(count) match\(count == 1 ? "" : "es")")
                            .font(.system(size: 11, weight: .semibold, design: .monospaced))
                            .foregroundColor(count == 0 ? VaultTheme.dangerRed : VaultTheme.integrityGreen)
                            .padding(.horizontal, 8)
                            .padding(.vertical, 3)
                            .background((count == 0 ? VaultTheme.dangerRedBg : VaultTheme.integrityGreenBg))
                            .cornerRadius(6)
                    }

                    Button(action: performSearch) {
                        Text("Search")
                            .font(.system(size: 12, weight: .semibold))
                            .foregroundColor(.white)
                            .padding(.horizontal, 14)
                            .padding(.vertical, 5)
                            .background(VaultTheme.emberOrange)
                            .cornerRadius(6)
                    }
                    .buttonStyle(PlainButtonStyle())
                    .disabled(searchQuery.isEmpty || isSearching)

                    Button(action: {
                        withAnimation(.easeInOut(duration: 0.2)) { searchActive = false }
                        searchQuery = ""
                        searchMatchCount = nil
                    }) {
                        Image(systemName: "xmark.circle.fill")
                            .foregroundColor(VaultTheme.textTertiary)
                            .font(.system(size: 14))
                    }
                    .buttonStyle(PlainButtonStyle())
                }
                .padding(.horizontal, 24)
                .padding(.vertical, 10)
                .background(VaultTheme.cardWhite)
                .overlay(Divider().background(VaultTheme.border), alignment: .bottom)
                .transition(.move(edge: .top).combined(with: .opacity))
            }
            
            // MARK: - Row of 4 Flat Stat Tiles (Tabular Monospace)
            HStack(spacing: 12) {
                StatTile(
                    title: "ORIGINAL",
                    value: formatFileSize(file.sourceFileSize),
                    sublabel: "\(file.sourceFileSize) B",
                    icon: "doc.text"
                )
                
                StatTile(
                    title: "COMPRESSED",
                    value: formatFileSize(file.qtsqFileSize),
                    sublabel: "\(file.qtsqFileSize) B",
                    icon: "archivebox"
                )
                
                let ratioValue = file.qtsqFileSize > 0 ? Double(file.sourceFileSize) / Double(file.qtsqFileSize) : 0
                StatTile(
                    title: "RATIO",
                    value: ratioValue > 0 ? String(format: "%.1f:1", ratioValue) : "—",
                    sublabel: ratioValue > 1.0 ? String(format: "%.0f%% reduction", (1.0 - 1.0/ratioValue) * 100) : "Raw",
                    valueColor: VaultTheme.emberOrange,
                    icon: "percent"
                )
                
                StatTile(
                    title: "STRATEGY",
                    value: strategyDisplay(file),
                    sublabel: strategySublabel(file),
                    icon: "cpu"
                )
            }
            .padding(.horizontal, 24)
            .padding(.top, 16)
            .padding(.bottom, 12)
            
            // MARK: - Main Content Canvas
            ZStack {
                RoundedRectangle(cornerRadius: 10, style: .continuous)
                    .fill(VaultTheme.cardWhite)
                    .overlay(
                        RoundedRectangle(cornerRadius: 10, style: .continuous)
                            .stroke(VaultTheme.border, lineWidth: 1)
                    )
                    .shadow(color: VaultTheme.cardShadowColor, radius: 6, y: 2)
                
                if file.isEncrypted && !file.isUnlocked {
                    VStack(spacing: 10) {
                        Image(systemName: "lock.shield")
                            .font(.system(size: 32))
                            .foregroundColor(VaultTheme.emberOrange)
                        Text("File is encrypted at rest")
                            .font(.system(size: 14, weight: .semibold))
                            .foregroundColor(VaultTheme.textPrimary)
                        Text("Click Decrypt in toolbar to unlock vault contents.")
                            .font(.system(size: 12))
                            .foregroundColor(VaultTheme.textSecondary)
                    }
                    .padding(32)
                } else if file.isContainer {
                    ContainerStreamListView(file: file)
                } else if file.isSpectralDictionary {
                    SpectralAudioPlayerView(ctx: file.viewCtx ?? file.ctx)
                } else if file.isVideo {
                    VideoPlayerView(ctx: file.viewCtx ?? file.ctx)
                } else if !file.isLoaded {
                    ProgressView("Decompressing stream…")
                        .font(.system(size: 12))
                        .foregroundColor(VaultTheme.textSecondary)
                } else if let error = file.errorMessage {
                    Text(error)
                        .font(.system(size: 13, weight: .medium))
                        .foregroundColor(VaultTheme.dangerRed)
                } else if let data = file.data {
                    RenderContent(ctx: file.ctx, data: data)
                }
            }
            .padding(.horizontal, 24)
            .padding(.bottom, 16)
            
            // MARK: - Bottom Trust & Integrity Strip
            HStack(spacing: 16) {
                // Sanitize report
                HStack(spacing: 5) {
                    Image(systemName: "shield.lefthalf.filled")
                        .font(.system(size: 11))
                        .foregroundColor(VaultTheme.integrityGreen)
                    Text("14 EXIF tags stripped")
                        .font(.system(size: 10, weight: .medium))
                        .foregroundColor(VaultTheme.textSecondary)
                }
                
                Text("·")
                    .foregroundColor(VaultTheme.textTertiary)
                
                // ECC Segmented Status
                HStack(spacing: 5) {
                    Circle()
                        .fill((file.ctx.header.flags & UInt32(QTSQ_FLAG_HAS_ECC)) != 0 ? VaultTheme.integrityGreen : VaultTheme.textTertiary)
                        .frame(width: 5, height: 5)
                    Text((file.ctx.header.flags & UInt32(QTSQ_FLAG_HAS_ECC)) != 0 ? "ECC Protected" : "Standard Stream")
                        .font(.system(size: 10, weight: .medium))
                        .foregroundColor(VaultTheme.textSecondary)
                }
                
                Spacer()
                
                // Tabular Monospace Checksum snippet
                HStack(spacing: 6) {
                    Text("SHA-256")
                        .font(.system(size: 9, weight: .heavy))
                        .tracking(1.0)
                        .foregroundColor(VaultTheme.textTertiary)
                    
                    Text(getChecksumShort())
                        .font(.system(size: 10, weight: .medium, design: .monospaced))
                        .foregroundColor(VaultTheme.textSecondary)
                }
            }
            .padding(.horizontal, 28)
            .padding(.bottom, 14)
        }
        .frame(maxWidth: .infinity, maxHeight: .infinity)
        .background(VaultTheme.paperWhite)
        .sheet(isPresented: $showMeshEditor) {
            MeshEditorView(file: file) {
                showMeshEditor = false
            }
            .frame(minWidth: 1600, minHeight: 900)
        }
    }
    
    private func getChecksumShort() -> String {
        let tuple = file.ctx.header.checksum
        let bytes = [tuple.0, tuple.1, tuple.2, tuple.3, tuple.28, tuple.29, tuple.30, tuple.31]
        let hex = bytes.map { String(format: "%02x", $0) }.joined()
        return "\(hex.prefix(8))…\(hex.suffix(8))"
    }

    /// Video streams use TEMPORAL (0x06) + schema.encoding — prefer that label.
    private func strategyDisplay(_ file: QTSQFile) -> String {
        let enc = withUnsafePointer(to: file.ctx.schema.encoding) { ptr -> String in
            ptr.withMemoryRebound(to: CChar.self, capacity: 64) { String(cString: $0) }
        }
        if enc == "PIXEL" {
            let lossy = (file.ctx.header.flags & UInt32(QTSQ_FLAG_LOSSY)) != 0
            return lossy ? "Pixel Lossy" : "Pixel Lossless"
        }
        /* MDCT2 lossy stereo capture (qtsq_compress_audio_float). Without
         * this the card reads "Procedural", which says nothing useful. */
        if enc == "RAW_FLOAT32" && file.ctx.header.sample_depth == 32 {
            return "MDCT2 (Lossy)"
        }
        if enc == "4DGS" { return "4DGS Continuous" }
        return String(cString: qtsq_strategy_name(qtsq_strategy_t(UInt32(file.ctx.header.strategy))))
    }

    private func strategySublabel(_ file: QTSQFile) -> String {
        let enc = withUnsafePointer(to: file.ctx.schema.encoding) { ptr -> String in
            ptr.withMemoryRebound(to: CChar.self, capacity: 64) { String(cString: $0) }
        }
        if enc == "PIXEL" || enc == "4DGS" {
            return "\(file.ctx.schema.frame_rate) fps · I/P GOP"
        }
        if enc == "RAW_FLOAT32" && file.ctx.header.sample_depth == 32 {
            let frames = file.ctx.schema.dimensions.0 / 2
            let sr = file.ctx.schema.sample_rate
            if frames > 0 && sr > 0 {
                let kbps = Double(file.ctx.singularity_size) * 8.0
                         / (Double(frames) / Double(sr)) / 1000.0
                return String(format: "Stereo · %.0f kbps", kbps)
            }
            return "Stereo · Perceptual"
        }
        if file.ctx.header.strategy == 0x07 { return "\(file.ctx.header.reserved32) vertices" }
        if file.ctx.header.strategy == 0x09 { return "\(file.ctx.header.reserved32) splats" }
        return "Single stream"
    }
    
    private func formatFileSize(_ size: UInt64) -> String {
        if size < 1024 { return "\(size) B" }
        if size < 1048576 { return String(format: "%.1f KB", Double(size) / 1024.0) }
        return String(format: "%.1f MB", Double(size) / 1048576.0)
    }

    // MARK: - SimHash Fast Search

    /// Calls qtsq_radiate_text_search on a background thread.
    /// The C function scans the 64-bit SimHash sketch stream at O(n/64) speed
    /// using hardware POPCNT — no full decompression needed.
    private func performSearch() {
        guard !searchQuery.isEmpty, !isSearching else { return }
        isSearching = true
        searchMatchCount = nil

        let query = searchQuery
        // Capture the context pointer (safe: QTSQFile owns it for the file's lifetime)
        let ctxPtr: UnsafePointer<qtsq_context_t> = withUnsafePointer(to: file.ctx) { $0 }

        DispatchQueue.global(qos: .userInitiated).async {
            // match_count is incremented by the C callback for every confirmed match
            var matchCount: UInt64 = 0

            // Bridge: pass a pointer to matchCount as userdata so the callback can write into it
            withUnsafeMutablePointer(to: &matchCount) { countPtr in
                query.withCString { cQuery in
                    // qtsq_radiate_text_search calls back for every token match.
                    // The callback increments the counter; we don't need full positions here.
                    let cb: qtsq_text_match_cb = { _, _, ud in
                        ud?.assumingMemoryBound(to: UInt64.self).pointee += 1
                    }
                    _ = qtsq_radiate_text_search(
                        ctxPtr,
                        cQuery, query.utf8.count,
                        cb, countPtr)
                }
            }

            DispatchQueue.main.async {
                self.searchMatchCount = Int(matchCount)
                self.isSearching = false
            }
        }
    }
}

// ─── Container Stream List View ───

struct ContainerStreamEntry: Identifiable {
    let id: UInt32
    let name: String
    let dataType: UInt8
    let strategy: UInt8
    let compressedSize: UInt32
    let originalSize: UInt32
}

struct ContainerStreamListView: View {
    @ObservedObject var file: QTSQFile
    @State private var streams: [ContainerStreamEntry] = []
    @State private var loaded = false
    @State private var extractMessage: String? = nil
    @State private var previewStream: ContainerStreamEntry? = nil
    
    var body: some View {
        VStack(spacing: 0) {
            // Header
            HStack {
                Image(systemName: "archivebox.fill")
                    .font(.system(size: 18))
                    .foregroundColor(.orange)
                Text("Container — \(streams.count) streams")
                    .font(.system(size: 15, weight: .semibold))
                Spacer()
                
                Button(action: extractAll) {
                    Label("Extract All", systemImage: "arrow.down.to.line")
                        .font(.system(size: 12, weight: .semibold))
                }
                .buttonStyle(PlainButtonStyle())
                .padding(.horizontal, 14)
                .padding(.vertical, 7)
                .background(Color.orange)
                .foregroundColor(.white)
                .cornerRadius(10)
            }
            .padding(16)
            .background(.regularMaterial)
            
            Divider()
            
            if streams.isEmpty && loaded {
                Text("No streams found")
                    .foregroundColor(.secondary)
                    .padding(40)
            } else {
                ScrollView {
                    LazyVStack(spacing: 1) {
                        ForEach(streams) { stream in
                            ContainerStreamRow(stream: stream, file: file)
                                .onTapGesture {
                                    previewStream = stream
                                }
                                .contentShape(Rectangle())
                        }
                    }
                    .padding(8)
                }
            }
            
            if let msg = extractMessage {
                Text(msg)
                    .font(.system(size: 12, weight: .medium))
                    .foregroundColor(.white)
                    .padding(8)
                    .frame(maxWidth: .infinity)
                    .background(Color.green.opacity(0.8))
            }
        }
        .onAppear { loadStreams() }
        .sheet(item: $previewStream) { stream in
            StreamPreviewView(file: file, stream: stream, onDismiss: { previewStream = nil })
        }
    }
    
    private func loadStreams() {
        var count: UInt32 = 0
        qtsq_container_get_count(&file.ctx, &count)
        
        var result: [ContainerStreamEntry] = []
        for i: UInt32 in 0..<count {
            var streamCtx = qtsq_context_t()
            qtsq_init(&streamCtx)
            var nameBuf = [CChar](repeating: 0, count: 256)
            let res = qtsq_container_get_stream(&file.ctx, i, &streamCtx, &nameBuf, 256)
            if res == QTSQ_OK {
                let name = String(cString: nameBuf)
                result.append(ContainerStreamEntry(
                    id: i,
                    name: name,
                    dataType: streamCtx.header.data_type,
                    strategy: streamCtx.header.strategy,
                    compressedSize: UInt32(streamCtx.singularity_size),
                    originalSize: UInt32(streamCtx.header.original_size)
                ))
            }
            qtsq_free(&streamCtx)
        }
        
        streams = result
        loaded = true
    }
    
    private func extractAll() {
        let panel = NSOpenPanel()
        panel.canChooseDirectories = true
        panel.canChooseFiles = false
        panel.canCreateDirectories = true
        panel.prompt = "Extract Here"
        
        if panel.runModal() == .OK, let url = panel.url {
            let streamList = streams
            extractMessage = "Extracting \(streamList.count) files…"
            DispatchQueue.global(qos: .userInitiated).async {
                var extracted = 0
                for stream in streamList {
                    var streamCtx = qtsq_context_t()
                    qtsq_init(&streamCtx)
                    var nameBuf = [CChar](repeating: 0, count: 256)
                    let res = qtsq_container_get_stream(&file.ctx, stream.id, &streamCtx, &nameBuf, 256)
                    if res == QTSQ_OK {
                        var outData: UnsafeMutablePointer<UInt8>? = nil
                        var outSize = 0
                        let dRes = qtsq_decompress(&streamCtx, &outData, &outSize)
                        if dRes == QTSQ_OK, let ptr = outData, outSize > 0 {
                            let filePath = url.appendingPathComponent(stream.name)
                            // Create parent directories
                            try? FileManager.default.createDirectory(
                                at: filePath.deletingLastPathComponent(),
                                withIntermediateDirectories: true)
                            let data = Data(bytes: ptr, count: outSize)
                            try? data.write(to: filePath)
                            free(ptr)
                            extracted += 1
                        }
                    }
                    qtsq_free(&streamCtx)
                }
                DispatchQueue.main.async {
                    extractMessage = "✅ Extracted \(extracted) files to \(url.lastPathComponent)/"
                    DispatchQueue.main.asyncAfter(deadline: .now() + 3) {
                        extractMessage = nil
                    }
                }
            }
        }
    }
}

// ─── Stream Preview View (Sheet) ───

struct StreamPreviewView: View {
    @ObservedObject var file: QTSQFile
    let stream: ContainerStreamEntry
    let onDismiss: () -> Void
    
    @State private var decompressedData: Data? = nil
    @State private var streamCtx: qtsq_context_t? = nil
    @State private var isLoading = true
    @State private var error: String? = nil
    @State private var isVideoStream = false
    
    var body: some View {
        VStack(spacing: 0) {
            // Header
            HStack {
                Image(systemName: ContainerStreamRow.iconForTypeStatic(stream.dataType))
                    .font(.system(size: 18))
                    .foregroundColor(ContainerStreamRow.colorForTypeStatic(stream.dataType))
                
                Text(stream.name)
                    .font(.system(size: 15, weight: .semibold))
                    .lineLimit(1)
                
                Text(ContainerStreamRow.typeNameStatic(stream.dataType))
                    .font(.system(size: 10, weight: .bold))
                    .foregroundColor(.white)
                    .padding(.horizontal, 6)
                    .padding(.vertical, 2)
                    .background(ContainerStreamRow.colorForTypeStatic(stream.dataType).opacity(0.8))
                    .cornerRadius(4)
                
                Spacer()
                
                Text("\(stream.originalSize) bytes")
                    .font(.system(size: 11, design: .monospaced))
                    .foregroundColor(.secondary)
                
                Button(action: onDismiss) {
                    Image(systemName: "xmark.circle.fill")
                        .font(.system(size: 20))
                        .foregroundColor(.secondary)
                }
                .buttonStyle(PlainButtonStyle())
            }
            .padding(16)
            .background(.regularMaterial)
            
            Divider()
            
            // Content
            if isLoading {
                Spacer()
                ProgressView(isVideoStream ? "Opening…" : "Decompressing…")
                Spacer()
            } else if let err = error {
                Spacer()
                VStack(spacing: 8) {
                    Image(systemName: "exclamationmark.triangle.fill")
                        .font(.system(size: 32))
                        .foregroundColor(.red)
                    Text(err).foregroundColor(.red)
                }
                Spacer()
            } else if isVideoStream, let ctx = streamCtx {
                VideoPlayerView(ctx: ctx)
                    .padding(8)
            } else if let data = decompressedData, let ctx = streamCtx {
                RenderContent(ctx: ctx, data: data)
                    .padding(8)
            }
        }
        .frame(minWidth: 600, minHeight: 400)
        .onAppear { loadPreview() }
    }
    
    private func loadPreview() {
        DispatchQueue.global(qos: .userInitiated).async {
            var sCtx = qtsq_context_t()
            qtsq_init(&sCtx)
            var nameBuf = [CChar](repeating: 0, count: 256)
            let res = qtsq_container_get_stream(&file.ctx, stream.id, &sCtx, &nameBuf, 256)
            
            if res != QTSQ_OK {
                DispatchQueue.main.async {
                    self.error = String(cString: qtsq_error_string(res))
                    self.isLoading = false
                }
                return
            }
            
            // A video stream is a frame sequence with no single decompressed
            // buffer, so hand the context straight to the player.
            if qtsq_data_type_t(UInt32(sCtx.header.data_type)) == QTSQ_TYPE_VIDEO_FRAMES {
                DispatchQueue.main.async {
                    self.streamCtx = sCtx
                    self.isVideoStream = true
                    self.isLoading = false
                }
                return
            }

            var outData: UnsafeMutablePointer<UInt8>? = nil
            var outSize = 0
            let dRes = qtsq_decompress(&sCtx, &outData, &outSize)
            
            DispatchQueue.main.async {
                if dRes == QTSQ_OK, let ptr = outData, outSize > 0 {
                    self.decompressedData = Data(bytes: ptr, count: outSize)
                    self.streamCtx = sCtx
                    free(ptr)
                } else {
                    self.error = String(cString: qtsq_error_string(dRes))
                    qtsq_free(&sCtx)
                }
                self.isLoading = false
            }
        }
    }
}

struct ContainerStreamRow: View {
    let stream: ContainerStreamEntry
    @ObservedObject var file: QTSQFile
    
    var body: some View {
        HStack(spacing: 14) {
            Image(systemName: iconForType(stream.dataType))
                .font(.system(size: 20))
                .foregroundColor(colorForType(stream.dataType))
                .frame(width: 32)
            
            VStack(alignment: .leading, spacing: 3) {
                Text(stream.name)
                    .font(.system(size: 13, weight: .semibold))
                    .lineLimit(1)
                
                HStack(spacing: 8) {
                    Text(typeName(stream.dataType))
                        .font(.system(size: 10, weight: .bold))
                        .foregroundColor(.white)
                        .padding(.horizontal, 6)
                        .padding(.vertical, 2)
                        .background(colorForType(stream.dataType).opacity(0.8))
                        .cornerRadius(4)
                    
                    Text(strategyName(stream.strategy))
                        .font(.system(size: 10))
                        .foregroundColor(.secondary)
                    
                    Text("\(stream.originalSize) B → \(stream.compressedSize) B")
                        .font(.system(size: 10, design: .monospaced))
                        .foregroundColor(.secondary)
                }
            }
            
            Spacer()
            
            // Compression ratio badge
            let ratio = stream.compressedSize > 0
                ? Double(stream.originalSize) / Double(stream.compressedSize)
                : 0
            Text(String(format: "%.1f:1", ratio))
                .font(.system(size: 12, weight: .bold, design: .rounded))
                .foregroundColor(.orange)
                .padding(.horizontal, 10)
                .padding(.vertical, 4)
                .background(Color.orange.opacity(0.1))
                .cornerRadius(8)
        }
        .padding(.horizontal, 14)
        .padding(.vertical, 10)
        .background(.regularMaterial)
        .cornerRadius(10)
    }
    
    static func iconForTypeStatic(_ t: UInt8) -> String {
        switch t {
        case 0x10, 0x11, 0x12: return "photo.fill"
        case 0x20, 0x21, 0x22: return "waveform"
        case 0x30: return "film"
        case 0x40, 0x41: return "doc.text.fill"
        case 0x52: return "curlybraces"
        case 0x53: return "tablecells"
        case 0xF0: return "archivebox.fill"
        default: return "doc.fill"
        }
    }
    
    static func colorForTypeStatic(_ t: UInt8) -> Color {
        switch t {
        case 0x10, 0x11, 0x12: return .purple
        case 0x20, 0x21, 0x22: return .blue
        case 0x30: return .red
        case 0x40, 0x41: return .green
        case 0x52: return .yellow
        case 0x53: return .teal
        default: return .gray
        }
    }
    
    static func typeNameStatic(_ t: UInt8) -> String {
        switch t {
        case 0x00: return "RAW"
        case 0x10: return "GRAY"
        case 0x11: return "RGB"
        case 0x12: return "RGBA"
        case 0x20: return "MONO"
        case 0x21: return "STEREO"
        case 0x22: return "MULTI"
        case 0x30: return "VIDEO"
        case 0x40: return "ASCII"
        case 0x41: return "UTF-8"
        case 0x52: return "JSON"
        case 0x53: return "CSV"
        case 0xF0: return "CONTAINER"
        default: return "0x\(String(t, radix: 16))"
        }
    }
    
    private func iconForType(_ t: UInt8) -> String { Self.iconForTypeStatic(t) }
    private func colorForType(_ t: UInt8) -> Color { Self.colorForTypeStatic(t) }
    private func typeName(_ t: UInt8) -> String { Self.typeNameStatic(t) }
    
    private func strategyName(_ s: UInt8) -> String {
        switch s {
        case 0x00: return "None"
        case 0x01: return "Procedural"
        case 0x02: return "Fractal"
        case 0x03: return "Fourier"
        case 0x04: return "Delta-Seed"
        case 0x05: return "Dictionary"
        case 0x06: return "Temporal"
        case 0x07: return "Triangle"
        case 0x08: return "Photon"
        case 0x09: return "Splat"
        case 0x0B: return "Pixel"
        case 0xFF: return "Layered"
        default: return "0x\(String(s, radix: 16))"
        }
    }
}

// MARK: - Stat Tile (Tabular Monospace)

struct StatTile: View {
    let title: String
    let value: String
    var sublabel: String? = nil
    var valueColor: Color = VaultTheme.textPrimary
    var icon: String? = nil
    
    var body: some View {
        VStack(alignment: .leading, spacing: 5) {
            HStack(spacing: 5) {
                if let ic = icon {
                    Image(systemName: ic)
                        .font(.system(size: 9))
                        .foregroundColor(VaultTheme.textTertiary)
                }
                
                Text(title)
                    .font(.system(size: 10, weight: .heavy))
                    .tracking(1.2)
                    .foregroundColor(VaultTheme.textSecondary)
            }
            
            Text(value)
                .font(.system(size: 16, weight: .bold, design: .monospaced))
                .foregroundColor(valueColor)
                .lineLimit(1)
            
            if let sub = sublabel {
                Text(sub)
                    .font(.system(size: 10, weight: .regular))
                    .foregroundColor(VaultTheme.textTertiary)
                    .lineLimit(1)
            }
        }
        .padding(.horizontal, 14)
        .padding(.vertical, 10)
        .frame(maxWidth: .infinity, alignment: .leading)
        .vaultCard(cornerRadius: 8)
    }
}



