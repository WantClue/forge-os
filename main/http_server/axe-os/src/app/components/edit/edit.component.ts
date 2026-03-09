import { HttpErrorResponse } from '@angular/common/http';
import { Component, Input, OnInit, OnDestroy } from '@angular/core';
import { FormBuilder, FormGroup, Validators } from '@angular/forms';
import { ToastrService } from 'ngx-toastr';
import { startWith, Subject, takeUntil } from 'rxjs';
import { LoadingService } from 'src/app/services/loading.service';
import { SystemService } from 'src/app/services/system.service';
import { eASICModel } from 'src/models/enum/eASICModel';
import { ActivatedRoute } from '@angular/router';

@Component({
  selector: 'app-edit',
  templateUrl: './edit.component.html',
  styleUrls: ['./edit.component.scss']
})
export class EditComponent implements OnInit, OnDestroy {

  public form!: FormGroup;

  public firmwareUpdateProgress: number | null = null;
  public websiteUpdateProgress: number | null = null;

  public savedChanges: boolean = false;
  public settingsUnlocked: boolean = false;
  public eASICModel = eASICModel;
  public ASICModel!: eASICModel;
  public restrictedModels: eASICModel[] = Object.values(eASICModel)
    .filter((v): v is eASICModel => typeof v === 'string');

  @Input() uri = '';

  public BM1370DropdownFrequency = [
    { name: '400', value: 400 },
    { name: '490', value: 490 },
    { name: '525 (default)', value: 525 },
    { name: '550', value: 550 },
    { name: '575', value: 575 },
    //{ name: '596', value: 596 },
    { name: '600', value: 600 },
    { name: '625', value: 625 },
  ];

  public BM1370CoreVoltage = [
    { name: '1000', value: 1000 },
    { name: '1060', value: 1060 },
    { name: '1100', value: 1100 },
    { name: '1150 (default)', value: 1150 },
    { name: '1200', value: 1200 },
    { name: '1250', value: 1250 },
  ];

  private destroy$ = new Subject<void>();

  constructor(
    private fb: FormBuilder,
    private systemService: SystemService,
    private toastr: ToastrService,
    private loadingService: LoadingService,
    private route: ActivatedRoute,
  ) {
    // Check URL parameter for settings unlock
    this.route.queryParams.subscribe(params => {
      const urlOcParam = params['oc'] !== undefined;
      if (urlOcParam) {
        // If ?oc is in URL, enable overclock and save to NVS
        this.settingsUnlocked = true;
        this.saveOverclockSetting(1);
        console.log(
          '🎉 The ancient seals have been broken!\n' +
          '⚡ Unlimited power flows through your miner...\n' +
          '🔧 You can now set custom frequency and voltage values.\n' +
          '⚠️ Remember: with great power comes great responsibility!'
        );
      } else {
        // If ?oc is not in URL, check NVS setting (will be loaded in ngOnInit)
        console.log('🔒 Here be dragons! Advanced settings are locked for your protection. \n' +
          'Only the bravest miners dare to venture forth... \n' +
          'If you wish to unlock dangerous overclocking powers, add: %c?oc',
          'color: #ff4400; text-decoration: underline; cursor: pointer; font-weight: bold;',
          'to the current URL'
        );
      }
    });
  }

  private saveOverclockSetting(enabled: number) {
    this.systemService.updateSystem(this.uri, { overclockEnabled: enabled })
      .subscribe({
        next: () => {
          console.log(`Overclock setting saved: ${enabled === 1 ? 'enabled' : 'disabled'}`);
        },
        error: (err) => {
          console.error(`Failed to save overclock setting: ${err.message}`);
        }
      });
  }

  ngOnInit(): void {
    this.systemService.getInfo(this.uri)
      .pipe(
        this.loadingService.lockUIUntilComplete(),
        takeUntil(this.destroy$)
      )
      .subscribe(info => {
        this.ASICModel = info.ASICModel;

        // Check if overclock is enabled in NVS
        if (info.overclockEnabled === 1) {
          this.settingsUnlocked = true;
          console.log(
            '🎉 Overclock mode is enabled from NVS settings!\n' +
            '⚡ Custom frequency and voltage values are available.'
          );
        }

        this.form = this.fb.group({
          coreVoltage: [info.coreVoltage, [Validators.required]],
          frequency: [info.frequency, [Validators.required]],
          autofanspeed: [info.autofanspeed == 1, [Validators.required]],
          fanspeed: [info.fanspeed, [Validators.required]],
          fanTargetTemp: [info.fanTargetTemp ?? 45, [Validators.required, Validators.min(35), Validators.max(75)]],
          fanMinSpeed: [info.fanMinSpeed ?? 35, [Validators.required, Validators.min(0), Validators.max(99)]],
          overheat_mode: [info.overheat_mode, [Validators.required]]
        });

        this.form.controls['autofanspeed'].valueChanges.pipe(
          startWith(this.form.controls['autofanspeed'].value),
          takeUntil(this.destroy$)
        ).subscribe(autofanspeed => {
          if (autofanspeed) {
            this.form.controls['fanspeed'].disable();
            this.form.controls['fanTargetTemp'].enable();
            this.form.controls['fanMinSpeed'].enable();
          } else {
            this.form.controls['fanspeed'].enable();
            this.form.controls['fanTargetTemp'].disable();
            this.form.controls['fanMinSpeed'].disable();
          }
        });
      });
  }

