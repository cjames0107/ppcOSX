import Foundation
import IOKit
import IOKit.usb

/// USB devices attached to this Mac, which can be handed to a guest
/// (QEMU's usb-host, through libusb). Hubs are left out.
enum HostUSB {
    static func devices() -> [USBPassthrough] {
        var iterator: io_iterator_t = 0
        guard IOServiceGetMatchingServices(kIOMainPortDefault,
                                           IOServiceMatching("IOUSBHostDevice"),
                                           &iterator) == KERN_SUCCESS else { return [] }
        defer { IOObjectRelease(iterator) }
        var found: [USBPassthrough] = []
        while case let service = IOIteratorNext(iterator), service != 0 {
            defer { IOObjectRelease(service) }
            func prop(_ key: String) -> Any? {
                IORegistryEntryCreateCFProperty(service, key as CFString,
                                                kCFAllocatorDefault, 0)?.takeRetainedValue()
            }
            guard let vendor = prop("idVendor") as? Int,
                  let product = prop("idProduct") as? Int else { continue }
            if (prop("bDeviceClass") as? Int) == 9 { continue }          // hub
            let name = (prop("USB Product Name") as? String)
                ?? (prop("kUSBProductString") as? String)
                ?? String(format: "USB device %04x:%04x", vendor, product)
            let maker = (prop("USB Vendor Name") as? String) ?? ""
            let full = maker.isEmpty || name.hasPrefix(maker) ? name : "\(maker) \(name)"
            let d = USBPassthrough(vendor: vendor, product: product, name: full)
            if !found.contains(d) { found.append(d) }
        }
        return found.sorted { $0.name.localizedStandardCompare($1.name) == .orderedAscending }
    }
}
