import UIKit
import AVKit
import CoreAudioKit

/// Native Bluetooth entry points laid over the core's UI: pair Bluetooth MIDI controllers
/// (CoreAudioKit's own pairing screen) and pick the audio output route (Bluetooth / AirPlay / wired).
final class BluetoothControls: UIView {
    weak var presenter: UIViewController?
    private let midi = UIButton(type: .system)
    private let route = AVRoutePickerView()

    override init(frame: CGRect) {
        super.init(frame: frame)
        var cfg = UIButton.Configuration.tinted()
        cfg.image = UIImage(systemName: "dot.radiowaves.left.and.right")
        cfg.cornerStyle = .capsule
        midi.configuration = cfg
        midi.accessibilityLabel = "Bluetooth MIDI controllers"
        midi.addTarget(self, action: #selector(pairMIDI), for: .touchUpInside)
        route.prioritizesVideoDevices = false
        route.tintColor = .white
        route.activeTintColor = .systemBlue
        let stack = UIStackView(arrangedSubviews: [midi, route])
        stack.spacing = 6
        stack.alignment = .center
        stack.translatesAutoresizingMaskIntoConstraints = false
        addSubview(stack)
        NSLayoutConstraint.activate([
            stack.leadingAnchor.constraint(equalTo: leadingAnchor), stack.trailingAnchor.constraint(equalTo: trailingAnchor),
            stack.topAnchor.constraint(equalTo: topAnchor), stack.bottomAnchor.constraint(equalTo: bottomAnchor),
            midi.widthAnchor.constraint(equalToConstant: 40), midi.heightAnchor.constraint(equalToConstant: 32),
            route.widthAnchor.constraint(equalToConstant: 40), route.heightAnchor.constraint(equalToConstant: 32),
        ])
    }
    required init?(coder: NSCoder) { fatalError() }

    @objc private func pairMIDI() {
        let vc = CABTMIDICentralViewController()
        vc.navigationItem.rightBarButtonItem = UIBarButtonItem(systemItem: .done, primaryAction: UIAction { [weak vc] _ in
            vc?.dismiss(animated: true)
        })
        let nav = UINavigationController(rootViewController: vc)
        nav.modalPresentationStyle = .formSheet
        presenter?.present(nav, animated: true)
    }
}
