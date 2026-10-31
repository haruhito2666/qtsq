import Foundation
import CoreML
import AppKit

/// MeshNet V3 — CoreML Inference Wrapper
///
/// The CoreML model was traced with img_size=64. That means:
///   - The image input must be 64×64 pixels, [0,1] normalized, [1,3,64,64]
///   - Vertex (x,y) must be scaled to the 64×64 coordinate space
///   - Vertex (r,g,b) stay in [0,255] (the model normalizes internally)
///   - Output vertices are in the 64×64 coordinate space — must be scaled back
class MeshNetV3Service {
    static let shared = MeshNetV3Service()
    
    private var model: MLModel?
    private let modelImgSize = 64  // Must match CoreMLWrapper.img_size in export
    private let maxVerts = 1024
    
    private init() {
        loadModel()
    }
    
    private func loadModel() {
        let candidates = ["MeshNetV3"]
        let extensions = ["mlmodelc", "mlpackage"]
        for name in candidates {
            for ext in extensions {
                if let url = Bundle.main.url(forResource: name, withExtension: ext) {
                    if let m = try? MLModel(contentsOf: url) {
                        model = m
                        print("✅ MeshNet V3 loaded (\(ext))")
                        return
                    }
                }
            }
        }
        print("⚠️ MeshNet V3 not found in bundle — V3 optimizations disabled")
    }
    
    var isAvailable: Bool { model != nil }
    
    /// Optimize mesh vertices using the trained AI model.
    ///
    /// Flow:
    ///  1. Extract raw vertices from C engine (pixel coords of original image)
    ///  2. Scale vertex (x,y) from original image space → 64×64 model space
    ///  3. Build MLMultiArrays and run inference
    ///  4. Scale corrected (x,y) back from 64×64 → original image space
    ///  5. Write corrected vertices back into the C engine
    func optimizeMesh(ctx: UnsafeMutablePointer<qtsq_context_t>,
                      originalRGB: UnsafePointer<UInt8>,
                      width: Int, height: Int) -> Bool {
        guard let model = model else { return false }
        
        let flatVerts = UnsafeMutablePointer<Float>.allocate(capacity: maxVerts * 5)
        defer { flatVerts.deallocate() }
        
        // 1. Extract raw vertices (x,y in original pixel coords, r,g,b in [0,255])
        let numVerts = qtsq_extract_vertices(ctx, flatVerts, UInt32(maxVerts))
        if numVerts <= 0 { return false }
        
        let n = Int(numVerts)
        let scaleX = Float(modelImgSize) / Float(width)
        let scaleY = Float(modelImgSize) / Float(height)
        
        // 2. Build vertex MLMultiArray — scale (x,y) to 64×64 space
        guard let vertexArray = try? MLMultiArray(shape: [1, NSNumber(value: n), 5], dataType: .float32),
              let imageArray = try? MLMultiArray(shape: [1, 3, NSNumber(value: modelImgSize), NSNumber(value: modelImgSize)], dataType: .float32) else {
            return false
        }
        
        for i in 0..<n {
            let rawX = flatVerts[i * 5 + 0]
            let rawY = flatVerts[i * 5 + 1]
            let r = flatVerts[i * 5 + 2]
            let g = flatVerts[i * 5 + 3]
            let b = flatVerts[i * 5 + 4]
            
            // Scale x,y to model's 64×64 coordinate space
            vertexArray[[0, NSNumber(value: i), 0]] = NSNumber(value: rawX * scaleX)
            vertexArray[[0, NSNumber(value: i), 1]] = NSNumber(value: rawY * scaleY)
            vertexArray[[0, NSNumber(value: i), 2]] = NSNumber(value: r)
            vertexArray[[0, NSNumber(value: i), 3]] = NSNumber(value: g)
            vertexArray[[0, NSNumber(value: i), 4]] = NSNumber(value: b)
        }
        
        // 3. Build image MLMultiArray — [1, 3, 64, 64] normalized to [0, 1]
        for y in 0..<modelImgSize {
            for x in 0..<modelImgSize {
                let srcX = min(x * width / modelImgSize, width - 1)
                let srcY = min(y * height / modelImgSize, height - 1)
                let srcIdx = (srcY * width + srcX) * 3
                
                imageArray[[0, 0, NSNumber(value: y), NSNumber(value: x)]] = NSNumber(value: Float(originalRGB[srcIdx]) / 255.0)
                imageArray[[0, 1, NSNumber(value: y), NSNumber(value: x)]] = NSNumber(value: Float(originalRGB[srcIdx+1]) / 255.0)
                imageArray[[0, 2, NSNumber(value: y), NSNumber(value: x)]] = NSNumber(value: Float(originalRGB[srcIdx+2]) / 255.0)
            }
        }
        
        // 4. Run ANE inference
        do {
            let provider = try MLDictionaryFeatureProvider(dictionary: [
                "vertices": vertexArray,
                "image": imageArray
            ])
            
            let result = try model.prediction(from: provider)
            guard let corrected = result.featureValue(for: "corrected_vertices")?.multiArrayValue else {
                return false
            }
            
            // 5. Scale corrected (x,y) back to original image space
            let invScaleX = Float(width) / Float(modelImgSize)
            let invScaleY = Float(height) / Float(modelImgSize)
            
            for i in 0..<n {
                let cx = corrected[[0, NSNumber(value: i), 0]].floatValue * invScaleX
                let cy = corrected[[0, NSNumber(value: i), 1]].floatValue * invScaleY
                let cr = corrected[[0, NSNumber(value: i), 2]].floatValue
                let cg = corrected[[0, NSNumber(value: i), 3]].floatValue
                let cb = corrected[[0, NSNumber(value: i), 4]].floatValue
                
                // Clamp to original image bounds
                flatVerts[i * 5 + 0] = max(0, min(cx, Float(width - 1)))
                flatVerts[i * 5 + 1] = max(0, min(cy, Float(height - 1)))
                flatVerts[i * 5 + 2] = max(0, min(cr, 255))
                flatVerts[i * 5 + 3] = max(0, min(cg, 255))
                flatVerts[i * 5 + 4] = max(0, min(cb, 255))
            }
            
            // 6. Write back to C engine
            let applyResult = qtsq_apply_vertices(ctx, flatVerts, UInt32(numVerts))
            return applyResult == 0 // QTSQ_OK
            
        } catch {
            print("⚠️ MeshNet V3 inference error: \(error.localizedDescription)")
            return false
        }
    }
}
