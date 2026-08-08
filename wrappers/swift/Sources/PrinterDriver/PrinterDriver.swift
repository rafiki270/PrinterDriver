import CPrinterDriver
import Foundation

/// The service: connections, queues and the persistent job store.
///
/// One driver per process is the intended shape. It owns every ``Printer`` and
/// ``PrintJob`` it hands out, and those hold it alive in turn, so dropping this object
/// while a job is still running does not tear the core down underneath it — the C driver
/// is destroyed when the last handle goes away, and destruction waits for in-flight jobs
/// to reach a terminal state.
///
/// ### Threading
///
/// Every method here is safe to call from any thread. Events, completion handlers and
/// `AsyncStream` elements are delivered on one private serial queue shared by the whole
/// driver — never on a core worker thread and never on the main queue. Hop to the main
/// actor yourself for UI work.
public final class PrinterDriver: @unchecked Sendable {
  let core: DriverCore

  /// Opens a driver.
  ///
  /// - Parameters:
  ///   - storageDirectory: where the job journal lives. `nil` means in-memory: no
  ///     journal, and therefore no crash recovery — after a restart, jobs that were in
  ///     flight are gone rather than resurfacing as ``JobResult/unknown(confidence:)``.
  ///     Point-of-sale integrations should always pass a directory.
  ///   - fsyncDisabled: skips the durability rule. For tests only.
  /// - Throws: ``PrinterDriverError`` when the storage directory cannot be created, which
  ///   is the only way this can fail.
  public init(storageDirectory: URL? = nil, fsyncDisabled: Bool = false) throws {
    var config = pd_config(
      storage_directory: nil, fsync_disabled: fsyncDisabled ? 1 : 0, log: nil, log_ctx: nil)

    let handle: OpaquePointer? = {
      guard let storageDirectory else {
        return withUnsafePointer(to: &config) { pd_create($0) }
      }
      return storageDirectory.path.withCString { path in
        config.storage_directory = path
        return withUnsafePointer(to: &config) { pd_create($0) }
      }
    }()

    guard let handle else {
      // pd_create is the one call with no handle to hang a reason on, so the ABI cannot
      // report why; its own documentation says a storage directory it cannot create is
      // the only cause.
      throw PrinterDriverError(
        "could not open the driver"
          + (storageDirectory.map { ": storage directory \($0.path) is not usable" } ?? ""))
    }
    core = DriverCore(handle: handle)
  }

  /// The capability-profile ids ``printer(_:width:profile:id:connectTimeoutMilliseconds:)``
  /// accepts, straight from `pd_profile_ids`.
  ///
  /// Enumerated rather than hardcoded, so a profile added to the core shows up here
  /// without a wrapper release.
  public static var profileIDs: [String] {
    var ids: [String] = []
    guard var cursor = pd_profile_ids() else { return ids }
    while let entry = cursor.pointee {
      ids.append(String(cString: entry))
      cursor += 1
    }
    return ids
  }

  /// Configures a printer and starts its worker.
  ///
  /// - Parameters:
  ///   - transport: how to reach the device.
  ///   - width: printable width in dots. 384, 504 and 576 are the deployed widths.
  ///   - profile: a capability-profile id from ``profileIDs``. `nil` means `"generic"`.
  ///   - id: a stable printer id for fleet-style configuration. `nil` derives one from
  ///     the endpoint.
  ///   - connectTimeoutMilliseconds: 0 means the ABI default of 3000.
  /// - Returns: a handle owned by this driver.
  /// - Note: Each call adds a printer. Calling it twice for the same endpoint gives two
  ///   printers with two queues, which is rarely what you want — keep the handle.
  public func printer(
    _ transport: Transport,
    width: PrinterWidth = .dots576,
    profile: String? = nil,
    id: String? = nil,
    connectTimeoutMilliseconds: UInt32 = 0
  ) throws -> Printer {
    switch transport {
    case .tcp(let host, let port):
      let handle = try host.withCString { hostPointer -> OpaquePointer in
        try withOptionalCString(id) { idPointer in
          try withOptionalCString(profile) { profilePointer in
            var config = pd_tcp_config(
              printer_id: idPointer,
              host: hostPointer,
              port: port,
              width_dots: width.rawValue,
              profile_id: profilePointer,
              connect_timeout_ms: connectTimeoutMilliseconds)
            guard let handle = withUnsafePointer(to: &config, { pd_add_printer_tcp(core.handle, $0) })
            else {
              throw core.lastError()
            }
            return handle
          }
        }
      }
      return Printer(core: core, handle: handle)
    }
  }

  /// Looks up any job this driver knows about by idempotency key, including jobs
  /// reconstructed from the journal after a restart.
  ///
  /// - Returns: `nil` when the key is unknown.
  /// - Note: A job reloaded from the journal carries what happened to it, never what it
  ///   contained: its result is ``JobResult/unknown(confidence:)`` and its
  ///   ``PrintJob/completionAuthority`` is ``CompletionMechanism/none``, because the
  ///   record does not say which printer's fence was available. Jobs submitted in this
  ///   session come back as the very same ``PrintJob`` object that
  ///   ``Printer/print(_:options:)`` returned.
  public func job(key: String) -> PrintJob? {
    guard let handle = key.withCString({ pd_find_job(core.handle, $0) }) else { return nil }
    return core.internJob(handle, authority: .none)
  }
}

/// Runs `body` with a C string for `value`, or with `nil`. The ABI treats `NULL` and `""`
/// alike, and both mean "use the documented default".
func withOptionalCString<R>(_ value: String?, _ body: (UnsafePointer<CChar>?) throws -> R)
  rethrows -> R
{
  guard let value else { return try body(nil) }
  return try value.withCString { try body($0) }
}
