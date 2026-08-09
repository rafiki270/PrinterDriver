namespace PrinterDriver.Tests;

/// <summary>
/// M15 — self-test, auto-detection and LAN discovery through the managed wrapper
/// (docs/api.md §15).
/// </summary>
/// <remarks>
/// The wrapper holds no detection logic, so what is under test here is that none of it is
/// lost on the way across: that a Done at grade A is still a Done at grade A, that a
/// printless probe's refusal to promote a provenance survives the bridge, and that a sweep
/// which must not print still does not print.
/// </remarks>
public sealed class DetectionTests
{
    [Fact]
    public void SelfTestPrintsOneTicketAndReportsWhatItProved()
    {
        using var driver = TestDriver.Open();
        var bench = driver.AddScripted("ok", "bench");

        var result = bench.SelfTest();

        // The proof is the ordinary tri-state result of the ordinary engine.
        var done = Assert.IsType<JobResult.Done>(result.Result);
        Assert.Equal(ConfidenceLevel.CutFaultFree, done.Confidence);
        Assert.Equal(ConfidenceGrade.AJobLevelConfirmation, done.Grade);
        Assert.Equal(CompletionAuthority.PhysicalPrinter, done.Authority);
        Assert.Equal("GS(H) fn48", done.Method);

        Assert.StartsWith("selftest-", result.Key, StringComparison.Ordinal);
        Assert.NotNull(result.VerificationId);
        Assert.Equal(4, result.VerificationId!.Length);
        Assert.NotNull(result.Job);
        Assert.Contains(result.TicketLines, l => l.Contains("PRINTERDRIVER SELF-TEST", StringComparison.Ordinal));
        Assert.Contains(result.TicketLines, l => l.Contains("CHARSET", StringComparison.Ordinal));

        // The bytes the device actually received.
        Assert.Equal(1, TestNative.pd_test_received_contains(bench.Handle, "PRINTERDRIVER SELF-TEST"));
        Assert.Equal(1, TestNative.pd_test_received_contains(bench.Handle, "V:"));
        Assert.Equal((nuint)1, TestNative.pd_test_cuts(bench.Handle));

        // The detection report the paper carries.
        Assert.Equal("bench", result.Detection.Endpoint);
        Assert.Equal(CompletionMechanism.GsParenH, result.Detection.Completion.Mechanism);
        Assert.Equal(ConfidenceGrade.AJobLevelConfirmation, result.Detection.Completion.GradeCeiling);
        Assert.Equal(576u, result.Detection.Media.PrintableWidthDots);
        Assert.Equal(48u, result.Detection.Media.CharsPerLine);
        Assert.Empty(result.Detection.Degradations);
        Assert.Contains("GS(H) fn48", result.Detection.ProvenanceSummary, StringComparison.Ordinal);
        Assert.Equal(ProfileSelection.Documented, result.Detection.Selection);

        // A self-test is an ordinary job under an ordinary key: the same key twice
        // prints once.
        var again = bench.SelfTest(new SelfTestOptions { Key = "selftest-fixed" });
        Assert.Equal((nuint)2, TestNative.pd_test_cuts(bench.Handle));
        var third = bench.SelfTest(new SelfTestOptions { Key = "selftest-fixed" });
        Assert.Equal((nuint)2, TestNative.pd_test_cuts(bench.Handle));
        Assert.Same(again.Job, third.Job);
    }

