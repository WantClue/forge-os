import { HttpErrorResponse } from '@angular/common/http';
import { Component, Input, OnInit } from '@angular/core';
import { AbstractControl, FormBuilder, FormGroup, ValidationErrors, ValidatorFn, Validators } from '@angular/forms';
import { ToastrService } from 'ngx-toastr';
import { LoadingService } from 'src/app/services/loading.service';
import { SystemService } from 'src/app/services/system.service';

const BASE58 = /^[1-9A-HJ-NP-Za-km-z]*$/;

// Dual-pool mode is Stratum V1 only, and an SV2 pool that requires
// authentication needs a key to authenticate with.
const sv2Validator: ValidatorFn = (group: AbstractControl): ValidationErrors | null => {
  const value = group.value;
  const errors: ValidationErrors = {};

  if (value.poolMode === 1 && (value.stratumProtocol === 1 || value.fallbackStratumProtocol === 1)) {
    errors['sv2DualPool'] = true;
  }
  if (value.stratumProtocol === 1 && value.stratumV2RequireAuth && !value.stratumV2AuthorityPubkey) {
    errors['sv2AuthKeyMissing'] = true;
  }
  if (value.fallbackStratumProtocol === 1 && value.fallbackStratumV2RequireAuth && !value.fallbackStratumV2AuthorityPubkey) {
    errors['fallbackSv2AuthKeyMissing'] = true;
  }

  return Object.keys(errors).length ? errors : null;
};

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
    { label: 'Dual', value: 1 }
  ];

  sv2ChannelTypes = [
    { label: 'Extended (recommended)', value: 2 },
    { label: 'Standard', value: 1 }
  ];

  protocolOptions = this.buildProtocolOptions(false);

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
          fallbackStratumCert: [info.fallbackStratumCert ?? 'x'],
          stratumProtocol: [info.stratumProtocol ?? 0],
          stratumV2ChannelType: [info.stratumV2ChannelType ?? 2],
          stratumV2AuthorityPubkey: [info.stratumV2AuthorityPubkey ?? '', [Validators.pattern(BASE58)]],
          stratumV2RequireAuth: [info.stratumV2RequireAuth ?? false],
          fallbackStratumProtocol: [info.fallbackStratumProtocol ?? 0],
          fallbackStratumV2ChannelType: [info.fallbackStratumV2ChannelType ?? 2],
          fallbackStratumV2AuthorityPubkey: [info.fallbackStratumV2AuthorityPubkey ?? '', [Validators.pattern(BASE58)]],
          fallbackStratumV2RequireAuth: [info.fallbackStratumV2RequireAuth ?? false]
        }, { validators: sv2Validator });

        const poolMode = this.form.get('poolMode')!;
        this.protocolOptions = this.buildProtocolOptions(poolMode.value === 1);
        poolMode.valueChanges.subscribe(mode => {
          this.protocolOptions = this.buildProtocolOptions(mode === 1);
        });
      });
  }

  private buildProtocolOptions(dualPool: boolean) {
    return [
      { label: 'Stratum V1', value: 0, disabled: false },
      { label: dualPool ? 'Stratum V2 (not available in dual mode)' : 'Stratum V2', value: 1, disabled: dualPool }
    ];
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
    const protocolControl = this.form.get(`${poolType}Protocol`);
    const authorityKeyControl = this.form.get(`${poolType}V2AuthorityPubkey`);
    if (!urlControl || !portControl || !tlsControl || !protocolControl || !authorityKeyControl) return;

    let urlValue = (urlControl.value || '').trim();
    if (!urlValue) return;

    // The scheme picks the protocol; the stored setting is what the miner uses.
    const prefixes = [
      { prefix: 'stratum2+tcp://', tlsMode: 0, protocol: 1 },
      { prefix: 'stratum+tcp://', tlsMode: 0, protocol: 0 },
      { prefix: 'stratum+tls://', tlsMode: 1, protocol: 0 },
      { prefix: 'stratum+ssl://', tlsMode: 1, protocol: 0 }
    ];

    const matched = prefixes.find(({ prefix }) => urlValue.toLowerCase().startsWith(prefix));
    if (matched) {
      urlValue = urlValue.slice(matched.prefix.length);
      tlsControl.setValue(matched.tlsMode);
      if (protocolControl.value !== matched.protocol) {
        protocolControl.setValue(matched.protocol);
        protocolControl.markAsDirty();
      }
    }

    // SV2 pool URLs carry the authority key as their path: host:port/<key>
    const slash = urlValue.indexOf('/');
    if (slash >= 0) {
      const path = urlValue.slice(slash + 1);
      urlValue = urlValue.slice(0, slash);
      if (protocolControl.value === 1 && path) {
        authorityKeyControl.setValue(path);
        authorityKeyControl.markAsDirty();
      }
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
