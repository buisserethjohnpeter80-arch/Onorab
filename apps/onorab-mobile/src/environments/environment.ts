import { environment as prod } from './environment.prod';

/**
 * Development environment. `mockMode` is the Phase 1 default: the app
 * renders mock files and never talks to Google Drive or a real Hub.
 * Flip it to false only when a real Hub is reachable (see hub.service.ts).
 */
export const environment = {
  ...prod,
  production: false,
  mockMode: true,
};
