import AppKit
import SwiftUI

extension View {
    func panelHelp(_ text: String) -> some View {
        background(HelpRegion(text: text).accessibilityHidden(true))
            .accessibilityHint(Text(text))
    }
}

private struct HelpRegion: NSViewRepresentable {
    let text: String
    func makeNSView(context: Context) -> HelpTarget {
        let target = HelpTarget()
        HelpTarget.all.add(target)
        return target
    }
    func updateNSView(_ view: HelpTarget, context: Context) { view.message = text }
}

private final class HelpTarget: NSView {
    static let all = NSHashTable<HelpTarget>.weakObjects()
    var message = ""
    weak var presenter: PanelHelpPresenter?

    override func viewDidMoveToWindow() {
        super.viewDidMoveToWindow()
        if window == nil { presenter?.dismiss() }
    }

    func contains(_ point: NSPoint, in window: NSWindow) -> Bool {
        guard self.window === window, !isHiddenOrHasHiddenAncestor else { return false }
        let local = convert(point, from: nil)
        return bounds.contains(local) && visibleRect.contains(local)
    }
    override func hitTest(_ point: NSPoint) -> NSView? { nil }
}

/// Disabled native controls can suppress tracking events. Resolve a resting pointer against
/// the rendered label/control bounds, independently of which control receives mouse events.
final class PanelHelpPresenter: NSResponder {
    private weak var host: NSView?
    private weak var target: HelpTarget?
    private var trackingArea: NSTrackingArea?
    private var movementMonitor: Any?
    private var delay: Timer?
    private var hoverPoint = NSPoint.zero
    private var bubble: NSPanel?

    func start(in view: NSView) {
        stop()
        guard let window = view.window else { return }
        host = view
        let area = NSTrackingArea(rect: .zero, options: [.activeAlways, .inVisibleRect, .mouseEnteredAndExited, .mouseMoved],
                                  owner: self, userInfo: nil)
        view.addTrackingArea(area)
        trackingArea = area
        // Native pickers and disabled SwiftUI controls can each be the event recipient.
        // Observe movement once at window level so both use the same hover delay.
        movementMonitor = NSEvent.addLocalMonitorForEvents(matching: [.mouseMoved, .scrollWheel, .leftMouseDragged]) { [weak self, weak window] event in
            guard let window, event.window === window else { return event }
            if event.type == .mouseMoved {
                self?.moved(to: event.locationInWindow)
            } else {
                self?.dismiss()
            }
            return event
        }
    }

    override func mouseExited(with event: NSEvent) { dismiss() }

    func stop() {
        if let movementMonitor { NSEvent.removeMonitor(movementMonitor) }
        movementMonitor = nil
        if let trackingArea { host?.removeTrackingArea(trackingArea) }
        trackingArea = nil
        host = nil
        dismiss()
    }

    func dismiss() {
        delay?.invalidate()
        delay = nil
        if let bubble {
            bubble.parent?.removeChildWindow(bubble)
            bubble.orderOut(nil)
            self.bubble = nil
        }
        target?.presenter = nil
        target = nil
    }

    private func moved(to point: NSPoint) {
        guard let window = host?.window else { return }
        if let target, !target.contains(point, in: window) { dismiss() }
        hoverPoint = point
        let dwell: TimeInterval = 0.45
        if let delay {
            delay.fireDate = Date(timeIntervalSinceNow: dwell)
            return
        }
        // Resolve geometry only once the pointer rests. Nested regions (e.g. p95 inside
        // the latency row) give priority to the most specific label.
        delay = Timer.scheduledTimer(withTimeInterval: dwell, repeats: false) { [weak self, weak window] _ in
            guard let self else { return }
            self.delay = nil
            guard let window, window.isVisible else { return }
            let point = self.hoverPoint
            let matches = HelpTarget.all.allObjects.lazy.filter { $0.contains(point, in: window) }
            guard let region = matches.min(by: { $0.bounds.width * $0.bounds.height < $1.bounds.width * $1.bounds.height }) else { return }
            guard region !== self.target else { return }
            self.dismiss()
            self.present(region, at: point, in: window)
        }
    }

    private func present(_ region: HelpTarget, at point: NSPoint, in window: NSWindow) {
        guard let screen = window.screen else { return }
        target = region
        region.presenter = self
        let label = NSTextField(wrappingLabelWithString: region.message)
        label.font = .systemFont(ofSize: NSFont.smallSystemFontSize)
        label.preferredMaxLayoutWidth = 280
        let size = label.fittingSize
        let frame = NSRect(x: 0, y: 0, width: ceil(size.width) + 16, height: ceil(size.height) + 10)
        let tip = NSPanel(contentRect: frame, styleMask: [.borderless, .nonactivatingPanel], backing: .buffered, defer: false)
        tip.isOpaque = false
        tip.backgroundColor = .clear
        tip.hasShadow = true
        tip.ignoresMouseEvents = true
        tip.appearance = region.effectiveAppearance
        let surface = NSVisualEffectView(frame: frame)
        surface.material = .popover
        surface.state = .active
        surface.wantsLayer = true
        surface.layer?.cornerRadius = 5
        label.frame = surface.bounds.insetBy(dx: 8, dy: 5)
        surface.addSubview(label)
        tip.contentView = surface
        let mouse = window.convertPoint(toScreen: point)
        let available = screen.visibleFrame
        let x = max(available.minX, min(mouse.x + 8, available.maxX - frame.width))
        let y = mouse.y - frame.height - 16
        tip.setFrameOrigin(NSPoint(x: x, y: y >= available.minY ? y : mouse.y + 14))
        window.addChildWindow(tip, ordered: .above)
        tip.orderFront(nil)
        bubble = tip
    }
}
