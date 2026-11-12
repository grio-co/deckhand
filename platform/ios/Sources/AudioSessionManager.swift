import AVFoundation

/// Playback category, small buffer, interruption + route handling. Reports output channel count and the
/// pairs for master/cue: 4+ channels -> master 0 (outs 1-2), cue 1 (outs 3-4); otherwise stereo, cue -1.
final class AudioSessionManager {
    var onRouteChange: ((Int, Int, Int) -> Void)?
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

    private func report() {
        let ch = max(2, s.outputNumberOfChannels)
        onRouteChange?(ch, 0, ch >= 4 ? 1 : -1)
    }
}
