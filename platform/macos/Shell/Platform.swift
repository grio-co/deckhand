// Platform callbacks the core can ask the shell for (the optional members of dh_platform).
import AppKit
import CoreAudioKit

enum Platform {
  private static var bluetoothWindow: CABTLEMIDIWindowController?

  /// The system's Bluetooth MIDI pairing window. Paired devices then appear through CoreMIDI like any other
  /// controller, in this process and in the audio engine helper alike.
  static func showBluetoothMIDI() {
    if bluetoothWindow == nil { bluetoothWindow = CABTLEMIDIWindowController() }
    bluetoothWindow?.showWindow(nil)
    bluetoothWindow?.window?.makeKeyAndOrderFront(nil)
  }

  static let showBluetooth: @convention(c) (UnsafeMutableRawPointer?) -> Void = { _ in
    Platform.showBluetoothMIDI()
  }

  /// Modal folder picker. Called by the core on the UI thread while it builds a frame; the view ignores
  /// the draw callbacks that the modal loop may trigger meanwhile.
  static let chooseFolder: @convention(c) (UnsafeMutableRawPointer?, UnsafePointer<CChar>?,
                                           UnsafeMutablePointer<CChar>?, UInt32) -> Bool = { _, title, out, cap in
    guard let out, cap > 1 else { return false }
    let panel = NSOpenPanel()
    panel.canChooseDirectories = true
    panel.canChooseFiles = false
    panel.allowsMultipleSelection = false
    panel.canCreateDirectories = true
    if let title { panel.message = String(cString: title) }
    guard panel.runModal() == .OK, let url = panel.url else { return false }
    let bytes = Array(url.path.utf8)
    guard bytes.count < Int(cap) else { return false }
    for (i, b) in bytes.enumerated() { out[i] = CChar(bitPattern: b) }
    out[bytes.count] = 0
    return true
  }
}
