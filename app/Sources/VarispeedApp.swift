import SwiftUI

@main
struct VarispeedApp: App {
    @NSApplicationDelegateAdaptor(AppDelegate.self) private var appDelegate

    var body: some Scene {
        MenuBarExtra {
            MenuView().environmentObject(appDelegate.engine)
        } label: {
            MenuBarLabel(engine: appDelegate.engine)
        }
        .menuBarExtraStyle(.window)
    }
}

final class AppDelegate: NSObject, NSApplicationDelegate {
    @MainActor lazy var engine = Engine()

    func applicationWillTerminate(_ notification: Notification) {
        MainActor.assumeIsolated { engine.shutdown() }
    }
}

/// Menu bar: a dial icon, plus the speed when it isn't 100 % and a dot while recording.
struct MenuBarLabel: View {
    @ObservedObject var engine: Engine

    var body: some View {
        HStack(spacing: 3) {
            Image(systemName: engine.isRecording ? "record.circle.fill" : "dial.medium")
            if abs(engine.currentSpeed - 1) > 0.0005 {
                Text(String(format: "%.0f%%", engine.currentSpeed * 100)).monospacedDigit()
            }
        }
    }
}
