import { Component, Input } from '@angular/core';
import { IonBadge } from '@ionic/angular/standalone';
import type { SyncState } from '../../models';

/**
 * Small sync pill used in every page header.
 * Colors: green = connected, cyan = syncing, violet = mock mode,
 * grey = disconnected. The label is always text, never color alone.
 */
@Component({
  selector: 'app-sync-indicator',
  standalone: true,
  imports: [IonBadge],
  template: `
    <ion-badge [color]="badgeColor">{{ label }}</ion-badge>
  `,
})
export class SyncIndicatorComponent {
  @Input() state: SyncState = 'mock';

  get label(): string {
    switch (this.state) {
      case 'connected':
        return 'Hub connected';
      case 'syncing':
        return 'Syncing…';
      case 'disconnected':
        return 'Hub disconnected';
      case 'mock':
      default:
        return 'MOCK MODE';
    }
  }

  get badgeColor(): string {
    switch (this.state) {
      case 'connected':
        return 'success';
      case 'syncing':
        return 'primary';
      case 'disconnected':
        return 'medium';
      case 'mock':
      default:
        return 'secondary';
    }
  }
}
