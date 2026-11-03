import { Component } from '@angular/core';
import { IonLabel, IonTabBar, IonTabButton, IonTabs } from '@ionic/angular/standalone';

/**
 * Bottom tab bar. `tab` + `href` is the canonical Ionic Angular tabs
 * pattern: the href matches a child route of /tabs (see app.routes.ts).
 */
@Component({
  selector: 'app-tabs',
  standalone: true,
  imports: [IonTabs, IonTabBar, IonTabButton, IonLabel],
  template: `
    <ion-tabs>
      <ion-tab-bar slot="bottom">
        <ion-tab-button tab="connect" href="/tabs/connect"><ion-label>Connect</ion-label></ion-tab-button>
        <ion-tab-button tab="library" href="/tabs/library"><ion-label>Library</ion-label></ion-tab-button>
        <ion-tab-button tab="player" href="/tabs/player"><ion-label>Player</ion-label></ion-tab-button>
        <ion-tab-button tab="upload" href="/tabs/upload"><ion-label>Upload</ion-label></ion-tab-button>
        <ion-tab-button tab="projects" href="/tabs/projects"><ion-label>Projects</ion-label></ion-tab-button>
        <ion-tab-button tab="transfers" href="/tabs/transfers"><ion-label>Transfers</ion-label></ion-tab-button>
        <ion-tab-button tab="profile" href="/tabs/profile"><ion-label>Profile</ion-label></ion-tab-button>
      </ion-tab-bar>
    </ion-tabs>
  `,
})
export class TabsPage {}
