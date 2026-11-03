/**
 * Shared Onorab protocol types for the mobile app.
 *
 * These field names intentionally mirror packages/protocol (the Hub's
 * shared JSON schemas): id, name, mimeType, sizeBytes, modifiedTime,
 * projectId, source. If the protocol changes, change it here too and
 * keep the two in sync — do not silently rename a field on one side.
 */

/** Where a file came from. `hub` = captured in FL Studio via the Hub. */
export type FileSource =
  | 'whatsapp'
  | 'telegram'
  | 'files'
  | 'hub'
  | 'phone'
  | 'other';

export interface OnorabFile {
  id: string;
  name: string;
  mimeType: string;
  sizeBytes: number;
  /** ISO 8601 timestamp, e.g. "2026-11-02T14:30:00Z". */
  modifiedTime: string;
  /** Project this file belongs to, or null for unassigned files. */
  projectId: string | null;
  source: FileSource;
  /**
   * Playable URL for local preview only. Present for the bundled demo
   * tone (assets/sample.wav); real Drive files get this only after a
   * real Hub download in Phase 2. Absent means "no playable bytes yet".
   */
  previewUrl?: string;
}

export type TransferStatus =
  | 'queued'
  | 'uploading'
  | 'downloading'
  | 'completed'
  | 'failed';

export interface Transfer {
  id: string;
  fileId: string;
  fileName: string;
  direction: 'upload' | 'download';
  status: TransferStatus;
  /** 0–100. Meaningful only while uploading/downloading. */
  progress: number;
  /** Human-readable error, or null when there is no error. Never empty-string. */
  error: string | null;
}

export interface Project {
  id: string;
  name: string;
  /** File ids belonging to this project. */
  fileIds: string[];
}

/** Sync state shown by the sync indicator in the header of every page. */
export type SyncState = 'mock' | 'connected' | 'disconnected' | 'syncing';
