import AppKit

enum StatusMark {
    case idle, microphone, voice, both, muted

    private static let microphoneBlue = NSColor(srgbRed: 0, green: 0.67, blue: 1, alpha: 1)
    private static let voiceViolet = NSColor(srgbRed: 0.67, green: 0.46, blue: 1, alpha: 1)

    init(running: Bool, microphone: Bool, voice: Bool, muted: Bool) {
        if muted { self = .muted }
        else if !running { self = .idle }
        else if microphone && voice { self = .both }
        else if voice { self = .voice }
        else { self = microphone ? .microphone : .idle }
    }

    var description: String {
        switch self {
        case .idle: return "Inactive"
        case .microphone: return "Microphone enabled"
        case .voice: return "Voice conversion enabled"
        case .both: return "Microphone and voice conversion enabled"
        case .muted: return "Output muted"
        }
    }

    func image(using mark: CGImage) -> NSImage {
        let size = NSSize(width: 22, height: 22 * CGFloat(mark.height) / CGFloat(mark.width))
        let image = NSImage(size: size, flipped: false) { rect in
            guard let ctx = NSGraphicsContext.current?.cgContext else { return false }
            ctx.saveGState()
            defer { ctx.restoreGState() }
            ctx.clip(to: rect, mask: mark)
            if self == .idle || self == .muted {
                (self == .idle ? NSColor.black : .systemRed).setFill()
                ctx.fill(rect)
            } else {
                (self == .voice || self == .both ? Self.voiceViolet : .systemGray).setFill()
                ctx.fill(rect)
                // Follow the shared diagonal fold in the original 96 × 66 mark.
                ctx.translateBy(x: rect.minX, y: rect.minY)
                ctx.scaleBy(x: rect.width / 96, y: rect.height / 66)
                ctx.move(to: .zero)
                ctx.addLine(to: CGPoint(x: 44, y: 0))
                ctx.addLine(to: CGPoint(x: 44, y: 41))
                ctx.addLine(to: CGPoint(x: 37, y: 66))
                ctx.addLine(to: CGPoint(x: 0, y: 66))
                ctx.closePath()
                (self == .microphone || self == .both ? Self.microphoneBlue : .systemGray).setFill()
                ctx.fillPath()
            }
            return true
        }
        image.isTemplate = self == .idle
        return image
    }
}
