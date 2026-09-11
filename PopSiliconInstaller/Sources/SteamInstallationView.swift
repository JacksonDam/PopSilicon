import SwiftUI

struct SteamInstallationView: View {
    @ObservedObject var model: InstallerModel

    var body: some View {
        VStack(alignment: .leading, spacing: 10) {
            if model.steamReplacementSucceeded {
                Label("\(model.productName) \(model.steamCompletionLabel)", systemImage: "checkmark.circle.fill")
                    .foregroundColor(.green)
                if let backupURL = model.steamBackupURL {
                    Text("Original saved as \(backupURL.lastPathComponent).")
                        .font(.caption)
                        .foregroundColor(.gray)
                }
            } else if let steamInstallationURL = model.steamInstallationURL {
                Label(headline, systemImage: symbol)
                    .font(.headline)
                Text(steamInstallationURL.path)
                    .font(.caption)
                    .foregroundColor(.gray)
                    .lineLimit(2)
                    .truncationMode(.middle)
                if let detail {
                    Text(detail)
                        .font(.caption)
                        .foregroundColor(.gray)
                        .fixedSize(horizontal: false, vertical: true)
                }
                Button(model.steamActionTitle, action: model.requestSteamReplacement)
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

    private var headline: String {
        switch model.steamInstallationState {
        case .unpatched: return "Steam installation detected"
        case .peggleSilicon: return "\(model.productName) installed in Steam"
        case .needsRepair: return "\(model.productName) in Steam needs repair"
        case .unsupported: return "Unsupported Steam installation"
        }
    }

    private var symbol: String {
        switch model.steamInstallationState {
        case .unpatched: return "gamecontroller"
        case .peggleSilicon: return "checkmark.circle"
        case .needsRepair, .unsupported: return "exclamationmark.triangle"
        }
    }

    private var detail: String? {
        switch model.steamInstallationState {
        case .unpatched:
            return "Steam's copy is DRM-protected; its game code is unwrapped automatically. No separate download is needed."
        case .needsRepair:
            return model.canReplaceSteam
                ? "The game image is still DRM-encrypted; it can be repaired from the backup."
                : "The game image is DRM-encrypted and no \(model.selectedGame.steamAppName).bak backup was found to rebuild from."
        case .peggleSilicon:
            return model.canReplaceSteam
                ? "It can be rebuilt from the backup so that it matches this copy of the project."
                : "No \(model.selectedGame.steamAppName).bak backup was found, so it cannot be rebuilt."
        case .unsupported:
            return "This app is not an unmodified 32-bit \(model.selectedGame.displayName) installation."
        }
    }
}
