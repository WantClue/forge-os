import { HttpErrorResponse } from '@angular/common/http';
import { Component, Input, OnInit } from '@angular/core';
import { FormBuilder, FormGroup, Validators } from '@angular/forms';
import { ToastrService } from 'ngx-toastr';
import { LoadingService } from 'src/app/services/loading.service';
import { SystemService } from 'src/app/services/system.service';

@Component({
    selector: 'app-pool',
    templateUrl: './pool.component.html',
    styleUrls: ['./pool.component.scss'],
    standalone: false
})
export class PoolComponent implements OnInit {
  public form!: FormGroup;
  public savedChanges: boolean = false;

  @Input() uri = '';

  tlsModes = [
    { label: 'Disabled', value: 0 },
    { label: 'Enabled (Bundled CA)', value: 1 }
    //{ label: 'Enabled (Custom CA)', value: 2 }
  ];

  poolModes = [
    { label: 'Fallback', value: 0 },
    { label: 'Dual Pool', value: 1 }
  ];

  constructor(
    private fb: FormBuilder,
    private systemService: SystemService,
    private toastr: ToastrService,
    private loadingService: LoadingService
  ) {}

  ngOnInit(): void {
    this.systemService.getInfo(this.uri)
      .pipe(
        this.loadingService.lockUIUntilComplete()
      )
      .subscribe(info => {
        this.form = this.fb.group({
          poolMode: [info.poolMode ?? 0],
          poolBalance: [info.poolBalance ?? 50, [
            Validators.required,
            Validators.min(0),
            Validators.max(100)
          ]],
          stratumURL: [info.stratumURL, [
            Validators.required,
            Validators.pattern(/^(?!.*stratum\+tcp:\/\/).*$/),
            Validators.pattern(/^[^:]*$/),
          ]],
          stratumPort: [info.stratumPort, [
            Validators.required,
            Validators.pattern(/^[^:]*$/),
            Validators.min(0),
            Validators.max(65535)
          ]],
          fallbackStratumURL: [info.fallbackStratumURL, [
            Validators.pattern(/^(?!.*stratum\+tcp:\/\/).*$/),
          ]],
          fallbackStratumPort: [info.fallbackStratumPort, [
            Validators.required,
            Validators.pattern(/^[^:]*$/),
            Validators.min(0),
            Validators.max(65535)
          ]],
          stratumUser: [info.stratumUser, [Validators.required]],
          stratumPassword: ['*****', [Validators.required]],
          fallbackStratumUser: [info.fallbackStratumUser, [Validators.required]],
          fallbackStratumPassword: ['password', [Validators.required]],
          stratumTLS: [info.stratumTLS ?? 0],
          stratumCert: [info.stratumCert ?? 'x'],
          fallbackStratumTLS: [info.fallbackStratumTLS ?? 0],
          fallbackStratumCert: [info.fallbackStratumCert ?? 'x']
        });
      });
  }

  public updateSystem() {
    this.onUrlChange('stratum');
    this.onUrlChange('fallbackStratum');

    const form = this.form.getRawValue();

    if (form.stratumPassword === '*****') {
      delete form.stratumPassword;
    }

    this.systemService.updateSystem(this.uri, form)
      .pipe(this.loadingService.lockUIUntilComplete())
      .subscribe({
        next: () => {
          const successMessage = this.uri ? `Saved pool settings for ${this.uri}` : 'Saved pool settings';
          this.toastr.success(successMessage, 'Success!');
          this.savedChanges = true;
        },
        error: (err: HttpErrorResponse) => {
          const errorMessage = this.uri ? `Could not save pool settings for ${this.uri}. ${err.message}` : `Could not save pool settings. ${err.message}`;
          this.toastr.error(errorMessage, 'Error');
          this.savedChanges = false;
        }
      });
  }

  private extractPort(url: string): { cleanUrl: string, port?: number } {
    const match = url.match(/:(\d{1,5})$/);
    if (!match) {
      return { cleanUrl: url };
    }

    return {
      cleanUrl: url.slice(0, match.index),
      port: parseInt(match[1], 10)
    };
  }

  public onUrlChange(poolType: 'stratum' | 'fallbackStratum') {
    const urlControl = this.form.get(`${poolType}URL`);
    const portControl = this.form.get(`${poolType}Port`);
    const tlsControl = this.form.get(`${poolType}TLS`);
    if (!urlControl || !portControl || !tlsControl) return;

    let urlValue = (urlControl.value || '').trim();
    if (!urlValue) return;

    const prefixes = [
      { prefix: 'stratum+tcp://', tlsMode: 0 },
      { prefix: 'stratum+tls://', tlsMode: 1 },
      { prefix: 'stratum+ssl://', tlsMode: 1 }
    ];

    const matched = prefixes.find(({ prefix }) => urlValue.toLowerCase().startsWith(prefix));
    if (matched) {
      urlValue = urlValue.slice(matched.prefix.length);
      tlsControl.setValue(matched.tlsMode);
    }

    const { cleanUrl, port } = this.extractPort(urlValue);
    if (port !== undefined && port >= 0 && port <= 65535) {
      portControl.setValue(port);
    }
    urlControl.setValue(cleanUrl);
  }

  showStratumPassword: boolean = false;
  toggleStratumPasswordVisibility() {
    this.showStratumPassword = !this.showStratumPassword;
  }

  showFallbackStratumPassword: boolean = false;
  toggleFallbackStratumPasswordVisibility() {
    this.showFallbackStratumPassword = !this.showFallbackStratumPassword;
  }

  public restart() {
    this.systemService.restart(this.uri)
      .pipe(this.loadingService.lockUIUntilComplete())
      .subscribe({
        next: () => {
          const successMessage = this.uri ? `Miner at ${this.uri} restarted` : 'Miner restarted';
          this.toastr.success(successMessage, 'Success');
        },
        error: (err: HttpErrorResponse) => {
          const errorMessage = this.uri ? `Failed to restart device at ${this.uri}. ${err.message}` : `Failed to restart device. ${err.message}`;
          this.toastr.error(errorMessage, 'Error');
        }
      });
  }
}
