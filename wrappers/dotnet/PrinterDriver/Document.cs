using System.Runtime.InteropServices;
using System.Text.Json.Nodes;

namespace PrinterDriver;

// M19 — the receipt DSL through the wrapper (docs/receipt-dsl.md, docs/api.md §3).
//
// No rendering logic lives here and no template engine. Which blocks a profile can draw,
// what a missing model path does, how `meta.cut` reaches a job: all of it is in the core
// behind pd_render_document and pd_print_document_json. What is here is the shape a .NET
// caller expects — records, a typed report instead of an opaque string, async, and a
// JsonNode overload for the JSON an application already has.

/// <summary>What kind of departure from the document was declared — <c>pd_report_kind</c>.</summary>
/// <remarks>
/// Every one of these RENDERS: the receipt still prints and the entry says what it lost.
/// A declared degradation is not a failure, but it is never silent either.
/// </remarks>
public enum ReportKind
{
    /// <summary><c>{{order.note}}</c> with no such path in the model.</summary>
    MissingPath = 0,

    /// <summary><c>{{v|frobnicate}}</c>.</summary>
    UnknownFormatter = 1,

    /// <summary><c>{{v|number:2}}</c> where <c>v</c> is an object.</summary>
    UnformattableValue = 2,

    /// <summary>
    /// An unterminated <c>{{</c>, an empty path — and the three hard failures that stop
    /// bytes being produced at all: JSON that is not JSON, a structure that is not a
    /// document, and a template submitted with no model.
    /// </summary>
    MalformedTemplate = 3,

    /// <summary>A style name no document defines.</summary>
    UnknownStyle = 4,

    /// <summary>An <c>extends</c> chain that loops.</summary>
    StyleCycle = 5,

    /// <summary>Italic, <c>rotate90</c>, a raster font on the hardware path.</summary>
    UnsupportedStyle = 6,

    /// <summary>A symbology this build cannot draw, or a block the profile has no hardware for.</summary>
    UnsupportedBlock = 7,

    /// <summary>An <c>image</c> reference with no asset supplied.</summary>
    MissingImage = 8,

    /// <summary>Content clipped to fit the media width.</summary>
    Truncated = 9,

    /// <summary><c>each</c> over a missing or non-array path.</summary>
    EmptyIteration = 10,

    /// <summary>An IANA zone name: this core ships no timezone database.</summary>
    UnsupportedTimezone = 11,

    /// <summary>A <c>raw</c> block carrying a cut or a status command — the core owns job termination.</summary>
    RawFramingRisk = 12,

    /// <summary>Everything else worth telling the operator.</summary>
    Note = 13,
}

/// <summary>How an entry's content reached the paper, if it did — <c>pd_render_path</c>.</summary>
public enum RenderPath
{
    /// <summary>Produced by ESC/POS commands.</summary>
    Hardware = 0,

    /// <summary>Produced as an image.</summary>
    Raster = 1,

    /// <summary>Requested, and deliberately absent from the output.</summary>
    NotRendered = 2,
}

/// <summary>One declared degradation — <c>pd_report_entry</c>.</summary>
/// <param name="Kind">What class of departure this is.</param>
/// <param name="Block">
/// Where it happened, as a path into the document: <c>blocks[3].cells[1]</c>,
/// <c>meta.tz</c>, or <c>document</c> for one that belongs to no block.
/// </param>
/// <param name="Requested">What the document asked for, in the words it is printed in.</param>
/// <param name="Delivered">What the paper got. Often <c>omitted</c>.</param>
/// <param name="Path">Which path produced it, or that nothing did.</param>
/// <param name="Note">Why, when the pair above does not say it all. Empty when it does.</param>
public sealed record ReportEntry(
    ReportKind Kind,
    string Block,
    string Requested,
    string Delivered,
    RenderPath Path,
    string Note)
{
    /// <summary>One line, for a log or an assertion message.</summary>
    /// <returns>The entry as text.</returns>
    public override string ToString() =>
        $"{Block}  {Kind.AbiName()}: requested \"{Requested}\", delivered " +
        $"\"{Delivered}\" [{Path.AbiName()}]" + (Note.Length == 0 ? "" : $" - {Note}");
}

