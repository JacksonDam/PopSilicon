import Foundation

/// A PopSilicon product: one family of compatibility apps built from the same
/// runtime.  The installer asks for the product first and then offers only
/// that product's games.
enum Product: String, CaseIterable, Identifiable {
    case peggleSilicon
    case bejeweledSilicon

    var id: String { rawValue }

    var displayName: String {
        switch self {
        case .peggleSilicon: return "PeggleSilicon"
        case .bejeweledSilicon: return "BejeweledSilicon"
        }
    }

    var games: [Game] {
        switch self {
        case .peggleSilicon: return [.deluxe, .nights]
        case .bejeweledSilicon: return [.bejeweled3, .bejeweled2]
        }
    }

    /// "Peggle Deluxe or Peggle Nights"
    var gameList: String {
        let names = games.map(\.displayName)
        guard names.count > 1, let last = names.last else { return names.first ?? "" }
        return names.dropLast().joined(separator: ", ") + " or " + last
    }

    var symbol: String {
        switch self {
        case .peggleSilicon: return "circle.hexagongrid.fill"
        case .bejeweledSilicon: return "diamond.fill"
        }
    }

    /// The product a game belongs to.
    static func containing(_ game: Game) -> Product {
        allCases.first { $0.games.contains(game) } ?? .peggleSilicon
    }
}
