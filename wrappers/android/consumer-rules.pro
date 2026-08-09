# Consumer ProGuard/R8 rules -- applied to any app that depends on this library and
# enables minification. Native methods and JNI callback targets must keep their exact
# names and signatures: the native side resolves them by the
# Java_com_printerdriver_internal_NativeBridge_<name> symbol convention (native
# methods) and by explicit env->GetMethodID(class, "onEvent"/"onMessage", <signature>)
# lookups (callback interfaces) -- see src/main/cpp/printerdriver_jni.cpp. R8 renaming
# either one is a silent runtime break (UnsatisfiedLinkError or a null jmethodID), not
# a compile error, so these keeps matter more than usual.

-keepclasseswithmembernames,includedescriptorclasses class com.printerdriver.internal.NativeBridge {
    native <methods>;
}

-keep,includedescriptorclasses class com.printerdriver.internal.NativeJobEventCallback { *; }
-keep,includedescriptorclasses class com.printerdriver.internal.NativeDeviceEventCallback { *; }
-keep,includedescriptorclasses class com.printerdriver.internal.NativeLogCallback { *; }

# The custom-transport vtable (pd_add_printer_custom). Its three method IDs are resolved
# ONCE, at registration time, so a renamed connect/write/close is not a callback that
# silently no-ops -- addPrinterCustom refuses the registration outright and the printer
# never exists. Louder than the others, and still a keep rule worth having.
-keep,includedescriptorclasses class com.printerdriver.internal.NativeTransportCallback { *; }
-keep,includedescriptorclasses class com.printerdriver.BluetoothSppTransport { *; }

# Lambdas/anonymous classes implementing the callback interfaces above (from
# callbackFlow collectors and Printer.send's onProgress/onResult sugar) get synthetic
# names under R8; keep their SAM method overrides reachable so GetMethodID still finds
# them by name+signature.
-keepclassmembers class * implements com.printerdriver.internal.NativeJobEventCallback { *; }
-keepclassmembers class * implements com.printerdriver.internal.NativeDeviceEventCallback { *; }
-keepclassmembers class * implements com.printerdriver.internal.NativeLogCallback { *; }
-keepclassmembers class * implements com.printerdriver.internal.NativeTransportCallback { *; }

# Note: PrinterDriverException does NOT need a keep rule. Native functions signal
# failure with a 0L sentinel handle; the corresponding `throw PrinterDriverException(...)`
# happens entirely in Kotlin (PrinterDriver.create/addPrinterTcp, Printer.print), never
# via a native FindClass+ThrowNew lookup by binary name, so ordinary R8 renaming is safe.
