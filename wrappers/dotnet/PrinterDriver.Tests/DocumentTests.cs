using System.Text.Json.Nodes;

namespace PrinterDriver.Tests;

/// <summary>
/// M19 — the receipt DSL through the wrapper (docs/receipt-dsl.md).
/// </summary>
/// <remarks>
/// End to end over the real engine, like every other suite here: the template is parsed,
/// bound and rendered by the C++ core, and the bytes are the bytes the scripted device
/// received. Nothing about the DSL is re-implemented in C#, so what these prove is that
/// the .NET surface reaches the real one.
/// </remarks>
public sealed class DocumentTests
{
    /// <summary>
    /// A template with an <c>each</c> loop and the built-in <c>upper</c> formatter, plus a
    /// <c>meta</c> the job has to honour.
    /// </summary>
    private const string OrderTemplate = """
        { "v": 1, "template": true,
          "meta": { "cut": "full", "margins": { "topDots": 24 } },
          "blocks": [
            { "text": "{{venue.name|upper}}" },
            { "each": "order.items",
              "block": { "text": "{{qty}}x {{name|upper}}" } } ] }
        """;

    private const string OrderModel = """
        { "venue": { "name": "my restaurant" },
          "order": { "items": [ { "qty": 2, "name": "pilsner" },
                                { "qty": 1, "name": "goulash" } ] } }
        """;

    [Fact]
    public async Task PrintDocumentBindsATemplateAndPrintsItThroughTheOrdinaryEngine()
    {
        using var driver = TestDriver.Open();
        var printer = driver.AddScripted("ok", "dotnet-doc");

        var job = printer.PrintDocument(OrderTemplate, OrderModel,
                                        new JobOptions(Key: "order-7F3A"));
        Assert.Empty(job.RenderReport);
        Assert.Equal("order-7F3A", job.Key);

        // A template job is an ordinary job: it earns exactly what the fence earns.
        var result = await job.GetResultAsync(TestTimeout.Token);
        var done = Assert.IsType<JobResult.Done>(result);
        Assert.Equal(ConfidenceLevel.CutFaultFree, done.Confidence);
        Assert.Equal(ConfidenceGrade.AJobLevelConfirmation, done.Grade);
        Assert.Equal("GS(H) fn48", done.Method);

        // The formatter ran, the loop repeated in model order, and no placeholder survived.
        Assert.Equal(1, TestNative.pd_test_received_contains(printer.Handle, "MY RESTAURANT"));
        Assert.Equal(1, TestNative.pd_test_received_contains(printer.Handle, "2x PILSNER"));
        Assert.Equal(1, TestNative.pd_test_received_contains(printer.Handle, "1x GOULASH"));
        Assert.Equal(0, TestNative.pd_test_received_contains(printer.Handle, "{{"));
        Assert.Equal(1u, (uint)TestNative.pd_test_cuts(printer.Handle));

        // Rule 2 of the idempotency contract reaches this entry point too.
        var again = printer.PrintDocument(OrderTemplate, OrderModel,
                                          new JobOptions(Key: "order-7F3A"));
        Assert.Same(job, again);
        Assert.Equal(1u, (uint)TestNative.pd_test_cuts(printer.Handle));
    }

    [Fact]
    public async Task PrintDocumentAcceptsAJsonNode()
    {
        using var driver = TestDriver.Open();
        var printer = driver.AddScripted("ok", "dotnet-doc-node");

        var document = JsonNode.Parse("""
            {"v":1,"template":true,"blocks":[{"text":"{{title|upper}}"}]}
            """)!;
        var model = JsonNode.Parse("""{"title":"table 4"}""")!;

        var job = printer.PrintDocument(document, model);
        Assert.IsType<JobResult.Done>(await job.GetResultAsync(TestTimeout.Token));
        Assert.Equal(1, TestNative.pd_test_received_contains(printer.Handle, "TABLE 4"));
    }

