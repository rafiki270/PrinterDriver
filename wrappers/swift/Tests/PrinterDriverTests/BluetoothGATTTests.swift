import Foundation
import XCTest

@testable import PrinterDriver

/// §4's GATT discovery algorithm, exercised with fake descriptors
/// (docs/wire-protocols.md §4, docs/compatibility-brief.md §25).
///
/// No radio, no CoreBluetooth, no printer. That is the point of the split: the heuristic
/// that decides which characteristic a receipt goes out of is the one piece of the
/// Bluetooth path that can be wrong *quietly* — a UUID that matched, properties that did
/// not, and a job that reports the same grade it would have reported had the bytes
/// arrived. A heuristic testable only by pairing a physical printer is a heuristic nobody
/// re-runs, so every rule §4 states is asserted here against plain values and runs on
/// every commit in CI.
///
/// The descriptors below are deliberately the awkward shapes rather than the textbook one:
/// a family whose UUIDs match and whose properties do not, a single characteristic doing
/// both directions, a peripheral carrying two families at once.
final class BluetoothGATTTests: XCTestCase {

  // MARK: - Fake descriptors

  private func characteristic(
    _ uuid: String, _ properties: GATTCharacteristicProperties
  ) -> GATTCharacteristicDescription {
    GATTCharacteristicDescription(uuid: GATTUUID(uuid), properties: properties)
  }

  private func service(
    _ uuid: String, _ characteristics: [GATTCharacteristicDescription]
  ) -> GATTServiceDescription {
    GATTServiceDescription(uuid: GATTUUID(uuid), characteristics: characteristics)
  }

  /// A well-behaved Microchip Transparent UART module.
  private var microchip: GATTServiceDescription {
    service(
      "49535343-FE7D-4AE5-8FA9-9FAFD205E455",
      [
        characteristic("49535343-8841-43F4-A8D4-ECBE34729BB3", [.write, .writeWithoutResponse]),
        characteristic("49535343-1E4D-4BD9-BA61-23C647249616", [.notify]),
      ])
  }

  /// A well-behaved Nordic UART Service.
  private var nordic: GATTServiceDescription {
    service(
      "6E400001-B5A3-F393-E0A9-E50E24DCCA9E",
      [
        characteristic("6E400002-B5A3-F393-E0A9-E50E24DCCA9E", [.write, .writeWithoutResponse]),
        characteristic("6E400003-B5A3-F393-E0A9-E50E24DCCA9E", [.notify]),
      ])
  }

  /// The generic-access service every peripheral carries and nothing can print through.
  private var genericAccess: GATTServiceDescription {
    service("1800", [characteristic("2A00", [.read])])
  }

  // MARK: - Step (1): a configured pair wins outright

  func testAnExplicitlyConfiguredPairWinsOverEveryProbeProfile() {
    // §4 step (1). The caller knows this printer; the probe order is what you fall back on
    // when nobody does. A device that also happens to expose a textbook Microchip UART must
    // still be driven through the pair the caller named, or a configuration would be a
    // suggestion rather than an instruction.
    let vendor = service(
      "A1B2C3D4-0000-4000-8000-000000000001",
      [
        characteristic("A1B2C3D4-0000-4000-8000-000000000002", [.write]),
        characteristic("A1B2C3D4-0000-4000-8000-000000000003", [.indicate]),
      ])
    let configuration = BLEEndpointConfiguration(
      service: "A1B2C3D4-0000-4000-8000-000000000001",
      outboundCharacteristic: "A1B2C3D4-0000-4000-8000-000000000002",
      inboundCharacteristic: "A1B2C3D4-0000-4000-8000-000000000003")

    let selection = BLEEndpointSelector.select(
      services: [microchip, vendor, nordic], configuration: configuration)

    XCTAssertEqual(selection?.profile, .configured)
    XCTAssertEqual(selection?.service, "A1B2C3D4-0000-4000-8000-000000000001")
    XCTAssertEqual(selection?.outbound, "A1B2C3D4-0000-4000-8000-000000000002")
    XCTAssertEqual(selection?.inbound, "A1B2C3D4-0000-4000-8000-000000000003")
    XCTAssertEqual(selection?.writeMode, .withResponse)
    // Indicate is an inbound property in its own right, not a lesser Notify.
    XCTAssertEqual(selection?.inboundDelivery, .indicate)
  }

