using System.Runtime.InteropServices;

namespace PrinterDriver;

// Custom method registration (docs/api.md §16): five ways to extend the SDK at runtime
// without forking it. All per-driver, all data-plus-callbacks, all keyed by namespaced
// string ids ("acme.x-idle"), and everything a registration claims — a completion grade,
// an authority, a formatter name — is attributed to it BY ID in the job result and in
// `pdctl verify`. A custom method's claims are auditable exactly like a built-in's, which
// is the only reason extending the honesty-critical part of this SDK is allowed at all.
//
// -- Threads --------------------------------------------------------------------------
// Every callback below runs on a CORE thread, never the caller's: a fence and matcher on
// the printer's worker thread and its transport reader path, a probe step's classify on
// the worker driving the probe, a formatter or block handler on whatever thread renders
// the document. None of them may block, and none may call back into the driver.
//
// -- Lifetime -------------------------------------------------------------------------
// Each registration roots its delegates for the life of the driver, exactly like a custom
// transport's. pd.h has no unregister call, so a delegate the GC could collect is a
// dangling stub the core keeps calling — see CallbackRoots.

/// <summary>A matcher's verdict on the printer→host bytes it was handed.</summary>
public enum CompletionMatchKind
{
    /// <summary>A fence answer carrying a token; confirms the job exactly like <c>GS ( H</c>.</summary>
    Matched = 0,

    /// <summary>Not this mechanism's bytes; the core drops its matcher buffer and reads on.</summary>
    NotMine = 1,

    /// <summary>An answer may be forming but is incomplete; the core keeps buffering.</summary>
    NeedMore = 2,
}

/// <summary>What a custom matcher concluded, and the token it matched.</summary>
/// <param name="Kind">The verdict.</param>
/// <param name="Token">
/// The four-character correlation token, for <see cref="CompletionMatchKind.Matched"/>
/// only. Empty otherwise.
/// </param>
public readonly record struct CompletionMatch(CompletionMatchKind Kind, string Token)
{
    /// <summary>A fence answer carrying <paramref name="token"/>.</summary>
    public static CompletionMatch Matched(string token) =>
        new(CompletionMatchKind.Matched, token);

    /// <summary>Not this mechanism's bytes.</summary>
    public static CompletionMatch NotMine { get; } = new(CompletionMatchKind.NotMine, string.Empty);

    /// <summary>An answer may still be forming.</summary>
    public static CompletionMatch NeedMore { get; } = new(CompletionMatchKind.NeedMore, string.Empty);

    /// <summary>The core's own spelling of this verdict, from <c>pd_match_kind_name</c>.</summary>
    public string AbiName => NativeMethods.ReadUtf8(NativeMethods.pd_match_kind_name((int)Kind));
}

/// <summary>
/// A custom completion mechanism (docs/api.md §16) — the marquee registration point.
/// </summary>
/// <remarks>
/// Bind a printer to it by attaching with the profile id <c>"vendoridle:&lt;id&gt;"</c>,
/// e.g. <c>"vendoridle:acme.x-idle"</c>, which resolves to a generic ESC/POS profile whose
/// completion is this method. The engine sends <see cref="FenceBytes"/> behind the payload
/// and routes the printer's response stream through <see cref="Matcher"/>; a
/// <see cref="CompletionMatchKind.Matched"/> confirms the job like <c>GS ( H</c>, with the
/// same per-job token map and the same resolvable verification identifier.
/// </remarks>
public sealed class CompletionMethod
{
    /// <summary>Namespaced id, e.g. <c>acme.x-idle</c>.</summary>
    public required string Id { get; init; }

    /// <summary>The fence bytes for a job's four-character token.</summary>
    public required Func<string, byte[]> FenceBytes { get; init; }

    /// <summary>Classifies the bytes accumulated since the last verdict.</summary>
    public required Func<byte[], CompletionMatch> Matcher { get; init; }

    /// <summary>What a confirmed completion on this method claims.</summary>
    public ConfidenceGrade Grade { get; init; } = ConfidenceGrade.AJobLevelConfirmation;

