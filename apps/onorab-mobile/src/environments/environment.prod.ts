/**
 * Production environment. Mock mode stays ON in Phase 1 even for
 * production builds: there is no Android OAuth client yet, so a
 * "real" build must not pretend to be connected. Phase 2 flips this
 * only together with the Android OAuth client configuration.
 */
export const environment = {
  production: true,
  mockMode: true,
};
