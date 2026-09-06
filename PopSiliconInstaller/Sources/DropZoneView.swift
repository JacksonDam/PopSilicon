import SwiftUI
import UniformTypeIdentifiers

struct DropZoneView: View {
    @ObservedObject var model: InstallerModel

    var body: some View {
        VStack(spacing: 10) {
            if model.installationSucceeded {
                SuccessMarkView(progress: model.successProgress)
                Text("Installed \(model.productName) successfully!")
                    .font(.headline)
            } else {
                Image(systemName: "arrow.down.app")
                    .font(.system(size: 34))
                    .foregroundColor(.accentColor)

                if let sourceURL = model.sourceURL {
                    Text(sourceURL.lastPathComponent)
                        .font(.headline)
                        .lineLimit(1)
                    Text(sourceURL.path)
                        .font(.caption)
                        .foregroundColor(.gray)
                        .lineLimit(2)
                } else {
                    Text("Drag \(model.selectedGame.steamAppName) here")
                        .font(.headline)
                    Text(model.dropZoneHint)
                        .font(.caption)
                        .foregroundColor(.gray)
                        .multilineTextAlignment(.center)
                        .fixedSize(horizontal: false, vertical: true)
                }
            }
        }
        .frame(maxWidth: .infinity, minHeight: 150)
        .background(
            RoundedRectangle(cornerRadius: 14)
                .fill(
                    model.isDropTargeted
                        ? Color.accentColor.opacity(0.12)
                        : Color.gray.opacity(0.08)
                )
        )
        .overlay(
            RoundedRectangle(cornerRadius: 14)
                .strokeBorder(
                    model.isDropTargeted ? Color.accentColor : Color.gray.opacity(0.35),
                    style: StrokeStyle(lineWidth: 2, dash: [8])
                )
        )
        .contentShape(Rectangle())
        .accessibilityElement(children: .combine)
        .accessibilityLabel("\(model.selectedGame.displayName) application drop area")
        .accessibilityHint("Drag the original \(model.selectedGame.displayName) application here")
        .onDrop(
            of: [UTType.fileURL],
            isTargeted: $model.isDropTargeted,
            perform: model.acceptDrop(providers:)
        )
    }
}
