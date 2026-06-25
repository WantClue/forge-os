import { HttpErrorResponse, HttpEventType } from '@angular/common/http';
import { Component, OnDestroy, ViewChild } from '@angular/core';
import { FormBuilder, FormGroup, Validators } from '@angular/forms';
import { ToastrService } from 'ngx-toastr';
import { FileUploadHandlerEvent, FileUpload } from 'primeng/fileupload';
import { map, Observable, shareReplay, startWith, Subscription } from 'rxjs';
import { GithubRelease, GithubUpdateService } from 'src/app/services/github-update.service';
import { LoadingService } from 'src/app/services/loading.service';
import { SystemService } from 'src/app/services/system.service';
import { eASICModel } from 'src/models/enum/eASICModel';
import { ModalComponent } from '../modal/modal.component';

@Component({
    selector: 'app-settings',
    templateUrl: './settings.component.html',
    styleUrls: ['./settings.component.scss'],
    standalone: false
})
export class SettingsComponent implements OnDestroy {

  public form!: FormGroup;

  public firmwareUpdateProgress: number = 0;
  public websiteUpdateProgress: number = 0;
  public isUpdating: boolean = false;

  public updateTarget: string = '';
  public updateStatus: 'progress' | 'success' | 'error' = 'progress';
  public updateMessage: string = '';

  @ViewChild('progressModal') progressModal?: ModalComponent;

  public eASICModel = eASICModel;
  public ASICModel!: eASICModel;

  public checkLatestRelease: boolean = false;
  public latestRelease$: Observable<any>;
  public releases$!: Observable<GithubRelease[]>;
  public selectedRelease: GithubRelease | null = null;

  // GitHub OTA state
  public isGithubOTA: boolean = false;
  public githubOTAStep: string = 'idle';
  public githubOTAProgress: number = 0;
  private otaPollSub: Subscription | null = null;
  private rebootCheckSub: Subscription | null = null;

  public info$: Observable<any>;

  @ViewChild('firmwareUpload') firmwareUpload!: FileUpload;
  @ViewChild('websiteUpload') websiteUpload!: FileUpload;

  constructor(
    private fb: FormBuilder,
    private systemService: SystemService,
    private toastr: ToastrService,
    private toastrService: ToastrService,
    private loadingService: LoadingService,
    private githubUpdateService: GithubUpdateService
  ) {



    this.latestRelease$ = this.githubUpdateService.getReleases().pipe(map(releases => {
      return releases[0];
    }));

    this.info$ = this.systemService.getInfo().pipe(shareReplay({refCount: true, bufferSize: 1}))


      this.info$.pipe(this.loadingService.lockUIUntilComplete())
      .subscribe(info => {
        this.ASICModel = info.ASICModel;
        this.form = this.fb.group({
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
          stratumUser: [info.stratumUser, [Validators.required]],
          stratumPassword: ['*****', [Validators.required]],
          coreVoltage: [info.coreVoltage, [Validators.required]],
          frequency: [info.frequency, [Validators.required]],
          autofanspeed: [info.autofanspeed == 1, [Validators.required]],
          manualFanSpeed: [info.manualFanSpeed, [Validators.required]],
        });

        this.form.controls['autofanspeed'].valueChanges.pipe(
          startWith(this.form.controls['autofanspeed'].value)
        ).subscribe(autofanspeed => {
          if (autofanspeed) {
            this.form.controls['manualFanSpeed'].disable();
          } else {
            this.form.controls['manualFanSpeed'].enable();
          }
        });
      });

  }

  ngOnDestroy() {
    this.stopOTAPolling();
    if (this.rebootCheckSub) {
      this.rebootCheckSub.unsubscribe();
    }
  }

  public updateSystem() {

    const form = this.form.getRawValue();

    form.frequency = parseInt(form.frequency);
    form.coreVoltage = parseInt(form.coreVoltage);

    // bools to ints
    form.autofanspeed = form.autofanspeed == true ? 1 : 0;

    if (form.stratumPassword === '*****') {
      delete form.stratumPassword;
    }

    this.systemService.updateSystem(undefined, form)
      .pipe(this.loadingService.lockUIUntilComplete())
      .subscribe({
        next: () => {
          this.toastr.success('Success!', 'Saved.');
        },
        error: (err: HttpErrorResponse) => {
          this.toastr.error('Error.', `Could not save. ${err.message}`);
        }
      });
  }

  otaUpdate(event: FileUploadHandlerEvent) {
    if (this.isUpdating) return;

    const file = event.files[0];
    this.firmwareUpload.clear();

    if (file.name != 'bitforgeos.bin') {
      this.toastrService.error('Incorrect file, looking for bitforgeos.bin.', 'Error');
      return;
    }

    this.isUpdating = true;
    this.updateTarget = 'Firmware';
    this.updateStatus = 'progress';
    this.updateMessage = '';
    if (this.progressModal) {
      this.progressModal.isVisible = true;
    }

    this.systemService.performOTAUpdate(file)
      .subscribe({
        next: (event) => {
          if (event.type === HttpEventType.UploadProgress) {
            this.firmwareUpdateProgress = Math.round((event.loaded / (event.total as number)) * 100);
          } else if (event.type === HttpEventType.Response) {
            if (event.ok) {
              this.updateStatus = 'success';
              this.updateMessage = 'Firmware updated. Device has been successfully restarted.';
            } else {
              this.updateStatus = 'error';
              this.updateMessage = event.statusText || 'An unknown error occurred.';
            }
          }
        },
        error: (err) => {
          this.firmwareUpdateProgress = 0;
          this.isUpdating = false;
          this.updateStatus = 'error';
          this.updateMessage = err.error?.message || err.error || err.message || 'Unknown error occurred';
        },
        complete: () => {
          this.firmwareUpdateProgress = 0;
          this.isUpdating = false;
        }
      });
  }

