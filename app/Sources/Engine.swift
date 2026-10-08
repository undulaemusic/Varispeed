import AppKit
import AVFoundation
import Foundation

struct OutputDevice: Identifiable, Hashable {
    let uid: String
    let name: String
    let channels: Int
    var id: String { uid }
}

struct Take: Identifiable, Hashable {
    let url: URL
    let date: Date
    let duration: Double
    let sampleRate: Int
    var id: URL { url }
}

/// Owns the bridge and recorder, talks to the Varispeed driver, and publishes state for the UI.
@MainActor
final class Engine: ObservableObject {
    static let minSemitones = 12 * log2(Double(kVarispeed_MinSpeed))   // -12
    static let maxSemitones = 12 * log2(Double(kVarispeed_MaxSpeed))   // +12
    /// The driver never glides faster than this (kVarispeed_MinRampSeconds in the driver);
    /// shorter settings behave identically, so the slider starts here.
    static let minGlideSeconds = 0.1
    static let maxGlideSeconds = 2.0

    // MARK: Speed
    @Published var targetSpeed: Double = 1.0 {
        didSet { if targetSpeed != oldValue { sendSpeed() } }
    }
    @Published private(set) var currentSpeed: Double = 1.0
    @Published var rampSeconds: Double {
        didSet {
            defaults.set(rampSeconds, forKey: "rampSeconds")
            if let dev = varispeed { VSControlSetDouble(dev, AudioObjectPropertySelector(kVarispeedProperty_RampSeconds), rampSeconds) }
        }
    }

    // MARK: Bridge
    @Published var bridgeEnabled: Bool {
        didSet { defaults.set(bridgeEnabled, forKey: "bridgeEnabled"); restartBridge() }
    }
    @Published var outputUID: String {
        didSet { if outputUID != oldValue { defaults.set(outputUID, forKey: "outputUID"); restartBridge() } }
    }
    @Published var outputLeftChannel: Int {     // 0-based; right is +1
        didSet { if outputLeftChannel != oldValue { defaults.set(outputLeftChannel, forKey: "outputLeftChannel"); restartBridge() } }
    }
    @Published private(set) var outputDevices: [OutputDevice] = []
    @Published private(set) var bridgeRunning = false
    @Published private(set) var bridgeProblem: String?
    @Published private(set) var latencyMs: Double = 0
    @Published private(set) var dropouts: UInt64 = 0
    /// Smallest IO buffer the DAW is using on Varispeed (0 = no DAW playing to it right now).
    @Published private(set) var dawBufferFrames: Int = 0
    /// The driver's current glide scale (depends on the DAW's buffer size).
    @Published private(set) var glideScale: Double = 1
    /// Varispeed's sample rate (the DAW project's rate).
    @Published private(set) var varispeedSampleRate: Double = 48000
    private var lastSlowPoll = Date.distantPast
    @Published private(set) var driverInstalled = true
    @Published private(set) var micDenied = false

    // MARK: Recording
    @Published private(set) var isRecording = false
    @Published private(set) var isSaving = false
    @Published private(set) var recordSeconds: Double = 0
    @Published private(set) var recordLevel: Double = 0
    @Published private(set) var takes: [Take] = []
    @Published private(set) var recordingProblem: String?

    static let defaultRecordingsFolder: URL = FileManager.default.urls(for: .musicDirectory, in: .userDomainMask)[0]
        .appendingPathComponent("Varispeed Recordings", isDirectory: true)

    /// Where takes are saved. Remembered between launches.
    @Published private(set) var recordingsFolder: URL = Engine.defaultRecordingsFolder

    private let defaults = UserDefaults.standard
    private var varispeed: AudioObjectID?
    private var bridge: OpaquePointer?
    private let recorder = VSRecorderCreate()
    private var recordingURL: URL?
    private var pollTimer: Timer?
    private var lastRetry = Date.distantPast

    init() {
        rampSeconds = min(Self.maxGlideSeconds, max(Self.minGlideSeconds, defaults.object(forKey: "rampSeconds") as? Double ?? 0.5))
        bridgeEnabled = defaults.object(forKey: "bridgeEnabled") as? Bool ?? true
        outputUID = defaults.string(forKey: "outputUID") ?? ""
        outputLeftChannel = defaults.object(forKey: "outputLeftChannel") as? Int ?? 0
        if let path = defaults.string(forKey: "recordingsFolder") {
            recordingsFolder = URL(fileURLWithPath: path, isDirectory: true)
        }

        refreshDevices()
        connectDriver()
        loadTakes()
        VSControlObserveDeviceList({ ctx in
            let engine = Unmanaged<Engine>.fromOpaque(ctx!).takeUnretainedValue()
            MainActor.assumeIsolated { engine.devicesChanged() }
        }, Unmanaged.passUnretained(self).toOpaque())

        pollTimer = Timer.scheduledTimer(withTimeInterval: 0.1, repeats: true) { [weak self] _ in
            MainActor.assumeIsolated { self?.poll() }
        }
        requestMicrophoneThenStart()
    }