  func testAConfiguredPairThatDoesNotValidateSelectsNothingRatherThanProbingOn() {
    // The silent substitution this SDK refuses. The caller named a service; that service is
    // present and its characteristics cannot carry traffic. Falling through to the Microchip
    // profile sitting right next to it would produce a printer that works, out of a
    // characteristic the caller never approved, with nothing anywhere saying so.
    let broken = service(
      "A1B2C3D4-0000-4000-8000-000000000001",
      [
        characteristic("A1B2C3D4-0000-4000-8000-000000000002", [.read]),
        characteristic("A1B2C3D4-0000-4000-8000-000000000003", [.read]),
      ])
    let configuration = BLEEndpointConfiguration(
      service: "A1B2C3D4-0000-4000-8000-000000000001",
      outboundCharacteristic: "A1B2C3D4-0000-4000-8000-000000000002",
      inboundCharacteristic: "A1B2C3D4-0000-4000-8000-000000000003")

    XCTAssertNil(
      BLEEndpointSelector.select(services: [broken, microchip], configuration: configuration))
  }

  func testConfiguringOnlyTheServiceNarrowsToItAndPicksCharacteristicsByProperty() {
    // The middle case: the caller knows which service, nobody published the characteristic
    // UUIDs. Naming the service is still a claim, so the search stays inside it.
    let vendor = service(
      "A1B2C3D4-0000-4000-8000-000000000001",
      [
        characteristic("A1B2C3D4-0000-4000-8000-00000000000A", [.read]),
        characteristic("A1B2C3D4-0000-4000-8000-00000000000B", [.writeWithoutResponse]),
        characteristic("A1B2C3D4-0000-4000-8000-00000000000C", [.notify]),
      ])
    let selection = BLEEndpointSelector.select(
      services: [nordic, vendor],
      configuration: BLEEndpointConfiguration(service: "A1B2C3D4-0000-4000-8000-000000000001"))

    XCTAssertEqual(selection?.profile, .configured)
    XCTAssertEqual(selection?.outbound, "A1B2C3D4-0000-4000-8000-00000000000B")
    XCTAssertEqual(selection?.inbound, "A1B2C3D4-0000-4000-8000-00000000000C")
    // Write Command only, so that is what it is driven with — the backpressure path.
    XCTAssertEqual(selection?.writeMode, .withoutResponse)
  }

  // MARK: - Step (2): the probe order

  func testMicrochipIsPreferredOverNordicWhenTheDeviceCarriesBoth() {
    // §4 step (2) states the order Microchip → Nordic → FFE0, and the order is load-bearing
    // rather than alphabetical: the specific, vendor-allocated UUIDs are probed before the
    // SIG-range ones any firmware can reuse. A dual-stack module must land on the family
    // whose behaviour is known.
    let selection = BLEEndpointSelector.select(services: [genericAccess, nordic, microchip])

    XCTAssertEqual(selection?.profile, .microchipTransparentUART)
    XCTAssertEqual(selection?.outbound, "49535343-8841-43F4-A8D4-ECBE34729BB3")
    XCTAssertEqual(selection?.inbound, "49535343-1E4D-4BD9-BA61-23C647249616")
    XCTAssertEqual(selection?.writeMode, .withResponse)
    XCTAssertEqual(selection?.inboundDelivery, .notify)
  }

  func testNordicIsPreferredOverTheFfeFamilyWhenBothArePresent() {
    let ffe = service(
      "FFE0", [characteristic("FFE1", [.write, .writeWithoutResponse, .notify])])
    let selection = BLEEndpointSelector.select(services: [ffe, nordic])

    XCTAssertEqual(selection?.profile, .nordicUART)
    XCTAssertEqual(selection?.outbound, "6E400002-B5A3-F393-E0A9-E50E24DCCA9E")
  }

  func testTheProbeOrderIsExactlyTheThreeFamiliesDocumentedInThatOrder() {
    XCTAssertEqual(
      BLEUARTProfile.probeOrder.map(\.identity),
      [.microchipTransparentUART, .nordicUART, .ffeFamily])
    XCTAssertEqual(
      BLEUARTProfile.microchipTransparentUART.service, "49535343-FE7D-4AE5-8FA9-9FAFD205E455")
    XCTAssertEqual(BLEUARTProfile.nordicUART.service, "6E400001-B5A3-F393-E0A9-E50E24DCCA9E")
    XCTAssertEqual(BLEUARTProfile.ffeFamily.service, "0000FFE0-0000-1000-8000-00805F9B34FB")
  }

  // MARK: - The FFE family's variants