  otaWWWUpdate(event: FileUploadHandlerEvent) {
    if (this.isUpdating) return;

    const file = event.files[0];
    this.websiteUpload.clear();

    if (file.name != 'www.bin') {
      this.toastrService.error('Incorrect file, looking for www.bin.', 'Error');
      return;
    }

    this.isUpdating = true;
    this.updateTarget = 'Website';
    this.updateStatus = 'progress';
    this.updateMessage = '';
    if (this.progressModal) {
      this.progressModal.isVisible = true;
    }

    this.systemService.performWWWOTAUpdate(file)
      .subscribe({
        next: (event) => {
          if (event.type === HttpEventType.UploadProgress) {
            this.websiteUpdateProgress = Math.round((event.loaded / (event.total as number)) * 100);
          } else if (event.type === HttpEventType.Response) {
            if (event.ok) {
              this.updateStatus = 'success';
              this.updateMessage = 'Website updated. The page will reload in a few seconds.';
              setTimeout(() => {
                window.location.reload();
              }, 2000);
            } else {
              this.updateStatus = 'error';
              this.updateMessage = event.statusText || 'An unknown error occurred.';
            }
          }
        },
        error: (err) => {
          this.websiteUpdateProgress = 0;
          this.isUpdating = false;
          this.updateStatus = 'error';
          this.updateMessage = err.error?.message || err.error || err.message || 'Unknown error occurred';
        },
        complete: () => {
          this.websiteUpdateProgress = 0;
          this.isUpdating = false;
        }
      });
  }

  public loadReleases() {
    this.checkLatestRelease = true;
    this.releases$ = this.githubUpdateService.getReleases();
  }

  public onReleaseSelected(release: GithubRelease) {
    this.selectedRelease = release;
  }

  public getStepLabel(step: string): string {
    switch (step) {
      case 'downloading_fw': return 'Downloading firmware...';
      case 'flashing_fw': return 'Downloading & flashing firmware...';
      case 'downloading_www': return 'Downloading website...';
      case 'flashing_www': return 'Flashing website...';
      case 'rebooting': return 'Rebooting...';
      case 'error': return 'Error';
      default: return 'Starting...';
    }
  }

  public installFromGithub() {
    if (!this.selectedRelease) return;

    const fwUrl = this.githubUpdateService.findAssetUrl(this.selectedRelease, 'bitforgeos.bin');
    const wwwUrl = this.githubUpdateService.findAssetUrl(this.selectedRelease, 'www.bin');

    if (!fwUrl || !wwwUrl) {
      this.toastrService.error('Release is missing bitforgeos.bin or www.bin assets', 'Error');
      return;
    }

    this.isGithubOTA = true;
    this.githubOTAStep = 'idle';
    this.githubOTAProgress = 0;
    this.firmwareUpdateProgress = 0;
    this.updateTarget = 'GitHub Firmware';
    this.updateStatus = 'progress';
    this.updateMessage = '';
    if (this.progressModal) {
      this.progressModal.isVisible = true;
    }

    this.systemService.startGithubOTA(fwUrl, wwwUrl).subscribe({
      next: () => {
        this.startOTAPolling();
      },
      error: (err) => {
        this.isGithubOTA = false;
        this.updateStatus = 'error';
        this.updateMessage = err.error?.message || err.message || 'Failed to start update';
      }
    });
  }

  private startOTAPolling() {
    this.stopOTAPolling();

    const poll = () => {
      this.otaPollSub = this.systemService.getGithubOTAStatus().subscribe({
        next: (status) => {
          this.githubOTAStep = status.step;
          this.githubOTAProgress = status.progress;
          this.firmwareUpdateProgress = status.progress;

          if (status.step === 'rebooting') {
            this.stopOTAPolling();
            this.updateStatus = 'success';
            this.updateMessage = 'Update complete! Device is rebooting...';
            this.startRebootCheck();
            return;
          }

          if (status.step === 'error') {
            this.stopOTAPolling();
            this.isGithubOTA = false;
            this.updateStatus = 'error';
            this.updateMessage = status.error || 'Update failed';
            return;
          }

          if (status.running) {
            setTimeout(() => poll(), 1000);
          } else {
            this.isGithubOTA = false;
          }
        },
        error: () => {
          // Device may have rebooted, start checking
          this.stopOTAPolling();
          this.startRebootCheck();
        }
      });
    };

    poll();
  }

  private stopOTAPolling() {
    if (this.otaPollSub) {
      this.otaPollSub.unsubscribe();
      this.otaPollSub = null;
    }
  }

  private startRebootCheck() {
    let attempts = 0;
    const maxAttempts = 60;

    const check = () => {
      attempts++;
      if (attempts > maxAttempts) {
        this.isGithubOTA = false;
        this.toastrService.warning('Device did not come back online within 60 seconds', 'Warning');
        return;
      }

      this.rebootCheckSub = this.systemService.getInfo().subscribe({
        next: () => {
          // Device is back!
          this.isGithubOTA = false;
          this.toastrService.success('Device is back online! Reloading...', 'Success');
          setTimeout(() => window.location.reload(), 1500);
        },
        error: () => {
          // Not back yet, retry
          setTimeout(() => check(), 1000);
        }
      });
    };

    // Wait 5 seconds before first check
    setTimeout(() => check(), 5000);
  }

  public restart() {
    this.systemService.restart().subscribe(res => {

    });
    this.toastr.success('Success!', 'BitForge restarted');
  }
}
