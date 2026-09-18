import Foundation

/// A PopSilicon product: one family of compatibility apps built from the same
/// runtime.  The installer asks for the product first and then offers only
/// that product's games.
enum Product: String, CaseIterable, Identifiable {
    case peggleSilicon
    case bejeweledSilicon
    case chuzzleSilicon
    case pvzSilicon
    case zumaSilicon
    case bookwormSilicon
    case feedingFrenzySilicon

    var id: String { rawValue }

    var displayName: String {
        switch self {
        case .peggleSilicon: return "PeggleSilicon"
        case .bejeweledSilicon: return "BejeweledSilicon"
        case .chuzzleSilicon: return "ChuzzleSilicon"
        case .pvzSilicon: return "PvZSilicon"
        case .zumaSilicon: return "ZumaSilicon"
        case .bookwormSilicon: return "BookwormSilicon"
        case .feedingFrenzySilicon: return "FeedingFrenzySilicon"
        }
    }

    var games: [Game] {
        switch self {
        case .peggleSilicon: return [.deluxe, .nights]
        case .bejeweledSilicon: return [.bejeweled3, .bejeweled2]
        case .chuzzleSilicon: return [.chuzzle]
        case .pvzSilicon: return [.plantsVsZombies]
        case .zumaSilicon: return [.zuma, .zumasRevenge]
        case .bookwormSilicon: return [.bookworm]
        case .feedingFrenzySilicon: return [.feedingFrenzy]
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
        case .chuzzleSilicon: return "circle.grid.3x3.fill"
        case .pvzSilicon: return "leaf.fill"
        case .zumaSilicon: return "circle.circle.fill"
        case .bookwormSilicon: return "book.fill"
        case .feedingFrenzySilicon: return "fish.fill"
        }
    }

    /// The product a game belongs to.
    static func containing(_ game: Game) -> Product {
        allCases.first { $0.games.contains(game) } ?? .peggleSilicon
    }
}
