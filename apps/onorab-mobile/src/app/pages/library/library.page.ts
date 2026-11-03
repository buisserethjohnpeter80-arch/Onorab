import { Component, OnInit } from '@angular/core';
import { CommonModule } from '@angular/common';
import {
  IonButton,
  IonContent,
  IonHeader,
  IonItem,
  IonLabel,
  IonList,
  IonSearchbar,
  IonTitle,
  IonToolbar,
} from '@ionic/angular/standalone';
import { Router } from '@angular/router';
import { environment } from '../../../environments/environment';
import type { OnorabFile, SyncState } from '../../models';
import { HubService } from '../../services/hub.service';
import { MockDataService } from '../../services/mock-data.service';
import { PlayerStateService } from '../../services/player-state.service';
import { SyncIndicatorComponent } from '../../components/sync-indicator/sync-indicator.component';

/** Cloud Library: file list + search. Errors are shown, never swallowed. */
@Component({
  selector: 'app-library',
  standalone: true,
  imports: [
    CommonModule, IonHeader, IonToolbar, IonTitle, IonContent, IonSearchbar,
    IonList, IonItem, IonLabel, IonButton, SyncIndicatorComponent,
  ],
  template: `
    <ion-header><ion-toolbar>
      <ion-title>Cloud Library</ion-title>
      <app-sync-indicator [state]="syncState"></app-sync-indicator>
    </ion-toolbar></ion-header>
    <ion-content class="ion-padding">
      <div *ngIf="isMock" class="mock-banner">
        MOCK MODE — showing mock files. Real Drive files appear once the
        Android OAuth client is configured (Phase 2).
      </div>
      <div *ngIf="error" class="error-box">{{ error }}</div>
      <ion-searchbar placeholder="Search audio files"
        (ionInput)="onSearch($event.detail.value ?? '')"></ion-searchbar>
      <p *ngIf="!loading && files.length === 0" class="muted">No audio files found.</p>
      <p *ngIf="loading" class="muted">Loading…</p>
      <ion-list>
        <ion-item *ngFor="let file of files">
          <ion-label>
            <h2>{{ file.name }}</h2>
            <p>{{ file.mimeType }} · {{ file.sizeBytes | number }} bytes · via {{ file.source }}</p>
            <p class="muted">{{ file.modifiedTime }}</p>
          </ion-label>
          <ion-button fill="clear" color="primary" (click)="openPlayer(file)">Play</ion-button>
          <ion-button fill="clear" color="secondary" (click)="download(file)">Download</ion-button>
        </ion-item>
      </ion-list>
      <ion-button expand="block" fill="outline" (click)="reload()">Refresh</ion-button>
    </ion-content>
  `,
})
export class LibraryPage implements OnInit {
  files: OnorabFile[] = [];
  error: string | null = null;
  loading = false;
  syncState: SyncState = 'mock';
  readonly isMock = environment.mockMode;
  private searchQuery = '';

  constructor(
    private mock: MockDataService,
    private hub: HubService,
    private playerState: PlayerStateService,
    private router: Router,
  ) {}

  ngOnInit(): void {
    this.reload();
  }

  reload(): void {
    this.loading = true;
    this.error = null;
    if (environment.mockMode) {
      this.syncState = 'mock';
      this.mock.getFiles().subscribe({
        next: (files) => { this.files = files; this.loading = false; },
        // Mock data cannot fail in practice; keep the handler so a
        // future regression still surfaces instead of hanging.
        error: (err: unknown) => this.fail(err),
      });
      return;
    }
    this.syncState = 'syncing';
    this.hub.getFiles().subscribe({
      next: (files) => {
        this.files = this.applySearch(files);
        this.syncState = 'connected';
        this.loading = false;
      },
      error: (err: unknown) => this.fail(err),
    });
  }

  /** Client-side name filter, applied identically in both modes. */
  private applySearch(files: OnorabFile[]): OnorabFile[] {
    const q = this.searchQuery.trim().toLowerCase();
    return q ? files.filter((f) => f.name.toLowerCase().includes(q)) : files;
  }

  onSearch(query: string): void {
    this.searchQuery = query;
    if (environment.mockMode) {
      this.mock.searchFiles(query).subscribe({ next: (f) => (this.files = f), error: (e: unknown) => this.fail(e) });
      return;
    }
    // Hub mode: reload from the Hub, then filter the fresh list
    // (applySearch runs in the reload success handler above).
    this.reload();
  }

  openPlayer(file: OnorabFile): void {
    this.playerState.select(file);
    void this.router.navigate(['/tabs/player']);
  }

  download(file: OnorabFile): void {
    if (environment.mockMode) {
      // Honest mock behaviour: there are no Drive bytes to download in
      // Phase 1, except the bundled demo tone (handled by the Player).
      this.error = file.previewUrl
        ? `Mock download: "${file.name}" is the bundled demo file — open it in Player to hear it.`
        : `Mock download: "${file.name}" has no downloadable bytes in Phase 1 (mock data only). Real downloads arrive with Hub + Drive in Phase 2.`;
      return;
    }
    window.open(this.hub.downloadUrl(file.id), '_blank');
  }

  private fail(err: unknown): void {
    this.loading = false;
    this.syncState = environment.mockMode ? 'mock' : 'disconnected';
    this.error = err instanceof Error ? err.message : `Failed to load files: ${String(err)}`;
  }
}