    // MARK: - Speed

    var semitones: Double {
        get { 12 * log2(targetSpeed) }
        set { targetSpeed = pow(2, newValue / 12) }
    }

    func setSemitones(_ st: Double) { semitones = min(max(st, Self.minSemitones), Self.maxSemitones) }
    func resetSpeed() { targetSpeed = 1.0 }

    /// Bumps the target up or down by some semitones (stops at the 25 % / 200 % limits).
    /// Rounds away float fuzz so repeated bumps land on exact semitones.
    func nudgeSemitones(_ delta: Double) {
        let st = (semitones * 1000).rounded() / 1000
        setSemitones(st + delta)
    }

    /// Sets the speed from a percentage (clamped to 25 %...200 %).
    func setPercent(_ percent: Double) {
        let s = percent / 100
        targetSpeed = min(max(s, Double(kVarispeed_MinSpeed)), Double(kVarispeed_MaxSpeed))
    }

    /// Slider position -1...1 with 100 % in the middle: the left half covers the minimum...0 semitones
    /// (50 %...100 %), the right half 0...+12 semitones (100 %...200 %). Snaps to 100 % near the middle.
    var sliderPosition: Double {
        get { semitones < 0 ? semitones / -Self.minSemitones : semitones / Self.maxSemitones }
        set {
            let p = abs(newValue) < 0.015 ? 0 : newValue
            setSemitones(p < 0 ? p * -Self.minSemitones : p * Self.maxSemitones)
        }
    }

    private func sendSpeed() {
        guard let dev = varispeed else { return }
        VSControlSetDouble(dev, AudioObjectPropertySelector(kVarispeedProperty_TargetSpeed), targetSpeed)
    }

    private func connectDriver() {
        let dev = VSControlVarispeedDevice()
        guard dev != AudioObjectID(kAudioObjectUnknown) else {
            varispeed = nil
            driverInstalled = false
            return
        }
        varispeed = dev
        driverInstalled = true
        VSControlSetDouble(dev, AudioObjectPropertySelector(kVarispeedProperty_RampSeconds), rampSeconds)
        var target = 1.0
        if VSControlGetDouble(dev, AudioObjectPropertySelector(kVarispeedProperty_TargetSpeed), &target) {
            // Keep within the app's range (an older driver allowed down to 25 %).
            targetSpeed = min(max(target, Double(kVarispeed_MinSpeed)), Double(kVarispeed_MaxSpeed))
            if targetSpeed != target { sendSpeed() }
        }
    }

    // MARK: - Bridge

    private func requestMicrophoneThenStart() {
        switch AVCaptureDevice.authorizationStatus(for: .audio) {
        case .authorized:
            restartBridge()
        case .notDetermined:
            AVCaptureDevice.requestAccess(for: .audio) { granted in
                Task { @MainActor in
                    self.micDenied = !granted
                    self.restartBridge()
                }
            }
        default:
            micDenied = true
        }
    }

    private func refreshDevices() {
        var buffer = [VSOutputDevice](repeating: VSOutputDevice(), count: 64)
        let n = Int(VSControlListOutputDevices(&buffer, Int32(buffer.count)))
        outputDevices = buffer.prefix(n).map { d in
            var d = d
            let uid = withUnsafeBytes(of: &d.uid) { String(cString: $0.bindMemory(to: CChar.self).baseAddress!) }
            let name = withUnsafeBytes(of: &d.name) { String(cString: $0.bindMemory(to: CChar.self).baseAddress!) }
            return OutputDevice(uid: uid, name: name, channels: Int(d.outputChannels))
        }
        // First run: start with whatever the Mac's default output is (we only read it).
        if outputUID.isEmpty {
            var buf = [CChar](repeating: 0, count: 256)
            if VSControlDefaultOutputDeviceUID(&buf, Int32(buf.count)) {
                let uid = String(cString: buf)
                if outputDevices.contains(where: { $0.uid == uid }) { outputUID = uid }
            }
        }
    }

    private func devicesChanged() {
        refreshDevices()
        if varispeed == nil || !driverInstalled { connectDriver() }
        if bridgeEnabled && !bridgeRunning { restartBridge() }
    }

    var selectedDevice: OutputDevice? { outputDevices.first { $0.uid == outputUID } }

    /// The selected device's own name for an output channel (1-based), if it has one.
    func channelName(_ channel: Int) -> String? {
        var buf = [CChar](repeating: 0, count: 128)
        guard VSControlOutputChannelName(outputUID, Int32(channel), &buf, Int32(buf.count)) else { return nil }
        return String(cString: buf)
    }

