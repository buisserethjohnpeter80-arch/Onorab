import { Injectable } from '@angular/core';
import { BehaviorSubject, type Observable } from 'rxjs';
import type { OnorabFile } from '../models';

/**
 * Shared "now playing" selection between Library and Player.
 * Holds only file metadata — never audio bytes and never a token.
 * The Player page owns the actual HTMLAudioElement (see player.page.ts).
 */
@Injectable({ providedIn: 'root' })
export class PlayerStateService {
  private readonly currentSubject = new BehaviorSubject<OnorabFile | null>(null);
  readonly currentFile$: Observable<OnorabFile | null> = this.currentSubject.asObservable();

  select(file: OnorabFile): void {
    this.currentSubject.next({ ...file });
  }

  get currentFile(): OnorabFile | null {
    return this.currentSubject.value;
  }
}
