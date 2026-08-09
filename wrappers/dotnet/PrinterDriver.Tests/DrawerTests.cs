namespace PrinterDriver.Tests;

/// <summary>
/// M14 — the cash drawer, through the managed wrapper (docs/cash-drawer.md).
/// </summary>
/// <remarks>
/// The wrapper holds no drawer logic: the sequence, the refusals and the polarity all live
/// in the core. What is under test here is that none of it is lost on the way across —
/// that a refusal is still a refusal, that <c>KickSentUnverified</c> does not quietly
/// become a success, and that an uncalibrated switch still reports a level rather than a
/// direction.
/// </remarks>
public sealed class DrawerTests
{
    [Fact]
    public void AVerifiedOpenIsReportedOnlyWhenTheSwitchMoves()
    {
        using var driver = TestDriver.Open();
        var till = driver.AddScripted("drawer", "till");

        var caps = till.DrawerCapabilities;
        Assert.True(caps.Present);
        Assert.True(caps.Kickable);
        Assert.Equal(DrawerPortStandard.Epson24V6P6C, caps.PortStandard);
        Assert.Equal(DrawerKickMethod.EpsonEscP, caps.KickMethod);
        Assert.Equal(DrawerStatusMethod.GsR2, caps.StatusMethod);
        Assert.Equal(24, caps.Voltage);
        Assert.Equal(1000, caps.MaxCurrentMa);
        Assert.Equal(2, caps.ChannelCount);
        // The single fact that separates this arrangement from Star's.
        Assert.Equal(3, caps.SensorPin);
        Assert.Equal(Provenance.Documented, caps.ElectricalProvenance);
        Assert.Equal(Provenance.Documented, caps.CommandsProvenance);

        var result = till.OpenDrawer();
        Assert.Equal(DrawerState.Closed, result.PreviousState);
        Assert.Equal(DrawerState.OpenVerified, result.State);
        Assert.True(result.Verified);
        Assert.Equal(1, result.Channel);
        Assert.Equal(200, result.PulseMs);
        Assert.Equal((nuint)1, TestNative.pd_test_drawer_kicks(till.Handle));
        Assert.Equal(1, TestNative.pd_test_drawer_is_open(till.Handle));

        // Step 1 of the sequence: a drawer that is already out is never pulsed again.
        var again = till.OpenDrawer(channel: 1, pulseMs: 120);
        Assert.Equal(DrawerState.Open, again.State);
        Assert.Equal(0, again.PulseMs);
        Assert.False(again.Verified);
        Assert.Equal((nuint)1, TestNative.pd_test_drawer_kicks(till.Handle));
    }

    [Fact]
    public void ALockedDrawerIsFailedToOpenAndNotASuccess()
    {
        using var driver = TestDriver.Open();
        var till = driver.AddScripted("drawer-locked", "locked");

        var result = till.OpenDrawer(channel: 2, pulseMs: 120);
        Assert.Equal(DrawerState.Closed, result.PreviousState);
        Assert.Equal(DrawerState.FailedToOpen, result.State);
        Assert.False(result.Verified);
        Assert.Equal(2, result.Channel);
        Assert.Equal(120, result.PulseMs);
        // The pulse was real; the drawer was not.
        Assert.Equal((nuint)1, TestNative.pd_test_drawer_kicks(till.Handle));
        Assert.Equal(0, TestNative.pd_test_drawer_is_open(till.Handle));
    }

    [Fact]
    public void AnUnclassifiedPortIsRefusedWithoutWritingAByte()
    {
        using var driver = TestDriver.Open();
        var till = driver.AddScripted("drawer-unknown-port", "unclassified");

        var caps = till.DrawerCapabilities;
        Assert.Equal(DrawerPortStandard.Unknown, caps.PortStandard);
        Assert.False(caps.Kickable);

        var result = till.OpenDrawer();
        Assert.Equal(DrawerState.Unknown, result.State);
        Assert.Equal(0, result.PulseMs);
        // RJ11/RJ12-looking drawer connectors are not a universal electrical standard, and
        // an unclassified port is never energised.
        Assert.Equal((nuint)0, TestNative.pd_test_drawer_kicks(till.Handle));

        // Reading the switch is still safe on the same hardware: it asks a question and
        // energises nothing, which is what makes it the probe's non-destructive half.
        var reading = till.ReadDrawerSensor(500);
        Assert.True(reading.Available);
        Assert.True(reading.Answered);
        Assert.Equal((nuint)0, TestNative.pd_test_drawer_kicks(till.Handle));
    }

    [Fact]
    public void AnUncalibratedSwitchReportsALevelAndNotADirection()
    {
        using var driver = TestDriver.Open();
        var till = driver.AddScripted("drawer-uncalibrated", "uncalibrated");
        Assert.False(till.IsDrawerPolarityCalibrated);

        // The operator procedure, without the prompts.
        TestNative.pd_test_set_drawer_open(till.Handle, 0);
        var shut = till.ReadDrawerSensor(500);
        Assert.True(shut.Answered);
        Assert.True(shut.NeedsCalibration);
        Assert.Equal(false, shut.PinHigh);
        // Whether the line reads high or low when the drawer is open depends on the drawer
        // that is plugged in, so until it is measured there is no interpretation.
        Assert.Equal(DrawerState.Unknown, shut.State);

        TestNative.pd_test_set_drawer_open(till.Handle, 1);
        var open = till.ReadDrawerSensor(500);
        Assert.Equal(true, open.PinHigh);
        Assert.NotEqual(shut.PinHigh, open.PinHigh);
        Assert.Equal((nuint)0, TestNative.pd_test_drawer_kicks(till.Handle));

        // In-memory driver: the calibration applies to this process, and says so by
        // returning false rather than pretending it was persisted.
        Assert.False(till.CalibrateDrawerPolarity(open.PinHigh ?? true));
        Assert.True(till.IsDrawerPolarityCalibrated);
        Assert.True(till.DrawerHighMeansOpen);

        var after = till.ReadDrawerSensor(500);
        Assert.False(after.NeedsCalibration);
        Assert.Equal(DrawerState.Open, after.State);
    }

    [Fact]
    public void ALabelPrinterHasNoDrawerAndFiresNothing()
    {
        using var driver = TestDriver.Open();
        // Zebra speaks ZPL and CPCL, has no drawer port, and every job on it is refused
        // before a byte is written. The drawer call is the same refusal.
        var labels = driver.AddPrinterTcp(new TcpPrinterConfig(
            "192.0.2.60", PrinterId: "labels", ProfileId: "zebra_zq600_plus"));

        var caps = labels.DrawerCapabilities;
        Assert.False(caps.Present);
        Assert.Equal(DrawerKickMethod.Unsupported, caps.KickMethod);
        Assert.False(caps.Kickable);

        var result = labels.OpenDrawer();
        Assert.Equal(DrawerState.Unknown, result.State);
        Assert.Equal(0, result.PulseMs);
    }
}
