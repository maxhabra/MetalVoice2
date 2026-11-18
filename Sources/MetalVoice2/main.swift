import AppKit
import SwiftUI
import AudioCore

final class AppDelegate: NSObject, NSApplicationDelegate, NSPopoverDelegate {
    private var item: NSStatusItem!
    private var popover: NSPopover!
    private var audio: AudioController!
    private var hosting: NSHostingController<MenuView>?

    func applicationDidFinishLaunching(_ notification: Notification) {
        NSApp.setActivationPolicy(.accessory)
        audio = AudioController()
        item = NSStatusBar.system.statusItem(withLength: NSStatusItem.variableLength)
        if let mark = NSImage(named: "VoiceMark")?.copy() as? NSImage {
            mark.size = NSSize(width: 22, height: 22 * mark.size.height / mark.size.width)
            mark.isTemplate = true
            item.button?.image = mark
        } else {
            item.button?.image = NSImage(systemSymbolName: "waveform", accessibilityDescription: "MetalVoice2")
        }
        item.button?.setAccessibilityLabel("MetalVoice2")
        item.button?.toolTip = "MetalVoice2 — click for controls, right-click for quick actions"
        item.button?.target = self
        item.button?.action = #selector(statusItemClicked)
        item.button?.sendAction(on: [.leftMouseUp, .rightMouseUp])
        popover = NSPopover()
        popover.behavior = .transient
        popover.animates = false
        popover.delegate = self
        if CommandLine.arguments.contains("--show") { togglePopover() }
    }

    @objc private func statusItemClicked() {
        if let event = NSApp.currentEvent,
           event.type == .rightMouseUp || event.modifierFlags.contains(.control) {
            guard let button = item.button else { return }
            popover.performClose(nil)
            NSMenu.popUpContextMenu(makeQuickMenu(), with: event, for: button)
        } else {
            togglePopover()
        }
    }

    private func makeQuickMenu() -> NSMenu {
        if !audio.running && !audio.busy { audio.refreshDevices() }
        let menu = NSMenu(title: "MetalVoice2")
        menu.autoenablesItems = false
        func add(_ title: String, _ action: Selector, enabled: Bool = true) {
            let entry = NSMenuItem(title: title, action: action, keyEquivalent: "")
            entry.target = self
            entry.isEnabled = enabled
            menu.addItem(entry)
        }
        add("Show Controls…", #selector(showControls))
        menu.addItem(.separator())
        add(audio.wantedRunning ? "Stop" : "Start", #selector(toggleAudio), enabled: audio.wantedRunning || audio.canStart)
        add(audio.muted ? "Unmute" : "Mute", #selector(toggleMute), enabled: audio.running && !audio.busy)
        menu.addItem(.separator())
        add("Quit MetalVoice2", #selector(quit))
        return menu
    }

    @objc private func showControls() {
        // Defer opening until the native context menu has finished tracking.
        DispatchQueue.main.async { [weak self] in
            guard let self, !self.popover.isShown else { return }
            self.togglePopover()
        }
    }
    @objc private func toggleAudio() { audio.wantedRunning ? audio.stop() : audio.start() }
    @objc private func toggleMute() { if audio.running && !audio.busy { audio.toggleMute() } }
    @objc private func quit() { NSApp.terminate(nil) }

    @objc private func togglePopover() {
        guard let button = item.button, let window = button.window else { return }
        if popover.isShown { popover.performClose(nil) }
        else {
            // Keep the top anchored below the menu bar. Details only grows the
            // lower section; content measurement also accommodates error text.
            let anchor = window.convertToScreen(button.convert(button.bounds, to: nil))
            let screen = window.screen ?? NSScreen.main
            let availableHeight = screen.map { anchor.minY - $0.visibleFrame.minY - 24 } ?? MenuView.panelHeight
            let size = NSSize(width: MenuView.panelWidth, height: min(MenuView.panelHeight, max(1, availableHeight)))
            let hosting = NSHostingController(rootView: MenuView(audio: audio, viewportHeight: max(1, availableHeight),
                onHeightChange: { [weak self] height in self?.resizePopover(height: height) }))
            self.hosting = hosting
            hosting.sizingOptions = []
            hosting.preferredContentSize = size
            hosting.view.setFrameSize(size)
            popover.contentViewController = hosting
            popover.contentSize = size
            audio.panelVisible = true
            NSApp.activate(ignoringOtherApps: true)
            // The lower edge depends on the positioning view's coordinate system.
            let lowerEdge: NSRectEdge = button.isFlipped ? .maxY : .minY
            popover.show(relativeTo: button.bounds, of: button, preferredEdge: lowerEdge)
        }
    }
    private func resizePopover(height: CGFloat) {
        guard let hosting, abs(popover.contentSize.height - height) > 0.5 else { return }
        let size = NSSize(width: MenuView.panelWidth, height: height)
        // MenuView calls after its layout pass. Let AppKit preserve the popover
        // anchor while resizing; manually setting its window frame fights the
        // content-size update and can undo it or accumulate extra height.
        hosting.preferredContentSize = size
        popover.contentSize = size
    }
    func popoverDidClose(_ notification: Notification) { audio.panelVisible = false }
    func applicationWillTerminate(_ notification: Notification) { audio.shutdown() }
}

if CommandLine.arguments.contains("--list-devices") {
    let devices = AudioDevice.list().map { device -> [String: Any] in
        ["id": device.id, "uid": device.uid, "name": device.name,
         "inputs": device.inputs, "outputs": device.outputs,
         "sampleRate": device.sampleRate, "bufferFrames": device.bufferFrames,
         "virtual": device.isVirtual, "defaultInput": device.isDefaultInput]
    }
    let data = try JSONSerialization.data(withJSONObject: devices, options: [.prettyPrinted, .sortedKeys])
    print(String(decoding: data, as: UTF8.self))
} else if CommandLine.arguments.contains("--benchmark") {
    exit(runBenchmark(arguments: CommandLine.arguments))
} else {
    let app = NSApplication.shared
    let delegate = AppDelegate()
    app.delegate = delegate
    app.run()
}
