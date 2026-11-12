import CoreMIDI

/// Connects every MIDI source (USB class-compliant controllers such as the DDJ-FLX4) and forwards raw bytes to the core.
/// Bluetooth MIDI would additionally need CABTMIDICentralViewController (CoreAudioKit) for pairing UI.
final class MIDIManager {
    var onMessage: (([UInt8]) -> Void)?
    private var client = MIDIClientRef()
    private var port = MIDIPortRef()

    func start() {
        MIDIClientCreateWithBlock("DeckHand" as CFString, &client) { [weak self] note in
            if note.pointee.messageID == .msgObjectAdded || note.pointee.messageID == .msgSetupChanged { self?.connectAll() }
        }
        MIDIInputPortCreateWithProtocol(client, "In" as CFString, ._1_0, &port) { [weak self] list, _ in
            var packet = list.pointee.packet
            for _ in 0..<list.pointee.numPackets {
                let words = withUnsafeBytes(of: packet.words) { Array($0.bindMemory(to: UInt32.self).prefix(Int(packet.wordCount))) }
                // UMP MIDI 1.0 channel voice: type 2 -> status, d1, d2 in low 24 bits.
                for w in words where (w >> 28) == 2 {
                    self?.onMessage?([UInt8((w >> 16) & 0xFF), UInt8((w >> 8) & 0xFF), UInt8(w & 0xFF)])
                }
                packet = MIDIEventPacketNext(&packet).pointee
            }
        }
        connectAll()
    }

    private func connectAll() {
        for i in 0..<MIDIGetNumberOfSources() { MIDIPortConnectSource(port, MIDIGetSource(i), nil) }
    }
}
