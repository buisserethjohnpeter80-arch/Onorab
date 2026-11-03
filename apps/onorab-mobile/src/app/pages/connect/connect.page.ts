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
import { SyncIndicatorComponent } from '../../components/sync-indicator/sync-indicator.component';

/**
 * Google account connection screen — Phase 1 = MOCK MODE, stated on screen.
 *
 * There is NO Android OAuth client configured yet, so there is deliberately:
 * - no client id hardcoded anywhere in this app,
 * - no sign-in that stores a token,
 * - no reuse of the Hub's Desktop OAuth client (the Android app must use
 *   its own Android OAuth client later — see README "What is NOT done").
 *
 * The "Connect Google" button is disabled with the reason next to it.
 * "Continue in mock mode" lets the rest of the Phase 1 app be exercised
 * without pretending a Google account is connected.
 */
@Component({
  selector: 'app-connect',
  standalone: true,
  imports: [
    IonHeader, IonToolbar, IonTitle, IonContent, IonCard, IonCardHeader,
    IonCardTitle, IonCardContent, IonButton, SyncIndicatorComponent, RouterLink,
  ],
  template: `
    <ion-header><ion-toolbar>
      <ion-title>Connect</ion-title>
      <app-sync-indicator state="mock"></app-sync-indicator>
    </ion-toolbar></ion-header>
    <ion-content class="ion-padding">
      <div class="mock-banner">
        <strong>MOCK MODE</strong> — No Google account is connected.
        Real Android sign-in arrives in Phase 2, once a dedicated Android
        OAuth client is configured. Nothing on this screen signs you in
        or stores a token.
      </div>
      <ion-card>
        <ion-card-header><ion-card-title>Google Drive</ion-card-title></ion-card-header>
        <ion-card-content>
          <p>Onorab will use Google Drive (drive.file scope only) to move
          audio between your phone, the Hub, and FL Studio.</p>
          <p class="muted">Status: not connected (mock mode).</p>
          <ion-button expand="block" color="primary" [disabled]="true">
            Connect Google — coming in Phase 2
          </ion-button>
          <ion-button expand="block" fill="outline" color="secondary" routerLink="/tabs/library">
            Continue in mock mode
          </ion-button>
        </ion-card-content>
      </ion-card>
    </ion-content>
  `,
})
export class ConnectPage {}
