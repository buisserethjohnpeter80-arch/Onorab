import { Component } from '@angular/core';
import {
  IonButton,
  IonCard,
  IonCardContent,
  IonCardHeader,
  IonCardTitle,
  IonContent,
  IonHeader,
  IonTitle,
  IonToolbar,
} from '@ionic/angular/standalone';
import { RouterLink } from '@angular/router';
import { environment } from '../../../environments/environment';
import { SyncIndicatorComponent } from '../../components/sync-indicator/sync-indicator.component';

/**
 * Profile page. Phase 1 shows a clearly-labelled mock account — there
 * is no real Google account, no email address, and no token to display,
 * and none is invented here.
 */
@Component({
  selector: 'app-profile',
  standalone: true,
  imports: [
    IonHeader, IonToolbar, IonTitle, IonContent, IonCard, IonCardHeader,
    IonCardTitle, IonCardContent, IonButton, SyncIndicatorComponent, RouterLink,
  ],
  template: `
    <ion-header><ion-toolbar>
      <ion-title>Profile</ion-title>
      <app-sync-indicator state="mock"></app-sync-indicator>
    </ion-toolbar></ion-header>
    <ion-content class="ion-padding">
      <div class="mock-banner">
        MOCK MODE — this is a placeholder account for Phase 1 testing,
        not a signed-in Google account.
      </div>
      <ion-card>
        <ion-card-header><ion-card-title>Mock User</ion-card-title></ion-card-header>
        <ion-card-content>
          <p class="muted">No Google account connected.</p>
          <p>App version: 0.1.0 (Phase 1 MVP)</p>
          <p>Mock mode: {{ mockMode ? 'ON' : 'OFF' }}</p>
          <p>Hub: not connected in mock mode.</p>
          <ion-button expand="block" fill="outline" routerLink="/tabs/connect">
            Go to Connect
          </ion-button>
        </ion-card-content>
      </ion-card>
    </ion-content>
  `,
})
export class ProfilePage {
  readonly mockMode = environment.mockMode;
}
