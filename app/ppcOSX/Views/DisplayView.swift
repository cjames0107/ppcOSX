import AppKit
import QuartzCore

/// The guest's screen in a VM window, and its keyboard and mouse.
///
/// - The screen keeps its aspect ratio, centred on black; it is drawn
///   pixel for pixel when the scale is a whole number, smoothed otherwise.
/// - With an absolute pointing device (the USB tablet) the guest's pointer
///   follows the Mac's while over the screen; the guest's own pointer image
///   (hardware cursor) is drawn in place of the Mac's.
/// - Guests with only a relative mouse (Mac OS X 10.0 - 10.2, Mac OS 9)
///   capture the pointer on a click. Capturing also sends every key to the
///   guest, Command shortcuts included; Control-Option gives them back.
/// - Otherwise keys go to the guest except Command shortcuts that ppcOSX's
///   menus use.
final class DisplayView: NSView {
    let client: DisplayClient
    /// told when capture starts or ends
    var onCaptureChange: ((Bool) -> Void)?

    private let screenLayer = CALayer()
    private let cursorLayer = CALayer()
    private(set) var captured = false
    private var monitor: Any?
    private var keysDown = Set<UInt16>()
    private var modifiers: NSEvent.ModifierFlags = []
    private var rawModifiers: UInt = 0
    private var scrollX: CGFloat = 0
    private var scrollY: CGFloat = 0
    private var tracking: NSTrackingArea?

    private static let blankCursor = NSCursor(image: NSImage(size: NSSize(width: 1, height: 1)),
                                              hotSpot: .zero)

    init(client: DisplayClient) {
        self.client = client
        super.init(frame: NSRect(x: 0, y: 0, width: 1024, height: 768))
        wantsLayer = true
        layer?.backgroundColor = NSColor.black.cgColor
        screenLayer.contentsGravity = .resize
        screenLayer.actions = ["contents": NSNull(), "bounds": NSNull(), "position": NSNull()]
        cursorLayer.anchorPoint = .zero
        cursorLayer.actions = ["contents": NSNull(), "bounds": NSNull(), "position": NSNull(),
                               "hidden": NSNull()]
        cursorLayer.isHidden = true
        cursorLayer.magnificationFilter = .nearest
        layer?.addSublayer(screenLayer)
        screenLayer.addSublayer(cursorLayer)
        client.view = self
        showFrame()
        cursorChanged()
    }

    required init?(coder: NSCoder) { fatalError("not used") }

    override var isFlipped: Bool { true }
    override var acceptsFirstResponder: Bool { true }
    override func acceptsFirstMouse(for event: NSEvent?) -> Bool { true }

    // MARK: drawing

    /// where the screen is drawn, in view points
    private var screenRect: CGRect {
        let size = client.size
        guard size.width > 0, size.height > 0, bounds.width > 0, bounds.height > 0 else {
            return bounds
        }
        let scale = min(bounds.width / size.width, bounds.height / size.height)
        let w = (size.width * scale).rounded(), h = (size.height * scale).rounded()
        return CGRect(x: ((bounds.width - w) / 2).rounded(), y: ((bounds.height - h) / 2).rounded(),
                      width: w, height: h)
    }

    /// view points per guest pixel
    private var scale: CGFloat {
        client.size.width > 0 ? screenRect.width / client.size.width : 1
    }

    override func layout() {
        super.layout()
        CATransaction.begin()
        CATransaction.setDisableActions(true)
        screenLayer.frame = screenRect
        let backing = scale * (window?.backingScaleFactor ?? 2)
        let whole = abs(backing - backing.rounded()) < 0.01
        screenLayer.magnificationFilter = whole ? .nearest : .linear
        screenLayer.minificationFilter = .linear
        placeCursor()
        CATransaction.commit()
        updateTracking()
        window?.invalidateCursorRects(for: self)
    }

    func showFrame() {
        CATransaction.begin()
        CATransaction.setDisableActions(true)
        screenLayer.contents = client.image
        CATransaction.commit()
    }

    func sizeChanged() {
        needsLayout = true
        (window?.windowController as? VMWindowController)?.guestSizeChanged(client.size)
    }

