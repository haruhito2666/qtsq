import SwiftUI

// MARK: - Data Structures

struct EditableVertex: Identifiable {
    let id: Int
    var x: CGFloat
    var y: CGFloat
    var r: UInt8
    var g: UInt8
    var b: UInt8
    var color: Color { Color(red: Double(r)/255, green: Double(g)/255, blue: Double(b)/255) }
}

struct EditableSplat: Identifiable {
    let id: Int
    var x: CGFloat; var y: CGFloat
    var sx: UInt8; var sy: UInt8; var theta: UInt8
    var r: UInt8; var g: UInt8; var b: UInt8
    var alpha: UInt8; var z: UInt8
}

struct TriFace {
    var a: Int; var b: Int; var c: Int
}

// MARK: - Edit State

enum MeshEditMode: String, CaseIterable { case vertex = "Vertices"; case splat = "Splats" }
enum MeshEditTool: String, CaseIterable { case select = "Select"; case pan = "Pan" }

enum MeshUndoAction {
    case moveVertex(id: Int, oldX: CGFloat, oldY: CGFloat)
    case colorVertex(id: Int, oldR: UInt8, oldG: UInt8, oldB: UInt8)
    case moveSplat(id: Int, oldX: CGFloat, oldY: CGFloat)
}

// MARK: - Mesh Editor View

struct MeshEditorView: View {
    @ObservedObject var file: QTSQFile
    var onDismiss: () -> Void
    
    // All vertices (full dataset for save)
    @State private var allVertices: [EditableVertex] = []
    // Display subset + triangulation (for rendering)
    @State private var displayVerts: [EditableVertex] = []
    @State private var displayFaces: [TriFace] = []
    @State private var displayToFull: [Int] = []  // maps display index → allVertices index
    
    @State private var splats: [EditableSplat] = []
    @State private var imageWidth: CGFloat = 0
    @State private var imageHeight: CGFloat = 0
    @State private var editMode: MeshEditMode = .vertex
    @State private var editTool: MeshEditTool = .select
    @State private var selectedDisplayIdx: Int? = nil
    @State private var selectedSplatId: Int? = nil
    @State private var undoStack: [MeshUndoAction] = []
    @State private var isSaving = false
    @State private var statusMessage = ""
    @State private var zoom: CGFloat = 1.0
    @State private var panOffset: CGPoint = .zero
    @State private var previewImage: NSImage? = nil
    @State private var isLoading = true
    @State private var isDragging = false
    @State private var lastDragPos: CGPoint = .zero
    @State private var showWireframe = true
    @State private var canvasSize: CGSize = CGSize(width: 800, height: 600)
    @State private var dragOffset: CGPoint = .zero  // offset from vertex center to grab point
    @State private var isMovingElement = false       // true when dragging a selected element
    
    private let maxDisplayVerts = 5000
    
    var body: some View {
        VStack(spacing: 0) {
            toolbar
            HSplitView {
                canvas.frame(minWidth: 600)
                inspector.frame(width: 280)
            }
            statusBar
        }
        .background(Color(NSColor.windowBackgroundColor))
        .onAppear { loadMeshData() }
    }
    
    // MARK: - Toolbar
    
