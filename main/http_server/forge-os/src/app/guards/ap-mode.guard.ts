import { CanActivateFn, Router } from '@angular/router';
import { inject } from '@angular/core';
import { Observable, map, catchError, of } from 'rxjs';
import { SystemService } from '../services/system.service';

export const ApModeGuard: CanActivateFn = (): Observable<boolean> => {
  const systemService = inject(SystemService);
  const router = inject(Router);

  return systemService.getInfo().pipe(
    map(info => {
      const requestFromAp = (info as any).requestFromAp || false;
      const isWifiConnected = info.wifiStatus && info.wifiStatus.includes('Connected');

      if (info.apEnabled && !isWifiConnected) {
        // AP mode only (no WiFi configured) - show setup page
        router.navigate(['/ap']);
        return false;
      } else if (requestFromAp && info.apEnabled && isWifiConnected) {
        // Only redirect to ipinfo if backend confirms request is from AP subnet
        window.location.href = '/ipinfo';
        return false;
      }
      return true;
    }),
    catchError(() => of(true))
  );
};