  func testTheFfeVariantWhereOneCharacteristicCarriesBothDirections() {
    // "TX often FFE1 … RX often FFE1 Notify". The commonest cheap module wires the whole
    // UART through a single characteristic, so outbound and inbound legitimately resolve to
    // the same UUID and a selector that insisted on two would reject the most widely
    // deployed shape on the market.
    let ffe = service(
      "0000FFE0-0000-1000-8000-00805F9B34FB",
      [characteristic("0000FFE1-0000-1000-8000-00805F9B34FB", [.writeWithoutResponse, .notify])])

    let selection = BLEEndpointSelector.select(services: [ffe])
    XCTAssertEqual(selection?.profile, .ffeFamily)
    XCTAssertEqual(selection?.outbound, "FFE1")
    XCTAssertEqual(selection?.inbound, "FFE1")
    XCTAssertEqual(selection?.writeMode, .withoutResponse)
    XCTAssertEqual(selection?.inboundDelivery, .notify)
  }

  func testTheFfeVariantWhereFfe2Notifies() {
    // "(sometimes FFE2)". Same service, same family, the directions split across two
    // characteristics — and FFE1, which is first in both candidate lists, must not be
    // chosen for the direction it cannot serve.
    let ffe = service(
      "FFE0",
      [
        characteristic("FFE1", [.write]),
        characteristic("FFE2", [.notify]),
      ])

    let selection = BLEEndpointSelector.select(services: [ffe])
    XCTAssertEqual(selection?.profile, .ffeFamily)
    XCTAssertEqual(selection?.outbound, "FFE1")
    XCTAssertEqual(selection?.inbound, "FFE2")
    XCTAssertEqual(selection?.writeMode, .withResponse)
  }

  func testTheFfeVariantWhereFfe2WritesAndFfe1Notifies() {
    // The inverted wiring, which exists in the field and would be missed by anything that
    // assumed FFE1 is always the TX.
    let ffe = service(
      "FFE0",
      [
        characteristic("FFE1", [.notify]),
        characteristic("FFE2", [.writeWithoutResponse]),
      ])

    let selection = BLEEndpointSelector.select(services: [ffe])
    XCTAssertEqual(selection?.outbound, "FFE2")
    XCTAssertEqual(selection?.inbound, "FFE1")
    XCTAssertEqual(selection?.inboundDelivery, .notify)
  }

  func testAnIndicateOnlyInboundCharacteristicIsAccepted() {
    // §4: "inbound: Notify **or** Indicate". Indicate is acknowledged notification, and a
    // module that uses it is not a module that cannot answer.
    let ffe = service(
      "FFE0",
      [
        characteristic("FFE1", [.writeWithoutResponse]),
        characteristic("FFE2", [.indicate]),
      ])
    XCTAssertEqual(BLEEndpointSelector.select(services: [ffe])?.inboundDelivery, .indicate)
  }

  // MARK: - Step (3): properties, not UUIDs

  func testAUuidMatchWithTheWrongPropertiesIsRejectedAndTheProbeFallsThrough() {
    // The whole reason §4 step (3) exists. This peripheral publishes the Microchip service
    // with both documented characteristic UUIDs — and the TX is read-only and the RX
    // notifies nothing. Matching on UUIDs alone would bind a receipt to a characteristic
    // that cannot carry a byte, and the failure mode is a print job that reports success
    // and produces nothing.
    let counterfeitMicrochip = service(
      "49535343-FE7D-4AE5-8FA9-9FAFD205E455",
      [
        characteristic("49535343-8841-43F4-A8D4-ECBE34729BB3", [.read]),
        characteristic("49535343-1E4D-4BD9-BA61-23C647249616", [.read]),
      ])

    let selection = BLEEndpointSelector.select(services: [counterfeitMicrochip, nordic])
    XCTAssertEqual(
      selection?.profile, .nordicUART, "a near-miss must not stop the probe at the near-miss")
    XCTAssertEqual(selection?.outbound, "6E400002-B5A3-F393-E0A9-E50E24DCCA9E")
  }

  func testAHalfUsableFamilyIsRejectedRatherThanDrivenWriteOnly() {
    // Outbound fine, nothing inbound. A write-only link can print and can never confirm, so
    // every job over it would be graded on the transport having accepted bytes. Falling
    // through to a family that can answer is strictly better; selecting nothing is better
    // than pretending.
    let mute = service(
      "6E400001-B5A3-F393-E0A9-E50E24DCCA9E",
      [characteristic("6E400002-B5A3-F393-E0A9-E50E24DCCA9E", [.write])])

    XCTAssertNil(BLEEndpointSelector.select(services: [mute]))
  }