  ngOnDestroy(): void {
    this.destroy$.next();
    this.destroy$.complete();
  }

  public updateSystem() {

    const form = this.form.getRawValue();

    if (form.stratumPassword === '*****') {
      delete form.stratumPassword;
    }

    this.systemService.updateSystem(this.uri, form)
      .pipe(this.loadingService.lockUIUntilComplete())
      .subscribe({
        next: () => {
          const successMessage = this.uri ? `Saved settings for ${this.uri}` : 'Saved settings';
          this.toastr.success(successMessage, 'Success!');
          this.savedChanges = true;
        },
        error: (err: HttpErrorResponse) => {
          const errorMessage = this.uri ? `Could not save settings for ${this.uri}. ${err.message}` : `Could not save settings. ${err.message}`;
          this.toastr.error(errorMessage, 'Error');
          this.savedChanges = false;
        }
      });
  }

  showWifiPassword: boolean = false;
  toggleWifiPasswordVisibility() {
    this.showWifiPassword = !this.showWifiPassword;
  }

  disableOverheatMode() {
    this.form.patchValue({ overheat_mode: 0 });
    this.updateSystem();
  }

  toggleOverclockMode(enable: boolean) {
    this.settingsUnlocked = enable;
    this.saveOverclockSetting(enable ? 1 : 0);

    if (enable) {
      console.log(
        '🎉 Overclock mode enabled!\n' +
        '⚡ Custom frequency and voltage values are now available.'
      );
    } else {
      console.log('🔒 Overclock mode disabled. Using safe preset values only.');
    }
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

  getDropdownFrequency() {
    // Get base frequency options based on ASIC model
    let options = [];
    switch(this.ASICModel) {
        case this.eASICModel.BM1370: options = [...this.BM1370DropdownFrequency]; break;
        default: return [];
    }

    // Get current frequency value from form
    const currentFreq = this.form?.get('frequency')?.value;

    // If current frequency exists and isn't in the options
    if (currentFreq && !options.some(opt => opt.value === currentFreq)) {
        options.push({
            name: `${currentFreq} (Custom)`,
            value: currentFreq
        });
        // Sort options by frequency value
        options.sort((a, b) => a.value - b.value);
    }

    return options;
  }

  getCoreVoltage() {
    // Get base voltage options based on ASIC model
    let options = [];
    switch(this.ASICModel) {
        case this.eASICModel.BM1370: options = [...this.BM1370CoreVoltage]; break;
        default: return [];
    }

    // Get current voltage value from form
    const currentVoltage = this.form?.get('coreVoltage')?.value;

    // If current voltage exists and isn't in the options
    if (currentVoltage && !options.some(opt => opt.value === currentVoltage)) {
        options.push({
            name: `${currentVoltage} (Custom)`,
            value: currentVoltage
        });
        // Sort options by voltage value
        options.sort((a, b) => a.value - b.value);
    }

    return options;
  }

  setPerformanceMode(mode: 'eco' | 'medium' | 'power') {
    let frequency: number;
    let coreVoltage: number;

    // Set predefined values based on performance mode
    switch (mode) {
      case 'eco':
        frequency = 430;
        coreVoltage = 1000;
        break;
      case 'medium':
        frequency = 525; // default
        coreVoltage = 1150; // default
        break;
      case 'power':
        frequency = 636;
        coreVoltage = 1160;
        break;
    }

    // Update form values
    this.form.patchValue({
      frequency: frequency,
      coreVoltage: coreVoltage
    });

    console.log(`Performance mode set to: ${mode} (Frequency: ${frequency}, Voltage: ${coreVoltage})`);

    // Instantly apply the settings
    this.updateSystem();
  }

}
