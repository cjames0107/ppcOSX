import Foundation

/// A QEMU Machine Protocol connection over a Unix socket: one command per
/// connection, run off the main thread.
enum QMP {
    @discardableResult
    static func execute(socket path: String, _ command: String,
                        arguments: [String: Any]? = nil,
                        timeout: TimeInterval = 3) async throws -> Any? {
        try await Task.detached {
            try executeSync(socket: path, command, arguments: arguments,
                            timeout: timeout)
        }.value
    }

    static func executeSync(socket path: String, _ command: String,
                            arguments: [String: Any]?,
                            timeout: TimeInterval) throws -> Any? {
        let fd = Darwin.socket(AF_UNIX, SOCK_STREAM, 0)
        guard fd >= 0 else { throw ToolError(message: "socket() failed") }
        defer { close(fd) }

        var tv = timeval(tv_sec: Int(timeout), tv_usec: 0)
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, socklen_t(MemoryLayout<timeval>.size))
        setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, socklen_t(MemoryLayout<timeval>.size))

        var addr = sockaddr_un()
        addr.sun_family = sa_family_t(AF_UNIX)
        let bytes = Array(path.utf8)
        guard bytes.count < MemoryLayout.size(ofValue: addr.sun_path) else {
            throw ToolError(message: "QMP socket path too long")
        }
        withUnsafeMutableBytes(of: &addr.sun_path) { buf in
            for (i, b) in bytes.enumerated() { buf[i] = b }
        }
        let ok = withUnsafePointer(to: &addr) {
            $0.withMemoryRebound(to: sockaddr.self, capacity: 1) {
                connect(fd, $0, socklen_t(MemoryLayout<sockaddr_un>.size))
            }
        }
        guard ok == 0 else { throw ToolError(message: "The virtual machine is not answering.") }

        var reader = LineReader(fd: fd)
        _ = try reader.readLine()                          // greeting
        try send(fd, ["execute": "qmp_capabilities"])
        _ = try reader.reply()
        var msg: [String: Any] = ["execute": command]
        if let arguments { msg["arguments"] = arguments }
        try send(fd, msg)
        return try reader.reply()
    }

    private static func send(_ fd: Int32, _ obj: [String: Any]) throws {
        var data = try JSONSerialization.data(withJSONObject: obj)
        data.append(0x0a)
        let n = data.withUnsafeBytes { write(fd, $0.baseAddress, $0.count) }
        guard n == data.count else { throw ToolError(message: "QMP write failed") }
    }

    private struct LineReader {
        let fd: Int32
        var buffer = Data()

        mutating func readLine() throws -> Data {
            while true {
                if let nl = buffer.firstIndex(of: 0x0a) {
                    let line = buffer[buffer.startIndex..<nl]
                    buffer.removeSubrange(buffer.startIndex...nl)
                    return Data(line)
                }
                var chunk = [UInt8](repeating: 0, count: 4096)
                let n = read(fd, &chunk, chunk.count)
                guard n > 0 else { throw ToolError(message: "QMP connection closed") }
                buffer.append(contentsOf: chunk[0..<n])
            }
        }

        /// the next reply, skipping asynchronous events
        mutating func reply() throws -> Any? {
            while true {
                let obj = try JSONSerialization.jsonObject(with: readLine())
                guard let dict = obj as? [String: Any] else { continue }
                if let r = dict["return"] { return r }
                if let e = dict["error"] as? [String: Any] {
                    throw ToolError(message: e["desc"] as? String ?? "QMP error")
                }
            }
        }
    }
}
