// Renders the core's draw list: one pipeline, textured triangles, scissor per command.
import Metal
import MetalKit

final class Renderer {
  private let device: MTLDevice
  private let queue: MTLCommandQueue
  private let pipeline: MTLRenderPipelineState
  private let sampler: MTLSamplerState
  private var textures: [UInt32: MTLTexture] = [:]

  private static let shader = """
  #include <metal_stdlib>
  using namespace metal;
  struct VIn { float2 pos [[attribute(0)]]; float2 uv [[attribute(1)]]; float4 color [[attribute(2)]]; };
  struct VOut { float4 pos [[position]]; float2 uv; float4 color; };
  struct Uniforms { float2 viewport; };
  vertex VOut vs(VIn in [[stage_in]], constant Uniforms& u [[buffer(1)]]) {
    VOut o;
    o.pos = float4(in.pos.x / u.viewport.x * 2.0 - 1.0, 1.0 - in.pos.y / u.viewport.y * 2.0, 0.0, 1.0);
    o.uv = in.uv;
    o.color = in.color;
    return o;
  }
  fragment float4 fs(VOut in [[stage_in]], texture2d<float> tex [[texture(0)]], sampler s [[sampler(0)]]) {
    return float4(in.color.rgb, in.color.a * tex.sample(s, in.uv).r);
  }
  """

  init?(device: MTLDevice, pixelFormat: MTLPixelFormat) {
    guard let queue = device.makeCommandQueue() else { return nil }
    let library: MTLLibrary
    do {
      library = try device.makeLibrary(source: Renderer.shader, options: nil)
    } catch {
      NSLog("DeckHand: shader compile failed: %@", String(describing: error))
      return nil
    }
    let vertex = MTLVertexDescriptor()
    vertex.attributes[0].format = .float2
    vertex.attributes[0].offset = 0
    vertex.attributes[1].format = .float2
    vertex.attributes[1].offset = 8
    vertex.attributes[2].format = .uchar4Normalized
    vertex.attributes[2].offset = 16
    vertex.layouts[0].stride = MemoryLayout<dh_vertex>.stride

    let descriptor = MTLRenderPipelineDescriptor()
    descriptor.vertexFunction = library.makeFunction(name: "vs")
    descriptor.fragmentFunction = library.makeFunction(name: "fs")
    descriptor.vertexDescriptor = vertex
    let color = descriptor.colorAttachments[0]!
    color.pixelFormat = pixelFormat
    color.isBlendingEnabled = true
    color.sourceRGBBlendFactor = .sourceAlpha
    color.destinationRGBBlendFactor = .oneMinusSourceAlpha
    color.sourceAlphaBlendFactor = .one
    color.destinationAlphaBlendFactor = .oneMinusSourceAlpha

    let samplerDescriptor = MTLSamplerDescriptor()
    samplerDescriptor.minFilter = .linear
    samplerDescriptor.magFilter = .linear
    let pipeline: MTLRenderPipelineState
    do {
      pipeline = try device.makeRenderPipelineState(descriptor: descriptor)
    } catch {
      NSLog("DeckHand: pipeline creation failed: %@", String(describing: error))
      return nil
    }
    guard let sampler = device.makeSamplerState(descriptor: samplerDescriptor) else { return nil }
    self.device = device
    self.queue = queue
    self.pipeline = pipeline
    self.sampler = sampler
  }

  private func apply(_ u: dh_tex_upload) {
    guard let pixels = u.pixels, u.w > 0, u.h > 0 else { return }
    var texture = textures[u.id]
    if texture == nil || texture!.width != Int(u.tex_w) || texture!.height != Int(u.tex_h) {
      let d = MTLTextureDescriptor.texture2DDescriptor(
        pixelFormat: u.fmt == DH_TEX_A8 ? .r8Unorm : .rgba8Unorm,
        width: Int(u.tex_w), height: Int(u.tex_h), mipmapped: false)
      d.usage = .shaderRead
      texture = device.makeTexture(descriptor: d)
      textures[u.id] = texture
    }
    texture?.replace(region: MTLRegionMake2D(Int(u.x), Int(u.y), Int(u.w), Int(u.h)), mipmapLevel: 0,
                     withBytes: pixels, bytesPerRow: Int(u.stride))
  }

  func render(_ f: dh_frame, drawable: CAMetalDrawable, pass: MTLRenderPassDescriptor, size: CGSize) {
    for i in 0..<Int(f.upload_count) { apply(f.uploads[i]) }
    for i in 0..<Int(f.release_count) { textures[f.releases[i]] = nil }

    pass.colorAttachments[0].loadAction = .clear
    pass.colorAttachments[0].clearColor = MTLClearColor(
      red: Double(f.clear_rgba.0), green: Double(f.clear_rgba.1), blue: Double(f.clear_rgba.2),
      alpha: Double(f.clear_rgba.3))
    guard let commands = queue.makeCommandBuffer(),
          let encoder = commands.makeRenderCommandEncoder(descriptor: pass) else { return }

    let width = Int(size.width), height = Int(size.height)
    if f.vtx_count > 0, f.idx_count > 0, width > 0, height > 0,
       let vertices = device.makeBuffer(bytes: f.vtx, length: Int(f.vtx_count) * MemoryLayout<dh_vertex>.stride,
                                        options: .storageModeShared),
       let indices = device.makeBuffer(bytes: f.idx, length: Int(f.idx_count) * 4, options: .storageModeShared) {
      encoder.setRenderPipelineState(pipeline)
      encoder.setVertexBuffer(vertices, offset: 0, index: 0)
      var viewport = SIMD2<Float>(Float(width), Float(height))
      encoder.setVertexBytes(&viewport, length: MemoryLayout<SIMD2<Float>>.stride, index: 1)
      encoder.setFragmentSamplerState(sampler, index: 0)
      for i in 0..<Int(f.cmd_count) {
        let c = f.cmds[i]
        guard let texture = textures[c.texture_id], c.idx_count > 0 else { continue }
        let x0 = max(0, Int(c.clip.0)), y0 = max(0, Int(c.clip.1))
        let x1 = min(width, Int(c.clip.2)), y1 = min(height, Int(c.clip.3))
        if x1 <= x0 || y1 <= y0 { continue }
        encoder.setScissorRect(MTLScissorRect(x: x0, y: y0, width: x1 - x0, height: y1 - y0))
        encoder.setFragmentTexture(texture, index: 0)
        encoder.drawIndexedPrimitives(type: .triangle, indexCount: Int(c.idx_count), indexType: .uint32,
                                      indexBuffer: indices, indexBufferOffset: Int(c.idx_offset) * 4,
                                      instanceCount: 1, baseVertex: Int(c.vtx_offset), baseInstance: 0)
      }
    }
    encoder.endEncoding()
    commands.present(drawable)
    commands.commit()
  }
}
