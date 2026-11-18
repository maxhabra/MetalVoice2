import AppKit
import Combine
import SwiftUI
import AudioCore

// The document lays out at its natural height. Only the viewport is capped on short screens.
private final class ControlsPanelController: NSViewController {
    private let host: NSHostingController<MenuView>
    private let header: NSHostingController<MenuHeader>
    private let scroll = NSScrollView()
    private let headerHeight: CGFloat
    private var heightLimit: CGFloat = 1

    init(root: MenuView) {
        header = NSHostingController(rootView: MenuHeader(audio: root.audio))
        header.sizingOptions = []
        headerHeight = ceil(header.sizeThatFits(in: NSSize(width: MenuView.panelWidth, height: 200)).height)
        host = NSHostingController(rootView: root)
        super.init(nibName: nil, bundle: nil)
        host.sizingOptions = []
        host.rootView.onContentHeightChange = { [weak self] height in self?.fit(height) }
        addChild(header)
        addChild(host)
    }
    required init?(coder: NSCoder) { fatalError("Use init(root:)") }

    override func loadView() {
        scroll.drawsBackground = false
        scroll.hasVerticalScroller = true
        scroll.scrollerStyle = .overlay
        scroll.autohidesScrollers = true
        view = NSView()
        let toolbar = header.view
        toolbar.translatesAutoresizingMaskIntoConstraints = false
        scroll.translatesAutoresizingMaskIntoConstraints = false
        view.addSubview(toolbar)
        view.addSubview(scroll)
        NSLayoutConstraint.activate([
            toolbar.topAnchor.constraint(equalTo: view.topAnchor),
            toolbar.leadingAnchor.constraint(equalTo: view.leadingAnchor),
            toolbar.trailingAnchor.constraint(equalTo: view.trailingAnchor),
            toolbar.heightAnchor.constraint(equalToConstant: headerHeight),
            scroll.topAnchor.constraint(equalTo: toolbar.bottomAnchor),
            scroll.leadingAnchor.constraint(equalTo: view.leadingAnchor),
            scroll.trailingAnchor.constraint(equalTo: view.trailingAnchor),
            scroll.bottomAnchor.constraint(equalTo: view.bottomAnchor)
        ])
        scroll.documentView = host.view
    }

    func prepare(heightLimit: CGFloat) {
        loadViewIfNeeded()
        self.heightLimit = max(1, heightLimit)
        let measured = host.sizeThatFits(in: NSSize(width: MenuView.panelWidth, height: .greatestFiniteMagnitude))
        fit(measured.height)
    }

    private func fit(_ height: CGFloat) {
        guard isViewLoaded, height > 0 else { return }
        let document = NSSize(width: MenuView.panelWidth, height: ceil(height))
        let viewport = NSSize(width: document.width, height: min(headerHeight + document.height, heightLimit))
        if host.view.frame.size != document { host.view.setFrameSize(document) }
        guard preferredContentSize != viewport else { return }
        preferredContentSize = viewport
    }
}

final class AppDelegate: NSObject, NSApplicationDelegate, NSPopoverDelegate {
    private var item: NSStatusItem!
    private var statusMarkUpdates: AnyCancellable?
    private var popover: NSPopover!
    private var audio: AudioController!
    private var panel: ControlsPanelController!
    private var localClick: Any?
    private var outsideClick: Any?
    private var choosingFile = false
    private let hoverHelp = PanelHelpPresenter()

    func applicationDidFinishLaunching(_ notification: Notification) {
        NSApp.setActivationPolicy(.accessory)
        audio = AudioController()
        item = NSStatusBar.system.statusItem(withLength: NSStatusItem.variableLength)
        let mark = NSImage(named: "VoiceMark")?.cgImage(forProposedRect: nil, context: nil, hints: nil)
        statusMarkUpdates = Publishers.CombineLatest4(audio.$running, audio.$cleanupEnabled, audio.$activeVoicePreset, audio.$muted)
            .map { StatusMark(running: $0, microphone: $1, voice: $2 != nil, muted: $3) }
            .removeDuplicates()
            .sink { [weak button = item.button] state in
                button?.image = mark.map(state.image(using:)) ?? NSImage(systemSymbolName: "waveform", accessibilityDescription: nil)
                button?.toolTip = "MetalVoice2 — \(state.description). Click for controls, right-click for quick actions."
                button?.setAccessibilityValue(state.description)
            }
        item.button?.setAccessibilityLabel("MetalVoice2")
        item.button?.target = self
        item.button?.action = #selector(statusItemClicked)
        item.button?.sendAction(on: [.leftMouseUp, .rightMouseUp])
        popover = NSPopover()
        popover.behavior = .applicationDefined
        popover.animates = false
        popover.delegate = self
        panel = ControlsPanelController(root: MenuView(audio: audio, presentPanel: { [weak self] picker in
            self?.choosingFile = true
            self?.hoverHelp.stop()
            defer {
                self?.choosingFile = false
                if let self, self.popover.isShown { self.hoverHelp.start(in: self.panel.view) }
            }
            return picker.runModal()
        }))
        popover.contentViewController = panel
        if audio.cleanupEnabled || audio.hasVoice { audio.start() }
        if CommandLine.arguments.contains("--show") { togglePopover() }
    }