    private var toolbar: some View {
        HStack(spacing: 10) {
            Text("Mesh Editor")
                .font(.system(size: 16, weight: .bold))
            
            Divider().frame(height: 20)
            
            Picker("", selection: $editMode) {
                ForEach(MeshEditMode.allCases, id: \.self) { Text($0.rawValue).tag($0) }
            }
            .pickerStyle(.segmented).frame(width: 160)
            
            Divider().frame(height: 20)
            
            ForEach(MeshEditTool.allCases, id: \.self) { tool in
                Button(action: { editTool = tool }) {
                    VStack(spacing: 2) {
                        Image(systemName: tool == .select ? "cursorarrow" : "hand.draw")
                            .font(.system(size: 14))
                        Text(tool.rawValue).font(.system(size: 9, weight: .medium))
                    }
                    .frame(width: 50, height: 34)
                    .background(editTool == tool ? Color.accentColor.opacity(0.25) : Color.clear)
                    .cornerRadius(6)
                }
                .buttonStyle(.plain)
            }
            
            Divider().frame(height: 20)
            
            Toggle("Wireframe", isOn: $showWireframe)
                .toggleStyle(.switch)
                .font(.system(size: 11))
            
            Spacer()
            
            Button(action: performUndo) {
                Label("Undo", systemImage: "arrow.uturn.backward")
            }
            .disabled(undoStack.isEmpty)
            .keyboardShortcut("z", modifiers: .command)
            
            Button(action: renderPreview) {
                Label("Preview", systemImage: "eye").font(.system(size: 12, weight: .semibold))
            }
            .buttonStyle(.plain)
            .padding(.horizontal, 8).padding(.vertical, 4)
            .background(Color.blue.opacity(0.15)).cornerRadius(6)
            
            Button(action: saveChanges) {
                Label("Save", systemImage: "square.and.arrow.down").font(.system(size: 12, weight: .semibold))
            }
            .buttonStyle(.plain)
            .padding(.horizontal, 12).padding(.vertical, 4)
            .background(Color.green.opacity(0.8)).foregroundColor(.white).cornerRadius(6)
            .disabled(isSaving)
            
            Button(action: onDismiss) {
                Image(systemName: "xmark").font(.system(size: 14, weight: .semibold))
            }
            .buttonStyle(.plain)
        }
        .padding(.horizontal, 14).padding(.vertical, 6)
        .background(.regularMaterial)
    }
    
    // MARK: - Canvas
    
    private var canvas: some View {
        GeometryReader { geo in
            ZStack {
                Color(NSColor.controlBackgroundColor)
                if isLoading {
                    ProgressView("Loading mesh & computing triangulation...")
                } else {
                    Canvas { ctx, size in
                        canvasSize = size
                        drawMesh(ctx: ctx, size: size)
                    }
                    .gesture(meshGesture)
                    .gesture(MagnificationGesture().onChanged { zoom = max(0.1, min(30, $0)) })
                }
            }
            .clipped()
        }
    }
    
    private func drawMesh(ctx: GraphicsContext, size: CGSize) {
        let s = fitScale(size) * zoom
        
        // 1. Background image
        if let img = previewImage {
            let r = CGRect(x: panOffset.x, y: panOffset.y, width: imageWidth * s, height: imageHeight * s)
            ctx.draw(Image(nsImage: img), in: r)
        }
        
        if editMode == .vertex {
            // 2. Draw triangle edges (wireframe)
            if showWireframe {
                for face in displayFaces {
                    guard face.a < displayVerts.count && face.b < displayVerts.count && face.c < displayVerts.count else { continue }
                    let va = displayVerts[face.a]
                    let vb = displayVerts[face.b]
                    let vc = displayVerts[face.c]
                    
                    let pa = CGPoint(x: panOffset.x + va.x * s, y: panOffset.y + va.y * s)
                    let pb = CGPoint(x: panOffset.x + vb.x * s, y: panOffset.y + vb.y * s)
                    let pc = CGPoint(x: panOffset.x + vc.x * s, y: panOffset.y + vc.y * s)
                    
                    // Skip very large triangles (degenerate from super-triangle)
                    let maxEdge = max(
                        hypot(pb.x - pa.x, pb.y - pa.y),
                        max(hypot(pc.x - pb.x, pc.y - pb.y),
                            hypot(pa.x - pc.x, pa.y - pc.y))
                    )
                    if maxEdge > size.width * 0.5 { continue }
                    
                    var path = Path()
                    path.move(to: pa)
                    path.addLine(to: pb)
                    path.addLine(to: pc)
                    path.closeSubpath()
                    
                    ctx.stroke(path, with: .color(.white.opacity(0.35)), lineWidth: 0.5)
                }
            }
            
            // 3. Draw vertex dots
            let dotR: CGFloat = max(3, min(6, 4 / zoom))
            for (di, v) in displayVerts.enumerated() {
                let px = panOffset.x + v.x * s
                let py = panOffset.y + v.y * s
                
                // Frustum cull
                guard px > -dotR && px < size.width + dotR && py > -dotR && py < size.height + dotR else { continue }
                
                let isSelected = selectedDisplayIdx == di
                let r = isSelected ? dotR * 2 : dotR
                let rect = CGRect(x: px - r, y: py - r, width: r * 2, height: r * 2)
                
                ctx.fill(Path(ellipseIn: rect), with: .color(v.color))
                ctx.stroke(Path(ellipseIn: rect), with: .color(isSelected ? .cyan : .white), lineWidth: isSelected ? 2 : 0.8)
            }
        }
        
        // 4. Splats (in splat mode)
        if editMode == .splat {
            for (_, sp) in splats.enumerated() {
                let cx = panOffset.x + sp.x * s
                let cy = panOffset.y + sp.y * s
                let rx = CGFloat(sp.sx) * s * 0.5
                let ry = CGFloat(sp.sy) * s * 0.5
                let isSelected = selectedSplatId == sp.id
                
                let ellipse = Path(ellipseIn: CGRect(x: cx - rx, y: cy - ry, width: rx * 2, height: ry * 2))
                var ctx2 = ctx
                ctx2.opacity = Double(sp.alpha) / 255.0
                ctx2.fill(ellipse, with: .color(Color(red: Double(sp.r)/255, green: Double(sp.g)/255, blue: Double(sp.b)/255)))
                
                if isSelected {
                    ctx.stroke(ellipse, with: .color(.cyan), lineWidth: 2)
                }
            }
        }
    }
    
