import { enableProdMode } from '@angular/core';
import { bootstrapApplication } from '@angular/platform-browser';
import { provideHttpClient } from '@angular/common/http';
import { provideRouter } from '@angular/router';
import { provideIonicAngular } from '@ionic/angular/standalone';

import { AppComponent } from './app/app.component';
import { routes } from './app/app.routes';
import { environment } from './environments/environment';

if (environment.production) {
  enableProdMode();
}

bootstrapApplication(AppComponent, {
  providers: [
    provideRouter(routes),
    provideHttpClient(),
    // `mode: 'md'` gives the Android Material look on every platform,
    // including the browser preview, so the UI matches the target device.
    provideIonicAngular({ mode: 'md' }),
  ],
}).catch((err: unknown) => {
  // Never swallow a bootstrap failure silently: surface it in the console
  // so a blank screen is diagnosable instead of mysterious.
  console.error('Onorab failed to start', err);
});