    func restartBridge() {
        stopBridge()
        guard bridgeEnabled, !micDenied, driverInstalled else { bridgeProblem = nil; return }
        guard let device = selectedDevice else {
            bridgeProblem = outputUID.isEmpty ? "Choose an output device" : "Output device not connected"
            return
        }
        var config = VSBridgeConfig()
        VSBridgeDefaultConfig(&config)
        let left = min(outputLeftChannel, max(0, device.channels - 2))
        config.outputChannels = (Int32(left), Int32(min(left + 1, device.channels - 1)))
        let b: OpaquePointer? = device.uid.withCString { uid in
            config.outputDeviceUID = uid
            return VSBridgeCreate(&config)         // copies the UID
        }
        guard let b else { bridgeProblem = "Couldn't create the bridge"; return }
        VSBridgeSetRecorder(b, recorder)
        if VSBridgeStart(b) {
            bridge = b
            bridgeRunning = true
            bridgeProblem = nil
        } else {
            var stats = VSBridgeStats()
            VSBridgeGetStats(b, &stats)
            bridgeProblem = stats.lastError.map { String(cString: $0) } ?? "Couldn't start"
            VSBridgeDestroy(b)
        }
    }

    private func stopBridge() {
        if isRecording { stopRecording() }
        if let b = bridge {
            VSBridgeSetRecorder(b, nil)
            VSBridgeDestroy(b)
        }
        bridge = nil
        bridgeRunning = false
    }

    // MARK: - Polling (10 Hz)

    /// The quickest a glide between two speeds can be at the DAW's current buffer size
    /// (same formula as the driver; see VarispeedProperties.h).
    func quickestGlide(from: Double, to: Double) -> Double {
        let step = (to > from ? Double(kVarispeed_MaxRiseSemitonesPerPeriod) : Double(kVarispeed_MaxFallSemitonesPerPeriod)) * glideScale
        let perFrame = log(2.0) / 12 * step / Double(kVarispeed_ZeroTimeStampPeriod)
        return max(Double(kVarispeed_MinRampSeconds), abs(1 / from - 1 / to) / (perFrame * varispeedSampleRate))
    }

    private func poll() {
        // Twice a second: the DAW's buffer size and sample rate (they change rarely).
        if let dev = varispeed, Date().timeIntervalSince(lastSlowPoll) > 0.5 {
            lastSlowPoll = Date()
            var frames = 0.0
            if VSControlGetDouble(dev, AudioObjectPropertySelector(kVarispeedProperty_ClientBufferFrames), &frames), Int(frames) != dawBufferFrames {
                dawBufferFrames = Int(frames)
            }
            var scale = 1.0
            if VSControlGetDouble(dev, AudioObjectPropertySelector(kVarispeedProperty_GlideScale), &scale), scale > 0, scale != glideScale {
                glideScale = scale
            }
            let rate = VSControlNominalSampleRate(dev)
            if rate > 0 && rate != varispeedSampleRate { varispeedSampleRate = rate }
        }
        if let dev = varispeed {
            var s = 1.0
            if VSControlGetDouble(dev, AudioObjectPropertySelector(kVarispeedProperty_CurrentSpeed), &s) {
                // Only publish visible changes: every change redraws the menu bar item.
                if abs(s - currentSpeed) > 0.0002 || (s == targetSpeed && s != currentSpeed) { currentSpeed = s }
            } else {
                varispeed = nil          // driver went away (Core Audio restarted?)
                driverInstalled = false
            }
        } else if Date().timeIntervalSince(lastRetry) > 2 {
            lastRetry = Date()
            connectDriver()
            if driverInstalled && bridgeEnabled { restartBridge() }
        }

        if let b = bridge {
            var stats = VSBridgeStats()
            VSBridgeGetStats(b, &stats)
            let latency = (stats.latencyMs + stats.outputDeviceLatencyMs).rounded()
            if latency != latencyMs { latencyMs = latency }
            let d = stats.underruns + stats.resyncs
            if d != dropouts { dropouts = d }
            if let err = stats.lastError {
                bridgeProblem = String(cString: err)
                stopBridge()
            }
        } else if bridgeEnabled && !micDenied && Date().timeIntervalSince(lastRetry) > 2 {
            lastRetry = Date()
            restartBridge()          // device reconnected, Core Audio came back, etc.
        }

        if isRecording, let rec = recorder {
            var st = VSRecorderStatus()
            VSRecorderGetStatus(rec, &st)
            if Int(st.seconds) != Int(recordSeconds) { recordSeconds = st.seconds }
            let level = recordLevel * 0.7 + min(1, st.peak) * 0.3
            if abs(level - recordLevel) > 0.01 { recordLevel = level }
        }
    }

    // MARK: - Recording

    func toggleRecording() {
        isRecording ? stopRecording() : startRecording()
    }