    // MARK: - Gesture
    
    private var meshGesture: some Gesture {
        DragGesture(minimumDistance: 0)
            .onChanged { val in
                let s = fitScale(canvasSize) * zoom
                
                if editTool == .pan {
                    if !isDragging { isDragging = true; lastDragPos = val.location }
                    panOffset.x += val.location.x - lastDragPos.x
                    panOffset.y += val.location.y - lastDragPos.y
                    lastDragPos = val.location
                    return
                }
                
                // === SELECT TOOL ===
                
                if !isDragging {
                    // FIRST TOUCH: select nearest vertex/splat (don't move anything yet)
                    isDragging = true
                    isMovingElement = false
                    
                    let imgX = (val.startLocation.x - panOffset.x) / s
                    let imgY = (val.startLocation.y - panOffset.y) / s
                    let hitR: CGFloat = 20 / s  // generous hit radius
                    
                    if editMode == .vertex {
                        var bestD2 = hitR * hitR
                        var bestIdx: Int? = nil
                        for (di, v) in displayVerts.enumerated() {
                            let d2 = (v.x - imgX) * (v.x - imgX) + (v.y - imgY) * (v.y - imgY)
                            if d2 < bestD2 { bestD2 = d2; bestIdx = di }
                        }
                        if let bi = bestIdx {
                            let fullIdx = displayToFull[bi]
                            // Record undo BEFORE any move
                            undoStack.append(.moveVertex(id: fullIdx, oldX: allVertices[fullIdx].x, oldY: allVertices[fullIdx].y))
                            selectedDisplayIdx = bi
                            selectedSplatId = nil
                            isMovingElement = true
                            // Store offset: difference between vertex position and click position (in image coords)
                            dragOffset = CGPoint(x: displayVerts[bi].x - imgX, y: displayVerts[bi].y - imgY)
                            statusMessage = "Selected vertex #\(fullIdx)"
                        } else {
                            selectedDisplayIdx = nil
                            isMovingElement = false
                            lastDragPos = val.location
                        }
                    } else {
                        var bestD2 = hitR * hitR
                        var bestId: Int? = nil
                        for sp in splats {
                            let d2 = (sp.x - imgX) * (sp.x - imgX) + (sp.y - imgY) * (sp.y - imgY)
                            if d2 < bestD2 { bestD2 = d2; bestId = sp.id }
                        }
                        if let id = bestId {
                            if let idx = splats.firstIndex(where: { $0.id == id }) {
                                undoStack.append(.moveSplat(id: id, oldX: splats[idx].x, oldY: splats[idx].y))
                                dragOffset = CGPoint(x: splats[idx].x - imgX, y: splats[idx].y - imgY)
                            }
                            selectedSplatId = id
                            selectedDisplayIdx = nil
                            isMovingElement = true
                            statusMessage = "Selected splat #\(id)"
                        } else {
                            selectedSplatId = nil
                            isMovingElement = false
                            lastDragPos = val.location
                        }
                    }
                    return  // Don't move on the first frame
                }
                
                // SUBSEQUENT FRAMES: move the selected element with offset
                if isMovingElement {
                    let imgX = (val.location.x - panOffset.x) / s
                    let imgY = (val.location.y - panOffset.y) / s
                    
                    if editMode == .vertex, let di = selectedDisplayIdx {
                        let fi = displayToFull[di]
                        let nx = max(0, min(imageWidth, imgX + dragOffset.x))
                        let ny = max(0, min(imageHeight, imgY + dragOffset.y))
                        displayVerts[di].x = nx
                        displayVerts[di].y = ny
                        allVertices[fi].x = nx
                        allVertices[fi].y = ny
                    } else if editMode == .splat, let sid = selectedSplatId,
                              let idx = splats.firstIndex(where: { $0.id == sid }) {
                        splats[idx].x = max(0, min(imageWidth, imgX + dragOffset.x))
                        splats[idx].y = max(0, min(imageHeight, imgY + dragOffset.y))
                    }
                } else {
                    // Nothing selected — pan fallback
                    panOffset.x += val.location.x - lastDragPos.x
                    panOffset.y += val.location.y - lastDragPos.y
                    lastDragPos = val.location
                }
            }
            .onEnded { _ in
                let wasMoving = isMovingElement
                isDragging = false
                isMovingElement = false
                
                // Auto-update preview when a vertex/splat was moved
                if wasMoving {
                    // Recompute wireframe triangulation after vertex move
                    if editMode == .vertex {
                        let w = imageWidth
                        let h = imageHeight
                        let pts = displayVerts.map { CGPoint(x: $0.x, y: $0.y) }
                        DispatchQueue.global(qos: .userInitiated).async {
                            let newFaces = bowyerWatson(points: pts, width: w, height: h)
                            DispatchQueue.main.async { displayFaces = newFaces }
                        }
                    }
                    // Auto-render preview so the image updates
                    renderPreview()
                }
            }
    }
    