    [Fact]
    public void AutoDetectClassifiesLoopbackListenersAndPrintsNothing()
    {
        using var answering = new ScriptedListener("ok");
        using var silent = new ScriptedListener("silent");
        using var gone = new ScriptedListener("ok");
        var refused = gone.Endpoint;
        gone.Stop(); // the port is now closed: a refusal, deterministically

        using var driver = TestDriver.Open();
        var options = new AutoDetectOptions
        {
            Endpoints = new[] { answering.Endpoint, silent.Endpoint, refused },
            ConnectTimeoutMs = 500,
            ResponseTimeoutMs = 150,
        };

        var streamed = new List<DetectedPrinter>();
        var found = driver.AutoDetect(options, streamed.Add);
        Assert.Equal(3, found.Count);
        Assert.Equal(3, streamed.Count);

        var talker = Assert.Single(found, p => p.Status == DetectionStatus.Answered);
        Assert.True(talker.IsPortOpen);
        Assert.False(talker.IsFromCache);
        Assert.Equal("TM-T88V", talker.Summary.Identity.Model);
        // GS I is a string the firmware chooses, and at least one family ships answering
        // as somebody else's model.
        Assert.False(talker.Summary.Identity.IsTrusted);
        Assert.Equal(CompletionMechanism.GsParenH, talker.Summary.Completion.Mechanism);
        Assert.Equal(ConfidenceGrade.AJobLevelConfirmation, talker.Summary.Completion.GradeCeiling);
        // The printless probe promotes the flag and not its provenance.
        Assert.Equal(Provenance.Unverified, talker.Summary.Completion.Provenance);
        Assert.Contains(talker.Summary.Degradations, l => l.Contains("empty buffer", StringComparison.Ordinal));
        Assert.NotEmpty(talker.DleEotHex);

        var quiet = Assert.Single(found, p => p.Status == DetectionStatus.Silent);
        Assert.True(quiet.IsPortOpen);
        Assert.Empty(quiet.DleEotHex);

        var dead = Assert.Single(found, p => p.Status == DetectionStatus.Unreachable);
        Assert.False(dead.IsPortOpen);
        Assert.Empty(dead.Summary.ProfileId);

        // The whole point: not one printable byte reached either live device.
        Assert.Equal((nuint)0, answering.PrintDataBytes);
        Assert.Equal((nuint)0, silent.PrintDataBytes);

        answering.Stop();
        silent.Stop();
    }

    [Fact]
    public async Task AutoDetectAsyncDeliversCandidatesAsTheyAreFound()
    {
        using var answering = new ScriptedListener("ok");
        using var driver = TestDriver.Open();
        var options = new AutoDetectOptions
        {
            Endpoints = new[] { answering.Endpoint },
            ConnectTimeoutMs = 500,
            ResponseTimeoutMs = 150,
        };

        var seen = new List<DetectedPrinter>();
        await foreach (var candidate in driver.AutoDetectAsync(options))
        {
            seen.Add(candidate);
        }
        Assert.Single(seen);
        Assert.Equal(DetectionStatus.Answered, seen[0].Status);
        Assert.Equal((nuint)0, answering.PrintDataBytes);
        answering.Stop();
    }

    [Fact]
    public void DiscoverSweepsALoopbackAddressAndWritesOnlyDleEot()
    {
        using var answering = new ScriptedListener("ok");
        using var driver = TestDriver.Open();

        var found = driver.Discover(new DiscoverOptions
        {
            SubnetCidr = "127.0.0.1/32",
            Port = answering.Port,
            ConnectTimeoutMs = 500,
            ResponseTimeoutMs = 300,
        });

        var device = Assert.Single(found);
        Assert.Equal("127.0.0.1", device.Ip);
        Assert.True(device.IsPortOpen);
        Assert.True(device.DidAnswer);
        // The scripted device's DLE EOT 1 answer: online, drawer pin high.
        Assert.Equal("16", device.DleEotHex);
        Assert.Equal((nuint)0, answering.PrintDataBytes);

        // A CIDR wider than /16 is a mistyped subnet, not a venue, and is refused.
        Assert.Throws<PrinterDriverException>(
            () => driver.Discover(new DiscoverOptions { SubnetCidr = "10.0.0.0/8" }));

        // And the local subnet is either a CIDR or an honest null.
        _ = driver.LocalSubnet;

        answering.Stop();
    }

    [Fact]
    public void TheDetectionEnumsMirrorTheCoreSpellings()
    {
        NativeFixture.Bind();
        Assert.Equal(
            Enum.GetValues<ProfileSelection>().Length,
            TestNative.pd_test_cpp_enum_count((int)BridgedEnum.ProfileSelection));
        Assert.Equal(
            Enum.GetValues<DetectionStatus>().Length,
            TestNative.pd_test_cpp_enum_count((int)BridgedEnum.DetectionStatus));

        foreach (var value in Enum.GetValues<ProfileSelection>())
        {
            Assert.Equal(
                value.ToString(),
                TestNative.ReadUtf8(TestNative.pd_test_cpp_enum_name(
                    (int)BridgedEnum.ProfileSelection, (int)value)));
        }
        foreach (var value in Enum.GetValues<DetectionStatus>())
        {
            Assert.Equal(
                value.ToString(),
                TestNative.ReadUtf8(TestNative.pd_test_cpp_enum_name(
                    (int)BridgedEnum.DetectionStatus, (int)value)));
        }
    }
}
