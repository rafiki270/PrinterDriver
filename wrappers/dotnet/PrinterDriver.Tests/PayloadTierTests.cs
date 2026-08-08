namespace PrinterDriver.Tests;

/// <summary>
/// All three payload tiers of docs/api.md §3, driven end to end.
/// </summary>
/// <remarks>
/// The raw tier is covered by <see cref="PrintJobTests"/>; these are the two that involve
/// real marshalling — an array of <c>pd_op</c> structs with UTF-8 strings hanging off it,
/// and a pinned pixel buffer. A layout mistake in either would not throw, it would send
/// the printer garbage, so each test asserts on what the device actually received.
/// </remarks>
public sealed class PayloadTierTests
{
    [Fact]
    public async Task DocumentTierReachesTheDeviceAsText()
    {
        using var driver = TestDriver.Open();
        var printer = driver.AddScripted("ok");

        var document = new Payload.Document(
        [
            DocumentOp.Align(Alignment.Center),
            DocumentOp.Bold(true),
            DocumentOp.Line("KITCHEN TICKET"),
            DocumentOp.Bold(false),
            DocumentOp.Align(Alignment.Left),
            DocumentOp.Line("2x MARGHERITA"),
            DocumentOp.Text("TABLE "),
            DocumentOp.Line("14"),
            DocumentOp.Feed(2),
        ], CodePage.PC437);

        var result = await printer.SendAsync(document, cancellationToken: TestTimeout.Token);

        Assert.IsType<JobResult.Done>(result);
        Assert.Equal(1, TestNative.pd_test_received_contains(printer.Handle, "KITCHEN TICKET"));
        Assert.Equal(1, TestNative.pd_test_received_contains(printer.Handle, "2x MARGHERITA"));
        // "TABLE " with no line break followed by "14" must arrive as one line, which is
        // only true if the op array's Kind field landed where the C side reads it.
        Assert.Equal(1, TestNative.pd_test_received_contains(printer.Handle, "TABLE 14"));
        Assert.Equal(1u, (uint)TestNative.pd_test_cuts(printer.Handle));
    }

    [Fact]
    public async Task EmptyDocumentIsStillAJobAndStillCuts()
    {
        using var driver = TestDriver.Open();
        var printer = driver.AddScripted("ok");

        var result = await printer.SendAsync(new Payload.Document([]),
                                             cancellationToken: TestTimeout.Token);

        Assert.IsType<JobResult.Done>(result);
        Assert.Equal(1u, (uint)TestNative.pd_test_cuts(printer.Handle));
    }

    [Fact]
    public async Task RasterTierIsImagedByTheCore()
    {
        using var driver = TestDriver.Open();
        var printer = driver.AddScripted("ok");

        // A 64x16 checkerboard, opaque. The core does the greyscale conversion, scaling,
        // binarization and banding; this only has to hand it well-formed RGBA8.
        const uint width = 64;
        const uint height = 16;
        var pixels = new byte[width * height * 4];
        for (var y = 0u; y < height; y++)
        {
            for (var x = 0u; x < width; x++)
            {
                var offset = ((y * width) + x) * 4;
                var dark = ((x / 8) + (y / 8)) % 2 == 0;
                pixels[offset + 0] = dark ? (byte)0 : (byte)255;
                pixels[offset + 1] = pixels[offset];
                pixels[offset + 2] = pixels[offset];
                pixels[offset + 3] = 255;
            }
        }

        var result = await printer.SendAsync(
            new Payload.Raster(pixels, width, height, Binarization: Binarization.FixedThreshold,
                               Threshold: 128),
            cancellationToken: TestTimeout.Token);

        Assert.IsType<JobResult.Done>(result);
        // GS v 0 (1D 76 30) is the raster bit-image command. The scripted device consumes
        // those blocks as commands rather than as print data, so what proves the pixels
        // were imaged rather than dropped is that the command reached the wire at all --
        // which it only can if Pixels, Width, Height and StrideBytes landed where the C
        // side reads them.
        Assert.Equal(1, TestNative.pd_test_received_contains(printer.Handle, "v0"));
        Assert.Equal(1u, (uint)TestNative.pd_test_cuts(printer.Handle));

        // And a payload the core rejects fails the call rather than printing nonsense.
        Assert.Throws<PrinterDriverException>(
            () => printer.Print(new Payload.Raster(new byte[4], Width: 0, Height: 0)));
    }

    [Fact]
    public async Task PayloadFromTextIsTheRawTier()
    {
        using var driver = TestDriver.Open();
        var printer = driver.AddScripted("ok");

        var payload = Payload.FromText("CONVENIENCE PATH");
        Assert.IsType<Payload.Raw>(payload);

        Assert.IsType<JobResult.Done>(
            await printer.SendAsync(payload, cancellationToken: TestTimeout.Token));
        Assert.Equal(1, TestNative.pd_test_received_contains(printer.Handle,
                                                             "CONVENIENCE PATH"));
    }

    [Fact]
    public void ThePayloadHierarchyIsClosedToThreeTiers()
    {
        var tiers = typeof(Payload)
            .GetNestedTypes(System.Reflection.BindingFlags.Public)
            .Where(t => t.IsSubclassOf(typeof(Payload)))
            .ToList();

        Assert.Equal(3, tiers.Count);
        Assert.All(tiers, t => Assert.True(t.IsSealed, $"{t.Name} must be sealed"));
    }
}