    private func startRecording() {
        guard let b = bridge, let rec = recorder, !isSaving else { return }
        var stats = VSBridgeStats()
        VSBridgeGetStats(b, &stats)
        do {
            try FileManager.default.createDirectory(at: recordingsFolder, withIntermediateDirectories: true)
        } catch {
            recordingProblem = "Can't use the recording folder (is the drive connected?). Choose another one."
            return
        }
        let formatter = DateFormatter()
        formatter.dateFormat = "yyyy-MM-dd HH.mm.ss"
        let url = recordingsFolder.appendingPathComponent("Varispeed \(formatter.string(from: Date())).wav")
        if VSRecorderStart(rec, url.path, stats.outputSampleRate) {
            recordingURL = url
            recordingProblem = nil
            isRecording = true
            recordSeconds = 0
        } else {
            recordingProblem = "Can't write to the recording folder. Choose another one."
        }
    }

    private func stopRecording() {
        guard isRecording, let rec = recorder else { return }
        isRecording = false
        isSaving = true
        // Save at the Live project's rate, i.e. Varispeed's nominal rate.
        let target = varispeed.map { VSControlNominalSampleRate($0) } ?? 0
        let recPtr = UInt(bitPattern: rec)
        Task.detached {
            _ = VSRecorderStop(OpaquePointer(bitPattern: recPtr), target)
            await MainActor.run {
                self.isSaving = false
                self.recordLevel = 0
                self.loadTakes()
            }
        }
    }

    func loadTakes() {
        let fm = FileManager.default
        let urls = (try? fm.contentsOfDirectory(at: recordingsFolder, includingPropertiesForKeys: [.creationDateKey, .fileSizeKey])) ?? []
        takes = urls.filter { $0.pathExtension.lowercased() == "wav" }
            .compactMap { url -> Take? in
                let values = try? url.resourceValues(forKeys: [.creationDateKey, .fileSizeKey])
                let rate = Self.wavSampleRate(url) ?? 0
                let bytes = Double(values?.fileSize ?? 0)
                let duration = rate > 0 ? max(0, bytes - 58) / Double(rate * 8) : 0
                return Take(url: url, date: values?.creationDate ?? .distantPast, duration: duration, sampleRate: rate)
            }
            .sorted { $0.date > $1.date }
            .prefix(6).map { $0 }
    }

    private static func wavSampleRate(_ url: URL) -> Int? {
        guard let h = FileHandle(forReadingAtPath: url.path) else { return nil }
        defer { try? h.close() }
        guard let data = try? h.read(upToCount: 28), data.count == 28 else { return nil }
        return Int(data[24]) | Int(data[25]) << 8 | Int(data[26]) << 16 | Int(data[27]) << 24
    }

    func revealRecordings() {
        try? FileManager.default.createDirectory(at: recordingsFolder, withIntermediateDirectories: true)
        if let latest = takes.first {
            NSWorkspace.shared.activateFileViewerSelecting([latest.url])
        } else {
            NSWorkspace.shared.open(recordingsFolder)
        }
    }

    /// Lets the user pick where takes are saved (standard folder picker; can create folders).
    func chooseRecordingsFolder() {
        let panel = NSOpenPanel()
        panel.title = "Choose where Varispeed saves recordings"
        panel.prompt = "Use This Folder"
        panel.canChooseDirectories = true
        panel.canChooseFiles = false
        panel.canCreateDirectories = true
        panel.allowsMultipleSelection = false
        panel.directoryURL = recordingsFolder
        NSApp.activate(ignoringOtherApps: true)
        guard panel.runModal() == .OK, let url = panel.url else { return }
        setRecordingsFolder(url)
    }

    func resetRecordingsFolder() { setRecordingsFolder(Self.defaultRecordingsFolder) }

    private func setRecordingsFolder(_ url: URL) {
        recordingsFolder = url
        recordingProblem = nil
        if url == Self.defaultRecordingsFolder {
            defaults.removeObject(forKey: "recordingsFolder")
        } else {
            defaults.set(url.path, forKey: "recordingsFolder")
        }
        loadTakes()
    }

    func openMicrophoneSettings() {
        NSWorkspace.shared.open(URL(string: "x-apple.systempreferences:com.apple.preference.security?Privacy_Microphone")!)
    }

    // MARK: - Quit

    /// Finishes any recording, puts the speed back to 100 % and closes the bridge.
    func shutdown() {
        if isRecording, let rec = recorder {
            isRecording = false
            _ = VSRecorderStop(rec, varispeed.map { VSControlNominalSampleRate($0) } ?? 0)
        }
        if let dev = varispeed {
            VSControlSetDouble(dev, AudioObjectPropertySelector(kVarispeedProperty_TargetSpeed), 1.0)
        }
        stopBridge()
    }
}