/// <summary>
/// What a document asks the engine for, read back from its <c>meta</c>
/// (docs/receipt-dsl.md "Cut control" and "Margins").
/// </summary>
/// <remarks>
/// Reported by <see cref="RenderedDocument"/> and applied by
/// <see cref="Printer.PrintDocument(string, string?, JobOptions?)"/> under that document's
/// precedence: what the caller put in <see cref="JobOptions"/> wins, this fills in what
/// the caller left alone, and the printer's profile answers what neither said.
/// </remarks>
/// <param name="Cut">Null when the document asked for no particular cut.</param>
/// <param name="TopFeedDots">Blank paper before the first content line; 0 when unstated.</param>
/// <param name="BottomFeedDots">
/// The <em>total</em> whitespace between the last content and the cut. The engine feeds
/// <c>max(the profile's blade clearance, this)</c>, so no document can clip its own trailer.
/// </param>
public sealed record DocumentMeta(CutSetting? Cut, uint TopFeedDots, uint BottomFeedDots);

/// <summary>
/// A rendered receipt-DSL document: the bytes a printer would receive, and everything that
/// was declared along the way.
/// </summary>
/// <param name="Bytes">
/// The ESC/POS the renderer produced. Never the whole job: the engine adds its own
/// initialise, trailing feed, cut and completion fence around this.
/// </param>
/// <param name="CodePage">The code page the text was transliterated against.</param>
/// <param name="Meta">What the document's own <c>meta</c> asks for.</param>
/// <param name="Report">Empty when every block rendered exactly as written.</param>
public sealed record RenderedDocument(
    byte[] Bytes,
    CodePage CodePage,
    DocumentMeta Meta,
    IReadOnlyList<ReportEntry> Report);

/// <summary>Options for <see cref="Printer.RenderDocument(string, string?, RenderOptions?)"/>.</summary>
/// <param name="WidthDots">
/// 0 lays the document out for this printer's own configured width, which is what the
/// engine rasterizes to. Anything else previews a different media width.
/// </param>
/// <param name="CutClearanceDots">
/// Extra whitespace before a <em>mid-document</em> <c>cut</c> block. The renderer feeds
/// <c>max(the profile's blade clearance, this)</c>: more is always granted, less never.
/// </param>
/// <param name="MaxRowsPerBand">0 means 1024 — the Epson tall-image split.</param>
/// <param name="Locale">Overrides the document's own <c>meta.locale</c>; null defers to it.</param>
/// <param name="Currency">Overrides <c>meta.currency</c>.</param>
/// <param name="TimeZone">
/// Overrides <c>meta.tz</c>. A fixed offset such as <c>+02:00</c>; an IANA name is
/// reported as a <see cref="ReportKind.UnsupportedTimezone"/> degradation, because a core
/// that ships no dependencies ships no timezone database.
/// </param>
public sealed record RenderOptions(
    uint WidthDots = 0,
    ushort CutClearanceDots = 0,
    uint MaxRowsPerBand = 0,
    string? Locale = null,
    string? Currency = null,
    string? TimeZone = null);

