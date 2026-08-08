using System.Runtime.InteropServices;

namespace PrinterDriver;

/// <summary>
/// Keeps every delegate and context object handed to the C ABI alive for exactly as long
/// as the ABI can still call it.
/// </summary>
/// <remarks>
/// <para>
/// <b>Why this type exists.</b> pd.h has no <c>pd_unsubscribe_*</c>. Once
/// <c>pd_subscribe_job</c> or <c>pd_subscribe_device</c> has been handed a function
/// pointer, the core may invoke it from a printer worker thread at any moment until
/// <c>pd_destroy</c> returns. A managed delegate that is only referenced by the
/// unmanaged side is not referenced at all as far as the GC is concerned: it gets
/// collected, the function pointer becomes a dangling stub, and the process dies inside a
/// native thread with a stack trace that names nothing. That is the single most common
/// bug in a P/Invoke wrapper of a callback API, and this class is the whole of the
/// defence against it.
/// </para>
/// <para>
/// <b>How.</b> Each subscription allocates two <see cref="GCHandle"/>s — a normal
/// (rooting, non-pinning) handle on the delegate, and one on the subscription's state
/// object whose <see cref="GCHandle.ToIntPtr(GCHandle)"/> value is passed as the ABI's
/// <c>void* ctx</c>. Both stay allocated until <see cref="FreeAll"/>, which the driver
/// calls <em>after</em> <c>pd_destroy</c> has returned, i.e. after every worker thread has
/// stopped and no further callback is possible. Freeing them any earlier would reopen the
/// exact window this exists to close.
/// </para>
/// <para>
/// A delegate does not need pinning: the runtime creates a stable native stub for it, and
/// the stub survives compaction. What it needs is a root, which
/// <see cref="GCHandleType.Normal"/> provides.
/// </para>
/// </remarks>
internal sealed class CallbackRoots
{
    private readonly object _gate = new();
    private readonly List<GCHandle> _handles = [];
    private bool _frozen;

    /// <summary>Roots <paramref name="value"/> and returns its handle.</summary>
    internal GCHandle Root(object value)
    {
        var handle = GCHandle.Alloc(value, GCHandleType.Normal);
        lock (_gate)
        {
            if (_frozen)
            {
                // The driver is being disposed. Nothing can be subscribed any more, and
                // holding a handle that FreeAll will never see would leak it.
                handle.Free();
                throw new ObjectDisposedException(nameof(PrinterDriver));
            }
            _handles.Add(handle);
        }
        return handle;
    }

    /// <summary>
    /// Releases every root. Must be called only after <c>pd_destroy</c> has returned.
    /// </summary>
    internal void FreeAll()
    {
        List<GCHandle> handles;
        lock (_gate)
        {
            _frozen = true;
            handles = [.. _handles];
            _handles.Clear();
        }
        foreach (var handle in handles)
        {
            if (handle.IsAllocated)
            {
                handle.Free();
            }
        }
    }

    /// <summary>Recovers a subscription state object from the ABI's <c>void* ctx</c>.</summary>
    internal static T? FromContext<T>(nint context) where T : class =>
        context == 0 ? null : GCHandle.FromIntPtr(context).Target as T;
}
