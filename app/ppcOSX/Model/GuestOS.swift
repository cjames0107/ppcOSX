import SwiftUI

/// The operating system a VM runs: what hardware suits it, and what the
/// emulator adjusts for it (`ppcosx --guest`).
enum GuestOS: String, Codable, CaseIterable, Identifiable {
    case macOS9 = "9.2"
    case cheetah = "10.0"
    case puma = "10.1"
    case jaguar = "10.2"
    case panther = "10.3"
    case tiger = "10.4"
    case leopard = "10.5"
    case other

    var id: String { rawValue }

    var rank: Int { GuestOS.allCases.firstIndex(of: self) ?? 0 }

    var title: String {
        switch self {
        case .macOS9: return "Mac OS 9.2"
        case .cheetah: return "Mac OS X 10.0 Cheetah"
        case .puma: return "Mac OS X 10.1 Puma"
        case .jaguar: return "Mac OS X 10.2 Jaguar"
        case .panther: return "Mac OS X 10.3 Panther"
        case .tiger: return "Mac OS X 10.4 Tiger"
        case .leopard: return "Mac OS X 10.5 Leopard"
        case .other: return "Other"
        }
    }

    var shortTitle: String {
        switch self {
        case .macOS9: return "Mac OS 9"
        case .other: return "Other"
        default: return "Mac OS X \(rawValue)"
        }
    }

    var symbol: String {
        switch self {
        case .macOS9: return "macwindow"
        case .other: return "questionmark.square.dashed"
        default: return "desktopcomputer"
        }
    }

    var tint: Color {
        switch self {
        case .macOS9: return .purple
        case .cheetah: return .orange
        case .puma: return .teal
        case .jaguar: return .yellow
        case .panther: return .gray
        case .tiger: return .blue
        case .leopard: return .indigo
        case .other: return .secondary
        }
    }

    /// processors that real Macs running this system had
    var cpus: [CPUType] {
        switch self {
        case .macOS9, .cheetah, .puma: return [.g3, .g4]
        case .jaguar: return [.g3, .g4, .g5]
        case .panther, .tiger, .other: return CPUType.allCases
        case .leopard: return [.g4, .g4_7447a, .g5]
        }
    }

    var recommendedGraphics: GraphicsCard {
        switch self {
        case .macOS9, .cheetah: return .rage128
        case .puma, .jaguar: return .radeon7000
        default: return .radeon9600
        }
    }

    /// graphics this system has drivers for, best first
    var graphics: [GraphicsCard] {
        switch self {
        case .macOS9, .cheetah: return [.rage128, .vga]
        case .puma, .jaguar: return [.radeon7000, .rage128, .vga]
        default: return [.radeon9600, .radeon7000, .vga]
        }
    }

    var recommendedMemory: Int {
        switch self {
        case .macOS9, .cheetah, .puma: return 512
        case .jaguar: return 768
        case .panther, .tiger, .other: return 1024
        case .leopard: return 1536
        }
    }

    /// MiB; 10.0's kernel stops above 512 MB here
    var maxMemory: Int {
        switch self {
        case .cheetah: return 512
        case .macOS9: return 1536
        default: return 2048
        }
    }

    var minMemory: Int { self == .leopard ? 512 : 128 }

    /// GB
    var recommendedDisk: Int {
        switch self {
        case .macOS9: return 8
        case .cheetah, .puma: return 10
        case .jaguar: return 16
        case .panther, .other: return 20
        case .tiger, .leopard: return 40
        }
    }

    var note: String? {
        switch self {
        case .macOS9:
            return "Experimental: ATI Video Accelerator stops Mac OS 9 at "
                + "startup; move it out of the Extensions folder after "
                + "installing."
        case .cheetah:
            return "Mac OS X 10.0 needs 512 MB of memory or less."
        case .leopard:
            return "Leopard needs a G4 or G5."
        default:
            return nil
        }
    }
}
