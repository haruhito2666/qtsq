import Foundation
import AppKit

/// TWIN FILE — kept in sync with tools/StudioSwift/DeviceKeyStore.swift.
/// Photon Studio is fully self-contained in this folder; the main app owns
/// its own identical copy. Edit both together.

/// Device-local keypair for passwordless (encrypt-to-self) saves.
///
/// Stored as a 0600 file under Application Support instead of the Keychain:
/// Keychain ACLs are tied to the exact code signature, so every ad-hoc
/// rebuild of QTSQStudio triggered a "wants to use your confidential
/// information" authorization prompt. A per-user file never prompts and
/// survives rebuilds. Files saved this way remain ML-KEM-768 encrypted at
/// rest and open without a password on this user account.
final class DeviceKeyStore {
    static let shared = DeviceKeyStore()

    private let pkSize = 1184
    private let skSize = 2400

    private var cachedPK: [UInt8]?
    private var cachedSK: [UInt8]?

    private var keyDir: URL? {
        guard let base = FileManager.default.urls(for: .applicationSupportDirectory, in: .userDomainMask).first else { return nil }
        return base.appendingPathComponent("QTSQ", isDirectory: true)
    }

    /// Loads (or generates) the device keypair. Returns false on IO failure.
    @discardableResult
    func ensureKeys() -> Bool {
        if cachedPK != nil && cachedSK != nil { return true }
        guard let dir = keyDir else { return false }

        let fm = FileManager.default
        try? fm.createDirectory(at: dir, withIntermediateDirectories: true)

        let skURL = dir.appendingPathComponent("device_kem.sk")
        let pkURL = dir.appendingPathComponent("device_kem.pk")

        if let sk = try? Data(contentsOf: skURL), let pk = try? Data(contentsOf: pkURL),
           sk.count == skSize, pk.count == pkSize {
            cachedSK = [UInt8](sk)
            cachedPK = [UInt8](pk)
            return true
        }

        /* First run on this account: generate a fresh ML-KEM-768 pair */
        var kp = qtsq_keypair_t()
        guard qtsq_keygen(&kp) == QTSQ_OK else { return false }

        let pkData = withUnsafeBytes(of: kp.public_key) { Data($0.prefix(pkSize)) }
        let skData = withUnsafeBytes(of: kp.secret_key) { Data($0.prefix(skSize)) }

        do {
            try pkData.write(to: pkURL, options: [.atomic])
            try skData.write(to: skURL, options: [.atomic, .completeFileProtection])
            try fm.setAttributes([.posixPermissions: 0o600], ofItemAtPath: skURL.path)
            try fm.setAttributes([.posixPermissions: 0o600], ofItemAtPath: pkURL.path)
        } catch {
            return false
        }

        cachedPK = [UInt8](pkData)
        cachedSK = [UInt8](skData)
        return true
    }

    /// True when a usable device keypair is available.
    var isAvailable: Bool {
        ensureKeys()
    }

    /// Encrypts ctx to this device (passwordless save). Returns QTSQ_OK.
    func encryptToSelf(_ ctx: UnsafeMutablePointer<qtsq_context_t>) -> Int32 {
        guard ensureKeys(), let pk = cachedPK else { return QTSQ_ERR_IO }
        var key = pk
        let res = key.withUnsafeMutableBufferPointer { buf in
            qtsq_vault_encrypt_pq(ctx, buf.baseAddress!)
        }
        return res
    }

    /// Decrypts a device-encrypted ctx. Returns QTSQ_OK.
    func decryptWithDeviceKey(_ ctx: UnsafeMutablePointer<qtsq_context_t>) -> Int32 {
        guard ensureKeys(), let sk = cachedSK else { return QTSQ_ERR_IO }
        var key = sk
        let res = key.withUnsafeMutableBufferPointer { buf in
            qtsq_vault_decrypt_pq(ctx, buf.baseAddress!)
        }
        return res
    }
}

/// Asks the user whether to encrypt before saving. Encryption is optional:
///  - "Encrypt & Save"            → encrypts with the entered passphrase
///  - "Save Without a Password"   → encrypts to this device's key (no passphrase)
///  - "Cancel"                    → aborts the save
/// Returns true when the caller should proceed with qtsq_write().
/// Shared between QTSQStudio and Photon Studio (both compile this file).
func qtsqPromptAndEncrypt(_ ctx: UnsafeMutablePointer<qtsq_context_t>!) -> Bool {
    /* If already encrypted, no prompt needed */
    if qtsq_is_encrypted(ctx) != 0 { return true }

    while true {
        let alert = NSAlert()
        alert.messageText = "Protect this file?"
        alert.informativeText = "Encrypt with a passphrase to open it anywhere, or save without a password to lock it to this Mac (files are always encrypted on disk)."
        alert.alertStyle = .informational

        let pwField = NSSecureTextField(frame: NSRect(x: 0, y: 0, width: 300, height: 24))
        pwField.placeholderString = "Passphrase (optional)"
        alert.accessoryView = pwField
        alert.window.initialFirstResponder = pwField

        alert.addButton(withTitle: "Encrypt & Save")
        alert.addButton(withTitle: "Save Without a Password")
        alert.addButton(withTitle: "Cancel")

        let response = alert.runModal()
        if response == .alertSecondButtonReturn {
            /* Passwordless save: encrypt to this device's key so qtsq_write's
             * no-plaintext gate is satisfied and the file opens here without
             * any password. */
            if !DeviceKeyStore.shared.ensureKeys() {
                let err = NSAlert()
                err.messageText = "Device Keys Unavailable"
                err.informativeText = "Could not load this Mac's encryption keys. Enter a passphrase instead."
                err.runModal()
                continue
            }
            let res = DeviceKeyStore.shared.encryptToSelf(ctx)
            if res != QTSQ_OK {
                let err = NSAlert()
                err.messageText = "Save Failed"
                err.informativeText = String(cString: qtsq_error_string(res))
                err.runModal()
                return false
            }
            return true
        }
        if response != .alertFirstButtonReturn {
            return false  /* cancel */
        }

        let pw = pwField.stringValue.trimmingCharacters(in: .whitespaces)
        if pw.isEmpty {
            let err = NSAlert()
            err.messageText = "No Passphrase"
            err.informativeText = "Enter a passphrase, or choose \"Save Without a Password\"."
            err.runModal()
            continue  /* re-show the prompt instead of aborting the save */
        }

        let result = pw.withCString { cstr in
            return qtsq_vault_encrypt_password(ctx, cstr)
        }

        if result != 0 {
            let err = NSAlert()
            err.messageText = "Encryption Failed"
            err.informativeText = String(cString: qtsq_error_string(result))
            err.runModal()
            return false
        }
        return true
    }
}
