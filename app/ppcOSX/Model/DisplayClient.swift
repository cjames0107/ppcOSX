import AppKit
import Foundation

/// A running VM's screen, from QEMU's ppcosx display (qemu/ui/ppcosx.c)
/// over a Unix socket: frames, the guest's pointer image and position,
/// whether its pointing device is absolute; keyboard and mouse go back.
///
/// The socket is read on a thread of its own, which keeps the framebuffer;
/// finished frames, as CGImages, go to the main thread. QEMU sends no new
/// frame until the last one is acknowledged (frameShown).
final class DisplayClient: ObservableObject, @unchecked Sendable {
    /// on the main thread
    @MainActor @Published private(set) var size = CGSize.zero
    @MainActor @Published private(set) var absolute = true
    @MainActor @Published private(set) var connected = false
    @MainActor private(set) var image: CGImage?
    @MainActor private(set) var cursor: CGImage?
    @MainActor private(set) var pointer = CGPoint.zero
    @MainActor private(set) var pointerVisible = false

    /// the view showing the screen, told of each change
    @MainActor weak var view: DisplayView?

    let path: String
    private var fd: Int32 = -1
    private let lock = NSLock()
    private var stopped = false
    private var thread: Thread?

    // the reading thread's
    private var width = 0, height = 0
    private var framebuffer: UnsafeMutableRawPointer?

    init(path: String) {
        self.path = path
    }

    deinit {
        framebuffer?.deallocate()
    }

    func start() {
        let t = Thread { [weak self] in self?.run() }
        t.name = "ppcosx display"
        t.qualityOfService = .userInteractive
        thread = t
        t.start()
    }

    func stop() {
        lock.lock()
        stopped = true
        let f = fd
        lock.unlock()
        if f >= 0 { shutdown(f, SHUT_RDWR) }
    }

    // MARK: input

    enum Message: UInt32 {
        case key = 1, mouseAbs, mouseRel, button, frameDone
    }

    enum Button: UInt32 {
        case left = 0, middle, right, wheelUp, wheelDown, wheelLeft, wheelRight
    }

    func key(_ code: UInt16, down: Bool) {
        send(.key, [UInt32(code), down ? 1 : 0])
    }

    func moveTo(x: Int, y: Int) {
        send(.mouseAbs, [UInt32(max(0, x)), UInt32(max(0, y))])
    }

    func move(dx: Int, dy: Int) {
        send(.mouseRel, [UInt32(bitPattern: Int32(dx)), UInt32(bitPattern: Int32(dy))])
    }

    func button(_ b: Button, down: Bool) {
        send(.button, [b.rawValue, down ? 1 : 0])
    }

    func frameShown() {
        send(.frameDone, [])
    }

    private func send(_ type: Message, _ values: [UInt32]) {
        var msg = [type.rawValue.littleEndian, UInt32(values.count * 4).littleEndian]
        msg += values.map(\.littleEndian)
        lock.lock()
        defer { lock.unlock() }
        guard fd >= 0 else { return }
        msg.withUnsafeBytes { buf in
            var off = 0
            while off < buf.count {
                let n = write(fd, buf.baseAddress! + off, buf.count - off)
                if n <= 0 { break }
                off += n
            }
        }
    }

    // MARK: reading

    private func run() {
        while true {
            lock.lock()
            let done = stopped
            lock.unlock()
            if done { return }
            if let f = connect() {
                lock.lock()
                fd = f
                lock.unlock()
                DispatchQueue.main.async { self.connected = true }
                readLoop(f)
                lock.lock()
                fd = -1
                lock.unlock()
                close(f)
                DispatchQueue.main.async { self.connected = false }
            }
            // QEMU not listening yet (starting), or gone
            Thread.sleep(forTimeInterval: 0.2)
        }
    }

    private func connect() -> Int32? {
        let f = socket(AF_UNIX, SOCK_STREAM, 0)
        guard f >= 0 else { return nil }
        var on: Int32 = 1
        setsockopt(f, SOL_SOCKET, SO_NOSIGPIPE, &on, socklen_t(MemoryLayout<Int32>.size))
        var addr = sockaddr_un()
        addr.sun_family = sa_family_t(AF_UNIX)
        let bytes = Array(path.utf8)
        guard bytes.count < MemoryLayout.size(ofValue: addr.sun_path) else {
            close(f)
            return nil
        }
        withUnsafeMutableBytes(of: &addr.sun_path) { buf in
            for (i, b) in bytes.enumerated() { buf[i] = b }
        }
        let ok = withUnsafePointer(to: &addr) {
            $0.withMemoryRebound(to: sockaddr.self, capacity: 1) {
                Darwin.connect(f, $0, socklen_t(MemoryLayout<sockaddr_un>.size))
            }
        }
        if ok != 0 {
            close(f)
            return nil
        }
        return f
    }

