import SwiftUI

struct SteamInstallationView: View {
    @ObservedObject var model: InstallerModel

    var body: some View {
        VStack(alignment: .leading, spacing: 10) {
            if model.steamReplacementSucceeded {
                Label("PeggleSilicon installed in Steam", systemImage: "checkmark.circle.fill")
                    .foregroundColor(.green)
                if let backupURL = model.steamBackupURL {
                    Text("Original saved as \(backupURL.lastPathComponent).")
                        .font(.caption)
                        .foregroundColor(.gray)
                }
            } else if let steamInstallationURL = model.steamInstallationURL {
                Label(
                    model.steamInstallationState == .unpatched
                        ? "Steam installation detected"
                        : model.steamInstallationState == .peggleSilicon
                            ? "PeggleSilicon already installed in Steam"
                            : "Unsupported Steam installation",
                    systemImage: model.steamInstallationState == .unpatched
                        ? "gamecontroller"
                        : model.steamInstallationState == .peggleSilicon
                            ? "checkmark.circle"
                            : "exclamationmark.triangle"
                )
                    .font(.headline)
                Text(steamInstallationURL.path)
                    .font(.caption)
                    .foregroundColor(.gray)
                    .lineLimit(2)
                    .truncationMode(.middle)
                if model.steamInstallationState == .unpatched {
                    Text("The unmodified Steam app will be used directly.")
                        .font(.caption)
                        .foregroundColor(.gray)
                } else if model.steamInstallationState == .unsupported {
                    Text("This app is not an unmodified 32-bit Peggle installation.")
                        .font(.caption)
                        .foregroundColor(.gray)
                }
                Button("Replace Steam installation…", action: model.requestSteamReplacement)
                    .disabled(!model.canReplaceSteam)
            }
        }
        .padding(14)
        .frame(maxWidth: .infinity, alignment: .leading)
        .background(
            RoundedRectangle(cornerRadius: 12)
                .fill(Color.gray.opacity(0.08))
        )
    }
}
