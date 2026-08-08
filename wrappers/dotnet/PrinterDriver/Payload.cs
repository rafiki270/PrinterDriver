using System.Buffers;
using System.Runtime.InteropServices;
using System.Text;

namespace PrinterDriver;

/// <summary>
/// What to print, in one of the three tiers of docs/api.md §3. The hierarchy is closed:
/// the constructor is private, so <see cref="Raster"/>, <see cref="Document"/> and
/// <see cref="Raw"/> are the only forms that exist.
/// </summary>
public abstract record Payload
{
    private Payload() { }

    /// <summary>
    /// Tier 1. Straight RGBA8 as every platform's bitmap hands it over. The core
    /// composites alpha over white paper, converts to grey with the ITU-R 601 luma
    /// weights, scales to the printer's dot width, binarizes and bands — all in integer
    /// arithmetic, so the same pixels always produce the same bytes.
    /// </summary>
    /// <param name="Pixels">Row-major RGBA8 pixels.</param>
    /// <param name="Width">Width in pixels.</param>
    /// <param name="Height">Height in pixels.</param>
    /// <param name="StrideBytes">Row stride; 0 for tightly packed rows.</param>
    /// <param name="Binarization">How grey becomes dots.</param>
    /// <param name="Threshold">
    /// Read only for <c>Binarization.FixedThreshold</c>; 0 means 128.
    /// </param>
    /// <param name="MaxRowsPerBand">0 means 1024, the Epson tall-image split.</param>
    public sealed record Raster(
        ReadOnlyMemory<byte> Pixels,
        uint Width,
        uint Height,
        uint StrideBytes = 0,
        Binarization Binarization = Binarization.FloydSteinberg,
        byte Threshold = 0,
        uint MaxRowsPerBand = 0) : Payload;

    /// <summary>
    /// Tier 2. A short list of receipt operations, deliberately minimal: everything a
    /// receipt needs and nothing a layout engine would need. Richer documents belong in
    /// <see cref="Raster"/>.
    /// </summary>
    /// <param name="Ops">The operations, in order.</param>
    /// <param name="CodePage">Which code page the text is transliterated into.</param>
    public sealed record Document(
        IReadOnlyList<DocumentOp> Ops,
        CodePage CodePage = CodePage.PC437) : Payload;

    /// <summary>
    /// Tier 3. Passed through verbatim. Must not embed its own cuts or realtime status
    /// tricks: the core owns job termination, and its trailing fence assumes that.
    /// </summary>
    /// <param name="Bytes">The bytes to send.</param>
    public sealed record Raw(ReadOnlyMemory<byte> Bytes) : Payload;

    /// <summary>Convenience: one line of text as a raw payload, UTF-8 encoded.</summary>
    /// <param name="text">The text to send.</param>
    /// <returns>A <see cref="Raw"/> payload.</returns>
    public static Payload FromText(string text)
    {
        ArgumentNullException.ThrowIfNull(text);
        return new Raw(Encoding.UTF8.GetBytes(text));
    }
}

/// <summary>One operation in a <see cref="Payload.Document"/>.</summary>
/// <param name="Kind">Which operation.</param>
/// <param name="Content">
/// UTF-8 text for <see cref="DocumentOpKind.Text"/> and <see cref="DocumentOpKind.Line"/>;
/// transliterated to the document's code page, lossily. Null for the others.
/// </param>
/// <param name="Value">The operand for the operations that take one.</param>
public readonly record struct DocumentOp(DocumentOpKind Kind, string? Content, int Value)
{
    /// <summary>Text with no line break.</summary>
    /// <param name="text">The text.</param>
    /// <returns>The operation.</returns>
    public static DocumentOp Text(string text) => new(DocumentOpKind.Text, text, 0);

    /// <summary>Text followed by a line feed; null emits a bare line feed.</summary>
    /// <param name="text">The text, or null.</param>
    /// <returns>The operation.</returns>
    public static DocumentOp Line(string? text = null) => new(DocumentOpKind.Line, text, 0);

    /// <summary>Set the alignment of everything that follows.</summary>
    /// <param name="alignment">The alignment.</param>
    /// <returns>The operation.</returns>
    public static DocumentOp Align(Alignment alignment) =>
        new(DocumentOpKind.Align, null, (int)alignment);

    /// <summary>Turn emphasis on or off.</summary>
    /// <param name="on">Whether emphasis is on.</param>
    /// <returns>The operation.</returns>
    public static DocumentOp Bold(bool on) => new(DocumentOpKind.Bold, null, on ? 1 : 0);

    /// <summary>Feed blank lines.</summary>
    /// <param name="lines">How many, 1 to 255.</param>
    /// <returns>The operation.</returns>
    public static DocumentOp Feed(int lines) => new(DocumentOpKind.Feed, null, lines);
}