  func testASecondInstanceOfTheSameServiceIsStillProbed() {
    // Peripherals do publish a service UUID twice — a duplicated firmware entry, or a real
    // second instance. Stopping at the first would fail a device that works.
    let empty = service("FFE0", [characteristic("FFE1", [.read])])
    let usable = service("FFE0", [characteristic("FFE1", [.writeWithoutResponse, .notify])])

    XCTAssertEqual(BLEEndpointSelector.select(services: [empty, usable])?.profile, .ffeFamily)
  }

  func testAPeripheralWithNothingSuitableSelectsNothing() {
    // A battery service and a device-information service. There is no BLE printer profile
    // to fall back on, so the honest answer is that this peripheral cannot print.
    let battery = service("180F", [characteristic("2A19", [.read, .notify])])
    let information = service("180A", [characteristic("2A29", [.read])])

    XCTAssertNil(BLEEndpointSelector.select(services: [battery, information, genericAccess]))
    XCTAssertNil(BLEEndpointSelector.select(services: []))
  }

  // MARK: - Write mode

  func testWriteWithResponseIsThePreferenceAndWriteCommandIsOptIn() {
    // Both properties present. With response costs a connection interval per chunk and buys
    // a report when a chunk is lost; without response is faster and silent about loss, which
    // for a receipt is the wrong default and a legitimate opt-in.
    XCTAssertEqual(BLEEndpointSelector.select(services: [nordic])?.writeMode, .withResponse)
    XCTAssertEqual(
      BLEEndpointSelector.select(
        services: [nordic],
        configuration: BLEEndpointConfiguration(prefersWriteWithoutResponse: true))?.writeMode,
      .withoutResponse)
  }

  func testTheSelectionIsLoggableInOneLine() throws {
    // "Which module did we decide this printer was?" is the first question asked about a
    // printer that half-worked, and it is unanswerable after the fact unless it was written
    // down at the time.
    let selection = try XCTUnwrap(BLEEndpointSelector.select(services: [microchip]))
    let description = selection.logDescription
    XCTAssertTrue(description.contains("Microchip Transparent UART"), description)
    XCTAssertTrue(description.contains("49535343-8841-43F4-A8D4-ECBE34729BB3"), description)
    XCTAssertTrue(description.contains("withResponse"), description)
    XCTAssertTrue(description.contains("notify"), description)
  }

  // MARK: - UUID comparison

  func testUuidsCompareCaseInsensitivelyAndAcrossTheShortAndLongForms() {
    // CoreBluetooth reports SIG-assigned UUIDs in the short form and vendor UUIDs in the
    // long one, and vendor documentation is written in whichever case the vendor felt like.
    // A comparison that missed either equivalence would reject the commonest module on the
    // market while looking, in a log, exactly like a printer that was not there.
    XCTAssertEqual(GATTUUID("FFE0"), GATTUUID("0000ffe0-0000-1000-8000-00805f9b34fb"))
    XCTAssertEqual(GATTUUID("ffe1"), GATTUUID("0000FFE1-0000-1000-8000-00805F9B34FB"))
    XCTAssertEqual(
      GATTUUID("6e400001-b5a3-f393-e0a9-e50e24dcca9e"),
      GATTUUID("6E400001-B5A3-F393-E0A9-E50E24DCCA9E"))
    XCTAssertNotEqual(GATTUUID("FFE1"), GATTUUID("FFE2"))
    XCTAssertEqual(GATTUUID("FFE0").shortForm, "FFE0")
    XCTAssertNil(GATTUUID("6E400001-B5A3-F393-E0A9-E50E24DCCA9E").shortForm)
    // Not a UUID at all: kept verbatim rather than coerced onto something it resembles.
    XCTAssertNotEqual(GATTUUID("not-a-uuid"), GATTUUID("FFE0"))
  }

  func testTheShortFormIsAcceptedInDescriptorsAndInConfiguration() {
    // The same equivalence, one layer up: a device that reports short-form UUIDs must match
    // a configuration written in long form, and the other way round.
    let ffe = service("FFE0", [characteristic("FFE1", [.write, .notify])])
    let selection = BLEEndpointSelector.select(
      services: [ffe],
      configuration: BLEEndpointConfiguration(
        service: "0000ffe0-0000-1000-8000-00805f9b34fb",
        outboundCharacteristic: "0000ffe1-0000-1000-8000-00805f9b34fb",
        inboundCharacteristic: "0000ffe1-0000-1000-8000-00805f9b34fb"))
    XCTAssertEqual(selection?.profile, .configured)
    XCTAssertEqual(selection?.outbound, "FFE1")
  }

