import Metal
import MetalKit

/// One pipeline: positions in pixels, UV, RGBA8 colour, texture sampled (A8 -> alpha, RGBA8 -> rgba), scissor per command.
final class Renderer {
    let device: MTLDevice
    let queue: MTLCommandQueue
    private let pipeline: MTLRenderPipelineState
    private let sampler: MTLSamplerState
    private var textures: [UInt32: MTLTexture] = [:]
    private var vbuf: MTLBuffer?, ibuf: MTLBuffer?

    static let source = """
    #include <metal_stdlib>
    using namespace metal;
    struct VIn { float2 pos [[attribute(0)]]; float2 uv [[attribute(1)]]; uchar4 col [[attribute(2)]]; };
    struct VOut { float4 pos [[position]]; float2 uv; float4 col; };
    vertex VOut vs(VIn in [[stage_in]], constant float2& vp [[buffer(1)]]) {
        VOut o; o.pos = float4(in.pos.x / vp.x * 2.0 - 1.0, 1.0 - in.pos.y / vp.y * 2.0, 0, 1);
        o.uv = in.uv; o.col = float4(in.col) / 255.0; return o; }
    fragment float4 fs(VOut in [[stage_in]], texture2d<float> t [[texture(0)]], sampler s [[sampler(0)]]) {
        float4 c = t.sample(s, in.uv);
        // A8 textures read as (a,0,0,1) via swizzle set at creation: r=g=b=1, a=alpha.
        return in.col * c; }
    """

    init?(view: MTKView) {
        guard let d = MTLCreateSystemDefaultDevice(), let q = d.makeCommandQueue() else { return nil }
        device = d; queue = q
        view.device = d
        view.colorPixelFormat = .bgra8Unorm
        guard let lib = try? d.makeLibrary(source: Renderer.source, options: nil) else { return nil }
        let vd = MTLVertexDescriptor()
        vd.attributes[0].format = .float2; vd.attributes[0].offset = 0; vd.attributes[0].bufferIndex = 0
        vd.attributes[1].format = .float2; vd.attributes[1].offset = 8; vd.attributes[1].bufferIndex = 0
        vd.attributes[2].format = .uchar4; vd.attributes[2].offset = 16; vd.attributes[2].bufferIndex = 0
        vd.layouts[0].stride = MemoryLayout<dh_vertex>.stride
        let pd = MTLRenderPipelineDescriptor()
        pd.vertexFunction = lib.makeFunction(name: "vs"); pd.fragmentFunction = lib.makeFunction(name: "fs")
        pd.vertexDescriptor = vd
        let ca = pd.colorAttachments[0]!
        ca.pixelFormat = view.colorPixelFormat; ca.isBlendingEnabled = true
        ca.sourceRGBBlendFactor = .sourceAlpha; ca.destinationRGBBlendFactor = .oneMinusSourceAlpha
        ca.sourceAlphaBlendFactor = .one; ca.destinationAlphaBlendFactor = .oneMinusSourceAlpha
        guard let p = try? d.makeRenderPipelineState(descriptor: pd) else { return nil }
        pipeline = p
        let sd = MTLSamplerDescriptor(); sd.minFilter = .linear; sd.magFilter = .linear
        sampler = d.makeSamplerState(descriptor: sd)!
    }

    private func upload(_ u: dh_tex_upload) {
        let a8 = u.fmt == DH_TEX_A8
        let desc = MTLTextureDescriptor.texture2DDescriptor(pixelFormat: a8 ? .r8Unorm : .rgba8Unorm,
                                                            width: Int(u.w), height: Int(u.h), mipmapped: false)
        if a8 { desc.swizzle = MTLTextureSwizzleChannels(red: .one, green: .one, blue: .one, alpha: .red) }
        guard let t = device.makeTexture(descriptor: desc), let px = u.pixels else { return }
        t.replace(region: MTLRegionMake2D(0, 0, Int(u.w), Int(u.h)), mipmapLevel: 0, withBytes: px,
                  bytesPerRow: Int(u.w) * (a8 ? 1 : 4))
        textures[u.id] = t
    }

    func draw(frame f: dh_frame, in view: MTKView) {
        for i in 0..<Int(f.upload_count) { upload(f.uploads[i]) }
        for i in 0..<Int(f.release_count) { textures[f.releases[i]] = nil }
        guard let rpd = view.currentRenderPassDescriptor, let drawable = view.currentDrawable,
              let cb = queue.makeCommandBuffer() else { return }
        rpd.colorAttachments[0].clearColor = MTLClearColor(red: Double(f.clear_rgba.0), green: Double(f.clear_rgba.1),
                                                           blue: Double(f.clear_rgba.2), alpha: Double(f.clear_rgba.3))
        rpd.colorAttachments[0].loadAction = .clear
        guard let enc = cb.makeRenderCommandEncoder(descriptor: rpd) else { return }
        if f.vtx_count > 0 && f.idx_count > 0 {
            vbuf = grow(vbuf, Int(f.vtx_count) * MemoryLayout<dh_vertex>.stride)
            ibuf = grow(ibuf, Int(f.idx_count) * 4)
            memcpy(vbuf!.contents(), f.vtx, Int(f.vtx_count) * MemoryLayout<dh_vertex>.stride)
            memcpy(ibuf!.contents(), f.idx, Int(f.idx_count) * 4)
            enc.setRenderPipelineState(pipeline)
            var vp = SIMD2<Float>(Float(view.drawableSize.width), Float(view.drawableSize.height))
            enc.setVertexBytes(&vp, length: 8, index: 1)
            enc.setFragmentSamplerState(sampler, index: 0)
            let W = Int(view.drawableSize.width), H = Int(view.drawableSize.height)
            for i in 0..<Int(f.cmd_count) {
                let c = f.cmds[i]
                guard let tex = textures[c.texture_id] else { continue }
                let x0 = max(0, min(W, Int(c.clip.0))), y0 = max(0, min(H, Int(c.clip.1)))
                let x1 = max(x0, min(W, Int(c.clip.2))), y1 = max(y0, min(H, Int(c.clip.3)))
                if x1 == x0 || y1 == y0 { continue }
                enc.setScissorRect(MTLScissorRect(x: x0, y: y0, width: x1 - x0, height: y1 - y0))
                enc.setVertexBuffer(vbuf, offset: Int(c.vtx_offset) * MemoryLayout<dh_vertex>.stride, index: 0)
                enc.setFragmentTexture(tex, index: 0)
                enc.drawIndexedPrimitives(type: .triangle, indexCount: Int(c.idx_count), indexType: .uint32,
                                          indexBuffer: ibuf!, indexBufferOffset: Int(c.idx_offset) * 4)
            }
        }
        enc.endEncoding()
        cb.present(drawable)
        cb.commit()
    }

    private func grow(_ b: MTLBuffer?, _ n: Int) -> MTLBuffer? {
        if let b, b.length >= n { return b }
        return device.makeBuffer(length: max(n, 64 * 1024), options: .storageModeShared)
    }
}
