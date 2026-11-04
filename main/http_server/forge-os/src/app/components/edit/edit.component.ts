import { HttpErrorResponse } from '@angular/common/http';
import { Component, Input, OnInit, OnDestroy } from '@angular/core';
import { FormBuilder, FormGroup, Validators } from '@angular/forms';
import { ToastrService } from 'ngx-toastr';
import { startWith, Subject, takeUntil } from 'rxjs';
import { LoadingService } from 'src/app/services/loading.service';
import { SystemService } from 'src/app/services/system.service';
import { eASICModel } from 'src/models/enum/eASICModel';
import { ActivatedRoute } from '@angular/router';

import { PERFORMANCE_MODES, type PerformanceMode, type PerformanceModeId } from 'src/app/utils/performance-modes';

@Component({
    selector: 'app-edit',
    templateUrl: './edit.component.html',
    styleUrls: ['./edit.component.scss'],
    standalone: false
})
export class EditComponent implements OnInit, OnDestroy {

  public form!: FormGroup;

  public settingsUnlocked: boolean = false;
  public ASICModel!: eASICModel;
  public restrictedModels: eASICModel[] = Object.values(eASICModel)
    .filter((v): v is eASICModel => typeof v === 'string');

  @Input() uri = '';

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
          manualFanSpeed: [info.manualFanSpeed, [Validators.required]],
          overheat_mode: [info.overheat_mode, [Validators.required]]
        });

        this.form.controls['autofanspeed'].valueChanges.pipe(
          startWith(this.form.controls['autofanspeed'].value),
          takeUntil(this.destroy$)
        ).subscribe(autofanspeed => {
          if (autofanspeed) {
            this.form.controls['manualFanSpeed'].disable();
          } else {
            this.form.controls['manualFanSpeed'].enable();
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

    this.systemService.updateSystem(this.uri, form)
      .pipe(this.loadingService.lockUIUntilComplete())
      .subscribe({
        next: () => {
          const successMessage = this.uri ? `Saved settings for ${this.uri}` : 'Saved settings';
          this.toastr.success(successMessage, 'Success!');
        },
        error: (err: HttpErrorResponse) => {
          const errorMessage = this.uri ? `Could not save settings for ${this.uri}. ${err.message}` : `Could not save settings. ${err.message}`;
          this.toastr.error(errorMessage, 'Error');
        }
      });
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

  public readonly performanceModes = PERFORMANCE_MODES;

  public isActiveMode(mode: PerformanceMode): boolean {
    return Number(this.form?.value.frequency) === mode.frequency && Number(this.form?.value.coreVoltage) === mode.coreVoltage;
  }

  setPerformanceMode(id: PerformanceModeId) {
    const { frequency, coreVoltage } = PERFORMANCE_MODES.find((m) => m.id === id)!;

    // Update form values
    this.form.patchValue({
      frequency: frequency,
      coreVoltage: coreVoltage
    });

    console.log(`Performance mode set to: ${id} (Frequency: ${frequency}, Voltage: ${coreVoltage})`);

    // Instantly apply the settings
    this.updateSystem();
  }

}