public sealed partial class Printer
{
    /// <summary>
    /// Renders a receipt-DSL document against this printer's capability profile.
    /// </summary>
    /// <remarks>
    /// <b>Nothing prints and no job exists.</b> This is the preview path: the bytes a
    /// printer would receive, plus every degradation the profile forced, before any paper
    /// is committed.
    /// </remarks>
    /// <param name="documentJson">A document or a template (docs/receipt-dsl.md).</param>
    /// <param name="modelJson">
    /// The parameter model bound into a template — <c>{{path.to.value}}</c>, <c>each</c>,
    /// <c>if</c>. Null for a document that carries its own content.
    /// </param>
    /// <param name="options">Media and locale overrides; null takes the defaults.</param>
    /// <returns>The bytes, the document's meta, and the render report.</returns>
    /// <exception cref="PrinterDriverException">
    /// The JSON is not JSON, the structure is not a document, or a template arrived with
    /// no model — a receipt full of <c>{{order.total}}</c> is worse than no receipt,
    /// because it looks like one. Everything softer is a <see cref="ReportEntry"/> and
    /// still renders.
    /// </exception>
    public RenderedDocument RenderDocument(string documentJson, string? modelJson = null,
                                           RenderOptions? options = null)
    {
        ArgumentException.ThrowIfNullOrEmpty(documentJson);
        options ??= new RenderOptions();

        var locale = Marshal.StringToCoTaskMemUTF8(options.Locale ?? string.Empty);
        var currency = Marshal.StringToCoTaskMemUTF8(options.Currency ?? string.Empty);
        var timeZone = Marshal.StringToCoTaskMemUTF8(options.TimeZone ?? string.Empty);
        try
        {
            var native = new PdRenderOptions
            {
                WidthDots = options.WidthDots,
                CutClearanceDots = options.CutClearanceDots,
                MaxRowsPerBand = options.MaxRowsPerBand,
                Locale = locale,
                Currency = currency,
                TimeZone = timeZone,
            };
            var ok = NativeMethods.pd_render_document(
                _driver.Handle, _handle, documentJson, modelJson, in native, out var result);
            if (ok == 0)
            {
                // The report is read before the throw: a caller that wants to show what
                // went wrong gets the same list it would show for a degradation.
                throw new PrinterDriverException(
                    _driver.LastError() + "\n" + string.Join('\n', _driver.RenderReport()));
            }

            var bytes = new byte[(int)result.Size];
            if (result.Size != 0 && result.Bytes != 0)
            {
                Marshal.Copy(result.Bytes, bytes, 0, bytes.Length);
            }
            return new RenderedDocument(
                bytes,
                (CodePage)result.CodePage,
                new DocumentMeta(
                    result.HasCut != 0 ? (CutSetting)result.Cut : null,
                    result.TopFeedDots,
                    result.BottomFeedDots),
                _driver.RenderReport());
        }
        finally
        {
            Marshal.FreeCoTaskMem(locale);
            Marshal.FreeCoTaskMem(currency);
            Marshal.FreeCoTaskMem(timeZone);
        }
    }

    /// <summary>Renders a document supplied as <see cref="JsonNode"/>.</summary>
    /// <remarks>
    /// The node is serialized here and parsed by the core's own strict RFC 8259 reader, so
    /// what is rendered is exactly what a stored <c>.json</c> template would render.
    /// </remarks>
    /// <param name="document">The document or template.</param>
    /// <param name="model">The parameter model, or null.</param>
    /// <param name="options">Media and locale overrides; null takes the defaults.</param>
    /// <returns>The bytes, the document's meta, and the render report.</returns>
    public RenderedDocument RenderDocument(JsonNode document, JsonNode? model = null,
                                           RenderOptions? options = null)
    {
        ArgumentNullException.ThrowIfNull(document);
        return RenderDocument(document.ToJsonString(), model?.ToJsonString(), options);
    }

    /// <summary>Runs <see cref="RenderDocument(string, string?, RenderOptions?)"/> off the calling thread.</summary>
    /// <param name="documentJson">A document or a template.</param>
    /// <param name="modelJson">The parameter model, or null.</param>
    /// <param name="options">Media and locale overrides; null takes the defaults.</param>
    /// <param name="cancellationToken">Cancels the wait, never the render.</param>
    /// <returns>The rendered document.</returns>
    public Task<RenderedDocument> RenderDocumentAsync(string documentJson,
                                                      string? modelJson = null,
                                                      RenderOptions? options = null,
                                                      CancellationToken cancellationToken = default) =>
        Task.Run(() => RenderDocument(documentJson, modelJson, options), cancellationToken);

    /// <summary>Renders a receipt-DSL document and prints it.</summary>
    /// <remarks>
    /// The job is an ordinary <see cref="PrintJob"/>: same worker, same preflight, same
    /// completion fence, same confidence grading, same idempotency-key dedupe as
    /// <see cref="Print"/>. There is no second engine — a template job proves exactly what
    /// a raster job proves. The document's <c>meta</c> reaches the job through
    /// <paramref name="options"/>: a default <see cref="JobOptions"/> lets the document
    /// decide its own cut and margins, and any field set explicitly wins over it.
    /// </remarks>
    /// <param name="documentJson">A document or a template.</param>
    /// <param name="modelJson">The parameter model bound into a template, or null.</param>
    /// <param name="options">Key, cut, preflight and timeout; null takes the defaults.</param>
    /// <returns>
    /// The job, carrying what the renderer declared in <see cref="PrintJob.RenderReport"/>.
    /// </returns>
    /// <exception cref="PrinterDriverException">
    /// There are no bytes to send. Nothing is submitted in that case — a malformed
    /// document never becomes a blank receipt.
    /// </exception>
    public PrintJob PrintDocument(string documentJson, string? modelJson = null,
                                  JobOptions? options = null)
    {
        ArgumentException.ThrowIfNullOrEmpty(documentJson);
        options ??= new JobOptions();
        var key = Marshal.StringToCoTaskMemUTF8(options.Key ?? string.Empty);
        try
        {
            var native = NativeOptions(options, key);
            var job = NativeMethods.pd_print_document_json(
                _driver.Handle, _handle, documentJson, modelJson, in native, 0);
            if (job == 0)
            {
                throw new PrinterDriverException(
                    _driver.LastError() + "\n" + string.Join('\n', _driver.RenderReport()));
            }
            var printJob = _driver.InternJob(job);
            printJob.SetRenderReport(_driver.RenderReport());
            return printJob;
        }
        finally
        {
            Marshal.FreeCoTaskMem(key);
        }
    }

