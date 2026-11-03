import { Injectable } from '@angular/core';
import { Capacitor } from '@capacitor/core';
import { BehaviorSubject, type Observable } from 'rxjs';

/**
 * Incoming "Share to Onorab" intents — Phase 1 STUB, by design.
 *
 * What is wired today (AndroidManifest.xml):
 * - MainActivity declares ACTION_SEND / ACTION_SEND_MULTIPLE intent
 *   filters for audio types (plus an any-type fallback, documented in
 *   the manifest), so Android offers Onorab when the user shares audio
 *   from WhatsApp, Files, Telegram, or any other app.
 *
 * What is NOT wired yet (Phase 2):
 * - Reading the shared file's content URI out of the launch intent and
 *   copying its bytes into the app. That needs either the Capacitor
 *   App plugin's `appUrlOpen` / intent handling or a small dedicated
 *   share-target plugin, plus a native-side content-resolver read.
 *   Neither is faked here: on a real device today, sharing to Onorab
 *   opens the app (via the manifest filter) but the shared file is
 *   NOT yet imported — `pendingSharedFiles` stays empty and the
 *   Upload page says so plainly instead of pretending success.
 *
 * On the web (`npm start` / browser preview) there is no Android
 * intent system at all, so this service degrades gracefully: it
 * reports `isSupported === false` and never throws.
 */
export interface SharedFileRef {
  /** Display name when known, otherwise the raw URI / path. */
  name: string;
  /** Content URI on Android, object URL / path on web (Phase 2 fills this). */
  uri: string;
}

@Injectable({ providedIn: 'root' })
export class ShareIntentService {
  /** True only inside the native Android app (Capacitor runtime). */
  readonly isSupported: boolean = Capacitor.isNativePlatform();

  private readonly pendingSubject = new BehaviorSubject<SharedFileRef[]>([]);
  readonly pendingSharedFiles$: Observable<SharedFileRef[]> =
    this.pendingSubject.asObservable();

  /**
   * Entry point for Phase 2 native code: when the native side resolves
   * a SEND intent, it calls this with the shared files and the Upload
   * page picks them up. Today nothing calls it, which is honest:
   * receiving is declared in the manifest, importing is not done yet.
   */
  handleIncomingFiles(files: SharedFileRef[]): void {
    this.pendingSubject.next([...files]);
  }

  /** Clear the pending list after the user acts on it (or dismisses it). */
  clearPending(): void {
    this.pendingSubject.next([]);
  }
}
