import { HttpClient, HttpErrorResponse, HttpEventType } from '@angular/common/http';
import { Injectable } from '@angular/core';
import { Capacitor } from '@capacitor/core';
import { Observable, catchError, map, throwError } from 'rxjs';
import type { OnorabFile, Transfer } from '../models';

/**
 * Client for the Onorab Hub (apps/onorab-hub), used ONLY when
 * `environment.mockMode` is false.
 *
 * Host resolution:
 * - Android emulator: the emulator's host loopback is 10.0.2.2, so the
 *   Hub on the developer's PC (listening on localhost:18741) is reached
 *   at http://10.0.2.2:18741. Plain `localhost` inside an emulator
 *   would point at the emulated phone itself, not the PC — that is the
 *   most common "Hub unreachable" mistake, so it is called out here.
 * - Real device on the same Wi-Fi: replace the base URL with the PC's
 *   LAN IP (e.g. http://192.168.1.42:18741). That IP is intentionally
 *   NOT hardcoded here — it changes per network. Phase 2 should add a
 *   Hub address field in Settings instead of an edit-and-rebuild step.
 * - Browser (`ionic serve` / `npm start`): localhost works because the
 *   browser and the Hub share the same machine.
 *
 * Errors are never swallowed: every method converts an HTTP failure
 * into a readable Error and rethrows it, so pages can show it.
 */
@Injectable({ providedIn: 'root' })
export class HubService {
  /** Default Hub base URL. Overridable per build via HUB_BASE_URL below. */
  readonly baseUrl: string;

  constructor(private http: HttpClient) {
    // Build-time override point (e.g. `HUB_BASE_URL=http://192.168.1.42:18741`
    // wired through an environment file in Phase 2). Defaults:
    // emulator -> 10.0.2.2, everything else -> localhost.
    const override = (globalThis as Record<string, unknown>)['__ONORAB_HUB_URL__'];
    if (typeof override === 'string' && override.length > 0) {
      this.baseUrl = override;
    } else if (Capacitor.isNativePlatform()) {
      this.baseUrl = 'http://10.0.2.2:18741';
    } else {
      this.baseUrl = 'http://localhost:18741';
    }
  }

  /** GET /health — used by the sync indicator. */
  checkHealth(): Observable<boolean> {
    return this.http.get<{ status?: string }>(`${this.baseUrl}/health`).pipe(
      map((res) => res?.status === 'ok'),
      catchError((err: unknown) => throwError(() => this.toError('Hub health check failed', err))),
    );
  }

  /** GET /files — Cloud Library listing from the Hub. */
  getFiles(): Observable<OnorabFile[]> {
    return this.http.get<OnorabFile[]>(`${this.baseUrl}/files`).pipe(
      catchError((err: unknown) => throwError(() => this.toError('Could not load files from the Hub', err))),
    );
  }

  /** GET /transfers — live transfer list from the Hub. */
  getTransfers(): Observable<Transfer[]> {
    return this.http.get<Transfer[]>(`${this.baseUrl}/transfers`).pipe(
      catchError((err: unknown) => throwError(() => this.toError('Could not load transfers from the Hub', err))),
    );
  }

  /** Download URL for a file (opened by the Player / download button). */
  downloadUrl(fileId: string): string {
    return `${this.baseUrl}/downloads/${encodeURIComponent(fileId)}`;
  }

  /**
   * POST /uploads — multipart upload of one audio file.
   * Emits upload progress percentages (0–100) and completes at 100.
   * The caller shows progress; this method does not hide failures.
   */
  upload(file: File): Observable<number> {
    const form = new FormData();
    form.append('file', file, file.name);
    return this.http
      .post(`${this.baseUrl}/uploads`, form, { observe: 'events', reportProgress: true })
      .pipe(
        map((event) => {
          if (event.type === HttpEventType.UploadProgress && event.total) {
            return Math.round((100 * event.loaded) / event.total);
          }
          return 100;
        }),
        catchError((err: unknown) => throwError(() => this.toError('Upload to the Hub failed', err))),
      );
  }

  /** Turn any HTTP failure into a readable Error (never a silent null). */
  private toError(context: string, err: unknown): Error {
    if (err instanceof HttpErrorResponse) {
      // Narrow `error` (typed `any` by Angular, `unknown` here) before
      // reading anything off it, so strict mode stays happy.
      const body: unknown = (err as { error?: unknown }).error;
      let detail: string;
      if (typeof body === 'string') {
        detail = body;
      } else if (
        body !== null &&
        typeof body === 'object' &&
        'message' in body &&
        typeof (body as { message: unknown }).message === 'string'
      ) {
        detail = (body as { message: string }).message;
      } else {
        // HttpErrorResponse always carries message/status in a real
        // build; narrow defensively so strict mode holds regardless.
        const httpErr = err as { message?: unknown; status?: unknown };
        detail =
          typeof httpErr.message === 'string'
            ? httpErr.message
            : `HTTP ${String(httpErr.status ?? '?')}`;
      }
      return new Error(`${context}: ${detail} (Hub: ${this.baseUrl})`);
    }
    return new Error(`${context}: ${err instanceof Error ? err.message : String(err)}`);
  }
}
