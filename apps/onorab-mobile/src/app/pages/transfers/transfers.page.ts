import { Component, OnInit } from '@angular/core';
import { CommonModule } from '@angular/common';
import {
  IonBadge,
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
import type { Transfer } from '../../models';
import { HubService } from '../../services/hub.service';
import { MockDataService } from '../../services/mock-data.service';
import { SyncIndicatorComponent } from '../../components/sync-indicator/sync-indicator.component';

/** Transfers: upload/download history with status + progress + errors. */
@Component({
  selector: 'app-transfers',
  standalone: true,
  imports: [
    CommonModule, IonHeader, IonToolbar, IonTitle, IonContent, IonList,
    IonItem, IonLabel, IonBadge, IonProgressBar, SyncIndicatorComponent,
  ],
  template: `
    <ion-header><ion-toolbar>
      <ion-title>Transfers</ion-title>
      <app-sync-indicator state="mock"></app-sync-indicator>
    </ion-toolbar></ion-header>
    <ion-content class="ion-padding">
      <div *ngIf="isMock" class="mock-banner">MOCK MODE — mock transfer history.</div>
      <div *ngIf="error" class="error-box">{{ error }}</div>
      <p *ngIf="transfers.length === 0 && !error" class="muted">No transfers yet.</p>
      <ion-list>
        <ion-item *ngFor="let t of transfers">
          <ion-label>
            <h2>{{ t.fileName }}</h2>
            <p>{{ t.direction }} · {{ t.status }} · {{ t.progress }}%</p>
            <p *ngIf="t.error" style="color:#fca5a5;">{{ t.error }}</p>
          </ion-label>
          <ion-badge [color]="badgeFor(t.status)">{{ t.status }}</ion-badge>
        </ion-item>
        <ion-progress-bar *ngFor="let t of transfers"
          [value]="t.progress / 100"
          [color]="t.status === 'failed' ? 'danger' : 'primary'"></ion-progress-bar>
      </ion-list>
    </ion-content>
  `,
})
export class TransfersPage implements OnInit {
  readonly isMock = environment.mockMode;
  transfers: Transfer[] = [];
  error: string | null = null;

  constructor(private mock: MockDataService, private hub: HubService) {}

  ngOnInit(): void {
    const source = environment.mockMode ? this.mock.getTransfers() : this.hub.getTransfers();
    source.subscribe({
      next: (t) => (this.transfers = t),
      error: (err: unknown) => {
        this.error = err instanceof Error ? err.message : `Failed to load transfers: ${String(err)}`;
      },
    });
  }

  badgeFor(status: Transfer['status']): string {
    switch (status) {
      case 'completed': return 'success';
      case 'failed': return 'danger';
      case 'uploading':
      case 'downloading': return 'primary';
      case 'queued':
      default: return 'medium';
    }
  }
}
