import SwiftUI
import UniformTypeIdentifiers

struct DropZoneView: View {
    @ObservedObject var model: InstallerModel

    var body: some View {
        VStack(spacing: 10) {
            if model.installationSucceeded {
                SuccessMarkView(progress: model.successProgress)
                Text("Installed PeggleSilicon successfully!")
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
                    Text("Drag Peggle Deluxe.app here")
                        .font(.headline)
                    Text(
                        model.steamInstallationState == .unpatched
                            ? "Required for a standalone export; Steam can be installed directly above."
                            : "The original Peggle Deluxe 1.0.5 application is required."
                        )
                        .font(.caption)
                        .foregroundColor(.gray)
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
        .accessibilityLabel("Peggle Deluxe application drop area")
        .accessibilityHint("Drag the original Peggle Deluxe application here")
        .onDrop(
            of: [UTType.fileURL],
            isTargeted: $model.isDropTargeted,
            perform: model.acceptDrop(providers:)
        )
    }
}