    // MARK: - Inspector
    
    private var inspector: some View {
        ScrollView {
            VStack(alignment: .leading, spacing: 14) {
                VStack(alignment: .leading, spacing: 4) {
                    Text("MESH INFO").font(.system(size: 10, weight: .bold, design: .monospaced)).foregroundColor(.secondary)
                    Text("Image: \(Int(imageWidth)) × \(Int(imageHeight))").font(.system(size: 12, design: .monospaced))
                    Text("All vertices: \(allVertices.count)").font(.system(size: 12, design: .monospaced))
                    Text("Displayed: \(displayVerts.count)").font(.system(size: 12, design: .monospaced)).foregroundColor(.purple)
                    Text("Triangles: \(displayFaces.count)").font(.system(size: 12, design: .monospaced)).foregroundColor(.orange)
                    Text("Splats: \(splats.count)").font(.system(size: 12, design: .monospaced)).foregroundColor(.cyan)
                }
                .padding(10).frame(maxWidth: .infinity, alignment: .leading)
                .background(Color(NSColor.controlBackgroundColor)).cornerRadius(8)
                
                Divider()
                
                if editMode == .vertex, let di = selectedDisplayIdx, di < displayVerts.count {
                    let fi = displayToFull[di]
                    vertexEditor(displayIdx: di, fullIdx: fi)
                } else if editMode == .splat, let sid = selectedSplatId,
                          let idx = splats.firstIndex(where: { $0.id == sid }) {
                    splatEditor(index: idx)
                } else {
                    VStack(spacing: 8) {
                        Image(systemName: "cursorarrow.click.2").font(.system(size: 28)).foregroundColor(.secondary)
                        Text("Click a \(editMode == .vertex ? "vertex" : "splat") to select & drag")
                            .font(.system(size: 12)).foregroundColor(.secondary).multilineTextAlignment(.center)
                    }
                    .frame(maxWidth: .infinity).padding(30)
                }
                
                Spacer()
            }
            .padding(12)
        }
        .background(Color(NSColor.windowBackgroundColor))
    }
    
