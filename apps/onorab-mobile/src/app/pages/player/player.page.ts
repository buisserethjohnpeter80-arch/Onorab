import { AfterViewInit, Component, ElementRef, OnDestroy, ViewChild } from '@angular/core';
import { CommonModule } from '@angular/common';
import {
  IonButton,
  IonContent,
  IonHeader,
  IonRange,
  IonTitle,
  IonToolbar,
} from '@ionic/angular/standalone';
import { Subscription } from 'rxjs';
import type { OnorabFile } from '../../models';
import { PlayerStateService } from '../../services/player-state.service';
import { SyncIndicatorComponent } from '../../components/sync-indicator/sync-indicator.component';

/**
 * Audio player, built on a plain HTMLAudioElement (no audio plugin
 * dependency in Phase 1). Play / pause / stop + seek + time display.
 *
 * Waveform: the canvas bars are a deterministic placeholder derived
 * from the file id — they visualise "a waveform exists here", they are
 * NOT decoded from the file's audio samples. Real sample-accurate
 * waveform decoding (Web Audio / AudioContext decodeAudioData) is a
 * Phase 2 item and is labelled as such in the UI note below.
 *
 * Playback source: the selected file's `previewUrl` when present
 * (today only the bundled demo tone, assets/sample.wav). A mock Drive
 * file without bytes shows a plain message instead of fake playback.
 */
@Component({
  selector: 'app-player',
  standalone: true,
  imports: [
    CommonModule, IonHeader, IonToolbar, IonTitle, IonContent, IonButton,
    IonRange, SyncIndicatorComponent,
  ],
  template: `
    <ion-header><ion-toolbar>
      <ion-title>Player</ion-title>
      <app-sync-indicator state="mock"></app-sync-indicator>
    </ion-toolbar></ion-header>
    <ion-content class="ion-padding">
      <div *ngIf="!file" class="muted">
        No file selected. Open the Library and tap Play on a file.
      </div>
      <div *ngIf="file">
        <h2>{{ file.name }}</h2>
        <p class="muted">{{ file.mimeType }} · {{ file.sizeBytes | number }} bytes</p>
        <canvas #waveform width="320" height="80"
          style="width:100%; background:#1e1f24; border-radius:8px;"></canvas>
        <p class="muted" style="font-size:12px;">
          Waveform bars are a Phase 1 placeholder (seeded from the file id),
          not decoded audio samples.
        </p>
        <p>{{ formatTime(currentTime) }} / {{ formatTime(duration) }}</p>
        <ion-range [min]="0" [max]="duration || 0" [value]="currentTime"
          (ionChange)="seek($event.detail.value)"></ion-range>
        <div *ngIf="error" class="error-box">{{ error }}</div>
        <div *ngIf="!file.previewUrl" class="mock-banner">
          No playable audio for this file in mock mode — it is a Drive
          description without bytes. Play the bundled
          "onorab-demo-tone.wav" from the Library to hear real audio.
        </div>
        <ion-button color="primary" (click)="play()" [disabled]="!file.previewUrl">Play</ion-button>
        <ion-button color="secondary" (click)="pause()" [disabled]="!file.previewUrl">Pause</ion-button>
        <ion-button fill="outline" (click)="stop()" [disabled]="!file.previewUrl">Stop</ion-button>
      </div>
    </ion-content>
  `,
})
export class PlayerPage implements AfterViewInit, OnDestroy {
  @ViewChild('waveform') waveformRef?: ElementRef<HTMLCanvasElement>;

  file: OnorabFile | null = null;
  currentTime = 0;
  duration = 0;
  error: string | null = null;

  private audio: HTMLAudioElement | null = null;
  private sub: Subscription | null = null;

  constructor(private playerState: PlayerStateService) {}

  ngAfterViewInit(): void {
    this.sub = this.playerState.currentFile$.subscribe((file) => {
      this.file = file;
      this.attachAudio();
      this.drawWaveform();
    });
    // If a file was selected before this page was created, the
    // BehaviorSubject replays it immediately on subscribe (above).
    if (this.playerState.currentFile && !this.file) {
      this.file = this.playerState.currentFile;
      this.attachAudio();
      this.drawWaveform();
    }
  }

  ngOnDestroy(): void {
    this.sub?.unsubscribe();
    this.audio?.pause();
    this.audio = null;
  }

  play(): void {
    this.error = null;
    if (!this.audio) {
      this.error = 'No playable audio loaded for this file.';
      return;
    }
    this.audio.play().catch((err: unknown) => {
      this.error = `Playback failed: ${err instanceof Error ? err.message : String(err)}`;
    });
  }

  pause(): void {
    this.audio?.pause();
  }

  stop(): void {
    if (!this.audio) return;
    this.audio.pause();
    this.audio.currentTime = 0;
    this.currentTime = 0;
  }

  seek(value: number | number[] | { lower: number; upper: number }): void {
    // IonRange emits a number for a single knob, or a {lower, upper}
    // object for a dual knob. This page uses a single knob.
    const target = Array.isArray(value) ? value[0] : typeof value === 'number' ? value : value.lower;
    if (this.audio && Number.isFinite(target)) {
      this.audio.currentTime = target;
    }
  }

  formatTime(seconds: number): string {
    if (!Number.isFinite(seconds) || seconds < 0) return '0:00';
    const m = Math.floor(seconds / 60);
    const s = Math.floor(seconds % 60);
    return `${m}:${s.toString().padStart(2, '0')}`;
  }

  private attachAudio(): void {
    this.audio?.pause();
    this.audio = null;
    this.currentTime = 0;
    this.duration = 0;
    this.error = null;
    if (!this.file?.previewUrl) return;
    this.audio = new Audio(this.file.previewUrl);
    this.audio.addEventListener('timeupdate', () => {
      this.currentTime = this.audio?.currentTime ?? 0;
    });
    this.audio.addEventListener('loadedmetadata', () => {
      this.duration = this.audio?.duration ?? 0;
    });
    this.audio.addEventListener('error', () => {
      this.error = 'Could not load this audio file for playback.';
    });
  }

  /** Deterministic placeholder bars, seeded from the file id. */
  private drawWaveform(): void {
    const canvas = this.waveformRef?.nativeElement;
    if (!canvas || !this.file) return;
    const ctx = canvas.getContext('2d');
    if (!ctx) return;
    ctx.clearRect(0, 0, canvas.width, canvas.height);
    let seed = 0;
    for (const ch of this.file.id) seed = (seed * 31 + ch.charCodeAt(0)) % 100000;
    ctx.fillStyle = '#22d3ee';
    const bars = 64;
    const gap = canvas.width / bars;
    for (let i = 0; i < bars; i++) {
      seed = (seed * 1103515245 + 12345) % 2147483648;
      const h = 8 + (seed % 56);
      ctx.fillRect(i * gap + 1, (canvas.height - h) / 2, gap - 2, h);
    }
  }
}