    /// <summary>Prints a document supplied as <see cref="JsonNode"/>.</summary>
    /// <param name="document">The document or template.</param>
    /// <param name="model">The parameter model, or null.</param>
    /// <param name="options">Key, cut, preflight and timeout; null takes the defaults.</param>
    /// <returns>The job.</returns>
    public PrintJob PrintDocument(JsonNode document, JsonNode? model = null,
                                  JobOptions? options = null)
    {
        ArgumentNullException.ThrowIfNull(document);
        return PrintDocument(document.ToJsonString(), model?.ToJsonString(), options);
    }

    /// <summary>
    /// Renders, prints, and waits for the tri-state result — the closure-free form for a
    /// caller that only wants to know what happened to the paper.
    /// </summary>
    /// <param name="documentJson">A document or a template.</param>
    /// <param name="modelJson">The parameter model, or null.</param>
    /// <param name="options">Key, cut, preflight and timeout; null takes the defaults.</param>
    /// <param name="cancellationToken">Cancels the wait, never the job.</param>
    /// <returns>The terminal result.</returns>
    public Task<JobResult> SendDocumentAsync(string documentJson, string? modelJson = null,
                                             JobOptions? options = null,
                                             CancellationToken cancellationToken = default) =>
        PrintDocument(documentJson, modelJson, options).GetResultAsync(cancellationToken);
}

public sealed partial class PrinterDriver
{
    /// <summary>
    /// The last document render's report, pulled out through the ABI's index reader.
    /// </summary>
    /// <remarks>
    /// Read immediately after the call that produced it: pd.h owns these strings until the
    /// next render on this driver, and this is where they stop being borrowed. Callers
    /// normally reach it through <see cref="RenderedDocument.Report"/> or
    /// <see cref="PrintJob.RenderReport"/>, which do exactly that at the right moment.
    /// </remarks>
    /// <returns>One entry per declared degradation, in the order they happened.</returns>
    internal IReadOnlyList<ReportEntry> RenderReport()
    {
        var count = (int)NativeMethods.pd_render_report_count(Handle);
        if (count == 0)
        {
            return [];
        }
        var entries = new List<ReportEntry>(count);
        for (var index = 0; index < count; index++)
        {
            if (NativeMethods.pd_render_report_at(Handle, index, out var entry) != 1)
            {
                continue;
            }
            entries.Add(new ReportEntry(
                (ReportKind)entry.Kind,
                NativeMethods.ReadUtf8(entry.Block),
                NativeMethods.ReadUtf8(entry.Requested),
                NativeMethods.ReadUtf8(entry.Delivered),
                (RenderPath)entry.Path,
                NativeMethods.ReadUtf8(entry.Note)));
        }
        return entries;
    }
}

/// <summary>The core's own spellings for the render report's enums.</summary>
public static class ReportEnumNames
{
    /// <summary><c>pd_report_kind_name</c>.</summary>
    /// <param name="value">The report kind.</param>
    /// <returns>The core's spelling.</returns>
    public static string AbiName(this ReportKind value) =>
        NativeMethods.ReadUtf8(NativeMethods.pd_report_kind_name((int)value));

    /// <summary><c>pd_render_path_name</c>.</summary>
    /// <param name="value">The render path.</param>
    /// <returns>The core's spelling.</returns>
    public static string AbiName(this RenderPath value) =>
        NativeMethods.ReadUtf8(NativeMethods.pd_render_path_name((int)value));
}