    func cursorChanged() {
        CATransaction.begin()
        CATransaction.setDisableActions(true)
        cursorLayer.contents = client.cursor
        placeCursor()
        CATransaction.commit()
        window?.invalidateCursorRects(for: self)
    }

    func pointerMoved() {
        CATransaction.begin()
        CATransaction.setDisableActions(true)
        placeCursor()
        CATransaction.commit()
    }

    func mouseModeChanged() {
        if client.absolute && captured && !keyboardOnlyCapture {
            release()
        }
        window?.invalidateCursorRects(for: self)
    }

    private func placeCursor() {
        guard let cursor = client.cursor else {
            cursorLayer.isHidden = true
            return
        }
        let s = scale
        cursorLayer.frame = CGRect(x: client.pointer.x * s, y: client.pointer.y * s,
                                   width: CGFloat(cursor.width) * s,
                                   height: CGFloat(cursor.height) * s)
        cursorLayer.isHidden = !client.pointerVisible
    }

    /// the Mac's pointer is hidden over the screen when the guest draws its own
    override func resetCursorRects() {
        if client.absolute && client.cursor != nil {
            addCursorRect(screenRect, cursor: Self.blankCursor)
        }
    }

    // MARK: window

    override func viewDidMoveToWindow() {
        super.viewDidMoveToWindow()
        if let monitor { NSEvent.removeMonitor(monitor) }
        monitor = nil
        guard window != nil else {
            release()
            return
        }
        monitor = NSEvent.addLocalMonitorForEvents(matching: [.keyDown, .keyUp, .flagsChanged]) {
            [weak self] event in
            self?.handleKey(event) ?? event
        }
        NotificationCenter.default.addObserver(self, selector: #selector(windowResigned),
                                               name: NSWindow.didResignKeyNotification,
                                               object: window)
    }

    @objc private func windowResigned() {
        release()
        releaseAllKeys()
    }

    private func updateTracking() {
        if let tracking { removeTrackingArea(tracking) }
        let t = NSTrackingArea(rect: bounds, options: [.mouseMoved, .activeInKeyWindow,
                                                       .inVisibleRect, .mouseEnteredAndExited],
                               owner: self, userInfo: nil)
        addTrackingArea(t)
        tracking = t
    }

    // MARK: capture

    /// captured only for the keyboard (absolute pointer)
    private var keyboardOnlyCapture = false

    func capture() {
        guard !captured else { return }
        window?.makeFirstResponder(self)
        captured = true
        keyboardOnlyCapture = client.absolute
        if !keyboardOnlyCapture {
            CGAssociateMouseAndMouseCursorPosition(0)
            NSCursor.hide()
        }
        onCaptureChange?(true)
    }

    func release() {
        guard captured else { return }
        captured = false
        if !keyboardOnlyCapture {
            CGAssociateMouseAndMouseCursorPosition(1)
            NSCursor.unhide()
        }
        keyboardOnlyCapture = false
        onCaptureChange?(false)
    }

    // MARK: mouse

    private func guestPoint(_ event: NSEvent) -> (Int, Int)? {
        let p = convert(event.locationInWindow, from: nil)
        let r = screenRect
        guard r.contains(p), client.size.width > 0 else { return nil }
        let s = r.width / client.size.width
        return (Int((p.x - r.minX) / s), Int((p.y - r.minY) / s))
    }

    private func pointer(_ event: NSEvent) {
        if client.absolute {
            if let (x, y) = guestPoint(event) { client.moveTo(x: x, y: y) }
        } else if captured {
            let dx = Int(event.deltaX.rounded()), dy = Int(event.deltaY.rounded())
            if dx != 0 || dy != 0 { client.move(dx: dx, dy: dy) }
        }
    }

    private func press(_ b: DisplayClient.Button, _ event: NSEvent, down: Bool) {
        if !client.absolute && !captured {
            if down && b == .left { capture() }
            return
        }
        if down { window?.makeFirstResponder(self) }
        pointer(event)
        client.button(b, down: down)
    }