    private func vertexEditor(displayIdx di: Int, fullIdx fi: Int) -> some View {
        VStack(alignment: .leading, spacing: 10) {
            Text("VERTEX #\(fi)").font(.system(size: 10, weight: .bold, design: .monospaced)).foregroundColor(.purple)
            
            HStack {
                Text("X:").frame(width: 20)
                TextField("", value: Binding(
                    get: { Double(displayVerts[di].x) },
                    set: { let v = CGFloat($0); displayVerts[di].x = v; allVertices[fi].x = v }
                ), format: .number).textFieldStyle(.roundedBorder).frame(width: 80)
                Text("Y:").frame(width: 20)
                TextField("", value: Binding(
                    get: { Double(displayVerts[di].y) },
                    set: { let v = CGFloat($0); displayVerts[di].y = v; allVertices[fi].y = v }
                ), format: .number).textFieldStyle(.roundedBorder).frame(width: 80)
            }
            
            ColorPicker("Color:", selection: Binding(
                get: { displayVerts[di].color },
                set: { nc in
                    if let c = NSColor(nc).usingColorSpace(.deviceRGB) {
                        let nr = UInt8(c.redComponent * 255)
                        let ng = UInt8(c.greenComponent * 255)
                        let nb = UInt8(c.blueComponent * 255)
                        undoStack.append(.colorVertex(id: fi, oldR: allVertices[fi].r, oldG: allVertices[fi].g, oldB: allVertices[fi].b))
                        displayVerts[di].r = nr; displayVerts[di].g = ng; displayVerts[di].b = nb
                        allVertices[fi].r = nr; allVertices[fi].g = ng; allVertices[fi].b = nb
                    }
                }
            ))
        }
        .padding(10).background(Color(NSColor.controlBackgroundColor)).cornerRadius(8)
    }
    
    private func splatEditor(index: Int) -> some View {
        VStack(alignment: .leading, spacing: 8) {
            Text("SPLAT #\(splats[index].id)").font(.system(size: 10, weight: .bold, design: .monospaced)).foregroundColor(.cyan)
            
            HStack {
                Text("X:").frame(width: 20)
                TextField("", value: Binding(get: { Double(splats[index].x) }, set: { splats[index].x = CGFloat($0) }), format: .number)
                    .textFieldStyle(.roundedBorder).frame(width: 80)
                Text("Y:").frame(width: 20)
                TextField("", value: Binding(get: { Double(splats[index].y) }, set: { splats[index].y = CGFloat($0) }), format: .number)
                    .textFieldStyle(.roundedBorder).frame(width: 80)
            }
            
            makeSlider("σx", val: $splats[index].sx)
            makeSlider("σy", val: $splats[index].sy)
            makeSlider("θ", val: $splats[index].theta)
            makeSlider("α", val: $splats[index].alpha)
            
            ColorPicker("Color:", selection: Binding(
                get: { Color(red: Double(splats[index].r)/255, green: Double(splats[index].g)/255, blue: Double(splats[index].b)/255) },
                set: { nc in
                    if let c = NSColor(nc).usingColorSpace(.deviceRGB) {
                        splats[index].r = UInt8(c.redComponent * 255)
                        splats[index].g = UInt8(c.greenComponent * 255)
                        splats[index].b = UInt8(c.blueComponent * 255)
                    }
                }
            ))
        }
        .padding(10).background(Color(NSColor.controlBackgroundColor)).cornerRadius(8)
    }
    
    private func makeSlider(_ label: String, val: Binding<UInt8>) -> some View {
        HStack {
            Text(label + ":").frame(width: 24)
            Slider(value: Binding(get: { Double(val.wrappedValue) }, set: { val.wrappedValue = UInt8(min(255, max(0, Int($0)))) }), in: 0...255)
            Text("\(val.wrappedValue)").font(.system(size: 10, design: .monospaced)).frame(width: 30)
        }
    }
    
    // MARK: - Status Bar
    