    /// <summary>Who makes that claim.</summary>
    public CompletionAuthority Authority { get; init; } = CompletionAuthority.PhysicalPrinter;

    /// <summary>Shown in the result and in <c>pdctl verify</c>; null uses <see cref="Id"/>.</summary>
    public string? MethodName { get; init; }
}

/// <summary>What a custom probe step concluded about the answer it was handed.</summary>
/// <param name="Answered">Whether the device replied to this step at all.</param>
/// <param name="Label">A short classification; truncated at 63 bytes.</param>
public readonly record struct ProbeFinding(bool Answered, string Label);

/// <summary>
/// An extra fingerprinting step for <c>probe</c> and auto-detection (docs/api.md §16).
/// </summary>
/// <remarks>
/// <see cref="RequestBytes"/> MUST be non-printing — no <c>0x20</c>–<c>0x7E</c> run, no
/// line feed. A printing step is refused at registration, because auto-detection must
/// never cost a venue a roll of paper.
/// </remarks>
public sealed class ProbeStep
{
    /// <summary>Namespaced id.</summary>
    public required string Id { get; init; }

    /// <summary>The non-printing request.</summary>
    public required byte[] RequestBytes { get; init; }

    /// <summary>Classifies the response.</summary>
    public required Func<byte[], ProbeFinding> Classify { get; init; }
}

/// <summary>What a custom block handler made of a block: ops, or a declared degradation.</summary>
/// <param name="Ops">Raw ESC/POS ops, when the block rendered.</param>
/// <param name="DegradationReason">
/// The one line saying why it did not, when it did not. Reported exactly like a built-in
/// block's degradation — never dropped in silence.
/// </param>
public readonly record struct BlockRendering(byte[]? Ops, string? DegradationReason)
{
    /// <summary>The block rendered to these ops.</summary>
    public static BlockRendering Rendered(byte[] ops) => new(ops, null);

    /// <summary>The block could not be drawn, and this is the line that says so.</summary>
    public static BlockRendering Degraded(string reason) => new(null, reason);
}

/// <summary>
/// Renders a new DSL block kind (docs/api.md §16). A handler registered for a kind always
/// owns it: unknown kinds otherwise degrade, and this intercepts first.
/// </summary>
public sealed class BlockHandler
{
    /// <summary>The block object key that selects this handler.</summary>
    public required string Kind { get; init; }

    /// <summary>
    /// Renders the block. The first argument is the block object as JSON, the second a
    /// small JSON of the render profile facts
    /// (<c>{"width_dots":576,"barcode":true,...}</c>).
    /// </summary>
    public required Func<string, string, BlockRendering> Render { get; init; }
}

/// <summary>
/// Backs <c>{{ v | name:args }}</c> in the template layer, checked before the built-in
/// table (docs/api.md §16).
/// </summary>
public sealed class TemplateFormatter
{
    /// <summary>The formatter name used in templates.</summary>
    public required string Name { get; init; }

    /// <summary>
    /// Formats (value, args, locale). Returning null declines and falls through to the
    /// built-ins.
    /// </summary>
    public required Func<string, string, string, string?> Format { get; init; }
}

/// <summary>
/// A vendor drawer-kick method, filling <see cref="DrawerKickMethod.Vendor"/> for a profile
/// (docs/api.md §16, docs/cash-drawer.md).
/// </summary>
/// <remarks>
/// <see cref="StatusRequest"/> and <see cref="StatusParse"/> go together: supplying neither
/// means the method has no readable switch, so a kick reports
/// <see cref="DrawerState.KickSentUnverified"/> rather than a verified open. That is the
/// honest answer, not a weak success.
/// </remarks>
public sealed class DrawerKick
{
    /// <summary>Namespaced id.</summary>
    public required string Id { get; init; }

    /// <summary>The pulse bytes for (channel, pulse duration).</summary>
    public required Func<byte, ushort, byte[]> KickBytes { get; init; }

    /// <summary>The bytes that ask for the switch state.</summary>
    public Func<byte[]>? StatusRequest { get; init; }