    override func mouseMoved(with event: NSEvent) { pointer(event) }
    override func mouseDragged(with event: NSEvent) { pointer(event) }
    override func rightMouseDragged(with event: NSEvent) { pointer(event) }
    override func otherMouseDragged(with event: NSEvent) { pointer(event) }

    override func mouseDown(with event: NSEvent) {
        // Control-click is the right button, as on a one-button Mac mouse
        press(.left, event, down: true)
    }
    override func mouseUp(with event: NSEvent) { press(.left, event, down: false) }
    override func rightMouseDown(with event: NSEvent) { press(.right, event, down: true) }
    override func rightMouseUp(with event: NSEvent) { press(.right, event, down: false) }
    override func otherMouseDown(with event: NSEvent) { press(.middle, event, down: true) }
    override func otherMouseUp(with event: NSEvent) { press(.middle, event, down: false) }

    override func scrollWheel(with event: NSEvent) {
        guard client.absolute || captured else { return }
        // trackpads send fine-grained deltas: one wheel step per 12 points
        let step: CGFloat = event.hasPreciseScrollingDeltas ? 12 : 1
        scrollY += event.scrollingDeltaY
        scrollX += event.scrollingDeltaX
        func emit(_ amount: inout CGFloat, _ plus: DisplayClient.Button, _ minus: DisplayClient.Button) {
            while abs(amount) >= step {
                let b = amount > 0 ? plus : minus
                client.button(b, down: true)
                client.button(b, down: false)
                amount -= amount > 0 ? step : -step
            }
        }
        emit(&scrollY, .wheelUp, .wheelDown)
        emit(&scrollX, .wheelLeft, .wheelRight)
    }

    // MARK: keyboard

    /// Key events for this window, before AppKit dispatches them: nil when
    /// they went to the guest.
    private func handleKey(_ event: NSEvent) -> NSEvent? {
        guard let window, event.window === window, window.isKeyWindow,
              window.firstResponder === self || captured else { return event }
        switch event.type {
        case .keyDown:
            if !captured && event.modifierFlags.contains(.command) {
                // ppcOSX's own shortcuts first (Quit, Close, Full Screen...)
                if NSApp.mainMenu?.performKeyEquivalent(with: event) == true { return nil }
            }
            if event.isARepeat { return nil }      // the guest repeats keys itself
            keysDown.insert(event.keyCode)
            client.key(event.keyCode, down: true)
            return nil
        case .keyUp:
            if keysDown.remove(event.keyCode) != nil {
                client.key(event.keyCode, down: false)
            }
            return nil
        case .flagsChanged:
            modifiersChanged(event)
            return nil
        default:
            return event
        }
    }

    /// modifier keys: macOS key codes and their device bits (left/right)
    private static let modifierKeys: [(code: UInt16, bit: UInt)] = [
        (59, 0x0001), (62, 0x2000),     // control
        (56, 0x0002), (60, 0x0004),     // shift
        (55, 0x0008), (54, 0x0010),     // command
        (58, 0x0020), (61, 0x0040),     // option
    ]

    private func modifiersChanged(_ event: NSEvent) {
        let raw = event.modifierFlags.rawValue
        if event.keyCode == 57 {
            // Caps Lock: macOS reports its state; the guest wants a press
            client.key(57, down: true)
            client.key(57, down: false)
        }
        for m in Self.modifierKeys where m.code == event.keyCode {
            let down = raw & m.bit != 0
            if down != (rawModifiers & m.bit != 0) {
                if down { keysDown.insert(m.code) } else { keysDown.remove(m.code) }
                client.key(m.code, down: down)
            }
        }
        rawModifiers = raw
        // Control-Option alone gives the pointer and keyboard back
        let flags = event.modifierFlags.intersection(.deviceIndependentFlagsMask)
        if captured && flags == [.control, .option] {
            releaseAllKeys()
            release()
        }
    }

    private func releaseAllKeys() {
        for code in keysDown { client.key(code, down: false) }
        keysDown.removeAll()
        rawModifiers = 0
    }

    deinit {
        if let monitor { NSEvent.removeMonitor(monitor) }
        NotificationCenter.default.removeObserver(self)
    }
}
