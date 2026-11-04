import { NgModule } from '@angular/core';
import { RouterModule, Routes } from '@angular/router';

import { HomeComponent } from './components/home/home.component';
import { LogsComponent } from './components/logs/logs.component';
import { SettingsComponent } from './components/settings/settings.component';
import { NetworkComponent } from './components/network/network.component';
import { SwarmComponent } from './components/swarm/swarm.component';
import { PoolComponent } from './components/pool/pool.component';
import { AppLayoutComponent } from './layout/app.layout.component';
import { ApModeGuard } from './guards/ap-mode.guard';

const routes: Routes = [
  {
      path: 'ap',
      component: AppLayoutComponent,
      children: [
        {
          path: '',
          component: NetworkComponent,
          data: { title: 'Wi-Fi setup' }
        }
      ]
  },
  {
    path: '',
    component: AppLayoutComponent,
    canActivate: [ApModeGuard],
    children: [
      {
        path: '',
        component: HomeComponent,
        data: { title: 'Dashboard' }
      },
      {
        path: 'logs',
        component: LogsComponent,
        data: { title: 'Logs' }
      },
      {
        path: 'network',
        component: NetworkComponent,
        data: { title: 'Network' }
      },
      {
        path: 'settings',
        component: SettingsComponent,
        data: { title: 'Settings' }
      },
      {
        path: 'swarm',
        component: SwarmComponent,
        data: { title: 'Swarm' }
      },
      {
        path: 'pool',
        component: PoolComponent,
        data: { title: 'Pools' }
      }
    ]
  },

];

@NgModule({
  imports: [RouterModule.forRoot(routes)],
  exports: [RouterModule]
})
export class AppRoutingModule { }