    /// <summary>
    /// The pin level a reply carries: true high, false low, null unreadable.
    /// </summary>
    public Func<byte[], bool?>? StatusParse { get; init; }
}

public sealed partial class PrinterDriver
{
    /// <summary>Registers a custom completion mechanism (docs/api.md §16).</summary>
    /// <param name="method">The mechanism.</param>
    /// <exception cref="PrinterDriverException">
    /// A bad or duplicate id, or a record the core refused.
    /// </exception>
    public void RegisterCompletionMethod(CompletionMethod method)
    {
        ArgumentNullException.ThrowIfNull(method);
        var binding = _roots.Root(new CompletionBinding(method));
        var fence = new NativeMethods.FenceBytesCallback(CompletionBinding.FenceTrampoline);
        var matcher = new NativeMethods.CompletionMatcherCallback(
            CompletionBinding.MatcherTrampoline);
        _roots.Root(fence);
        _roots.Root(matcher);

        var id = Marshal.StringToCoTaskMemUTF8(method.Id);
        var name = Marshal.StringToCoTaskMemUTF8(method.MethodName ?? method.Id);
        try
        {
            var native = new PdCompletionMethod
            {
                Id = id,
                FenceBytes = Marshal.GetFunctionPointerForDelegate(fence),
                Matcher = Marshal.GetFunctionPointerForDelegate(matcher),
                Context = GCHandle.ToIntPtr(binding),
                Grade = (int)method.Grade,
                Authority = (int)method.Authority,
                MethodName = name,
            };
            if (NativeMethods.pd_register_completion_method(Handle, in native) == 0)
            {
                throw new PrinterDriverException(LastError());
            }
        }
        finally
        {
            Marshal.FreeCoTaskMem(id);
            Marshal.FreeCoTaskMem(name);
        }
    }

    /// <summary>Registers an extra fingerprinting step (docs/api.md §16).</summary>
    /// <param name="step">The step. Its request must not be able to print.</param>
    /// <exception cref="PrinterDriverException">
    /// A bad or duplicate id, or a request whose bytes could mark paper.
    /// </exception>
    public void RegisterProbeStep(ProbeStep step)
    {
        ArgumentNullException.ThrowIfNull(step);
        var binding = _roots.Root(new ProbeBinding(step));
        var classify = new NativeMethods.ProbeClassifyCallback(ProbeBinding.ClassifyTrampoline);
        _roots.Root(classify);

        var id = Marshal.StringToCoTaskMemUTF8(step.Id);
        var request = Marshal.AllocCoTaskMem(Math.Max(step.RequestBytes.Length, 1));
        try
        {
            Marshal.Copy(step.RequestBytes, 0, request, step.RequestBytes.Length);
            var native = new PdProbeStep
            {
                Id = id,
                RequestBytes = request,
                RequestSize = (nuint)step.RequestBytes.Length,
                Classify = Marshal.GetFunctionPointerForDelegate(classify),
                Context = GCHandle.ToIntPtr(binding),
            };
            if (NativeMethods.pd_register_probe_step(Handle, in native) == 0)
            {
                throw new PrinterDriverException(LastError());
            }
        }
        finally
        {
            Marshal.FreeCoTaskMem(id);
            Marshal.FreeCoTaskMem(request);
        }
    }

    /// <summary>Registers a renderer for a new DSL block kind (docs/api.md §16).</summary>
    /// <remarks>
    /// The core stores this and the receipt-DSL render path calls it — but that path has no
    /// entry point in <c>pd.h</c> yet, so a registration made from .NET is not reached by
    /// <see cref="Printer.Print(Payload, JobOptions?)"/> today. See docs/api.md §17.1.
    /// </remarks>
    /// <param name="handler">The handler.</param>
    /// <exception cref="PrinterDriverException">A bad or duplicate kind.</exception>
    public void RegisterBlockHandler(BlockHandler handler)
    {
        ArgumentNullException.ThrowIfNull(handler);
        var binding = _roots.Root(new BlockBinding(handler));
        var render = new NativeMethods.BlockHandlerCallback(BlockBinding.RenderTrampoline);
        _roots.Root(render);

        var kind = Marshal.StringToCoTaskMemUTF8(handler.Kind);
        try
        {
            var native = new PdBlockHandler
            {
                Kind = kind,
                Handler = Marshal.GetFunctionPointerForDelegate(render),
                Context = GCHandle.ToIntPtr(binding),
            };
            if (NativeMethods.pd_register_block_handler(Handle, in native) == 0)
            {
                throw new PrinterDriverException(LastError());
            }
        }
        finally
        {
            Marshal.FreeCoTaskMem(kind);
        }
    }

