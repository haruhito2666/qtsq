import SwiftUI

@main
struct QTSQApp: App {
    @StateObject private var engine = QtsqEngine()

    var body: some Scene {
        WindowGroup {
            ContentView()
                .environmentObject(engine)
        }
    }
}