    private var statusBar: some View {
        HStack {
            Text(statusMessage.isEmpty ? "Select tool + click vertex to select, drag to move" : statusMessage)
                .font(.system(size: 11, design: .monospaced)).foregroundColor(.secondary)
            Spacer()
            Text("Zoom: \(Int(zoom * 100))%").font(.system(size: 11, design: .monospaced)).foregroundColor(.secondary)
            Button("-") { zoom = max(0.1, zoom * 0.75) }.buttonStyle(.plain)
            Button("+") { zoom = min(30, zoom * 1.33) }.buttonStyle(.plain)
            Button("Fit") { zoom = 1.0; panOffset = .zero }.buttonStyle(.plain)
        }
        .padding(.horizontal, 14).padding(.vertical, 5)
        .background(.regularMaterial)
    }
    
    // MARK: - Load Data
    
    private func loadMeshData() {
        isLoading = true
        statusMessage = "Loading vertices & computing triangulation..."
        
        DispatchQueue.global(qos: .userInitiated).async {
            // 1. Extract ALL vertices
            let maxV: UInt32 = 800_000
            var flat = [Float](repeating: 0, count: Int(maxV) * 5)
            let nv = qtsq_extract_vertices(&file.ctx, &flat, maxV)
            
            var allV: [EditableVertex] = []
            if nv > 0 {
                allV.reserveCapacity(Int(nv))
                for i in 0..<Int(nv) {
                    allV.append(EditableVertex(id: i,
                        x: CGFloat(flat[i*5]), y: CGFloat(flat[i*5+1]),
                        r: UInt8(min(255, max(0, flat[i*5+2]))),
                        g: UInt8(min(255, max(0, flat[i*5+3]))),
                        b: UInt8(min(255, max(0, flat[i*5+4])))
                    ))
                }
            }
            
            // 2. Subsample for display
            let step = max(1, allV.count / maxDisplayVerts)
            var dispV: [EditableVertex] = []
            var d2f: [Int] = []
            for i in stride(from: 0, to: allV.count, by: step) {
                let newId = dispV.count
                dispV.append(EditableVertex(id: newId, x: allV[i].x, y: allV[i].y, r: allV[i].r, g: allV[i].g, b: allV[i].b))
                d2f.append(i)
            }
            
            // 3. Compute Delaunay triangulation (Bowyer-Watson)
            let w = CGFloat(file.ctx.schema.dimensions.0)
            let h = CGFloat(file.ctx.schema.dimensions.1)
            let faces = bowyerWatson(points: dispV.map { CGPoint(x: $0.x, y: $0.y) }, width: w, height: h)
            
            // 4. Extract splats
            let maxS: UInt32 = 50_000
            var splatBuf = [splat2d_t](repeating: splat2d_t(), count: Int(maxS))
            var sw: UInt32 = 0, sh: UInt32 = 0
            let ns = qtsq_extract_splats(&file.ctx, &splatBuf, maxS, &sw, &sh)
            var loadedSplats: [EditableSplat] = []
            if ns > 0 {
                for i in 0..<Int(ns) {
                    let s = splatBuf[i]
                    loadedSplats.append(EditableSplat(id: i, x: CGFloat(s.x), y: CGFloat(s.y),
                        sx: s.sx, sy: s.sy, theta: s.theta, r: s.r, g: s.g, b: s.b, alpha: s.alpha, z: s.z))
                }
            }
            
            DispatchQueue.main.async {
                self.allVertices = allV
                self.displayVerts = dispV
                self.displayToFull = d2f
                self.displayFaces = faces
                self.splats = loadedSplats
                self.imageWidth = w > 0 ? w : 1024
                self.imageHeight = h > 0 ? h : 1024
                self.isLoading = false
                self.statusMessage = "Loaded \(allV.count) verts → \(dispV.count) displayed, \(faces.count) triangles"
                
                if let data = file.data {
                    self.previewImage = createPreviewImage(from: data, w: UInt32(self.imageWidth), h: UInt32(self.imageHeight))
                }
            }
        }
    }
    
    // MARK: - Bowyer-Watson Delaunay Triangulation
    