    /// <summary>Registers a template formatter (docs/api.md §16).</summary>
    /// <remarks>
    /// Same reachability caveat as <see cref="RegisterBlockHandler(BlockHandler)"/>: the
    /// template layer is not in <c>pd.h</c> yet (docs/api.md §17.1).
    /// </remarks>
    /// <param name="formatter">The formatter. Checked before the built-in table.</param>
    /// <exception cref="PrinterDriverException">A bad or duplicate name.</exception>
    public void RegisterFormatter(TemplateFormatter formatter)
    {
        ArgumentNullException.ThrowIfNull(formatter);
        var binding = _roots.Root(new FormatterBinding(formatter));
        var format = new NativeMethods.FormatterCallback(FormatterBinding.FormatTrampoline);
        _roots.Root(format);

        var name = Marshal.StringToCoTaskMemUTF8(formatter.Name);
        try
        {
            var native = new PdFormatter
            {
                Name = name,
                Formatter = Marshal.GetFunctionPointerForDelegate(format),
                Context = GCHandle.ToIntPtr(binding),
            };
            if (NativeMethods.pd_register_formatter(Handle, in native) == 0)
            {
                throw new PrinterDriverException(LastError());
            }
        }
        finally
        {
            Marshal.FreeCoTaskMem(name);
        }
    }

    /// <summary>Registers a vendor drawer-kick method (docs/api.md §16).</summary>
    /// <param name="kick">The method.</param>
    /// <exception cref="PrinterDriverException">A bad or duplicate id.</exception>
    public void RegisterDrawerKick(DrawerKick kick)
    {
        ArgumentNullException.ThrowIfNull(kick);
        var binding = _roots.Root(new DrawerKickBinding(kick));
        var kickBytes = new NativeMethods.DrawerKickBytesCallback(
            DrawerKickBinding.KickTrampoline);
        _roots.Root(kickBytes);

        nint statusRequest = 0;
        nint statusParse = 0;
        if (kick.StatusRequest is not null && kick.StatusParse is not null)
        {
            var request = new NativeMethods.DrawerStatusRequestCallback(
                DrawerKickBinding.StatusRequestTrampoline);
            var parse = new NativeMethods.DrawerStatusParseCallback(
                DrawerKickBinding.StatusParseTrampoline);
            _roots.Root(request);
            _roots.Root(parse);
            statusRequest = Marshal.GetFunctionPointerForDelegate(request);
            statusParse = Marshal.GetFunctionPointerForDelegate(parse);
        }

        var id = Marshal.StringToCoTaskMemUTF8(kick.Id);
        try
        {
            var native = new PdDrawerKickReg
            {
                Id = id,
                KickBytes = Marshal.GetFunctionPointerForDelegate(kickBytes),
                StatusRequest = statusRequest,
                StatusParse = statusParse,
                Context = GCHandle.ToIntPtr(binding),
            };
            if (NativeMethods.pd_register_drawer_kick(Handle, in native) == 0)
            {
                throw new PrinterDriverException(LastError());
            }
        }
        finally
        {
            Marshal.FreeCoTaskMem(id);
        }
    }
}

// --- Trampolines ----------------------------------------------------------------------
//
// Static, so the function pointer never depends on an instance the GC could move, and the
// registration is recovered from the ABI's void* ctx exactly as the transport ones are. A
// managed exception must never cross back into C: each trampoline answers with the
// registration's own "I could not" value instead, which the core already knows how to
// handle (a fence that does not fit fails its job Unknown; a formatter that declines falls
// through to the built-ins).