    @objc private func statusItemClicked() {
        if let event = NSApp.currentEvent,
           event.type == .rightMouseUp || event.modifierFlags.contains(.control) {
            guard let button = item.button else { return }
            closePopover()
            NSMenu.popUpContextMenu(makeQuickMenu(), with: event, for: button)
        } else {
            togglePopover()
        }
    }

    private func makeQuickMenu() -> NSMenu {
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
        add(audio.cleanupEnabled ? "Disable microphone" : "Enable microphone", #selector(toggleMicrophone), enabled: !audio.busy)
        add(audio.voiceEnabled ? "Disable voice changer" : "Enable voice changer", #selector(toggleVoice), enabled: !audio.busy)
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
    @objc private func toggleMicrophone() { audio.setCleanupEnabled(!audio.cleanupEnabled) }
    @objc private func toggleVoice() {
        audio.setVoiceEnabled(!audio.voiceEnabled)
        if audio.voiceEnabled && audio.voices.selected == nil {
            UserDefaults.standard.set(true, forKey: "showVoiceChangerTab")
            showControls()
        }
    }
    @objc private func toggleMute() { if audio.running && !audio.busy { audio.toggleMute() } }
    @objc private func quit() { NSApp.terminate(nil) }

    @objc private func togglePopover() {
        guard let button = item.button, let window = button.window else { return }
        if popover.isShown { closePopover() }
        else {
            if !audio.running && !audio.busy { audio.refreshDevices() }
            let anchor = window.convertToScreen(button.convert(button.bounds, to: nil))
            let screen = window.screen ?? NSScreen.main
            audio.panelVisible = true
            panel.prepare(heightLimit: screen.map { anchor.minY - $0.visibleFrame.minY - 24 } ?? 800)
            if popover.contentSize != panel.preferredContentSize { popover.contentSize = panel.preferredContentSize }
            NSApp.activate(ignoringOtherApps: true)
            popover.show(relativeTo: button.bounds, of: button, preferredEdge: button.isFlipped ? .maxY : .minY)
        }
    }

    private func closePopover() {
        // AppKit can decline performClose while the hover tip is attached as a child window.
        hoverHelp.stop()
        popover.close()
    }

    private func hitsStatusButton(_ point: NSPoint) -> Bool {
        guard let button = item.button, let window = button.window else { return false }
        return window.convertToScreen(button.convert(button.bounds, to: nil)).contains(point)
    }

    func popoverDidShow(_ notification: Notification) {
        hoverHelp.start(in: panel.view)
        let clicks: NSEvent.EventTypeMask = [.leftMouseDown, .rightMouseDown, .otherMouseDown]
        // Leave clicks on the status item to its button action, so mouse-up cannot reopen it.
        localClick = NSEvent.addLocalMonitorForEvents(matching: [clicks, .keyDown]) { [weak self] event in
            guard let self, !self.choosingFile else { return event }
            self.hoverHelp.dismiss()
            if event.type == .keyDown {
                if event.keyCode == 53 && event.window === self.panel.view.window {
                    self.closePopover()
                    return nil
                }
            } else if let window = event.window, window !== self.item.button?.window,
                      window !== self.panel.view.window, window.level.rawValue < NSWindow.Level.popUpMenu.rawValue {
                self.closePopover()
            }
            return event
        }
        outsideClick = NSEvent.addGlobalMonitorForEvents(matching: clicks) { [weak self] event in
            guard let self, !self.choosingFile, !self.hitsStatusButton(event.locationInWindow) else { return }
            self.closePopover()
        }
    }

    func popoverDidClose(_ notification: Notification) {
        hoverHelp.stop()
        if let localClick { NSEvent.removeMonitor(localClick) }
        if let outsideClick { NSEvent.removeMonitor(outsideClick) }
        localClick = nil
        outsideClick = nil
        audio.panelVisible = false
    }
    func applicationDidResignActive(_ notification: Notification) {
        if popover.isShown && !choosingFile {
            if NSEvent.pressedMouseButtons != 0 && hitsStatusButton(NSEvent.mouseLocation) { return }
            closePopover()
        }
    }
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
