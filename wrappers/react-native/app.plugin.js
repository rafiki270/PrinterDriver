/**
 * The Expo config plugin.
 *
 * WHAT IT IS FOR
 *   Expo Go CANNOT load this package: it contains native code, and Expo Go ships a fixed
 *   set of native modules. An Expo app needs a DEVELOPMENT BUILD (`npx expo prebuild`,
 *   `npx expo run:ios` / `run:android`, or EAS Build), which is the normal path for any
 *   native module and not a limitation of this one.
 *
 *   Inside that flow, `expo prebuild` regenerates the ios/ and android/ directories from
 *   app.json every time, so any hand edit to them is lost. This plugin makes the two
 *   changes the SDK needs survive that regeneration:
 *
 *     1. THE NEW ARCHITECTURE. The C++ TurboModule path requires it. The plugin sets
 *        `newArchEnabled` in gradle.properties and `RCT_NEW_ARCH_ENABLED` in the Podfile
 *        properties, and fails the prebuild loudly if the app has explicitly turned it
 *        off -- a silent fallback would produce an app whose printing API is simply
 *        missing at runtime, which is the worst possible way to learn about it.
 *
 *     2. LOCAL NETWORK ACCESS ON iOS. A TCP printer on port 9100 is on the local network,
 *        and iOS 14+ requires NSLocalNetworkUsageDescription before an app may reach it.
 *        Without it the first connection fails with a permission error that looks exactly
 *        like an offline printer. `pd_discover`'s subnet sweep needs it too.
 *
 *   Bluetooth is deliberately NOT configured here. This package does not own a Bluetooth
 *   stack -- an app supplies its own through a JavaScript transport (src/transports.ts) --
 *   so the permission strings belong to whichever BLE library the app chose, and adding
 *   them here would put a Bluetooth usage prompt in front of apps that only print over TCP.
 *
 * USAGE (app.json / app.config.js):
 *
 *   { "expo": { "plugins": [
 *       ["printerdriver-react-native", { "localNetworkUsageDescription": "…" }]
 *   ] } }
 */

const DEFAULT_LOCAL_NETWORK_DESCRIPTION =
  'This app connects to receipt printers on your local network.';

/**
 * Written against @expo/config-plugins, which is a peer of the Expo app rather than of
 * this package: an Expo project always has it, and a bare React Native project must never
 * be made to install it. So it is required lazily, and a bare project that somehow reaches
 * this file gets a readable error instead of a module-not-found trace.
 */
function loadConfigPlugins() {
  try {
    // eslint-disable-next-line @typescript-eslint/no-var-requires
    return require('@expo/config-plugins');
  } catch (error) {
    throw new Error(
      'printerdriver-react-native/app.plugin.js needs @expo/config-plugins, which comes ' +
        'with Expo. If this is a bare React Native app, remove the plugin entry from ' +
        'app.json — autolinking (react-native.config.js) already handles the package.'
    );
  }
}

module.exports = function withPrinterDriver(config, options = {}) {
  const { withInfoPlist, withGradleProperties, withPodfileProperties, createRunOncePlugin } =
    loadConfigPlugins();

  let next = config;

  // --- 1. iOS: local network usage description -------------------------------------------
  next = withInfoPlist(next, (infoPlistConfig) => {
    const description =
      options.localNetworkUsageDescription ??
      infoPlistConfig.modResults.NSLocalNetworkUsageDescription ??
      DEFAULT_LOCAL_NETWORK_DESCRIPTION;
    infoPlistConfig.modResults.NSLocalNetworkUsageDescription = description;
    return infoPlistConfig;
  });

  // --- 2. The New Architecture, on both platforms -----------------------------------------
  next = withGradleProperties(next, (gradleConfig) => {
    const properties = gradleConfig.modResults;
    const existing = properties.find(
      (entry) => entry.type === 'property' && entry.key === 'newArchEnabled'
    );
    if (existing !== undefined && existing.value === 'false') {
      throw new Error(
        'printerdriver-react-native requires the New Architecture, but newArchEnabled is ' +
          'false for this app. The SDK binds its C++ core through a C++ TurboModule, which ' +
          'the old architecture cannot load. Enable the New Architecture, or remove the ' +
          'package.'
      );
    }
    if (existing === undefined) {
      properties.push({ type: 'property', key: 'newArchEnabled', value: 'true' });
    }
    return gradleConfig;
  });

  next = withPodfileProperties(next, (podfileConfig) => {
    const properties = podfileConfig.modResults;
    if (properties['newArchEnabled'] === 'false') {
      throw new Error(
        'printerdriver-react-native requires the New Architecture, but newArchEnabled is ' +
          'false in this app\'s Podfile properties. See the Android message above: the ' +
          'reason is the same on both platforms.'
      );
    }
    properties['newArchEnabled'] = 'true';
    return podfileConfig;
  });

  // createRunOncePlugin keeps the plugin idempotent when several packages depend on it.
  return createRunOncePlugin(
    (innerConfig) => innerConfig,
    'printerdriver-react-native',
    require('./package.json').version
  )(next);
};