internal sealed class CompletionBinding(CompletionMethod method)
{
    private readonly CompletionMethod _method = method;

    internal static unsafe nuint FenceTrampoline(nint context, nint jobToken, nint output,
                                                 nuint capacity)
    {
        var binding = CallbackRoots.FromContext<CompletionBinding>(context);
        if (binding is null || output == 0)
        {
            return 0;
        }
        byte[] bytes;
        try
        {
            bytes = binding._method.FenceBytes(NativeMethods.ReadUtf8(jobToken)) ?? [];
        }
        catch
        {
            return 0;
        }
        // Over capacity is reported as such and never truncated: half a fence is a job the
        // core must fail Unknown, not one it may claim.
        if ((nuint)bytes.Length > capacity)
        {
            return capacity + 1;
        }
        Marshal.Copy(bytes, 0, output, bytes.Length);
        return (nuint)bytes.Length;
    }

    internal static unsafe PdMatchResult MatcherTrampoline(nint context, nint data, nuint size)
    {
        var result = default(PdMatchResult);
        result.Kind = (int)CompletionMatchKind.NotMine;
        var binding = CallbackRoots.FromContext<CompletionBinding>(context);
        if (binding is null)
        {
            return result;
        }

        var bytes = new byte[(int)size];
        if (size > 0 && data != 0)
        {
            Marshal.Copy(data, bytes, 0, bytes.Length);
        }

        CompletionMatch verdict;
        try
        {
            verdict = binding._method.Matcher(bytes);
        }
        catch
        {
            return result;
        }

        result.Kind = (int)verdict.Kind;
        if (verdict.Kind == CompletionMatchKind.Matched)
        {
            var token = System.Text.Encoding.UTF8.GetBytes(verdict.Token ?? string.Empty);
            var count = Math.Min(token.Length, 7);
            for (var index = 0; index < count; index++)
            {
                result.Token[index] = token[index];
            }
            result.Token[count] = 0;
        }
        return result;
    }
}

internal sealed class ProbeBinding(ProbeStep step)
{
    private readonly ProbeStep _step = step;

    internal static unsafe PdProbeFinding ClassifyTrampoline(nint context, nint response,
                                                             nuint size)
    {
        var finding = default(PdProbeFinding);
        var binding = CallbackRoots.FromContext<ProbeBinding>(context);
        if (binding is null)
        {
            return finding;
        }

        var bytes = new byte[(int)size];
        if (size > 0 && response != 0)
        {
            Marshal.Copy(response, bytes, 0, bytes.Length);
        }

        ProbeFinding answer;
        try
        {
            answer = binding._step.Classify(bytes);
        }
        catch
        {
            return finding;
        }

        finding.Answered = answer.Answered ? 1 : 0;
        var label = System.Text.Encoding.UTF8.GetBytes(answer.Label ?? string.Empty);
        var count = Math.Min(label.Length, 63);
        for (var index = 0; index < count; index++)
        {
            finding.Label[index] = label[index];
        }
        finding.Label[count] = 0;
        return finding;
    }
}

internal sealed class BlockBinding(BlockHandler handler)
{
    private readonly BlockHandler _handler = handler;

    internal static nuint RenderTrampoline(nint context, nint blockJson, nint profileJson,
                                           nint output, nuint capacity, nint ok, nint detail,
                                           nuint detailCapacity)
    {
        var binding = CallbackRoots.FromContext<BlockBinding>(context);
        if (binding is null || output == 0 || ok == 0)
        {
            return 0;
        }

        BlockRendering rendering;
        try
        {
            rendering = binding._handler.Render(
                NativeMethods.ReadUtf8(blockJson), NativeMethods.ReadUtf8(profileJson));
        }
        catch (Exception error)
        {
            rendering = BlockRendering.Degraded(error.Message);
        }

        if (rendering.Ops is { } ops)
        {
            Marshal.WriteInt32(ok, 1);
            if ((nuint)ops.Length > capacity)
            {
                return capacity + 1;
            }
            Marshal.Copy(ops, 0, output, ops.Length);
            return (nuint)ops.Length;
        }

        Marshal.WriteInt32(ok, 0);
        WriteUtf8(detail, detailCapacity, rendering.DegradationReason ?? "block not rendered");
        return 0;
    }

