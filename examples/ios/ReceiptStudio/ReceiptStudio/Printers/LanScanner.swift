//
//  LanScanner.swift
//  ReceiptStudio
//
//  Sweeps the local /24 for anything accepting TCP on 9100.
//
//  A connect-and-drop probe is all this does, deliberately: it says a socket
//  answered, nothing more. An open 9100 is a candidate, not a printer, and the
//  app never calls it one — identification is the probe behind the Identify
//  button (docs/capability-profiles.md), and that only runs when an operator asks.
//
//  Requires NSLocalNetworkUsageDescription; iOS shows the local-network prompt on
//  the first connection attempt.
//

import Foundation
import Network
import Observation

@MainActor
@Observable
final class LanScanner {

    struct Progress: Equatable, Sendable {
        var scanned: Int = 0
        var total: Int = 0
        var fraction: Double { total == 0 ? 0 : Double(scanned) / Double(total) }
    }

    private(set) var isScanning = false
    private(set) var progress = Progress()
    private(set) var found: [DiscoveredPrinter] = []
    private(set) var subnet: String?
    private(set) var message: String?

    /// Roughly the whole /24 in flight at once. The connections are cheap and
    /// short-lived; the cap exists so a busy Wi-Fi link is not asked for 254
    /// simultaneous handshakes.
    private let maxConcurrent = 200
    private let connectTimeout: TimeInterval = 0.5
    private let port: UInt16 = 9100

    private var engine: PortSweep?

    func start() {
        guard !isScanning else { return }

        guard let address = LanScanner.localIPv4Address() else {
            message = "No Wi-Fi address. Join a network and try again."
            return
        }
        let octets = address.split(separator: ".")
        guard octets.count == 4 else {
            message = "Could not read the local subnet."
            return
        }
        let base = octets.prefix(3).joined(separator: ".")

        subnet = "\(base).0/24"
        message = nil
        found = []
        progress = Progress(scanned: 0, total: 254)
        isScanning = true

        let sweep = PortSweep(base: base,
                              port: port,
                              timeout: connectTimeout,
                              maxConcurrent: maxConcurrent)
        engine = sweep
        sweep.run(
            onProgress: { [weak self] scanned in
                guard let self else { return }
                self.progress.scanned = scanned
            },
            onFound: { [weak self] host, elapsed in
                guard let self else { return }
                let discovered = DiscoveredPrinter(host: host, port: self.port, respondedIn: elapsed)
                guard !self.found.contains(where: { $0.host == host }) else { return }
                self.found.append(discovered)
                self.found.sort { LanScanner.lastOctet($0.host) < LanScanner.lastOctet($1.host) }
            },
            onFinished: { [weak self] in
                guard let self else { return }
                self.isScanning = false
                self.engine = nil
                if self.found.isEmpty {
                    self.message = "Nothing answered on port 9100. Add a printer by IP if you know it."
                }
            })
    }

    func cancel() {
        engine?.cancel()
        engine = nil
        isScanning = false
    }

    private static func lastOctet(_ host: String) -> Int {
        Int(host.split(separator: ".").last ?? "0") ?? 0
    }

    // MARK: Local address

    /// First non-loopback, non-link-local IPv4 on an `en*` interface — Wi-Fi in
    /// practice, which is the only interface a venue's printers live on.
    static func localIPv4Address() -> String? {
        var head: UnsafeMutablePointer<ifaddrs>?
        guard getifaddrs(&head) == 0, let first = head else { return nil }
        defer { freeifaddrs(head) }

        var best: String?
        var cursor: UnsafeMutablePointer<ifaddrs>? = first
        while let pointer = cursor {
            let interface = pointer.pointee
            cursor = interface.ifa_next

            guard let sockaddr = interface.ifa_addr,
                  sockaddr.pointee.sa_family == UInt8(AF_INET) else { continue }
            let name = String(cString: interface.ifa_name)
            guard name.hasPrefix("en") else { continue }

            var buffer = [CChar](repeating: 0, count: Int(NI_MAXHOST))
            guard getnameinfo(sockaddr, socklen_t(sockaddr.pointee.sa_len),
                              &buffer, socklen_t(buffer.count),
                              nil, 0, NI_NUMERICHOST) == 0 else { continue }
            let address = String(cString: buffer)
            guard !address.hasPrefix("127."), !address.hasPrefix("169.254.") else { continue }

            if name == "en0" { return address }
            if best == nil { best = address }
        }
        return best
    }
}

