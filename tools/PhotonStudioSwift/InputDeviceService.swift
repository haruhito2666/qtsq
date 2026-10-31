import Foundation
import CoreAudio
import AudioToolbox

// MARK: - Input Device Model

/// A CoreAudio input device available for Studio recording.
struct InputDevice: Identifiable, Equatable, Hashable {
    /// Stable CoreAudio device UID (survives reboots; used for selection).
    let uid: String
    /// Human-readable device name.
    let name: String
    /// Input channel count reported by the hardware.
    let channels: Int

    var id: String { uid }

    var displayName: String { name }

    var channelLabel: String {
        switch channels {
        case 1: return "mono"
        case 2: return "stereo"
        default: return "\(channels)-ch"
        }
    }
}

// MARK: - Input Device Service

/// Enumerates CoreAudio input devices and tracks the system default.
///
/// Selection happens at record time in AudioRecorderService (it sets
/// kAudioOutputUnitProperty_CurrentDevice on the input unit before the
/// engine starts). This service only lists devices and resolves the
/// persisted UID against the currently-plugged-in set.
final class InputDeviceService: ObservableObject {

    @Published private(set) var devices: [InputDevice] = []
    @Published private(set) var defaultUID: String? = nil

    init() {
        refresh()
    }

    /// Re-scan the hardware. Safe to call any time (USB hot-plug, etc.).
    func refresh() {
        var result: [InputDevice] = []
        var resolvedDefault: String? = nil

        // System default input UID.
        var defaultID: AudioDeviceID = 0
        var defaultSize: UInt32 = UInt32(MemoryLayout<AudioDeviceID>.size)
        var defaultAddr = AudioObjectPropertyAddress(
            mSelector: kAudioHardwarePropertyDefaultInputDevice,
            mScope: kAudioObjectPropertyScopeGlobal,
            mElement: kAudioObjectPropertyElementMain)
        if AudioObjectGetPropertyData(AudioObjectID(kAudioObjectSystemObject),
                                      &defaultAddr, 0, nil, &defaultSize, &defaultID) == noErr {
            resolvedDefault = Self.uid(forDevice: defaultID)
        }

        // All devices with at least one input channel.
        var devicesAddr = AudioObjectPropertyAddress(
            mSelector: kAudioHardwarePropertyDevices,
            mScope: kAudioObjectPropertyScopeGlobal,
            mElement: kAudioObjectPropertyElementMain)
        var listSize: UInt32 = 0
        if AudioObjectGetPropertyDataSize(AudioObjectID(kAudioObjectSystemObject),
                                          &devicesAddr, 0, nil, &listSize) == noErr,
           listSize >= UInt32(MemoryLayout<AudioDeviceID>.size) {
            var ids = [AudioDeviceID](repeating: 0, count: Int(listSize) / MemoryLayout<AudioDeviceID>.size)
            var ioSize = listSize
            let status = ids.withUnsafeMutableBytes { raw in
                AudioObjectGetPropertyData(AudioObjectID(kAudioObjectSystemObject),
                                           &devicesAddr, 0, nil, &ioSize, raw.baseAddress!)
            }
            if status == noErr {
                for id in ids.prefix(Int(ioSize) / MemoryLayout<AudioDeviceID>.size) {
                    if let dev = Self.device(id) {
                        result.append(dev)
                    }
                }
            }
        }

        DispatchQueue.main.async { [weak self] in
            guard let self = self else { return }
            self.devices = result
            self.defaultUID = resolvedDefault
        }
    }

    /// Resolves a saved UID. Returns nil when the device is unplugged
    /// (the caller should fall back to the system default and tell the
    /// user via toast).
    func device(uid: String) -> InputDevice? {
        devices.first { $0.uid == uid }
    }

    // MARK: - CoreAudio helpers

    /// Total input channels across the device's input stream buffers.
    private static func inputChannelCount(_ id: AudioDeviceID) -> Int {
        var addr = AudioObjectPropertyAddress(
            mSelector: kAudioDevicePropertyStreamConfiguration,
            mScope: kAudioDevicePropertyScopeInput,
            mElement: kAudioObjectPropertyElementMain)
        var size: UInt32 = 0
        guard AudioObjectGetPropertyDataSize(id, &addr, 0, nil, &size) == noErr,
              size >= UInt32(MemoryLayout<AudioBufferList>.size) else { return 0 }
        let buf = malloc(Int(size))!
        defer { free(buf) }
        var ioSize = size
        let status = AudioObjectGetPropertyData(id, &addr, 0, nil, &ioSize, buf)
        guard status == noErr else { return 0 }
        let list = buf.assumingMemoryBound(to: AudioBufferList.self)
        var ch: UInt32 = 0
        let buffers = UnsafeMutableAudioBufferListPointer(list)
        for b in buffers where b.mNumberChannels > 0 {
            ch += b.mNumberChannels
        }
        return Int(ch)
    }

    private static func device(_ id: AudioDeviceID) -> InputDevice? {
        guard let uid = uid(forDevice: id) else { return nil }
        let channels = inputChannelCount(id)
        guard channels > 0 else { return nil }
        let name = stringProperty(id, kAudioObjectPropertyName) ?? "Unknown device"
        /* Manufacturer intentionally not queried: the deprecated
         * kAudioDevicePropertyDeviceManufacturer misbehaves on this
         * system's drivers (raw bytes instead of a CFStringRef) and the
         * name alone is all the UI shows. */
        return InputDevice(uid: uid, name: name, channels: channels)
    }

    private static func uid(forDevice id: AudioDeviceID) -> String? {
        stringProperty(id, kAudioDevicePropertyDeviceUID)
    }

    private static func stringProperty(_ id: AudioDeviceID,
                                      _ selector: AudioObjectPropertySelector) -> String? {
        var addr = AudioObjectPropertyAddress(
            mSelector: selector,
            mScope: kAudioObjectPropertyScopeGlobal,
            mElement: kAudioObjectPropertyElementMain)
        var size: UInt32 = 0
        guard AudioObjectGetPropertyDataSize(id, &addr, 0, nil, &size) == noErr,
              size >= UInt32(MemoryLayout<CFString>.size) else { return nil }
        let buf = malloc(Int(size))!
        memset(buf, 0, Int(size))
        defer { free(buf) }
        var ioSize = size
        let status = AudioObjectGetPropertyData(id, &addr, 0, nil, &ioSize, buf)
        guard status == noErr else { return nil }
        /* Some third-party drivers (observed on an MSI monitor) write raw
         * string bytes instead of a CFStringRef for these properties — the
         * reported size covers the content, the actual write is shorter.
         * Only a full pointer-sized write is safe to bridge. */
        guard ioSize >= UInt32(MemoryLayout<CFString>.size) else { return nil }
        let ref = buf.bindMemory(to: UnsafeRawPointer?.self, capacity: 1)[0]
        guard let ptr = ref else { return nil }
        let cf = unsafeBitCast(ptr, to: CFString.self)
        /* CoreAudio hands back a +1 reference for object-typed properties. */
        return Unmanaged.passRetained(cf).takeRetainedValue() as String?
    }
}
