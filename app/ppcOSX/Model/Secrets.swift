import Foundation
import Security

/// VNC passwords, kept in the login keychain (not in config.json), one per
/// VM by its id.
enum Secrets {
    private static let service = "org.ppcosx.manager.vnc"

    static func vncPassword(for id: UUID) -> String? {
        let query: [String: Any] = [
            kSecClass as String: kSecClassGenericPassword,
            kSecAttrService as String: service,
            kSecAttrAccount as String: id.uuidString,
            kSecReturnData as String: true,
            kSecMatchLimit as String: kSecMatchLimitOne,
        ]
        var item: CFTypeRef?
        guard SecItemCopyMatching(query as CFDictionary, &item) == errSecSuccess,
              let data = item as? Data else { return nil }
        return String(data: data, encoding: .utf8)
    }

    static func setVNCPassword(_ password: String?, for id: UUID) {
        let match: [String: Any] = [
            kSecClass as String: kSecClassGenericPassword,
            kSecAttrService as String: service,
            kSecAttrAccount as String: id.uuidString,
        ]
        SecItemDelete(match as CFDictionary)
        guard let password, !password.isEmpty else { return }
        var add = match
        add[kSecValueData as String] = Data(password.utf8)
        add[kSecAttrLabel as String] = "ppcOSX screen sharing"
        SecItemAdd(add as CFDictionary, nil)
    }
}
