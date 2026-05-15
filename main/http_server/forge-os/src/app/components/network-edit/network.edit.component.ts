import { HttpClient, HttpErrorResponse } from '@angular/common/http';
import { Component, Input, OnInit, ViewChild } from '@angular/core';
import { FormBuilder, FormGroup, Validators } from '@angular/forms';
import { ToastrService } from 'ngx-toastr';
import { finalize } from 'rxjs/operators';
import { ModalComponent } from '../modal/modal.component';
import { LoadingService } from 'src/app/services/loading.service';
import { SystemService } from 'src/app/services/system.service';

interface WifiNetwork {
  ssid: string;
  rssi: number;
  authmode: number;
}

@Component({
  selector: 'app-network-edit',
  templateUrl: './network.edit.component.html',
  styleUrls: ['./network.edit.component.scss']
})
export class NetworkEditComponent implements OnInit {
  @ViewChild('wifiScanModal') wifiScanModal?: ModalComponent;

  public form!: FormGroup;
  public savedChanges: boolean = false;
  public scanning: boolean = false;
  public scannedNetworks: WifiNetwork[] = [];

  @Input() uri = '';

  constructor(
    private fb: FormBuilder,
    private systemService: SystemService,
    private toastr: ToastrService,
    private toastrService: ToastrService,
    private loadingService: LoadingService,
    private http: HttpClient
  ) {

  }
  ngOnInit(): void {
    this.systemService.getInfo(this.uri)
      .pipe(this.loadingService.lockUIUntilComplete())
      .subscribe(info => {
        this.form = this.fb.group({
          hostname: [info.hostname, [Validators.required]],
          ssid: [info.ssid, [Validators.required]],
          wifiPass: ['*****'],
        });

      });
  }


  public updateSystem() {

    const form = this.form.getRawValue();

    // Allow an empty wifi password
    form.wifiPass = form.wifiPass == null ? '' : form.wifiPass;

    if (form.wifiPass === '*****') {
      delete form.wifiPass;
    }

    // Trim SSID to remove any leading/trailing whitespace
    if (form.ssid) {
      form.ssid = form.ssid.trim();
    }

    this.systemService.updateSystem(this.uri, form)
      .pipe(this.loadingService.lockUIUntilComplete())
      .subscribe({
        next: () => {
          this.toastr.success('Success!', 'Saved.');
          this.savedChanges = true;
        },
        error: (err: HttpErrorResponse) => {
          this.toastr.error('Error.', `Could not save. ${err.message}`);
          this.savedChanges = false;
        }
      });
  }

  showWifiPassword: boolean = false;
  toggleWifiPasswordVisibility() {
    this.showWifiPassword = !this.showWifiPassword;
  }

  public selectWifiNetwork(selectedSsid: string) {
    this.form.patchValue({ ssid: selectedSsid });
    this.form.markAsDirty();

    if (this.wifiScanModal) {
      this.wifiScanModal.isVisible = false;
    }
  }

  public scanWifi() {
    this.scanning = true;
    this.http.get<{networks: WifiNetwork[]}>('/api/system/wifi/scan')
      .pipe(
        finalize(() => this.scanning = false)
      )
      .subscribe({
        next: (response) => {
          const networks = response.networks
            .filter(network => network.ssid.trim().length > 0)
            .sort((a, b) => b.rssi - a.rssi);

          // Keep one row per SSID and preserve the strongest reading for that SSID.
          const uniqueNetworks = networks.reduce((acc, network) => {
            if (!acc[network.ssid] || acc[network.ssid].rssi < network.rssi) {
              acc[network.ssid] = network;
            }
            return acc;
          }, {} as { [key: string]: WifiNetwork });

          this.scannedNetworks = Object.values(uniqueNetworks);

          if (this.wifiScanModal) {
            this.wifiScanModal.isVisible = true;
          }
        },
        error: (err) => {
          this.toastr.error('Failed to scan WiFi networks', 'Error');
        }
      });
  }

  public restart() {
    this.systemService.restart()
      .pipe(this.loadingService.lockUIUntilComplete())
      .subscribe({
        next: () => {
          this.toastr.success('Success!', 'BitForge restarted');
        },
        error: (err: HttpErrorResponse) => {
          this.toastr.error('Error', `Could not restart. ${err.message}`);
        }
      });
  }
}
