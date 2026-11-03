import type { Routes } from '@angular/router';
import { TabsPage } from './tabs/tabs.page';
import { ConnectPage } from './pages/connect/connect.page';
import { LibraryPage } from './pages/library/library.page';
import { PlayerPage } from './pages/player/player.page';
import { UploadPage } from './pages/upload/upload.page';
import { ProjectsPage } from './pages/projects/projects.page';
import { TransfersPage } from './pages/transfers/transfers.page';
import { ProfilePage } from './pages/profile/profile.page';

/**
 * All pages are standalone components (no NgModule anywhere in the app).
 * TabsPage is the layout shell; each tab is a child route so the
 * bottom tab bar stays visible while switching pages.
 */
export const routes: Routes = [
  { path: '', redirectTo: 'tabs/connect', pathMatch: 'full' },
  {
    path: 'tabs',
    component: TabsPage,
    children: [
      { path: '', redirectTo: 'connect', pathMatch: 'full' },
      { path: 'connect', component: ConnectPage },
      { path: 'library', component: LibraryPage },
      { path: 'player', component: PlayerPage },
      { path: 'upload', component: UploadPage },
      { path: 'projects', component: ProjectsPage },
      { path: 'transfers', component: TransfersPage },
      { path: 'profile', component: ProfilePage },
    ],
  },
  { path: '**', redirectTo: 'tabs/connect' },
];