    [Fact]
    public void RenderDocumentReportsADeclaredDegradationAndPrintsNothing()
    {
        using var driver = TestDriver.Open();
        var printer = driver.AddScripted("no-barcode", "dotnet-no-gs-k");

        var rendered = printer.RenderDocument("""
            { "v": 1, "blocks": [ { "text": "WIDGET CO" },
                                  { "barcode": "12345670", "symbology": "code128" } ] }
            """);

        // The text still rendered: a declared degradation is not a failure.
        Assert.NotEmpty(rendered.Bytes);

        var entry = Assert.Single(rendered.Report);
        Assert.Equal(ReportKind.UnsupportedBlock, entry.Kind);
        Assert.Equal("unsupportedBlock", entry.Kind.AbiName());
        Assert.Equal("blocks[1]", entry.Block);
        Assert.Contains("code128", entry.Requested);
        Assert.Equal("omitted", entry.Delivered);
        Assert.Equal(RenderPath.NotRendered, entry.Path);
        Assert.Equal("notRendered", entry.Path.AbiName());
        Assert.NotEmpty(entry.Note);

        // Rendering is not printing.
        Assert.Equal(0u, (uint)TestNative.pd_test_print_data_bytes(printer.Handle));
        Assert.Equal(0u, (uint)TestNative.pd_test_cuts(printer.Handle));
    }

    [Fact]
    public async Task RenderDocumentReturnsTheDocumentsOwnMeta()
    {
        using var driver = TestDriver.Open();
        var printer = driver.AddScripted("ok", "dotnet-meta");

        var rendered = await printer.RenderDocumentAsync(
            OrderTemplate, OrderModel, cancellationToken: TestTimeout.Token);
        Assert.Equal(CutSetting.Full, rendered.Meta.Cut);
        Assert.Equal(24u, rendered.Meta.TopFeedDots);
        Assert.Equal(0u, rendered.Meta.BottomFeedDots);
        Assert.Empty(rendered.Report);
    }

    [Fact]
    public void MalformedDocumentsAreRefusedAndNothingIsPrinted()
    {
        using var driver = TestDriver.Open();
        var printer = driver.AddScripted("ok", "dotnet-doc-bad");

        Assert.Throws<PrinterDriverException>(() => printer.RenderDocument("this is not json"));
        // A template with no model is refused rather than printed: a receipt full of
        // {{order.total}} is worse than no receipt, because it looks like one.
        Assert.Throws<PrinterDriverException>(() => printer.RenderDocument(OrderTemplate));
        Assert.Throws<PrinterDriverException>(() => printer.PrintDocument("this is not json"));

        Assert.Equal(0u, (uint)TestNative.pd_test_print_data_bytes(printer.Handle));
        Assert.Equal(0u, (uint)TestNative.pd_test_cuts(printer.Handle));
    }

    /// <summary>
    /// docs/api.md §17.1: <c>RegisterFormatter</c> was accepted and stored, and nothing a
    /// wrapper could call consulted it. This is that call site.
    /// </summary>
    [Fact]
    public async Task ARegisteredFormatterFiresThroughThisPath()
    {
        const string template =
            """{"v":1,"template":true,"blocks":[{"text":"{{item|acme.stars}}"}]}""";
        const string model = """{"item":"tip"}""";

        using var driver = TestDriver.Open();

        // The control: no registration, so the name is unknown and the report says so.
        var control = driver.AddScripted("ok", "dotnet-fmt-control");
        Assert.Equal(ReportKind.UnknownFormatter,
                     control.RenderDocument(template, model).Report[0].Kind);

        var calls = 0;
        driver.RegisterFormatter(new TemplateFormatter
        {
            Name = "acme.stars",
            Format = (value, _, _) =>
            {
                Interlocked.Increment(ref calls);
                return $"***{value}***";
            },
        });

        var printer = driver.AddScripted("ok", "dotnet-fmt");
        Assert.Empty(printer.RenderDocument(template, model).Report);
        Assert.True(calls > 0);

        // And on paper, not only in a preview.
        var job = printer.PrintDocument(template, model);
        Assert.IsType<JobResult.Done>(await job.GetResultAsync(TestTimeout.Token));
        Assert.Equal(1, TestNative.pd_test_received_contains(printer.Handle, "***tip***"));
    }
}
