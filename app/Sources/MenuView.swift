import SwiftUI

struct MenuView: View {
    @EnvironmentObject var engine: Engine

    var body: some View {
        VStack(alignment: .leading, spacing: 14) {
            header
            if !engine.driverInstalled {
                Notice(text: "The Varispeed driver isn't loaded. Run install.sh, then reopen this menu.", color: .orange)
            }
            speedSection
            Divider()
            recordSection
            Divider()
            outputSection
            Divider()
            HStack {
                Spacer()
                Button("Quit Varispeed") { NSApp.terminate(nil) }
                    .keyboardShortcut("q")
            }
        }
        .padding(16)
        .frame(width: 330)
    }

    // MARK: Header

    private var header: some View {
        HStack(alignment: .firstTextBaseline) {
            Text("Varispeed").font(.headline)
            Spacer()
            Circle()
                .fill(engine.bridgeRunning ? Color.green : Color.secondary.opacity(0.4))
                .frame(width: 8, height: 8)
            Text(engine.bridgeRunning ? "Playing through" : "Not playing through")
                .font(.caption).foregroundStyle(.secondary)
        }
    }

    // MARK: Speed

    private var speedSection: some View {
        VStack(alignment: .leading, spacing: 10) {
            HStack(alignment: .firstTextBaseline) {
                Text(percent(engine.currentSpeed))
                    .font(.system(size: 30, weight: .semibold, design: .rounded).monospacedDigit())
                Text(semitoneText(12 * log2(engine.currentSpeed)))
                    .font(.system(.body, design: .rounded).monospacedDigit())
                    .foregroundStyle(.secondary)
                Spacer()
                if abs(engine.currentSpeed - engine.targetSpeed) > 0.0005 {
                    Text("→ \(percent(engine.targetSpeed))")
                        .font(.caption.monospacedDigit()).foregroundStyle(.secondary)
                }
            }

            HStack(spacing: 6) {
                Text("25%").font(.caption2).foregroundStyle(.secondary)
                Slider(value: Binding(get: { engine.sliderPosition }, set: { engine.sliderPosition = $0 }), in: -1...1)
                    .background(alignment: .center) {
                        // 100 % mark: the slider's middle
                        Rectangle()
                            .fill(Color.secondary.opacity(0.6))
                            .frame(width: 1.5, height: 16)
                            .allowsHitTesting(false)
                    }
                Text("200%").font(.caption2).foregroundStyle(.secondary)
            }
            .disabled(!engine.driverInstalled)

            HStack(spacing: 6) {
                ForEach([-12.0, -1.0, 1.0, 12.0], id: \.self) { st in
                    Button(semitoneText(st)) { engine.nudgeSemitones(st) }
                        .controlSize(.small)
                        .help(st < 0 ? "Down \(Int(-st)) semitone\(st == -1 ? "" : "s")" : "Up \(Int(st)) semitone\(st == 1 ? "" : "s")")
                }
                Spacer()
                PercentField { engine.setPercent($0) }
                Button("100%") { engine.resetSpeed() }
                    .controlSize(.small)
                    .keyboardShortcut("0")
            }
            .disabled(!engine.driverInstalled)

            HStack {
                Text("Glide").font(.callout)
                Slider(value: $engine.rampSeconds, in: 0...5)
                Text(engine.rampSeconds < 0.05 ? "quick" : String(format: "%.1f s", engine.rampSeconds))
                    .font(.caption.monospacedDigit()).foregroundStyle(.secondary)
                    .frame(width: 42, alignment: .trailing)
            }
        }
    }

    // MARK: Recording

    private var recordSection: some View {
        VStack(alignment: .leading, spacing: 8) {
            HStack(spacing: 10) {
                Button(action: engine.toggleRecording) {
                    Label(engine.isRecording ? "Stop" : "Record",
                          systemImage: engine.isRecording ? "stop.fill" : "record.circle")
                        .foregroundStyle(engine.isRecording ? Color.primary : Color.red)
                }
                .disabled(!engine.bridgeRunning || engine.isSaving)

                if engine.isRecording {
                    Text(duration(engine.recordSeconds)).font(.callout.monospacedDigit())
                    LevelMeter(level: engine.recordLevel)
                } else if engine.isSaving {
                    ProgressView().controlSize(.small)
                    Text("Saving…").font(.callout).foregroundStyle(.secondary)
                } else {
                    Text("Records what you hear").font(.caption).foregroundStyle(.secondary)
                }
                Spacer()
            }

            if !engine.takes.isEmpty {
                Text("Drag a take into Live").font(.caption).foregroundStyle(.secondary)
                VStack(spacing: 2) {
                    ForEach(engine.takes) { take in TakeRow(take: take) }
                }
            }
            Button("Show recordings in Finder") { engine.revealRecordings() }
                .buttonStyle(.link).font(.caption)
        }
    }

    // MARK: Output

