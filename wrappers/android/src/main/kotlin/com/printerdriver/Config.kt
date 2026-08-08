package com.printerdriver

/**
 * Driver-level configuration (pd_config). All-zeroes/all-defaults is a valid, safe
 * configuration (pd.h): no [storageDirectory] means in-memory, no journal, no crash
 * recovery -- fine for a quick test, not for a real POS deployment.
 */
data class PrinterDriverConfig(
    /** `null` -> in-memory: no journal, no recovery across restarts/crashes. */
    val storageDirectory: String? = null,
    /** `false` keeps the durability rule (fsync on every journal write). `true` is
     *  for tests only (pd.h) -- never set it in a shipping app. */
    val fsyncDisabled: Boolean = false,
    /** Bridges pd_config.log: ABI-level diagnostics from the native layer. Called on
     *  whatever thread produced the message -- see README.md "Threading contract". */
    val onLog: ((String) -> Unit)? = null
)

/**
 * Configuration for [PrinterDriver.addPrinterTcp] (pd_tcp_config).
 */
data class TcpPrinterConfig(
    val host: String,
    /** 0 -> 9100. */
    val port: Int = 0,
    /** `null` -> derived from the endpoint by the core. */
    val printerId: String? = null,
    /** 0 -> 576. 384, 504 and 576 are the deployed widths (docs/api.md §2). */
    val widthDots: Int = 0,
    /** `null`/`""` -> "generic". See [PrinterDriver.profileIds] for the accepted set. */
    val profileId: String? = null,
    /** 0 -> 3000. */
    val connectTimeoutMs: Int = 0
)
