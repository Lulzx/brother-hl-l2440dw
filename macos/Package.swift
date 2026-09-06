// swift-tools-version: 6.2
import PackageDescription

let mupdf = "/opt/homebrew"

let package = Package(
    name: "Brhbp",
    platforms: [.macOS(.v26)],
    targets: [
        // The C++ engine behind a flat C surface. Sources are symlinks into
        // ../cpp so there is exactly one copy of the encoder in the repo.
        .target(
            name: "BrhbpBridge",
            path: "Sources/BrhbpBridge",
            sources: ["brhbp_bridge.cc", "brhbp.cc", "status.cc", "ippcancel.cc"],
            publicHeadersPath: "include",
            cxxSettings: [
                .headerSearchPath("."),
                .unsafeFlags(["-I\(mupdf)/include", "-std=c++17"]),
            ],
            linkerSettings: [
                .unsafeFlags(["-L\(mupdf)/lib", "-lmupdf", "-lmupdf-third"]),
            ]
        ),
        .executableTarget(
            name: "BrhbpApp",
            dependencies: ["BrhbpBridge"],
            path: "Sources/BrhbpApp",
            swiftSettings: [.swiftLanguageMode(.v6)]
        ),
    ]
)