    private func readFully(_ f: Int32, _ p: UnsafeMutableRawPointer, _ n: Int) -> Bool {
        var off = 0
        while off < n {
            let r = read(f, p + off, n - off)
            if r <= 0 { return false }
            off += r
        }
        return true
    }

    private func readLoop(_ f: Int32) {
        var header = [UInt32](repeating: 0, count: 2)
        var payload = UnsafeMutableRawPointer.allocate(byteCount: 1 << 16, alignment: 16)
        var capacity = 1 << 16
        defer { payload.deallocate() }

        while true {
            let ok = header.withUnsafeMutableBytes { readFully(f, $0.baseAddress!, 8) }
            if !ok { return }
            let type = UInt32(littleEndian: header[0])
            let len = Int(UInt32(littleEndian: header[1]))
            if len > capacity {
                payload.deallocate()
                capacity = len
                payload = UnsafeMutableRawPointer.allocate(byteCount: capacity, alignment: 16)
            }
            if len > 0 && !readFully(f, payload, len) { return }
            func u32(_ i: Int) -> UInt32 {
                UInt32(littleEndian: payload.load(fromByteOffset: i * 4, as: UInt32.self))
            }

            switch type {
            case 2:     // SURFACE
                width = Int(u32(0))
                height = Int(u32(1))
                framebuffer?.deallocate()
                framebuffer = UnsafeMutableRawPointer.allocate(byteCount: max(1, width * height * 4),
                                                               alignment: 16)
                memset(framebuffer!, 0, width * height * 4)
                let s = CGSize(width: width, height: height)
                DispatchQueue.main.async {
                    self.size = s
                    self.view?.sizeChanged()
                }
            case 3:     // UPDATE
                let x = Int(u32(0)), y = Int(u32(1)), w = Int(u32(2)), h = Int(u32(3))
                guard let fb = framebuffer, x + w <= width, y + h <= height else { continue }
                for row in 0..<h {
                    memcpy(fb + ((y + row) * width + x) * 4, payload + 16 + row * w * 4, w * 4)
                }
            case 4:     // FRAME
                guard let image = makeImage() else {
                    frameShown()
                    continue
                }
                DispatchQueue.main.async {
                    self.image = image
                    self.view?.showFrame()
                    self.frameShown()
                }
            case 5:     // CURSOR
                let w = Int(u32(0)), h = Int(u32(1))
                let hot = CGPoint(x: Int(u32(2)), y: Int(u32(3)))
                _ = hot     // the guest places the image's corner itself
                let data = Data(bytes: payload + 16, count: w * h * 4)
                let image = CGImage(width: w, height: h, bitsPerComponent: 8, bitsPerPixel: 32,
                                    bytesPerRow: w * 4,
                                    space: CGColorSpace(name: CGColorSpace.sRGB)!,
                                    bitmapInfo: CGBitmapInfo(rawValue:
                                        CGBitmapInfo.byteOrder32Little.rawValue |
                                        CGImageAlphaInfo.first.rawValue),
                                    provider: CGDataProvider(data: data as CFData)!,
                                    decode: nil, shouldInterpolate: false, intent: .defaultIntent)
                DispatchQueue.main.async {
                    self.cursor = image
                    self.view?.cursorChanged()
                }
            case 6:     // POINTER
                let p = CGPoint(x: Int(Int32(bitPattern: u32(0))), y: Int(Int32(bitPattern: u32(1))))
                let on = u32(2) != 0
                DispatchQueue.main.async {
                    self.pointer = p
                    self.pointerVisible = on
                    self.view?.pointerMoved()
                }
            case 7:     // MOUSE
                let abs = u32(0) != 0
                DispatchQueue.main.async {
                    self.absolute = abs
                    self.view?.mouseModeChanged()
                }
            default:
                break
            }
        }
    }

    /// a copy of the framebuffer (the thread goes on writing into it)
    private func makeImage() -> CGImage? {
        guard let fb = framebuffer, width > 0, height > 0 else { return nil }
        let data = Data(bytes: fb, count: width * height * 4)
        guard let provider = CGDataProvider(data: data as CFData) else { return nil }
        return CGImage(width: width, height: height, bitsPerComponent: 8, bitsPerPixel: 32,
                       bytesPerRow: width * 4,
                       space: CGColorSpace(name: CGColorSpace.sRGB)!,
                       bitmapInfo: CGBitmapInfo(rawValue:
                           CGBitmapInfo.byteOrder32Little.rawValue |
                           CGImageAlphaInfo.noneSkipFirst.rawValue),
                       provider: provider, decode: nil, shouldInterpolate: false,
                       intent: .defaultIntent)
    }
}