    private func bowyerWatson(points: [CGPoint], width: CGFloat, height: CGFloat) -> [TriFace] {
        guard points.count >= 3 else { return [] }
        
        struct Tri { var a: Int; var b: Int; var c: Int }
        struct Edge: Hashable {
            var a: Int; var b: Int
            init(_ a: Int, _ b: Int) { self.a = min(a, b); self.b = max(a, b) }
        }
        
        // Super-triangle vertices (indices: n, n+1, n+2)
        let n = points.count
        let margin: CGFloat = 100
        let superA = CGPoint(x: -margin, y: -margin)
        let superB = CGPoint(x: (width + margin) * 2, y: -margin)
        let superC = CGPoint(x: -margin, y: (height + margin) * 2)
        
        var pts = points
        pts.append(superA); pts.append(superB); pts.append(superC)
        
        var triangles: [Tri] = [Tri(a: n, b: n+1, c: n+2)]
        
        func circumcircleContains(_ tri: Tri, _ p: CGPoint) -> Bool {
            let ax = pts[tri.a].x, ay = pts[tri.a].y
            let bx = pts[tri.b].x, by = pts[tri.b].y
            let cx = pts[tri.c].x, cy = pts[tri.c].y
            
            let d = 2 * (ax * (by - cy) + bx * (cy - ay) + cx * (ay - by))
            if abs(d) < 1e-10 { return false }
            
            let ux = ((ax*ax + ay*ay) * (by - cy) + (bx*bx + by*by) * (cy - ay) + (cx*cx + cy*cy) * (ay - by)) / d
            let uy = ((ax*ax + ay*ay) * (cx - bx) + (bx*bx + by*by) * (ax - cx) + (cx*cx + cy*cy) * (bx - ax)) / d
            let r2 = (ax - ux) * (ax - ux) + (ay - uy) * (ay - uy)
            let d2 = (p.x - ux) * (p.x - ux) + (p.y - uy) * (p.y - uy)
            return d2 < r2
        }
        
        for i in 0..<n {
            let p = pts[i]
            var badTris: [Tri] = []
            
            for t in triangles {
                if circumcircleContains(t, p) { badTris.append(t) }
            }
            
            // Find boundary edges
            var edgeCount: [Edge: Int] = [:]
            for t in badTris {
                for e in [Edge(t.a, t.b), Edge(t.b, t.c), Edge(t.a, t.c)] {
                    edgeCount[e, default: 0] += 1
                }
            }
            let boundary = edgeCount.filter { $0.value == 1 }.map { $0.key }
            
            // Remove bad triangles
            triangles.removeAll { t in
                badTris.contains { $0.a == t.a && $0.b == t.b && $0.c == t.c }
            }
            
            // Create new triangles
            for e in boundary {
                triangles.append(Tri(a: e.a, b: e.b, c: i))
            }
        }
        
        // Remove triangles that reference super-triangle vertices
        triangles.removeAll { $0.a >= n || $0.b >= n || $0.c >= n }
        
        return triangles.map { TriFace(a: $0.a, b: $0.b, c: $0.c) }
    }
    
    // MARK: - Actions
    
    private func performUndo() {
        guard let action = undoStack.popLast() else { return }
        switch action {
        case .moveVertex(let id, let oldX, let oldY):
            allVertices[id].x = oldX; allVertices[id].y = oldY
            if let di = displayToFull.firstIndex(of: id) {
                displayVerts[di].x = oldX; displayVerts[di].y = oldY
            }
        case .colorVertex(let id, let r, let g, let b):
            allVertices[id].r = r; allVertices[id].g = g; allVertices[id].b = b
            if let di = displayToFull.firstIndex(of: id) {
                displayVerts[di].r = r; displayVerts[di].g = g; displayVerts[di].b = b
            }
        case .moveSplat(let id, let oldX, let oldY):
            if let idx = splats.firstIndex(where: { $0.id == id }) {
                splats[idx].x = oldX; splats[idx].y = oldY
            }
        }
        statusMessage = "Undo applied"
    }
    
    private func renderPreview() {
        statusMessage = "Rendering preview..."
        DispatchQueue.global(qos: .userInitiated).async {
            // Apply edits directly to the file context (modifies singularity in-place)
            applyEditsToContext(&file.ctx)
            
            // Now decompress to see the result
            var outPtr: UnsafeMutablePointer<UInt8>? = nil
            var outSize: Int = 0
            let rc = qtsq_decompress(&file.ctx, &outPtr, &outSize)
            if rc == 0, let ptr = outPtr, outSize > 0 {
                let w = file.ctx.schema.dimensions.0
                let h = file.ctx.schema.dimensions.1
                let img = createPreviewImage(from: Data(bytes: ptr, count: outSize), w: w, h: h)
                free(ptr)
                DispatchQueue.main.async {
                    self.previewImage = img
                    self.statusMessage = "Preview rendered — changes applied"
                }
            } else {
                DispatchQueue.main.async { self.statusMessage = "Preview failed (rc=\(rc))" }
            }
        }
    }
    
