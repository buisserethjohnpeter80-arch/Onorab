import { Component } from '@angular/core';
import { CommonModule } from '@angular/common';
import {
  IonButton,
  IonContent,
  IonHeader,
  IonItem,
  IonLabel,
  IonList,
  IonProgressBar,
  IonTitle,
  IonToolbar,
} from '@ionic/angular/standalone';
import { environment } from '../../../environments/environment';
import type { SharedFileRef } from '../../services/share-intent.service';
import { HubService } from '../../services/hub.service';
import { ShareIntentService } from '../../services/share-intent.service';
import { SyncIndicatorComponent } from '../../components/sync-indicator/sync-indicator.component';
import type { Observable } from 'rxjs';

/**
 * Upload / download controls.
 * - File picker: a plain `<input type="file" accept="audio/*">`, which
 *   works in the browser preview and on Android (system file picker).
 * - Mock mode: upload is simulated in the UI only — it is labelled as
 *   a mock, no bytes leave the device, and no Drive upload is claimed.
 * - Hub mode: real multipart POST to the Hub (see hub.service.ts)
 *   with genuine HttpEventType.UploadProgress percentages.
 * - Shared files: shows whatever ShareIntentService has pending. In
 *   Phase 1 that list is empty on a real device (import not wired yet,
 *   see share-intent.service.ts) — the page says so instead of hiding it.
 */
@Component({
  selector: 'app-upload',
  standalone: true,
  imports: [
    CommonModule, IonHeader, IonToolbar, IonTitle, IonContent, IonButton,
    IonList, IonItem, IonLabel, IonProgressBar, SyncIndicatorComponent,
  ],
  template: `
    <ion-header><ion-toolbar>
      <ion-title>Upload</ion-title>
      <app-sync-indicator state="mock"></app-sync-indicator>
    </ion-toolbar></ion-header>
    <ion-content class="ion-padding">
      <div *ngIf="isMock" class="mock-banner">
        MOCK MODE — uploads here do not reach Google Drive in Phase 1.
        Real upload (phone → Hub → Drive) arrives in Phase 2.
      </div>
      <div *ngIf="error" class="error-box">{{ error }}</div>
      <div *ngIf="info" style="background:#12291a;border:1px solid #22c55e;border-radius:8px;padding:12px;margin:12px 0;">
        {{ info }}
      </div>

      <h3>Shared to Onorab</h3>
      <p *ngIf="(pending$ | async)?.length === 0" class="muted">
        No shared files pending. Sharing audio to Onorab from another app
        opens the app (manifest Share Target), but importing the shared
        file's bytes is not wired yet in Phase 1 — see README.
      </p>
      <ion-list>
        <ion-item *ngFor="let f of (pending$ | async) ?? []">
          <ion-label><h2>{{ f.name }}</h2><p class="muted">{{ f.uri }}</p></ion-label>
        </ion-item>
      </ion-list>

      <h3>Pick an audio file</h3>
      <input type="file" accept="audio/*" (change)="onFilePicked($event)" />
      <p *ngIf="selectedFile" class="muted">
        Selected: {{ selectedFile.name }} ({{ selectedFile.size | number }} bytes)
      </p>
      <ion-button expand="block" color="primary" (click)="upload()"
        [disabled]="!selectedFile || uploading">Upload</ion-button>
      <ion-progress-bar *ngIf="uploading" [value]="progress / 100" color="primary"></ion-progress-bar>
      <p *ngIf="uploading" class="muted">Upload progress: {{ progress }}%</p>
    </ion-content>
  `,
})
export class UploadPage {
  readonly isMock = environment.mockMode;
  /** Assigned in the constructor (not a field initializer) so the
   * injected ShareIntentService is definitely initialized first. */
  readonly pending$: Observable<SharedFileRef[]>;

  selectedFile: File | null = null;
  uploading = false;
  progress = 0;
  error: string | null = null;
  info: string | null = null;

  constructor(private hub: HubService, private share: ShareIntentService) {
    this.pending$ = this.share.pendingSharedFiles$;
  }

  onFilePicked(event: Event): void {
    const input = event.target as HTMLInputElement;
    this.selectedFile = input.files?.[0] ?? null;
    this.error = null;
    this.info = null;
    this.progress = 0;
  }

  upload(): void {
    if (!this.selectedFile) return;
    this.error = null;
    this.info = null;
    if (environment.mockMode) {
      // Labelled mock: animate the bar locally, claim nothing about Drive.
      this.uploading = true;
      this.progress = 0;
      const timer = window.setInterval(() => {
        this.progress += 25;
        if (this.progress >= 100) {
          window.clearInterval(timer);
          this.uploading = false;
          this.info =
            `Mock upload complete (UI simulation only): "${this.selectedFile?.name}" ` +
            'was NOT sent to Google Drive in Phase 1.';
          this.selectedFile = null;
        }
      }, 200);
      return;
    }
    this.uploading = true;
    this.hub.upload(this.selectedFile).subscribe({
      next: (pct) => (this.progress = pct),
      error: (err: unknown) => {
        this.uploading = false;
        this.error = err instanceof Error ? err.message : `Upload failed: ${String(err)}`;
      },
      complete: () => {
        this.uploading = false;
        this.info = 'Upload to the Hub finished.';
        this.selectedFile = null;
      },
    });
  }
}