// MARK: - The sweep itself

/// Off the main actor on purpose: 254 NWConnections with their own state handlers
/// have no business waking the UI thread for anything but progress.
private final class PortSweep: @unchecked Sendable {

    private let base: String
    private let port: UInt16
    private let timeout: TimeInterval
    private let maxConcurrent: Int

    private let connectionQueue = DispatchQueue(label: "receiptstudio.scan.connections",
                                                attributes: .concurrent)
    private let driverQueue = DispatchQueue(label: "receiptstudio.scan.driver")
    private let lock = NSLock()
    private var cancelled = false
    private var scanned = 0
    private var live: [String: NWConnection] = [:]

    init(base: String, port: UInt16, timeout: TimeInterval, maxConcurrent: Int) {
        self.base = base
        self.port = port
        self.timeout = timeout
        self.maxConcurrent = maxConcurrent
    }

    func cancel() {
        lock.lock()
        cancelled = true
        let connections = live
        live.removeAll()
        lock.unlock()
        for connection in connections.values {
            connection.cancel()
        }
    }

    func run(onProgress: @escaping @MainActor (Int) -> Void,
             onFound: @escaping @MainActor (String, TimeInterval) -> Void,
             onFinished: @escaping @MainActor () -> Void) {

        driverQueue.async { [self] in
            let slots = DispatchSemaphore(value: maxConcurrent)
            let group = DispatchGroup()

            for octet in 1...254 {
                lock.lock()
                let stop = cancelled
                lock.unlock()
                if stop { break }

                slots.wait()
                group.enter()
                let host = "\(base).\(octet)"
                probe(host: host) { open, elapsed in
                    if open {
                        Task { @MainActor in onFound(host, elapsed) }
                    }
                    self.lock.lock()
                    self.scanned += 1
                    let count = self.scanned
                    self.lock.unlock()
                    Task { @MainActor in onProgress(count) }
                    slots.signal()
                    group.leave()
                }
            }

            group.wait()
            Task { @MainActor in onFinished() }
        }
    }

    /// One connect attempt, resolved exactly once: whichever of the state handler
    /// and the timeout gets there first wins, and the other becomes a no-op.
    private func probe(host: String, completion: @escaping (Bool, TimeInterval) -> Void) {
        guard let endpointPort = NWEndpoint.Port(rawValue: port) else {
            completion(false, 0)
            return
        }

        let options = NWProtocolTCP.Options()
        options.connectionTimeout = 2
        options.noDelay = true
        let parameters = NWParameters(tls: nil, tcp: options)
        parameters.prohibitedInterfaceTypes = [.cellular]

        let connection = NWConnection(host: NWEndpoint.Host(host),
                                      port: endpointPort,
                                      using: parameters)
        let started = Date()
        let settled = NSLock()
        var done = false

        func finish(_ open: Bool) {
            settled.lock()
            if done {
                settled.unlock()
                return
            }
            done = true
            settled.unlock()

            connection.stateUpdateHandler = nil
            connection.cancel()
            lock.lock()
            live[host] = nil
            lock.unlock()
            completion(open, Date().timeIntervalSince(started))
        }

        lock.lock()
        live[host] = connection
        lock.unlock()

        connection.stateUpdateHandler = { state in
            switch state {
            case .ready:
                finish(true)
            case .failed, .cancelled:
                finish(false)
            case .waiting:
                // "Waiting" on a LAN sweep means refused or unreachable; nothing on
                // this address is going to become ready by waiting longer.
                finish(false)
            default:
                break
            }
        }

        connection.start(queue: connectionQueue)
        connectionQueue.asyncAfter(deadline: .now() + timeout) {
            finish(false)
        }
    }
}
