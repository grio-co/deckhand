import AVFoundation

/// Playback category, small buffer, 4 output channels when offered, interruption + route handling.
/// The core picks master/cue pairs itself and reopens its output on each report.
final class AudioSessionManager {
    var onRouteChange: (() -> Void)?
    private let s = AVAudioSession.sharedInstance()

    func start() {
        do {
            try s.setCategory(.playback, mode: .default, options: [])
            try s.setPreferredIOBufferDuration(0.005)
            try s.setActive(true)
            if s.maximumOutputNumberOfChannels > 2 { try? s.setPreferredOutputNumberOfChannels(min(4, s.maximumOutputNumberOfChannels)) }
        } catch { NSLog("DeckHand audio session: \(error)") }
        let nc = NotificationCenter.default
        nc.addObserver(forName: AVAudioSession.interruptionNotification, object: s, queue: .main) { [weak self] n in
            guard let self, let raw = n.userInfo?[AVAudioSessionInterruptionTypeKey] as? UInt,
                  AVAudioSession.InterruptionType(rawValue: raw) == .ended else { return }
            try? self.s.setActive(true)   // core engine is resumed by the route callback below
            self.report()
        }
        nc.addObserver(forName: AVAudioSession.routeChangeNotification, object: s, queue: .main) { [weak self] _ in
            if let self, let n = Optional(self.s.maximumOutputNumberOfChannels), n > 2 {
                try? self.s.setPreferredOutputNumberOfChannels(min(4, n))
            }
            self?.report()
        }
        report()
    }

    private func report() { onRouteChange?() }
}
