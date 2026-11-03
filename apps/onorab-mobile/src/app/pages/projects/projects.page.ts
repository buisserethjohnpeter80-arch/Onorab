import { Component, OnInit } from '@angular/core';
import { CommonModule } from '@angular/common';
import {
  IonCard,
  IonCardContent,
  IonCardHeader,
  IonCardTitle,
  IonContent,
  IonHeader,
  IonTitle,
  IonToolbar,
} from '@ionic/angular/standalone';
import { forkJoin } from 'rxjs';
import { environment } from '../../../environments/environment';
import type { OnorabFile, Project } from '../../models';
import { MockDataService } from '../../services/mock-data.service';
import { SyncIndicatorComponent } from '../../components/sync-indicator/sync-indicator.component';

/**
 * Projects: mock projects with their files listed underneath.
 * In Hub mode (Phase 2) this page will group Hub files by projectId;
 * Phase 1 shows mock projects only and says so.
 */
@Component({
  selector: 'app-projects',
  standalone: true,
  imports: [
    CommonModule, IonHeader, IonToolbar, IonTitle, IonContent, IonCard,
    IonCardHeader, IonCardTitle, IonCardContent, SyncIndicatorComponent,
  ],
  template: `
    <ion-header><ion-toolbar>
      <ion-title>Projects</ion-title>
      <app-sync-indicator state="mock"></app-sync-indicator>
    </ion-toolbar></ion-header>
    <ion-content class="ion-padding">
      <div *ngIf="isMock" class="mock-banner">MOCK MODE — mock projects only.</div>
      <div *ngIf="error" class="error-box">{{ error }}</div>
      <p *ngIf="projects.length === 0 && !error" class="muted">No projects yet.</p>
      <ion-card *ngFor="let project of projects">
        <ion-card-header><ion-card-title>{{ project.name }}</ion-card-title></ion-card-header>
        <ion-card-content>
          <p *ngFor="let file of filesFor(project)" class="muted">
            {{ file.name }} · via {{ file.source }}
          </p>
          <p *ngIf="filesFor(project).length === 0" class="muted">No files in this project.</p>
        </ion-card-content>
      </ion-card>
      <p *ngIf="unassigned.length > 0" class="muted">Unassigned files:</p>
      <p *ngFor="let file of unassigned" class="muted">{{ file.name }}</p>
    </ion-content>
  `,
})
export class ProjectsPage implements OnInit {
  readonly isMock = environment.mockMode;
  projects: Project[] = [];
  files: OnorabFile[] = [];
  error: string | null = null;

  constructor(private mock: MockDataService) {}

  ngOnInit(): void {
    if (!environment.mockMode) {
      // Hub has no /projects endpoint in Phase 1 — say so, don't fake it.
      this.error = 'Projects from the Hub are not available in Phase 1 (no Hub projects endpoint yet).';
      return;
    }
    forkJoin({ projects: this.mock.getProjects(), files: this.mock.getFiles() }).subscribe({
      next: ({ projects, files }) => { this.projects = projects; this.files = files; },
      error: (err: unknown) => {
        this.error = err instanceof Error ? err.message : `Failed to load projects: ${String(err)}`;
      },
    });
  }

  filesFor(project: Project): OnorabFile[] {
    return this.files.filter((f) => project.fileIds.includes(f.id));
  }

  get unassigned(): OnorabFile[] {
    return this.files.filter((f) => f.projectId === null);
  }
}
