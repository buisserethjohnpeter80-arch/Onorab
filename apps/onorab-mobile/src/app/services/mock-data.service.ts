import { Injectable } from '@angular/core';
import { Observable, of } from 'rxjs';
import type { OnorabFile, Project, Transfer } from '../models';

/**
 * Phase 1 mock data. This is the ONLY data source while
 * `environment.mockMode` is true. Nothing here touches the network,
 * Google Drive, or any token — the files are descriptions, not bytes,
 * except `sample.wav`, a locally generated 2-second tone used so the
 * Player page has something real to play (see assets/sample.wav).
 */
@Injectable({ providedIn: 'root' })
export class MockDataService {
  private readonly files: OnorabFile[] = [
    {
      id: 'mock-001',
      name: 'onorab-demo-tone.wav',
      mimeType: 'audio/wav',
      sizeBytes: 176444,
      modifiedTime: '2026-11-02T14:30:00Z',
      projectId: 'proj-demo',
      source: 'phone',
      // The only mock file with playable bytes (bundled demo tone).
      previewUrl: 'assets/sample.wav',
    },
    {
      id: 'mock-002',
      name: 'vocal-idea-whatsapp.mp3',
      mimeType: 'audio/mpeg',
      sizeBytes: 2841320,
      modifiedTime: '2026-11-01T19:12:00Z',
      projectId: 'proj-demo',
      source: 'whatsapp',
    },
    {
      id: 'mock-003',
      name: 'fl-mixdown-capture.wav',
      mimeType: 'audio/wav',
      sizeBytes: 18432044,
      modifiedTime: '2026-10-31T22:05:00Z',
      projectId: 'proj-fl-session',
      source: 'hub',
    },
    {
      id: 'mock-004',
      name: 'beat-sketch-telegram.mp3',
      mimeType: 'audio/mpeg',
      sizeBytes: 4120560,
      modifiedTime: '2026-10-30T09:41:00Z',
      projectId: null,
      source: 'telegram',
    },
  ];

  private readonly projects: Project[] = [
    { id: 'proj-demo', name: 'Demo Project', fileIds: ['mock-001', 'mock-002'] },
    { id: 'proj-fl-session', name: 'FL Session — Nov 2026', fileIds: ['mock-003'] },
  ];

  private readonly transfers: Transfer[] = [
    {
      id: 'tr-001',
      fileId: 'mock-002',
      fileName: 'vocal-idea-whatsapp.mp3',
      direction: 'upload',
      status: 'completed',
      progress: 100,
      error: null,
    },
    {
      id: 'tr-002',
      fileId: 'mock-003',
      fileName: 'fl-mixdown-capture.wav',
      direction: 'download',
      status: 'completed',
      progress: 100,
      error: null,
    },
    {
      id: 'tr-003',
      fileId: 'mock-004',
      fileName: 'beat-sketch-telegram.mp3',
      direction: 'upload',
      status: 'failed',
      progress: 42,
      error: 'Mock failure example: Hub unreachable during upload (mock data).',
    },
  ];

  getFiles(): Observable<OnorabFile[]> {
    // Return copies so pages cannot mutate the mock store by accident.
    return of(this.files.map((f) => ({ ...f })));
  }

  getFile(id: string): Observable<OnorabFile | undefined> {
    return of(this.files.find((f) => f.id === id));
  }

  getProjects(): Observable<Project[]> {
    return of(this.projects.map((p) => ({ ...p, fileIds: [...p.fileIds] })));
  }

  getTransfers(): Observable<Transfer[]> {
    return of(this.transfers.map((t) => ({ ...t })));
  }

  /** Case-insensitive search over file names (Library page). */
  searchFiles(query: string): Observable<OnorabFile[]> {
    const q = query.trim().toLowerCase();
    if (!q) {
      return this.getFiles();
    }
    return of(this.files.filter((f) => f.name.toLowerCase().includes(q)));
  }
}