/// <summary>The operations <see cref="Payload.Document"/> supports (pd.h's pd_op_kind).</summary>
public enum DocumentOpKind
{
    /// <summary>Text, no line break.</summary>
    Text = 0,

    /// <summary>Text followed by a line feed; null text emits a bare line feed.</summary>
    Line = 1,

    /// <summary>Set alignment.</summary>
    Align = 2,

    /// <summary>Set emphasis.</summary>
    Bold = 3,

    /// <summary>Feed blank lines.</summary>
    Feed = 4,
}

/// <summary>
/// Turns a managed <see cref="Payload"/> into the <c>pd_payload</c> the ABI takes, and
/// keeps every pointer inside it alive for exactly as long as the call.
/// </summary>
/// <remarks>
/// pd_print copies the payload before it returns (capi/src/pd_capi.cpp buildPayload), so
/// the lifetime that matters is the duration of the call and nothing longer. Managed
/// arrays are pinned rather than copied; strings and the op array, which have no managed
/// form the ABI can read directly, are copied into unmanaged memory and freed here.
/// </remarks>
internal sealed class PayloadMarshaller : IDisposable
{
    private readonly List<MemoryHandle> _pinned = [];
    private readonly List<nint> _hglobal = [];
    private readonly List<nint> _cotaskmem = [];

    internal PdPayload Native { get; }

    internal PayloadMarshaller(Payload payload)
    {
        switch (payload)
        {
            case Payload.Raster raster:
            {
                var handle = raster.Pixels.Pin();
                _pinned.Add(handle);
                Native = new PdPayload
                {
                    Kind = (int)PayloadKind.Raster,
                    As = new PdPayloadUnion
                    {
                        Raster = new PdRasterRgba8
                        {
                            Pixels = Unsafe(handle),
                            Width = raster.Width,
                            Height = raster.Height,
                            StrideBytes = raster.StrideBytes,
                            Binarization = (int)raster.Binarization,
                            Threshold = raster.Threshold,
                            MaxRowsPerBand = raster.MaxRowsPerBand,
                        },
                    },
                };
                break;
            }

            case Payload.Document document:
            {
                var count = document.Ops.Count;
                var array = Marshal.AllocHGlobal(Marshal.SizeOf<PdOp>() * Math.Max(count, 1));
                _hglobal.Add(array);
                for (var i = 0; i < count; i++)
                {
                    var op = document.Ops[i];
                    var text = op.Content is null ? 0 : AllocateUtf8(op.Content);
                    Marshal.StructureToPtr(
                        new PdOp { Kind = (int)op.Kind, Text = text, Value = op.Value },
                        array + (i * Marshal.SizeOf<PdOp>()),
                        fDeleteOld: false);
                }
                Native = new PdPayload
                {
                    Kind = (int)PayloadKind.Document,
                    As = new PdPayloadUnion
                    {
                        Document = new PdDocument
                        {
                            Ops = array,
                            Count = (nuint)count,
                            CodePage = (int)document.CodePage,
                        },
                    },
                };
                break;
            }

            case Payload.Raw raw:
            {
                var handle = raw.Bytes.Pin();
                _pinned.Add(handle);
                Native = new PdPayload
                {
                    Kind = (int)PayloadKind.Raw,
                    As = new PdPayloadUnion
                    {
                        Raw = new PdRaw { Bytes = Unsafe(handle), Size = (nuint)raw.Bytes.Length },
                    },
                };
                break;
            }

            default:
                throw new PrinterDriverException(
                    $"unknown payload form {payload.GetType().Name}");
        }
    }

    private nint AllocateUtf8(string value)
    {
        var pointer = Marshal.StringToCoTaskMemUTF8(value);
        _cotaskmem.Add(pointer);
        return pointer;
    }

    private static unsafe nint Unsafe(MemoryHandle handle) => (nint)handle.Pointer;

    /// <summary>
    /// Releases every pointer handed to the ABI. Safe to call once pd_print has returned,
    /// and only then: the ABI reads through these during the call.
    /// </summary>
    public void Dispose()
    {
        foreach (var pointer in _cotaskmem)
        {
            Marshal.FreeCoTaskMem(pointer);
        }
        _cotaskmem.Clear();
        foreach (var pointer in _hglobal)
        {
            Marshal.FreeHGlobal(pointer);
        }
        _hglobal.Clear();
        foreach (var handle in _pinned)
        {
            handle.Dispose();
        }
        _pinned.Clear();
    }
}
