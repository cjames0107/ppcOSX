import Foundation

/// A certificate for a VM's screen sharing over TLS (VeNCrypt): a CA of its
/// own and a server certificate signed by it, in the VM's tls/ folder, as
/// QEMU's tls-creds-x509 reads them. Made with macOS's openssl.
enum TLSCertificates {
    static func ensure(in dir: URL) throws {
        let fm = FileManager.default
        // server.ext marks certificates with the extensions GnuTLS and
        // strict clients need (the first ones made had none): older are remade
        let needed = ["ca-cert.pem", "server-cert.pem", "server-key.pem", "server.ext"]
        if needed.allSatisfy({ fm.fileExists(atPath: dir.appendingPathComponent($0).path) }) {
            return
        }
        try fm.createDirectory(at: dir, withIntermediateDirectories: true)
        let host = ProcessInfo.processInfo.hostName
        func openssl(_ args: [String]) throws {
            let p = Process()
            p.executableURL = URL(fileURLWithPath: "/usr/bin/openssl")
            p.arguments = args
            p.currentDirectoryURL = dir
            p.standardOutput = FileHandle.nullDevice
            p.standardError = FileHandle.nullDevice
            try p.run()
            p.waitUntilExit()
            if p.terminationStatus != 0 {
                throw ToolError(message: "Making the TLS certificate failed (openssl \(args[0])).")
            }
        }
        // macOS's openssl (LibreSSL) adds no extensions by itself, and
        // GnuTLS refuses a CA without basic constraints
        try openssl(["req", "-x509", "-newkey", "rsa:2048", "-nodes", "-days", "3650",
                     "-keyout", "ca-key.pem", "-out", "ca-cert.pem",
                     "-subj", "/CN=ppcOSX screen sharing CA",
                     "-addext", "basicConstraints=critical,CA:TRUE",
                     "-addext", "keyUsage=critical,keyCertSign,cRLSign",
                     "-addext", "subjectKeyIdentifier=hash"])
        try openssl(["req", "-newkey", "rsa:2048", "-nodes",
                     "-keyout", "server-key.pem", "-out", "server.csr",
                     "-subj", "/CN=\(host)"])
        var names = ["DNS:localhost", "IP:127.0.0.1"]
        if !host.isEmpty && host != "localhost" { names.insert("DNS:\(host)", at: 0) }
        let ext = """
            basicConstraints=critical,CA:FALSE
            keyUsage=critical,digitalSignature,keyEncipherment
            extendedKeyUsage=serverAuth
            subjectKeyIdentifier=hash
            authorityKeyIdentifier=keyid
            subjectAltName=\(names.joined(separator: ","))

            """
        try ext.write(to: dir.appendingPathComponent("server.ext"), atomically: true,
                      encoding: .utf8)
        try openssl(["x509", "-req", "-days", "3650", "-in", "server.csr",
                     "-CA", "ca-cert.pem", "-CAkey", "ca-key.pem", "-CAcreateserial",
                     "-extfile", "server.ext", "-out", "server-cert.pem"])
        try? fm.removeItem(at: dir.appendingPathComponent("server.csr"))
        // the private keys are for this user only
        for key in ["ca-key.pem", "server-key.pem"] {
            try? fm.setAttributes([.posixPermissions: 0o600],
                                  ofItemAtPath: dir.appendingPathComponent(key).path)
        }
    }
}