  // MARK: - Chunking

  func testChunkingFollowsTheNegotiatedLengthAndNeverAssumesTwenty() {
    // §4 step (4), "never a fixed 20 bytes". 20 is ATT_MTU-3 for the 23-byte default, and on
    // any modern peripheral it throws away nine tenths of the throughput.
    let negotiated = BLEChunking.ranges(byteCount: 500, maximumWriteLength: 182)
    XCTAssertEqual(negotiated.count, 3)
    XCTAssertEqual(negotiated.first?.count, 182)
    XCTAssertNotEqual(negotiated.first?.count, 20)
    XCTAssertEqual(negotiated.last, 364..<500)
    XCTAssertEqual(negotiated.reduce(0) { $0 + $1.count }, 500)
    // Contiguous and in order: a receipt reassembled out of order is not a receipt.
    XCTAssertEqual(negotiated.map(\.lowerBound), [0, 182, 364])

    // The other direction: a stack that really did negotiate the default must not be
    // written to in 182-byte values it would truncate.
    XCTAssertEqual(BLEChunking.ranges(byteCount: 45, maximumWriteLength: 20).count, 3)
    XCTAssertEqual(BLEChunking.ranges(byteCount: 45, maximumWriteLength: 20).last, 40..<45)

    // Exact multiples produce no empty trailing range.
    XCTAssertEqual(BLEChunking.ranges(byteCount: 400, maximumWriteLength: 100).count, 4)
    // A payload smaller than one write is one write.
    XCTAssertEqual(BLEChunking.ranges(byteCount: 7, maximumWriteLength: 512), [0..<7])
    XCTAssertTrue(BLEChunking.ranges(byteCount: 0, maximumWriteLength: 182).isEmpty)
    // A stack reporting a nonsensical limit is clamped rather than allowed to loop forever
    // or to drop the job.
    XCTAssertEqual(BLEChunking.ranges(byteCount: 3, maximumWriteLength: 0), [0..<1, 1..<2, 2..<3])
    XCTAssertEqual(BLEChunking.ranges(byteCount: 2, maximumWriteLength: -5).count, 2)
  }

  // MARK: - Backpressure

  func testTheWriteGateBlocksUntilTheStackSaysThereIsRoom() {
    // §4 step (5). The writer parks; the delegate queue opens the gate; the writer resumes.
    let gate = BLEWriteGate()
    XCTAssertFalse(gate.waitUntilOpen(timeout: 0.05), "a closed gate must not let a write through")

    let opened = expectation(description: "gate opened from another thread")
    DispatchQueue.global().asyncAfter(deadline: .now() + 0.05) {
      gate.open()
      opened.fulfill()
    }
    XCTAssertTrue(gate.waitUntilOpen(timeout: 5))
    wait(for: [opened], timeout: 5)
  }

  func testASignalThatArrivesBeforeTheWaitIsNotLost() {
    // The lost-wakeup race, and the reason this is a condition variable and not a flag:
    // `peripheralIsReady` fires on CoreBluetooth's queue whenever it likes, including
    // between the writer's capacity check and its wait.
    let gate = BLEWriteGate()
    gate.open()
    XCTAssertTrue(gate.waitUntilOpen(timeout: 0.05))
    // One-shot: the stack's signal means room for *a* write, not room from now on.
    XCTAssertFalse(gate.waitUntilOpen(timeout: 0.05))
  }

  func testInvalidatingTheGateReleasesAWaiterImmediatelyAndResetMakesItUsableAgain() {
    // A link that drops must not leave the core's worker parked for the whole budget
    // waiting for room in the queue of a peripheral that is gone.
    let gate = BLEWriteGate()
    gate.invalidate()
    XCTAssertFalse(gate.waitUntilOpen(timeout: 5))

    // The core builds a fresh internal transport after a drop and calls back into the same
    // object, so an invalidated gate has to come back or the reconnection writes nothing.
    gate.reset()
    gate.open()
    XCTAssertTrue(gate.waitUntilOpen(timeout: 0.05))
  }

  // MARK: - Status framing

