import UIKit
import UniformTypeIdentifiers

/// Documents folder (exposed in Files via Info.plist) + document picker with security-scoped bookmarks.
/// DRM-protected streaming tracks cannot be read: they are not exposed as files by their apps.
final class FileAccess: NSObject, UIDocumentPickerDelegate {
    static var documentsURL: URL { FileManager.default.urls(for: .documentDirectory, in: .userDomainMask)[0] }
    weak var presenter: UIViewController?
    var onFile: ((URL) -> Void)?
    private let key = "deckhand.bookmarks"
    private var open: [URL] = []

    func presentPicker() {
        let p = UIDocumentPickerViewController(forOpeningContentTypes: [.audio, .folder])
        p.allowsMultipleSelection = true
        p.delegate = self
        presenter?.present(p, animated: true)
    }

    func documentPicker(_ c: UIDocumentPickerViewController, didPickDocumentsAt urls: [URL]) {
        var saved = UserDefaults.standard.array(forKey: key) as? [Data] ?? []
        for u in urls where u.startAccessingSecurityScopedResource() {
            open.append(u)
            if let b = try? u.bookmarkData() { saved.append(b) }
            onFile?(u)
        }
        UserDefaults.standard.set(saved, forKey: key)
    }

    /// Re-open bookmarks from earlier launches.
    func restoreBookmarks() {
        for b in UserDefaults.standard.array(forKey: key) as? [Data] ?? [] {
            var stale = false
            if let u = try? URL(resolvingBookmarkData: b, bookmarkDataIsStale: &stale), u.startAccessingSecurityScopedResource() {
                open.append(u)
            }
        }
    }
}
