import SwiftUI
import UniformTypeIdentifiers

struct DiffView: View {
    @EnvironmentObject var appState: AppState
    
    var body: some View {
        VStack(alignment: .leading, spacing: 24) {
            // Header
            HStack(spacing: 8) {
                Image(systemName: "arrow.triangle.merge")
                    .font(.system(size: 15, weight: .semibold))
                    .foregroundColor(VaultTheme.emberOrange)
                
                Text("Diff & Patch (Gravitational Waves)")
                    .font(.system(size: 18, weight: .semibold))
                    .foregroundColor(VaultTheme.textPrimary)
            }
            .padding(.bottom, 4)
            
            // Generate Diff Section
            VStack(alignment: .leading, spacing: 12) {
                Text("GENERATE DIFFERENTIAL WAVE")
                    .font(.system(size: 10, weight: .heavy))
                    .tracking(1.2)
                    .foregroundColor(VaultTheme.textSecondary)
                
                Text("Select two .qtsq files to compute a cryptographic differential patch (.qtsw).")
                    .font(.system(size: 13))
                    .foregroundColor(VaultTheme.textSecondary)
                
                Button(action: doDiffSelect) {
                    HStack(spacing: 6) {
                        Image(systemName: "plus.forwardslash.minus")
                            .font(.system(size: 11))
                        Text("Select Baseline + Target Files…")
                            .font(.system(size: 12, weight: .semibold))
                    }
                    .foregroundColor(.white)
                    .padding(.horizontal, 20)
                    .padding(.vertical, 8)
                    .background(VaultTheme.emberOrange)
                    .cornerRadius(6)
                }
                .buttonStyle(PlainButtonStyle())
            }
            .padding(20)
            .frame(maxWidth: .infinity, alignment: .leading)
            .vaultCard(cornerRadius: 10)
            
            // Apply Patch Section
            VStack(alignment: .leading, spacing: 12) {
                Text("APPLY DIFFERENTIAL PATCH")
                    .font(.system(size: 10, weight: .heavy))
                    .tracking(1.2)
                    .foregroundColor(VaultTheme.textSecondary)
                
                Text("Select the original baseline file and apply the differential .qtsw wave.")
                    .font(.system(size: 13))
                    .foregroundColor(VaultTheme.textSecondary)
                
                Button(action: doPatchSelect) {
                    HStack(spacing: 6) {
                        Image(systemName: "arrow.triangle.pull")
                            .font(.system(size: 11))
                        Text("Select File + Patch…")
                            .font(.system(size: 12, weight: .medium))
                    }
                    .foregroundColor(VaultTheme.textPrimary)
                    .padding(.horizontal, 20)
                    .padding(.vertical, 8)
                    .background(VaultTheme.cardWhite)
                    .cornerRadius(6)
                    .overlay(RoundedRectangle(cornerRadius: 6).stroke(VaultTheme.border, lineWidth: 1))
                }
                .buttonStyle(PlainButtonStyle())
            }
            .padding(20)
            .frame(maxWidth: .infinity, alignment: .leading)
            .vaultCard(cornerRadius: 10)
            
            Spacer()
        }
        .padding(32)
        .frame(maxWidth: .infinity, maxHeight: .infinity, alignment: .topLeading)
        .background(VaultTheme.paperWhite)
    }
    
    private func doDiffSelect() {
        let p = NSOpenPanel()
        p.allowedContentTypes = [UTType(filenameExtension: "qtsq")!]
        p.allowsMultipleSelection = true
        p.message = "Select OLD file first, then NEW file (hold Cmd)"
        if p.runModal() == .OK, p.urls.count >= 2 {
            let oldPath = p.urls[0].path
            let newPath = p.urls[1].path
            
            var oldCtx = qtsq_context_t()
            var newCtx = qtsq_context_t()
            qtsq_init(&oldCtx)
            qtsq_init(&newCtx)
            defer {
                qtsq_free(&oldCtx)
                qtsq_free(&newCtx)
            }
            
            qtsq_read(&oldCtx, oldPath)
            qtsq_read(&newCtx, newPath)
            
            let save = NSSavePanel()
            save.allowedContentTypes = [UTType(filenameExtension: "qtsw")!]
            save.nameFieldStringValue = "update.qtsw"
            if save.runModal() == .OK, let saveUrl = save.url {
                 let res = saveUrl.path.withCString { cPath in
                    qtsq_emit_wave(&oldCtx, &newCtx, cPath)
                }
                showResult(success: res == QTSQ_OK, title: res == QTSQ_OK ? "Wave Generated ✅" : "Failed ❌", msg: res == QTSQ_OK ? "Patch file created." : "Could not generate diff.")
            }
        }
    }
    
    private func doPatchSelect() {
        let p = NSOpenPanel()
        p.allowsMultipleSelection = true
        p.message = "Select the .qtsq file and the .qtsw patch (hold Cmd)"
        if p.runModal() == .OK, p.urls.count >= 2 {
            var qtsqPath: String?
            var qtswPath: String?
            for url in p.urls {
                if url.pathExtension == "qtsq" { qtsqPath = url.path }
                if url.pathExtension == "qtsw" { qtswPath = url.path }
            }
            
            guard let qtsq = qtsqPath, let qtsw = qtswPath else {
                showResult(success: false, title: "Need one .qtsq and one .qtsw", msg: "")
                return
            }
            
            var ctx = qtsq_context_t()
            qtsq_init(&ctx)
            defer { qtsq_free(&ctx) }
            
            qtsq_read(&ctx, qtsq)
            let res = qtsw.withCString { cPath in
                qtsq_apply_wave(&ctx, cPath)
            }
            
            if res == QTSQ_OK {
                _ = qtsq.withCString { cPath in
                    if qtsqPromptAndEncrypt(&ctx) { qtsq_write(&ctx, cPath) }
                }
                appState.openFile(path: qtsq)
            }
            showResult(success: res == QTSQ_OK, title: res == QTSQ_OK ? "Patch Applied ✅" : "Failed ❌", msg: "")
        }
    }
    
    private func showResult(success: Bool, title: String, msg: String) {
        DispatchQueue.main.async {
            let a = NSAlert()
            a.messageText = title
            if !msg.isEmpty { a.informativeText = msg }
            a.alertStyle = success ? .informational : .critical
            a.runModal()
        }
    }
}
