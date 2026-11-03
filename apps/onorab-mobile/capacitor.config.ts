import type { CapacitorConfig } from '@capacitor/cli';

// Phase 1: Android first. `webDir` is the Angular build output that
// Capacitor copies into the native Android project on `npx cap sync`.
// No OAuth client id, token, or secret belongs in this file (see README).
const config: CapacitorConfig = {
  appId: 'com.potoriktrack.onorab',
  appName: 'Onorab',
  webDir: 'www/browser',
  // Keep the bundled web app navigable only to itself in Phase 1.
  server: {
    androidScheme: 'https',
  },
};

export default config;