    private func saveChanges() {
        isSaving = true; statusMessage = "Saving..."
        DispatchQueue.global(qos: .userInitiated).async {
            // Apply edits to the live context
            applyEditsToContext(&file.ctx)
            // Write to file
            let rc = qtsqPromptAndEncrypt(&file.ctx) ? file.path.withCString { qtsq_write(&file.ctx, $0) } : -1
            DispatchQueue.main.async {
                isSaving = false
                if rc == 0 {
                    statusMessage = "✅ Saved to \(file.name)"
                    // Re-decompress so the viewer shows updated image
                    file.performDecompress()
                } else {
                    statusMessage = "❌ Save failed (rc=\(rc))"
                }
            }
        }
    }
    
    private func applyEditsToContext(_ ctx: inout qtsq_context_t) {
        // Apply vertex changes
        var flat = [Float](repeating: 0, count: allVertices.count * 5)
        for (i, v) in allVertices.enumerated() {
            flat[i*5] = Float(v.x); flat[i*5+1] = Float(v.y)
            flat[i*5+2] = Float(v.r); flat[i*5+3] = Float(v.g); flat[i*5+4] = Float(v.b)
        }
        let vrc = qtsq_apply_vertices(&ctx, flat, UInt32(allVertices.count))
        if vrc != 0 {
            print("[MeshEditor] qtsq_apply_vertices failed with rc=\(vrc)")
        }
        
        // Apply splat changes
        if !splats.isEmpty {
            var arr = splats.map { s -> splat2d_t in
                var sp = splat2d_t()
                sp.x = UInt16(max(0, min(65535, s.x))); sp.y = UInt16(max(0, min(65535, s.y)))
                sp.sx = s.sx; sp.sy = s.sy; sp.theta = s.theta
                sp.r = s.r; sp.g = s.g; sp.b = s.b; sp.alpha = s.alpha; sp.z = s.z
                return sp
            }
            let src = qtsq_apply_splats(&ctx, &arr, UInt32(splats.count), UInt32(imageWidth), UInt32(imageHeight))
            if src != 0 {
                print("[MeshEditor] qtsq_apply_splats failed with rc=\(src)")
            }
        }
    }
    
    // MARK: - Helpers
    
    private func fitScale(_ size: CGSize) -> CGFloat {
        guard imageWidth > 0, imageHeight > 0 else { return 1 }
        return min(size.width / imageWidth, size.height / imageHeight) * 0.9
    }
    
    private func createPreviewImage(from data: Data, w: UInt32, h: UInt32) -> NSImage? {
        guard w > 0, h > 0 else { return nil }
        let ch = data.count / (Int(w) * Int(h))
        guard ch >= 1, ch <= 4 else { return nil }
        let stride = Int(w) * 4
        guard let rep = NSBitmapImageRep(bitmapDataPlanes: nil, pixelsWide: Int(w), pixelsHigh: Int(h),
            bitsPerSample: 8, samplesPerPixel: 4, hasAlpha: true, isPlanar: false,
            colorSpaceName: .calibratedRGB, bytesPerRow: stride, bitsPerPixel: 32),
              let out = rep.bitmapData else { return nil }
        data.withUnsafeBytes { buf in
            guard let src = buf.bindMemory(to: UInt8.self).baseAddress else { return }
            for y in 0..<Int(h) {
                for x in 0..<Int(w) {
                    let si = (y * Int(w) + x) * ch, di = y * stride + x * 4
                    guard si + ch <= data.count else { return }
                    out[di] = src[si]; out[di+1] = ch >= 2 ? src[si+1] : src[si]; out[di+2] = ch >= 3 ? src[si+2] : src[si]; out[di+3] = 255
                }
            }
        }
        let img = NSImage(size: NSSize(width: Int(w), height: Int(h)))
        img.addRepresentation(rep)
        return img
    }
}