    internal static void WriteUtf8(nint destination, nuint capacity, string text)
    {
        if (destination == 0 || capacity == 0)
        {
            return;
        }
        var bytes = System.Text.Encoding.UTF8.GetBytes(text);
        var count = Math.Min(bytes.Length, (int)capacity - 1);
        Marshal.Copy(bytes, 0, destination, count);
        Marshal.WriteByte(destination, count, 0);
    }
}

internal sealed class FormatterBinding(TemplateFormatter formatter)
{
    private readonly TemplateFormatter _formatter = formatter;

    internal static nuint FormatTrampoline(nint context, nint value, nint args, nint locale,
                                           nint output, nuint capacity, nint handled)
    {
        var binding = CallbackRoots.FromContext<FormatterBinding>(context);
        if (binding is null || output == 0 || handled == 0)
        {
            return 0;
        }

        string? text;
        try
        {
            text = binding._formatter.Format(
                NativeMethods.ReadUtf8(value), NativeMethods.ReadUtf8(args),
                NativeMethods.ReadUtf8(locale));
        }
        catch
        {
            text = null;
        }

        if (text is null)
        {
            Marshal.WriteInt32(handled, 0);
            return 0;
        }

        Marshal.WriteInt32(handled, 1);
        var bytes = System.Text.Encoding.UTF8.GetBytes(text);
        if ((nuint)bytes.Length > capacity)
        {
            return capacity + 1;
        }
        Marshal.Copy(bytes, 0, output, bytes.Length);
        return (nuint)bytes.Length;
    }
}

internal sealed class DrawerKickBinding(DrawerKick kick)
{
    private readonly DrawerKick _kick = kick;

    internal static nuint KickTrampoline(nint context, byte channel, ushort pulseMs,
                                         nint output, nuint capacity)
    {
        var binding = CallbackRoots.FromContext<DrawerKickBinding>(context);
        if (binding is null || output == 0)
        {
            return 0;
        }
        byte[] bytes;
        try
        {
            bytes = binding._kick.KickBytes(channel, pulseMs) ?? [];
        }
        catch
        {
            return 0;
        }
        if ((nuint)bytes.Length > capacity)
        {
            return capacity + 1;
        }
        Marshal.Copy(bytes, 0, output, bytes.Length);
        return (nuint)bytes.Length;
    }

    internal static nuint StatusRequestTrampoline(nint context, nint output, nuint capacity)
    {
        var binding = CallbackRoots.FromContext<DrawerKickBinding>(context);
        if (binding is null || output == 0 || binding._kick.StatusRequest is null)
        {
            return 0;
        }
        byte[] bytes;
        try
        {
            bytes = binding._kick.StatusRequest() ?? [];
        }
        catch
        {
            return 0;
        }
        if ((nuint)bytes.Length > capacity)
        {
            return capacity + 1;
        }
        Marshal.Copy(bytes, 0, output, bytes.Length);
        return (nuint)bytes.Length;
    }

    /// <summary>-1 unknown, 0 low, 1 high — pd.h's PD_UNKNOWN / PD_FALSE / PD_TRUE.</summary>
    internal static int StatusParseTrampoline(nint context, nint response, nuint size)
    {
        var binding = CallbackRoots.FromContext<DrawerKickBinding>(context);
        if (binding is null || binding._kick.StatusParse is null)
        {
            return -1;
        }
        var bytes = new byte[(int)size];
        if (size > 0 && response != 0)
        {
            Marshal.Copy(response, bytes, 0, bytes.Length);
        }
        try
        {
            return binding._kick.StatusParse(bytes) switch
            {
                true => 1,
                false => 0,
                null => -1,
            };
        }
        catch
        {
            return -1;
        }
    }
}
