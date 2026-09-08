import SwiftUI

/// The installer's first screen: which product to install.
struct ProductPickerView: View {
    @ObservedObject var model: InstallerModel

    var body: some View {
        VStack(alignment: .leading, spacing: 20) {
            VStack(alignment: .leading, spacing: 6) {
                Text("PopSilicon Installer")
                    .font(.largeTitle.bold())
                Text("Run the classic 32-bit PopCap Mac games on Apple silicon. Choose what to install.")
                    .foregroundColor(.gray)
                    .fixedSize(horizontal: false, vertical: true)
            }

            // The list grows with every product added, so let it scroll rather
            // than push the heading off the top of the window.
            ScrollView {
                VStack(alignment: .leading, spacing: 12) {
                    ForEach(Product.allCases) { product in
                        card(for: product)
                    }
                }
            }
        }
        .padding(28)
    }

    private func card(for product: Product) -> some View {
        Button {
            model.selectProduct(product)
        } label: {
            HStack(spacing: 16) {
                Image(systemName: product.symbol)
                    .font(.system(size: 30))
                    .frame(width: 44)
                VStack(alignment: .leading, spacing: 4) {
                    Text(product.displayName)
                        .font(.title2.bold())
                    Text(product.gameList)
                        .foregroundColor(.gray)
                    if let note = steamNote(for: product) {
                        Text(note)
                            .font(.caption)
                            .foregroundColor(.gray)
                    }
                }
                Spacer()
                Image(systemName: "chevron.right")
                    .foregroundColor(.gray)
            }
            .padding(18)
            .frame(maxWidth: .infinity, alignment: .leading)
            .background(
                RoundedRectangle(cornerRadius: 12)
                    .fill(Color.gray.opacity(0.08))
            )
            .overlay(
                RoundedRectangle(cornerRadius: 12)
                    .stroke(Color.gray.opacity(0.25))
            )
            .contentShape(Rectangle())
        }
        .buttonStyle(.plain)
        .accessibilityLabel("Install \(product.displayName)")
    }

    private func steamNote(for product: Product) -> String? {
        let detected = product.games
            .filter { SteamLocator.find($0) != nil }
            .map(\.displayName)
        guard !detected.isEmpty else { return nil }
        return "Steam installation detected: " + detected.joined(separator: ", ")
    }
}
