import SwiftUI

struct SuccessMarkView: View {
    let progress: Double

    var body: some View {
        ZStack {
            Circle()
                .stroke(.green, lineWidth: 5)
                .frame(width: 76, height: 76)

            CheckmarkShape()
                .trim(from: 0, to: progress)
                .stroke(
                    .green,
                    style: StrokeStyle(lineWidth: 5, lineCap: .round, lineJoin: .round)
                )
                .frame(width: 46, height: 46)
                .animation(.easeOut(duration: 0.75), value: progress)
        }
        .accessibilityElement(children: .ignore)
        .accessibilityLabel("Installed successfully")
    }
}