  func testStatusFramingIsOffUnlessDocumentedOrProbed() {
    // §4 step (6). A generic BLE-UART bridge documents nothing about where a notification
    // boundary falls, so a block-level handshake built on it stalls forever on a frame it
    // will never recognise — on a device that was working perfectly.
    XCTAssertEqual(BLEEndpointConfiguration().statusFraming, .disabled)
    XCTAssertFalse(BLEStatusFraming.disabled.pacesOnInboundFrames)
    XCTAssertTrue(BLEStatusFraming.documented.pacesOnInboundFrames)
    XCTAssertTrue(BLEStatusFraming.probed.pacesOnInboundFrames)
    // Not derived from a successful selection: recognising a module says nothing about how
    // it frames what comes back.
    XCTAssertEqual(
      BLEEndpointConfiguration(service: "FFE0").statusFraming, .disabled)
  }

  // MARK: - Vendor facts

  func testTheMfiProtocolStringsAreExactlyTheDocumentedOnesAndCitizenHasNone() {
    // §4's vendor table. iOS matches these verbatim against
    // UISupportedExternalAccessoryProtocols and against the accessory's own certification,
    // so a character out of place does not fail loudly — it simply never opens a session,
    // which on a support call is indistinguishable from a printer that is switched off.
    XCTAssertEqual(BluetoothVendorFacts.epson.mfiProtocol.value, "com.epson.escpos")
    XCTAssertEqual(BluetoothVendorFacts.star.mfiProtocol.value, "jp.star-m.starpro")
    XCTAssertEqual(BluetoothVendorFacts.bixolon.mfiProtocol.value, "com.bixolon.protocol")

    // Absent by policy, not by omission. Citizen's protocol name is vendor-gated — issued
    // only through MFi registration and approval — so there is no string to record and a
    // plausible `com.citizen.*` invention would be a guess shipped as a fact.
    XCTAssertEqual(BluetoothVendorFacts.citizen.mfiProtocol, .vendorGated)
    XCTAssertNil(BluetoothVendorFacts.citizen.mfiProtocol.value)
    XCTAssertFalse(BluetoothVendorFacts.citizen.mfiProtocol.isPublished)
    XCTAssertTrue(BluetoothVendorFacts.citizen.note.contains("MFi"))

    XCTAssertEqual(BluetoothVendorFacts.all.count, 4)
    XCTAssertEqual(BluetoothVendorFacts.facts(vendor: "epson")?.mfiProtocol.value, "com.epson.escpos")
    XCTAssertNil(BluetoothVendorFacts.facts(vendor: "Rongta"))
    XCTAssertEqual(BluetoothCapabilities.vendorFacts.count, 4)
  }

  func testNoVendorInTheTablePublishesRawGattAndNoneIsAutoSelectable() {
    // §4: "Epson/Star/Bixolon BLE stay sdkRequired/profileUnknown — never silently mapped
    // onto FFE1." The guarantee is structural rather than a rule somebody has to remember:
    // the probe order contains three generic families and no vendor entry at all, so the
    // only way an Epson reaches a generic profile is by genuinely answering a generic probe.
    for facts in BluetoothVendorFacts.all {
      XCTAssertFalse(facts.publishesRawGATT, facts.vendor)
      XCTAssertFalse(facts.bleProfileStatus.permitsRawGATT, facts.vendor)
      XCTAssertNotEqual(facts.bleProfileStatus, .genericUARTProbe, facts.vendor)
      XCTAssertFalse(facts.note.isEmpty, facts.vendor)
    }
    XCTAssertEqual(BluetoothVendorFacts.epson.bleProfileStatus, .sdkRequired)
    XCTAssertEqual(BluetoothVendorFacts.star.bleProfileStatus, .sdkRequired)
    XCTAssertEqual(BluetoothVendorFacts.bixolon.bleProfileStatus, .profileUnknown)
    XCTAssertEqual(BluetoothVendorFacts.citizen.bleProfileStatus, .profileUnknown)

    // Nothing in the selection core names a vendor, and the only identity that is not one
    // of the three families is the one the caller supplies itself.
    let identities = Set(BLEUARTProfile.probeOrder.map(\.identity))
    XCTAssertEqual(identities, [.microchipTransparentUART, .nordicUART, .ffeFamily])
    XCTAssertFalse(identities.contains(.configured))
    for profile in BLEUARTProfile.probeOrder {
      XCTAssertFalse(profile.identity.displayName.isEmpty)
    }
  }
}