    private var outputSection: some View {
        VStack(alignment: .leading, spacing: 8) {
            Toggle("Play through to", isOn: $engine.bridgeEnabled)
            Picker("Device", selection: $engine.outputUID) {
                if engine.selectedDevice == nil {
                    Text(engine.outputUID.isEmpty ? "Choose…" : "Not connected").tag(engine.outputUID)
                }
                ForEach(engine.outputDevices) { d in Text(d.name).tag(d.uid) }
            }
            .labelsHidden()
            Picker("Outputs", selection: $engine.outputLeftChannel) {
                ForEach(channelPairs, id: \.self) { left in Text(pairName(left)).tag(left) }
            }
            .labelsHidden()

            if engine.micDenied {
                Notice(text: "Varispeed needs microphone access to hear the Varispeed device.", color: .orange) {
                    Button("Open Privacy Settings") { engine.openMicrophoneSettings() }.controlSize(.small)
                }
            } else if let problem = engine.bridgeProblem {
                Notice(text: problem, color: .orange)
            } else if engine.bridgeRunning {
                Text(String(format: "Latency %.0f ms · dropouts %llu", engine.latencyMs, engine.dropouts))
                    .font(.caption.monospacedDigit()).foregroundStyle(.secondary)
            }
        }
    }

    private var channelPairs: [Int] {
        let n = engine.selectedDevice?.channels ?? 2
        return Array(stride(from: 0, to: max(n - 1, 1), by: 2))
    }

    private func pairName(_ left: Int) -> String {
        // MOTU UltraLite mk5: 1-2 Main, 11-12 Phones
        let isMotu = engine.selectedDevice?.name.contains("UltraLite") ?? false
        let base = "Outputs \(left + 1)–\(left + 2)"
        if isMotu && left == 0 { return base + " (Main)" }
        if isMotu && left == 10 { return base + " (Phones)" }
        return base
    }

    // MARK: Formatting

    private func percent(_ s: Double) -> String { String(format: "%.1f%%", s * 100) }

    private func semitoneText(_ st: Double) -> String {
        if abs(st) < 0.005 { return "0 st" }
        let v = abs(st - st.rounded()) < 0.005 ? String(format: "%.0f", abs(st)) : String(format: "%.2f", abs(st))
        return (st < 0 ? "−" : "+") + v + " st"
    }
}

func duration(_ seconds: Double) -> String {
    let s = Int(seconds.rounded(.down))
    return String(format: "%d:%02d", s / 60, s % 60)
}

struct TakeRow: View {
    let take: Take
    @State private var hovering = false

    var body: some View {
        HStack(spacing: 8) {
            Image(systemName: "waveform").foregroundStyle(.secondary)
            Text(take.url.deletingPathExtension().lastPathComponent.replacingOccurrences(of: "Varispeed ", with: ""))
                .font(.callout).lineLimit(1)
            Spacer()
            Text(duration(take.duration)).font(.caption.monospacedDigit()).foregroundStyle(.secondary)
            Image(systemName: "line.3.horizontal").foregroundStyle(.tertiary)
        }
        .padding(.horizontal, 8).padding(.vertical, 5)
        .background(RoundedRectangle(cornerRadius: 6).fill(Color.primary.opacity(hovering ? 0.08 : 0.04)))
        .contentShape(Rectangle())
        .onHover { hovering = $0 }
        .onDrag { NSItemProvider(contentsOf: take.url) ?? NSItemProvider() }
        .help("\(take.sampleRate) Hz · drag into Live")
    }
}

/// Type a speed in percent and press Return.
struct PercentField: View {
    let onCommit: (Double) -> Void
    @State private var text = ""
    @FocusState private var focused: Bool

    var body: some View {
        HStack(spacing: 2) {
            TextField("%", text: $text)
                .textFieldStyle(.roundedBorder)
                .controlSize(.small)
                .multilineTextAlignment(.trailing)
                .frame(width: 52)
                .focused($focused)
                .onSubmit(commit)
                .help("Type a speed in percent (25 to 200) and press Return")
            Text("%").font(.caption).foregroundStyle(.secondary)
        }
    }

    private func commit() {
        let cleaned = text.replacingOccurrences(of: "%", with: "").replacingOccurrences(of: ",", with: ".")
            .trimmingCharacters(in: .whitespaces)
        if let v = Double(cleaned) { onCommit(v) }
        text = ""
        focused = false
    }
}

struct LevelMeter: View {
    let level: Double
    var body: some View {
        GeometryReader { g in
            ZStack(alignment: .leading) {
                Capsule().fill(Color.secondary.opacity(0.2))
                Capsule().fill(level > 0.95 ? Color.red : Color.green)
                    .frame(width: g.size.width * min(1, max(0, level)))
            }
        }
        .frame(width: 70, height: 6)
    }
}

struct Notice<Accessory: View>: View {
    let text: String
    let color: Color
    @ViewBuilder var accessory: () -> Accessory

    init(text: String, color: Color, @ViewBuilder accessory: @escaping () -> Accessory = { EmptyView() }) {
        self.text = text
        self.color = color
        self.accessory = accessory
    }

    var body: some View {
        VStack(alignment: .leading, spacing: 6) {
            Label(text, systemImage: "exclamationmark.triangle.fill")
                .font(.caption)
                .foregroundStyle(color)
                .fixedSize(horizontal: false, vertical: true)
            accessory()
        }
    }
}
