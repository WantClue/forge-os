import { HttpClient, HttpEvent } from '@angular/common/http';
import { Injectable } from '@angular/core';
import { catchError, delay, EMPTY, exhaustMap, Observable, of, shareReplay, timer } from 'rxjs';
import { eASICModel } from 'src/models/enum/eASICModel';
import { ISystemInfo } from 'src/models/ISystemInfo';

import { environment } from '../../environments/environment';

@Injectable({
  providedIn: 'root'
})
export class SystemService {

  /**
   * Shared live poll of this device's /api/system/info. All views subscribe
   * to this one stream, so an open tab sends one request per interval no
   * matter how many components display the data. Treat emitted objects as
   * read-only; they are shared between subscribers.
   */
  public readonly info$: Observable<ISystemInfo>;

  private static readonly INFO_POLL_INTERVAL_MS = 3000;

  constructor(
    private httpClient: HttpClient
  ) {
    this.info$ = timer(0, SystemService.INFO_POLL_INTERVAL_MS).pipe(
      // Skip ticks while a request is still running instead of stacking them
      // up, and keep polling after a failed request
      exhaustMap(() => this.getInfo().pipe(catchError(() => EMPTY))),
      shareReplay({ refCount: true, bufferSize: 1 })
    );
  }

  public getInfo(uri: string = ''): Observable<ISystemInfo> {
    if (environment.production) {
      return this.httpClient.get(`${uri}/api/system/info`) as Observable<ISystemInfo>;
    } else {
      return of(
        {
          power: 42.670,
          voltage: 12208.75,
          current: 2237.5,
          temp: 60,
          vrTemp: 55,
          maxPower: 55,
          nominalVoltage: 12,
          hashRate: 2475,
          bestDiff: 666000000000,
          bestSessionDiff: "33M",
          stratumDiff: 8192,
          freeHeap: 200504,
          freeHeapInternal: 150000,
          freeHeapSpiram: 50504,
          isPSRAMAvailable: 1,
          coreVoltage: 1200,
          coreVoltageActual: 1200,
          hostname: "BitForge",
          macAddr: "2C:54:91:88:C9:E3",
          ssid: "default",
          wifiPass: "password",
          wifiStatus: "Connected!",
          apEnabled: 0,
          sharesAccepted: 1,
          sharesRejected: 0,
          sharesRejectedReasons: [],
          poolMode: 0,
          poolBalance: 50,
          pools: [
            { connected: true, validNotify: true, difficulty: 8192, accepted: 1, rejected: 0 },
            { connected: false, validNotify: false, difficulty: 8192, accepted: 0, rejected: 0 }
          ],
          coinbase: {
            blockHeight: 912345,
            scriptsig: "/public-pool.io/",
            valueTotalSatoshis: 312500000,
            outputs: [
              { address: "bc1q99n3pu025yyu0jlywpmwzalyhm36tg5u37w20d", value: 312500000 },
              { address: "OP_RETURN: witness commitment", value: 0 }
            ]
          },
          uptimeSeconds: 38,
          asicCount: 1,
          smallCoreCount: 672,
          ASICModel: eASICModel.BM1370,
          stratumURL: "public-pool.io",
          stratumPort: 21496,
          fallbackStratumURL: "solo.atlaspool.io",
          fallbackStratumPort: 21497,
          stratumUser: "bc1q99n3pu025yyu0jlywpmwzalyhm36tg5u37w20d.bitforge-U1",
          fallbackStratumUser: "bc1q99n3pu025yyu0jlywpmwzalyhm36tg5u37w20d.bitforge-U1",
          isUsingFallbackStratum: true,
          frequency: 485,
          version: "2.0",
          idfVersion: "v6.0.2",
          boardVersion: "204",
          autofanspeed: 1,
          fanSpeed: 100,
          manualFanSpeed: 100,
          fanrpm: 3333,

          chiptemp1: 30,
          chiptemp2: 40,
          overheat_mode: 0,
          stratumTLS: 0,
          fallbackStratumTLS: 0,
          stratumCert: 'x',
          fallbackStratumCert: 'x'
        }
      ).pipe(delay(1000));
    }
  }

  public restart(uri: string = '') {
    return this.httpClient.post(`${uri}/api/system/restart`, {}, {responseType: 'text'});
  }

  public updateSystem(uri: string = '', update: any) {
    return this.httpClient.patch(`${uri}/api/system`, update);
  }


  private otaUpdate(file: File | Blob, url: string) {
    return new Observable<HttpEvent<string>>((subscriber) => {
      const reader = new FileReader();

      reader.onload = (event: any) => {
        const fileContent = event.target.result;

        return this.httpClient.post(url, fileContent, {
          reportProgress: true,
          observe: 'events',
          responseType: 'text', // Specify the response type
          headers: {
            'Content-Type': 'application/octet-stream', // Set the content type
          },
        }).subscribe({
          next: (event) => {
            subscriber.next(event);
          },
          error: (err) => {
            subscriber.error(err)
          },
          complete: () => {
            subscriber.complete();
          }
        });
      };
      reader.readAsArrayBuffer(file);
    });
  }

  public performOTAUpdate(file: File | Blob) {
    return this.otaUpdate(file, `/api/system/OTA`);
  }
  public performWWWOTAUpdate(file: File | Blob) {
    return this.otaUpdate(file, `/api/system/OTAWWW`);
  }


  public startGithubOTA(fwUrl: string, wwwUrl: string): Observable<any> {
    return this.httpClient.post('/api/system/OTA/github', { fw_url: fwUrl, www_url: wwwUrl });
  }

  public getGithubOTAStatus(): Observable<any> {
    return this.httpClient.get('/api/system/OTA/github');
  }

  public getSwarmInfo(uri: string = ''): Observable<{ ip: string }[]> {
    return this.httpClient.get(`${uri}/api/swarm/info`) as Observable<{ ip: string }[]>;
  }

  public updateSwarm(uri: string = '', swarmConfig: any) {
    return this.httpClient.patch(`${uri}/api/swarm`, swarmConfig);
  }
}
